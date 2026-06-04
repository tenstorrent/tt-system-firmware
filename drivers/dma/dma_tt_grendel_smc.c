/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT tenstorrent_grendel_smc_dma

#include <zephyr/device.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>

#include <platform.h>

LOG_MODULE_REGISTER(dma_tt_grendel_smc);

#define DMA_REG_STATUS(ch)  (SMC_CPU_AXI_DATA_ACCEL_AXI_DMA_CTRL_STATUS_0_REG_OFFSET + (ch) * 0x4)
#define DMA_REG_NEXT_ID(ch) (SMC_CPU_AXI_DATA_ACCEL_AXI_DMA_CTRL_NEXT_ID_0_REG_OFFSET + (ch) * 0x8)
#define DMA_REG_DONE(ch)    (SMC_CPU_AXI_DATA_ACCEL_AXI_DMA_CTRL_DONE_0_REG_OFFSET + (ch) * 0x4)

#define DMA_READ(reg) sys_read32(cfg->base + SMC_CPU_AXI_DATA_ACCEL_AXI_DMA_CTRL_##reg##_REG_OFFSET)

#define DMA_READ_CH(reg, ch) sys_read32(cfg->base + DMA_REG_##reg(ch))
#define DMA_WRITE(reg, val)                                                                        \
	sys_write32((val), cfg->base + SMC_CPU_AXI_DATA_ACCEL_AXI_DMA_CTRL_##reg##_REG_OFFSET)

struct dma_grendel_channel_data {
	struct dma_config cfg;
	struct dma_block_config blocks[CONFIG_DMA_TT_GRENDEL_SMC_MAX_BLOCKS];
	uint32_t transfer_id;
	bool configured;
};

struct dma_grendel_config {
	uintptr_t base;
	struct dma_grendel_channel_data *channels;
	uint8_t num_channels;
};

struct dma_grendel_data {
	struct k_spinlock lock;
};

static uint32_t dma_transfer(const struct dma_grendel_config *cfg, uint32_t channel,
			     const struct dma_block_config *block)
{
	uint64_t src = block->source_address;
	uint64_t dst = block->dest_address;
	uint64_t len = block->block_size;
	uint32_t reps = block->source_gather_count;

	DMA_WRITE(SRC_ADDRESS_LO, (uint32_t)src);
	DMA_WRITE(SRC_ADDRESS_HI, (uint32_t)(src >> 32));
	DMA_WRITE(DST_ADDRESS_LO, (uint32_t)dst);
	DMA_WRITE(DST_ADDRESS_HI, (uint32_t)(dst >> 32));
	DMA_WRITE(LENGTH_LO, (uint32_t)len);
	DMA_WRITE(LENGTH_HI, (uint32_t)(len >> 32));
	DMA_WRITE(SRC_STRIDE_LO, block->source_gather_interval);
	DMA_WRITE(SRC_STRIDE_HI, 0);
	DMA_WRITE(DST_STRIDE_LO, block->dest_scatter_interval);
	DMA_WRITE(DST_STRIDE_HI, 0);
	DMA_WRITE(NUM_REPETITIONS_LO, reps > 0 ? reps : 1);
	DMA_WRITE(NUM_REPETITIONS_HI, 0);

	return DMA_READ_CH(NEXT_ID, channel);
}

static int dma_grendel_config(const struct device *dev, uint32_t channel, struct dma_config *config)
{
	const struct dma_grendel_config *cfg = dev->config;
	struct dma_grendel_data *data = dev->data;

	if (channel >= cfg->num_channels) {
		LOG_ERR("Invalid channel %u (max %u)", channel, cfg->num_channels - 1);
		return -EINVAL;
	}

	if (config->block_count == 0 || config->head_block == NULL) {
		LOG_ERR("No block configuration provided");
		return -EINVAL;
	}

	if (config->block_count > CONFIG_DMA_TT_GRENDEL_SMC_MAX_BLOCKS) {
		LOG_ERR("Too many blocks %u (max %u)", config->block_count,
			CONFIG_DMA_TT_GRENDEL_SMC_MAX_BLOCKS);
		return -EINVAL;
	}

	struct dma_grendel_channel_data *ch = &cfg->channels[channel];
	k_spinlock_key_t key = k_spin_lock(&data->lock);

	ch->cfg = *config;

	const struct dma_block_config *src_block = config->head_block;

	for (uint32_t i = 0; i < config->block_count && src_block != NULL; i++) {
		ch->blocks[i] = *src_block;
		ch->blocks[i].next_block =
			(i + 1 < config->block_count) ? &ch->blocks[i + 1] : NULL;
		src_block = src_block->next_block;
	}

	ch->cfg.head_block = &ch->blocks[0];
	ch->transfer_id = 0;
	ch->configured = true;

	k_spin_unlock(&data->lock, key);
	return 0;
}

static int dma_grendel_start(const struct device *dev, uint32_t channel)
{
	const struct dma_grendel_config *cfg = dev->config;
	struct dma_grendel_data *data = dev->data;
	struct dma_grendel_channel_data *ch;
	const struct dma_block_config *block;
	k_timepoint_t timeout;
	k_spinlock_key_t key;

	if (channel >= cfg->num_channels) {
		LOG_ERR("Invalid channel %u", channel);
		return -EINVAL;
	}

	ch = &cfg->channels[channel];

	if (!ch->configured) {
		LOG_ERR("Channel %u not configured", channel);
		return -EINVAL;
	}

	block = ch->cfg.head_block;

	for (uint32_t i = 0; i < ch->cfg.block_count && block != NULL; i++) {
		key = k_spin_lock(&data->lock);
		ch->transfer_id = dma_transfer(cfg, channel, block);
		k_spin_unlock(&data->lock, key);

		if (ch->transfer_id == 0) {
			LOG_ERR("DMA transfer failed on channel %u block %u", channel, i);
			return -EIO;
		}

		/* DMA hardware lacks a completion interrupt so we poll the DONE register. */
		timeout = sys_timepoint_calc(K_MSEC(CONFIG_DMA_TT_GRENDEL_SMC_TIMEOUT_MS));
		while (DMA_READ_CH(DONE, channel) < ch->transfer_id) {
			if (sys_timepoint_expired(timeout)) {
				LOG_ERR("DMA ch%u timed out (id=%u)", channel, ch->transfer_id);
				return -ETIMEDOUT;
			}
		}

		block = block->next_block;
	}

	if (ch->cfg.dma_callback) {
		ch->cfg.dma_callback(dev, ch->cfg.user_data, channel, DMA_STATUS_COMPLETE);
	}

	return 0;
}

static int dma_grendel_stop(const struct device *dev, uint32_t channel)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(channel);

	return 0;
}

