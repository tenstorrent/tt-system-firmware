/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#include "spi_hal.h"

#include "stm32u375xx.h"

/* GLX2 routes the external SPI flash to SPI1 on PE12-PE15. */
#define SPI_MUX0_PIN   0U
#define SPI_MUX1_PIN   1U
#define SPI_MUX_EN_PIN 13U
#define SPI_RESET_PIN  5U

void LL_mDelay(uint32_t delay);

static void set_output(GPIO_TypeDef *port, uint32_t pin, uint32_t high)
{
	if (high) {
		port->BSRR = 1U << pin;
	} else {
		port->BSRR = 1U << (pin + 16U);
	}
}

static void configure_gpio_pin(GPIO_TypeDef *port, uint32_t pin, uint32_t mode, uint32_t pull,
			       uint32_t alternate)
{
	uint32_t shift = pin * 2U;
	uint32_t af_shift = (pin & 7U) * 4U;

	port->MODER = (port->MODER & ~(3U << shift)) | (mode << shift);
	port->PUPDR = (port->PUPDR & ~(3U << shift)) | (pull << shift);
	port->OSPEEDR |= 3U << shift;
	if (pin < 8U) {
		port->AFR[0] = (port->AFR[0] & ~(0xFU << af_shift)) | (alternate << af_shift);
	} else {
		port->AFR[1] = (port->AFR[1] & ~(0xFU << af_shift)) | (alternate << af_shift);
	}
}

static void spi_gpio_init(void)
{
	RCC->AHB2ENR1 |= RCC_AHB2ENR1_GPIOBEN | RCC_AHB2ENR1_GPIOCEN | RCC_AHB2ENR1_GPIODEN |
			 RCC_AHB2ENR1_GPIOEEN;

	/* PB0 high and PB1 low select the DMC path through both muxes. */
	configure_gpio_pin(GPIOB, SPI_MUX0_PIN, 1U, 0U, 0U);
	configure_gpio_pin(GPIOB, SPI_MUX1_PIN, 1U, 0U, 0U);
	set_output(GPIOB, SPI_MUX0_PIN, 1U);
	set_output(GPIOB, SPI_MUX1_PIN, 0U);

	/* PC13 is active-low and enables the SPI mux. */
	configure_gpio_pin(GPIOC, SPI_MUX_EN_PIN, 1U, 0U, 0U);
	set_output(GPIOC, SPI_MUX_EN_PIN, 0U);

	configure_gpio_pin(GPIOD, SPI_RESET_PIN, 1U, 0U, 0U);
	set_output(GPIOD, SPI_RESET_PIN, 0U);

	/* SPI1 alternate function 5: NSS, SCK, MISO, and MOSI. */
	configure_gpio_pin(GPIOE, 12U, 2U, 1U, 5U);
	configure_gpio_pin(GPIOE, 13U, 2U, 0U, 5U);
	configure_gpio_pin(GPIOE, 14U, 2U, 0U, 5U);
	configure_gpio_pin(GPIOE, 15U, 2U, 0U, 5U);

	LL_mDelay(1U);
	set_output(GPIOD, SPI_RESET_PIN, 1U);
}

static void spi_init_registers(void)
{
	RCC->APB2ENR |= RCC_APB2ENR_SPI1EN;
	RCC->APB2RSTR |= RCC_APB2RSTR_SPI1RST;
	RCC->APB2RSTR &= ~RCC_APB2RSTR_SPI1RST;

	SPI1->CR1 = 0U;
	SPI1->CR2 = 0U;
	/* 96 MHz APB2 / 16 = 6 MHz, under the flash's 8 MHz limit. */
	SPI1->CFG1 = SPI_CFG1_DSIZE_0 | SPI_CFG1_DSIZE_1 | SPI_CFG1_DSIZE_2 | SPI_CFG1_MBR_0 |
		     SPI_CFG1_MBR_1;
	SPI1->CFG2 = SPI_CFG2_MASTER | SPI_CFG2_SSOE | SPI_CFG2_CPOL | SPI_CFG2_CPHA;
	SPI1->IFCR = SPI_IFCR_EOTC | SPI_IFCR_OVRC | SPI_IFCR_UDRC | SPI_IFCR_MODFC;
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
	return 0;
}

int stm32_spi_deinit(void)
{
	SPI1->CR1 &= ~SPI_CR1_SPE;
	RCC->APB2RSTR |= RCC_APB2RSTR_SPI1RST;
	RCC->APB2RSTR &= ~RCC_APB2RSTR_SPI1RST;
	return 0;
}

int stm32_spi_transfer(struct spi_buf *bufs, uint8_t count)
{
	uint32_t total = 0U;

	for (uint8_t buffer_index = 0U; buffer_index < count; buffer_index++) {
		total += bufs[buffer_index].len;
	}

	/* TSIZE == 0 means an endless transaction, so EOT would never be raised. */
	SPI1->CR1 &= ~SPI_CR1_SPE;
	SPI1->CR2 = total & SPI_CR2_TSIZE;
	SPI1->CR1 |= SPI_CR1_SPE;
	SPI1->CR1 |= SPI_CR1_CSTART;

	for (uint8_t buffer_index = 0U; buffer_index < count; buffer_index++) {
		for (uint32_t byte_index = 0U; byte_index < bufs[buffer_index].len; byte_index++) {
			while ((SPI1->SR & SPI_SR_TXP) == 0U) {
			}
			/* 8-bit frames need byte accesses; a word access moves four frames. */
			*(volatile uint8_t *)&SPI1->TXDR =
				bufs[buffer_index].tx_buf ? bufs[buffer_index].tx_buf[byte_index]
							  : 0xFFU;
			while ((SPI1->SR & SPI_SR_RXP) == 0U) {
			}
			uint8_t received = *(volatile uint8_t *)&SPI1->RXDR;

			if (bufs[buffer_index].rx_buf) {
				bufs[buffer_index].rx_buf[byte_index] = received;
			}
		}
	}

	while ((SPI1->SR & SPI_SR_EOT) == 0U) {
	}
	SPI1->IFCR = SPI_IFCR_EOTC;
	SPI1->CR1 &= ~SPI_CR1_SPE;
	return 0;
}
