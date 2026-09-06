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
#include <sys/time.h>
#include <unistd.h>

#include "logging.h"
#include "vuart.h"

#ifndef UART_TT_VIRT_MAGIC
#define UART_TT_VIRT_MAGIC 0x775e21a1
#endif

#ifndef UART_TT_VIRT_DISCOVERY_ADDR
#define UART_TT_VIRT_DISCOVERY_ADDR 0x800304a0
#endif

#ifndef UART_CHANNEL
#define UART_CHANNEL 1
#endif

#define BH_SCRAPPY_PCI_DEVICE_ID 0xb140

#define MSEC_PER_SEC  1000UL
#define USEC_PER_MSEC 1000UL
#define USEC_PER_SEC  1000000UL

#define VUART_NOT_READY_SLEEP_US (1 * USEC_PER_SEC)

#define KB(n) (1024 * (n))

#define TT_DEVICE "/dev/tenstorrent/0"

struct tracing {
	bool stop;
	bool enabled;
	struct vuart_data vuart;
	char *filename;
	FILE *fp;
};

static struct tracing tracing = {
	.stop = false,
	.enabled = false,
	.vuart = VUART_DATA_INIT(TT_DEVICE, UART_TT_VIRT_DISCOVERY_ADDR, UART_TT_VIRT_MAGIC,
				 BH_SCRAPPY_PCI_DEVICE_ID, UART_CHANNEL),
	.filename = NULL,
};

int verbose;

/* Read buffer for VUART */
static uint8_t rx_buf[KB(4)];

static int loop(struct tracing *tracing)
{
	int ret;
	/* Strings to manage tracing */
	char enable[] = "enable\r";
	char disable[] = "disable\r";
	size_t bytes_read = 0;
	size_t write_ret;

	ret = vuart_open(&tracing->vuart);
	if (ret < 0) {
		goto out;
	}

	if (tracing->filename == NULL) {
		E("No filename provided for tracing output");
		ret = -EINVAL;
		goto out;
	}

	tracing->fp = fopen(tracing->filename, "wb");
	if (tracing->fp == NULL) {
		E("Failed to open file %s for writing: %s", tracing->filename, strerror(errno));
		ret = -errno;
		goto out;
	}

	I("Writing tracing output to %s, press Ctrl+C to stop", tracing->filename);
	while (!tracing->stop) {
		if (vuart_start(&tracing->vuart) < 0) {
			usleep(VUART_NOT_READY_SLEEP_US);
			continue;
		}

		/* Enable tracing only once */
		if (!tracing->enabled) {
			for (size_t i = 0; i < sizeof(enable) - 1; ++i) {
				vuart_putc(&tracing->vuart, enable[i]);
			}
			tracing->enabled = true;
		}

		/* Read from VUART in a block */
		while ((ret = vuart_read(&tracing->vuart, rx_buf, sizeof(rx_buf))) != -EAGAIN) {
			/* Write data to tracing file */
			if (ret > 0) {
				write_ret = fwrite(rx_buf, 1, ret, tracing->fp);
				if (write_ret != ((size_t)ret)) {
					E("Failed to write to tracing file: %s", strerror(errno));
					ret = -errno;
					goto out;
				}
				bytes_read += ret;
			}
		}
	}

	I("Stopping tracing, writing remaining data to file");

	/* Disable tracing */
	for (size_t i = 0; i < sizeof(disable) - 1; ++i) {
		vuart_putc(&tracing->vuart, disable[i]);
	}

	struct timeval timeout;

	gettimeofday(&timeout, NULL);
	timeout.tv_sec += 1; /* wait for 1 second to flush the VUART */
	while (1) {
		struct timeval now;

		gettimeofday(&now, NULL);

		if (now.tv_sec >= timeout.tv_sec) {
			break;
		}

		while ((ret = vuart_read(&tracing->vuart, rx_buf, sizeof(rx_buf))) != -EAGAIN) {
			/* Write data to tracing file */
			if (ret > 0) {
				write_ret = fwrite(rx_buf, 1, ret, tracing->fp);
				if (write_ret != ((size_t)ret)) {
					E("Failed to write to tracing file: %s", strerror(errno));
					ret = -errno;
					goto out;
				}
				bytes_read += ret;
			}
		}
	}
	I("Tracing stopped, total bytes read: %zu", bytes_read);
out:
	if (tracing->fp) {
		fflush(tracing->fp);
		fclose(tracing->fp);
	}
	vuart_close(&tracing->vuart);

	return ret;
}

