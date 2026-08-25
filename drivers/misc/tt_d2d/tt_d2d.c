/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/sys_io.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(tt_d2d, CONFIG_TT_D2D_LOG_LEVEL);

#include <zephyr/drivers/misc/tt_d2d.h>

#define DT_DRV_COMPAT tenstorrent_grendel_d2d

#ifdef CONFIG_DMA
BUILD_ASSERT(CONFIG_TT_D2D_INIT_PRIO > CONFIG_DMA_INIT_PRIORITY,
	     "TT_D2D_INIT_PRIO must be higher than DMA_INIT_PRIORITY so the DMA "
	     "controller is ready when a tile checks for it");
#endif

/*
 * Offsets within a D2D tile's register map.
 *
 * The Rocket's SRAM is a window inside the tile rather than a separate region,
 * and the firmware configuration block is not a register block at all: it is
 * an area near the top of that SRAM which the firmware reads once at startup.
 * Keeping the configuration offset SRAM-relative is what makes the collision
 * with the image (see TT_D2D_IMAGE_MAX below) obvious rather than a surprise.
 */
#define TT_D2D_STRAP_RESET_OFFSET 0x100000U
#define TT_D2D_CPU_CTRL_OFFSET    0x1800U
#define TT_D2D_SRAM_OFFSET        0x2000U
#define TT_D2D_CFG_OFFSET         0xFCC0U

/* Anything at or past the configuration block would be overwritten by it. */
#define TT_D2D_IMAGE_MAX TT_D2D_CFG_OFFSET

/* Word indices within the configuration block. */
#define TT_D2D_CFG_MAGIC             0U
#define TT_D2D_CFG_BITS_PER_CHANNEL  1U
#define TT_D2D_CFG_REFCLK_PERIOD_NS  2U
#define TT_D2D_CFG_APB_CLK_PERIOD_PS 3U
#define TT_D2D_CFG_PLL_FREQ_GHZ      4U
#define TT_D2D_CFG_TX_WILD_CLK       5U
#define TT_D2D_CFG_RX_WILD_CLK       6U
#define TT_D2D_CFG_BYPASS_VREG       7U
#define TT_D2D_CFG_PKG_TYPE_THIS     8U
#define TT_D2D_CFG_PKG_TYPE_OTHER    9U
#define TT_D2D_CFG_SLICE_COUNT       10U
#define TT_D2D_CFG_LPBK2_ENABLE      11U
#define TT_D2D_CFG_LPBK2_PATTERN     12U
#define TT_D2D_CFG_LPBK2_TX_SLICE    13U
#define TT_D2D_CFG_LPBK2_RX_SLICE    14U
#define TT_D2D_CFG_LPBK2_MEM_PATTERN 15U
#define TT_D2D_CFG_LPBK2_PRBS_SIZE   16U
#define TT_D2D_CFG_BYPASS_PHY        17U
#define TT_D2D_CFG_DISABLE_SIDEBAND  18U

/* Tells the firmware a host filled the configuration block in. */
#define TT_D2D_CFG_MAGIC_VALUE 0x1234BEAFU

/* Loopback-2 is a bring-up aid; these keep it off but consistently described. */
#define TT_D2D_LPBK2_PATTERN_MEMORY 1U
#define TT_D2D_LPBK2_RX_SLICE       4U
#define TT_D2D_LPBK2_MEM_PATTERN    0xABCDU
#define TT_D2D_LPBK2_PRBS_SIZE      4U

/* 0 = fast, 1 = slow. */
#define TT_D2D_TX_WILD_CLK 0U
#define TT_D2D_RX_WILD_CLK 1U

/*
 * CPU control fields. Reset default is all three asserted (0x00010101).
 *
 * Releasing uncore is what makes the Rocket's SRAM reachable from here, so it
 * has to come out before a load while the core stays in reset. debug_reset is
 * left asserted throughout: the reference bring-up keeps it that way, and
 * clearing it is not needed to run firmware.
 *
 * These are written whole rather than read-modify-written. Preserving unknown
 * bits out of a read is what breaks the Rocket, so the full value is always
 * stated explicitly.
 */
#define TT_D2D_CPU_CTRL_CORE_RESET   BIT(0)
#define TT_D2D_CPU_CTRL_UNCORE_RESET BIT(8)
#define TT_D2D_CPU_CTRL_DEBUG_RESET  BIT(16)

