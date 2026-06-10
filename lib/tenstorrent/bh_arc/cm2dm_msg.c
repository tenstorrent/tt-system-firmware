/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cm2dm_msg.c
 * @brief CMFW to DMFW message handling
 *
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/misc/bh_fwtable.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <tenstorrent/smc_msg.h>
#include <tenstorrent/msgqueue.h>

#include "cm2dm_msg.h"
#include "asic_state.h"
#include "reg.h"
#include "status_reg.h"
#include "fan_ctrl.h"
#include "telemetry.h"
#include "dvfs.h"

static const struct device *const fwtable_dev = DEVICE_DT_GET(DT_NODELABEL(fwtable));

typedef struct {
	atomic_t pending_messages;
	uint8_t next_id_rr;
	uint8_t next_seq_num;

	bool curr_msg_valid;
	cm2dmMessage curr_msg;

	volatile uint32_t next_msgs[kCm2DmMsgCount];
} Cm2DmMsgState;

static Cm2DmMsgState cm2dm_msg_state;
K_SEM_DEFINE(dmfw_ping_sem, 0, 1);
static uint16_t power;
static uint16_t telemetry_reg;
static struct {
	uint8_t chip_reset_asic_called: 1;
	uint8_t chip_reset_dmc_called: 1;
} chip_reset_state;
static uint8_t reset_type;

void PostCm2DmMsg(Cm2DmMsgId msg_id, uint32_t data)
{
	cm2dm_msg_state.next_msgs[msg_id] = data;
	atomic_set_bit(&cm2dm_msg_state.pending_messages, msg_id);
}

static Cm2DmMsgId next_id_rr(uint32_t pending_messages)
{
	uint32_t hi_pending = pending_messages & GENMASK(31, cm2dm_msg_state.next_id_rr);
	uint32_t search_messages = hi_pending ? hi_pending : pending_messages;

	uint32_t next_message_id = LOG2(LSB_GET(search_messages));

	/* next_message_id + 1 ensures that the message type we chose this time becomes lowest
	 * priority next time.
	 */
	cm2dm_msg_state.next_id_rr = (next_message_id + 1) % kCm2DmMsgCount;

	return (Cm2DmMsgId)next_message_id;
}

int32_t Cm2DmMsgReqSmbusHandler(uint8_t *data, uint8_t *size)
{
	BUILD_ASSERT(sizeof(cm2dm_msg_state.curr_msg) == 6,
		     "Unexpected size of cm2dm_msg_state.curr_msg");
	*size = sizeof(cm2dm_msg_state.curr_msg);

	if (!cm2dm_msg_state.curr_msg_valid) {
		atomic_val_t pending_messages = atomic_get(&cm2dm_msg_state.pending_messages);

		if (pending_messages != 0) {
			Cm2DmMsgId next_message_id = next_id_rr(pending_messages);

			atomic_clear_bit(&cm2dm_msg_state.pending_messages, next_message_id);
			/* atomic_clear_bit must be before reading curr_msg_data.
			 * A data update may be done by writing data first then setting the bit.
			 * We might send the same data twice, but we'll always send the final
			 * value.
			 */

			cm2dm_msg_state.curr_msg.msg_id = next_message_id;
			cm2dm_msg_state.curr_msg.seq_num = cm2dm_msg_state.next_seq_num++;
			cm2dm_msg_state.curr_msg.data = cm2dm_msg_state.next_msgs[next_message_id];
			cm2dm_msg_state.curr_msg_valid = true;
		}
	}

	memcpy(data, &cm2dm_msg_state.curr_msg, sizeof(cm2dm_msg_state.curr_msg));
	return 0;
}

