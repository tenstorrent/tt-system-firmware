/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * A second shell instance, on the USB CDC ACM port.
 *
 * Zephyr's serial shell backend is a single instance bound to the
 * ``zephyr,shell-uart`` chosen node (subsys/shell/backends/shell_uart.c binds
 * one SHELL_DEFINE to DT_CHOSEN(zephyr_shell_uart) and nothing else). Pointing
 * that chosen node at the CDC ACM -- what the upstream ``cdc-acm-console``
 * snippet does -- would therefore have *moved* the shell to USB and taken it off
 * uart4, breaking every host tool that drives this DMC over the FTDI.
 *
 * So instead of moving it, this declares a second transport and shell against
 * the same public SHELL_UART_DEFINE API. Both run at once: uart4 keeps its
 * shell, and the USB-C port gets an equivalent one.
 *
 * The prompt is deliberately CONFIG_SHELL_PROMPT_UART, the same string the uart4
 * shell uses: host tools find the end of a reply by matching the prompt, so the
 * two transports have to be indistinguishable from the outside.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_uart.h>

#define USB_SHELL_NODE DT_NODELABEL(cdc_acm_uart0)

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

	/*
	 * log_backend = false: the uart4 shell is already the log backend, and a
	 * second one would duplicate every message. Logs stay on the FTDI, which
	 * also keeps them somewhere readable while USB itself is being brought up.
	 */
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
