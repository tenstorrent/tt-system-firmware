#!/usr/bin/env python3
"""Set / restore the firmware telemetry update interval and read the DVFS counters (Blackhole).

Power CI option `telem_interval_ms`. The SMC firmware refreshes its telemetry table every 100 ms by default; the
characterization message TT_SMC_MSG_CHARACTERISATION (0xC6) with sub-message TT_SUB_MSG_SET_TELEMETRY_UPDATE_INTERVAL
(0x05) changes that at runtime (tt-system-firmware PR 1430, firmware >= 19.15; PR 1448 lifted the 10 ms floor and
added the DVFS counter bank, firmware >= 19.16). The telemetry pass shares the work queue with the 1 ms DVFS loop, so
a short interval can delay voltage/frequency control: the COUNTER_BANK_DVFS counters say whether that happened
(dropped ticks must stay 0 for the capture to be representative).

Messages are sent with pyluwen's `arc_msg_buf` (full 8-word request, full 8-word response); the request word 0 low
byte is the command, byte 1 the sub-message id. Nothing here initialises the device (that woke idle chips before).

  telem_interval.py set <ms> [--clear-counters] [--out FILE]   set on every Blackhole chip, verify via telemetry tag 5
  telem_interval.py restore [--read-counters] [--out FILE]     read DVFS counters, then restore the default (0)
  telem_interval.py status                                     print tag 5 and the counters per chip

Always exits 0: a run must never fail because the firmware does not know the message (older bundles).
"""

from __future__ import annotations

import argparse
import json
import sys
import time

MSG_CHARACTERISATION = 0xC6
SUBMSG_SET_TELEM_INTERVAL = 0x05
MSG_COUNTER = 0x35
COUNTER_CMD_GET, COUNTER_CMD_CLEAR = 0, 1
COUNTER_BANK_DVFS = 1
DVFS_COUNTERS = {"dvfs_dropped_ticks": 0, "dvfs_max_period_us": 1, "dvfs_max_pass_us": 2}
TAG_UPDATE_TELEM_SPEED = 5
TAG_FW_BUNDLE_VERSION = 28  # FlashBundleVersion in luwen telemetry_tags.rs


def _chips():
    import pyluwen

    out = {}
    for i, dev in enumerate(pyluwen.detect_chips_fallible()):
        try:
            if not dev.have_comms():
                continue
            bh = dev.force_upgrade().as_bh()
            if bh is not None:
                out[i] = bh
        except Exception as exc:  # noqa: BLE001
            print(f"pci:{i}: not a reachable Blackhole ({exc})", flush=True)
    return out


def _tag_addr(chip, tag: int) -> int | None:
    """Data address of a telemetry tag via the firmware table (SCRATCH_RAM[13] -> [version][count][entries][data])."""
    try:
        base = chip.axi_read32(chip.axi_translate("arc_ss.reset_unit.SCRATCH_RAM[13]").addr)
        count = chip.axi_read32(base + 4)
        if not (0 < count < 1024):
            return None
        for i in range(count):
            entry = chip.axi_read32(base + 8 + 4 * i)
            if entry & 0xFFFF == tag:
                return base + 8 + 4 * count + 4 * ((entry >> 16) & 0xFFFF)
    except Exception:  # noqa: BLE001
        return None
    return None


def _read_tag(chip, tag: int) -> int | None:
    addr = _tag_addr(chip, tag)
    return int(chip.axi_read32(addr)) if addr is not None else None


def _msg(chip, words: list[int], timeout: float = 2.0):
    buf = (words + [0] * 8)[:8]
    return chip.arc_msg_buf(buf, True, False, timeout)


def set_interval(chip, ms: int):
    return _msg(chip, [MSG_CHARACTERISATION | (SUBMSG_SET_TELEM_INTERVAL << 8), int(ms)])


def counter_get(chip, index: int) -> int | None:
    resp = _msg(chip, [MSG_COUNTER, COUNTER_CMD_GET | (COUNTER_BANK_DVFS << 8) | (index << 16), 0])
    return int(resp[2]) if resp else None  # the firmware shell prints response.data[2] for `tt counter`


