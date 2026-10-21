/*
 * Copyright (c) 2025 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>

#include <tenstorrent/bh_power.h>
#ifdef CONFIG_TT_MSGQUEUE
#include <tenstorrent/msgqueue.h>
#include <tenstorrent/smc_msg.h>
#endif

#include "telemetry.h"
#include "smbus_target.h"
#include "gddr.h"
#include "asic_state.h"
#include "noc.h"
#include "noc_init.h"
#include "ecc_monitor.h"
#include "noc_ecc.h"
#include "tensix_ecc.h"

LOG_MODULE_REGISTER(tt_shell, CONFIG_LOG_DEFAULT_LEVEL);

static int parse_u32_arg(const char *arg, uint32_t *value)
{
	char *endptr;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(arg, &endptr, 0);
	if (errno != 0 || endptr == arg || *endptr != '\0' || parsed > UINT32_MAX) {
		return -EINVAL;
	}

	*value = (uint32_t)parsed;
	return 0;
}

static int l2cpu_enable_handler(const struct shell *sh, size_t argc, char **argv)
{
	bool on = false;

	if (strcmp(argv[1], "off") == 0) {
		on = false;

	} else if (strcmp(argv[1], "on") == 0) {
		on = true;
	} else {
		shell_error(sh, "Invalid L2CPU power setting");

		return -EINVAL;
	}

	int ret = bh_set_l2cpu_enable(on);

	if (ret != 0) {
		shell_error(sh, "Failure to set L2CPU power setting %u", on);
		return ret;
	}
	shell_print(sh, "OK");
	return 0;
}

static int tensix_enable_handler(const struct shell *sh, size_t argc, char **argv)
{
	bool on = false;

	if (strcmp(argv[1], "off") == 0) {
		on = false;

	} else if (strcmp(argv[1], "on") == 0) {
		on = true;
	} else {
		shell_error(sh, "Invalid tensix power setting");

		return -EINVAL;
	}

	int ret = set_tensix_enable(on);

	if (ret != 0) {
		shell_error(sh, "Failure to set tensix power setting %u", on);
		return ret;
	}
	shell_print(sh, "OK");
	return 0;
}

static int mrisc_power_handler(const struct shell *sh, size_t argc, char **argv)
{
	bool on = false;

	if (strcmp(argv[1], "off") == 0) {
		on = false;

	} else if (strcmp(argv[1], "on") == 0) {
		on = true;
	} else {
		shell_error(sh, "Invalid MRISC power setting");

		return -EINVAL;
	}

	int ret = set_mrisc_power_setting(on);

	if (ret != 0) {
		shell_error(sh, "Failure to set MRISC power setting %u", on);
		return ret;
	}
	shell_print(sh, "OK");
	return 0;
}

static int asic_state_handler(const struct shell *sh, size_t argc, char **argv)
{
	if (argc == 2U) {
		AsicState state = (AsicState)atoi(argv[1]);

		if (state == A0State || state == A3State) {
			set_asic_state(state);
			shell_print(sh, "OK");
		} else {
			shell_error(sh, "Invalid ASIC State");
			return -EINVAL;
		}
	} else {
		shell_print(sh, "ASIC State: %u", get_asic_state());
	}

	return 0;
}

static int telem_handler(const struct shell *sh, size_t argc, char **argv)
{
	int32_t idx = atoi(argv[1]);
	char fmt;
	uint32_t value;

	if (argc == 3 && (strlen(argv[2]) != 1U)) {
		shell_error(sh, "Invalid format");
		return -EINVAL;
	}
	if (argc == 2) {
		fmt = 'x';
	} else {
		fmt = argv[2][0];
	}

	if (!GetTelemetryTagValid(idx)) {
		shell_error(sh, "Invalid telemetry tag");
		return -EINVAL;
	}

	value = GetTelemetryTag(idx);

	if (fmt == 'x') {
		shell_print(sh, "0x%08X", value);
	} else if (fmt == 'f') {
		shell_print(sh, "%lf", (double)ConvertTelemetryToFloat(value));
	} else if (fmt == 'd') {
		shell_print(sh, "%d", value);
	} else {
		shell_error(sh, "Invalid format");
		return -EINVAL;
	}

	return 0;
}

#ifdef CONFIG_TT_MSGQUEUE
static int msg_handler(const struct shell *sh, size_t argc, char **argv)
{
	union request request = {0};
	struct response response = {0};
	uint32_t parsed;
	int ret;

	for (size_t i = 1; i < argc; ++i) {
		ret = parse_u32_arg(argv[i], &parsed);
		if (ret != 0) {
			shell_error(sh, "Invalid u32 value: %s", argv[i]);
			return ret;
		}

		request.data[i - 1U] = parsed;
	}

	ret = msgqueue_request_push(0, &request);
	if (ret != 0) {
		shell_error(sh, "Failed to queue request (%d)", ret);
		return ret;
	}

	process_message_queues();

	ret = msgqueue_response_pop(0, &response);
	if (ret != 0) {
		shell_error(sh, "Failed to read response (%d)", ret);
		return ret;
	}

	for (size_t i = 0; i < RESPONSE_MSG_LEN; ++i) {
		shell_print(sh, "rsp[%u] = 0x%08x", (unsigned int)i, response.data[i]);
	}

	return 0;
}

/* Exchange one counter request and leave the reply in *response. */
static int counter_msg_exchange(const struct shell *sh, uint8_t command, uint32_t bank,
				uint32_t index_or_mask, struct response *response)
{
	union request request = {0};
	int ret;

	request.counter.command_code = TT_SMC_MSG_COUNTER;
	request.counter.command = command;
	request.counter.counter_bank = bank;
	if (command == COUNTER_CMD_GET) {
		request.counter.bank_index = index_or_mask;
	} else {
		request.counter.mask = index_or_mask;
	}

	ret = msgqueue_request_push(0, &request);
	if (ret != 0) {
		shell_error(sh, "Failed to queue request (%d)", ret);
		return ret;
	}

	process_message_queues();

	ret = msgqueue_response_pop(0, response);
	if (ret != 0) {
		shell_error(sh, "Failed to read response (%d)", ret);
		return ret;
	}

	if (response->data[0] != 0) {
		shell_error(sh, "Counter request rejected (status %u)", response->data[0]);
		return -EINVAL;
	}

	return 0;
}

