/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Tenstorrent firmware virtual UART interface.
 */

#ifndef TENSTORRENT_UART_TT_VIRT_H_
#define TENSTORRENT_UART_TT_VIRT_H_

#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup tt_vuart_apis Tenstorrent virtual UART APIs
 * @{
 */

/**
 * @brief Role of the virtual UART in the context of the shared memory buffer.
 *
 * The virtual UART is a shared memory buffer that is used to communicate between a device and a
 * host. The device and host have different perspectives on the buffer, so the role of the virtual
 * UART is used to clarify the direction of communication.
 *
 * From the perspective of the device, the transmit buffer is used to send data to the host, and
 * the receive buffer is used to receive data from the host. Conversely, from the perspective of
 * the host, the transmit buffer is used to send data to the device, and the receive buffer is used
 * to receive data from the device.
 */
enum tt_vuart_role {
	TT_VUART_ROLE_DEVICE, /**< Device perspective of @ref tt_vuart  */
	TT_VUART_ROLE_HOST,   /**< Host perspective of @ref tt_vuart  */
};

/**
 * @brief In-memory ring buffer descriptor for Tenstorrent virtual UART.
 *
 * This in-memory ring buffer descriptor describes two ring buffers in a contiguous section of
 * uncached memory. Following the descriptor, there are `tx_cap` bytes of space for the transmit
 * buffer, followed by `rx_cap` bytes of space for the receive buffer.
 *
 * The `tx_head`, `tx_tail`, `rx_head`, and `rx_tail` variables are all up-counters (which may
 * wrap around the 2^32 limit). Therefore, the transmit buffer should be read at an offset of
 * zero added to the appropriate counter modulo `tx_cap`, and the receive buffer should be read at
 * an offset of `tx_cap` added to the appropriate counter modulo `rx_cap`.
 *
 * This convention mitigates ambiguity between empty and full buffers. A buffer is empty when the
 * head and tail indices are equal. A buffer is full when the tail index is equal to the head
 * index plus the buffer capacity.
 *
 * Since this descriptor is intended to be shared between both a device, it is important to
 * clarify that the transmit (tx) and receive (rx) directions are from the perspective of the
 * device and should be reversed when viewed from the host. See also `tt_vuart_role`.
 *
 * TODO: add a nice ascii-art diagram.
 */
struct tt_vuart {
	uint32_t magic;    /**< Magic number used to identify the virtual uart in memory */
	uint32_t rx_cap;   /**< Receive buffer capacity, in bytes */
	uint32_t rx_head;  /**< Receive head counter */
	uint32_t rx_tail;  /**< Receive tail counter */
	uint32_t tx_cap;   /**< Transmit buffer capacity, in bytes */
	uint32_t tx_head;  /**< Transmit head counter */
	uint32_t tx_oflow; /**< Number of transmit overflows (device to host) */
	uint32_t tx_tail;  /**< Transmit tail counter */
	uint32_t version;  /**< Version info MS-Byte to LS-Byte [INST.MAJOR.MINOR.PATCH] */
	uint8_t buf[];     /**< Buffer area of `tx_cap` bytes followed by `rx_cap` bytes */
};

/**
 * @brief Determine the instance number of a virtual UART buffer descriptor.
 *
 * @param vuart Pointer to the virtual UART buffer descriptor
 *
 * @return The instance number
 */
static inline size_t tt_vuart_inst(volatile const struct tt_vuart *vuart)
{
	return vuart->version >> 24;
}

/**
 * @brief Determine the size of the given buffer.
 *
 * @param head Head index of the buffer
 * @param tail Tail index of the buffer
 * @return Size of the buffer
 */
static inline uint32_t tt_vuart_buf_size(uint32_t head, uint32_t tail)
{
	return tail - head;
}

/**
 * @brief Determine the free space available in the given buffer.
 *
 * @param head Head index of the buffer
 * @param tail Tail index of the buffer
 * @param cap Capacity of the buffer
 * @return Free space available in the buffer
 */
static inline uint32_t tt_vuart_buf_space(uint32_t head, uint32_t tail, uint32_t cap)
{
	return cap - (tt_vuart_buf_size(head, tail));
}

/**
 * @brief Determine if the given buffer is empty.
 *
 * @param head Head index of the buffer
 * @param tail Tail index of the buffer
 * @return `true` if the buffer is empty, false otherwise
 */
static inline bool tt_vuart_buf_empty(uint32_t head, uint32_t tail)
{
	return tt_vuart_buf_size(head, tail) == 0;
}

/**
 * @brief Determine if the given buffer is full.
 *
 * @param head Head index of the buffer
 * @param tail Tail index of the buffer
 * @param cap Capacity of the buffer
 * @return `true` if the buffer is full, false otherwise
 */
static inline bool tt_vuart_buf_full(uint32_t head, uint32_t tail, uint32_t cap)
{
	return tt_vuart_buf_size(head, tail) == cap;
}

/**
 * @brief Poll the virtual UART buffer for incoming data.
 *
 * This method returns one byte of data from the virtual UART buffer, if available.
 * If the buffer is empty, this method returns -1 (EOF).
 *
 * @param vuart Pointer to the virtual UART buffer descriptor
 * @param p_char Pointer to the location to store the incoming byte
 * @param role Role with respect to the virtual UART buffer
 * @return The incoming byte, or -1 (EOF) if the buffer is empty
 */
static inline int tt_vuart_poll_in(volatile struct tt_vuart *vuart, unsigned char *p_char,
				   enum tt_vuart_role role)
{
	uint32_t cap;
	uint32_t offs;
	uint32_t tail;
	uint32_t head;
	volatile atomic_uint *headp;

	do {
		if (role == TT_VUART_ROLE_DEVICE) {
			headp = (volatile atomic_uint *)&vuart->rx_head;
			head = vuart->rx_head;
			tail = vuart->rx_tail;
			cap = vuart->rx_cap;
			offs = vuart->tx_cap;
		} else if (role == TT_VUART_ROLE_HOST) {
			headp = (volatile atomic_uint *)&vuart->tx_head;
			head = vuart->tx_head;
			tail = vuart->tx_tail;
			cap = vuart->tx_cap;
			offs = 0;
		} else {
			assert((role == TT_VUART_ROLE_DEVICE) || (role == TT_VUART_ROLE_HOST));
			return -1;
		}

		if (tt_vuart_buf_empty(head, tail)) {
			return -1;
		}

		if (atomic_compare_exchange_strong(headp, &head, head + 1)) {
			*p_char = vuart->buf[offs + (head % cap)];
			return *p_char;
		}
	} while (true);

	assert(false);
	return -1;
}

/**
 * @brief Poll the virtual UART buffer with outgoing data.
 *
 * This method transmits one byte of data to the virtual UART buffer, if possible.
 * If the buffer is empty, this method simply returns.
 *
 * If writing to the virtual UART results in buffer overflow, the overflow counter is incremented
 * and the data is discarded.
 *
 * @param vuart Pointer to the virtual UART buffer descriptor
 * @param out_char The byte to transmit
 * @param role Role with respect to the virtual UART buffer
 */
static inline void tt_vuart_poll_out(volatile struct tt_vuart *vuart, unsigned char out_char,
				     enum tt_vuart_role role)
{
	uint32_t head;
	uint32_t cap;
	uint32_t offs;
	uint32_t tail;
	volatile atomic_uint *tailp;

	do {
		if (role == TT_VUART_ROLE_DEVICE) {
			tailp = (volatile atomic_uint *)&vuart->tx_tail;
			tail = vuart->tx_tail;
			head = vuart->tx_head;
			cap = vuart->tx_cap;
			offs = 0;
		} else if (role == TT_VUART_ROLE_HOST) {
			tailp = (volatile atomic_uint *)&vuart->rx_tail;
			tail = vuart->rx_tail;
			head = vuart->rx_head;
			cap = vuart->rx_cap;
			offs = vuart->tx_cap;
		} else {
			assert((role == TT_VUART_ROLE_DEVICE) || (role == TT_VUART_ROLE_HOST));
			break;
		}

		if ((role == TT_VUART_ROLE_DEVICE) && tt_vuart_buf_full(head, tail, cap)) {
			++vuart->tx_oflow;
			return;
		}

		if (atomic_compare_exchange_strong(tailp, &tail, tail + 1)) {
			vuart->buf[offs + (tail % cap)] = out_char;
			return;
		}
	} while (true);
}

#ifdef __ZEPHYR__
#include <zephyr/device.h>

/**
 * @brief Get the virtual UART buffer descriptor for the given device.
 *
 * @param dev Pointer to the device
 * @return Pointer to the virtual UART buffer descriptor
 */
volatile struct tt_vuart *uart_tt_virt_get(const struct device *dev);

#endif

/**
 * @}
 */

#ifdef __cplusplus
}
#endif

#endif /* TENSTORRENT_UART_TT_VIRT_H_ */
