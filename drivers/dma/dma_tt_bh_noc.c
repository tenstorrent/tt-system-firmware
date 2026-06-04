/*
 * Copyright (c) 2025 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT tenstorrent_noc_dma

#include <zephyr/device.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/dma/dma_tt_bh_noc.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/kernel.h>
#include <string.h>

#include "noc_init.h"
#include "noc2axi.h"
#include "util.h"

LOG_MODULE_REGISTER(dma_noc_tt_bh, CONFIG_DMA_LOG_LEVEL);

#define NOC_DMA_TLB        0
#define NOC_DMA_NOC_ID     0
#define NOC_DMA_TIMEOUT_MS 50
#define NOC_MAX_BURST_SIZE 16384

#define DMA_MAX_TRANSFER_BLOCKS 4

/* NOC CMD fields */
#define NOC_CMD_CPY               (0 << 0)
#define NOC_CMD_RD                (0 << 1)
#define NOC_CMD_WR                (1 << 1)
#define NOC_CMD_RESP_MARKED       (1 << 4)
#define NOC_CMD_BRCST_PACKET      (1 << 5)
#define NOC_CMD_PATH_RESERVE      (1 << 8)
#define NOC_CMD_BRCST_SRC_INCLUDE (1 << 17)

/* NOC0 RISC0 DMA registers */
#define TARGET_ADDR_LO           0xFFB20000
#define TARGET_ADDR_MID          0xFFB20004
#define TARGET_ADDR_HI           0xFFB20008
#define RET_ADDR_LO              0xFFB2000C
#define RET_ADDR_MID             0xFFB20010
#define RET_ADDR_HI              0xFFB20014
#define PACKET_TAG               0xFFB20018
#define CMD_BRCST                0xFFB2001C
#define AT_LEN                   0xFFB20020
#define AT_LEN_1                 0xFFB20024
#define AT_DATA                  0xFFB20028
#define BRCST_EXCLUDE            0xFFB2002C
#define CMD_CTRL                 0xFFB20040
#define NIU_MST_WR_ACK_RECEIVED  0xFFB20204
#define NIU_MST_RD_RESP_RECEIVED 0xFFB20208

/* Define invalid channel constant - using a high value that's unlikely to be used */
#define DMA_CHANNEL_INVALID 0xFFFFFFFF

struct tt_bh_dma_channel_resettable_data {
	/* Hardware completion tracking for get_status() */
	uint32_t last_noc_cmd;
	uint32_t last_expected_acks;
	uint16_t block_index;
	uint16_t block_count;
	bool configured: 1;
};

struct tt_bh_dma_channel_data {
	struct dma_block_config blocks[DMA_MAX_TRANSFER_BLOCKS];
	struct tt_bh_dma_noc_coords coords;
	struct dma_config config;
	struct tt_bh_dma_channel_resettable_data state;
};

/*
 * Initialized in the TT_BH_DMA_NOC_INIT() macro.
 */
struct tt_bh_dma_noc_config {
	/* Initialized to a static array of channels. */
	struct tt_bh_dma_channel_data *channels;

	/* Initialized to the dma-channels devicetree property. */
	uint8_t num_channels;
};

/*
 * Initialized in the tt_bh_dma_noc_init() functions.
 */
struct tt_bh_dma_noc_data {
	struct k_spinlock lock;
};

static bool noc_wait_cmd_ready(void)
{
	uint32_t cmd_ctrl;
	k_timepoint_t timeout = sys_timepoint_calc(K_MSEC(NOC_DMA_TIMEOUT_MS));

	do {
		cmd_ctrl = NOC2AXIRead32(NOC_DMA_NOC_ID, NOC_DMA_TLB, CMD_CTRL);
	} while (cmd_ctrl != 0 && !sys_timepoint_expired(timeout));

	return cmd_ctrl == 0;
}

