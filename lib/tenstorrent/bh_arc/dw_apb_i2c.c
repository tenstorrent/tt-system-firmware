/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/drivers/i2c.h>
#include <string.h>
#include "timer.h"
#include "dw_apb_i2c.h"
#include "asic_state.h"
#include "reg.h"
#include "timer.h"
#include "util.h"

#define DW_APB_I2C_REG_MAP_BASE_ADDR      0x80060000
#define DW_APB_I2C1_REG_MAP_BASE_ADDR     0x80090000
#define DW_APB_I2C2_REG_MAP_BASE_ADDR     0x800A0000
#define RESET_UNIT_I2C_PAD_CNTL_REG_ADDR  0x800301C0
#define RESET_UNIT_I2C1_PAD_CNTL_REG_ADDR 0x800305CC
#define RESET_UNIT_I2C2_PAD_CNTL_REG_ADDR 0x800305D8
#define RESET_UNIT_I2C_PAD_DATA_REG_ADDR  0x800301C4
#define RESET_UNIT_I2C1_PAD_DATA_REG_ADDR 0x800305D0
#define RESET_UNIT_I2C2_PAD_DATA_REG_ADDR 0x800305DC
#define RESET_UNIT_I2C_CNTL_REG_ADDR      0x800300F0

#define DW_APB_I2C_IC_CON_REG_OFFSET                        0x00000000
#define DW_APB_I2C_IC_STATUS_REG_OFFSET                     0x00000070
#define DW_APB_I2C_IC_DATA_CMD_REG_OFFSET                   0x00000010
#define DW_APB_I2C_IC_TX_ABRT_SOURCE_REG_OFFSET             0x00000080
#define DW_APB_I2C_IC_CLR_TX_ABRT_REG_OFFSET                0x00000054
#define DW_APB_I2C_IC_SMBUS_THIGH_MAX_IDLE_COUNT_REG_OFFSET 0x000000C4
#define DW_APB_I2C_IC_TAR_REG_OFFSET                        0x00000004
#define DW_APB_I2C_IC_ENABLE_REG_OFFSET                     0x0000006C
#define DW_APB_I2C_IC_SAR_REG_OFFSET                        0x00000008
#define DW_APB_I2C_IC_SS_SCL_HCNT_REG_OFFSET                0x00000014
#define DW_APB_I2C_IC_SS_SCL_LCNT_REG_OFFSET                0x00000018
#define DW_APB_I2C_IC_FS_SPKLEN_REG_OFFSET                  0x000000A0
#define DW_APB_I2C_IC_SDA_HOLD_REG_OFFSET                   0x0000007C
#define DW_APB_I2C_IC_FS_SCL_HCNT_REG_OFFSET                0x0000001C
#define DW_APB_I2C_IC_FS_SCL_LCNT_REG_OFFSET                0x00000020
#define DW_APB_I2C_IC_RAW_INTR_STAT_REG_OFFSET              0x00000034
#define DW_APB_I2C_IC_CLR_RX_OVER_REG_OFFSET                0x00000048
#define DW_APB_I2C_IC_CLR_RD_REQ_REG_OFFSET                 0x00000050
#define DW_APB_I2C_IC_CLR_STOP_DET_REG_OFFSET               0x00000060

#define DW_APB_I2C_IC_CON_MASTER_MODE_MASK      0x1
#define DW_APB_I2C_IC_STATUS_TFE_MASK           0x4
#define DW_APB_I2C_IC_STATUS_TFNF_MASK          0x2
#define DW_APB_I2C_IC_STATUS_RFNE_MASK          0x8
#define RESET_UNIT_I2C_PAD_CTRL_TRIEN_SCL_MASK  0x1
#define RESET_UNIT_I2C_PAD_CTRL_TRIEN_SDA_MASK  0x2
#define RESET_UNIT_I2C_PAD_CNTL_PUEN_MASK       0xC
#define RESET_UNIT_I2C_PAD_CNTL_RXEN_MASK       0xC0
#define DW_APB_I2C_IC_STATUS_MST_ACTIVITY_MASK  0x20
#define RESET_UNIT_I2C_PAD_CNTL_TRIEN_MASK      0x3
#define RESET_UNIT_I2C_CNTL_RESET_MASK          0x10
#define DW_APB_I2C_IC_TAR_IC_TAR_MASK           0x3FF
#define DW_APB_I2C_IC_SAR_IC_SAR_MASK           0x3FF
#define DW_APB_I2C_IC_CON_IC_RESTART_EN_MASK    0x20
#define DW_APB_I2C_IC_CON_IC_SLAVE_DISABLE_MASK 0x40

