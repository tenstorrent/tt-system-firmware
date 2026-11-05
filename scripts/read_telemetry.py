# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

"""Chip telemetry reader for Power CI (Blackhole, pyluwen).

Writes one CSV per board id (telemetry_<BOARD_ID>.csv, one row per chip per sample) until SIGINT/SIGTERM, then
writes a JSON report (rows per chip, read failures, firmware telemetry interval, DVFS counters).

    read_telemetry.py --csv telemetry --workload idle [--delay 0.05] [--interval-ms 10] [--report telemetry_report.json]
    read_telemetry.py --restore-interval          # put the firmware telemetry interval back to its default and exit

--interval-ms sets the firmware telemetry refresh interval (default 100 ms) with the characterisation message
(TT_SMC_MSG_CHARACTERISATION 0xC6 / sub-message 0x05, firmware >= 19.15) and clears the DVFS counter bank
(firmware >= 19.16); the counters are read back and the default restored when the capture stops. A firmware that
does not know the message is reported, never fatal. Nothing here initialises the device.
"""

from __future__ import annotations

import argparse
import csv
import glob
import json
import os
import signal
import subprocess
import sys
import time
from datetime import datetime, timezone

import pyluwen

GDDR_TEMP_TAGS = {
    0: "GDDR01_TEMP",
    1: "GDDR01_TEMP",
    2: "GDDR23_TEMP",
    3: "GDDR23_TEMP",
    4: "GDDR45_TEMP",
    5: "GDDR45_TEMP",
    6: "GDDR67_TEMP",
    7: "GDDR67_TEMP",
}
INVALID_FLOAT_TELEMETRY = 0x80000000  # firmware ConvertFloatToTelemetry(): not a valid float

# Telemetry tag ids (include/tenstorrent/telemetry_tags.h). AICLK_ARB_MAX = arb_max_freq | (arbiter << 16).
ARB_TAGS = {
    "AICLK_ARB_MIN": 65,
    "AICLK_ARB_MAX": 66,
    "KERNEL_THROTTLER": 75,
    "UPDATE_TELEM_SPEED": 5,
}
TAG_UPDATE_TELEM_SPEED = 5
TAG_FW_BUNDLE_VERSION = 28

# ARC messages for the telemetry interval knob and the DVFS counters
MSG_CHARACTERISATION = 0xC6
SUBMSG_SET_TELEM_INTERVAL = 0x05
MSG_COUNTER = 0x35
COUNTER_CMD_GET, COUNTER_CMD_CLEAR = 0, 1
COUNTER_BANK_DVFS = 1
DVFS_COUNTERS = {"dvfs_dropped_ticks": 0, "dvfs_max_period_us": 1, "dvfs_max_pass_us": 2}
DEFAULT_INTERVAL_MS = 100
# A board reset mid-capture (tt_burnin, tt-smi -r) leaves pyluwen hung inside a read, which no signal interrupts,
# so the sampling runs in a child process: the parent restarts it when the CSVs stop growing.
STALL_S = (
    10  # no new CSV bytes for this long: kill and restart the sampler (it re-detects the chips)
)
START_GRACE_S = 60  # a fresh sampler gets this long to detect the chips and apply the interval
GIVE_UP_S = 300  # no reading at all for this long: the capture is dead

STOP = False


def _stop(signum, frame):  # noqa: ARG001
    global STOP
    STOP = True


def signed_16_16(value: int) -> float:
    value &= 0xFFFFFFFF
    if value in (INVALID_FLOAT_TELEMETRY, 0xFFFFFFFF):
        return -1
    if value & 0x80000000:
        value -= 1 << 32
    return (value >> 16) + (value & 0xFFFF) / 65536.0


# ---------------------------------------------------------------- telemetry table access (read-only AXI)