int32_t Cm2DmMsgAckSmbusHandler(const uint8_t *data, uint8_t size)
{
	BUILD_ASSERT(sizeof(cm2dmAck) == 2, "Unexpected size of cm2dmAck");
	if (size != sizeof(cm2dmAck)) {
		return -1;
	}

	cm2dmAck *ack = (cm2dmAck *)data;

	if (cm2dm_msg_state.curr_msg_valid && ack->msg_id == cm2dm_msg_state.curr_msg.msg_id &&
	    ack->seq_num == cm2dm_msg_state.curr_msg.seq_num) {
		/* Message handled when msg_id and seq_num match the current valid message */
		cm2dm_msg_state.curr_msg_valid = false;
		memset(&cm2dm_msg_state.curr_msg, 0, sizeof(cm2dm_msg_state.curr_msg));

		return 0;
	} else {
		return -1;
	}
}

void IssueChipReset(Cm2DmResetLevel reset_level)
{
	lock_down_for_reset();
	chip_reset_state.chip_reset_asic_called |= reset_level == kCm2DmResetLevelAsic;
	chip_reset_state.chip_reset_dmc_called |= reset_level == kCm2DmResetLevelDmc;
	/* Send a reset request to the DMFW */
	PostCm2DmMsg(kCm2DmMsgIdResetReq, reset_level);
}

void ChipResetRequest(void *arg)
{
	uint32_t irq_num = POINTER_TO_UINT(arg);

	irq_disable(irq_num); /* So we don't get repeatedly interrupted */

	IssueChipReset(kCm2DmResetLevelAsic);
}

void UpdateFanSpeedRequest(uint32_t fan_speed)
{
	PostCm2DmMsg(kCm2DmMsgIdFanSpeedUpdate, fan_speed);
}

void UpdateForcedFanSpeedRequest(uint32_t fan_speed)
{
	PostCm2DmMsg(kCm2DmMsgIdForcedFanSpeedUpdate, fan_speed);
}

void Dm2CmReadyRequest(void)
{
	/* Send a message to dmfw to indicate ready to receive messages */
	PostCm2DmMsg(kCm2DmMsgIdReady, 0);
}

void UpdateAutoResetTimeoutRequest(uint32_t timeout)
{
	PostCm2DmMsg(kCm2DmMsgIdAutoResetTimeoutUpdate, timeout); /* in ms */
}

void UpdateTelemHeartbeatRequest(uint32_t heartbeat)
{
	PostCm2DmMsg(kCm2DmMsgTelemHeartbeatUpdate, heartbeat); /* in ms */
}

void RequestLedBlink(uint32_t blink_mode)
{
	PostCm2DmMsg(kCm2DmMsgIdLedBlink, blink_mode);
}

void ReportGddrThermTrip(GddrThermTripReason reason)
{
	PostCm2DmMsg(kCm2DmMsgIdGddrThermTrip, (uint32_t)reason);
}

void reset_request_handler(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	IssueChipReset(reset_type);
}

K_TIMER_DEFINE(reset_timer, reset_request_handler, NULL);

/**
 * @brief Handler for @ref TT_SMC_MSG_TRIGGER_RESET
 * @see trigger_reset_rqst
 */
static uint8_t reset_dm_handler(const union request *request, struct response *response)
{
	reset_type = request->trigger_reset.reset_level;

	/* Don't expect a response from the dmfw so need to check here for a valid reset level */
	uint8_t ret = 0;

	switch (reset_type) {
	case kCm2DmResetLevelAsic:
	case kCm2DmResetLevelDmc:
		/* Delay slightly to allow SMC response to be sent before reset occurs */
		k_timer_start(&reset_timer, K_MSEC(5), K_NO_WAIT);
		break;
	default:
		/* Can never be zero because that case is covered by asic reset */
		ret = reset_type;
	}

	return ret;
}

REGISTER_MESSAGE(TT_SMC_MSG_TRIGGER_RESET, reset_dm_handler);

/**
 * @brief Handler for host request to ping DMC
 *
 * Handler for a host request to ping the DMC. This request is used to test
 * SMBUS stability.
 * @param[in] request The request, of type @ref dmc_ping_rqst, with command code
 *	@ref TT_SMC_MSG_PING_DM to ping the DMC. If @ref dmc_ping_rqst::legacy_ping is
 *	false, The SMC requests a block read; otherwise, the SMC requests a block write.
 * @param[out] response The response to the host. Set to 1 if DMC is alive, 0 otherwise.
 * @return 0
 */