static uint32_t get_expected_acks(uint32_t noc_cmd, uint64_t size)
{
	uint32_t ack_reg_addr =
		(noc_cmd & NOC_CMD_WR) ? NIU_MST_WR_ACK_RECEIVED : NIU_MST_RD_RESP_RECEIVED;
	uint32_t packet_received = NOC2AXIRead32(NOC_DMA_NOC_ID, NOC_DMA_TLB, ack_reg_addr);
	uint32_t expected_acks = packet_received + DIV_ROUND_UP(size, NOC_MAX_BURST_SIZE);

	return expected_acks;
}

/* wrap around aware comparison for half-range rule */
static inline bool is_behind(uint32_t current, uint32_t target)
{
	/*
	 * "target" and "current" are NOC transaction counters, and may wrap around, so we must
	 * consider the case where target and current have wrapped a different number of times.
	 * There's no way to know how many times they have wrapped, instead we assume that they are
	 * within 2**31 of each other as that gives an unambiguous ordering.
	 *
	 * We deal with this by considering
	 * target - 2**31 <  current < target         MOD 2**32 as before target and
	 * target         <= current < target + 2**31 MOD 2**32 as after target.
	 *
	 * We can't just check target == current because just one spurious NOC transaction could
	 * result in the loop hanging with current = target+1.
	 */
	return (int32_t)(current - target) < 0;
}

static inline uint32_t noc_coord_encode(uint32_t x, uint32_t y)
{
	return (y << 6) | x;
}

static inline uint32_t noc_coord_encode_range(uint32_t start_x, uint32_t start_y, uint32_t end_x,
					      uint32_t end_y)
{
	return (start_y << 18) | (start_y << 12) | (end_y << 6) | end_x;
}

static void handle_transfer_callbacks(const struct device *dev,
				      struct tt_bh_dma_channel_data *chan_data, uint32_t channel,
				      int transfer_ret, bool is_final_block)
{
	if (!chan_data->config.dma_callback) {
		return;
	}

	if (transfer_ret == 0) {
		/* Success callbacks */
		if (chan_data->config.complete_callback_en && !is_final_block) {
			/* Per-block callback */
			chan_data->config.dma_callback(dev, chan_data->config.user_data, channel,
						       DMA_STATUS_BLOCK);
		}

		if (is_final_block) {
			/* Transfer completion callback */
			chan_data->config.dma_callback(dev, chan_data->config.user_data, channel,
						       DMA_STATUS_COMPLETE);
		}
	} else if (!chan_data->config.error_callback_dis) {
		/* Error callback - pass negative errno */
		chan_data->config.dma_callback(dev, chan_data->config.user_data, channel, -EIO);
	}
}

