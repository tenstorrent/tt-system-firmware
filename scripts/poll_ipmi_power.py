# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0
"""Poll Galaxy system power in-band over IPMI and write a CSV.

Reads the BMC's Power_* sensors (Power_PSU0..3, Power_UBB0..3, Power_MB_HSC,
Power_CPU, Power_Memory, Power_FAN, Power_Total) with `ipmitool sdr list` once, then `ipmitool sensor reading` on just those names
through /dev/ipmi0, so it needs no BMC address or password; it needs root (or
the ipmi group), which in CI it gets by running inside a container with
`--device /dev/ipmi0 --user root`. Stops cleanly on SIGINT/SIGTERM.

    python3 scripts/poll_ipmi_power.py --out ipmi_power.csv --interval 2

Output columns: Timestamp (ISO, UTC) followed by the sensor names, one row per poll.
Standard library only.
"""

import argparse
import csv
import datetime as dt
import re
import signal
import subprocess
import sys
import time

STOP = False


def _stop(signum, frame):  # noqa: ARG001
    global STOP
    STOP = True


def _parse_rows(out: str, prefix: str) -> dict:
    values = {}
    for line in out.splitlines():
        parts = [p.strip() for p in line.split("|")]
        if len(parts) < 2 or not parts[0].startswith(prefix):
            continue
        m = re.match(r"([-\d.]+)", parts[1])
        if m:
            try:
                values[parts[0]] = float(m.group(1))
            except ValueError:
                pass
    return values


def _run(args: list[str], timeout: float) -> str:
    try:
        return subprocess.run(args, capture_output=True, text=True, timeout=timeout, check=False).stdout
    except (subprocess.TimeoutExpired, FileNotFoundError) as exc:
        print(f"ipmitool failed: {exc}", file=sys.stderr, flush=True)
        return ""


def read_power(prefix: str, timeout: float, names: list[str] | None = None) -> dict:
    """`ipmitool sdr list` walks every sensor on the BMC (~10 s on a Galaxy). Once the Power_* names are
    known, `ipmitool sensor reading <names>` asks for just those (~1-2 s), so peaks are not smeared."""
    if names:
        values = _parse_rows(_run(["ipmitool", "sensor", "reading", *names], timeout), prefix)
        if len(values) >= max(1, len(names) // 2):
            return values
        print("sensor reading returned too few values; falling back to sdr list", file=sys.stderr, flush=True)
    return _parse_rows(_run(["ipmitool", "sdr", "list"], timeout), prefix)


def main() -> int:
    ap = argparse.ArgumentParser(description="Poll IPMI Power_* sensors to CSV")
    ap.add_argument("--out", default="ipmi_power.csv")
    ap.add_argument("--interval", type=float, default=2.0, help="seconds between polls (the first poll walks the whole SDR and takes ~10 s on Galaxy; later polls read only the Power_* sensors)")
    ap.add_argument("--prefix", default="Power_", help="sensor name prefix to keep")
    ap.add_argument("--timeout", type=float, default=30.0, help="ipmitool timeout per poll")
    args = ap.parse_args()

    signal.signal(signal.SIGINT, _stop)
    signal.signal(signal.SIGTERM, _stop)

    columns: list[str] = []
    rows = 0
    with open(args.out, "w", newline="") as fh:
        writer = None
        while not STOP:
            t0 = time.monotonic()
            values = read_power(args.prefix, args.timeout, columns or None)
            if values:
                if writer is None:
                    columns = sorted(values)
                    writer = csv.DictWriter(fh, fieldnames=["Timestamp", *columns], extrasaction="ignore")
                    writer.writeheader()
                writer.writerow({"Timestamp": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"), **values})
                fh.flush()
                rows += 1
            elif rows == 0:
                print("no Power_* sensors yet (is /dev/ipmi0 available and ipmitool installed?)", file=sys.stderr, flush=True)
            remaining = args.interval - (time.monotonic() - t0)
            if remaining > 0:
                end = time.monotonic() + remaining
                while not STOP and time.monotonic() < end:
                    time.sleep(0.2)
    print(f"wrote {rows} rows to {args.out}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