static uint8_t ping_dm_handler(const union request *request, struct response *response)
{
	int ret;
	uint64_t timestamp;

	/* Send a ping to the dmfw */
	k_sem_reset(&dmfw_ping_sem);
	PostCm2DmMsg(kCm2DmMsgIdPing, request->dmc_ping.legacy_ping);
	/* Delay to allow DMFW to respond */
	timestamp = k_uptime_get();
	ret = k_sem_take(&dmfw_ping_sem, K_MSEC(CONFIG_TT_BH_ARC_DMFW_PING_TIMEOUT));
	/* Record the time it took for DMFW to respond in us */
	WriteReg(PING_DMFW_DURATION_REG_ADDR, k_uptime_delta(&timestamp));

	/* Send 1 if DMFW is alive, 0 otherwise */
	response->data[1] = (ret == 0);
	return 0;
}

REGISTER_MESSAGE(TT_SMC_MSG_PING_DM, ping_dm_handler);

/**
 * @brief Handler for @ref TT_SMC_MSG_SET_WDT_TIMEOUT
 *
 * Handler for a host request to set the watchdog timeout value. This can be used
 * to configure the watchdog timer period or disable it entirely.
 *
 * @param[in] request The request, of type @ref set_wdt_timeout_rqst, with command code
 *	@ref TT_SMC_MSG_SET_WDT_TIMEOUT. The @ref set_wdt_timeout_rqst::timeout_ms field
 *	specifies the timeout value in milliseconds. If set to 0, the watchdog is disabled.
 * @param[out] response The response to the host (unused for this handler).
 * @retval 0 Success
 * @retval ENODEV Watchdog device is not ready
 * @retval ENOTSUP Timeout is below the minimum feed interval
 * @return Other error codes for watchdog setup failures
 * @see set_wdt_timeout_rqst
 */
static uint8_t set_watchdog_timeout(const union request *request, struct response *response)
{
	const struct device *wdt_dev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(wdt0));
	struct wdt_timeout_cfg cfg = {0};
	int ret;

	if (!device_is_ready(wdt_dev)) {
		return ENODEV;
	}

	if (request->set_wdt_timeout.timeout_ms != 0) {
		/* Deny a timeout lower than our feed interval */
		if (request->set_wdt_timeout.timeout_ms <= CONFIG_TT_BH_ARC_WDT_FEED_INTERVAL) {
			return ENOTSUP;
		}
		cfg.window.max = request->set_wdt_timeout.timeout_ms;
		/* Program watchdog timeout */
		ret = wdt_install_timeout(wdt_dev, &cfg);
		if (ret < 0) {
			return 0 - ret;
		}
		ret = wdt_setup(wdt_dev, WDT_FLAG_RESET_CPU_CORE);
	} else {
		/* Turn off watchdog */
		ret = wdt_disable(wdt_dev);
	}
	return 0 - ret;
}

REGISTER_MESSAGE(TT_SMC_MSG_SET_WDT_TIMEOUT, set_watchdog_timeout);

int32_t Dm2CmSendDataHandler(const uint8_t *data, uint8_t size)
{
#ifndef CONFIG_TT_SMC_RECOVERY
	if (size != sizeof(dmStaticInfo)) {
		return -1;
	}

	dmStaticInfo *info = (dmStaticInfo *)data;

	if (info->version != 0) {
		UpdateDmFwVersion(info->bl_version, info->app_version);
		WriteReg(ARC_START_TIME_REG_ADDR, info->arc_start_time);
		WriteReg(PERST_TO_DMFW_INIT_DONE_REG_ADDR, info->dm_init_duration);
		if (info->arc_hang_pc != 0) {
			/* Record last fault PC */
			WriteReg(ARC_HANG_PC, info->arc_hang_pc);
		}
		return 0;
	}
#endif

	return -1;
}

int32_t Dm2CmPingHandler(const uint8_t *data, uint8_t size)
{
	if (size != 2) {
		return -1;
	}

	uint16_t response = *(uint16_t *)data;

	if (response != 0xA5A5) {
		return -1;
	}
	k_sem_give(&dmfw_ping_sem);
	return 0;
}

