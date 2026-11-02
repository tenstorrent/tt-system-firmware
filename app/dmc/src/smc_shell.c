/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Shell commands that run on the DMC and reach the SMC (ARC) over JTAG/AXI.
 * Useful for debugging when the SMC is not answering on I2C/SMBus.
 */

#include <stdlib.h>
#include <string.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/jtag.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include <tenstorrent/bh_arc.h>
#include <tenstorrent/bh_chip.h>
#include <tenstorrent/tt_smbus_regs.h>
#include <tenstorrent/uart_tt_virt.h>

#include "status_reg.h"

#if defined(CONFIG_SHELL) && BH_CHIP_COUNT > 0

#define SCRATCH_REG_COUNT     8
#define SCRATCH_RAM_REG_COUNT 64
#define RD_MAX_WORDS          256
#define ARC_PC_CORE_0_ADDR    0x80030C00
#define VUART_MAGIC           0x775E21A1
#define VUART_MAX_INST        4
#define VUART_MAX_TX_CAP      16384
#define VUART_HDR_WORDS       (sizeof(struct tt_vuart) / sizeof(uint32_t))
#define VUART_LINE_MAX        160

static const struct {
	uint8_t idx;
	const char *name;
} scratch_ram_names[] = {
	{0, "FW_VERSION"},
	{2, "BOOT_STATUS0"},
	{3, "BOOT_STATUS1"},
	{4, "ERROR_STATUS0"},
	{5, "ERROR_STATUS1"},
	{6, "INTERFACE_TABLE_BASE"},
	{8, "MSG_Q_STATUS"},
	{9, "MSG_Q_ERR_FLAGS"},
	{10, "SPI_BUFFER_INFO"},
	{12, "TELEMETRY_DATA"},
	{13, "TELEMETRY_TABLE"},
	{14, "PCIE_INIT_CPL_TIME"},
	{15, "CMFW_START_TIME"},
	{16, "ARC_START_TIME"},
	{17, "PERST_TO_DMFW_INIT_DONE"},
	{18, "PING_DMFW_DURATION"},
	{19, "I2C0_TARGET_DEBUG_STATE"},
};

static const char *scratch_ram_name(unsigned int idx)
{
	for (size_t i = 0; i < ARRAY_SIZE(scratch_ram_names); i++) {
		if (scratch_ram_names[i].idx == idx) {
			return scratch_ram_names[i].name;
		}
	}
	return "";
}

/* Reads `count` consecutive 32-bit words starting at `addr` in a single JTAG session. */
static int smc_read_words(const struct shell *sh, uint32_t addr, uint32_t *out, size_t count)
{
	const struct device *jtag = BH_CHIPS[BH_CHIP_PRIMARY_INDEX].config.jtag;
	int ret = jtag_setup(jtag);

	if (ret != 0) {
		shell_error(sh, "jtag_setup failed: %d", ret);
		return ret;
	}

	jtag_reset(jtag);
	for (size_t i = 0; i < count; i++) {
		ret = jtag_axi_read32(jtag, addr + 4 * i, &out[i]);
		if (ret != 0) {
			shell_error(sh, "AXI read of 0x%08zx failed: %d", (size_t)addr + 4 * i,
				    ret);
			break;
		}
	}
	jtag_teardown(jtag);

	return ret;
}

static int cmd_smc_scratch(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t regs[SCRATCH_REG_COUNT];
	uint32_t ram[SCRATCH_RAM_REG_COUNT];
	bool all = argc > 1 && strcmp(argv[1], "all") == 0;
	int ret;

	ret = smc_read_words(sh, RESET_UNIT_SCRATCH_BASE_ADDR, regs, ARRAY_SIZE(regs));
	if (ret != 0) {
		return ret;
	}

	shell_print(sh, "SCRATCH[0-7] @ 0x%08x", RESET_UNIT_SCRATCH_BASE_ADDR);
	for (size_t i = 0; i < ARRAY_SIZE(regs); i++) {
		shell_print(sh, "  [%zu] 0x%08x%s", i, regs[i],
			    i == 0   ? "  (POST_CODE)"
			    : i == 1 ? "  (CABLE_POWER_LIMIT)"
				     : "");
	}

	ret = smc_read_words(sh, RESET_UNIT_SCRATCH_RAM_BASE_ADDR, ram, ARRAY_SIZE(ram));
	if (ret != 0) {
		return ret;
	}

	shell_print(sh, "SCRATCH_RAM[0-63] @ 0x%08x", RESET_UNIT_SCRATCH_RAM_BASE_ADDR);
	for (size_t i = 0; i < ARRAY_SIZE(ram); i++) {
		const char *name = scratch_ram_name(i);

		if (all || ram[i] != 0 || name[0] != '\0') {
			shell_print(sh, "  [%2zu] 0x%08x  %s", i, ram[i], name);
		}
	}

	uint32_t pc;

	if (smc_read_words(sh, ARC_PC_CORE_0_ADDR, &pc, 1) == 0) {
		shell_print(sh, "ARC PC core0 @ 0x%08x: 0x%08x", ARC_PC_CORE_0_ADDR, pc);
	}

	return 0;
}

