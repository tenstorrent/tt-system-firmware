/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/drivers/mbox.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>

#include "platform.h"

/**
 * @file
 * @brief Driver for Tenstorrent Grendel SMC Mailbox
 * @section mbox_ip_design MBOX IP Design Overview
 *
 * @dot
 * digraph mailbox_communication {
 *    rankdir=LR;
 *    node [shape=box, style=rounded];
 *
 *    subgraph cluster_outbound {
 *        label="Outbound MBOX";
 *        style=rounded;
 *
 *        out_write [label="WRITE_DATA"];
 *        out_read [label="READ_DATA"];
 *    }
 *
 *    subgraph cluster_inbound {
 *        label="Inbound Mailbox";
 *        style=rounded;
 *
 *        in_read [label="READ_DATA"];
 *        in_write [label="WRITE_DATA"];
 *    }
 *
 *    // Data flow connections
 *    out_write -> in_read [label="data flow", penwidth=2];
 *    in_write -> out_read [label="data flow", penwidth=2];
 * }
 * @enddot
 *
 * @section messaging Messaging Protocol
 *
 * - **To message the SMC core**: write to the outbound mailbox channels (even channel numbers)
 * - **To message other system components**: write to the inbound mailbox channels (odd channel
 * numbers)
 * - **To listen for a message within the SMC core**: listen for interrupts on the inbound mailbox
 * channels
 * - **To listen for a message within other system components**: listen for interrupts on the
 * outbound mailbox channels (not supported by this driver)
 *
 * @section interrupts Interrupt Handling
 *
 * The SMC only has inbound mailbox interrupts connected, so in order to message
 * the SMC core a message must be written to the outbound mailbox. This can be
 * done using this driver or from a remote host writing to the SMC's
 * outbound mailbox registers directly.
 *
 * Outbound MBOX interrupts would be routed to other system components
 * (such as Ascalon) rather than the SMC core, and would be triggered
 * by writing to an inbound mailbox channel.
 */

LOG_MODULE_REGISTER(mbox_tt_grendel, CONFIG_MBOX_LOG_LEVEL);

#define DT_DRV_COMPAT tenstorrent_grendel_mbox

#define MBOX_CHANNEL_STRIDE          0x800
#define TT_GRENDEL_MBOX_MSG_MAX_SIZE 8

#define MBOX_WRITE_DATA_REG_OFFSET SMC_CPU_SMC_OUTBOUND_MAILBOX_0_WRITE_DATA_REG_OFFSET
#define MBOX_READ_DATA_REG_OFFSET  SMC_CPU_SMC_INBOUND_MAILBOX_0_READ_DATA_REG_OFFSET
#define MBOX_STATUS_REG_OFFSET     SMC_CPU_SMC_OUTBOUND_MAILBOX_0_STATUS_REG_OFFSET
#define MBOX_IRQEN_REG_OFFSET      SMC_CPU_SMC_OUTBOUND_MAILBOX_0_IRQEN_REG_OFFSET
#define MBOX_IRQP_REG_OFFSET       SMC_CPU_SMC_INBOUND_MAILBOX_0_IRQP_REG_OFFSET
#define MBOX_IRQS_REG_OFFSET       SMC_CPU_SMC_OUTBOUND_MAILBOX_0_IRQS_REG_OFFSET
#define MBOX_WIRQT_REG_OFFSET      SMC_CPU_SMC_OUTBOUND_MAILBOX_0_WIRQT_REG_OFFSET
#define MBOX_RIRQT_REG_OFFSET      SMC_CPU_SMC_INBOUND_MAILBOX_0_RIRQT_REG_OFFSET

/**
 * @brief Test class
 *
 * @dot
 * digraph mailbox_communication {
 *    rankdir=LR;
 *    node [shape=box, style=rounded];
 *
 *    subgraph cluster_outbound {
 *        label="Outbound MBOX";
 *        style=rounded;
 *        color=blue;
 *
 *        out_write [label="WRITE_DATA"];
 *        out_read [label="READ_DATA"];
 *    }
 *
 *    subgraph cluster_inbound {
 *        label="Inbound Mailbox";
 *        style=rounded;
 *        color=green;
 *
 *        in_read [label="READ_DATA"];
 *        in_write [label="WRITE_DATA"];
 *    }
 *
 *    // Data flow connections
 *    out_write -> in_read [label="data flow", color=red, penwidth=2];
 *    in_write -> out_read [label="data flow", color=red, penwidth=2];
 * }
 * @enddot
 */
#define MBOX_BASE(cfg, channel) (cfg->base_addr + (channel * MBOX_CHANNEL_STRIDE))

struct tt_grendel_mbox_config {
	void (*irq_config_func)(const struct device *dev);
	uint32_t base_addr;
	uint8_t channel_pairs;
};

struct tt_grendel_mbox_data {
	mbox_callback_t *callbacks;
	void **callback_user_data;
};

static int tt_grendel_mbox_mtu_get(const struct device *dev)
{
	return TT_GRENDEL_MBOX_MSG_MAX_SIZE;
}