#define DW_APB_I2C_IC_CON_SPEED_SHIFT     1
#define DW_APB_I2C_IC_DATA_CMD_CMD_SHIFT  8
#define DW_APB_I2C_IC_DATA_CMD_STOP_SHIFT 9
#define RESET_UNIT_I2C_PAD_CNTL_DRV_SHIFT 10

/* Timing parameters */
#define IC_SS_SCL_HCNT_DEFAULT 200
#define IC_SS_SCL_LCNT_DEFAULT 235
#define IC_FS_SPKLEN_DEFAULT   3
#define IC_SDA_HOLD_DEFAULT    15
#define IC_FS_SCL_HCNT_DEFAULT 30
#define IC_FS_SCL_LCNT_DEFAULT 65

/* TX ABORT macros */
#define IC_TX_ABRT_SOURCE_MASK 0xFFFFF

/* IC control macros */
#define IC_DATA_READ    (0x1 << DW_APB_I2C_IC_DATA_CMD_CMD_SHIFT)
#define IC_DATA_WRITE   (0x0 << DW_APB_I2C_IC_DATA_CMD_CMD_SHIFT)
#define IC_DATA_STOP    (0x1 << DW_APB_I2C_IC_DATA_CMD_STOP_SHIFT)
#define IC_DATA_RESTART (0x1 << DW_APB_I2C_IC_DATA_CMD_RESTART_SHIFT)

/* IC abort source */
/* starting from bit21, bit20-0 is reserved. */
#define IC_ABRT_A3_STATE (0x1 << 21)
#define IC_VERIFY_FAIL   (0x1 << 22)

#define GET_I2C_OFFSET(REG_NAME) DW_APB_I2C_##REG_NAME##_REG_OFFSET

typedef struct {
	uint32_t master_mode: 1;
	uint32_t speed: 2;
	uint32_t ic_10bitaddr_slave: 1;
	uint32_t ic_10bitaddr_master: 1;
	uint32_t ic_restart_en: 1;
	uint32_t ic_slave_disable: 1;
	uint32_t stop_det_ifaddressed: 1;
	uint32_t tx_empty_ctrl: 1;
	uint32_t rx_fifo_full_hld_ctrl: 1;
	uint32_t stop_det_if_master_active: 1;
	uint32_t bus_clear_feature_ctrl: 1;
	uint32_t rsvd_ic_con_1: 4;
	uint32_t rsvd_optional_sar_ctrl: 1;
	uint32_t smbus_slave_quick_en: 1;
	uint32_t smbus_arp_en: 1;
	uint32_t smbus_persistent_slv_addr_en: 1;
	uint32_t rsvd_0: 3;
	uint32_t rsvd_ic_sar2_smbus_arp_en: 1;
	uint32_t rsvd_ic_sar3_smbus_arp_en: 1;
	uint32_t rsvd_ic_sar4_smbus_arp_en: 1;
	uint32_t rsvd_ic_con_2: 6;
} DW_APB_I2C_IC_CON_reg_t;

typedef union {
	uint32_t val;
	DW_APB_I2C_IC_CON_reg_t f;
} DW_APB_I2C_IC_CON_reg_u;

#define DW_APB_I2C_IC_CON_REG_DEFAULT (0x00000065)

typedef struct {
	uint32_t rx_under: 1;
	uint32_t rx_over: 1;
	uint32_t rx_full: 1;
	uint32_t tx_over: 1;
	uint32_t tx_empty: 1;
	uint32_t rd_req: 1;
	uint32_t tx_abrt: 1;
	uint32_t rx_done: 1;
	uint32_t activity: 1;
	uint32_t stop_det: 1;
	uint32_t start_det: 1;
	uint32_t gen_call: 1;
	uint32_t restart_det: 1;
	uint32_t master_on_hold: 1;
	uint32_t scl_stuck_at_low: 1;
	uint32_t rsvd_wr_req: 1;
	uint32_t rsvd_slv_addr1_tag: 1;
	uint32_t rsvd_slv_addr2_tag: 1;
	uint32_t rsvd_slv_addr3_tag: 1;
	uint32_t rsvd_slv_addr4_tag: 1;
	uint32_t rsvd_ic_raw_intr_stat: 12;
} DW_APB_I2C_IC_RAW_INTR_STAT_reg_t;

