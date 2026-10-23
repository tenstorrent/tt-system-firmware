# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

"""
Helpers for firmware tests that access a Blackhole chip through tt-dal

Tests use the tt-dal session and TLB directly, and these helpers cover the
steps that would otherwise repeat:

    with open_chip(asic_id) as (sess, tlb):
        noc_write32(tlb, x, y, addr, data)
        status, rsp = smc_message(sess, code, arg)
        sess.power(ttdal.power.Flag.HI)
"""

import time
from contextlib import contextmanager

import ttdal
import ttdal.smc
import ttdal.telem
import ttdal.tlb

TLB_SIZE = 1 << 21
ARC_TILE = (8, 0)
ARC_STATUS_ADDR = 0x80030060
BH_GRID_X = 17
BH_GRID_Y = 12


@contextmanager
def open_chip(asic_id):
    """
    Open a session on /dev/tenstorrent/<asic_id> and allocate one 2 MiB
    uncached TLB, yielding (session, tlb)
    """
    dev = ttdal.Device.from_path(f"/dev/tenstorrent/{asic_id}")
    with (
        dev.open() as sess,
        sess.alloc(ttdal.tlb.Size.Mb2, ttdal.tlb.Caching.Uncached) as tlb,
    ):
        yield sess, tlb


def wait_arc_boot(asic_id, timeout=15):
    """
    Wait until SMC firmware on /dev/tenstorrent/<asic_id> posts a booted
    status, raising TimeoutError otherwise
    """
    deadline = time.monotonic() + timeout
    status = None
    while True:
        try:
            with open_chip(asic_id) as (_, tlb):
                status = noc_read32(tlb, *ARC_TILE, ARC_STATUS_ADDR)
            if (status >> 16) == 0xC0DE and (status & 0xFFFF) >= 0x1D:
                return
        except OSError:
            # The device node may be missing or unusable while the chip resets
            pass
        if time.monotonic() > deadline:
            raise TimeoutError(
                f"SMC firmware did not boot within {timeout}s (status {status})"
            )
        time.sleep(0.5)


def noc_bind(tlb, x, y, addr, noc=0, **cfg):
    """
    Point tlb at the window holding addr on tile (x, y), returning
    (window, offset of addr in the window)
    """
    base = addr & ~(TLB_SIZE - 1)
    win = tlb.bind(ttdal.tlb.Config(addr=base, x_end=x, y_end=y, noc=noc, **cfg))
    return win, addr - base


def noc_read32(tlb, x, y, addr, noc=0):
    win, offset = noc_bind(tlb, x, y, addr, noc)
    return win.read_u32(offset)


def noc_write32(tlb, x, y, addr, data, noc=0):
    win, offset = noc_bind(tlb, x, y, addr, noc)
    win.write_u32(offset, data)


def noc_broadcast32(sess, tlb, addr, data, noc=0):
    """
    Write data at addr on every tile of the grid
    """
    if sess.telemetry()[ttdal.telem.Tag.NocTranslation]:
        # With translation enabled the rectangle is given in logical
        # coordinates, flipped for NOC1
        start, end = ((2, 3), (1, 2)) if noc == 0 else ((1, 2), (2, 3))
    else:
        start, end = (0, 1), (BH_GRID_X - 1, BH_GRID_Y - 1)
    win, offset = noc_bind(
        tlb, *end, addr, noc, x_start=start[0], y_start=max(start[1], 1), mcast=True
    )
    win.write_u32(offset, data)


def smc_message(sess, code, *args):
    """
    Send an SMC message with request words 1.. set to args, returning the
    handler's status byte and the eight response words
    """
    rsp = sess.call(ttdal.smc.Message([code, *args])).message
    return rsp[0] & 0xFF, rsp