static uint32_t tt_grendel_mbox_max_channels_get(const struct device *dev)
{
	const struct tt_grendel_mbox_config *cfg = dev->config;

	return cfg->channel_pairs * 2;
}

static int tt_grendel_mbox_register_callback(const struct device *dev, mbox_channel_id_t channel_id,
					     mbox_callback_t cb, void *user_data)
{
	struct tt_grendel_mbox_data *data = dev->data;

	if (channel_id >= tt_grendel_mbox_max_channels_get(dev)) {
		LOG_ERR("%s: Invalid channel ID %d, max is %d", __func__, channel_id,
			tt_grendel_mbox_max_channels_get(dev) - 1);
		return -EINVAL;
	}

	data->callbacks[channel_id] = cb;
	data->callback_user_data[channel_id] = user_data;
	return 0;
}

static int tt_grendel_mbox_send(const struct device *dev, mbox_channel_id_t channel_id,
				const struct mbox_msg *msg)
{
	const struct tt_grendel_mbox_config *cfg = dev->config;
	SMC_MAILBOX_STATUS_reg_u status;
	uint64_t data_to_write;

	LOG_DBG("Sending message on channel %d: data=%p, size=%zu", channel_id, msg->data,
		msg->size);

	if (channel_id >= tt_grendel_mbox_max_channels_get(dev)) {
		LOG_ERR("%s: Invalid channel ID %d, max is %d", __func__, channel_id,
			tt_grendel_mbox_max_channels_get(dev) - 1);
		return -EINVAL;
	}

	/* Check if there is space in the FIFO */
	status.val = sys_read64(MBOX_BASE(cfg, channel_id) + MBOX_STATUS_REG_OFFSET);
	if (status.f.write_level_above_thresh) {
		LOG_ERR("Outbound FIFO for channel %d is full, cannot send "
			"message",
			channel_id);
		return -EBUSY;
	}

	if (msg->size > tt_grendel_mbox_mtu_get(dev)) {
		LOG_ERR("Message size %zu exceeds maximum supported size of %d "
			"bytes",
			msg->size, tt_grendel_mbox_mtu_get(dev));
		return -EMSGSIZE;
	}
	memcpy(&data_to_write, msg->data, msg->size);
	sys_write64(data_to_write, MBOX_BASE(cfg, channel_id) + MBOX_WRITE_DATA_REG_OFFSET);
	return 0;
}

static int tt_grendel_mbox_set_enabled(const struct device *dev, mbox_channel_id_t channel_id,
				       bool enabled)
{

	const struct tt_grendel_mbox_config *cfg = dev->config;
	SMC_MAILBOX_IRQEN_reg_u irqen;

	if (channel_id >= tt_grendel_mbox_max_channels_get(dev)) {
		LOG_ERR("%s: Invalid channel ID %d, max is %d", __func__, channel_id,
			tt_grendel_mbox_max_channels_get(dev) - 1);
		return -EINVAL;
	}
	LOG_DBG("%sabling channel %d", enabled ? "En" : "Dis", channel_id);

	/* Enable/disable interrupt for channel */
	irqen.val = sys_read64(MBOX_BASE(cfg, channel_id) + MBOX_IRQEN_REG_OFFSET);
	irqen.f.rtirq = enabled ? 1 : 0;
	sys_write64(irqen.val, MBOX_BASE(cfg, channel_id) + MBOX_IRQEN_REG_OFFSET);
	return 0;
}

static DEVICE_API(mbox, tt_grendel_mbox_api) = {
	.send = tt_grendel_mbox_send,
	.mtu_get = tt_grendel_mbox_mtu_get,
	.max_channels_get = tt_grendel_mbox_max_channels_get,
	.set_enabled = tt_grendel_mbox_set_enabled,
	.register_callback = tt_grendel_mbox_register_callback,
};

static int tt_grendel_mbox_init(const struct device *dev)
{
	const struct tt_grendel_mbox_config *cfg = dev->config;
	SMC_CPU_CTRL_RESET_CTRL_reg_u reset_ctrl;

	/* Clear mailbox reset */
	reset_ctrl.val = sys_read32(SMC_CPU_SMC_CPU_CTRL_RESET_CTRL_REG_ADDR);
	reset_ctrl.f.mailbox_reset_n_n0_scan = 1;
	sys_write32(reset_ctrl.val, SMC_CPU_SMC_CPU_CTRL_RESET_CTRL_REG_ADDR);

	cfg->irq_config_func(dev);

	for (int i = 0; i < tt_grendel_mbox_max_channels_get(dev); i++) {
		/*
		 * Disable interrupts, and set read/write FIFO threshold to 0
		 * For outbound and inbound channels.
		 */
		sys_write64(0, MBOX_BASE(cfg, i) + MBOX_IRQEN_REG_OFFSET);
		sys_write64(0, MBOX_BASE(cfg, i) + MBOX_WIRQT_REG_OFFSET);
		sys_write64(0, MBOX_BASE(cfg, i) + MBOX_RIRQT_REG_OFFSET);
	}
	return 0;
}

