.. _tt_boot_fs:

Tenstorrent Boot Filesystem
*****************************

The Tenstorrent Boot Filesystem (TT Boot FS) is a simple file system designed to
store and manage firmware images and related data in the bootloader and firmware
of Tenstorrent platforms. It provides a structured way to organize and access
binary files necessary for booting and operating the device.

FileSystem Structure
--------------------

The fundamental structure of the TT Boot FS consists of file descriptors, which
are defined by the :c:struct:`tt_boot_fs_fd` structure. Each file descriptor
contains metadata about a binary file stored in the boot filesystem, such as its
location in SPI flash memory, size, type, and various flags indicating its
properties.

The TT Boot FS supports multiple tables of file descriptors, allowing for
flexibility in managing different sets of binaries. Tables are defined as an
array of file descriptors, with the final entry having the ``invalid`` flag set.
The file descriptor tables may be located anywhere in flash memory. A fixed
header at :c:macro:`TT_BOOT_FS_HEADER_ADDR` describes the location and number of
these tables.  The header is defined by the :c:struct:`tt_boot_fs_header`
structure. Addresses of each file descriptor table are stored as an array of
32-bit integers immediately following the header.

File Types and Flags
====================

Each file descriptor includes a ``flags`` field, which is a bitmask that defines
various properties of the file. The field is represented by the
:c:struct:`tt_boot_fs_flags` structure. Key flags include:

* ``executable``: Indicates whether the file is executable. If set, the bootloader
  will attempt to execute the binary after loading it.
* ``invalid``: Marks the file as invalid. If set, the bootloader will assume this
  descriptor marks the end of the table.
* ``image_size``: Specifies the size of the binary image in bytes.

File Checksums
==============

Each file descriptor includes a checksum field, ``data_crc``, which is
calculated over the binary data. If a checksum mismatch is detected during
loading, the bootloader will treat the file as invalid.

Static and Mutable Tables
-------------------------

On Blackhole boards the boot filesystem is split into three tables:

* A ROM table at ``0x0`` holding boot-critical records that do not change
  after manufacturing (MCUBoot, the recovery image and its trailer, and board
  configuration data), plus the DMC ROM-update image (``bmfw``). Firmware
  before the BL2 DMC bootloader looks up ``bmfw`` only in the table at
  ``0x0``, so an in-field upgrade from 18.x depends on that descriptor
  staying here.
* A failover table at ``0x4000`` holding the failover MCUBoot record.
* A mutable table at ``0x170000`` holding records that are updated in the
  field with each firmware release, including the firmware configuration
  and MCUBoot update records.

The MCUBoot and recovery binaries referenced by the ROM and failover tables are
built from source along with the rest of the firmware, but a field update is
expected to leave them alone so that a power loss cannot corrupt the fallback
boot path. A flashing tool cannot decide whether they changed by comparing raw
bytes, because signing is not reproducible: two builds of the same source are
identical except for the trailing signature. It compares image content instead.
The recovery image is signed, so its identity is the SHA-256 and key hash
descriptors that ``imgtool`` embeds in it. MCUBoot is unsigned and therefore
byte-reproducible, so its identity is the image itself.

When the content matches, the resident descriptor and image are both left in
place. Note that the two must be skipped together: writing a descriptor from
the update while leaving the resident image in place would record a ``data_crc``
that does not describe the image on flash, which the boot ROM rejects on both
the primary and failover paths.

Boot Process
------------

During the boot process, the SMC ROM reads the TT Boot FS descriptor table at
``0x0``. Note that the SMC ROM does not consult the multi-table header at
:c:macro:`TT_BOOT_FS_HEADER_ADDR`; it uses the fixed ``0x0`` and ``0x4000``
addresses directly. If no descriptors in the table at ``0x0`` are marked as
executable, the ROM proceeds to read the failover descriptor table at
``0x4000``.

The ROM will load each binary it encounters in the descriptor table into RAM at
the specified ``copy_dest`` address. If a binary is marked as executable, the
ROM will transfer control to that binary after loading it. If control returns,
the ROM will continue to load binaries from the table.