def telemetry_table(chip) -> tuple[int, int, int] | None:
    """(table base, entry count, data base) of the firmware telemetry table: tag table at ARC SCRATCH_RAM[13]
    ([version][count][entries tag | offset<<16]), data at SCRATCH_RAM[12] or right after the table."""
    base = chip.axi_read32(chip.axi_translate("arc_ss.reset_unit.SCRATCH_RAM[13]").addr)
    if base < 0x10000000 or base > 0x1007FFFF:
        raise RuntimeError(f"telemetry table address {base:#x} outside CSM")
    count = chip.axi_read32(base + 4)
    if count <= 0 or count >= 1024:
        raise RuntimeError(f"implausible entry count {count}")
    data = chip.axi_read32(chip.axi_translate("arc_ss.reset_unit.SCRATCH_RAM[12]").addr)
    if data < 0x10000000 or data > 0x1007FFFF:
        data = base + 8 + 4 * count
    return base, count, data


def tag_addresses(chip, tags: dict[str, int]) -> dict[str, int]:
    base, count, data = telemetry_table(chip)
    found = {}
    for i in range(count):
        entry = chip.axi_read32(base + 8 + 4 * i)
        tag, offset = entry & 0xFFFF, (entry >> 16) & 0xFFFF
        for name, want in tags.items():
            if tag == want:
                found[name] = data + 4 * offset
    return found


def read_tag(chip, tag: int) -> int | None:
    try:
        addr = tag_addresses(chip, {"t": tag}).get("t")
        return int(chip.axi_read32(addr)) if addr is not None else None
    except Exception:  # noqa: BLE001  a transient AXI error must not abort the other chips
        return None


class ArbReader:
    """AICLK arbiter / kernel throttler / telemetry interval tags, which the pyluwen struct does not expose."""

    def __init__(self, bh_chips: dict):
        self.addrs: dict[int, dict[str, int]] = {}
        self.chips = {}
        for pci, chip in bh_chips.items():
            try:
                self.addrs[pci] = tag_addresses(chip, ARB_TAGS)
                self.chips[pci] = chip
            except Exception as exc:  # noqa: BLE001
                print(f"arbiter tags unavailable on pci:{pci} ({exc})", flush=True)
        print(
            f"arbiter tags on {len(self.addrs)} device(s): {sorted({n for a in self.addrs.values() for n in a})}",
            flush=True,
        )

    def read(self, pci: int) -> dict[str, int]:
        addrs = self.addrs.get(pci)
        if not addrs:
            return {}
        try:
            return {name: int(self.chips[pci].axi_read32(addr)) for name, addr in addrs.items()}
        except Exception as exc:  # noqa: BLE001
            print(f"arbiter read failed on pci:{pci}; disabled for this device ({exc})", flush=True)
            self.addrs[pci] = {}
            return {}


# ---------------------------------------------------------------- telemetry interval knob and DVFS counters


def arc_msg(chip, words: list[int], timeout: float = 2.0):
    """One 8-word ARC request (timeout in seconds, as pyluwen's arc_msg_buf takes it);
    response word 0 nonzero = rejected or unknown (tt_shell.c makes the same check)."""
    resp = chip.arc_msg_buf((words + [0] * 8)[:8], timeout=timeout)
    if resp and int(resp[0]) != 0:
        raise RuntimeError(f"ARC request 0x{words[0] & 0xFF:02x} rejected (status {int(resp[0])})")
    return resp


def set_interval(chip, ms: int):
    return arc_msg(chip, [MSG_CHARACTERISATION | (SUBMSG_SET_TELEM_INTERVAL << 8), int(ms)])


def counter_get(chip, index: int) -> int | None:
    resp = arc_msg(
        chip, [MSG_COUNTER, COUNTER_CMD_GET | (COUNTER_BANK_DVFS << 8) | (index << 16), 0]
    )
    return int(resp[2]) if resp else None


def counter_clear(chip, mask: int = 0x7):
    return arc_msg(chip, [MSG_COUNTER, COUNTER_CMD_CLEAR | (COUNTER_BANK_DVFS << 8), mask & 0xFFFF])


def fw_version(chip) -> str | None:
    v = read_tag(chip, TAG_FW_BUNDLE_VERSION)
    return (
        None if v is None else f"{(v >> 24) & 0xFF}.{(v >> 16) & 0xFF}.{(v >> 8) & 0xFF}.{v & 0xFF}"
    )