static int noc_dma_transfer(uint32_t cmd, uint32_t ret_coord, uint64_t ret_addr,
			    uint32_t targ_coord, uint64_t targ_addr, uint32_t size, bool multicast,
			    uint8_t transaction_id, bool include_self, uint32_t *noc_cmd_out,
			    uint32_t *expected_acks_out)
{
	uint32_t ret_addr_lo = low32(ret_addr);
	uint32_t ret_addr_mid = high32(ret_addr);
	uint32_t ret_addr_hi = ret_coord;

	uint32_t targ_addr_lo = low32(targ_addr);
	uint32_t targ_addr_mid = high32(targ_addr);
	uint32_t targ_addr_hi = targ_coord;

	uint32_t noc_at_len_be = size;
	uint32_t noc_packet_tag = transaction_id << 10;

	uint32_t noc_ctrl = NOC_CMD_CPY | cmd;

	if (multicast) {
		noc_ctrl |= NOC_CMD_PATH_RESERVE | NOC_CMD_BRCST_PACKET;

		if (include_self) {
			noc_ctrl |= NOC_CMD_BRCST_SRC_INCLUDE;
		}
	}

	/* Always enable response marking for completion tracking */
	noc_ctrl |= NOC_CMD_RESP_MARKED;
	uint32_t expected_acks = get_expected_acks(noc_ctrl, size);

	/* Return tracking info to caller */
	if (noc_cmd_out) {
		*noc_cmd_out = noc_ctrl;
	}
	if (expected_acks_out) {
		*expected_acks_out = expected_acks;
	}

	if (!noc_wait_cmd_ready()) {
		LOG_ERR("Waiting for transfer command timed out");
		return -ETIMEDOUT;
	}

	NOC2AXIWrite32(NOC_DMA_NOC_ID, NOC_DMA_TLB, TARGET_ADDR_LO, targ_addr_lo);
	NOC2AXIWrite32(NOC_DMA_NOC_ID, NOC_DMA_TLB, TARGET_ADDR_MID, targ_addr_mid);
	NOC2AXIWrite32(NOC_DMA_NOC_ID, NOC_DMA_TLB, TARGET_ADDR_HI, targ_addr_hi);
	NOC2AXIWrite32(NOC_DMA_NOC_ID, NOC_DMA_TLB, RET_ADDR_LO, ret_addr_lo);
	NOC2AXIWrite32(NOC_DMA_NOC_ID, NOC_DMA_TLB, RET_ADDR_MID, ret_addr_mid);
	NOC2AXIWrite32(NOC_DMA_NOC_ID, NOC_DMA_TLB, RET_ADDR_HI, ret_addr_hi);
	NOC2AXIWrite32(NOC_DMA_NOC_ID, NOC_DMA_TLB, PACKET_TAG, noc_packet_tag);
	NOC2AXIWrite32(NOC_DMA_NOC_ID, NOC_DMA_TLB, AT_LEN, noc_at_len_be);
	NOC2AXIWrite32(NOC_DMA_NOC_ID, NOC_DMA_TLB, AT_LEN_1, 0);
	NOC2AXIWrite32(NOC_DMA_NOC_ID, NOC_DMA_TLB, AT_DATA, 0);
	NOC2AXIWrite32(NOC_DMA_NOC_ID, NOC_DMA_TLB, BRCST_EXCLUDE, 0);
	NOC2AXIWrite32(NOC_DMA_NOC_ID, NOC_DMA_TLB, CMD_BRCST, noc_ctrl);
	NOC2AXIWrite32(NOC_DMA_NOC_ID, NOC_DMA_TLB, CMD_CTRL, 1);

	return 0;
}

/*
 * Config the source and dest NOC coordinates, the source and dest addresses and
 * the size of data transfer.
 */
static int tt_bh_dma_noc_config(const struct device *dev, uint32_t channel,
				struct dma_config *config, struct tt_bh_dma_noc_coords *coords)
{
	struct tt_bh_dma_noc_data *dma_data = (struct tt_bh_dma_noc_data *)dev->data;
	const struct tt_bh_dma_noc_config *dma_cfg =
		(const struct tt_bh_dma_noc_config *)dev->config;

	if (config->block_count == 0) {
		LOG_ERR("No block configuration provided");
		return -EINVAL;
	}
	if (config->block_count > DMA_MAX_TRANSFER_BLOCKS) {
		LOG_ERR("Too many blocks: %u > %u", config->block_count, DMA_MAX_TRANSFER_BLOCKS);
		return -EINVAL;
	}
	if (channel >= dma_cfg->num_channels) {
		LOG_ERR("Invalid channel %u", channel);
		return -EINVAL;
	}

	struct tt_bh_dma_channel_data *chan_data = &dma_cfg->channels[channel];

	k_spinlock_key_t key = k_spin_lock(&dma_data->lock);

	/* Deep copy all blocks from the linked list */
	struct dma_block_config *src_block = config->head_block;

	for (int i = 0; i < config->block_count && src_block != NULL; i++) {
		chan_data->blocks[i] = *src_block;
		/* Clear the next_block pointer since we're storing in an array */
		chan_data->blocks[i].next_block = NULL;

		src_block = src_block->next_block;
	}