/* Print one counter as a plain decimal value, rather than the full response dump of `tt msg`. */
static int counter_get_handler(const struct shell *sh, size_t argc, char **argv)
{
	struct response response = {0};
	uint32_t bank, index;
	int ret;

	ARG_UNUSED(argc);

	if (parse_u32_arg(argv[1], &bank) != 0 || parse_u32_arg(argv[2], &index) != 0) {
		shell_error(sh, "Invalid u32 value");
		return -EINVAL;
	}

	ret = counter_msg_exchange(sh, COUNTER_CMD_GET, bank, index, &response);
	if (ret != 0) {
		return ret;
	}

	shell_print(sh, "%u", response.data[2]);

	return 0;
}

static int counter_clear_handler(const struct shell *sh, size_t argc, char **argv)
{
	struct response response = {0};
	uint32_t bank, mask;
	int ret;

	ARG_UNUSED(argc);

	if (parse_u32_arg(argv[1], &bank) != 0 || parse_u32_arg(argv[2], &mask) != 0) {
		shell_error(sh, "Invalid u32 value");
		return -EINVAL;
	}

	ret = counter_msg_exchange(sh, COUNTER_CMD_CLEAR, bank, mask, &response);
	if (ret != 0) {
		return ret;
	}

	return 0;
}
#endif

/*
 * Optional trailing "<noc_x> <noc_y>" pair, starting at argv[first].
 * Supplying neither picks an arbitrary enabled tile; supplying one is a typo, not a default.
 */
static int parse_noc_coord_args(const struct shell *sh, size_t argc, char **argv, size_t first,
				uint8_t *noc_x, uint8_t *noc_y)
{
	uint32_t parsed_x, parsed_y;

	if (argc == first) {
		GetEnabledTensix(noc_x, noc_y);
		return 0;
	}

	if (argc != first + 2) {
		shell_error(sh, "Provide both noc_x and noc_y, or neither");
		return -EINVAL;
	}

	if (parse_u32_arg(argv[first], &parsed_x) != 0 ||
	    parse_u32_arg(argv[first + 1], &parsed_y) != 0) {
		shell_error(sh, "Invalid NOC coordinate");
		return -EINVAL;
	}
	if (parsed_x >= NOC_X_SIZE || parsed_y >= NOC_Y_SIZE) {
		shell_error(sh, "NOC coordinate out of range (x < %u, y < %u)", NOC_X_SIZE,
			    NOC_Y_SIZE);
		return -EINVAL;
	}
	/* A NOC read to a node with no reachable NIU (SERDES, column 8) can hang ARC. */
	if (!NocEccIsKnownNode(parsed_x, parsed_y)) {
		shell_error(sh, "(%u, %u) is not a Tensix/ETH/GDDR/PCIE/ARC node", parsed_x,
			    parsed_y);
		return -EINVAL;
	}

	*noc_x = parsed_x;
	*noc_y = parsed_y;
	return 0;
}