static int cmd_smc_rd(const struct shell *sh, size_t argc, char **argv)
{
	static uint32_t buf[RD_MAX_WORDS];
	uint32_t addr = strtoul(argv[1], NULL, 0);
	size_t count = argc > 2 ? strtoul(argv[2], NULL, 0) : 1;

	if (count == 0 || count > RD_MAX_WORDS || (addr & 3) != 0) {
		shell_error(sh, "addr must be 4-byte aligned, count 1..%d", RD_MAX_WORDS);
		return -EINVAL;
	}

	int ret = smc_read_words(sh, addr, buf, count);

	if (ret != 0) {
		return ret;
	}

	for (size_t i = 0; i < count; i++) {
		shell_print(sh, "0x%08zx: 0x%08x", (size_t)addr + 4 * i, buf[i]);
	}

	return 0;
}

/*
 * Dumps the SMC's virtual UART transmit ring (its console output) over JTAG. The descriptor
 * address is published by the SMC in STATUS_FW_VUART_REG_ADDR(inst). By default the whole ring
 * history is printed (up to tx_cap bytes ending at tx_tail), including text a host may already
 * have consumed; "new" limits it to the unread bytes between tx_head and tx_tail.
 */
static int cmd_smc_vuart(const struct shell *sh, size_t argc, char **argv)
{
	static uint32_t ring[VUART_MAX_TX_CAP / sizeof(uint32_t)];
	uint32_t hdr[VUART_HDR_WORDS];
	unsigned long inst = 0;
	bool unread_only = false;
	uint32_t desc;
	int ret;

	for (size_t i = 1; i < argc; i++) {
		if (strcmp(argv[i], "new") == 0) {
			unread_only = true;
		} else {
			char *end;

			inst = strtoul(argv[i], &end, 0);
			if (*end != '\0' || inst >= VUART_MAX_INST) {
				shell_error(sh, "inst must be 0..%d", VUART_MAX_INST - 1);
				return -EINVAL;
			}
		}
	}

	ret = smc_read_words(sh, STATUS_FW_VUART_REG_ADDR(inst), &desc, 1);
	if (ret != 0) {
		return ret;
	}
	if (desc == 0 || (desc & 3) != 0) {
		shell_error(sh, "vuart%lu: no descriptor published (SCRATCH_RAM[%lu] = 0x%08x)",
			    inst, 40 + inst, desc);
		return -ENODEV;
	}

	ret = smc_read_words(sh, desc, hdr, ARRAY_SIZE(hdr));
	if (ret != 0) {
		return ret;
	}

	const struct tt_vuart *v = (const struct tt_vuart *)hdr;

	shell_print(sh,
		    "vuart%lu @ 0x%08x: magic=0x%08x ver=0x%08x tx_cap=%u head=%u tail=%u "
		    "oflow=%u rx_cap=%u",
		    inst, desc, v->magic, v->version, v->tx_cap, v->tx_head, v->tx_tail,
		    v->tx_oflow, v->rx_cap);

	if (v->magic != VUART_MAGIC) {
		shell_error(sh, "bad magic (expected 0x%08x)", VUART_MAGIC);
		return -EIO;
	}
	if (v->tx_cap == 0 || v->tx_cap > VUART_MAX_TX_CAP) {
		shell_error(sh, "tx_cap %u unsupported (max %d)", v->tx_cap, VUART_MAX_TX_CAP);
		return -EIO;
	}

	uint32_t avail = unread_only ? tt_vuart_buf_size(v->tx_head, v->tx_tail)
				     : MIN(v->tx_tail, v->tx_cap);

	avail = MIN(avail, v->tx_cap);
	if (avail == 0) {
		shell_print(sh, "(empty)");
		return 0;
	}

	ret = smc_read_words(sh, desc + sizeof(struct tt_vuart), ring,
			     DIV_ROUND_UP(v->tx_cap, sizeof(uint32_t)));
	if (ret != 0) {
		return ret;
	}

	const uint8_t *bytes = (const uint8_t *)ring;
	char line[VUART_LINE_MAX + 1];
	size_t len = 0;

	shell_print(sh, "----- %u bytes -----", avail);
	for (uint32_t pos = v->tx_tail - avail; pos != v->tx_tail; pos++) {
		char c = bytes[pos % v->tx_cap];

		if (c == '\r') {
			continue;
		}
		if (c == '\n' || len == VUART_LINE_MAX) {
			line[len] = '\0';
			shell_print(sh, "%s", line);
			len = 0;
			if (c == '\n') {
				continue;
			}
		}
		line[len++] = (c == '\t' || (c >= 0x20 && c < 0x7f)) ? c : '.';
	}
	if (len > 0) {
		line[len] = '\0';
		shell_print(sh, "%s", line);
	}
	shell_print(sh, "-----");

	return 0;
}