	chan_data->state.block_index = 0;
	chan_data->state.block_count = config->block_count;
	chan_data->config = *config;
	/* Update the config to point to our copied blocks */
	chan_data->config.head_block = &chan_data->blocks[0];
	chan_data->state.configured = true;
	chan_data->state.last_noc_cmd = 0;
	chan_data->state.last_expected_acks = 0;

	chan_data->coords = *coords;

	k_spin_unlock(&dma_data->lock, key);

	return 0;
}

static int tt_bh_dma_noc_base_config(const struct device *dev, uint32_t channel,
				     struct dma_config *config)
{
	/* Set NOC to ARC*/
	struct tt_bh_dma_noc_coords coords = {
		.dest_x = 8U, /*ARC_X*/
		.dest_y = 0U, /*ARC_Y*/
	};
	GetEnabledTensix(&coords.source_x, &coords.source_y);
	return tt_bh_dma_noc_config(dev, channel, config, &coords);
}

static int noc_dma_write_multicast(uint8_t local_x, uint8_t local_y, uint64_t local_addr,
				   uint8_t remote_start_x, uint8_t remote_start_y,
				   uint8_t remote_end_x, uint8_t remote_end_y, uint64_t remote_addr,
				   uint32_t size, bool include_self, uint32_t *noc_cmd_out,
				   uint32_t *expected_acks_out)
{
	uint32_t ret_coord =
		noc_coord_encode_range(remote_start_x, remote_start_y, remote_end_x, remote_end_y);
	uint64_t ret_addr = remote_addr;

	uint32_t targ_coord = noc_coord_encode(local_x, local_y);
	uint64_t targ_addr = local_addr;

	NOC2AXITlbSetup(NOC_DMA_NOC_ID, NOC_DMA_TLB, local_x, local_y, TARGET_ADDR_LO);

	return noc_dma_transfer(NOC_CMD_WR, ret_coord, ret_addr, targ_coord, targ_addr, size, true,
				0, include_self, noc_cmd_out, expected_acks_out);
}

static int tt_bh_dma_noc_start_mem_to_per(const struct device *dev, uint32_t channel)
{
	const struct tt_bh_dma_noc_config *cfg = (const struct tt_bh_dma_noc_config *)dev->config;
	struct tt_bh_dma_channel_data *chan_data = &cfg->channels[channel];
	struct tt_bh_dma_noc_coords *coords = &chan_data->coords;
	struct dma_block_config *current_block = &chan_data->blocks[chan_data->state.block_index];

	uint32_t ret_coord = noc_coord_encode(coords->source_x, coords->source_y);
	uint64_t ret_addr = current_block->source_address;

	uint32_t targ_coord = noc_coord_encode(coords->dest_x, coords->dest_y);
	uint64_t targ_addr = current_block->dest_address;

	NOC2AXITlbSetup(NOC_DMA_NOC_ID, NOC_DMA_TLB, coords->source_x, coords->source_y,
			TARGET_ADDR_LO);

	return noc_dma_transfer(NOC_CMD_RD, ret_coord, ret_addr, targ_coord, targ_addr,
				current_block->block_size, false, 0, false,
				&chan_data->state.last_noc_cmd,
				&chan_data->state.last_expected_acks);
}

static int tt_bh_dma_noc_start_per_to_mem(const struct device *dev, uint32_t channel)
{
	const struct tt_bh_dma_noc_config *cfg = (const struct tt_bh_dma_noc_config *)dev->config;
	struct tt_bh_dma_channel_data *chan_data = &cfg->channels[channel];
	struct tt_bh_dma_noc_coords *coords = &chan_data->coords;
	struct dma_block_config *current_block = &chan_data->blocks[chan_data->state.block_index];

	uint32_t ret_coord = noc_coord_encode(coords->dest_x, coords->dest_y);
	uint64_t ret_addr = current_block->dest_address;

	uint32_t targ_coord = noc_coord_encode(coords->source_x, coords->source_y);
	uint64_t targ_addr = current_block->source_address;

	NOC2AXITlbSetup(NOC_DMA_NOC_ID, NOC_DMA_TLB, coords->source_x, coords->source_y,
			TARGET_ADDR_LO);

	return noc_dma_transfer(NOC_CMD_WR, ret_coord, ret_addr, targ_coord, targ_addr,
				current_block->block_size, false, 0, false,
				&chan_data->state.last_noc_cmd,
				&chan_data->state.last_expected_acks);
}