/* Core held, uncore out: SRAM reachable, firmware not yet running. */
#define TT_D2D_CPU_CTRL_HALT (TT_D2D_CPU_CTRL_DEBUG_RESET | TT_D2D_CPU_CTRL_CORE_RESET)
/* Core released: firmware runs. */
#define TT_D2D_CPU_CTRL_START TT_D2D_CPU_CTRL_DEBUG_RESET

/*
 * Written to the first SRAM word and read back before a load, to tell an
 * unreachable tile apart from a genuine load failure. Any value works; this
 * one is just recognisable in a bus trace.
 */
#define TT_D2D_PROBE_PATTERN 0xD2D0F00DU

/*
 * Strap reset bits, deasserted one per write in the order below. The order is
 * required by the hardware, so these are applied cumulatively rather than in a
 * single write.
 */
#define TT_D2D_STRAP_NOC2AXI_RESET_N BIT(0)
#define TT_D2D_STRAP_D2D_NOC_RESET_N BIT(1)
#define TT_D2D_STRAP_SYS_RST_NI      BIT(3)
#define TT_D2D_STRAP_AXI4L_ARESETN   BIT(4)
#define TT_D2D_STRAP_APB_RESETN      BIT(5)
#define TT_D2D_STRAP_LL_ARESETN      BIT(6)
#define TT_D2D_STRAP_QNP_ARESETN     BIT(7)

static const uint32_t tt_d2d_strap_sequence[] = {
	TT_D2D_STRAP_D2D_NOC_RESET_N, TT_D2D_STRAP_APB_RESETN,  TT_D2D_STRAP_SYS_RST_NI,
	TT_D2D_STRAP_LL_ARESETN,      TT_D2D_STRAP_AXI4L_ARESETN, TT_D2D_STRAP_QNP_ARESETN,
	TT_D2D_STRAP_NOC2AXI_RESET_N,
};

/*
 * Block size for the DMA zero-fill. The engine has no fill mode, so zeroing is
 * a copy from a page of zeroes whose source address never advances; this is
 * both that page's size and the transfer block size.
 */
#define TT_D2D_ZERO_BLOCK 4096U

/*
 * Re-read once per block by the DMA, so one page covers a window of any size.
 * In .bss, which the kernel has already zeroed by the time any of this runs,
 * and 4 KB aligned to match the block size the engine is given.
 */
static uint64_t tt_d2d_zero_page[TT_D2D_ZERO_BLOCK / sizeof(uint64_t)] __aligned(TT_D2D_ZERO_BLOCK);

struct tt_d2d_config {
	uintptr_t base;
	const struct device *dma_dev; /* NULL when no dmas property: CPU stores */
	uint32_t dma_channel;
	uint32_t fw_max_size;
	uint32_t bits_per_channel;
	uint32_t refclk_period_ns;
	uint32_t apb_clk_period_ps;
	uint32_t pll_freq_ghz;
	uint32_t package_type_this_die;
	uint32_t package_type_other_die;
	bool bypass_phy;
	bool bypass_vreg;
	bool disable_sideband;
};

static inline uintptr_t tt_d2d_sram(const struct tt_d2d_config *config)
{
	return config->base + TT_D2D_SRAM_OFFSET;
}

static inline void tt_d2d_cfg_write(const struct tt_d2d_config *config, uint32_t index,
				    uint32_t value)
{
	sys_write32(value, tt_d2d_sram(config) + TT_D2D_CFG_OFFSET + (index * sizeof(uint32_t)));
}

int tt_d2d_reset_release(const struct device *dev)
{
	const struct tt_d2d_config *config = dev->config;
	uintptr_t strap = config->base + TT_D2D_STRAP_RESET_OFFSET;
	uint32_t value = 0;

	/*
	 * 64-bit accesses: the strap block is only reachable that way, unlike
	 * the rest of the tile which is written 32 bits at a time.
	 */
	for (size_t i = 0; i < ARRAY_SIZE(tt_d2d_strap_sequence); i++) {
		value |= tt_d2d_strap_sequence[i];
		sys_write64(value, strap);
	}

	LOG_DBG("%s: subsystem resets released (straps 0x%02x)", dev->name, value);

	return 0;
}

