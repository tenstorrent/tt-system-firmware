/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>

#include "spi_hal.h"

#include "stm32u375xx.h"
#include "stm32u3xx_ll_bus.h"
#include "stm32u3xx_ll_gpio.h"
#include "stm32u3xx_ll_rcc.h"
#include "stm32u3xx_ll_spi.h"

#define SPI_FLAG_TIMEOUT 10000U

void LL_mDelay(uint32_t delay);

static void configure_spi_pin(GPIO_TypeDef *port, uint32_t pin, uint32_t pull)
{
	LL_GPIO_SetPinMode(port, pin, LL_GPIO_MODE_ALTERNATE);
	LL_GPIO_SetPinSpeed(port, pin, LL_GPIO_SPEED_FREQ_VERY_HIGH);
	LL_GPIO_SetPinOutputType(port, pin, LL_GPIO_OUTPUT_PUSHPULL);
	LL_GPIO_SetPinPull(port, pin, pull);
	LL_GPIO_SetAFPin_8_15(port, pin, LL_GPIO_AF_5);
}

static void configure_output_pin(GPIO_TypeDef *port, uint32_t pin)
{
	LL_GPIO_SetPinMode(port, pin, LL_GPIO_MODE_OUTPUT);
	LL_GPIO_SetPinSpeed(port, pin, LL_GPIO_SPEED_FREQ_VERY_HIGH);
	LL_GPIO_SetPinOutputType(port, pin, LL_GPIO_OUTPUT_PUSHPULL);
	LL_GPIO_SetPinPull(port, pin, LL_GPIO_PULL_NO);

	LL_GPIO_ResetOutputPin(port, pin);
}

static void configure_spi_pins(void)
{
	configure_spi_pin(GPIOE, LL_GPIO_PIN_12, LL_GPIO_PULL_UP);
	configure_spi_pin(GPIOE, LL_GPIO_PIN_13, LL_GPIO_PULL_DOWN);
	configure_spi_pin(GPIOE, LL_GPIO_PIN_14, LL_GPIO_PULL_DOWN);
	configure_spi_pin(GPIOE, LL_GPIO_PIN_15, LL_GPIO_PULL_DOWN);
}

static void configure_control_pins(void)
{
	configure_output_pin(GPIOB, LL_GPIO_PIN_0);
	configure_output_pin(GPIOB, LL_GPIO_PIN_1);
	configure_output_pin(GPIOD, LL_GPIO_PIN_5);

	LL_GPIO_SetOutputPin(GPIOB, LL_GPIO_PIN_0);
	LL_GPIO_ResetOutputPin(GPIOB, LL_GPIO_PIN_1);
	LL_GPIO_ResetOutputPin(GPIOD, LL_GPIO_PIN_5);
}

static void spi_gpio_init(void)
{
	LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_GPIOB | LL_AHB2_GRP1_PERIPH_GPIOC |
				 LL_AHB2_GRP1_PERIPH_GPIOD | LL_AHB2_GRP1_PERIPH_GPIOE);

	configure_control_pins();
	configure_spi_pins();

	LL_mDelay(1U);
	LL_GPIO_SetOutputPin(GPIOD, LL_GPIO_PIN_5);
	LL_mDelay(100U);
}

static void spi_init_registers(void)
{
	LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_SPI1);
	LL_APB2_GRP1_ForceReset(LL_APB2_GRP1_PERIPH_SPI1);
	LL_APB2_GRP1_ReleaseReset(LL_APB2_GRP1_PERIPH_SPI1);
	LL_RCC_SetSPIClockSource(LL_RCC_SPI1_CLKSOURCE_PCLK2);

	LL_SPI_SetTransferDirection(SPI1, LL_SPI_FULL_DUPLEX);
	LL_SPI_SetMode(SPI1, LL_SPI_MODE_MASTER);
	LL_SPI_SetDataWidth(SPI1, LL_SPI_DATAWIDTH_8BIT);
	LL_SPI_SetClockPolarity(SPI1, LL_SPI_POLARITY_HIGH);
	LL_SPI_SetClockPhase(SPI1, LL_SPI_PHASE_2EDGE);
	LL_SPI_SetTransferBitOrder(SPI1, LL_SPI_MSB_FIRST);
	LL_SPI_SetStandard(SPI1, LL_SPI_PROTOCOL_MOTOROLA);
	LL_SPI_SetNSSMode(SPI1, LL_SPI_NSS_HARD_OUTPUT);
	LL_SPI_SetNSSPolarity(SPI1, LL_SPI_NSS_POLARITY_LOW);
	LL_SPI_DisableNSSPulseMgt(SPI1);
	LL_SPI_DisableCRC(SPI1);
	/* 96 MHz APB2 / 16 = 6 MHz, under the flash's 8 MHz limit. */
	LL_SPI_SetBaudRatePrescaler(SPI1, LL_SPI_BAUDRATEPRESCALER_DIV16);
	SPI1->IFCR = SPI_IFCR_EOTC | SPI_IFCR_OVRC | SPI_IFCR_UDRC | SPI_IFCR_MODFC;
}