typedef union {
	uint32_t val;
	DW_APB_I2C_IC_RAW_INTR_STAT_reg_t f;
} DW_APB_I2C_IC_RAW_INTR_STAT_reg_u;

#define DW_APB_I2C_IC_RAW_INTR_STAT_REG_DEFAULT (0x00000000)

extern uint8_t asic_state;
/* i2c_target_config for Zephyr callbacks in I2C slave mode */
struct i2c_target_config i2c_target_config[3];

static inline uint32_t GetI2CBaseAddress(uint32_t id)
{
	switch (id) {
	case 0:
		return DW_APB_I2C_REG_MAP_BASE_ADDR;
	case 1:
		return DW_APB_I2C1_REG_MAP_BASE_ADDR;
	case 2:
		return DW_APB_I2C2_REG_MAP_BASE_ADDR;
	default:
		return 0;
	}
}

bool IsValidI2CMasterId(uint32_t id)
{
	return GetI2CBaseAddress(id) != 0;
}

static inline uint32_t GetI2CRegAddr(uint32_t id, uint32_t offset)
{
	return GetI2CBaseAddress(id) + offset;
}

/* Get I2C_PAD_CNTL register offset with respect to RESET_UNIT. */
static inline uint32_t GetI2CPadCntlAddr(uint32_t id)
{
	switch (id) {
	case 0:
		return RESET_UNIT_I2C_PAD_CNTL_REG_ADDR;
	case 1:
		return RESET_UNIT_I2C1_PAD_CNTL_REG_ADDR;
	case 2:
		return RESET_UNIT_I2C2_PAD_CNTL_REG_ADDR;
	default:
		return 0;
	}
}

/* Get I2C_PAD_DATA register offset with respect to RESET_UNIT. */
static inline uint32_t GetI2CPadDataAddr(uint32_t id)
{
	switch (id) {
	case 0:
		return RESET_UNIT_I2C_PAD_DATA_REG_ADDR;
	case 1:
		return RESET_UNIT_I2C1_PAD_DATA_REG_ADDR;
	case 2:
		return RESET_UNIT_I2C2_PAD_DATA_REG_ADDR;
	default:
		return 0;
	}
}

/* Bitbang recovery sequence on I2C bus */
void I2CRecoverBus(uint32_t id)
{
	uint32_t drive_strength = 0x7F; /* 50% of max 0xFF */
	uint32_t i2c_cntl = (drive_strength << RESET_UNIT_I2C_PAD_CNTL_DRV_SHIFT) |
			    RESET_UNIT_I2C_PAD_CNTL_TRIEN_MASK;
	uint32_t i2c_rst_cntl = ReadReg(RESET_UNIT_I2C_CNTL_REG_ADDR);

	/* Disable I2C controller */
	WriteReg(GetI2CRegAddr(id, GET_I2C_OFFSET(IC_ENABLE)), 0);
	/* Release control of pads from I2C controller */
	WriteReg(RESET_UNIT_I2C_CNTL_REG_ADDR, i2c_rst_cntl & ~BIT(id));
	/* Init I2C pads for I2C controller */
	WriteReg(GetI2CPadCntlAddr(id), i2c_cntl);
	/* Set both pads to output low */
	WriteReg(GetI2CPadDataAddr(id), 0x0);
	/*
	 * First, manually hold SCL low for 150 ms. Per the SMBUS spec,
	 * we should only need to hold the line low for 25 ms, but that does
	 * not work reliably and this does...
	 */
	i2c_cntl ^= RESET_UNIT_I2C_PAD_CTRL_TRIEN_SCL_MASK;
	WriteReg(GetI2CPadCntlAddr(id), i2c_cntl);
	Wait(150 * WAIT_1MS);
	/*
	 * Bitbang I2C reset to unstick bus. Hold SDA low, toggle SCL 32 times to create 16
	 * clock cycles. Note we toggle the TRIEN bit, as when TRIEN is
	 * set the bus will be released and external pullups will
	 * drive SCL high.
	 */
	for (int i = 0; i < 32; i++) {
		i2c_cntl ^= RESET_UNIT_I2C_PAD_CTRL_TRIEN_SCL_MASK;
		WriteReg(GetI2CPadCntlAddr(id), i2c_cntl);
		Wait(100 * WAIT_1US);
	}
	/* Add stop condition- transition SDA to high while SCL is high. */
	WriteReg(GetI2CPadCntlAddr(id), RESET_UNIT_I2C_PAD_CTRL_TRIEN_SCL_MASK);
	Wait(100 * WAIT_1US);
	WriteReg(GetI2CPadCntlAddr(id),
		 RESET_UNIT_I2C_PAD_CTRL_TRIEN_SCL_MASK | RESET_UNIT_I2C_PAD_CTRL_TRIEN_SDA_MASK);
	Wait(100 * WAIT_1US);
	/* Restore pads to input mode */
	WriteReg(GetI2CPadCntlAddr(id), (drive_strength << RESET_UNIT_I2C_PAD_CNTL_DRV_SHIFT) |
						RESET_UNIT_I2C_PAD_CNTL_RXEN_MASK |
						RESET_UNIT_I2C_PAD_CNTL_TRIEN_MASK);
	/* Return control of pads to I2C controller */
	WriteReg(RESET_UNIT_I2C_CNTL_REG_ADDR, i2c_rst_cntl | BIT(id));
	/* Reenable I2C controller */
	WriteReg(GetI2CRegAddr(id, GET_I2C_OFFSET(IC_ENABLE)), 1);
}