static void tt_grendel_mbox_isr(const struct device *dev, mbox_channel_id_t channel_id)
{
	const struct tt_grendel_mbox_config *cfg = dev->config;
	struct tt_grendel_mbox_data *data = dev->data;
	SMC_MAILBOX_STATUS_reg_u status;
	SMC_MAILBOX_IRQP_reg_u irqp;
	uint64_t mbox_data;
	struct mbox_msg msg;

	LOG_DBG("MBOX ISR triggered for device %s, channel %d", dev->name, channel_id);

	/* ISRs are only mapped for inbound channels, read status from there */
	status.val = sys_read64(MBOX_BASE(cfg, channel_id) + MBOX_STATUS_REG_OFFSET);
	irqp.val = sys_read64(MBOX_BASE(cfg, channel_id) + MBOX_IRQP_REG_OFFSET);
	/* Clear the interrupt by writing to IRQS. We only ACK enabled
	 * interrupts
	 */
	sys_write64(irqp.val, MBOX_BASE(cfg, channel_id) + MBOX_IRQS_REG_OFFSET);

	LOG_DBG("Status register: 0x%016llx, IRQP register: 0x%016llx", status.val, irqp.val);

	if (!status.f.empty) {
		mbox_data = sys_read64(MBOX_BASE(cfg, channel_id) + MBOX_READ_DATA_REG_OFFSET);
		LOG_DBG("Received message on channel %d: data=0x%016llx", channel_id, mbox_data);
		/* Pass data to callback */
		if (data->callbacks[channel_id]) {
			msg.data = &mbox_data;
			msg.size = sizeof(mbox_data);
			data->callbacks[channel_id](dev, channel_id,
						    data->callback_user_data[channel_id], &msg);
		} else {
			LOG_WRN("No callback registered for channel %d, "
				"dropping message",
				channel_id);
		}
	}
}

#define TT_GRENDEL_MBOX_CHANNEL_IRQ_HANDLER_DEFINE(channel_id, _)                                  \
	static void tt_grendel_mbox_isr_##channel_id(const struct device *dev)                     \
	{                                                                                          \
		/* Interrupts only fire on inbound channel IDs */                                  \
		tt_grendel_mbox_isr(dev, (channel_id * 2) + 1);                                    \
	}

#define TT_GRENDEL_MBOX_CHANNEL_IRQ_HANDLER_CONNECT(channel_id, inst)                              \
	IRQ_CONNECT(DT_INST_IRQN_BY_IDX(inst, channel_id),                                         \
		    DT_INST_IRQ_BY_IDX(inst, channel_id, priority),                                \
		    tt_grendel_mbox_isr_##channel_id, DEVICE_DT_INST_GET(inst), 0);                \
	irq_enable(DT_INST_IRQN_BY_IDX(inst, channel_id));

#define TT_GRENDEL_MBOX_DEFINE(inst)                                                               \
	LISTIFY(DT_INST_PROP(inst, channel_pairs), \
		TT_GRENDEL_MBOX_CHANNEL_IRQ_HANDLER_DEFINE, \
		(;))                                               \
                                                                                                   \
	static void tt_grendel_mbox_irq_config_func_##inst(const struct device *dev)               \
	{                                                                                          \
		ARG_UNUSED(dev);                                                                   \
		LISTIFY(DT_INST_PROP(inst, channel_pairs), \
			TT_GRENDEL_MBOX_CHANNEL_IRQ_HANDLER_CONNECT, (;), inst);        \
	}                                                                                          \
                                                                                                   \
	static const struct tt_grendel_mbox_config tt_grendel_mbox_config_##inst = {               \
		.irq_config_func = tt_grendel_mbox_irq_config_func_##inst,                         \
		.base_addr = DT_INST_REG_ADDR(inst),                                               \
		.channel_pairs = DT_INST_PROP(inst, channel_pairs),                                \
	};                                                                                         \
                                                                                                   \
	static mbox_callback_t                                                                     \
		tt_grendel_mbox_callbacks_##inst[DT_INST_PROP(inst, channel_pairs)];               \
	static void *tt_grendel_mbox_callback_user_data_##inst[DT_INST_PROP(inst, channel_pairs)]; \
                                                                                                   \
	static struct tt_grendel_mbox_data tt_grendel_mbox_data_##inst = {                         \
		.callbacks = tt_grendel_mbox_callbacks_##inst,                                     \
		.callback_user_data = tt_grendel_mbox_callback_user_data_##inst,                   \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(inst, tt_grendel_mbox_init, NULL, &tt_grendel_mbox_data_##inst,      \
			      &tt_grendel_mbox_config_##inst, POST_KERNEL,                         \
			      CONFIG_MBOX_INIT_PRIORITY, &tt_grendel_mbox_api);

DT_INST_FOREACH_STATUS_OKAY(TT_GRENDEL_MBOX_DEFINE)