static int cmd_smc_pgood(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	for (size_t i = 0; i < BH_CHIP_COUNT; i++) {
		const struct bh_chip *chip = &BH_CHIPS[i];

		shell_print(sh,
			    "chip%zu: pgood=%d severe_fault=%d fall_pending=%d rise_pending=%d "
			    "last_trip_ms=%lld uptime_ms=%lld",
			    i, gpio_pin_get_dt(&chip->config.pgood), chip->data.pgood_severe_fault,
			    chip->data.pgood_fall_triggered, chip->data.pgood_rise_triggered,
			    chip->data.pgood_last_trip_ms, k_uptime_get());
	}

	return 0;
}

/* Reads CMFW_SMBUS_PING_V2 over SMBus; a live SMC answers 0xA5A5. */
static int cmd_smc_ping(const struct shell *sh, size_t argc, char **argv)
{
	size_t first = 0;
	size_t last = BH_CHIP_COUNT - 1;
	int result = 0;

	if (argc > 1) {
		char *end;
		unsigned long idx = strtoul(argv[1], &end, 0);

		if (*end != '\0' || idx >= BH_CHIP_COUNT) {
			shell_error(sh, "chip must be 0..%d", BH_CHIP_COUNT - 1);
			return -EINVAL;
		}
		first = idx;
		last = idx;
	}

	for (size_t i = first; i <= last; i++) {
		uint16_t data = 0;
		int64_t start = k_uptime_get();
		int ret = bharc_smbus_word_data_read(&BH_CHIPS[i].config.arc, CMFW_SMBUS_PING_V2,
						     &data);
		int64_t elapsed = k_uptime_delta(&start);

		if (ret != 0) {
			shell_error(sh, "chip%zu: no response (%d) after %lld ms", i, ret, elapsed);
			result = ret;
		} else if (data != 0xA5A5) {
			shell_error(sh, "chip%zu: unexpected response 0x%04x (expected 0xa5a5)", i,
				    data);
			result = -EIO;
		} else {
			shell_print(sh, "chip%zu: SMC alive (0x%04x) in %lld ms", i, data, elapsed);
		}
	}

	return result;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_smc,
	SHELL_CMD(pgood, NULL, "Show DMC pgood state and severe-fault latch", cmd_smc_pgood),
	SHELL_CMD_ARG(ping, NULL,
		      "Ping the SMC over SMBus (CMFW_SMBUS_PING_V2)\n"
		      "usage: smc ping [chip]   (default: all chips)",
		      cmd_smc_ping, 1, 1),
	SHELL_CMD_ARG(scratch, NULL,
		      "Read SMC scratch registers over JTAG\n"
		      "usage: smc scratch [all]   (all also shows zero SCRATCH_RAM entries)",
		      cmd_smc_scratch, 1, 1),
	SHELL_CMD_ARG(rd, NULL,
		      "Read SMC AXI address over JTAG\n"
		      "usage: smc rd <addr> [count]",
		      cmd_smc_rd, 2, 1),
	SHELL_CMD_ARG(vuart, NULL,
		      "Dump SMC virtual UART (console) output over JTAG\n"
		      "usage: smc vuart [inst] [new]   (inst 0 = console, 2 = DMC; "
		      "new = unread bytes only)",
		      cmd_smc_vuart, 1, 2),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(smc, &sub_smc, "SMC access over JTAG", NULL);

#endif /* CONFIG_SHELL && BH_CHIP_COUNT > 0 */
