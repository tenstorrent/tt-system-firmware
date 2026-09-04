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

#include <d2d_api_driver.h>
#include <d2d_api_general_definitions.h>
#include <platform.h>

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
 * The SiVal register header names one absolute address per tile instance, but
 * a tile's base comes from devicetree here, so the instance-0 addresses are
 * turned back into offsets. Both tiles must therefore share a layout, which is
 * what the build assert below states.
 *
 * The SRAM here belongs to the D2D tile, not to this SMC: it is the private
 * memory of the tile's own Rocket core, exposed to us as a window in the
 * tile's register map. The D2D firmware is what runs out of it. Nothing in
 * this file touches SMC memory, and the image loaded here is separate from
 * whatever the SMC is itself running.
 */
#define TT_D2D_OFFSET(addr) ((addr) - D2D_0_REG_MAP_BASE_ADDR)

#define TT_D2D_STRAP_RESET_OFFSET TT_D2D_OFFSET(D2D_0_STRAP_RESET_REG_ADDR)
#define TT_D2D_CPU_CTRL_OFFSET    TT_D2D_OFFSET(D2D_0_D2D_D2D_SS_ASYNC_CPU_CTRL_REG_ADDR)
#define TT_D2D_SRAM_OFFSET        TT_D2D_OFFSET(D2D_0_D2D_D2D_ROCKET_REG_MAP_BASE_ADDR)

BUILD_ASSERT(D2D_1_STRAP_RESET_REG_ADDR - D2D_1_REG_MAP_BASE_ADDR == TT_D2D_STRAP_RESET_OFFSET &&
		     D2D_1_D2D_D2D_SS_ASYNC_CPU_CTRL_REG_ADDR - D2D_1_REG_MAP_BASE_ADDR ==
			     TT_D2D_CPU_CTRL_OFFSET &&
		     D2D_1_D2D_D2D_ROCKET_REG_MAP_BASE_ADDR - D2D_1_REG_MAP_BASE_ADDR ==
			     TT_D2D_SRAM_OFFSET,
	     "every D2D tile must share one register layout for base-relative offsets to hold");

/*
 * The firmware's side of the same window, from the drop's D2D API headers: the
 * addresses there are tile-relative and start at the SRAM, so the two views
 * have to agree on where that is before any of them can be used as offsets.
 */
BUILD_ASSERT(D2D_MEMORY_BASE == TT_D2D_SRAM_OFFSET,
	     "the D2D API memory map and the register map disagree on the SRAM window");

/*
 * Configuration space: an area near the top of that SRAM which the firmware
 * reads once at startup. HOST_CONFIGURATION_AVAILABLE_REG holds the magic that
 * says a host filled it in, and the parameters indexed by tCONFIG_PARM follow
 * from HOST_CONFIGURATION_START. Both are tile-relative in the header, so they
 * are rebased onto the SRAM window here.
 */
#define TT_D2D_CFG_OFFSET ((uint32_t)(HOST_CONFIGURATION_AVAILABLE_REG - D2D_MEMORY_BASE))
#define TT_D2D_CFG_PARM_OFFSET(parm)                                                               \
	((uint32_t)((HOST_CONFIGURATION_START - D2D_MEMORY_BASE) + ((parm) * sizeof(uint32_t))))

/*
 * The firmware reads the whole parameter table, up to and including
 * RECOVERY_MODE, so the window has to be big enough to hold all of it. This
 * driver only writes the parameters it has something to say about; the rest
 * are left at the zero tt_d2d_clear_sram() put there, which is only true if
 * they are inside the window in the first place.
 */
#define TT_D2D_CFG_END TT_D2D_CFG_PARM_OFFSET(RECOVERY_MODE + 1)

/* Anything at or past the configuration block would be overwritten by it. */
#define TT_D2D_IMAGE_MAX TT_D2D_CFG_OFFSET