def apply_interval(bh_chips: dict, ms: int, clear_counters: bool) -> dict:
    """Set the interval on every chip and verify it through telemetry tag 5. ms=0 restores the default."""
    wanted = ms or DEFAULT_INTERVAL_MS
    per_chip, applied = {}, 0
    for pci, chip in bh_chips.items():
        rec: dict = {
            "fw_bundle": fw_version(chip),
            "before_ms": read_tag(chip, TAG_UPDATE_TELEM_SPEED),
        }
        if clear_counters:
            try:
                counter_clear(chip)
            except Exception as exc:  # noqa: BLE001  counters need firmware >= 19.16, the setter >= 19.15
                rec["counters_error"] = str(exc)[:120]
        try:
            set_interval(chip, ms)
        except Exception as exc:  # noqa: BLE001  firmware without the message, or a busy mailbox
            rec["error"] = str(exc)[:200]
        time.sleep(0.25)  # the new interval applies on the next telemetry timer expiry
        rec["after_ms"] = read_tag(chip, TAG_UPDATE_TELEM_SPEED)
        applied += rec["after_ms"] == wanted
        per_chip[str(pci)] = rec
    return {
        "requested_ms": ms,
        "applied": bool(bh_chips) and applied == len(bh_chips),
        "applied_chips": applied,
        "chips": len(bh_chips),
        "per_chip": per_chip,
    }


def read_counters(bh_chips: dict) -> dict[str, int]:
    """Max over chips of each DVFS counter (dropped ticks must stay 0 for the capture to be representative)."""
    agg: dict[str, int] = {}
    for chip in bh_chips.values():
        for name, idx in DVFS_COUNTERS.items():
            try:
                v = counter_get(chip, idx)
            except Exception:  # noqa: BLE001
                v = None
            if v is not None:
                agg[name] = max(agg.get(name, 0), int(v))
    return agg


# ---------------------------------------------------------------- one telemetry sample -> CSV row


def chip_row(raw: dict, pci: int, workload: str, arb: dict) -> dict:
    def hexval(name, default=None):
        v = raw.get(name)
        return default if v is None else int(v, 16)

    def avail(name, default=0):  # all-ones = not available on this board (e.g. FAN_RPM on Galaxy)
        v = hexval(name)
        return default if v is None or v == 0xFFFFFFFF else v

    def temp(name):  # 0 = not implemented, all-ones = unavailable: both -1, never 0.0 C
        v = hexval(name)
        return -1 if v in (None, 0, 0xFFFFFFFF, INVALID_FLOAT_TELEMETRY) else signed_16_16(v)

    row = {
        "TIMESTAMP": datetime.now(timezone.utc).isoformat(sep=" ", timespec="milliseconds"),
        "WORKLOAD": workload,
        "PCI_INDEX": pci,  # on Galaxy every chip reports the same BOARD_ID; this tells the 32 chips apart
        "BOARD_ID": raw.get("BOARD_ID", "-1"),  # hex string, as in every earlier capture
        "VCORE": hexval("VCORE", -1000) / 1000,
        "TDC": hexval("TDC", -1) & 0xFFFF if raw.get("TDC") is not None else -1,
        "TDP": hexval("TDP") & 0xFFFF if raw.get("TDP") is not None else -1,
        "INPUT_POWER": hexval("INPUT_POWER") & 0xFFFF if raw.get("INPUT_POWER") is not None else -1,
        "ASIC_TEMP": signed_16_16(hexval("ASIC_TEMPERATURE"))
        if raw.get("ASIC_TEMPERATURE") is not None
        else -1,
        "AICLK": hexval("AICLK") & 0xFFFF if raw.get("AICLK") is not None else -1,
        "THERM_TRIP_COUNT": avail("THERM_TRIP_COUNT") & 0xFFFF,
        "TIMER_HEARTBEAT": avail("TIMER_HEARTBEAT"),
        "ETH_LIVE_STATUS": avail("ETH_STATUS0"),
    }
    for pair in ("01", "23", "45", "67"):
        row[f"GDDR{pair}_CORR_ERRS"] = avail(f"GDDR{pair}_CORR_ERRS")
    row["GDDR_UNCORR_ERRS"] = avail("GDDR_UNCORR_ERRS")
    row["VREG_TEMP"] = temp("VREG_TEMPERATURE")
    row["BOARD_TEMP"] = temp("BOARD_TEMPERATURE")
    for name in (
        "FAN_RPM",
        "TDP_LIMIT_MAX",
        "TDC_LIMIT_MAX",
        "AICLK_LIMIT_MAX",
        "THM_LIMIT_THROTTLE",
    ):
        row[name] = avail(name, -1)
    amax, amin = arb.get("AICLK_ARB_MAX", -1), arb.get("AICLK_ARB_MIN", -1)
    row["AICLK_ARB_MAX_ID"] = amax >> 16 if amax >= 0 else -1
    row["AICLK_ARB_MAX_MHZ"] = amax & 0xFFFF if amax >= 0 else -1
    row["AICLK_ARB_MIN_ID"] = amin >> 16 if amin >= 0 else -1
    row["AICLK_ARB_MIN_MHZ"] = amin & 0xFFFF if amin >= 0 else -1
    row["KERNEL_THROTTLER"] = arb.get("KERNEL_THROTTLER", -1)
    row["UPDATE_TELEM_SPEED"] = arb.get("UPDATE_TELEM_SPEED", -1)
    for ctrl, tag in GDDR_TEMP_TAGS.items():
        v = hexval(tag)
        if v is None:
            row[f"GDDR{ctrl}_TEMP_BOTTOM"] = row[f"GDDR{ctrl}_TEMP_TOP"] = -1
        else:
            shift = 0 if ctrl % 2 == 0 else 16
            row[f"GDDR{ctrl}_TEMP_BOTTOM"] = (v >> shift) & 0xFF
            row[f"GDDR{ctrl}_TEMP_TOP"] = (v >> (shift + 8)) & 0xFF
    return row


