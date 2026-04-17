# Copyright (c) 2025 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

"""
Helper functions to start an openocd RTT server and connect to it as a client.
"""

import subprocess
from pathlib import Path
import argparse
import logging
import shutil
import socket
import sys
import selectors
import time

logger = logging.getLogger(__name__)


def get_sdk_sysroot():
    """
    Gets the Zephyr SDK sysroot, which is used as a path base for
    openocd executable
    """
    # Execute CMake to locate SDK sysroot
    proc = subprocess.run(
        ["cmake", "-P", str(Path(__file__).parent / "find_zephyr_sdk.cmake")],
        capture_output=True,
        check=False,
    )
    if "SDK_INSTALL_DIR" in str(proc.stderr):
        return (
            Path(str(proc.stderr).split(":")[1][:-3])
            / "hosttools"
            / "sysroots"
            / "x86_64-pokysdk-linux"
        )
    return None


class OpenOCDServer:
    """
    Server to run openocd with rtt port
    """

    def __init__(self, openocd_exec, search_dir):
        """
        Sets up openocd class
        @param openocd_exec: Openocd executable to use
        @param search_dir: Search directory to use for openocd
        """
        self._openocd = openocd_exec
        self._search_dir = search_dir
        self._proc = None

    def launch_openocd_server(self, cfg, rtt_port, search_base, search_range):
        """
        Launches an openocd server, using the RTT port provided
        @param cfg openocd configuration file to use
        @param rtt_port RTT port to use
        @param search_base: base address to search for RTT block
        @param search_range: range to search for RTT block
        """
        openocd_cmd = [
            self._openocd,
            "-s",
            self._search_dir,
            "-s",
            str(Path(cfg).parent),
            "-f",
            cfg,
            "-c",
            "init",
            "-c",
            "halt",
            "-c",
            "reg pc",
            "-c",
            "resume",
            "-c",
            f'rtt setup {search_base} {search_range} "SEGGER RTT"',
            "-c",
            "rtt start",
            "-c",
            f"rtt server start {rtt_port} 0",
        ]
        logger.debug("OpenOCD command: %s", openocd_cmd)
        try:
            self._proc = subprocess.Popen(
                openocd_cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            while self._proc.poll() is None:
                line = self._proc.stderr.readline().decode().strip()
                logger.debug("Openocd Output: %s", line)
                if "pc (/32)" in line:
                    # Print openocd's read of the program counter
                    print(f"System PC:{line.split(':')[1]}")
                if f"Listening on port {rtt_port} for rtt connections" in line:
                    break
        except subprocess.CalledProcessError as e:
            logger.error("Failed to start OpenOCD server: %s", e)
            raise e

    def stop_openocd_server(self):
        """
        Stops the openocd server
        """
        logger.info("Stopping OpenOCD server")
        self._proc.terminate()
        self._proc.wait()


# Defaults used by the RTT helper
DEFAULT_RTT_PORT = 5555
DEFAULT_SYSROOT = get_sdk_sysroot() or Path(
    "/opt/zephyr-sdk/sysroots/x86_64-pokysdk-linux"
)


class RTTHelper:
    """
    Helper to start rtt. Takes arguments from user
    """

    def __init__(
        self,
        cfg,
        search_base,
        search_range,
        openocd=DEFAULT_SYSROOT / "usr" / "bin" / "openocd",
        rtt_port=DEFAULT_RTT_PORT,
        search_dir=DEFAULT_SYSROOT / "usr" / "share" / "openocd" / "scripts",
    ):
        """
        Inits the RTT helper with default values
        @param cfg: default config to use for openocd
        @param search_base: default search base for RTT
        @param search_range: default search range for RTT
        @param openocd: default value for openocd executable
        @param rtt_port: default port to use for rtt
        @param search_dir: default directory for search
        """
        self._cfg = cfg
        self._search_base = search_base
        self._search_range = search_range
        self._openocd = openocd
        self._rtt_port = rtt_port
        self._search_dir = search_dir

    def parse_args(self, input_args):
        """
        Parse arguments from user, and store them as object properties
        """
        parser = argparse.ArgumentParser(
            description="Helper to run RTT console", allow_abbrev=False
        )
        parser.add_argument(
            "-c",
            "--config",
            type=Path,
            default=self._cfg,
            help="OpenOCD config file to use",
        )
        parser.add_argument(
            "-a",
            "--search_base",
            type=int,
            default=self._search_base,
            help="Base address to search for RTT block",
        )
        parser.add_argument(
            "-r",
            "--search_range",
            type=int,
            default=self._search_range,
            help="Range to search for RTT block",
        )
        parser.add_argument(
            "-p",
            "--rtt_port",
            type=int,
            default=self._rtt_port,
            help="Port to use for RTT server",
        )
        parser.add_argument(
            "-o",
            "--openocd",
            default=self._openocd,
            help="Path to OpenOCD executable",
            metavar="FILE",
            type=Path,
        )
        parser.add_argument(
            "-s",
            "--search_dir",
            default=self._search_dir,
            help="Path to OpenOCD search directory",
        )
        parser.add_argument(
            "-d",
            "--debug",
            action="count",
            default=0,
            help="Increase debugging verbosity (pass -dd for debug, -d for info)",
        )
        parser.add_argument(
            "-n",
            "--non-blocking",
            action="store_true",
            help="Dump rtt data in non blocking mode, rather than running interactive server",
        )
        args = parser.parse_args(input_args)
        if args.debug == 2:
            logging.basicConfig(level=logging.DEBUG)
        elif args.debug == 1:
            logging.basicConfig(level=logging.INFO)
        else:
            logging.basicConfig(level=logging.WARNING)

        self._openocd = str(args.openocd)
        self._search_dir = str(args.search_dir)
        self._cfg = str(args.config)
        self._rtt_port = args.rtt_port
        self._search_base = args.search_base
        self._search_range = args.search_range
        self._interactive = not args.non_blocking

    def run_rtt_server(self):
        """
        Run the RTT server, or dump data if interactive mode is off
        """
        if self._interactive:
            self.run_rtt_server_interactive()
        else:
            self.dump_rtt_data()

    def run_rtt_server_interactive(self):
        """
        Run the RTT server with the parameters provided
        """
        openocd = OpenOCDServer(self._openocd, self._search_dir)
        openocd.launch_openocd_server(
            self._cfg,
            self._rtt_port,
            self._search_base,
            self._search_range,
        )
        print(f"OpenOCD server started on port {self._rtt_port}")
        if shutil.which("nc") is not None:
            # If a `nc` command is available, run it, as it will provide the
            # best support for CONFIG_SHELL_VT100_COMMANDS etc.
            client_cmd = ["nc", "localhost", str(self._rtt_port)]
            try:
                subprocess.run(client_cmd, check=True)
            except KeyboardInterrupt:
                # If the user interrupts the command, we need to stop the openocd server
                print("Caught SIGINT, exiting...")
                openocd.stop_openocd_server()
            except subprocess.CalledProcessError as e:
                print(f"Error running nc command: {e}")
                openocd.stop_openocd_server()
            return

        logger.warning("No nc command found, using pure python implementation")
        # Otherwise, use a pure python implementation. This will work well for logging,
        # but input is line based only.
        # Start a new socket connection
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect(("localhost", self._rtt_port))

        sel = selectors.DefaultSelector()
        sel.register(sys.stdin, selectors.EVENT_READ)
        sel.register(sock, selectors.EVENT_READ)
        try:
            while True:
                events = sel.select()
                for key, _ in events:
                    if key.fileobj == sys.stdin:
                        text = sys.stdin.readline()
                        if text:
                            sock.send(text.encode())

                    elif key.fileobj == sock:
                        resp = sock.recv(2048)
                        if resp:
                            print(resp.decode(), end="")
        except KeyboardInterrupt:
            # If the user interrupts the command, we need to stop the openocd server
            print("Caught SIGINT, exiting...")

        # Finally, shutdown the RTT server
        openocd.stop_openocd_server()

    def dump_rtt_data(self):
        """
        Start RTT server, dump data, and exit
        """
        openocd = OpenOCDServer(self._openocd, self._search_dir)
        openocd.launch_openocd_server(
            self._cfg,
            self._rtt_port,
            self._search_base,
            self._search_range,
        )
        print(f"OpenOCD server started on port {self._rtt_port}")
        # Start a new socket connection
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect(("localhost", self._rtt_port))
        sock.settimeout(0.5)
        timeout = time.time() + 60  # Hard timeout of 60 seconds to dump logs
        while time.time() < timeout:
            try:
                data = sock.recv(2048)
                print(data.decode())
            except socket.timeout:
                # No data was sent within 0.5 seconds, close connection
                break
            except UnicodeDecodeError:
                print("Invalid byte")
                break
        sock.close()
        openocd.stop_openocd_server()
