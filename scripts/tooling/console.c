/*
 * Copyright (c) 2025 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __ZEPHYR__
#include "attrs.x"
#endif

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <libgen.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "logging.h"
#include "rescan.h"
#include "vuart.h"

#ifndef UART_CHANNEL
#define UART_CHANNEL 0
#endif

#define BH_SCRAPPY_PCI_DEVICE_ID 0xb140

#define MSEC_PER_SEC  1000UL
#define USEC_PER_MSEC 1000UL
#define USEC_PER_SEC  1000000UL
#define NSEC_PER_MSEC 1000000UL

#define VUART_NOT_READY_SLEEP_US (1 * USEC_PER_SEC)

/* ASCII Start of Heading (SOH) byte (or Ctrl-A) */
#define SOH       0x01
#define CTRL_A    SOH
#define TT_DEVICE "/dev/tenstorrent/0"

struct console {
	bool stop;
	bool skip_rescan;
	timer_t timer;
	unsigned long timeout_rel_ms;

	/* backup of original termios settings */
	struct termios term;
	struct vuart_data vuart;
};

static struct console _cons;
int verbose;

static void console_init(struct console *cons)
{
	*cons = (struct console){
		.vuart = VUART_DATA_INIT(TT_DEVICE, UART_TT_VIRT_DISCOVERY_ADDR, UART_TT_VIRT_MAGIC,
					 BH_SCRAPPY_PCI_DEVICE_ID, UART_CHANNEL),
	};
}

static int termio_raw(struct console *cons)
{
	if (!isatty(STDIN_FILENO)) {
		D(2, "Not an interactive console")
		return 0;
	}

	const uint8_t buf[sizeof(struct termios)] = {0};

	if (memcmp(buf, &cons->term, sizeof(buf)) == 0) {
		if (tcgetattr(STDIN_FILENO, &cons->term) < 0) {
			E("tcgetattr: %s", strerror(errno));
			return -errno;
		}
	}

	struct termios raw = cons->term;

	raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
	raw.c_iflag &= ~(BRKINT | INPCK | ISTRIP | IXON | ICRNL);
	raw.c_oflag &= ~(OPOST);
	raw.c_cflag |= (CS8);

	if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) < 0) {
		E("tcsetattr: %s", strerror(errno));
		return -errno;
	}

	return 0;
}

static void termio_cooked(struct console *cons)
{
	const uint8_t buf[sizeof(struct termios)] = {0};

	if (memcmp(buf, &cons->term, sizeof(buf)) == 0) {
		/* not an interactive console */
		return;
	}

	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &cons->term) < 0) {
		E("tcgetattr: %s", strerror(errno));
		return;
	}

	cons->term = (struct termios){0};
}

static int loop(struct console *const cons)
{
	int ret;
	bool ctrl_a_pressed = false;
	static bool press_ctrl_a_printed;

	D(1, "loop begin: dev=%s discovery=0x%08x channel=%u magic=0x%08x skip_rescan=%d",
	  cons->vuart.dev_name, cons->vuart.addr, cons->vuart.channel, cons->vuart.magic,
	  cons->skip_rescan);

	ret = vuart_open(&cons->vuart);
	if (ret < 0) {
		D(1, "vuart_open failed: %d", ret);
		goto out;
	}
	D(1, "vuart_open success: tlb_id=%u tlb=%p", cons->vuart.tlb_id, cons->vuart.tlb);

	if (!press_ctrl_a_printed) {
		I("Press Ctrl-a,x to quit");
		press_ctrl_a_printed = true;
	}

	while (!cons->stop) {
		ret = vuart_start(&cons->vuart);
		if (ret < 0) {
			D(2, "Lost vuart connection..");
			D(1, "vuart_start failed: %d", ret);
			goto out;
		}

		if (termio_raw(cons) < 0) {
			E("Failed to set terminal to raw mode");
			break;
		}

		int ch;

		/* dump anything available from the console before sending anything */
		while ((ch = vuart_getc(&cons->vuart)) != EOF) {
			(void)putchar(ch);
			/* Flush to STDOUT */
			(void)fflush(stdout);
		}

		fd_set fds;
		struct timeval tv = {
			.tv_usec = 1,
		};

		FD_ZERO(&fds);
		FD_SET(STDIN_FILENO, &fds);

		ret = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
		if (ret < 0) {
			if (errno == EINTR) {
				/* Interrupted by a signal- no need to log error */
				D(2, "select interrupted by signal");
				ret = 0; /* Don't exit with error */
			} else {
				E("select: %s", strerror(errno));
			}
			break;
		}
		if (ret == 0) {
			continue;
		}

		ch = getchar();
		if (ctrl_a_pressed) {
			if (ch == 'x') {
				D(2, "Received Ctrl-a,x");
				cons->stop = true;
				break;
			}
			/* assumes we only ever need to capture Ctrl-a,x */
			ctrl_a_pressed = false;
		} else {
			if (ch == CTRL_A) {
				ctrl_a_pressed = true;
				D(2, "Received Ctrl-a");
			} else {
				if (vuart_space(&cons->vuart) > 0) {
					vuart_putc(&cons->vuart, ch);
				} else {
					D_RL(1, 250, "host->fw backpressure: rx ring full, deferring char 0x%02x", ch);
					ungetc(ch, stdin);
				}
			}
		}
	}

out:
	termio_cooked(cons);
	vuart_close(&cons->vuart);

	return ret;
}

