Telemetry
=========

In Chip Management Firmware, telemetry reporting is done in the common file `telemetry.c </tt-system-firmware/doxygen/telemetry_8c.html>`_, with the per-SoC table and data collection in `telemetry_bh.c </tt-system-firmware/doxygen/telemetry__bh_8c.html>`_ (Blackhole) and `telemetry_gr.c </tt-system-firmware/doxygen/telemetry__gr_8c.html>`_ (Grendel). The versioned telemetry table contains a tag-to-offset mapping followed by the telemetry data. The table location and its discovery mechanism are SoC-specific.

See `telemetry table </tt-system-firmware/doxygen/group__telemetry__table.html>`_ for information.

The telemetry table is updated every 100ms by a Zephyr worker thread.

Procedure to Read Telemetry
---------------------------

1. Find ``telemetry_table`` by reading the telemetry-table scratch register for the SoC.
2. Check ``telemetry_table.entry_count`` to find out how many ``tag_table`` entries there are.
3. Read through all ``tag_table`` entries and keep the tag-offset mapping around. These will not change unless the firmware gets updated and the chip is reset.
4. To find specific telemetry entries:
   - Look up the offset in the tag-offset mapping.
   - Read 4 bytes from the telemetry data array at that offset.

Telemetry Discovery Registers
-----------------------------

.. list-table::
   :header-rows: 1
   :widths: 20 40 40

   * - SoC
     - Telemetry table
     - Telemetry data
   * - Blackhole
     - ``reset_unit.SCRATCH_RAM[13]`` (WH: ``ARC_RESET.NOC_NODEID_X_0``) contains the table address in ``tensix_sm`` (ARC) memory.
     - ``reset_unit.SCRATCH_RAM[12]`` (WH: ``ARC_RESET.NOC_NODEID_Y_0``) contains the data-array address.
   * - Grendel
     - ``telemetry_info`` at Cold Scratch 4 (``0xc0002810``) contains the complete ``telemetry_table`` address in RISC-V SMC SRAM.
     - The data array follows the tag table within ``telemetry_table`` in SMC SRAM.

Via SMBUS
~~~~~~~~~

(i.e., from DMC or Galaxy UBB CPLD)

1. Write the tag ID you wish to read to SMBUS address ``0x26``. This register is 8 bits wide—the SMBUS write should include a PEC.
2. The tag ID is persistent, so it can be programmed once, and the telemetry data can be read multiple times from address ``0x27``.
3. Read the telemetry tag data via SMBUS by issuing a block read to address ``0x27``. This register is 32 bits wide.

.. list-table::
   :header-rows: 1
   :widths: 20 20 60

   * - Register
     - Address
     - Usage
   * - TELEMETRY_TAG
     - 0x26
     - Write only. Write with telemetry tag value to select which telemetry field will be read when reading from TELEMETRY_DATA register.
   * - TELEMETRY_DATA
     - 0x27
     - Read only. Read to get the latest telemetry data for the tag programmed to the TELEMETRY_TAG register.

Via west attach
~~~~~~~~~~~~~~~

You can print ``telemetry_table.telemetry[<OFFSET>]`` in gdb, where ``<OFFSET>`` is the offset
for the tag in ``telemetry_table.tag_table``.

.. code-block:: shell

   west attach
   (gdb) print telemetry_table.tag_table
   (gdb) print telemetry_table.telemetry[7]
   $6 = 47

Tag IDs
-------

Tag IDs are permanent in the sense that they’ll never change meaning, but may not be present.

See `telemetry group </tt-system-firmware/doxygen//group__telemetry__tags.html>`_ for more details.
