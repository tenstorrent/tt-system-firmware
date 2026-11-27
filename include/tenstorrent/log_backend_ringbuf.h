/*
 * Copyright (c) 2025 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef LOG_BACKEND_RINGBUF_H_
#define LOG_BACKEND_RINGBUF_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Get address of ring buffer log backend buffer.
 * internally calls `ring_buf_get_ptr()` on the logging ring buffer.
 * @param data Pointer to address. Will be set to location within ring buffer
 * @param length Requested length of data to claim
 * @return Number of valid bytes claimed. May be less than requested length.
 */
int log_backend_ringbuf_get_claim(uint8_t **data, size_t length);

/**
 * Finish claiming data from the ring buffer log backend.
 * internally calls `ring_buf_consume()` on the logging ring buffer.
 * @param length Number of bytes read from the buffer.
 * @return 0 on success, negative error code on failure.
 */
int log_backend_ringbuf_finish_claim(size_t length);

/**
 * Clear the ring buffer log backend.
 * Resets the ring buffer to empty, so new log messages will be written
 */
void log_backend_ringbuf_clear(void);

/**
 * Get the number of bytes currently stored in the ring buffer log backend.
 *
 * @return Number of bytes currently stored in the ring buffer
 */
size_t log_backend_ringbuf_get_used(void);

#ifdef __cplusplus
}
#endif

#endif /* LOG_BACKEND_RINGBUF_H_ */