static void usage(const char *progname)
{
	I("Firmware console application for use with Tenstorrent PCIe cards\n"
	  "Copyright (c) 2025 Tenstorrent AI ULC\n"
	  "\n"
	  "\n"
	  "%s: %s [args..]\n"
	  "\n"
	  "args:\n"
	  "-a <addr>          : vuart discovery address (default: %08x)\n"
	  "-c <channel>       : channel number (default: %d)\n"
	  "-d <path>          : path to device node (default: %s)\n"
	  "-h                 : print this help message\n"
	  "-i <pci_device_id> : pci device id (default: %04x)\n"
	  "-m <magic>         : vuart magic (default: %08x)\n"
	  "-p                 : skip PCIe rescan if device not found (passive mode)\n"
	  "-q                 : decrease debug verbosity\n"
	  "-v                 : increase debug verbosity\n"
	  "-w <timeout>       : wait timeout ms and exit\n",
	  __func__, progname, UART_TT_VIRT_DISCOVERY_ADDR, UART_CHANNEL, TT_DEVICE,
	  BH_SCRAPPY_PCI_DEVICE_ID, UART_TT_VIRT_MAGIC);
}

static int parse_args(struct console *cons, int argc, char **argv)
{
	int c;

	while ((c = getopt(argc, argv, ":a:c:d:hi:m:pqt:vw:")) != -1) {
		switch (c) {
		case 'a': {
			unsigned long addr;

			errno = 0;
			addr = strtol(optarg, NULL, 0);
			if (errno != 0) {
				E("invalid operand to -i %s: %s", optarg, strerror(errno));
				usage(basename(argv[0]));
				return -errno;
			}
			cons->vuart.addr = addr;
		} break;
		case 'c': {
			unsigned long channel;

			errno = 0;
			channel = strtol(optarg, NULL, 0);
			/* Limit to 16 channels so we don't use too many scratch registers */
			if (channel > 15) {
				E("Only channels 0-15 are supported, not %s", optarg);
				usage(basename(argv[0]));
				return -EINVAL;
			} else if (errno != 0) {
				E("invalid operand to -c %s: %s", optarg, strerror(errno));
				usage(basename(argv[0]));
				return -errno;
			}
			cons->vuart.channel = channel;
		} break;
		case 'd':
			cons->vuart.dev_name = optarg;
			break;
		case 'h':
			usage(basename(argv[0]));
			exit(EXIT_SUCCESS);
		case 'i': {
			long pci_device_id;

			errno = 0;
			pci_device_id = strtol(optarg, NULL, 0);
			if ((pci_device_id < 0) || (pci_device_id > UINT16_MAX)) {
				errno = ERANGE;
			}
			if (errno != 0) {
				E("invalid operand to -i %s: %s", optarg, strerror(errno));
				usage(basename(argv[0]));
				return -errno;
			}
			cons->vuart.pci_device_id = (uint16_t)pci_device_id;
		} break;
		case 'm': {
			unsigned long magic;

			errno = 0;
			magic = strtol(optarg, NULL, 0);
			magic &= 0xffffffff;
			if (errno != 0) {
				E("invalid operand to -i %s: %s", optarg, strerror(errno));
				usage(basename(argv[0]));
				return -errno;
			}
			cons->vuart.magic = magic;
		} break;
		case 'p':
			cons->skip_rescan = true;
			break;
		case 'q':
			--verbose;
			break;
		case 'v':
			++verbose;
			break;
		case 'w': {
			long timeout;

			errno = 0;
			timeout = strtol(optarg, NULL, 0);
			if (timeout < 0) {
				errno = ERANGE;
			} else if (timeout == 0) {
				break;
			}
			if (errno != 0) {
				E("invalid operand to -i %s: %s", optarg, strerror(errno));
				usage(basename(argv[0]));
				return -errno;
			}

			cons->timeout_rel_ms = timeout;
		} break;
		case ':':
			E("option -%c requires an operand\n", optopt);
			usage(basename(argv[0]));
			return -EINVAL;
		case '?':
			E("unrecognized option -%c\n", optopt);
			usage(basename(argv[0]));
			return -EINVAL;
		}
	}

	/* Offset address based on channel selection */
	cons->vuart.addr += cons->vuart.channel * sizeof(uint32_t);

	/* perform extra checking here and error as needed */

	return 0;
}

