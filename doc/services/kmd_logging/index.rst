.. _ttfw_kmd_logging:

KMD Logging Mechanism
=====================

This page describes how firmware log records are forwarded to the host kernel
module (KMD) through the ``tt_pcie_log`` backend.

Overview
--------

The mechanism has two paths:

1. Control path: KMD sends setup and release messages over the ARC message queue
   (message ID ``TT_SMC_MSG_TT_PCIE_LOG`` / ``0xC7``).
2. Data path: firmware writes framed log batches into a host DMA buffer and
   raises a PCIe MSI interrupt so KMD can consume them.

On firmware side, the implementation lives in
``lib/tenstorrent/bh_arc/tt_pcie_log.c`` and is enabled by
``CONFIG_TT_PCIE_LOG_BACKEND``.

If the CONFIG is not enabled, FW logs will not be sent to host, but FW will enumerate and operate.
Other logging backends may be used in conjunction with this backend.

Handshake: Setup And Release
----------------------------

KMD uses sub-commands in ``tt_pcie_log_rqst``:

``SETUP`` (subcmd ``1``): KMD allocates a coherent DMA buffer and sends DMA
address (low/high 32-bit words) plus buffer size to firmware. Firmware
validates size, configures NOC2AXI access, initializes the header, and starts
periodic flush.

``RELEASE`` (subcmd ``2``): KMD asks firmware to stop host logging. Firmware
stops the flush timer and returns to local buffering only.

If host is not set up, firmware still formats logs into a local staging buffer.
Once setup succeeds, subsequent flushes transfer staged data to host memory.

Shared Buffer Contract
----------------------

Firmware and KMD must agree on these packed headers:

.. list-table:: Buffer Metadata (16 bytes)
   :header-rows: 1
   :widths: 20 12 12 56

   * - Field
     - Type
     - Size
     - Meaning
   * - ``write_offset``
     - ``uint32_t``
     - 4
     - End offset of valid payload bytes in the shared buffer.
   * - ``buffer_size``
     - ``uint32_t``
     - 4
     - Total host DMA buffer size in bytes.
   * - ``magic``
     - ``uint32_t``
     - 4
     - Constant ``0x544C4F47`` ("TLOG") for contract validation.
   * - ``owner``
     - ``uint8_t``
     - 1
     - Ownership byte: ``0`` = FW owns buffer, ``1`` = host owns buffer.
   * - ``version``
     - ``uint8_t``
     - 1
     - The version of the messaging protocol. Fw currently only supports Version 0.
   * - ``reserved``
     - ``uint8_t[2]``
     - 2
     - Reserved padding for future use.

.. list-table:: Entry Header (12 bytes)
   :header-rows: 1
   :widths: 20 12 12 56

   * - Field
     - Type
     - Size
     - Meaning
   * - ``msg_size``
     - ``uint16_t``
     - 2
     - Total entry size (entry header + payload). Payload includes a trailing
       ``\0`` byte.
   * - ``log_level``
     - ``uint8_t``
     - 1
     - Zephyr log level of the message.
   * - ``source``
     - ``uint8_t``
     - 1
     - Message source: ``0`` = SMC, ``1`` = DMC.
   * - ``timestamp``
     - ``uint32_t``
     - 4
     - Firmware timestamp captured for the log entry.
   * - ``sequence``
     - ``uint32_t``
     - 4
     - Monotonic sequence number used for gap detection.

Payload bytes follow each entry header and are emitted by Zephyr's log output
formatter in text mode. Firmware appends a trailing ``\0`` to each payload for
host-side C-string consumers; this NUL byte is included in ``msg_size``.


Data Flow
---------

1. Zephyr logging calls the backend ``process`` callback for each message.
2. Firmware writes an entry header plus formatted text into a local framed
   buffer.
3. Flush occurs periodically (timer + work queue) or when near local capacity.
4. During flush:
   - firmware validates host ``magic``
   - firmware checks ``owner`` is ``0`` (previous batch consumed)
   - firmware copies bytes to host DMA buffer after the 16-byte buffer header
   - firmware updates ``write_offset``
   - firmware sets ``owner = 1`` (handoff to host)
   - firmware triggers MSI so KMD can process promptly
5. KMD interrupt handler schedules work, parses entries, prints them to kernel
   log with level mapping, then clears ``owner`` back to ``0``.

Current Behavior Notes
----------------------

- Firmware batches data and sends only when host is ready.
- If host has not yet consumed the previous batch, firmware drops the pending
  local batch for forward progress.
- If local staged data exceeds available host payload space, firmware truncates
  to fit.
- Data is currently copied as a contiguous batch (not a true host-side circular
  producer/consumer protocol yet).

Configuration
-------------

Relevant Kconfig options:

- ``CONFIG_TT_PCIE_LOG_BACKEND``: enable host logging backend.
- ``CONFIG_TT_PCIE_LOG_BACKEND_BUFFER_SIZE``: local firmware staging size
  (default ``4096`` bytes).

This backend uses Zephyr text formatting. Other formatters are not supported.

Minimum practical requirement for setup is host buffer size >=
``sizeof(fw_log_buffer_header) + sizeof(fw_log_entry_header) + 1``.

Troubleshooting
---------------

- Setup rejected by firmware: check host-provided buffer size and DMA address
  validity.
- No host logs appearing: ensure ``CONFIG_TT_PCIE_LOG_BACKEND=y`` in firmware
  build and confirm KMD successfully sent setup message and enabled logging.
- Gaps or dropped records: if host processing is slower than firmware
  production, batches can be dropped by design. Reduce firmware log volume or
  increase host consumption responsiveness.
- Ensure the KMD version supports the feature
