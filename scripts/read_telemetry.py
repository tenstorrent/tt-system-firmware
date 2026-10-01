# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

# Pretty much just rip out the telemetry code out of tt-smi and format it into a csv
import os
import csv
import time
import signal
import argparse
from datetime import datetime
import pandas as pd

import pyluwen

# optimize this
gddr_controller_temperature_map = {
    0: "GDDR01_TEMP",
    1: "GDDR01_TEMP",
    2: "GDDR23_TEMP",
    3: "GDDR23_TEMP",
    4: "GDDR45_TEMP",
    5: "GDDR45_TEMP",
    6: "GDDR67_TEMP",
    7: "GDDR67_TEMP",
}

interrupt_flag = False


def handle_interrupt(signum, frame):
    global interrupt_flag
    interrupt_flag = True


signal.signal(signal.SIGINT, handle_interrupt)


def dict_from_public_attrs(obj) -> dict:
    all_attrs = obj.__dir__()
    public = [attr for attr in all_attrs if not attr.startswith("_")]
    ret = {}
    for attr in public:
        ret[attr] = getattr(obj, attr)
    return ret


def convert_signed_16_16_to_float(value):
    return (value >> 16) + (value & 0xFFFF) / 65536.0


def extract_bottom_gddr(value, controller_id):
    if controller_id % 2 == 0:
        bottom = value & 0xFF
    else:
        bottom = (value >> 16) & 0xFF

    return bottom


def extract_top_gddr(value, controller_id):
    if controller_id % 2 == 0:
        top = (value >> 8) & 0xFF
    else:
        top = (value >> 24) & 0xFF

    return top


# Telemetry tag ids from tt-system-firmware include/tenstorrent/telemetry_tags.h (the ids, not the table positions).
# AICLK_ARB_MAX = arb_max_freq | (arbiter << 16); arbiter 0 fmax, 1 tdp, 2 fast_tdc, 3 tdc, 4 thm, 5 board_power,
# 6 voltage, 7 gddr_thm, 8 doppler_slow, 9 doppler_critical, 10 host_fmax.
ARB_TAGS = {"AICLK_ARB_MIN": 65, "AICLK_ARB_MAX": 66, "KERNEL_THROTTLER": 75, "UPDATE_TELEM_SPEED": 5}  # tag 5 = firmware telemetry refresh interval, ms


class ArbReader:
    """Reads the telemetry entries pyluwen's struct does not expose (AICLK arbiters, kernel throttler) straight from the
    firmware telemetry table over the same AXI path pyluwen itself uses: table address in ARC SCRATCH_RAM[13], then
    [version][entry_count][entries: tag | offset<<16][data...]. Read-only; no device init. (tt-umd's TTDevice init was
    used before and woke the chips: Galaxy idle went from 13 W to 26 W per chip while the reader ran.)"""

    def __init__(self, bh_chips):
        self.addrs = {}   # pci index -> {tag name: data address}
        self.chips = {}
        for pci, chip in bh_chips.items():
            try:
                scratch13 = chip.axi_translate("arc_ss.reset_unit.SCRATCH_RAM[13]").addr
                base = chip.axi_read32(scratch13)
                if not (0x10000000 <= base <= 0x1007FFFF):
                    raise RuntimeError(f"telemetry table address {base:#x} outside CSM")
                entry_count = chip.axi_read32(base + 4)
                if not (0 < entry_count < 1024):
                    raise RuntimeError(f"implausible entry count {entry_count}")
                data_base = base + 8 + 4 * entry_count
                found = {}
                for i in range(entry_count):
                    entry = chip.axi_read32(base + 8 + 4 * i)
                    tag, offset = entry & 0xFFFF, (entry >> 16) & 0xFFFF
                    for name, want in ARB_TAGS.items():
                        if tag == want:
                            found[name] = data_base + 4 * offset
                self.addrs[pci] = found
                self.chips[pci] = chip
            except Exception as exc:
                print(f"arbiter tags unavailable on pci:{pci} ({exc})", flush=True)
        print(f"AICLK arbiter tags via pyluwen AXI on {len(self.addrs)} device(s): {sorted({n for a in self.addrs.values() for n in a})}", flush=True)

    def read(self, pci):
        addrs = self.addrs.get(pci)
        if not addrs:
            return {}
        chip = self.chips[pci]
        out = {}
        try:
            for name, addr in addrs.items():
                out[name] = int(chip.axi_read32(addr))
        except Exception as exc:
            print(f"arbiter read failed on pci:{pci}; disabled for this device ({exc})", flush=True)
            self.addrs[pci] = {}
            return {}
        return out