/*
 * One synchronous memory-to-memory transfer.
 *
 * The Zephyr DMA block fields do not map onto this engine's registers by name,
 * so the correspondence is worth stating: source_gather_interval and
 * dest_scatter_interval are the source and destination strides, and
 * source_gather_count is the repetition count. A stride of zero means the
 * address does not advance between repetitions, which is what turns a copy
 * into a fill.
 *
 * The Grendel DMA driver polls the transfer to completion inside dma_start(),
 * so there is nothing to wait for afterwards.
 */
static int tt_d2d_dma_run(const struct device *dev, uintptr_t src, uintptr_t dst, uint32_t len,
			  uint32_t src_stride, uint32_t dst_stride, uint32_t repetitions)
{
	const struct tt_d2d_config *config = dev->config;
	struct dma_block_config block = {
		.source_address = src,
		.dest_address = dst,
		.block_size = len,
		.source_gather_interval = src_stride,
		.dest_scatter_interval = dst_stride,
		.source_gather_count = repetitions,
		.source_gather_en = (src_stride == 0) ? 1 : 0,
		.dest_scatter_en = (dst_stride != 0) ? 1 : 0,
	};
	struct dma_config dma_cfg = {
		.channel_direction = MEMORY_TO_MEMORY,
		.source_data_size = sizeof(uint32_t),
		.dest_data_size = sizeof(uint32_t),
		.block_count = 1,
		.head_block = &block,
	};
	int ret;

	ret = dma_config(config->dma_dev, config->dma_channel, &dma_cfg);
	if (ret != 0) {
		LOG_ERR("%s: dma_config failed: %d", dev->name, ret);
		return ret;
	}

	ret = dma_start(config->dma_dev, config->dma_channel);
	if (ret != 0) {
		LOG_ERR("%s: dma_start failed: %d", dev->name, ret);
		return ret;
	}

	return 0;
}

static int tt_d2d_clear_sram(const struct device *dev, uintptr_t sram)
{
	const struct tt_d2d_config *config = dev->config;

	/* The zero-fill works in whole pages, so a window that is not a
	 * multiple of one falls back rather than leaving a tail unwritten.
	 */
	if (config->dma_dev != NULL && (config->fw_max_size % TT_D2D_ZERO_BLOCK) == 0) {
		return tt_d2d_dma_run(dev, (uintptr_t)tt_d2d_zero_page, sram, TT_D2D_ZERO_BLOCK, 0,
				      TT_D2D_ZERO_BLOCK, config->fw_max_size / TT_D2D_ZERO_BLOCK);
	}

	for (uint32_t off = 0; off < config->fw_max_size; off += sizeof(uint32_t)) {
		sys_write32(0, sram + off);
	}

	return 0;
}

static int tt_d2d_copy_image(const struct device *dev, uintptr_t sram, const uint8_t *img,
			     size_t img_size)
{
	const struct tt_d2d_config *config = dev->config;

	if (config->dma_dev != NULL) {
		return tt_d2d_dma_run(dev, (uintptr_t)img, sram, img_size, 0, 0, 1);
	}

	for (size_t off = 0; off < img_size; off += sizeof(uint32_t)) {
		sys_write32(sys_get_le32(&img[off]), sram + off);
	}

	return 0;
}