static void handler(int sig)
{
	I("\nCaught signal %d (%s)", sig, strsignal(sig));
	_cons.stop = true;
}

static int install_handlers(struct console *cons)
{
	int ret;

	if (signal(SIGINT, handler) == SIG_ERR) {
		E("signal: %s", strerror(errno));
		return EXIT_FAILURE;
	}

	if (cons->timeout_rel_ms != 0) {
		/* timeout parameter has been specified, so fire off a timer */

		ret = timer_create(CLOCK_REALTIME, NULL, &cons->timer);
		if (ret < 0) {
			E("%s() failed: %s", "timer_create", strerror(errno));
			return EXIT_FAILURE;
		}

		D(1, "Setting timer for %lu ms", cons->timeout_rel_ms);
		struct itimerspec its = {
			.it_value.tv_sec = cons->timeout_rel_ms / MSEC_PER_SEC,
			.it_value.tv_nsec = cons->timeout_rel_ms % NSEC_PER_MSEC,
		};

		ret = timer_settime(cons->timer, 0, &its, NULL);
		if (ret < 0) {
			E("%s() failed: %s", "timer_settime", strerror(errno));
			timer_delete(cons->timer);
			return EXIT_FAILURE;
		}

		if (signal(SIGALRM, handler) == SIG_ERR) {
			E("signal: %s", strerror(errno));
			timer_delete(cons->timer);
			return EXIT_FAILURE;
		}
	}

	return 0;
}

static void uninstall_handlers(struct console *cons)
{
	(void)signal(SIGALRM, SIG_DFL);
	(void)timer_delete(cons->timer);
	(void)signal(SIGINT, SIG_DFL);
}

static void loop_ratelimit(void)
{
	struct timespec now;
	unsigned long now_ms;
	static unsigned long last_ms;

	clock_gettime(CLOCK_REALTIME, &now);

	now_ms = now.tv_sec * MSEC_PER_SEC + now.tv_nsec / NSEC_PER_MSEC;

	if (now_ms - last_ms < 100) {
		usleep(100 * USEC_PER_MSEC);
	}

	last_ms = now_ms;
}

int main(int argc, char **argv)
{
	int ret;

	console_init(&_cons);

	if (parse_args(&_cons, argc, argv) < 0) {
		return EXIT_FAILURE;
	}

	D(1, "config: dev=%s discovery=0x%08x channel=%u magic=0x%08x pci_dev=0x%04x timeout_ms=%lu",
	  _cons.vuart.dev_name, _cons.vuart.addr, _cons.vuart.channel, _cons.vuart.magic,
	  _cons.vuart.pci_device_id, _cons.timeout_rel_ms);

	ret = install_handlers(&_cons);
	if (ret < 0) {
		return EXIT_FAILURE;
	}

	do {
		loop_ratelimit();

		ret = loop(&_cons);
		if ((ret == -ENOENT) || (ret == -ENXIO)) {
			if (_cons.skip_rescan) {
				I("Skipping PCIe rescan");
				ret = 0;
				continue;
			}
			/*
			 * Lost the virtual uart connection OR it was not found in the first place.
			 * Remove and rescan for the device if possible
			 */
			ret = rescan_pcie(_cons.vuart.dev_name);

			if (ret > 0) {
				/* found at least one device */
				continue;
			}

			switch (ret) {
			case 0:
			case -EACCES: /* permission denied (e.g. not run with sudo) */
			case -ENOENT: /* no devices found after rescan */
				D(2, "sleeping for %lu us", VUART_NOT_READY_SLEEP_US);
				usleep(VUART_NOT_READY_SLEEP_US);
				break;
			default:
				E("Failed to remove and rescan PCIe devices: %s", strerror(-ret));
				return EXIT_FAILURE;
			}
		}

		if (ret < 0) {
			/* e.g. I/O error */
			return EXIT_FAILURE;
		}

	} while (!_cons.stop);

	uninstall_handlers(&_cons);

	/* gracefully terminated via Ctrl+a,x */
	return EXIT_SUCCESS;
}