static void WaitTxFifoEmpty(uint32_t id)
{
	uint32_t ic_status = 0;
	uint64_t ts = k_uptime_get();

	do {
		ic_status = ReadReg(GetI2CRegAddr(id, GET_I2C_OFFSET(IC_STATUS)));

		if (IS_ENABLED(CONFIG_TT_BH_ARC_I2C_TIMEOUT)) {
			if ((k_uptime_get() - ts) >
			    COND_CODE_1(CONFIG_TT_BH_ARC_I2C_TIMEOUT,
							(CONFIG_TT_BH_ARC_I2C_TIMEOUT_DURATION),
							(0))) {
				I2CRecoverBus(id);
				break;
			}
		}
	} while ((ic_status & DW_APB_I2C_IC_STATUS_TFE_MASK) == 0);
}

static void WaitTxFifoNotFull(uint32_t id)
{
	uint32_t ic_status = 0;
	uint64_t ts = k_uptime_get();

	do {
		ic_status = ReadReg(GetI2CRegAddr(id, GET_I2C_OFFSET(IC_STATUS)));
		if (IS_ENABLED(CONFIG_TT_BH_ARC_I2C_TIMEOUT)) {
			if ((k_uptime_get() - ts) >
			    COND_CODE_1(CONFIG_TT_BH_ARC_I2C_TIMEOUT,
							(CONFIG_TT_BH_ARC_I2C_TIMEOUT_DURATION),
							(0))) {
				I2CRecoverBus(id);
				break;
			}
		}
	} while ((ic_status & DW_APB_I2C_IC_STATUS_TFNF_MASK) == 0);
}

static void WaitMasterIdle(uint32_t id)
{
	uint32_t ic_status = 0;
	uint64_t ts = k_uptime_get();

	do {
		ic_status = ReadReg(GetI2CRegAddr(id, GET_I2C_OFFSET(IC_STATUS)));
		if (IS_ENABLED(CONFIG_TT_BH_ARC_I2C_TIMEOUT)) {
			if ((k_uptime_get() - ts) >
			    COND_CODE_1(CONFIG_TT_BH_ARC_I2C_TIMEOUT,
							(CONFIG_TT_BH_ARC_I2C_TIMEOUT_DURATION),
							(0))) {
				I2CRecoverBus(id);
				break;
			}
		}
	} while (ic_status & DW_APB_I2C_IC_STATUS_MST_ACTIVITY_MASK);
}

static void WriteTxFifo(uint32_t id, uint32_t data)
{
	WaitTxFifoNotFull(id);
	WriteReg(GetI2CRegAddr(id, GET_I2C_OFFSET(IC_DATA_CMD)), data);
}

