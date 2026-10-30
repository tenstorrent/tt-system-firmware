# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0
"""Poll Galaxy system power in-band over IPMI and write a CSV.

Reads the BMC's Power_* sensors (Power_PSU0..3, Power_UBB0..3 and their ASICs, Power_MB_HSC, Power_CPU,
Power_Memory, Power_FAN, Power_Total) with `ipmitool sdr list` through /dev/ipmi0: no BMC address or password,
but root (the CI container runs as root with --device /dev/ipmi0). Stops cleanly on SIGINT/SIGTERM.

    python3 scripts/poll_ipmi_power.py --out ipmi_power.csv --interval 2

Output columns: Timestamp (ISO, UTC) followed by the sensor names, one row per poll. Standard library only.
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
_CHILD: subprocess.Popen | None = None  # the ipmitool call in flight, so a stop signal ends it too


def _stop(signum, frame):  # noqa: ARG001
    global STOP
    STOP = True
    if _CHILD is not None and _CHILD.poll() is None:
        _CHILD.terminate()


def parse_rows(out: str, prefix: str) -> dict:
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


def sdr_list(timeout: float) -> str:
    global _CHILD
    if STOP:
        return ""
    try:
        _CHILD = subprocess.Popen(
            ["ipmitool", "sdr", "list"],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
        )
        try:
            out, _ = _CHILD.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            _CHILD.kill()
            _CHILD.communicate()
            raise
        return out
    except (subprocess.TimeoutExpired, FileNotFoundError) as exc:
        print(f"ipmitool failed: {exc}", file=sys.stderr, flush=True)
        return ""
    finally:
        _CHILD = None


def main() -> int:
    ap = argparse.ArgumentParser(allow_abbrev=False, description="Poll IPMI Power_* sensors to CSV")
    ap.add_argument("--out", default="ipmi_power.csv")
    ap.add_argument(
        "--interval",
        type=float,
        default=2.0,
        help="seconds between polls (one SDR walk takes a few seconds on Galaxy)",
    )
    ap.add_argument("--prefix", default="Power_", help="sensor name prefix to keep")
    ap.add_argument("--timeout", type=float, default=30.0, help="ipmitool timeout per poll")
    args = ap.parse_args()
    signal.signal(signal.SIGINT, _stop)
    signal.signal(signal.SIGTERM, _stop)

    rows = 0
    with open(args.out, "w", newline="") as fh:
        writer = None
        while not STOP:
            t0 = time.monotonic()
            values = parse_rows(sdr_list(args.timeout), args.prefix)
            if values:
                if writer is None:
                    writer = csv.DictWriter(
                        fh, fieldnames=["Timestamp", *sorted(values)], extrasaction="ignore"
                    )
                    writer.writeheader()
                writer.writerow(
                    {
                        "Timestamp": dt.datetime.now(dt.UTC).isoformat(timespec="seconds"),
                        **values,
                    }
                )
                fh.flush()
                rows += 1
            elif rows == 0 and not STOP:
                print(
                    "no Power_* sensors yet (is /dev/ipmi0 available and ipmitool installed?)",
                    file=sys.stderr,
                    flush=True,
                )
            end = t0 + args.interval
            while not STOP and time.monotonic() < end:
                time.sleep(0.2)
    print(f"wrote {rows} rows to {args.out}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
