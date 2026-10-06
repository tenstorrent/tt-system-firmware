/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef STM32U3_SPI_HAL_H
#define STM32U3_SPI_HAL_H

#include <stdint.h>

struct spi_buf;

int stm32_spi_init(void);
int stm32_spi_deinit(void);
int stm32_spi_transfer(struct spi_buf *bufs, uint8_t count);

#endif
