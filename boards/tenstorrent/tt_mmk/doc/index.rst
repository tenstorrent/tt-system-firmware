.. _tt_mmk:

TT MMK Board
############

Overview
********

The ``tt_mmk`` "board" is a simulation target intended to be run within
simulation/emulation environments. It represents the MMK (Mimir-Mimir-Keraunos)
board configuration, consisting of 1 Keraunos chiplet (the IO chiplet) and
2 Mimir chiplets (the memory chiplets) in the Grendel Family.

Hardware
********

The MMK board consists of:

- 2 Mimir chiplets, each with:
  - 4 instances of the RISC-V Rocket Chip (5-stage pipeline)
  - 1 MB of SRAM
  - SiFive CLINT interrupt controller
  - SiFive E300 watchdog timer
  - SiFive PLIC interrupt controller

- 1 Keraunos chiplet with:
  - 4 instances of the RISC-V Rocket Chip (5-stage pipeline)
  - 1 SEP cluster (bring-up target ``tt_mmk/tt_keraunos/sep``)
  - 1 MB of SRAM
  - SiFive CLINT interrupt controller
  - SiFive E300 watchdog timer
  - SiFive PLIC interrupt controller
  - 4x Designware APB UART
  - 6x Cadence I3C controllers

Supported Boards / SoC Targets
******************************

- ``tt_mmk/tt_keraunos/smc`` — Keraunos SMC
- ``tt_mmk/tt_keraunos/sep`` — Keraunos SEP
- ``tt_mmk/tt_mimir/smc`` — Mimir SMC on the MMK board

Programming
***********

Hello World::

   west build -b tt_mmk/tt_keraunos/sep samples/hello_world

SEP BL1 (unsecured bring-up; no SPI BUN1 reader yet)::

   west build -b tt_mmk/tt_keraunos/sep app/sep_bl1

Interconnects
*************

- **I3C1 Connection**: I3C1 of each Mimir chiplet is connected to I3C1 of the Keraunos chiplet.
  The Mimir chiplets operate as I3C targets, while the Keraunos chiplet operates as the I3C controller.

System Clock
************

The peripheral bus clock is configured to run at 1.5GHz. The CLINT timebase is
1MHz, yielding a rate of 1,000,000 ticks per second.