static void tt_d2d_write_config(const struct device *dev)
{
	const struct tt_d2d_config *config = dev->config;

	tt_d2d_cfg_write(config, TT_D2D_CFG_BITS_PER_CHANNEL, config->bits_per_channel);
	tt_d2d_cfg_write(config, TT_D2D_CFG_REFCLK_PERIOD_NS, config->refclk_period_ns);
	tt_d2d_cfg_write(config, TT_D2D_CFG_APB_CLK_PERIOD_PS, config->apb_clk_period_ps);
	tt_d2d_cfg_write(config, TT_D2D_CFG_PLL_FREQ_GHZ, config->pll_freq_ghz);
	tt_d2d_cfg_write(config, TT_D2D_CFG_TX_WILD_CLK, TT_D2D_TX_WILD_CLK);
	tt_d2d_cfg_write(config, TT_D2D_CFG_RX_WILD_CLK, TT_D2D_RX_WILD_CLK);
	tt_d2d_cfg_write(config, TT_D2D_CFG_BYPASS_VREG, config->bypass_vreg ? 1U : 0U);
	tt_d2d_cfg_write(config, TT_D2D_CFG_PKG_TYPE_THIS, config->package_type_this_die);
	tt_d2d_cfg_write(config, TT_D2D_CFG_PKG_TYPE_OTHER, config->package_type_other_die);
	tt_d2d_cfg_write(config, TT_D2D_CFG_SLICE_COUNT, 0U); /* 0 = auto */
	tt_d2d_cfg_write(config, TT_D2D_CFG_LPBK2_ENABLE, 0U);
	tt_d2d_cfg_write(config, TT_D2D_CFG_LPBK2_PATTERN, TT_D2D_LPBK2_PATTERN_MEMORY);
	tt_d2d_cfg_write(config, TT_D2D_CFG_LPBK2_TX_SLICE, 0U);
	tt_d2d_cfg_write(config, TT_D2D_CFG_LPBK2_RX_SLICE, TT_D2D_LPBK2_RX_SLICE);
	tt_d2d_cfg_write(config, TT_D2D_CFG_LPBK2_MEM_PATTERN, TT_D2D_LPBK2_MEM_PATTERN);
	tt_d2d_cfg_write(config, TT_D2D_CFG_LPBK2_PRBS_SIZE, TT_D2D_LPBK2_PRBS_SIZE);
	tt_d2d_cfg_write(config, TT_D2D_CFG_BYPASS_PHY, config->bypass_phy ? 1U : 0U);
	tt_d2d_cfg_write(config, TT_D2D_CFG_DISABLE_SIDEBAND, config->disable_sideband ? 1U : 0U);

	/*
	 * Magic last: it is what tells the firmware the rest of the block is
	 * populated, so writing it first would expose a half-filled config.
	 */
	tt_d2d_cfg_write(config, TT_D2D_CFG_MAGIC, TT_D2D_CFG_MAGIC_VALUE);
}

int tt_d2d_load_fw(const struct device *dev, const uint8_t *img, size_t img_size)
{
	const struct tt_d2d_config *config = dev->config;
	uintptr_t sram = tt_d2d_sram(config);
	uint32_t ctrl;
	uint32_t probe;
	int ret;

	if (img == NULL || img_size == 0 || (img_size % sizeof(uint32_t)) != 0) {
		return -EINVAL;
	}

	if (img_size > TT_D2D_IMAGE_MAX) {
		LOG_ERR("%s: image of %zu bytes would overwrite the config block at 0x%x",
			dev->name, img_size, TT_D2D_CFG_OFFSET);
		return -ENOSPC;
	}

	/*
	 * Hold the Rocket in reset for the whole load; release its uncore,
	 * which is what maps its SRAM into this address space at all. Read the
	 * register back before using that mapping: the write is posted, and
	 * the reference bring-up likewise confirms the bit cleared rather than
	 * assuming it.
	 */
	sys_write32(TT_D2D_CPU_CTRL_HALT, config->base + TT_D2D_CPU_CTRL_OFFSET);
	ctrl = sys_read32(config->base + TT_D2D_CPU_CTRL_OFFSET);
	if ((ctrl & TT_D2D_CPU_CTRL_UNCORE_RESET) != 0) {
		LOG_ERR("%s: uncore still in reset (cpu_ctrl 0x%08x); SRAM is not mapped",
			dev->name, ctrl);
		return -ENODEV;
	}

	/*
	 * One word before committing to the full image. Without this a tile
	 * that is not responding at all fails identically to a corrupted load,
	 * except 64 KB later and with a message that points at the image.
	 */
	sys_write32(TT_D2D_PROBE_PATTERN, sram);
	probe = sys_read32(sram);
	if (probe != TT_D2D_PROBE_PATTERN) {
		LOG_ERR("%s: SRAM at 0x%lx not responding (wrote 0x%08x read 0x%08x); "
			"tile unreachable -- check cold reset and SMC firewalls",
			dev->name, (unsigned long)sram, TT_D2D_PROBE_PATTERN, probe);
		return -ENODEV;
	}

	/*
	 * Clear the whole window, not just the image: the firmware's .bss lives
	 * here and nothing else zeroes it, and it also drops any stale config
	 * block from a previous load.
	 */
	ret = tt_d2d_clear_sram(dev, sram);
	if (ret != 0) {
		LOG_ERR("%s: clearing SRAM failed: %d", dev->name, ret);
		return ret;
	}

	ret = tt_d2d_copy_image(dev, sram, img, img_size);
	if (ret != 0) {
		LOG_ERR("%s: copying the image failed: %d", dev->name, ret);
		return ret;
	}

	if (IS_ENABLED(CONFIG_TT_D2D_VERIFY_LOAD)) {
		for (size_t off = 0; off < img_size; off += sizeof(uint32_t)) {
			uint32_t want = sys_get_le32(&img[off]);
			uint32_t got = sys_read32(sram + off);

			if (got != want) {
				LOG_ERR("%s: image mismatch at +0x%zx: wrote 0x%08x read 0x%08x",
					dev->name, off, want, got);
				return -EIO;
			}
		}
	}

	tt_d2d_write_config(dev);

	LOG_DBG("%s: loaded %zu bytes, Rocket held in reset", dev->name, img_size);

	return 0;
}