/*
 * Loopback-2 is a bring-up aid; these keep it off but consistently described.
 * The pattern type and PRBS size come from the drop's own enumerations.
 */
#define TT_D2D_LPBK2_RX_SLICE    4U
#define TT_D2D_LPBK2_MEM_PATTERN 0xABCDU

/* 0 = fast, 1 = slow. */
#define TT_D2D_TX_WILD_CLK 0U
#define TT_D2D_RX_WILD_CLK 1U

/*
 * Reset default asserts all three of the CPU control resets
 * (D2D_SS_ASYNC_CPU_CTRL_REG_DEFAULT).
 *
 * Releasing uncore is what makes the Rocket's SRAM reachable from here, so it
 * has to come out before a load while the core stays in reset. debug_reset is
 * left asserted throughout: the reference bring-up keeps it that way, and
 * clearing it is not needed to run firmware.
 *
 * The register is written whole rather than read-modify-written. Preserving
 * unknown bits out of a read is what breaks the Rocket, so every field is
 * always stated explicitly.
 */
static inline uint32_t tt_d2d_cpu_ctrl(bool run_core)
{
	D2D_SS_ASYNC_CPU_CTRL_reg_u ctrl = {.f = {
						    .core_reset = run_core ? 0U : 1U,
						    .uncore_reset = 0U,
						    .debug_reset = 1U,
					    }};

	return ctrl.val;
}

/*
 * Written to the first SRAM word and read back before a load, to tell an
 * unreachable tile apart from a genuine load failure. Any value works; this
 * one is just recognisable in a bus trace.
 */
#define TT_D2D_PROBE_PATTERN 0xD2D0F00DU

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
	uint32_t sram_size;
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

static inline void tt_d2d_cfg_write(const struct tt_d2d_config *config, tCONFIG_PARM parm,
				    uint32_t value)
{
	sys_write32(value, tt_d2d_sram(config) + TT_D2D_CFG_PARM_OFFSET(parm));
}

