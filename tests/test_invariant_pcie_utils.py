import pytest
import subprocess
import sys
import os
from unittest.mock import patch, MagicMock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'scripts'))

@pytest.mark.parametrize("rescan_path", [
    "/sys/bus/pci/rescan; rm -rf /tmp/pwned",  # exact exploit: command injection
    "/sys/bus/pci/rescan$(id>/tmp/injected)",   # boundary: subshell injection
    "/sys/bus/pci/rescan",                       # valid: legitimate path
])
def test_pcie_rescan_no_shell_injection(rescan_path):
    """Invariant: rescan_path must never be passed unsanitized to a shell=True subprocess call."""
    calls = []

    def mock_call(cmd, **kwargs):
        calls.append((cmd, kwargs))
        return 0

    with patch('subprocess.call', side_effect=mock_call):
        import pcie_utils
        try:
            pcie_utils.rescan_pcie(rescan_path)
        except Exception:
            pass

    for cmd, kwargs in calls:
        shell_used = kwargs.get('shell', False)
        if shell_used and isinstance(cmd, str):
            # If shell=True is used, the path must be sanitized (no shell metacharacters)
            dangerous_chars = [';', '&', '|', '$', '`', '>', '<', '!', '\n']
            injected = any(c in rescan_path for c in dangerous_chars)
            assert not injected, (
                f"shell=True used with unsanitized rescan_path containing injection payload: {rescan_path!r}. "
                f"Full command: {cmd!r}"
            )
        elif not shell_used:
            # shell=False is always safe — pass
            pass