/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(grendel_log_bridge, CONFIG_TT_GR_SMC_LOG_BRIDGE_LOG_LEVEL);

#define GRENDEL_LOG_LINE_MAX 256

static bool level_eq(const char *lhs, const char *rhs)
{
	return lhs != NULL && rhs != NULL && strcmp(lhs, rhs) == 0;
}

static void emit_zephyr_log(const char *level, const char *message)
{
	if (level_eq(level, "ERROR") || level_eq(level, "FAIL")) {
		LOG_ERR("%s", message);
		return;
	}

	if (level_eq(level, "WARN")) {
		LOG_WRN("%s", message);
		return;
	}

	if (level_eq(level, "DEBUG")) {
		LOG_DBG("%s", message);
		return;
	}

	LOG_INF("%s", message);
}

static void emit_zephyr_log_with_location(const char *level, const char *file, int line,
					  const char *message)
{
	const char *safe_level = level != NULL ? level : "INFO";
	const char *safe_file = file != NULL ? file : "unknown";

	if (level_eq(level, "ERROR") || level_eq(level, "FAIL")) {
		LOG_ERR("[%s] %s:%d: %s", safe_level, safe_file, line, message);
		return;
	}

	if (level_eq(level, "WARN")) {
		LOG_WRN("[%s] %s:%d: %s", safe_level, safe_file, line, message);
		return;
	}

	if (level_eq(level, "DEBUG")) {
		LOG_DBG("[%s] %s:%d: %s", safe_level, safe_file, line, message);
		return;
	}

	LOG_INF("[%s] %s:%d: %s", safe_level, safe_file, line, message);
}

void log_message(const char *level, const char *fmt, ...)
{
	char body[GRENDEL_LOG_LINE_MAX];
	int body_len;
	va_list args;

	if (fmt == NULL) {
		return;
	}

	va_start(args, fmt);
	body_len = vsnprintf(body, sizeof(body), fmt, args);
	va_end(args);

	if (body_len < 0) {
		return;
	}

	emit_zephyr_log(level, body);
}

void log_with_location(const char *level, const char *file, int line, const char *fmt, ...)
{
	char body[GRENDEL_LOG_LINE_MAX];
	int body_len;
	va_list args;

	if (fmt == NULL) {
		return;
	}

	va_start(args, fmt);
	body_len = vsnprintf(body, sizeof(body), fmt, args);
	va_end(args);

	if (body_len < 0) {
		return;
	}

	emit_zephyr_log_with_location(level, file, line, body);
}
