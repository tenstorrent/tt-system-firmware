.. _ttzp_compatibility:

Compatibility Guidelines
========================

This guide documents compatibility guarantees for externally consumed firmware
interfaces. The goal is to let newer firmware continue working with older
controllers, and to let newer controllers detect and adapt to older firmware
without relying on undefined behavior.

Compatibility Model
-------------------

Tenstorrent firmware uses a stability-first model for published host-facing
interfaces:

- Existing telemetry tags keep their meaning.
- Existing host message IDs and request layouts keep their meaning.
- Existing SMBus register IDs and payload meanings keep their meaning.
- New functionality is added by extension, not by repurposing an existing
  field, message, or register.

Controllers must assume that older firmware may not implement newly added
features. Firmware must assume that older controllers may continue using older,
previously documented interfaces indefinitely.

Interface Rules
---------------

Telemetry Interface
~~~~~~~~~~~~~~~~~~~

The telemetry interface is defined in ``lib/tenstorrent/bh_arc/telemetry.h``.

Compatibility Discovery
***********************

Controllers can and should detect newly added telemetry entries by reading the
telemetry table metadata and tag table, rather than assuming a fixed tag set is
present.

As documented in
`Procedure to Read Telemetry </tt-system-firmware/services/telemetry/index.html#procedure-to-read-telemetry>`_:

- Read ``telemetry_table.entry_count``.
- Walk the telemetry ``tag_table``.
- Use the discovered tag-to-offset mapping to determine whether a given tag is
  present in the running firmware.

This is the preferred discovery mechanism for new telemetry entries.

Compatibility Rules
*******************

- Do not rename an existing telemetry tag.
- Do not repurpose an existing telemetry tag to mean something else.
- Do not change the encoding or field definition of an existing telemetry tag.
- Do not change the meaning of an existing bit in a published telemetry bitfield.
- Best practice is to reserve unused bits explicitly, and keep them reading as 0 until they are
  intentionally assigned in a new compatibility version.
- Bits not marked as reserved may not be repurposed
- Repurposing of documented reserved bits in an allocated telemetry field is allowed and expected.
- Add new telemetry information by defining new tags
- FW doxygen is the single source of truth for field definitions.

Rationale
*********

Controllers may hardcode assumptions about the meaning, width, and encoding of
existing tags and bit positions. If an existing field is silently repurposed,
older controllers can misinterpret the data without any negotiation step.

Host Message Interface
~~~~~~~~~~~~~~~~~~~~~~

The host message interface is documented in
`Host Message Interface </tt-system-firmware/doxygen/group__tt__msg__apis.html>`_
and is defined primarily in ``include/tenstorrent/smc_msg.h`` and
``include/tenstorrent/msgqueue.h``.

Compatibility Discovery
***********************

Message support can be discovered by attempting to send the message and
checking the firmware response.

If a message is not supported by the running firmware, the generic message
dispatch path returns ``0xff`` in ``response.data[0]``.

Controllers should treat this as "message not recognized by this firmware" and
fall back accordingly.

Compatibility Rules
*******************

- Do not repurpose an existing ``TT_SMC_MSG_*`` message ID.
- Do not change the layout or semantic meaning of an existing request or
  response structure once published.
- Do not change the meaning of an existing field inside a published message.
- Add new functionality by allocating a new message ID, a new submessage ID, or
  a new request/response structure.
- Extensions to existing messages are allowed, but they must be introduced as new protocol elements,
  not as reinterpretations of existing ones.

Rationale
*********

Controllers often serialize these messages directly and may not be updated in
lockstep with firmware. A wire-compatible extension is safe; changing the
meaning of an existing message is not.

SMBus Register Interface
~~~~~~~~~~~~~~~~~~~~~~~~

The SMBus register interface is defined in
``include/tenstorrent/tt_smbus_regs.h``.

Compatibility Discovery
***********************

The SMBus interface does not provide a general in-band capability-discovery
mechanism.

Controllers should therefore treat the SMBus command map as a fixed,
versioned contract:

- Use only documented SMBus registers for the target firmware version.
- Do not assume that probing unknown command IDs is a supported discovery
  mechanism.
- When SMBus behavior depends on newer firmware features, prefer discovering
  those features through telemetry or firmware-version information before using
  a newer SMBus command.

Compatibility Rules
*******************

- Do not rename an existing ``CMFW_SMBUS_*`` register definition.
- Do not repurpose an existing SMBus register address.
- Do not change the meaning or payload contract of an existing SMBus register.
- Do not extend an existing SMBus command by changing its payload size or
  payload interpretation.
- Add new SMBus functionality only by allocating a new register/command.

Rationale
*********

SMBus integrations are often implemented in external controller firmware or
board-management logic where message size and meaning are tightly coupled to a
specific register address.

Compatibility Matrix
--------------------

.. list-table::
   :header-rows: 1
   :widths: 20 20 20 40

   * - Controller
     - Firmware
     - Expected status
     - Notes
   * - Old
     - Old
     - Supported
     - Baseline case.
   * - Old
     - New
     - Supported
     - New firmware must preserve all previously published telemetry tags,
       message IDs, message layouts, and SMBus register definitions.
   * - New
     - Old
     - Baseline supported
     - New controllers must discover capabilities and only use features supported
       by the older firmware. New optional functionality must degrade cleanly.
   * - New
     - New
     - Supported
     - Full functionality is available, including newly added features.

Practical Guidance
------------------

When adding a new externally visible feature:

1. Add a new capability indication if feature discovery is needed.
2. Preserve all existing message, telemetry, and SMBus definitions.
3. Prefer adding a new tag, new message ID, new submessage ID, or new SMBus
   register over changing an existing one.
4. Document the compatibility contract in the relevant header and update this
   guide if the new interface introduces a new compatibility surface.

Feature Capability Discovery
----------------------------

Feature capability and active-configuration telemetry are documented in
`Feature Capabilities </tt-system-firmware/doxygen/group__telemetry__feature__capabilities.html>`_.

That interface provides an explicit discovery path for optional or newly implemented behavior:

- ``TAG_FW_CAPABILITIES_<x>`` reports whether firmware supports a feature.
- ``TAG_FW_ACTIVE_CONFIG_<x>`` reports whether that feature is currently enabled.

Where possible, controllers may use capability telemetry to decide whether it is safe to use
optional or newly implemented functionality.

ABI Breaks
----------

ABI breaks must be avoided wherever possible.

If an ABI break is unavoidable:

- Increment the major version.
- Document the break and migration impact in the release notes.
- Inform all relevant stakeholders before release so downstream integrations can
  plan and validate the transition.