static int tt_bh_dma_noc_start(const struct device *dev, uint32_t channel)
{
	const struct tt_bh_dma_noc_config *cfg = (const struct tt_bh_dma_noc_config *)dev->config;

	if (channel >= cfg->num_channels) {
		LOG_ERR("Invalid channel %u", channel);
		return -EINVAL;
	}

	struct tt_bh_dma_channel_data *chan_data = &cfg->channels[channel];

	if (!chan_data->state.configured) {
		LOG_ERR("Channel %u not configured", channel);
		return -EINVAL;
	}

	chan_data->state.block_index = 0;

	struct tt_bh_dma_noc_coords *coords = &chan_data->coords;
	struct dma_block_config *current_block = &chan_data->blocks[chan_data->state.block_index];

	if (!current_block) {
		LOG_ERR("No valid block configuration");
		return -EINVAL;
	}

	int ret;

	/* Handle different transfer types - all asynchronous except for MEMORY to MEMORY */
	for (; chan_data->state.block_index < chan_data->state.block_count;
	     chan_data->state.block_index++) {
		current_block = &chan_data->blocks[chan_data->state.block_index];
		if (!current_block) {
			LOG_ERR("No valid block configuration");
			return -EINVAL;
		}

		switch (chan_data->config.channel_direction) {
		case MEMORY_TO_MEMORY: {
			int dest_address_ = current_block->dest_address;

			current_block->dest_address = current_block->source_address;
			current_block->source_address = 0;

			ret = tt_bh_dma_noc_start_mem_to_per(dev, channel);
			if (ret != 0) {
				return ret;
			}

			current_block->dest_address = dest_address_;

			/* Wait for read operation to complete */
			k_timepoint_t timeout = sys_timepoint_calc(K_MSEC(NOC_DMA_TIMEOUT_MS));
			struct dma_status status;

			do {
				dma_get_status(dev, channel, &status);
			} while (status.busy && !sys_timepoint_expired(timeout));

			if (sys_timepoint_expired(timeout)) {
				return -ETIMEDOUT;
			}

			ret = tt_bh_dma_noc_start_per_to_mem(dev, channel);
			if (ret != 0) {
				return ret;
			}

			/* Wait for write operation to complete */
			timeout = sys_timepoint_calc(K_MSEC(NOC_DMA_TIMEOUT_MS));

			do {
				dma_get_status(dev, channel, &status);
			} while (status.busy && !sys_timepoint_expired(timeout));

			if (sys_timepoint_expired(timeout)) {
				return -ETIMEDOUT;
			}

			/* Invoke callback function at transfer or block completion */
			handle_transfer_callbacks(dev, chan_data, channel, 0,
						  chan_data->state.block_index + 1 ==
							  chan_data->state.block_count);

			break;
		}
		case MEMORY_TO_PERIPHERAL: {
			return tt_bh_dma_noc_start_mem_to_per(dev, channel);
		}
		case PERIPHERAL_TO_MEMORY: {
			return tt_bh_dma_noc_start_per_to_mem(dev, channel);
		}
		case TT_BH_DMA_NOC_CHANNEL_DIRECTION_BROADCAST: {
			/* Use pre translation coords as NOC translation has enabled. */
			uint8_t remote_start_x = 2;
			uint8_t remote_start_y = 2;
			uint8_t remote_end_x = 1;
			uint8_t remote_end_y = 11;
			uint64_t local_addr = current_block->source_address;
			uint64_t remote_addr = current_block->dest_address;

			ret = noc_dma_write_multicast(
				coords->dest_x, coords->dest_y, local_addr, remote_start_x,
				remote_start_y, remote_end_x, remote_end_y, remote_addr,
				current_block->block_size, false, &chan_data->state.last_noc_cmd,
				&chan_data->state.last_expected_acks);

			return ret;
		}
		default:
			LOG_ERR("Invalid channel direction %d",
				chan_data->config.channel_direction);
			return -EINVAL;
		}

		if (chan_data->config.linked_channel != DMA_CHANNEL_INVALID) {
			uint32_t linked_chan = chan_data->config.linked_channel;

			if (linked_chan < cfg->num_channels &&
			    cfg->channels[linked_chan].state.configured) {
				if (chan_data->config.dest_chaining_en ||
				    chan_data->config.source_chaining_en) {
					tt_bh_dma_noc_start(dev, linked_chan);
				}
			}
		}
	}

	return 0;
}