static int dma_grendel_get_status(const struct device *dev, uint32_t channel,
				  struct dma_status *status)
{
	const struct dma_grendel_config *cfg = dev->config;

	if (channel >= cfg->num_channels) {
		return -EINVAL;
	}

	struct dma_grendel_channel_data *ch = &cfg->channels[channel];

	status->busy = (DMA_READ_CH(STATUS, channel) & AXI_DMA_CTRL_STATUS_BUSY_MASK) != 0;

	if (ch->transfer_id > 0 && DMA_READ_CH(DONE, channel) < ch->transfer_id) {
		status->pending_length = ch->cfg.head_block->block_size;
	} else {
		status->pending_length = 0;
	}

	return 0;
}

static int dma_grendel_init(const struct device *dev)
{
	const struct dma_grendel_config *cfg = dev->config;
	SMC_CPU_CTRL_RESET_CTRL_reg_u reset_ctrl;
	AXI_DMA_CTRL_CONFIG_reg_u config = {.f.enabled_nd = 1};

	/* Release DMA from reset */
	reset_ctrl.val = sys_read32(SMC_CPU_SMC_CPU_CTRL_RESET_CTRL_REG_ADDR);
	reset_ctrl.f.dma_reset_n_n0_scan = 1;
	sys_write32(reset_ctrl.val, SMC_CPU_SMC_CPU_CTRL_RESET_CTRL_REG_ADDR);

	DMA_WRITE(CONFIG, config.val);

	return 0;
}

static DEVICE_API(dma, dma_grendel_api) = {
	.config = dma_grendel_config,
	.reload = NULL,
	.start = dma_grendel_start,
	.stop = dma_grendel_stop,
	.suspend = NULL,
	.resume = NULL,
	.get_status = dma_grendel_get_status,
	.get_attribute = NULL,
	.chan_filter = NULL,
	.chan_release = NULL,
};

#define DMA_GRENDEL_INIT(inst)                                                                     \
	static struct dma_grendel_channel_data                                                     \
		dma_grendel_channels_##inst[DT_INST_PROP(inst, dma_channels)];                     \
                                                                                                   \
	static const struct dma_grendel_config dma_grendel_config_##inst = {                       \
		.base = DT_INST_REG_ADDR(inst),                                                    \
		.channels = dma_grendel_channels_##inst,                                           \
		.num_channels = DT_INST_PROP(inst, dma_channels),                                  \
	};                                                                                         \
                                                                                                   \
	static struct dma_grendel_data dma_grendel_data_##inst = {};                               \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(inst, &dma_grendel_init, NULL, &dma_grendel_data_##inst,             \
			      &dma_grendel_config_##inst, POST_KERNEL, CONFIG_DMA_INIT_PRIORITY,   \
			      &dma_grendel_api);

DT_INST_FOREACH_STATUS_OKAY(DMA_GRENDEL_INIT)