/* TODO: this function should log tx abort in CSM. */
/* Check TX_ABRT and return the source of TX_ABRT if any, otherwise return 0. */
static uint32_t CheckTxAbrt(uint32_t id)
{
	uint32_t tx_abrt_source = ReadReg(GetI2CRegAddr(id, GET_I2C_OFFSET(IC_TX_ABRT_SOURCE)));
	uint32_t ic_error = tx_abrt_source & IC_TX_ABRT_SOURCE_MASK;

	/* clear error status and return error */
	if (ic_error) {
		ReadReg(GetI2CRegAddr(id, GET_I2C_OFFSET(IC_CLR_TX_ABRT)));
		return ic_error;
	}
	return 0;
}

/* Wait until TX FIFO is empty and I2C master is idle. */
/* Return immediately upon TX_ABRT, otherwise return 0. */
static uint32_t WaitAllTxDone(uint32_t id)
{
	uint32_t master_active = 0;
	uint32_t tx_fifo_not_empty = 0;
	uint64_t ts = k_uptime_get();

	do {
		uint32_t ic_error = CheckTxAbrt(id);

		if (ic_error) {
			return ic_error;
		}
		uint32_t ic_status = ReadReg(GetI2CRegAddr(id, GET_I2C_OFFSET(IC_STATUS)));

		master_active = ic_status & DW_APB_I2C_IC_STATUS_MST_ACTIVITY_MASK;
		tx_fifo_not_empty = (ic_status & DW_APB_I2C_IC_STATUS_TFE_MASK) == 0;

		if (IS_ENABLED(CONFIG_TT_BH_ARC_I2C_TIMEOUT)) {
			if ((k_uptime_get() - ts) >
			    COND_CODE_1(CONFIG_TT_BH_ARC_I2C_TIMEOUT,
							(CONFIG_TT_BH_ARC_I2C_TIMEOUT_DURATION),
							(0))) {
				I2CRecoverBus(id);
				return -ETIMEDOUT;
			}
		}
	} while (master_active || tx_fifo_not_empty);
	return 0;
}

/* Reads RX FIFO when it is not empty. */
/* Return immediately upon TX_ABRT, otherwise return 0. */
uint32_t I2CReadRxFifo(uint32_t id, uint8_t *p_read_buf)
{
	uint32_t ic_status = 0;
	uint64_t ts = k_uptime_get();

	do {
		uint32_t ic_error = CheckTxAbrt(id);

		if (ic_error) {
			return ic_error;
		}
		ic_status = ReadReg(GetI2CRegAddr(id, GET_I2C_OFFSET(IC_STATUS)));

		if (IS_ENABLED(CONFIG_TT_BH_ARC_I2C_TIMEOUT)) {
			if ((k_uptime_get() - ts) >
			    COND_CODE_1(CONFIG_TT_BH_ARC_I2C_TIMEOUT,
							(CONFIG_TT_BH_ARC_I2C_TIMEOUT_DURATION),
							(0))) {
				I2CRecoverBus(id);
				return -ETIMEDOUT;
			}
		}
	} while ((ic_status & DW_APB_I2C_IC_STATUS_RFNE_MASK) == 0);
	*p_read_buf = ReadReg(GetI2CRegAddr(id, GET_I2C_OFFSET(IC_DATA_CMD)));
	return 0;
}

void I2CInitGPIO(uint32_t id)
{
	/* initialize I2C pads for i2c controller */
	uint32_t drive_strength = 0x7F; /* 50% of max 0xFF */

	WriteReg(GetI2CPadCntlAddr(id), (drive_strength << RESET_UNIT_I2C_PAD_CNTL_DRV_SHIFT) |
						RESET_UNIT_I2C_PAD_CNTL_RXEN_MASK |
						RESET_UNIT_I2C_PAD_CNTL_TRIEN_MASK);
	WriteReg(GetI2CPadDataAddr(id), 0);

	uint32_t i2c_cntl = ReadReg(RESET_UNIT_I2C_CNTL_REG_ADDR);

	WriteReg(RESET_UNIT_I2C_CNTL_REG_ADDR, i2c_cntl | 1 << id);
}