class CsvSink:
    """Append rows to <prefix>_<BOARD_ID>.csv; header once, flushed per sample (the stop step copies right after SIGINT)."""

    def __init__(self, prefix: str):
        self.prefix = prefix
        self.files: dict[str, tuple] = {}

    def write(self, row: dict) -> None:
        name = f"{self.prefix}_{row['BOARD_ID']}.csv"
        entry = self.files.get(name)
        if entry is None:
            fh = open(name, "a", newline="")
            writer = csv.DictWriter(
                fh, fieldnames=list(row.keys()), extrasaction="ignore", restval=-1
            )
            if fh.tell() == 0:
                writer.writeheader()
            entry = self.files[name] = (fh, writer)
        fh, writer = entry
        writer.writerow(row)
        fh.flush()

    def close(self) -> None:
        for fh, _ in self.files.values():
            fh.close()
        self.files.clear()


def detect() -> tuple[list[int], list, dict]:
    pci_ids, devices, bh_chips = [], [], {}
    for i, dev in enumerate(pyluwen.detect_chips_fallible()):
        if not dev.have_comms():
            print(f"Cannot communicate with device at pci:{i}", flush=True)
            continue
        pci_ids.append(i)
        devices.append(dev)
        try:
            bh = dev.force_upgrade().as_bh()
            if bh is not None:
                bh_chips[i] = bh
        except Exception:  # noqa: BLE001
            pass
    return pci_ids, devices, bh_chips


def write_report(path: str, report: dict) -> None:
    if path:
        with open(path, "w") as f:
            json.dump(report, f, indent=2)


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(
        description="Read Blackhole chip telemetry to CSV", allow_abbrev=False
    )
    ap.add_argument("--csv", default="telemetry", help="CSV name prefix")
    ap.add_argument(
        "--delay",
        type=float,
        default=None,
        help="seconds between samples (default: half the firmware interval)",
    )
    ap.add_argument("--workload", default="", help="label stamped onto every row")
    ap.add_argument(
        "--interval-ms",
        type=int,
        default=0,
        help="firmware telemetry interval for the capture (0 = leave the default)",
    )
    ap.add_argument("--report", default="", help="JSON report written when the capture stops")
    ap.add_argument("--no-arb", action="store_true", help="skip the AICLK arbiter / throttler tags")
    ap.add_argument(
        "--restore-interval", action="store_true", help="restore the firmware interval and exit"
    )
    ap.add_argument("--sampler", action="store_true", help=argparse.SUPPRESS)  # the child process
    return ap.parse_args()


