/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Links the D2D firmware binary into the image and publishes it under the
 * symbol names objcopy's "-I binary" mode would have produced. Those names are
 * an interface with the Sival library, which imports them rather than carrying
 * its own copy of the image, so they are not ours to rename.
 *
 * .incbin is used in preference to actually running objcopy. objcopy derives
 * the symbol names from the input path exactly as spelled on its command line,
 * mangling each non-alphanumeric character to an underscore: the names only
 * come out as _binary_d2d_fw_bin_* if it is invoked from the directory holding
 * the file, with a bare filename. Referring to the blob by any other path
 * silently produces different symbols, which surfaces as an unresolved import
 * in someone else's prebuilt library. objcopy also has to be told the output
 * format and architecture, which then have to track the toolchain. Assembling
 * the image instead keeps the names literal and lets the compiler driver pick
 * the format, as it does for every other object here.
 *
 * TT_D2D_FW_BLOB_PATH is supplied by this directory's CMakeLists.
 */

	.section .rodata.d2d_fw_bin, "a"

	/* tt_d2d_load_fw() reads the image with 32-bit loads. */
	.balign 4

	.global _binary_d2d_fw_bin_start
_binary_d2d_fw_bin_start:
	.incbin TT_D2D_FW_BLOB_PATH

	.global _binary_d2d_fw_bin_end
_binary_d2d_fw_bin_end:

	/*
	 * objcopy also emits a _size symbol, as an absolute whose address --
	 * not contents -- is the length. Code written against that convention
	 * may use it, so define it rather than leave a link failure for
	 * whoever first does.
	 */
	.global _binary_d2d_fw_bin_size
	.set _binary_d2d_fw_bin_size, _binary_d2d_fw_bin_end - _binary_d2d_fw_bin_start