/* Initialize I2C controller by setting up I2C pads and configuration settings. */
void I2CInit(I2CMode mode, uint32_t slave_addr, I2CSpeedMode speed, uint32_t id)
{
	if (asic_state == A3State) {
		return;
	}

	WaitTxFifoEmpty(id);
	WaitMasterIdle(id);

	I2CInitGPIO(id);

	/* configure dw_apb_i2c controller */
	WriteReg(GetI2CRegAddr(id, GET_I2C_OFFSET(IC_ENABLE)), 0);
	Wait(10 * WAIT_1US);
	/* lower the number of wait cycles for idle bus from 0xffff (default) to 0xf for now */
	WriteReg(GetI2CRegAddr(id, GET_I2C_OFFSET(IC_SMBUS_THIGH_MAX_IDLE_COUNT)), 0xf);
	/* program interrupt FIFO threshold */

	if (mode == I2CMst) {
		WriteReg(GetI2CRegAddr(id, GET_I2C_OFFSET(IC_TAR)),
			 slave_addr & DW_APB_I2C_IC_TAR_IC_TAR_MASK);
		WriteReg(GetI2CRegAddr(id, GET_I2C_OFFSET(IC_CON)),
			 DW_APB_I2C_IC_CON_MASTER_MODE_MASK |
				 (speed << DW_APB_I2C_IC_CON_SPEED_SHIFT) |
				 DW_APB_I2C_IC_CON_IC_RESTART_EN_MASK |
				 DW_APB_I2C_IC_CON_IC_SLAVE_DISABLE_MASK);
	} else {
		WriteReg(GetI2CRegAddr(id, GET_I2C_OFFSET(IC_SAR)),
			 slave_addr & DW_APB_I2C_IC_SAR_IC_SAR_MASK);
		DW_APB_I2C_IC_CON_reg_u ic_con = {
			.f.master_mode = 0,
			.f.speed = speed,
			.f.ic_slave_disable = 0,
			.f.rx_fifo_full_hld_ctrl = 1,
			.f.stop_det_ifaddressed = 1,
		};
		WriteReg(GetI2CRegAddr(id, GET_I2C_OFFSET(IC_CON)), ic_con.val);

		i2c_target_config[id].address = slave_addr;
		i2c_target_config[id].flags = 0;
		i2c_target_config[id].callbacks = NULL;
	}

	/* section 2.9, 2.14.4.6, 2.16 for calculation */
	if (speed == I2CStandardMode) {
		WriteReg(GetI2CRegAddr(id, GET_I2C_OFFSET(IC_SS_SCL_HCNT)), IC_SS_SCL_HCNT_DEFAULT);
		WriteReg(GetI2CRegAddr(id, GET_I2C_OFFSET(IC_SS_SCL_LCNT)), IC_SS_SCL_LCNT_DEFAULT);
		WriteReg(GetI2CRegAddr(id, GET_I2C_OFFSET(IC_FS_SPKLEN)), IC_FS_SPKLEN_DEFAULT);
		WriteReg(GetI2CRegAddr(id, GET_I2C_OFFSET(IC_SDA_HOLD)), IC_SDA_HOLD_DEFAULT);
	} else {
		WriteReg(GetI2CRegAddr(id, GET_I2C_OFFSET(IC_FS_SCL_HCNT)), IC_FS_SCL_HCNT_DEFAULT);
		WriteReg(GetI2CRegAddr(id, GET_I2C_OFFSET(IC_FS_SCL_LCNT)), IC_FS_SCL_LCNT_DEFAULT);
		WriteReg(GetI2CRegAddr(id, GET_I2C_OFFSET(IC_FS_SPKLEN)), IC_FS_SPKLEN_DEFAULT);
		WriteReg(GetI2CRegAddr(id, GET_I2C_OFFSET(IC_SDA_HOLD)), IC_SDA_HOLD_DEFAULT);
	}

	WriteReg(GetI2CRegAddr(id, GET_I2C_OFFSET(IC_ENABLE)), 1);
	Wait(10 * WAIT_1US);
}

/* Resets the all I2C controller instances */
void I2CReset(void)
{
	uint32_t i2c_cntl = ReadReg(RESET_UNIT_I2C_CNTL_REG_ADDR);

	WriteReg(RESET_UNIT_I2C_CNTL_REG_ADDR, i2c_cntl | RESET_UNIT_I2C_CNTL_RESET_MASK);
	Wait(WAIT_1US);
	WriteReg(RESET_UNIT_I2C_CNTL_REG_ADDR, i2c_cntl & ~RESET_UNIT_I2C_CNTL_RESET_MASK);
	Wait(WAIT_1US);
}

/* Generalized transaction function called by I2CWriteBytes and I2CReadBytes, implements SMBUS write
 * bytes and read bytes protocols, returns TX_ABRT error if any, otherwise returns 0.
 */
