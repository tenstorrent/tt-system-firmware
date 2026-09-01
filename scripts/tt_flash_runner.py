# Copyright (c) 2025 Tenstorrent AI ULC
#
# SPDX-License-Identifier: Apache-2.0

from runners.core import RunnerCaps, ZephyrBinaryRunner
from pathlib import Path
import sys  # noqa: F401
from pcie_utils import rescan_pcie


class TTFlashRunner(ZephyrBinaryRunner):
    """Runner for the tt_flash command"""

    def __init__(
        self, cfg, force, allow_major_downgrades, update_boot_images, tt_flash
    ):
        super().__init__(cfg)
        self.force = force
        self.allow_major_downgrades = allow_major_downgrades
        self.update_boot_images = update_boot_images
        # If file is passed, flash that. Otherwise flash update.fwbundle
        # in build dir
        if cfg.file:
            self.file = Path(cfg.file)
        else:
            self.file = Path(cfg.build_dir).parent / "update.fwbundle"
        self.tt_flash = tt_flash

    @classmethod
    def name(cls):
        return "tt_flash"

    @classmethod
    def capabilities(cls):
        return RunnerCaps(commands={"flash"}, file=True)

    @classmethod
    def do_add_parser(cls, parser):
        parser.add_argument("--force", action="store_true", help="Force flash")
        parser.add_argument(
            "--allow-major-downgrades",
            action="store_true",
            help="Allow major downgrades",
        )
        parser.add_argument(
            "--update-boot-images",
            action="store_true",
            help=(
                "Force tt-flash to rewrite boot-critical images (cmfw, safeimg, "
                "safetail, failover) instead of using its content-compare "
                "skip_boot_critical shortcut."
            ),
        )
        parser.add_argument("--tt-flash", default="tt-flash", help="Path to tt-flash")

    @classmethod
    def do_create(cls, cfg, args):
        return cls(
            cfg,
            force=args.force,
            allow_major_downgrades=args.allow_major_downgrades,
            update_boot_images=args.update_boot_images,
            tt_flash=args.tt_flash,
        )

    def do_run(self, command, **kwargs):
        # Prior to flashing, check to make sure that device is present and we can
        # interface with it
        try:
            import pyluwen
        except ImportError:
            raise RuntimeError(
                "Error, required modules missing. Please run 'pip install pyluwen'"
            )

        try:
            chips = pyluwen.detect_chips()
            if len(chips) == 0:
                # This will be caught in the same except block
                raise RuntimeError("No chips detected")
            # Delete chips object before flashing, to avoid holding open FD across reset
            del chips
        except Exception as e:
            self.logger.warning(f"Failed to detect chips, rescanning PCIe bus: {e}")
            rescan_pcie()

        self.tt_flash = self.require(self.tt_flash)
        if not self.file.exists():
            raise RuntimeError(f"File {self.file} does not exist")
        cmd = [self.tt_flash, "flash", str(self.file)]
        if self.force:
            cmd.append("--force")
        if self.allow_major_downgrades:
            cmd.append("--allow-major-downgrades")
        if self.update_boot_images:
            cmd.append("--update-boot-images")
        self.logger.debug(f"Running {cmd}")
        self.check_call(cmd)
