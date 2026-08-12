/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Declare a shell with a USB CDC ACM backend. Works alongside any other shell
 * instances active. Logging stays on the serial shell.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_uart.h>

/* The usb-shell alias names the CDC ACM instance the shell runs on. */
#define USB_SHELL_NODE DT_ALIAS(usb_shell)

#if DT_NODE_HAS_STATUS_OKAY(USB_SHELL_NODE)

SHELL_UART_DEFINE(shell_transport_usb);
SHELL_DEFINE(shell_usb, CONFIG_SHELL_PROMPT_UART, &shell_transport_usb,
	     CONFIG_SHELL_BACKEND_SERIAL_LOG_MESSAGE_QUEUE_SIZE,
	     CONFIG_SHELL_BACKEND_SERIAL_LOG_MESSAGE_QUEUE_TIMEOUT, SHELL_FLAG_OLF_CRLF);

static int enable_shell_usb(void)
{
	const struct device *const dev = DEVICE_DT_GET(USB_SHELL_NODE);
	static const struct shell_backend_config_flags cfg_flags =
		SHELL_DEFAULT_BACKEND_CONFIG_FLAGS;

	if (!device_is_ready(dev)) {
		return -ENODEV;
	}

	/* log_backend = false: the serial shell is already the log backend. */
	shell_init(&shell_usb, dev, cfg_flags, false, 0);

	return 0;
}

/*
 * Same priority the built-in backend uses. The CDC ACM UART device itself is
 * created at PRE_KERNEL_1 (CONFIG_SERIAL_INIT_PRIORITY), so it is ready well
 * before this runs; writes made before the host enumerates are simply dropped.
 */
SYS_INIT(enable_shell_usb, POST_KERNEL, CONFIG_SHELL_BACKEND_SERIAL_INIT_PRIORITY);

#endif /* DT_NODE_HAS_STATUS_OKAY(USB_SHELL_NODE) */