ARB_READER = None


def get_telemetry(telem_dicts, workload: str = "") -> dict:
    results = []

    for list_index, map in enumerate(telem_dicts):
        telem = {}
        pci_index = int(map.get("_PCI", hex(list_index)), 16)

        # Timestamp
        telem["TIMESTAMP"] = datetime.now().isoformat(sep=" ", timespec="milliseconds")  # ms resolution; ctime() only had seconds

        # Workload label (free-form string supplied by the caller)
        telem["WORKLOAD"] = workload

        # Position in the detected-device list: on Galaxy every chip reports the same
        # BOARD_ID, so this is what tells the 32 chips apart in the shared CSV.
        telem["PCI_INDEX"] = pci_index

        # Board id
        telem["BOARD_ID"] = (
            map["BOARD_ID"]
            if "BOARD_ID" in map.keys() and map["BOARD_ID"] is not None
            else -1
        )

        # Vcore voltage
        telem["VCORE"] = (
            int(map["VCORE"], 16) / 1000
            if "VCORE" in map.keys() and map["VCORE"] is not None
            else -1
        )

        # Current
        telem["TDC"] = (
            int(map["TDC"], 16) & 0xFFFF
            if "TDC" in map.keys() and map["TDC"] is not None
            else -1
        )

        # Power
        telem["TDP"] = (
            int(map["TDP"], 16) & 0xFFFF
            if "TDP" in map.keys() and map["TDP"] is not None
            else -1
        )

        telem["INPUT_POWER"] = (
            int(map["INPUT_POWER"], 16) & 0xFFFF
            if "INPUT_POWER" in map.keys() and map["INPUT_POWER"] is not None
            else -1
        )

        # Asic temperature
        telem["ASIC_TEMP"] = (
            convert_signed_16_16_to_float(int(map["ASIC_TEMPERATURE"], 16))
            if "ASIC_TEMPERATURE" in map.keys() and map["ASIC_TEMPERATURE"] is not None
            else -1
        )

        telem["AICLK"] = (
            int(map["AICLK"], 16) & 0xFFFF
            if "AICLK" in map.keys() and map["AICLK"] is not None
            else -1
        )

        # ---- health / status tags (all from the same pyluwen struct; 0 when the firmware does not populate them)
        def raw(name, default=0):
            v = map.get(name)
            if v is None:
                return default
            v = int(v, 16)
            return default if v == 0xFFFFFFFF else v  # all-ones = "not available" on this board (e.g. FAN_RPM on Galaxy)

        telem["THERM_TRIP_COUNT"] = raw("THERM_TRIP_COUNT") & 0xFFFF
        telem["TIMER_HEARTBEAT"] = raw("TIMER_HEARTBEAT")
        telem["ETH_LIVE_STATUS"] = raw("ETH_STATUS0")  # link status << 16 | heartbeat status
        for pair in ("01", "23", "45", "67"):
            telem[f"GDDR{pair}_CORR_ERRS"] = raw(f"GDDR{pair}_CORR_ERRS")  # bytes: rd_lo, wr_lo, rd_hi, wr_hi
        telem["GDDR_UNCORR_ERRS"] = raw("GDDR_UNCORR_ERRS")  # bit per controller rd/wr
        telem["VREG_TEMP"] = convert_signed_16_16_to_float(raw("VREG_TEMPERATURE")) if map.get("VREG_TEMPERATURE") else -1
        telem["BOARD_TEMP"] = convert_signed_16_16_to_float(raw("BOARD_TEMPERATURE")) if map.get("BOARD_TEMPERATURE") else -1
        telem["FAN_RPM"] = raw("FAN_RPM", -1)
        telem["TDP_LIMIT_MAX"] = raw("TDP_LIMIT_MAX", -1)
        telem["TDC_LIMIT_MAX"] = raw("TDC_LIMIT_MAX", -1)
        telem["AICLK_LIMIT_MAX"] = raw("AICLK_LIMIT_MAX", -1)
        telem["THM_LIMIT_THROTTLE"] = raw("THM_LIMIT_THROTTLE", -1)
        # AICLK arbiter tags are not in the pyluwen struct; ARB_READER fills them through tt-umd when available.
        arb = ARB_READER.read(pci_index) if ARB_READER else {}
        telem["AICLK_ARB_MAX_ID"] = arb.get("AICLK_ARB_MAX", -1) >> 16 if arb.get("AICLK_ARB_MAX", -1) >= 0 else -1
        telem["AICLK_ARB_MAX_MHZ"] = arb.get("AICLK_ARB_MAX", -1) & 0xFFFF if arb.get("AICLK_ARB_MAX", -1) >= 0 else -1
        telem["AICLK_ARB_MIN_ID"] = arb.get("AICLK_ARB_MIN", -1) >> 16 if arb.get("AICLK_ARB_MIN", -1) >= 0 else -1
        telem["AICLK_ARB_MIN_MHZ"] = arb.get("AICLK_ARB_MIN", -1) & 0xFFFF if arb.get("AICLK_ARB_MIN", -1) >= 0 else -1
        telem["KERNEL_THROTTLER"] = arb.get("KERNEL_THROTTLER", -1)
        telem["UPDATE_TELEM_SPEED"] = arb.get("UPDATE_TELEM_SPEED", -1)  # ms between firmware telemetry refreshes (100 default; Power CI telem_interval_ms)

        for key, value in gddr_controller_temperature_map.items():
            telem[f"GDDR{key}_TEMP_BOTTOM"] = (
                extract_bottom_gddr(int(map[value], 16), key)
                if value in map.keys() and map[value] is not None
                else -1
            )

            telem[f"GDDR{key}_TEMP_TOP"] = (
                extract_top_gddr(int(map[value], 16), key)
                if value in map.keys() and map[value] is not None
                else -1
            )
        results.append(telem)
    return results


