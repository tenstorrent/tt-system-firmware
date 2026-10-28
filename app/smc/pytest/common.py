# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

"""Helpers shared by the SMC pytest modules."""

import logging
import subprocess
import time

logger = logging.getLogger(__name__)


def _run_iterations(test_fn, dal_chip, total_tries):
    fail_count = 0
    for i in range(total_tries):
        try:
            test_fn(dal_chip)
        except AssertionError as e:
            logger.error(f"Failed iteration {i}: {e}")
            fail_count += 1

    assert fail_count == 0


def _run_tt_burnin(duration=180):
    """Run tt-burnin for duration seconds and check it exits successfully."""
    burnin_process = subprocess.Popen(
        ["tt-burnin", "--no-reset"],
        stdin=subprocess.PIPE,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )

    terminated_early = False
    try:
        end_time = time.time() + duration
        while time.time() < end_time:
            if burnin_process.poll() is not None:
                terminated_early = True
                break
            time.sleep(1.0)
    finally:
        if burnin_process.poll() is None:
            # Send enter key to stop tt-burnin gracefully
            try:
                burnin_process.stdin.write(b"\n")
                burnin_process.stdin.flush()
                burnin_process.wait(timeout=5)
            except (subprocess.TimeoutExpired, BrokenPipeError):
                logger.warning(
                    "tt-burnin did not terminate gracefully, killing process"
                )
                burnin_process.terminate()
                try:
                    burnin_process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    burnin_process.kill()
                    burnin_process.wait()

    assert not terminated_early, "tt-burnin terminated early"
    assert burnin_process.returncode == 0, (
        f"tt-burnin failed with {burnin_process.returncode}"
    )
