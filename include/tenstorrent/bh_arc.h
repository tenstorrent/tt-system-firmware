/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef INCLUDE_TENSTORRENT_LIB_BH_ARC_H_
#define INCLUDE_TENSTORRENT_LIB_BH_ARC_H_

#include <stdint.h>

#include <zephyr/drivers/smbus.h>
#include <zephyr/drivers/gpio.h>

typedef enum {
	kCm2DmMsgIdNull = 0,
	kCm2DmMsgIdResetReq = 1,
	kCm2DmMsgIdPing = 2,
	kCm2DmMsgIdFanSpeedUpdate = 3,
	kCm2DmMsgIdReady = 4,
	kCm2DmMsgIdAutoResetTimeoutUpdate = 5,
	kCm2DmMsgTelemHeartbeatUpdate = 6,
	kCm2DmMsgIdForcedFanSpeedUpdate = 7,
	kCm2DmMsgIdLedBlink = 8,
	kCm2DmMsgIdGddrThermTrip = 9,
	kCm2DmMsgCount
} Cm2DmMsgId;

/* These values are used by CM2DM and MSG_TYPE_TRIGGER_RESET. */
typedef enum {
	kCm2DmResetLevelAsic = 0,
	kCm2DmResetLevelDmc = 3,
} Cm2DmResetLevel;

/* Payload for kCm2DmMsgIdGddrThermTrip. */
typedef enum {
	kGddrThermTripReasonSustained = 0,
	kGddrThermTripReasonInstantaneous = 1,
} GddrThermTripReason;

/* GDDR thermal trip thresholds */
#define GDDR_THERM_TRIP_TEMP          95  /* sustained over-temp threshold, degrees Celsius */
#define GDDR_THERM_TRIP_CRITICAL_TEMP 110 /* instantaneous trip threshold, degrees Celsius */
#define GDDR_THERM_TRIP_DURATION_MIN  1   /* sustained dwell time before trip, minutes */

typedef struct dmStaticInfo {
	/*
	 * Non-zero for valid data
	 * Allows for breaking changes
	 */
	uint32_t version;
	uint32_t bl_version;
	uint32_t app_version;
	uint32_t arc_start_time;   /* Timestamp in ASIC refclk (50 MHz) */
	uint32_t dm_init_duration; /* Duration in DMC refclk (64 MHz) */
	uint32_t arc_hang_pc;      /* Program counter during last ARC hang */
} __packed dmStaticInfo;

typedef struct cm2dmMessage {
	uint8_t msg_id;
	uint8_t seq_num;
	uint32_t data;
} __packed cm2dmMessage;

typedef struct cm2dmAck {
	uint8_t msg_id;
	uint8_t seq_num;
} __packed cm2dmAck;

union cm2dmAckWire {
	cm2dmAck f;
	uint16_t val;
};

struct bh_arc {
	const struct smbus_dt_spec smbus;
	const struct gpio_dt_spec enable;
	const struct device *i2c_dev;
};

typedef struct cm2dmMessageRet {
	cm2dmMessage msg;
	int ret;

	cm2dmAck ack;
	int ack_ret;
} cm2dmMessageRet;

int bharc_smbus_block_read(const struct bh_arc *dev, uint8_t cmd, uint8_t *count, uint8_t *output);
int bharc_smbus_block_write(const struct bh_arc *dev, uint8_t cmd, uint8_t count, uint8_t *input);
int bharc_smbus_word_data_write(const struct bh_arc *dev, uint16_t cmd, uint16_t word);
int bharc_smbus_word_data_read(const struct bh_arc *dev, uint16_t cmd, uint16_t *word);
int bharc_smbus_byte_data_write(const struct bh_arc *dev, uint8_t cmd, uint8_t word);
int bharc_smbus_block_write_block_read(const struct bh_arc *dev, uint8_t cmd, uint8_t snd_count,
				       uint8_t *send_buf, uint8_t *rcv_count, uint8_t *rcv_buf);
int bharc_enable_i2cbus(const struct bh_arc *dev);
int bharc_disable_i2cbus(const struct bh_arc *dev);

#define BH_ARC_INIT(n)                                                                             \
	{.smbus = SMBUS_DT_SPEC_GET(n),                                                            \
	 .i2c_dev = DEVICE_DT_GET(DT_PHANDLE(DT_BUS(n), i2c)),                                     \
	 .enable = COND_CODE_1(DT_PROP_HAS_IDX(n, gpios, 0),	({	\
			.port = DEVICE_DT_GET(DT_GPIO_CTLR_BY_IDX(n, gpios, 0)),                   \
			.pin = DT_GPIO_PIN_BY_IDX(n, gpios, 0),                                    \
			.dt_flags = DT_GPIO_FLAGS_BY_IDX(n, gpios, 0),                             \
		}), ({})) }

#endif