def new_report() -> dict:
    return {
        "chips_detected": 0,
        "rows_per_chip": {},
        "read_failures": {},
        "interval": {},
        "counters_max": {},
        "restored": None,
        "restarts": 0,
    }


def sample(args: argparse.Namespace) -> int:
    """One chip detection, then rows until SIGINT/SIGTERM; the report goes to --report."""
    pci_ids, devices, bh_chips = detect()
    report = new_report()
    report["chips_detected"] = len(pci_ids)
    if not pci_ids:
        print("no chip detected", flush=True)
        write_report(args.report, report)
        return 1

    delay = args.delay if args.delay is not None else DEFAULT_INTERVAL_MS / 2000
    if args.interval_ms > 0:
        applied = apply_interval(bh_chips, args.interval_ms, clear_counters=True)
        report["interval"] = applied
        if applied["applied"]:
            if args.delay is None:
                delay = max(0.002, args.interval_ms / 2000)  # two reads per firmware refresh
        else:
            print(
                f"::warning::telemetry interval {args.interval_ms} ms applied on {applied['applied_chips']}/{len(bh_chips)} "
                "chip(s); capturing at the firmware default (needs firmware >= 19.15, counters >= 19.16)",
                flush=True,
            )
    arb = None if args.no_arb else ArbReader(bh_chips)
    sink = CsvSink(args.csv)
    rows: dict[int, int] = {}
    failures: dict[int, int] = {}
    rc = 0
    reported = time.monotonic()
    print(f"capturing {len(pci_ids)} chip(s) every {delay:.3f} s", flush=True)
    try:
        while not STOP:
            if (
                time.monotonic() - reported > 5
            ):  # a sampler killed mid-reset still leaves a recent count
                report["rows_per_chip"] = {str(k): v for k, v in sorted(rows.items())}
                report["read_failures"] = {str(k): v for k, v in sorted(failures.items())}
                write_report(args.report, report)
                reported = time.monotonic()
            for pci, dev in zip(pci_ids, devices):
                try:
                    t = dev.force_upgrade().as_bh().get_telemetry()
                except Exception as exc:  # noqa: BLE001  the other chips keep their rows; the gap is reported at the end
                    failures[pci] = failures.get(pci, 0) + 1
                    if failures[pci] == 1:
                        print(f"telemetry read failed on pci:{pci}: {exc}", flush=True)
                    continue
                raw = {}
                for key in dir(t):
                    if key.startswith("_"):
                        continue
                    v = getattr(t, key)
                    if v is not None and not isinstance(v, (str, bool)):
                        try:
                            raw[key.upper()] = hex(int(v))
                        except (TypeError, ValueError):
                            pass
                sink.write(chip_row(raw, pci, args.workload, arb.read(pci) if arb else {}))
                rows[pci] = rows.get(pci, 0) + 1
            time.sleep(delay)
    except Exception as exc:  # noqa: BLE001  a collector that died must not look like a complete measurement
        print(f"telemetry reader error: {exc}", flush=True)
        rc = 1
    finally:
        sink.close()
    for pci, n in sorted(failures.items()):
        print(f"chip pci:{pci}: {n} read(s) failed during the capture", flush=True)
    report["rows_per_chip"] = {str(k): v for k, v in sorted(rows.items())}
    report["read_failures"] = {str(k): v for k, v in sorted(failures.items())}
    if args.interval_ms > 0:
        report["counters_max"] = read_counters(bh_chips)
        restore = apply_interval(bh_chips, 0, clear_counters=False)
        report["restored"] = restore["applied"]
        if not restore["applied"]:
            print(
                f"::warning::telemetry interval restored on {restore['applied_chips']}/{len(bh_chips)} chip(s)",
                flush=True,
            )
    write_report(args.report, report)
    print(
        f"sampler stopped: {sum(rows.values())} rows from {len(rows)} chip(s)"
        + (" (reader error)" if rc else ""),
        flush=True,
    )
    return rc