int32_t Dm2CmPingV2(uint8_t *data, uint8_t *size)
{
	*size = 2;

	data[0] = 0xA5;
	data[1] = 0xA5;
	k_sem_give(&dmfw_ping_sem);
	return 0;
}

int32_t Dm2CmSendPowerHandler(const uint8_t *data, uint8_t size)
{
	if (size != 2) {
		return -1;
	}

	AdjustDVFSTimer();

	power = sys_get_le16(data) +
		tt_bh_fwtable_get_fw_table(fwtable_dev)->chip_limits.additional_board_power;

	return 0;
}

/* TODO: Put these somewhere else? */

uint16_t GetInputPower(void)
{
	return power;
}

int32_t Dm2CmSendFanRPMHandler(const uint8_t *data, uint8_t size)
{
#ifndef CONFIG_TT_SMC_RECOVERY
	if (size != 2) {
		return -1;
	}

	SetFanRPM(sys_get_le16(data));

	return 0;
#endif

	return -1;
}

int32_t SMBusTelemRegHandler(const uint8_t *data, uint8_t size)
{
	if (size != 1) {
		return -1;
	}

	/* Load telemetry register with data */
	telemetry_reg = data[0];
	return 0;
}

int32_t SMBusTelemDataHandler(uint8_t *data, uint8_t *size)
{
	uint32_t telemetry_data;

	*size = 7U;
	data[0] = GetTelemetryTagValid(telemetry_reg) ? 0U : 1U;
	data[1] = 0U;
	data[2] = 0U;
	telemetry_data = GetTelemetryTag(telemetry_reg);
	memcpy(&data[3], &telemetry_data, sizeof(telemetry_data));
	return 0;
}

int32_t Dm2CmSendThermTripCountHandler(const uint8_t *data, uint8_t size)
{
	if (size != 2) {
		return -1;
	}

	uint32_t therm_trip_count = sys_get_le16(data);

	UpdateTelemetryThermTripCount(therm_trip_count);
	return 0;
}

int32_t Dm2CmWriteTelemetry(const uint8_t *data, uint8_t size)
{
	if (size != 33) {
		return -1;
	}
	/* nothing to do? Note this discards the PEC */
	return 0;
}

int32_t Dm2CmReadControlData(uint8_t *data, uint8_t *size)
{
	*size = 20U;
	(void)memset(data, 0U, *size);

	struct {
		uint32_t pcie_index: 8;
		uint32_t trigger_asic_reset: 1;
		uint32_t trigger_spi_copy_1_to_r: 1;
		uint32_t arc_state_a3_req: 1;
		uint32_t arc_state_a0_req: 1;
		uint32_t trigger_asic_and_m3_reset: 1;
		uint32_t clear_num_auto_reset: 1;
		uint32_t spare: 18;
	} ctl_data = {0};

	ctl_data.trigger_asic_reset = chip_reset_state.chip_reset_asic_called;
	ctl_data.trigger_asic_and_m3_reset = chip_reset_state.chip_reset_dmc_called;

	memcpy(&data[11], &ctl_data, sizeof(ctl_data));

	/* This PEC is not SMBUS compliant. SMBUS requires the PEC be computed on the whole
	 * message. Therefore it is calculated here in the command processor. It is kept
	 * here for compatibility with WH products.
	 */
	uint8_t pec = 0U;

	pec = crc8(size, 1, 0x7, 0U, false);
	pec = crc8(data, *size - 1, 0x7, pec, false);
	data[19] = pec;
	return 0;
}

const struct device *dmc_uart = DEVICE_DT_GET_OR_NULL(DT_ALIAS(dmc_vuart));

int32_t Dm2CmDMCLogHandler(const uint8_t *data, uint8_t size)
{
	/* Just print the log data for now */
	for (uint8_t i = 0; i < size; i++) {
		uart_poll_out(dmc_uart, data[i]);
	}
	return 0;
}
