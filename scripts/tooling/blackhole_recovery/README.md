# Blackhole PCIe Card Automated Recovery Script

This script helps simplify the recovery of blackhole PCIe cards, by
reprogramming their flash eeprom via the device management controller (DMC).

In order to use this script, you should have an SWD connection to the DMC,
usually made via a debug board and an ST-Link probe.

Usage:
```
# Download a recovery bundle or build one using prepare-recovery-bundle.py
python3 ./prepare-recovery-bundle.py <bundle_file> # Or download a bundle
python3 ./recover-blackhole.py <bundle_file> p100a --adapter-id B55B5A1A000000005833EF01 \
	--card-serial 0x431xxxxxxxx
Checking proto_txt_table for read_only...
read_only proto_txt_table is valid
read_only table has been written to a binary file /tmp/tmphamimip3/p100a/generated_proto_bins/P100A/read_only.bin

Flashing /tmp/tmphamimip3/p100a/tt_boot_fs_recovery.hex to ASIC 0...
[==================================================] 100%
Powering off device at /sys/bus/pci/devices/0000:01:00.0
Successfully flashed tt_boot_fs
```

The script will exit once the card is recovered.

The script currently should support `p100a`, `p150a`, `p150b`, and `p150c` cards, and the `galaxy_2` board.