def _die_with_parent() -> None:  # Linux: SIGKILL the sampler if the supervisor is killed
    try:
        import ctypes

        ctypes.CDLL("libc.so.6", use_errno=True).prctl(1, signal.SIGKILL)  # PR_SET_PDEATHSIG
    except (OSError, AttributeError):
        pass


def merge_report(total: dict, path: str) -> None:
    try:
        with open(path) as f:
            part = json.load(f)
        os.remove(path)
    except (OSError, ValueError):
        return
    total["chips_detected"] = max(total["chips_detected"], part.get("chips_detected", 0))
    for key in ("rows_per_chip", "read_failures"):
        for k, v in (part.get(key) or {}).items():
            total[key][k] = total[key].get(k, 0) + int(v)
    for k, v in (part.get("counters_max") or {}).items():
        total["counters_max"][k] = max(total["counters_max"].get(k, 0), int(v))
    if part.get("interval"):
        total["interval"] = part["interval"]
    if part.get("restored") is not None:
        total["restored"] = part["restored"]


def supervise(args: argparse.Namespace) -> int:
    """Run sample() in a child and restart it when the CSVs stop growing (a board reset hangs pyluwen reads)."""
    report = new_report()
    part_path = (args.report or f"{args.csv}_report.json") + ".part"
    cmd = [
        sys.executable,
        os.path.abspath(__file__),
        *sys.argv[1:],
        "--sampler",
        "--report",
        part_path,
    ]

    def csv_bytes() -> int:
        return sum(os.path.getsize(f) for f in glob.glob(f"{args.csv}_*.csv"))

    child = None
    last_rows_at = time.monotonic()
    gave_up = False
    last_rc = 0
    while True:
        if child is None:
            if STOP:
                break
            child = subprocess.Popen(cmd, preexec_fn=_die_with_parent)
            started = progressed = time.monotonic()
            size = csv_bytes()
        time.sleep(0.5)
        now = time.monotonic()
        cur = csv_bytes()
        if cur != size:
            size, progressed, last_rows_at = cur, now, now
        ret = child.poll()
        if STOP and ret is None:
            child.send_signal(signal.SIGINT)
            try:
                ret = child.wait(20)
            except subprocess.TimeoutExpired:
                child.kill()
                ret = child.wait()
        elif ret is None and now - started > START_GRACE_S and now - progressed > STALL_S:
            print(
                f"no telemetry written for {STALL_S} s (board reset?); restarting the sampler",
                flush=True,
            )
            child.kill()
            ret = child.wait()
        if ret is None:
            continue
        merge_report(report, part_path)
        last_rc = ret
        child = None
        if STOP:
            break
        if now - last_rows_at > GIVE_UP_S:
            print(f"no telemetry for {GIVE_UP_S} s; giving up", flush=True)
            gave_up = True
            break
        report["restarts"] += 1
        time.sleep(2)
    rc = 1 if gave_up or last_rc not in (0, -signal.SIGKILL) else 0
    write_report(args.report, report)
    rows = report["rows_per_chip"]
    print(
        f"stopped: {sum(rows.values())} rows from {len(rows)} chip(s), {report['restarts']} restart(s)"
        + (" (reader error)" if rc else ""),
        flush=True,
    )
    return rc


def main() -> int:
    args = parse_args()
    signal.signal(signal.SIGINT, _stop)
    signal.signal(signal.SIGTERM, _stop)
    if args.restore_interval:
        _, _, bh_chips = detect()
        report = new_report()
        restore = apply_interval(bh_chips, 0, clear_counters=False)
        report["interval"], report["restored"] = restore, restore["applied"]
        write_report(args.report, report)
        print(json.dumps(report["interval"], indent=1))
        return 0
    return sample(args) if args.sampler else supervise(args)


if __name__ == "__main__":
    sys.exit(main())