int tt_d2d_start(const struct device *dev)
{
	const struct tt_d2d_config *config = dev->config;

	sys_write32(TT_D2D_CPU_CTRL_START, config->base + TT_D2D_CPU_CTRL_OFFSET);

	LOG_DBG("%s: Rocket released", dev->name);

	return 0;
}

static int tt_d2d_init(const struct device *dev)
{
	const struct tt_d2d_config *config = dev->config;

	/*
	 * No hardware is touched here. Reset release has to be ordered against
	 * the CCE clock switch, which this driver knows nothing about, so it is
	 * left to the caller.
	 */
	if (config->fw_max_size < TT_D2D_CFG_OFFSET + sizeof(uint32_t)) {
		LOG_ERR("%s: fw-max-size 0x%x is too small to hold the config block", dev->name,
			config->fw_max_size);
		return -EINVAL;
	}

	if (config->dma_dev != NULL && !device_is_ready(config->dma_dev)) {
		LOG_ERR("%s: DMA controller %s is not ready", dev->name, config->dma_dev->name);
		return -ENODEV;
	}

	return 0;
}

/*
 * dmas is optional, so the phandle can only be dereferenced when it is there.
 */
#define TT_D2D_DMA_DEV(inst)                                                                       \
	COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, dmas),                                             \
		    (DEVICE_DT_GET(DT_INST_DMAS_CTLR_BY_IDX(inst, 0))), (NULL))

#define TT_D2D_DMA_CHANNEL(inst)                                                                   \
	COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, dmas),                                             \
		    (DT_INST_DMAS_CELL_BY_IDX(inst, 0, channel)), (0))

#define TT_D2D_INIT(inst)                                                                          \
	static const struct tt_d2d_config tt_d2d_config_##inst = {                                 \
		.base = DT_INST_REG_ADDR(inst),                                                    \
		.dma_dev = TT_D2D_DMA_DEV(inst),                                                   \
		.dma_channel = TT_D2D_DMA_CHANNEL(inst),                                           \
		.fw_max_size = DT_INST_PROP(inst, fw_max_size),                                    \
		.bits_per_channel = DT_INST_PROP(inst, bits_per_channel),                          \
		.refclk_period_ns = DT_INST_PROP(inst, refclk_period_ns),                          \
		.apb_clk_period_ps = DT_INST_PROP(inst, apb_clk_period_ps),                        \
		.pll_freq_ghz = DT_INST_PROP(inst, pll_freq_ghz),                                  \
		.package_type_this_die = DT_INST_PROP(inst, package_type_this_die),                \
		.package_type_other_die = DT_INST_PROP(inst, package_type_other_die),              \
		.bypass_phy = DT_INST_PROP(inst, bypass_phy),                                      \
		.bypass_vreg = DT_INST_PROP(inst, bypass_vreg),                                    \
		.disable_sideband = DT_INST_PROP(inst, disable_sideband),                          \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(inst, tt_d2d_init, NULL, NULL, &tt_d2d_config_##inst, POST_KERNEL,   \
			      CONFIG_TT_D2D_INIT_PRIO, NULL);

DT_INST_FOREACH_STATUS_OKAY(TT_D2D_INIT)
