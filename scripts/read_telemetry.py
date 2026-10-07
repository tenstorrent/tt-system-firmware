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


def get_telemetry(telem_dicts, workload: str = "") -> dict:
    results = []

    for map in telem_dicts:
        telem = {}

        # Timestamp
        telem["TIMESTAMP"] = time.ctime()

        # Workload label (free-form string supplied by the caller)
        telem["WORKLOAD"] = workload

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
    for i, device in enumerate(raw_devices):
        if not device.have_comms():
            print(f"Cannot communicate with device at pci:{i}")
        else:
            devices.append(device)

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
            for map in json_map:
                temp_dict = {}
                for key, value in map.items():
                    if value:
                        temp_dict[key.upper()] = hex(value)
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
