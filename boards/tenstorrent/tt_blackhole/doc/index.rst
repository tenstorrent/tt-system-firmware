.. _tt_blackhole:

Tenstorrent Blackhole PCI Express Cards
#######################################

Tenstorrent is proud to release firmware for Blackhole PCIe AI accelerator cards powered by the
`Zephyr Real-Time Operating System`_. Following our commitment to providing the best hardware,
tools, and developer experience to our customers, partners, and the Open Source Community, we
believe that embracing the Open Source development model is key to empowering everyone
to build the next generation of AI applications with the best silicon available.

Of possibly historical note, we believe Blackhole to be the world's first consumer-focused
PCIe AI accelerator card that is also a
`Zephyr-based product <https://www.zephyrproject.org/products-running-zephyr/>`_.

Own your Silicon Future.

Product Family
**************

The Blackhole product family officially supported by this repository include the following
revisions:

* Blackhole P100 (aka "Scrappy" - an internal development board)
* Blackhole P100A
* Blackhole P150A
* Blackhole P150B
* Blackhole P150C
* Blackhole P300A
* Blackhole P300B
* Blackhole P300C
* Blackhole Galaxy

Additional boards in the Blackhole product family may be added in the future as they are
announced.

Overview
********

The Blackhole SoC is Tenstorrent's third-generation AI accelerator SoC, packing up to

* 140 Tensix™ cores
* 8 GDDR6 channels for 3.5 Tb/s of throughput
* 32 GB of on-board GDDR6 memory
* 4 QSFP-DD ports at 800 Gb/s
* 16 PCIe 5.0 lanes for 500 Gb/s of throughput

.. container:: twocol

   .. container:: leftside

      .. figure:: img/blackhole.webp
         :align: center
         :alt: Tenstorrent Blackhole SoC

         Tenstorrent Blackhole Block Diagram

   .. container:: rightside

      .. figure:: img/blackhole-p150a-pcb.webp
         :align: center
         :alt: Tenstorrent Blackhole SoC

         Tenstorrent Blackhole P150A PCB

See the `Blackhole Product Page`_ for additional details and specifications.

Blackhole firmware is built for two targets:

* System Management Controller (SMC): the ARC cluster in the Blackhole SoC
* Device Management Controller (DMC): an external ARM microcontroller

DMC firmware is mainly responsible for power-on, fan control, some telemetry, SMBus
communication, and other board-level management functions.

SMC firmware focuses on management of high-speed I/O (i.e PCIe, GDDR, and Ethernet), power
management, frequency scaling, thermal management, host communication over PCIe and other
chip-level functionality.

Hardware
********

Board Configurations
=====================

.. toctree::
   :maxdepth: 1

   _generated/tt_blackhole_configuration

Supported Features
==================

.. note::
   Some drivers not specific to Tenstorrent hardware may be located in the main
   `Zephyr git repository <https://github.com/zephyrproject/zephyr>`_.

.. note::
   Some components are in the process of being migrated to Zephyr's driver model, and are
   currently implemented in the ``lib/tenstorrent/bh_arc`` library.

Building
========

SMC firmware can be built using the standard Zephyr build system. For other apps, tests,
and samples, simply point the build system to the desired app directory.

.. zephyr-app-commands::
   :zephyr-app: <tt_zephyr_platforms>/app/smc
   :host-os: unix
   :board: tt_blackhole@p100a/tt_blackhole/smc
   :flash-args: -r tt_flash --force
   :goals: build flash
   :compact:

The ``tt-fw-terminal`` app is a terminal-emulator-like utility that can be used to view log messages
and interact with the Zephyr shell.

.. code-block:: shell

    # Build the TT firmware terminal utility (for board communication)
    make -j -C scripts/tooling OUTDIR=/tmp tt-fw-terminal

    # Reset the board and refresh PCIe connectivity
    tt-smi -r
    ./scripts/rescan-pcie.sh

    # Connect to the board console
    /tmp/tt-fw-terminal

Press ``Ctrl-x,a`` to exit the ``tt-fw-terminal`` app.

Debugging
=========

Tenstorrent has developed a custom adapter board to facilitate debugging firmware running on the
Blackhole SMC and DMC.

Details coming soon!

References
**********

.. _Blackhole Product Page:
   https://tenstorrent.com/hardware/blackhole

.. _Zephyr Real-Time Operating System:
   https://www.zephyrproject.org/