static void ecc_print_counters(const struct shell *sh, const char *label,
			       const uint32_t counters[NOC_ECC_NUM_SOURCES])
{
	shell_print(sh, "%-7s mem_parity=%u hdr_sbe=%u hdr_dbe=%u", label,
		    counters[NOC_ECC_MEM_PARITY], counters[NOC_ECC_HDR_SBE],
		    counters[NOC_ECC_HDR_DBE]);
}

static int parse_ecc_mask_arg(const struct shell *sh, const char *arg, uint32_t *which)
{
	if (parse_u32_arg(arg, which) != 0 || *which == 0 || (*which & ~NOC_ECC_SOURCE_MASK) != 0) {
		shell_error(sh, "Mask must be 1..%u (bit0 mem_parity, bit1 hdr_sbe, bit2 hdr_dbe)",
			    (unsigned int)NOC_ECC_SOURCE_MASK);
		return -EINVAL;
	}

	return 0;
}

static int ecc_force_handler(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t before[NOC_ECC_NUM_SOURCES];
	uint32_t after[NOC_ECC_NUM_SOURCES];
	uint8_t noc_x, noc_y;
	uint32_t which = BIT(NOC_ECC_MEM_PARITY);
	size_t coord_argc = argc;
	int ret;

	/* Optional mask is last so a pair is always coordinates: [x y] [mask]. */
	if (argc == 2 || argc == 4) {
		ret = parse_ecc_mask_arg(sh, argv[argc - 1], &which);
		if (ret != 0) {
			return ret;
		}
		coord_argc = argc - 1;
	}

	ret = parse_noc_coord_args(sh, coord_argc, argv, 1, &noc_x, &noc_y);
	if (ret != 0) {
		return ret;
	}

	/*
	 * All three registers live in the NIU, so no clock gate check. The counters are the only
	 * evidence the force landed: NIU ECC_CTRL is write-only. Hold the router lock across the
	 * access so a Tensix reset cannot drop encode after this check.
	 */
	NocEccStateLock();
	if (!NocEccEnabled() || !NocTensixRoutersUp()) {
		NocEccStateUnlock();
		shell_error(sh, "NOC ECC disabled or Tensix routers down");
		return -EBUSY;
	}
	NocEccReadCounters(noc_x, noc_y, before);
	NocEccForce(noc_x, noc_y, which);
	NocEccReadCounters(noc_x, noc_y, after);
	NocEccStateUnlock();

	shell_print(sh, "tile: noc0 (%u, %u)   forced mask 0x%X", noc_x, noc_y, which);
	ecc_print_counters(sh, "before:", before);
	ecc_print_counters(sh, "after:", after);

	for (int i = 0; i < NOC_ECC_NUM_SOURCES; i++) {
		if ((which & BIT(i)) != 0 && after[i] == before[i]) {
			shell_warn(sh, "Counter %d did not move; the force did not reach the NIU",
				   i);
		}
	}

	return 0;
}