_CSV_WRITERS: dict = {}  # csv path -> (file handle, DictWriter); header written once, rows appended


def append_rows(telems, csv_prefix: str, csv_name: str | None = None) -> set:
    """Append each telemetry dict to <csv_prefix>_<BOARD_ID>.csv (or csv_name). Returns the files written."""
    written = set()
    for telem in telems:
        name = csv_name or f"{csv_prefix}_{telem['BOARD_ID']}.csv"
        entry = _CSV_WRITERS.get(name)
        if entry is None:
            fieldnames = list(telem.keys())
            if os.path.exists(name) and os.path.getsize(name) > 0:
                with open(name, newline="") as f:
                    header = next(csv.reader(f), None)
                fieldnames = header or fieldnames
                fh = open(name, "a", newline="")
                writer = csv.DictWriter(fh, fieldnames=fieldnames, extrasaction="ignore", restval=-1)
            else:
                fh = open(name, "w", newline="")
                writer = csv.DictWriter(fh, fieldnames=fieldnames, extrasaction="ignore", restval=-1)
                writer.writeheader()
            entry = _CSV_WRITERS[name] = (fh, writer)
        fh, writer = entry
        writer.writerow(telem)
        fh.flush()  # the CI action copies the CSVs right after SIGINT; nothing may sit in a buffer
        written.add(name)
    return written


def close_csvs() -> None:
    for fh, _ in _CSV_WRITERS.values():
        try:
            fh.close()
        except Exception:
            pass
    _CSV_WRITERS.clear()


