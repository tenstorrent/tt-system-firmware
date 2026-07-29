.. _ttzp_runtime_telemetry:

Runtime Telemetry
=================

Chip Management Firmware reserves a region of memory for Metal runtime use.
The buffer is zero-initialized at boot, 32-byte aligned, and located in CSM
(the ``tensix_sm`` / ARC memory space, i.e. within ``NOC0 8-0``).

Scratch Registers
-----------------

As per the scratch register definitions in `status_reg.h </tt-system-firmware/doxygen/status__reg_8h.html>`_ (``SCRATCH_RAM[0..63]``):

.. list-table::
   :header-rows: 1
   :widths: 30 20 50

   * - Register
     - SCRATCH_RAM index
     - Description
   * - ``RUNTIME_TELEMETRY_ADDR``
     - 22 (``0x80030458``)
     - CSM address of the runtime telemetry buffer
   * - ``RUNTIME_TELEMETRY_SIZE``
     - 23 (``0x8003045C``)
     - Size of the runtime telemetry buffer in bytes

Both values are written once during init and do not change until the
next chip reset. For a given CMFW build, the size is fixed at compile time
and the address is fixed at link time.

Procedure to Access Runtime Telemetry
---------------------------------------

Via NOC Access to ARC Memory
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

1. Read ``reset_unit.SCRATCH_RAM[23]`` to obtain the buffer size in bytes.
   If the size is 0, assume the buffer is unavailable and stop. Older CMFW
   builds leave this register at 0.
2. Read ``reset_unit.SCRATCH_RAM[22]`` to obtain the tile-local address of the buffer.
3. Read or write the buffer via NOC using the address from step 2 and the
   size from step 1.

Firmware Details
----------------

- The buffer is placed in the ``.bss.runtime_telemetry`` linker section and is
  cleared with the rest of BSS during boot.
- Size is controlled by ``CONFIG_TT_BH_ARC_RUNTIME_TELEMETRY_SIZE`` (default
  1024 bytes). The value must be a positive multiple of 32.
- Address and size are published via ``SYS_INIT_APP`` in ``runtime_telemetry.c``.
