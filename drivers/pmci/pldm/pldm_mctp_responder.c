/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT tenstorrent_pldm_mctp_responder

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pmci/mctp/mctp_i3c_target.h>
#include <libmctp.h>
#include <libpldm/base.h>

#include <errno.h>
#include <string.h>

#include "pldm_base.h"
#include "pldm_mctp_responder.h"
#include "pldm_pdr.h"
#include "pldm_platform.h"
#include <zephyr/drivers/pmci/pldm/pldm_oem_handler.h>

LOG_MODULE_REGISTER(tt_pldm_mctp_rsp, CONFIG_TT_APP_LOG_LEVEL);

#define TT_PLDM_MSG_TYPE_PLDM       0x01U
#define TT_PLDM_MAX_MESSAGE_LEN     64U
#define TT_PLDM_RX_QUEUE_DEPTH      8U
#define TT_PLDM_RX_WORKQ_STACK_SIZE 1024U

struct tt_pldm_rx_work_item {
	uint8_t eid;
	uint8_t msg_tag;
	size_t len;
	uint8_t buf[1U + TT_PLDM_MAX_MESSAGE_LEN];
};

struct tt_pldm_mctp_responder_config {
	uint8_t endpoint_id;
	uint8_t tid;
	struct mctp_binding_i3c_target *binding;
	const struct device *pdr_dev;
	const struct device *oem_handler_dev;
};

struct tt_pldm_mctp_responder_data {
	struct mctp *mctp_ctx;
	bool ready;
	uint8_t tid;
	const struct device *pdr_dev;
	const struct device *dev;
	struct k_work_q rx_work_q;
	struct k_work rx_work;
	struct k_msgq rx_msgq;

	K_KERNEL_STACK_MEMBER(rx_work_q_stack, TT_PLDM_RX_WORKQ_STACK_SIZE);
	uint8_t rx_msgq_buffer[TT_PLDM_RX_QUEUE_DEPTH * sizeof(struct tt_pldm_rx_work_item)];
};

static bool g_dmc_mctp_transport_ready;

static int tt_pldm_build_response(const struct device *dev, const struct pldm_msg *req_msg,
				  size_t req_pldm_len, struct pldm_msg *resp_msg,
				  size_t *resp_pldm_len);

static void tt_pldm_mctp_process_rx_message(const struct device *dev, uint8_t eid, uint8_t msg_tag,
					    const uint8_t *rx_buf, size_t len)
{
	struct tt_pldm_mctp_responder_data *data = dev->data;
	uint8_t tx_buf[1U + TT_PLDM_MAX_MESSAGE_LEN];
	struct pldm_msg *resp_msg = (struct pldm_msg *)&tx_buf[1];
	size_t req_pldm_len;
	size_t resp_pldm_len = 0U;
	int rc;

	if (len < 1U || rx_buf[0] != TT_PLDM_MSG_TYPE_PLDM) {
		return;
	}

	req_pldm_len = len - 1U;
	rc = tt_pldm_build_response(dev, (const struct pldm_msg *)&rx_buf[1], req_pldm_len,
				    resp_msg, &resp_pldm_len);
	if (rc != 0 || resp_pldm_len == 0U) {
		LOG_WRN("Failed to build PLDM response rc=%d", rc);
		return;
	}

	if (resp_pldm_len > TT_PLDM_MAX_MESSAGE_LEN) {
		LOG_WRN("PLDM response too large (%u bytes)", (unsigned int)resp_pldm_len);
		return;
	}

	tx_buf[0] = TT_PLDM_MSG_TYPE_PLDM;
	rc = mctp_message_tx(data->mctp_ctx, eid, false, msg_tag, tx_buf, 1U + resp_pldm_len);
	if (rc < 0) {
		LOG_WRN("Failed to TX PLDM response rc=%d", rc);
	}
}