def parse_args():
    parser = argparse.ArgumentParser(
        description="Read telemetry from Tenstorrent chips", allow_abbrev=False
    )
    parser.add_argument("--csv", type=str, default="telemetry", help="Output file")
    parser.add_argument(
        "--delay", type=float, default=0.1, help="Delay between telemetry reads"
    )
    parser.add_argument(
        "--pad", action="store_true", help="Pad the output csv with -1 when read fails"
    )
    parser.add_argument(
        "--no-umd", action="store_true", help="Do not read the AICLK arbiter / kernel-throttler tags from the telemetry table"
    )
    parser.add_argument("--vf", action="store_true", help="Run in vf sweep mode")
    parser.add_argument(
        "--workload", type=str, default="", help="Workload label stamped onto every row"
    )
    return parser.parse_args()


if __name__ == "__main__":
    args = parse_args()

    # Detecting chips
    raw_devices = pyluwen.detect_chips_fallible()
    devices = []
    pci_ids = []
    for i, device in enumerate(raw_devices):
        if not device.have_comms():
            print(f"Cannot communicate with device at pci:{i}")
        else:
            devices.append(device)
            pci_ids.append(i)
    if not args.no_umd:
        bh_chips = {}
        for pci, device in zip(pci_ids, devices):
            try:
                bh = device.force_upgrade().as_bh()
                if bh is not None:
                    bh_chips[pci] = bh
            except Exception:
                pass
        ARB_READER = ArbReader(bh_chips)

    print("Starting telemetry collection")
    try:
        while True:
            # Get telemetry readings
            telem_structs = []
            for pl_chip in devices:
                try:
                    telem_structs.append(
                        pl_chip.force_upgrade().as_bh().get_telemetry()
                    )
                except Exception:
                    pass

            json_map = [
                dict_from_public_attrs(telem_struct) for telem_struct in telem_structs
            ]
            telem_dicts = []
            for pci, map in zip(pci_ids, json_map):
                temp_dict = {"_PCI": hex(pci)}
                for key, value in map.items():
                    if value is not None and not isinstance(value, (str, bool)):
                        try:
                            temp_dict[key.upper()] = hex(int(value))
                        except (TypeError, ValueError):
                            pass
                telem_dicts.append(temp_dict)
            telems = get_telemetry(telem_dicts, workload=args.workload)

            # Append one row per chip. (The previous version re-read, concatenated and rewrote the whole CSV with
            # pandas on every sample: O(file size) per loop, so a Galaxy capture slowed from ~4 Hz to one row every
            # 22 s over a 35 h run. Appending keeps the loop time flat.)
            written = append_rows(telems, args.csv)
            if args.pad:
                for csv_name in [f for f in os.listdir() if f.startswith(args.csv) and f.endswith(".csv") and f not in written]:
                    append_rows([{"TIMESTAMP": datetime.now().isoformat(sep=" ", timespec="milliseconds"), "WORKLOAD": args.workload,
                                  "PCI_INDEX": -1, "BOARD_ID": -1, "VCORE": -1, "TDC": -1, "TDP": -1, "INPUT_POWER": -1,
                                  "ASIC_TEMP": -1, "AICLK": -1}], args.csv, csv_name=csv_name)

            if interrupt_flag:
                print("\nKeyboard interrupt detected")
                close_csvs()
                if args.vf:
                    if not os.path.exists("vf_pending_upload"):
                        os.makedirs("vf_pending_upload")
                    if not os.path.exists("vf_archived_logs"):
                        os.makedirs("vf_archived_logs")

                    # If logs do not exist in vf_pending_upload directory, move them there
                    matching_csvs = [f for f in os.listdir() if f.startswith(args.csv)]
                    for csv in matching_csvs:
                        if not os.path.exists(f"vf_pending_upload/{csv}"):
                            os.rename(csv, f"vf_pending_upload/{csv}")
                        else:
                            # Otherwise move them to vf_archived_logs directory
                            os.rename(csv, f"vf_archived_logs/{csv}")

                break

            # Pause? Kind of glitches otherwise
            time.sleep(args.delay)

    except Exception as e:
        print(f"Exception caught: {e}")

    close_csvs()
    print("Stopping telemetry collection")
