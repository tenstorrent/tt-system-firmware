/*
 * Copyright (c) 2025 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef TENSTORRENT_GRENDEL_TC_UTIL_USER_OVERRIDE_H
#define TENSTORRENT_GRENDEL_TC_UTIL_USER_OVERRIDE_H

#include <stdint.h>

/* For WRITE_SCRATCH. Defining it here as well would collide with soc.h in any
 * test that uses the scratch registers for its own reporting.
 */
#include <soc.h>

#define TEST_PASS_VALUE 0xacafaca1

/*
 * Override ztest's testcase end report macro. We should still report
 * the testcase result via serial output, but also write the test pass value
 * to the scratch register. This way, tests running within simulation will
 * also pass
 */
#define TC_END_REPORT(result)                                                                      \
	do {                                                                                       \
		if ((result) == TC_PASS) {                                                         \
			WRITE_SCRATCH(0, TEST_PASS_VALUE);                                         \
		}                                                                                  \
		PRINT_LINE;                                                                        \
		TC_PRINT_RUNID;                                                                    \
		TC_END(result, "PROJECT EXECUTION %s\n",                                           \
		       (result) == TC_PASS ? "SUCCESSFUL" : "FAILED");                             \
		TC_END_POST(result);                                                               \
	} while (false)

#endif /* TENSTORRENT_GRENDEL_TC_UTIL_USER_OVERRIDE_H */
