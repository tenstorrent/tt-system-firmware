# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

# Pretty much just rip out the telemetry code out of tt-smi and format it into a csv
import os
import time
import signal
import argparse
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
# 6 voltage, 7 gddr_thm, 8 doppler_slow, 9 doppler_critical, 10 host_fmax. tt_umd.TelemetryTag members are preferred when present.
ARB_TAGS = {"AICLK_ARB_MIN": 65, "AICLK_ARB_MAX": 66, "KERNEL_THROTTLER": 75}


def _resolve_tags():
    try:
        from tt_umd import TelemetryTag
    except Exception:
        return dict(ARB_TAGS)
    out = {}
    for name, fallback in ARB_TAGS.items():
        member = getattr(TelemetryTag, name, None)
        out[name] = member if member is not None else fallback
    return out


class UmdArbReader:
    """Reads the telemetry entries pyluwen's struct does not expose (AICLK arbiters, kernel throttler) through tt-umd.
    Both tt-umd and pyluwen only read ARC memory, so they can share the devices. Disabled on the first failure."""

    def __init__(self, pci_ids):
        self.readers = {}
        self.failed = False
        self.tags = _resolve_tags()
        try:
            from tt_umd import TTDevice
        except Exception as exc:  # tt-umd not installed
            print(f"tt-umd not available; AICLK arbiter tags will not be recorded ({exc})", flush=True)
            self.failed = True
            return
        for pci in pci_ids:
            try:
                dev = TTDevice.create(pci)
                dev.init_tt_device()  # required before any reader call ("cannot be called before initializing TTDevice")
                rd = dev.get_arc_telemetry_reader()
                self.readers[pci] = (dev, rd, {n: rd.is_entry_available(t) for n, t in self.tags.items()})
            except Exception as exc:
                print(f"tt-umd could not open pci:{pci} for arbiter tags ({exc})", flush=True)
        print(f"AICLK arbiter tags via tt-umd on {len(self.readers)} device(s); tags {self.tags}", flush=True)

    def read(self, pci):
        if self.failed or pci not in self.readers:
            return {}
        _, rd, avail = self.readers[pci]
        out = {}
        try:
            for name, tag in self.tags.items():
                if avail.get(name):
                    out[name] = int(rd.read_entry(tag))
        except Exception as exc:
            print(f"tt-umd read failed on pci:{pci}; arbiter tags disabled ({exc})", flush=True)
            self.failed = True
            return {}
        return out


ARB_READER = None


def get_telemetry(telem_dicts, workload: str = "") -> dict:
    results = []

    for list_index, map in enumerate(telem_dicts):
        telem = {}
        pci_index = int(map.get("_PCI", hex(list_index)), 16)

        # Timestamp
        telem["TIMESTAMP"] = time.ctime()

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
        "--no-umd", action="store_true", help="Do not read AICLK arbiter / kernel-throttler tags through tt-umd"
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
        ARB_READER = UmdArbReader(pci_ids)

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

            # Format into a csv
            matching_csvs = [f for f in os.listdir() if f.startswith(args.csv)]
            for i, telem in enumerate(telems):
                new_row = pd.DataFrame([telem])
                csv_name = args.csv + "_" + str(telem["BOARD_ID"]) + ".csv"
                if os.path.exists(csv_name):
                    test_log = pd.read_csv(csv_name)
                    test_log = pd.concat([test_log, new_row], ignore_index=True)
                    if csv_name in matching_csvs:
                        matching_csvs.remove(csv_name)
                else:
                    test_log = new_row
                test_log.to_csv(csv_name, index=False)

            if args.pad:
                for csv in matching_csvs:
                    test_log = pd.read_csv(csv)
                    test_log = pd.concat(
                        [
                            test_log,
                            pd.DataFrame(
                                [
                                    {
                                        "TIMESTAMP": time.ctime(),
                                        "WORKLOAD": args.workload,
                                        "PCI_INDEX": -1,
                                        "BOARD_ID": -1,
                                        "VCORE": -1,
                                        "TDC": -1,
                                        "TDP": -1,
                                        "INPUT_POWER": -1,
                                        "ASIC_TEMP": -1,
                                        "AICLK": -1,
                                    }
                                ]
                            ),
                        ],
                        ignore_index=True,
                    )
                    test_log.to_csv(csv, index=False)

            if interrupt_flag:
                print("\nKeyboard interrupt detected")
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

    print("Stopping telemetry collection")