static int spi_exit_deep_power_down(void)
{
	uint8_t command = 0xAB;
	struct spi_buf buffer = {
		.tx_buf = &command,
		.rx_buf = NULL,
		.len = 1,
	};
	int ret = stm32_spi_transfer(&buffer, 1);

	if (ret == 0) {
		/* LL_mDelay(100) is approximately 30 ms at the FLM clock rate. */
		LL_mDelay(100U);
	}
	return ret;
}

void LL_mDelay(uint32_t delay)
{
	/* The FLM only needs a short reset pulse; this loop is clock-independent. */
	for (volatile uint32_t count = delay * 10000U; count != 0U; count--) {
		__NOP();
	}
}

int stm32_spi_init(void)
{
	spi_gpio_init();
	spi_init_registers();
	return spi_exit_deep_power_down();
}

int stm32_spi_deinit(void)
{
	LL_SPI_Disable(SPI1);
	LL_APB2_GRP1_ForceReset(LL_APB2_GRP1_PERIPH_SPI1);
	LL_APB2_GRP1_ReleaseReset(LL_APB2_GRP1_PERIPH_SPI1);
	return 0;
}

int stm32_spi_transfer(struct spi_buf *bufs, uint8_t count)
{
	uint32_t total = 0U;

	for (uint8_t buffer_index = 0U; buffer_index < count; buffer_index++) {
		total += bufs[buffer_index].len;
	}

	/* TSIZE == 0 means an endless transaction, so EOT would never be raised. */
	LL_SPI_Disable(SPI1);
	SPI1->CR2 = total & SPI_CR2_TSIZE;
	LL_SPI_Enable(SPI1);
	LL_SPI_StartMasterTransfer(SPI1);

	for (uint8_t buffer_index = 0U; buffer_index < count; buffer_index++) {
		for (uint32_t byte_index = 0U; byte_index < bufs[buffer_index].len; byte_index++) {
			uint32_t timeout = SPI_FLAG_TIMEOUT;

			while (LL_SPI_IsActiveFlag_TXP(SPI1) == 0U && timeout-- != 0U) {
			}
			if (timeout == 0U) {
				LL_SPI_Disable(SPI1);
				return -1;
			}
			LL_SPI_TransmitData8(SPI1, bufs[buffer_index].tx_buf
							   ? bufs[buffer_index].tx_buf[byte_index]
							   : 0xFFU);
			timeout = SPI_FLAG_TIMEOUT;
			while (LL_SPI_IsActiveFlag_RXP(SPI1) == 0U && timeout-- != 0U) {
			}
			if (timeout == 0U) {
				LL_SPI_Disable(SPI1);
				return -1;
			}
			uint8_t received = LL_SPI_ReceiveData8(SPI1);

			if (bufs[buffer_index].rx_buf) {
				bufs[buffer_index].rx_buf[byte_index] = received;
			}
		}
	}

	uint32_t timeout = SPI_FLAG_TIMEOUT;

	while (LL_SPI_IsActiveFlag_EOT(SPI1) == 0U && timeout-- != 0U) {
	}
	if (timeout == 0U) {
		LL_SPI_Disable(SPI1);
		LL_SPI_SetTransferSize(SPI1, 0U);
		return -1;
	}
	LL_SPI_ClearFlag_TXTF(SPI1);
	LL_SPI_ClearFlag_OVR(SPI1);
	LL_SPI_ClearFlag_EOT(SPI1);
	LL_SPI_Disable(SPI1);
	LL_SPI_SetTransferSize(SPI1, 0U);
	return 0;
}