int tt_d2d_reset_release(const struct device *dev)
{
	const struct tt_d2d_config *config = dev->config;
	uintptr_t strap = config->base + TT_D2D_STRAP_RESET_OFFSET;
	TT_MIMIR_D2D_STRAP_RESET_reg_u reset = {.val = 0};

	/*
	 * One reset per write, in the order the hardware requires, so each is
	 * added to the ones already deasserted rather than replacing them.
	 *
	 * 64-bit accesses: the strap block is only reachable that way, unlike
	 * the rest of the tile which is written 32 bits at a time.
	 */
	reset.f.d2d_noc_reset_n = 1;
	sys_write64(reset.val, strap);

	reset.f.d2d_i_apb_resetn = 1;
	sys_write64(reset.val, strap);

	reset.f.d2d_sys_rst_ni = 1;
	sys_write64(reset.val, strap);

	reset.f.d2d_ll_aresetn = 1;
	sys_write64(reset.val, strap);

	reset.f.d2d_i_axi4l_aresetn = 1;
	sys_write64(reset.val, strap);

	reset.f.d2d_qnp_aresetn = 1;
	sys_write64(reset.val, strap);

	reset.f.noc2axi_reset_n = 1;
	sys_write64(reset.val, strap);

	LOG_DBG("%s: subsystem resets released (straps 0x%02x)", dev->name, reset.val);

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
	if (config->dma_dev != NULL && (config->sram_size % TT_D2D_ZERO_BLOCK) == 0) {
		return tt_d2d_dma_run(dev, (uintptr_t)tt_d2d_zero_page, sram, TT_D2D_ZERO_BLOCK, 0,
				      TT_D2D_ZERO_BLOCK, config->sram_size / TT_D2D_ZERO_BLOCK);
	}

	for (uint32_t off = 0; off < config->sram_size; off += sizeof(uint32_t)) {
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

	tt_d2d_cfg_write(config, BITS_PER_CHANNEL, config->bits_per_channel);
	tt_d2d_cfg_write(config, REFERENCE_CLOCK_PERIOD_NS, config->refclk_period_ns);
	tt_d2d_cfg_write(config, APB_CLOCK_PERIOD_IN_PS, config->apb_clk_period_ps);
	tt_d2d_cfg_write(config, PLL_FREQUENCY_IN_GHZ, config->pll_freq_ghz);
	tt_d2d_cfg_write(config, TX_WILD_CLOCK_0_FAST_1_SLOW, TT_D2D_TX_WILD_CLK);
	tt_d2d_cfg_write(config, RX_WILD_CLOCK_0_FAST_1_SLOW, TT_D2D_RX_WILD_CLK);
	tt_d2d_cfg_write(config, BYPASS_VREG_0_FALSE_1_TRUE, config->bypass_vreg ? 1U : 0U);
	tt_d2d_cfg_write(config, PACKAGE_TYPE_THIS_DIE, config->package_type_this_die);
	tt_d2d_cfg_write(config, PACKAGE_TYPE_OTHER_DIE, config->package_type_other_die);
	tt_d2d_cfg_write(config, SLICE_COUNT_4_USED, 0U); /* 0 = auto */
	tt_d2d_cfg_write(config, LPBK_2_ENABLE, 0U);
	tt_d2d_cfg_write(config, LPBK_2_PATTERN_TYPE, MEM_PATTERN);
	tt_d2d_cfg_write(config, LPBK_2_TX_SLICE, 0U);
	tt_d2d_cfg_write(config, LPBK_2_RX_SLICE, TT_D2D_LPBK2_RX_SLICE);
	tt_d2d_cfg_write(config, LPBK_2_MEM_PATTERN_INFO, TT_D2D_LPBK2_MEM_PATTERN);
	tt_d2d_cfg_write(config, LPBK_2_PRBS_PATTERN_SIZE, PRBS_31_BIT);
	tt_d2d_cfg_write(config, PHY_BYPASS, config->bypass_phy ? 1U : 0U);
	tt_d2d_cfg_write(config, DISABLE_SIDEBAND, config->disable_sideband ? 1U : 0U);

	/*
	 * Magic last, and in its own register rather than the parameter table:
	 * it is what tells the firmware the rest of the block is populated, so
	 * writing it first would expose a half-filled config.
	 */
	sys_write32(CFG_AVAILABLE_SPECIAL_CODE, tt_d2d_sram(config) + TT_D2D_CFG_OFFSET);
}

int tt_d2d_load_fw(const struct device *dev, const uint8_t *img, size_t img_size)
{
	const struct tt_d2d_config *config = dev->config;
	uintptr_t sram = tt_d2d_sram(config);
	D2D_SS_ASYNC_CPU_CTRL_reg_u ctrl;
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
	sys_write32(tt_d2d_cpu_ctrl(false), config->base + TT_D2D_CPU_CTRL_OFFSET);
	ctrl.val = sys_read32(config->base + TT_D2D_CPU_CTRL_OFFSET);
	if (ctrl.f.uncore_reset != 0) {
		LOG_ERR("%s: uncore still in reset (cpu_ctrl 0x%08x); SRAM is not mapped",
			dev->name, ctrl.val);
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

	sys_write32(tt_d2d_cpu_ctrl(true), config->base + TT_D2D_CPU_CTRL_OFFSET);

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
	if (config->sram_size < TT_D2D_CFG_END) {
		LOG_ERR("%s: sram-size 0x%x is too small to hold the config block, which ends at "
			"0x%x",
			dev->name, config->sram_size, TT_D2D_CFG_END);
		return -EINVAL;
	}

	if ((config->sram_size % sizeof(uint32_t)) != 0U) {
		LOG_ERR("%s: sram-size 0x%x is not a multiple of 4 bytes", dev->name,
			config->sram_size);
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
		.sram_size = DT_INST_PROP(inst, sram_size),                                        \
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
