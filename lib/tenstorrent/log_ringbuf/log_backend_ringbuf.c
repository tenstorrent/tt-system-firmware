/*
 * Copyright (c) 2025 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/** @file
 * @brief Ring buffer log backend
 *
 * This backend logs into a ring buffer, which can be read via the
 * `log_backend_ringbuf_get_data()` API. Applications can call this API
 * to stream log data to an external consumer.
 */

#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_core.h>
#include <zephyr/logging/log_output.h>
#include <zephyr/logging/log_backend_std.h>

/*
 * We have a ringbuffer outside of the log framework, so this one can
 * be kept small
 */
static uint8_t buf[1];
static uint32_t log_format_current = CONFIG_LOG_BACKEND_RINGBUF_OUTPUT_DEFAULT;

RING_BUF_DECLARE(ringbuf_output_buf, CONFIG_LOG_BACKEND_RINGBUF_BUFFER_SIZE);

/* Zeroed whenever the ring buffer is reset, so a claim that spans a reset
 * cannot be finished against data logged after it.
 */
static uint32_t claimed;

/**
 * Get address of ring buffer log backend buffer.
 * internally calls `ring_buf_get_ptr()` on the logging ring buffer.
 * @param data Pointer to address. Will be set to location within ring buffer
 * @param length Requested length of data to claim
 * @return Number of valid bytes claimed. May be less than requested length.
 */
int log_backend_ringbuf_get_claim(uint8_t **data, size_t length)
{
	uint32_t n = MIN(ring_buf_get_ptr(&ringbuf_output_buf, data, claimed), length);

	claimed += n;
	return n;
}

/**
 * Finish claiming data from the ring buffer log backend.
 * internally calls `ring_buf_consume()` on the logging ring buffer.
 * @param length Number of bytes read from the buffer.
 * @return 0 on success, negative error code on failure.
 */
int log_backend_ringbuf_finish_claim(size_t length)
{
	if (length > claimed) {
		return -EINVAL;
	}

	ring_buf_consume(&ringbuf_output_buf, length);
	claimed = 0;
	return 0;
}

/* This function is exported so the application can reset buffer state */
void log_backend_ringbuf_clear(void)
{
	ring_buf_reset(&ringbuf_output_buf);
	claimed = 0;
}

/* Finally, allow the application to query ring buffer size */
size_t log_backend_ringbuf_get_used(void)
{
	return ring_buf_size_get(&ringbuf_output_buf);
}

static int char_out(uint8_t *data, size_t length, void *ctx)
{
	ARG_UNUSED(ctx);

	if (IS_ENABLED(CONFIG_LOG_BACKEND_RINGBUF_MODE_DROP)) {
		/* If drop mode is enabled, drop the message if there isn't enough space */
		if (ring_buf_space_get(&ringbuf_output_buf) < length) {
			/* Simply lie to the logging framework that we sent the message */
			return length;
		}
	} else if (IS_ENABLED(CONFIG_LOG_BACKEND_RINGBUF_MODE_OVERWRITE)) {
		if (ring_buf_space_get(&ringbuf_output_buf) < length) {
			/* Drop existing data and start logging to front of buffer */
			ring_buf_reset(&ringbuf_output_buf);
			claimed = 0;
		}
	}

	/* Write the data to the ring buffer */
	return ring_buf_put(&ringbuf_output_buf, data, length);
}

LOG_OUTPUT_DEFINE(log_output_ringbuf, char_out, buf, sizeof(buf));

static void log_backend_ringbuf_process(const struct log_backend *const backend,
					union log_msg_generic *msg)
{
	uint32_t flags = log_backend_std_get_flags();

	log_format_func_t log_output_func = log_format_func_t_get(log_format_current);

	log_output_func(&log_output_ringbuf, &msg->log, flags);
}

static int format_set(const struct log_backend *const backend, uint32_t log_type)
{
	log_format_current = log_type;
	return 0;
}

static void log_backend_ringbuf_panic(struct log_backend const *const backend)
{
}

static void dropped(const struct log_backend *const backend, uint32_t cnt)
{
	ARG_UNUSED(backend);

	log_backend_std_dropped(&log_output_ringbuf, cnt);
}

const struct log_backend_api log_backend_ringbuf_api = {
	.process = log_backend_ringbuf_process,
	.panic = log_backend_ringbuf_panic,
	.dropped = IS_ENABLED(CONFIG_LOG_MODE_IMMEDIATE) ? NULL : dropped,
	.format_set = format_set,
};

LOG_BACKEND_DEFINE(log_backend_ringbuf, log_backend_ringbuf_api, true);