static void tt_pldm_mctp_rx_work_handler(struct k_work *work)
{
	struct tt_pldm_mctp_responder_data *data =
		CONTAINER_OF(work, struct tt_pldm_mctp_responder_data, rx_work);
	struct tt_pldm_rx_work_item item;

	while (k_msgq_get(&data->rx_msgq, &item, K_NO_WAIT) == 0) {
		if (data->dev != NULL) {
			tt_pldm_mctp_process_rx_message(data->dev, item.eid, item.msg_tag, item.buf,
							item.len);
		}
	}
}

bool dmc_mctp_transport_ready(void)
{
	return g_dmc_mctp_transport_ready;
}

const struct device *tt_pldm_mctp_responder_oem_handler_get(const struct device *dev)
{
	if (dev == NULL) {
		return NULL;
	}

	const struct tt_pldm_mctp_responder_config *cfg = dev->config;

	return cfg->oem_handler_dev;
}

static int tt_pldm_build_response(const struct device *dev, const struct pldm_msg *req_msg,
				  size_t req_pldm_len, struct pldm_msg *resp_msg,
				  size_t *resp_pldm_len)
{
	const struct tt_pldm_mctp_responder_config *cfg = dev->config;
	struct tt_pldm_mctp_responder_data *data = dev->data;
	struct pldm_header_info req_hdr;
	size_t req_payload_len;
	uint8_t cc;

	if (req_pldm_len < sizeof(struct pldm_msg_hdr)) {
		return -EMSGSIZE;
	}

	req_payload_len = req_pldm_len - sizeof(struct pldm_msg_hdr);

	cc = unpack_pldm_header(&req_msg->hdr, &req_hdr);
	if (cc != PLDM_SUCCESS) {
		return -EBADMSG;
	}

	if (req_hdr.msg_type != PLDM_REQUEST) {
		return -ENOMSG;
	}

	if (req_hdr.pldm_type == PLDM_BASE) {
		return pldm_base_build_response(&data->tid, dev, &req_hdr, req_msg, req_payload_len,
						resp_msg, resp_pldm_len);
	}

	if (req_hdr.pldm_type == PLDM_PLATFORM) {
		return pldm_platform_build_response(data->pdr_dev, &req_hdr, req_msg,
						    req_payload_len, resp_msg, resp_pldm_len);
	}

	if (req_hdr.pldm_type == PLDM_OEM_TYPE && cfg->oem_handler_dev != NULL) {
		return pldm_oem_handler_build_response(cfg->oem_handler_dev, &req_hdr, req_msg,
						       req_payload_len, resp_msg, resp_pldm_len);
	}

	return pldm_cc_only_response(&req_hdr, PLDM_ERROR_INVALID_PLDM_TYPE, resp_msg,
				     resp_pldm_len);
}

static void tt_pldm_mctp_rx_message(uint8_t eid, bool tag_owner, uint8_t msg_tag, void *cb_data,
				    void *msg, size_t len)
{
	const struct device *dev = cb_data;
	struct tt_pldm_mctp_responder_data *data = dev->data;
	const uint8_t *rx_buf = msg;
	struct tt_pldm_rx_work_item item;

	ARG_UNUSED(tag_owner);

	if (len < 1U || len > sizeof(item.buf)) {
		return;
	}

	item.eid = eid;
	item.msg_tag = msg_tag;
	item.len = len;
	memcpy(item.buf, rx_buf, len);

	if (k_msgq_put(&data->rx_msgq, &item, K_NO_WAIT) != 0) {
		LOG_WRN("Dropping PLDM RX message: RX message queue full");
		return;
	}

	k_work_submit_to_queue(&data->rx_work_q, &data->rx_work);
}