static int ecc_probe_handler(const struct shell *sh, size_t argc, char **argv)
{
	struct tensix_ecc_probe probe;
	uint8_t noc_x, noc_y;
	int ret;

	ret = parse_noc_coord_args(sh, argc, argv, 1, &noc_x, &noc_y);
	if (ret != 0) {
		return ret;
	}

	/* ECC_CTRL/ECC_STATUS exist only in Tensix; rows 0-1 and the GDDR columns have none. */
	if (noc_y < 2 || noc_x == 0 || noc_x == 9) {
		shell_error(sh, "(%u, %u) is not a Tensix tile", noc_x, noc_y);
		return -EINVAL;
	}

	NocEccStateLock();
	if (!NocEccEnabled() || !NocTensixRoutersUp()) {
		NocEccStateUnlock();
		shell_error(sh, "NOC ECC disabled or Tensix routers down");
		return -EBUSY;
	}
	TensixEccProbe(noc_x, noc_y, &probe);
	NocEccStateUnlock();

	shell_print(sh, "tile: noc0 (%u, %u)", noc_x, noc_y);
	for (int i = 0; i < 2; i++) {
		shell_print(
			sh, "noc%d NIU_CFG_0: 0x%08X  ecc_int_en=0x%X  tile_clk_off=%u", i,
			probe.niu_cfg_0[i],
			(unsigned int)FIELD_GET(NOC_NIU_CFG_0_ECC_IRQ_EN, probe.niu_cfg_0[i]),
			(unsigned int)FIELD_GET(NOC_NIU_CFG_0_TILE_CLK_OFF, probe.niu_cfg_0[i]));
	}
	ecc_print_counters(sh, "noc0:", probe.noc);
	if (probe.clock_gated) {
		shell_warn(sh, "tile clock off: ECC_CTRL reads as zero and the IRQ cannot fire");
		return 0;
	}
	shell_print(sh, "ECC_CTRL:  0x%08X  (armed = 0x%08X)%s", probe.ecc_ctrl,
		    (unsigned int)TENSIX_ECC_CTRL_IRQ_ARMED,
		    (probe.ecc_ctrl & TENSIX_ECC_CTRL_IRQ_ARMED) == TENSIX_ECC_CTRL_IRQ_ARMED
			    ? ""
			    : "  NOT ARMED");
	shell_print(sh,
		    "noc level: mem_parity=%u hdr_sbe=%u hdr_dbe=%u  (what the ECC manager sees)",
		    (unsigned int)(probe.noc_level >> NOC_ECC_MEM_PARITY) & 1U,
		    (unsigned int)(probe.noc_level >> NOC_ECC_HDR_SBE) & 1U,
		    (unsigned int)(probe.noc_level >> NOC_ECC_HDR_DBE) & 1U);

	return 0;
}

static int ecc_status_handler(const struct shell *sh, size_t argc, char **argv)
{
	struct ecc_totals totals;
	struct ecc_monitor_irq_state irq;

	EccMonitorGetTotals(&totals);
	EccMonitorGetIrqState(&irq);

	shell_print(sh, "monitor: %s", irq.running ? "running" : "off (ecc capability clear)");
	ecc_print_counters(sh, "noc:", totals.noc);
	shell_print(sh, "l1:     sbe=%u dbe=%u", totals.l1_sbe, totals.l1_dbe);
	shell_print(sh, "groups: pending=0x%02X masked=0x%02X blocked=0x%02X", irq.pending,
		    irq.masked, irq.blocked);
	for (int i = 0; i < irq.num_groups; i++) {
		shell_print(sh, "status[%d] (tiles %3d-%3d): 0x%08X", i, i * 32, i * 32 + 31,
			    irq.status[i]);
	}

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_tt_commands, SHELL_CMD_ARG(mrisc_power, NULL, "[off|on]", mrisc_power_handler, 2, 0),
	SHELL_CMD_ARG(tensix_power, NULL, "[off|on]", tensix_enable_handler, 2, 0),
	SHELL_CMD_ARG(l2cpu_power, NULL, "[off|on]", l2cpu_enable_handler, 2, 0),
	SHELL_CMD_ARG(asic_state, NULL, "[|0|3]", asic_state_handler, 1, 1),
	SHELL_CMD_ARG(telem, NULL, "<Telemetry Index> [|x|f|d]", telem_handler, 2, 1),
	SHELL_CMD_ARG(ecc_force, NULL, "[<noc_x> <noc_y>] [<mask>]", ecc_force_handler, 1, 3),
	SHELL_CMD_ARG(ecc_status, NULL, "", ecc_status_handler, 1, 0),
	SHELL_CMD_ARG(ecc_probe, NULL, "[<noc_x> <noc_y>]", ecc_probe_handler, 1, 2),
#ifdef CONFIG_TT_MSGQUEUE
	SHELL_CMD_ARG(msg, NULL, "<cmd> [data1 ... data7]", msg_handler, 2, 7),
	SHELL_CMD_ARG(counter, NULL, "<bank> <index>", counter_get_handler, 3, 0),
	SHELL_CMD_ARG(counter_clr, NULL, "<bank> <mask>", counter_clear_handler, 3, 0),
#endif
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(tt, &sub_tt_commands, "Tensorrent commands", NULL);