def counter_clear(chip, mask: int = 0x7):
    return _msg(chip, [MSG_COUNTER, COUNTER_CMD_CLEAR | (COUNTER_BANK_DVFS << 8), mask & 0xFFFF])


def fw_version(chip) -> str | None:
    v = _read_tag(chip, TAG_FW_BUNDLE_VERSION)
    if v is None:
        return None
    return f"{(v >> 24) & 0xFF}.{(v >> 16) & 0xFF}.{(v >> 8) & 0xFF}.{v & 0xFF}"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("set")
    s.add_argument("ms", type=int)
    s.add_argument("--clear-counters", action="store_true")
    s.add_argument("--out", default="")
    r = sub.add_parser("restore")
    r.add_argument("--read-counters", action="store_true")
    r.add_argument("--out", default="")
    sub.add_parser("status")
    args = ap.parse_args()

    chips = _chips()
    report: dict = {"command": args.cmd, "chips": len(chips), "per_chip": {}, "warnings": []}
    if not chips:
        report["warnings"].append("no Blackhole chips reachable")
    for pci, chip in chips.items():
        rec: dict = {"fw_bundle": fw_version(chip), "before_ms": _read_tag(chip, TAG_UPDATE_TELEM_SPEED)}
        try:
            # counters (fw >= 19.16) are handled separately from the interval setter (fw >= 19.15): a firmware that
            # knows the setter but not the counter message must still get the interval applied
            if args.cmd == "set":
                if args.clear_counters:
                    try:
                        counter_clear(chip)
                    except Exception as exc:  # noqa: BLE001
                        rec["counters_error"] = str(exc)[:120]
                resp = set_interval(chip, args.ms)
                rec["response"] = [int(x) for x in resp] if resp else None
            elif args.cmd == "restore":
                if args.read_counters:
                    try:
                        rec["counters"] = {name: counter_get(chip, idx) for name, idx in DVFS_COUNTERS.items()}
                    except Exception as exc:  # noqa: BLE001
                        rec["counters_error"] = str(exc)[:120]
                resp = set_interval(chip, 0)
                rec["response"] = [int(x) for x in resp] if resp else None
            else:
                rec["counters"] = {name: counter_get(chip, idx) for name, idx in DVFS_COUNTERS.items()}
        except Exception as exc:  # noqa: BLE001  firmware without the message, or a busy mailbox
            rec["error"] = str(exc)[:200]
        if args.cmd in ("set", "restore"):
            time.sleep(0.25)  # the new interval applies on the next telemetry timer expiry
            rec["after_ms"] = _read_tag(chip, TAG_UPDATE_TELEM_SPEED)
        report["per_chip"][str(pci)] = rec

    wanted = args.ms if args.cmd == "set" else (100 if args.cmd == "restore" else None)
    if wanted is not None:
        applied = [p for p, rec in report["per_chip"].items() if rec.get("after_ms") == wanted]
        report["requested_ms"] = wanted
        report["applied_chips"] = len(applied)
        report["applied"] = bool(chips) and len(applied) == len(chips)
        if chips and not report["applied"]:
            report["warnings"].append(
                f"telemetry interval {wanted} ms applied on {len(applied)}/{len(chips)} chip(s); "
                "the firmware may predate the characterization message (needs >= 19.15, counters >= 19.16)"
            )
    if args.cmd == "restore" and args.read_counters:
        agg: dict[str, int] = {}
        for rec in report["per_chip"].values():
            for name, v in (rec.get("counters") or {}).items():
                if v is not None:
                    agg[name] = max(agg.get(name, 0), int(v))
        report["counters_max"] = agg
    print(json.dumps(report, indent=2))
    out = getattr(args, "out", "")
    if out:
        with open(out, "w") as f:
            json.dump(report, f, indent=2)
    for w in report["warnings"]:
        print(f"::warning::telem_interval: {w}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