static int tt_pldm_mctp_responder_init(const struct device *dev)
{
	const struct tt_pldm_mctp_responder_config *cfg = dev->config;
	struct tt_pldm_mctp_responder_data *data = dev->data;
	int rc;

	data->mctp_ctx = mctp_init();
	if (data->mctp_ctx == NULL) {
		LOG_ERR("mctp_init failed");
		return -ENOMEM;
	}
	data->dev = dev;

	k_msgq_init(&data->rx_msgq, data->rx_msgq_buffer, sizeof(struct tt_pldm_rx_work_item),
		    TT_PLDM_RX_QUEUE_DEPTH);
	k_work_init(&data->rx_work, tt_pldm_mctp_rx_work_handler);
	k_work_queue_start(&data->rx_work_q, data->rx_work_q_stack,
			   K_KERNEL_STACK_SIZEOF(data->rx_work_q_stack), K_PRIO_PREEMPT(0), NULL);

	rc = mctp_register_bus(data->mctp_ctx, &cfg->binding->binding, cfg->endpoint_id);
	if (rc != 0) {
		LOG_ERR("mctp_register_bus failed rc=%d", rc);
		return rc;
	}

	/* Advertise PLDM in MCTP Control Get Message Type Support. A bus owner (mctpd + pldmd)
	 * only treats endpoints that list type 0x01 as PLDM termini.
	 */
	rc = mctp_control_add_type(data->mctp_ctx, TT_PLDM_MSG_TYPE_PLDM);
	if (rc != 0) {
		LOG_ERR("mctp_control_add_type failed rc=%d", rc);
		return rc;
	}

	data->tid = cfg->tid;
	data->pdr_dev = cfg->pdr_dev;
	if (cfg->pdr_dev != NULL) {
		rc = pldm_pdr_update_tid_eid(cfg->pdr_dev, cfg->tid, cfg->endpoint_id);
		if (rc != 0) {
			LOG_ERR("PDR update_tid_eid failed rc=%d", rc);
			return rc;
		}
	}

	if (cfg->oem_handler_dev != NULL && !device_is_ready(cfg->oem_handler_dev)) {
		LOG_ERR("OEM handler device not ready");
		return -ENODEV;
	}

	mctp_set_rx_all(data->mctp_ctx, tt_pldm_mctp_rx_message, (void *)dev);

	data->ready = true;
	g_dmc_mctp_transport_ready = true;
	LOG_INF("PLDM MCTP responder ready, EID %u", cfg->endpoint_id);

	return 0;
}

/* clang-format off */
#define TT_PLDM_MCTP_BINDING(inst)                                                                 \
	MCTP_I3C_TARGET_DT_DEFINE(tt_pldm_mctp_i3c_##inst,                                         \
				  DT_PHANDLE(DT_DRV_INST(inst), mctp_target))

#define TT_PLDM_MCTP_RESPONDER_DEFINE(inst)                                                        \
	TT_PLDM_MCTP_BINDING(inst);                                                                \
	static const struct tt_pldm_mctp_responder_config tt_pldm_mctp_cfg_##inst = {              \
		.endpoint_id = DT_PROP(DT_PHANDLE(DT_DRV_INST(inst), mctp_target), endpoint_id),   \
		.tid = DT_PROP(DT_DRV_INST(inst), pldm_tid),                                       \
		.binding = &tt_pldm_mctp_i3c_##inst,                                               \
		.pdr_dev = COND_CODE_1(DT_NODE_HAS_PROP(DT_DRV_INST(inst), pdr),                   \
				      (DEVICE_DT_GET(DT_PHANDLE(DT_DRV_INST(inst), pdr))), (NULL)),\
			 .oem_handler_dev = COND_CODE_1(DT_NODE_HAS_PROP                           \
							      (DT_DRV_INST(inst), oem_handler),    \
					      (DEVICE_DT_GET(DT_PHANDLE(DT_DRV_INST(inst),         \
								     oem_handler))), (NULL)),      \
	};                                                                                         \
	static struct tt_pldm_mctp_responder_data tt_pldm_mctp_data_##inst;                        \
	DEVICE_DT_INST_DEFINE(inst, tt_pldm_mctp_responder_init, NULL, &tt_pldm_mctp_data_##inst,  \
			      &tt_pldm_mctp_cfg_##inst, POST_KERNEL,                               \
			      CONFIG_APPLICATION_INIT_PRIORITY, NULL)

/* clang-format on */
DT_INST_FOREACH_STATUS_OKAY(TT_PLDM_MCTP_RESPONDER_DEFINE)
