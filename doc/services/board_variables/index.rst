Board Variables
===============

A firmware image is compatible with a board by virtue of two things: the board
type, which selects the image out of a firmware bundle, and the board
variables, which are hardware attributes that vary between boards of the same
type.

Board variables are numbered from 0, independently of board type and ASIC, in
:file:`include/tenstorrent/board_variables.h`. A number is never reused. The
first is the SPI flash JEDEC ID, which matters because one image can drive
several second-source flash parts while an older image speaks only the Micron
MT25QU512ABB command set. Writing an MT25-only image to a board carrying
another part leaves an image that cannot read itself back at the next boot.

Interlock flow
--------------
1. Each board image lists which board variables it knows and what values it
   supports for each. This is included in the fwbundle.
2. tt-flash reads fwbundle info and checks the variables. If any check fails,
   it reports an error.
3. tt-flash requests flash unlock and includes the list of variables it
   checked. FW confirms that all required variables have been checked,
   and fails unlock if any were not.

.. mermaid::

   sequenceDiagram
       autonumber
       participant TF as tt-flash
       participant FB as fwbundle
       participant TM as SMC telemetry
       participant FW as SMC firmware

       TF->>TM: read board ID
       TM-->>TF: board type and revision
       TF->>FB: read BOARD-REV/compat-variables.json
       FB-->>TF: the variables this image constrains,<br/>where to read each one, and what it permits

       loop each constrained variable
           TF->>TM: read the variable's telemetry tag
           alt tag reported
               TM-->>TF: value
           else tag absent
               TM-->>TF: not reported
               Note right of TF: firmware older than the variable, so this is<br/>an "original design" board. Variable left out of the checked list, but not rejected.
           end
       end

       alt a value is outside what the image supports
           Note right of TF: Stop, naming the variable and<br/>what the image supports.
       else every value read is supported
           TF->>FW: FLASH_UNLOCK, with the bitmap of<br/>variables it verified
           Note left of FW: required = the variables that matter on<br/>this board.
           alt a required variable is missing from the bitmap
               FW-->>TF: error, plus the required bitmap
               Note right of TF: name the variables it must check.<br/>Nothing is written
           else every required variable is verified
               FW-->>TF: ok, plus the required bitmap
               Note left of FW: flash unlocked, ready to write
               TF->>FW: WRITE_EEPROM
           end
       end

The FW bundle
-------------

Each image in a firmware bundle carries a ``compat-variables.json`` beside its
image, describing the hardware it is compatible with: the variables it
constrains, how to read each one from the board, and the values it permits.
Values are unsigned 32-bit integers, written as a JSON integer or a ``"0x..."``
string.

.. code-block:: json

   {
     "version": 1,
     "variables": [
       {
         "name": "SPI EEPROM",
         "number": 0,
         "formatter": "hex",
         "source": { "type": "telemetry", "tag": 80 },
         "constraints": [ { "in": ["0x20bb20", "0xc2253a", "0xc8631a", "0xef6020"] } ]
       }
     ]
   }

``number``
   The board variable number. This is the bit index in the unlock message.

``name``
   A free-form display string, printed verbatim in messages. Not an identifier.

``formatter``
   Optional. Names how to render the value and the constraint values in
   messages. A consumer renders as hex if this is absent *or* if it names a
   formatter the consumer does not recognize, so a new formatter never breaks
   an older tool. Defined so far: ``hex``, ``semver``.

``source``
   Optional. How to read the value from the board; the only method defined so
   far is ``{"type": "telemetry", "tag": <int>}``. Unlike ``formatter``, an
   unrecognized ``source`` fails closed: the consumer must treat the variable
   as unverified rather than guess, because it affects correctness rather than
   presentation.

``constraints``
   A list of comparisons, every one of which must hold. Each entry names
   exactly one of ``eq``, ``ne``, ``lt``, ``le``, ``gt``, ``ge``, ``in``,
   ``not_in``, so an operator may appear more than once. An empty list places
   no restriction.

The file is generated at build time from the devicetree by
``tt_boot_fs.py generate_compat_variables``, so the flash mux candidate list
stays the single source of truth, and validated against
:file:`scripts/schemas/compat-variables-schema.json`.

The generator currently only supports SPI EEPROM JEDEC ID. It scans devicetree
files and checks the jedec-id of SPI entries. It has special-case code to
comprehend the flash_mux device and the standard SPI generic fallback.

The tt-flash checks
-------------------

After identifying the board ID & board version, tt-flash will check for
compat-variables.json in the board subdirectory. It performs the checks
as documented above. Currently it understands how to use Blackhole-style
telemetry to get a 32-bit value for a variable. If any check fails, it
refuses to load the firmware with an error message.

There is an important exception: If a board variable's telemetry value is not
present, tt-flash will attempt to unlock and flash without this variable in
the list of checked variables. This addresses the case of an original design
board and old firmware from before the variable was introduced. We assume that
original designs will always be supported.

The firmware-side flash unlock
------------------------------

The running firmware is the final authority on which board variables must be
checked. It refuses to unlock the flash for a tool that does not confirm that
it checked every required variable.

``TT_SMC_MSG_FLASH_UNLOCK`` carries a bitmap of the board variables the host
verified against the image it is about to write. An error is reported if any
required variable is not listed. Otherwise the flash is unlocked.

The unlock persists until ``TT_SMC_MSG_FLASH_LOCK`` or until a flash unlock
message with ``num_variable_words != 0``. There is a special exception to
ignore redundant unlocks with ``num_variable_words == 0`` because luwen's
``spi_write`` unlocks and relocks each time it is called.