uint32_t I2CTransaction(uint32_t id, const uint8_t *write_data, uint32_t write_len,
			uint8_t *read_data, uint32_t read_len)
{
	if (asic_state == A3State) {
		return IC_ABRT_A3_STATE;
	}

	/* Writing */
	for (uint32_t i = 0; i < write_len; i++) {
		uint32_t last_byte_flag = (read_len == 0 && i == write_len - 1) ? IC_DATA_STOP : 0;

		WriteTxFifo(id, write_data[i] | IC_DATA_WRITE | last_byte_flag);
	}

	uint32_t ic_error = 0;

	/* Only waits for TX done if it's a transaction that only writes data
	 * and doesn't read data
	 */
	if (read_len == 0) {
		ic_error = WaitAllTxDone(id);
	}

	if (ic_error) {
		return ic_error;
	}
	if (read_len == 0) {
		return ic_error; /* This ends the transaction if there is no data to be read */
	}

	/* Reading */
	for (uint32_t i = 0; i < read_len; i++) {
		uint32_t last_byte_flag = (i == read_len - 1) ? IC_DATA_STOP : 0;

		WriteTxFifo(id, IC_DATA_READ | last_byte_flag);
		ic_error = I2CReadRxFifo(id, &read_data[i]);
		if (ic_error) {
			return ic_error;
		}
	}
	return ic_error;
}

uint32_t I2CWriteBytes(uint32_t id, uint16_t command, uint32_t command_byte_size,
		       const uint8_t *p_write_buf, uint32_t data_byte_size)
{
	if (asic_state == A3State) {
		return IC_ABRT_A3_STATE;
	}

	const uint8_t *cmd_buf = (const uint8_t *)&command;

	for (uint32_t i = 0; i < command_byte_size; i++) {
		uint32_t last_byte_flag =
			(data_byte_size == 0 && i == command_byte_size - 1) ? IC_DATA_STOP : 0;

		WriteTxFifo(id, cmd_buf[i] | IC_DATA_WRITE | last_byte_flag);
	}

	if (p_write_buf != NULL) {
		for (uint32_t i = 0; i < data_byte_size; i++) {
			uint32_t last_byte_flag = (i == data_byte_size - 1) ? IC_DATA_STOP : 0;

			WriteTxFifo(id, p_write_buf[i] | IC_DATA_WRITE | last_byte_flag);
		}
	}

	/* This is a write-only transaction, so wait for completion and aborts. */
	if ((command_byte_size > 0) || (data_byte_size > 0)) {
		return WaitAllTxDone(id);
	}

	return 0;
}

uint32_t I2CReadBytes(uint32_t id, uint16_t command, uint32_t command_byte_size,
		      uint8_t *p_read_buf, uint32_t data_byte_size, uint8_t flip_bytes)
{
	uint32_t ic_error = I2CTransaction(id, (uint8_t *)&command, command_byte_size, p_read_buf,
					   data_byte_size);
	if (!ic_error && flip_bytes) {
		FlipBytes(p_read_buf, data_byte_size);
	}
	return ic_error;
}

/**
 * @brief I2C Read-Modify-Write-Verify
 */
uint32_t I2CRMWV(uint32_t id, uint16_t command, uint32_t command_byte_size, const uint8_t *p_data,
		 const uint8_t *p_mask, uint32_t data_byte_size)
{
	uint32_t ic_error;
	uint8_t buffer[data_byte_size];

	/* Read */
	ic_error = I2CReadBytes(id, command, command_byte_size, buffer, data_byte_size, 0);
	if (ic_error) {
		return ic_error;
	}

	/* Modify */
	for (uint32_t i = 0; i < data_byte_size; i++) {
		buffer[i] = (buffer[i] & ~p_mask[i]) | (p_data[i] & p_mask[i]);
	}

	/* Write */
	ic_error = I2CWriteBytes(id, command, command_byte_size, buffer, data_byte_size);
	if (ic_error) {
		return ic_error;
	}

	/* Verify */
	ic_error = I2CReadBytes(id, command, command_byte_size, buffer, data_byte_size, 0);
	if (ic_error) {
		return ic_error;
	}

	for (uint32_t i = 0; i < data_byte_size; i++) {
		if ((buffer[i] & p_mask[i]) != (p_data[i] & p_mask[i])) {
			return IC_VERIFY_FAIL;
		}
	}

	return 0;
}
