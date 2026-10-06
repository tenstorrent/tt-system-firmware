/*
 * Copyright (c) 2025 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SPI_HAL_H
#define SPI_HAL_H

#include <stdint.h>

/*
 * This file contains the HAL interface for SPI peripherals.
 * It includes initialization, deinit and data transfer functions.
 */

struct spi_buf {
	/* Pointer to transmit buffer. If NULL 0xFF will be clocked. */
	const uint8_t *tx_buf;
	/* Pointer to receive buffer. If NULL data will be discarded. */
	uint8_t *rx_buf;
	/* Length of the buffer in bytes. */
	uint32_t len;
};

typedef int (*spi_transfer_fn)(struct spi_buf *bufs, uint8_t cnt);

#ifdef STM32G0xx

#include "stm32_spi_hal.h"
#define spi_init     stm32_spi_init
#define spi_deinit   stm32_spi_deinit
#define spi_transfer stm32_spi_transfer

#elif defined(STM32U3xx)

#include "stm32u3_spi_hal.h"
#define spi_init     stm32_spi_init
#define spi_deinit   stm32_spi_deinit
#define spi_transfer stm32_spi_transfer

#else
#error "No SPI HAL implementation defined. Please define STM32G0xx or STM32U3xx."
#endif

#endif /* SPI_HAL_H */
