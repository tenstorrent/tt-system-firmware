#!/usr/bin/env python3
#
# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

"""
TT System Firmware Compliance Checker

This script imports and runs the Zephyr compliance checker for tt-system-firmware.
"""

import re
import subprocess
import sys
from pathlib import Path

TT_SYS_FW_ROOT = Path(__file__).parent.absolute()
ZEPHYR_ROOT = TT_SYS_FW_ROOT.parents[1] / "zephyr"
zephyr_scripts_dir = ZEPHYR_ROOT / "scripts" / "ci"

sys.path.insert(0, str(zephyr_scripts_dir))
import check_compliance  # noqa: E402

# Extend the upstream Kconfig allowlist with TT-specific symbols. Currently nothing.
_ZEPHYR_CI = Path(check_compliance.__file__).parent
check_compliance.KconfigCheck.UNDEF_KCONFIG_ALLOWLIST = (
    check_compliance.get_set_from_file(str(_ZEPHYR_CI / "undef_kconfig_allowlist.txt"))
)

# Extend the sysbuild Kconfig allowlist with TT-specific symbols. Currently nothing.
check_compliance.SysbuildKconfigCheck.UNDEF_KCONFIG_ALLOWLIST = (
    check_compliance.SysbuildKconfigCheck.UNDEF_KCONFIG_ALLOWLIST
)

# Upstream module_kconfigs greps only ZEPHYR_BASE/tests+samples+modules for
# symbol definitions, so tt-system-firmware's own Kconfig trees are invisible.
# Wrap the method to also grep GIT_TOP (set at runtime by check_compliance).
_orig_module_kconfigs = check_compliance.KconfigCheck.module_kconfigs


def _tt_module_kconfigs(self, regex):
    result = _orig_module_kconfigs(self, regex)
    tt_output = check_compliance.git(
        "grep",
        "-I",
        "-h",
        "--perl-regexp",
        regex,
        "--",
        ":tests",
        ":samples",
        ":modules",
        cwd=check_compliance.GIT_TOP,
        ignore_non_zero=True,
    )

    if tt_output:
        result += tt_output + "\n"
    return result


check_compliance.KconfigCheck.module_kconfigs = _tt_module_kconfigs


class CheckpatchFile(check_compliance.ComplianceTest):
    """
    Runs checkpatch on full files that were changed in the commit range
    """

    name = "CheckpatchFile"
    doc = "See https://docs.zephyrproject.org/latest/contribute/guidelines.html#coding-style for more details."

    def run(self):
        # Get changed C/C++ files
        try:
            files = check_compliance.git(
                "diff",
                "--name-only",
                "--diff-filter=AMR",
                check_compliance.COMMIT_RANGE,
            ).splitlines()
            c_files = [
                f
                for f in files
                if f.endswith((".c", ".h", ".cpp", ".hpp", ".cc", ".hh"))
                and (check_compliance.GIT_TOP / f).exists()
            ]
        except Exception as e:
            self.failure(f"Failed to get changed files: {e}")
            return

        if not c_files:
            return

        # Setup command
        cmd_base = [str(check_compliance.ZEPHYR_BASE / "scripts" / "checkpatch.pl")]

        # Check each file
        for fname in c_files:
            try:
                subprocess.run(
                    cmd_base + ["--no-tree", "-f", fname],
                    check=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    cwd=check_compliance.GIT_TOP,
                )
            except subprocess.CalledProcessError as ex:
                output = ex.output.decode("utf-8")
                regex = (
                    r"^\s*\S+:(\d+):\s*(ERROR|WARNING):(.+?):(.+)(?:\n|\r\n?)+"
                    r"^\s*#(\d+):\s*FILE:\s*(.+):(\d+):"
                )

                matches = re.findall(regex, output, re.MULTILINE)

                for m in matches:
                    self.fmtd_failure(
                        m[1].lower(), m[2], m[5], m[6], col=None, desc=m[3]
                    )

                # If the regex has not matched add the whole output as a failure
                if len(matches) == 0:
                    self.failure(output)


# Patch all ComplianceTest inheritors to convert warnings to errors
def err_fmtd(original, always_error):
    return lambda self, severity, *args, **kwargs: original(
        self,
        "error" if severity == "warning" or always_error else severity,
        *args,
        **kwargs,
    )


# List of compliance checks to exclude
exclude_checks = [
    "BinaryFiles",
    "KconfigBasicNoModules",
    "SysbuildKconfigBasic",
    "LicenseAndCopyrightCheck",
]
# items in this list will return the severity based on zephyr upstream
default_fmtd = ["Checkpatch", "CheckpatchFile"]
# items in this list will always return a severity of error
all_errors = ["ClangFormat"]

# all other items will convert warnings to errors only
for cls in check_compliance.inheritors(check_compliance.ComplianceTest):
    if cls.name not in default_fmtd:
        cls.fmtd_failure = err_fmtd(cls.fmtd_failure, cls.name in all_errors)

if __name__ == "__main__":
    for exclude in exclude_checks:
        sys.argv.extend(["-e", exclude])
    check_compliance.main()