static void usage(const char *progname)
{
	I("Firmware console application for use with Tenstorrent PCIe cards\n"
	  "Copyright (c) 2025 Tenstorrent AI ULC\n"
	  "\n"
	  "\n"
	  "%s: %s [args..] <filename>\n"
	  "\n"
	  "args:\n"
	  "-a <addr>          : vuart discovery address (default: %08x)\n"
	  "-b <bar_idx>       : BAR index to use (0 or 4, default: 0)\n"
	  "-c <channel>       : channel number (default: %d)\n"
	  "-d <path>          : path to device node (default: %s)\n"
	  "-h                 : print this help message\n"
	  "-i <pci_device_id> : pci device id (default: %04x)\n"
	  "-m <magic>         : vuart magic (default: %08x)\n"
	  "-q                 : decrease debug verbosity\n"
	  "-v                 : increase debug verbosity\n"
	  "\n"
	  "<filename>         : output file for tracing data\n",
	  __func__, progname, UART_TT_VIRT_DISCOVERY_ADDR, UART_CHANNEL, TT_DEVICE,
	  BH_SCRAPPY_PCI_DEVICE_ID, UART_TT_VIRT_MAGIC);
}

static int parse_args(struct tracing *tracing, int argc, char **argv)
{
	int c;

	tracing->vuart.bar_idx = 0;

	while ((c = getopt(argc, argv, ":a:b:c:d:hi:m:qt:v")) != -1) {
		switch (c) {
		case 'a': {
			unsigned long addr;

			errno = 0;
			addr = strtol(optarg, NULL, 0);
			if (errno != 0) {
				E("invalid operand to -a %s: %s", optarg, strerror(errno));
				usage(basename(argv[0]));
				return -errno;
			}
			tracing->vuart.addr = addr;
		} break;
		case 'b': {
			long bar;

			errno = 0;
			bar = strtol(optarg, NULL, 0);
			if (errno != 0) {
				E("invalid operand to -b %s: %s", optarg, strerror(errno));
				usage(basename(argv[0]));
				return -errno;
			}
			if (bar != 0 && bar != 4) {
				E("invalid operand to -b %s: must be 0 or 4", optarg);
				usage(basename(argv[0]));
				return -EINVAL;
			}
			tracing->vuart.bar_idx = (int)bar;
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
			tracing->vuart.channel = channel;
		} break;
		case 'd':
			tracing->vuart.dev_name = optarg;
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
			tracing->vuart.pci_device_id = (uint16_t)pci_device_id;
		} break;
		case 'm': {
			unsigned long magic;

			errno = 0;
			magic = strtol(optarg, NULL, 0);
			magic &= 0xffffffff;
			if (errno != 0) {
				E("invalid operand to -m %s: %s", optarg, strerror(errno));
				usage(basename(argv[0]));
				return -errno;
			}
			tracing->vuart.magic = magic;
		} break;
		case 'q':
			--verbose;
			break;
		case 'v':
			++verbose;
			break;
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

	/* Check for required non-optional argument (filename) */
	if (optind >= argc) {
		E("Missing required filename argument");
		usage(basename(argv[0]));
		return -EINVAL;
	}

	/* Get the filename from the remaining arguments */
	tracing->filename = argv[optind];

	/* Check if there are extra arguments */
	if (optind + 1 < argc) {
		E("Too many arguments provided");
		usage(basename(argv[0]));
		return -EINVAL;
	}

	/* Offset address based on channel selection */
	tracing->vuart.addr += tracing->vuart.channel * sizeof(uint32_t);

	/* perform extra checking here and error as needed */

	return 0;
}

static void handler(int sig)
{
	D(1, "\nCaught signal %d (%s)", sig, strsignal(sig));
	tracing.stop = true;
}

int main(int argc, char **argv)
{
	if (parse_args(&tracing, argc, argv) < 0) {
		return EXIT_FAILURE;
	}

	if (signal(SIGINT, handler) == SIG_ERR) {
		E("signal: %s", strerror(errno));
		return EXIT_FAILURE;
	}

	if (loop(&tracing) < 0) {
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