static int tt_bh_dma_noc_init(const struct device *dev)
{
	return 0;
}

static int tt_bh_dma_noc_get_status(const struct device *dev, uint32_t channel,
				    struct dma_status *status)
{
	const struct tt_bh_dma_noc_config *dma_cfg =
		(const struct tt_bh_dma_noc_config *)dev->config;

	if (channel >= dma_cfg->num_channels) {
		return -EINVAL;
	}

	struct tt_bh_dma_channel_data *chan_data = &dma_cfg->channels[channel];

	uint32_t ack_reg_addr = (chan_data->state.last_noc_cmd & NOC_CMD_WR)
					? NIU_MST_WR_ACK_RECEIVED
					: NIU_MST_RD_RESP_RECEIVED;
	uint32_t ack_received = NOC2AXIRead32(NOC_DMA_NOC_ID, NOC_DMA_TLB, ack_reg_addr);

	status->busy = is_behind(ack_received, chan_data->state.last_expected_acks);

	return 0;
}

static int tt_bh_dma_noc_stop(const struct device *dev, uint32_t channel)
{
	return 0;
}

static DEVICE_API(tt_bh_dma_noc, tt_bh_dma_noc_api) = {
	.dma_api.config = tt_bh_dma_noc_base_config,
	.dma_api.reload = NULL,
	.dma_api.start = tt_bh_dma_noc_start,
	.dma_api.stop = tt_bh_dma_noc_stop,
	.dma_api.suspend = NULL,
	.dma_api.resume = NULL,
	.dma_api.get_status = tt_bh_dma_noc_get_status,
	.dma_api.get_attribute = NULL,
	.dma_api.chan_filter = NULL,
	.dma_api.chan_release = NULL,
	.config = tt_bh_dma_noc_config,
};

#define TT_BH_DMA_NOC_INIT(inst)                                                                   \
	static struct tt_bh_dma_channel_data                                                       \
		tt_bh_dma_noc_channels_##inst[DT_INST_PROP(inst, dma_channels)];                   \
                                                                                                   \
	static const struct tt_bh_dma_noc_config noc_dma_config_##inst = {                         \
		.channels = tt_bh_dma_noc_channels_##inst,                                         \
		.num_channels = DT_INST_PROP(inst, dma_channels),                                  \
	};                                                                                         \
                                                                                                   \
	static struct tt_bh_dma_noc_data noc_dma_data_##inst = {};                                 \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(inst, &tt_bh_dma_noc_init, NULL, &noc_dma_data_##inst,               \
			      &noc_dma_config_##inst, POST_KERNEL, CONFIG_DMA_INIT_PRIORITY,       \
			      &tt_bh_dma_noc_api);

DT_INST_FOREACH_STATUS_OKAY(TT_BH_DMA_NOC_INIT)
