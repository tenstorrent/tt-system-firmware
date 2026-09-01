/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mis_launch.h"
#include <zephyr/drivers/misc/tt_bundle_loader.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/mbox.h>
#include <zephyr/device.h>

LOG_MODULE_REGISTER(mis_launch_m, CONFIG_LOG_DEFAULT_LEVEL);

/* TODO: Better placement so we can re-place with one change instead of multiple. */
#define MIS_ENTRY_POINT (0xC0067000)

/* Writing K's outbound channel 0 triggers Keraunos's RX on its inbound channel 1 */
#define K_MBOX_CHANNEL 0

/* M-SMC-MIS signals readiness on M's own local mbox0, inbound channel 1 */
#define MIS_MBOX_CHANNEL 1

static void mis_mbox_rx_cb(const struct device *dev, mbox_channel_id_t channel_id, void *user_data,
			   struct mbox_msg *msg)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(channel_id);
	ARG_UNUSED(msg);
	k_sem_give((struct k_sem *)user_data);
}

void launch_mis(void)
{
	const struct device *k_mbox = DEVICE_DT_GET(DT_NODELABEL(k_mbox));
	uint64_t payload = 0;
	struct mbox_msg msg = {
		.data = &payload,
		.size = sizeof(payload),
	};
	int ret;

	LOG_INF("BL1 boot complete, signalling Keraunos via k_mbox");
	if (!device_is_ready(k_mbox)) {
		LOG_ERR("k_mbox device not ready");
		return;
	}
	ret = mbox_send(k_mbox, K_MBOX_CHANNEL, &msg);
	if (ret != 0) {
		LOG_ERR("Failed to signal Keraunos over k_mbox: %d", ret);
		return;
	}
	const struct device *mbox = DEVICE_DT_GET(DT_NODELABEL(mbox0));
	struct k_sem ready_sem;

	if (!device_is_ready(mbox)) {
		LOG_ERR("mbox0 device not ready");
		return;
	}

	k_sem_init(&ready_sem, 0, 1);

	ret = mbox_register_callback(mbox, MIS_MBOX_CHANNEL, mis_mbox_rx_cb, &ready_sem);
	if (ret != 0) {
		LOG_ERR("Failed to register mbox callback: %d", ret);
		return;
	}

	ret = mbox_set_enabled(mbox, MIS_MBOX_CHANNEL, true);
	if (ret != 0) {
		LOG_ERR("Failed to enable mbox channel %d: %d", MIS_MBOX_CHANNEL, ret);
		(void)mbox_set_enabled(mbox, MIS_MBOX_CHANNEL, false);
		return;
	}

	LOG_INF("Waiting for M-SMC-MIS ready signal on mbox channel %d", MIS_MBOX_CHANNEL);
	k_sem_take(&ready_sem, K_FOREVER);
	LOG_INF("M-SMC-MIS signalled ready");

	__asm__ volatile("fence\nfence.i" ::: "memory");

	LOG_INF("Jumping to M-SMC-MIS at %p", (void *)(uintptr_t)MIS_ENTRY_POINT);

	/* Jump to M-SMC-MIS. Does not return; M-SMC-MIS owns the core from here on. */
	__asm__ volatile("csrw mepc, %0\n"
			 "li   t0, 0x1800\n" /* MPP=M-mode, MIE=0, MPIE=0 */
			 "csrw mstatus, t0\n"
			 "mret\n"
			 :
			 : "r"((uintptr_t)MIS_ENTRY_POINT)
			 : "t0");

	/* Unreachable unless M-SMC-MIS returns, which means the handoff failed and the
	 * core's state (stack, vector table, PMP, ...) can no longer be trusted.
	 */
	LOG_ERR("M-SMC-MIS unexpectedly returned");
	k_panic();
}
