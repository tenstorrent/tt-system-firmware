.. _ttzp_services_pldm:

PLDM Service
============

Overview
--------

The DMC PLDM service exposes platform sensor data to a BMC over
MCTP-on-I3C. It is implemented as a Zephyr driver.

Acronyms used in this document:

.. list-table::
   :header-rows: 1

   * - Acronym
     - Expansion
     - Description
   * - BMC
     - Baseboard Management Controller
     - Requester side that discovers PLDM capabilities and reads sensor values.
   * - DAA
     - Dynamic Address Assignment
     - I3C enumeration step where the controller assigns runtime addresses to targets.
   * - DMC
     - Device Management Controller
     - Target/responder firmware side in this project.
   * - DMTF
     - Distributed Management Task Force
     - Standards body that publishes PLDM and MCTP specifications used by this service.
   * - DT
     - Device Tree
     - Hardware description data used to configure Zephyr, including PLDM sensor bindings and endpoint wiring.
   * - I3C
     - Improved Inter-Integrated Circuit
     - Physical bus used for target connectivity and message transport in this integration.
   * - MCTP
     - Management Component Transport Protocol
     - Transport layer that carries PLDM messages between requester and responder.
   * - PDR
     - Platform Descriptor Record
     - Structured records describing platform entities and sensors used by PLDM discovery.
   * - PLDM
     - Platform Level Data Model
     - DMTF protocol used here for platform discovery and sensor telemetry commands.
   * - TID
     - Terminus ID
     - PLDM identifier returned by ``GetTID`` for a responder terminus.

Architecture
------------

Component view
~~~~~~~~~~~~~~

.. mermaid::

   block
     columns 6
     block:BMC_CONTAINER
       columns 1
       space
       BMC["BMC"]
       space
     end
     block:DMC:4
       columns 5
       space
       space
       LABEL["DMC"]
       space
       space

       DMC_TGT_IF["I3C Target Ctrl"]
       space
       space
       space
       DMC_SENSORS["DMC Sensors"]

       MCTP_BIND["MCTP Target"]
       space
       block:PLDM
         columns 1
         PLDM_LABEL["PLDM"]
         PLDM_SENS_BIND["Sensor Bindings"]
         PDR_REPO["PDR"]
         PLDM_RESP["Responder"]
       end
       space
       SMC_SENSORS["SMC Sensors"]
     end

      block:SMC_CONTAINER
       columns 1
       space
       space
       SMC["SMC"]
     end

     style BMC_CONTAINER fill:none,stroke:none,stroke-width:0px
     style LABEL fill:none,stroke:none,stroke-width:0px,font-weight:bold
     style PLDM_LABEL fill:none,stroke:none,stroke-width:0px,font-weight:bold
     style SMC_CONTAINER fill:none,stroke:none,stroke-width:0px,font-weight:bold

     BMC --- DMC_TGT_IF
     DMC_TGT_IF --- MCTP_BIND
     MCTP_BIND --- PLDM_RESP
     SMC_SENSORS --- PLDM_SENS_BIND
     SMC_SENSORS --- SMC
     DMC_SENSORS --- PLDM_SENS_BIND
     PLDM_SENS_BIND --- PDR_REPO
     PDR_REPO --- PLDM_RESP

Request/Response Ladder Example
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. mermaid::

   sequenceDiagram
     participant BMC as BMC
     participant I3C as I3C Target Interface
     participant MCTP as MCTP I3C Target Binding
     participant RESP as PLDM Responder
     participant PDR as PDR Repository Builder
     participant MAP as PLDM Sensor Bindings
     participant SNS as Sensor Nodes

     BMC->>I3C: PLDM over MCTP frame
     I3C->>MCTP: Deliver frame payload
     I3C-->>BMC: I3C Write Acknowledged
     MCTP->>RESP: Dispatch PLDM request

     alt PDR discovery path
       RESP->>PDR: GetPDRRepositoryInfo / GetPDR
       PDR-->>RESP: Repository metadata / record data
     else Sensor reading path
       RESP->>MAP: Resolve sensor-id to descriptor
       MAP->>SNS: sensor_sample_fetch + sensor_channel_get
       SNS-->>MAP: sensor_value
       MAP-->>RESP: Scaled PLDM reading
     end

     RESP-->>MCTP: PLDM response
     MCTP-->>I3C: MCTP frame

     alt IBI
       I3C-->>BMC: IBI
       BMC->>I3C: I3C read
     else Polling
       BMC->>I3C: I3C read
     end

     I3C-->>BMC: MCTP response on bus

I3C Target Interface
~~~~~~~~~~~~~~~~~~~~

This service uses the STM32 I3C peripheral in target mode. It acts as the physical endpoint for
incoming management traffic. In the DT, ``zephyr,mctp-i3c-target`` binds to that target-mode
controller and exposes an MCTP transport endpoint to the PLDM responder.

This interface is responsible for:

- participating in I3C bus enumeration/addressing driven by the BMC
- receiving/sending MCTP frames over I3C once the endpoint is reachable
- passing payloads up to the MCTP binding used by the PLDM responder

MCTP I3C Target Binding
~~~~~~~~~~~~~~~~~~~~~~~

The service uses Zephyr's upstream ``zephyr,mctp-i3c-target`` binding/API as
the transport shim between the STM32 I3C target peripheral and ``libmctp``.
At init time, the binding instance is created from DT and then registered as a bus with ``libmctp``.

PLDM Responder
~~~~~~~~~~~~~~

The PLDM responder is the protocol-facing driver component that sits above the
MCTP I3C target binding and serves PLDM requests from the BMC.

Responsibilities:

- initializes responder context and connects PLDM processing to MCTP RX/TX
- handles PLDM Base discovery commands (for example ``GetTID``,
  ``GetPLDMTypes``, ``GetPLDMCommands``)
- dispatches PLDM Platform commands to repository/handler logic
- references a dedicated PDR provider node that supplies descriptor data for
  repository generation and sensor reads

PDR Builder and Platform Handlers
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The PDR builder/platform handler component manages PLDM Platform repository
state and serves PDR-related commands from the responder.

Responsibilities:

- binds numeric-sensor descriptors from ``tenstorrent,pldm-pdr`` child nodes
- builds the in-memory PDR repository at init
- appends records in handle order (Terminus Locator, Numeric Sensor, and
  Sensor Auxiliary Names when ``sensor-name`` is present)
- serves repository metadata and record payloads for
  ``GetPDRRepositoryInfo`` and ``GetPDR``

Field sources
^^^^^^^^^^^^^

The current implementation does not negotiate PDR content at runtime. The
repository is assembled once at init from a mix of DT-provided values,
DT-derived defaults, and fixed code constants.

Terminus Locator PDR
~~~~~~~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1

   * - Field
     - Source
     - Notes
   * - ``record_handle``
     - Static code
     - Starts at ``1`` and increments in append order.
   * - ``version``
     - Static code
     - Set to ``1``.
   * - ``type``
     - Static code
     - Set to ``PLDM_TERMINUS_LOCATOR_PDR``.
   * - ``record_change_num``
     - Static code
     - Fixed at ``TT_PLDM_PDR_RECORD_CHANGE_NUM`` (currently ``0``).
   * - ``length``
     - Static code
     - Computed from the encoded struct size.
   * - ``terminus_handle``
     - Static code
     - Fixed at ``1``.
   * - ``validity``
     - Static code
     - Set to ``PLDM_TL_PDR_VALID``.
   * - ``tid``
     - DTS
     - Comes from ``pldm-tid`` on ``tenstorrent,pldm-mctp-responder``. Can be overwritten by the command SetTID.
   * - ``container_id``
     - Static code
     - Fixed at ``0`` for the locator record.
   * - ``terminus_locator_type``
     - Static code
     - Set to ``PLDM_TERMINUS_LOCATOR_TYPE_MCTP_EID``.
   * - ``terminus_locator_value_size``
     - Static code
     - Fixed at ``1`` byte.
   * - ``terminus_locator_value[0]``
     - DTS
     - Comes from ``endpoint-id`` on the referenced
       ``zephyr,mctp-i3c-target`` node.

Numeric Sensor PDR
~~~~~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1

   * - Field
     - Source
     - Notes
   * - ``record_handle``
     - Static code
     - Assigned sequentially after the Terminus Locator record.
   * - ``version``
     - Static code
     - Set to ``1``.
   * - ``type``
     - Static code
     - Set to ``PLDM_NUMERIC_SENSOR_PDR``.
   * - ``record_change_num``
     - Static code
     - Fixed at ``TT_PLDM_PDR_RECORD_CHANGE_NUM``.
   * - ``length``
     - Static code
     - Computed from the struct size.
   * - ``terminus_handle``
     - Static code
     - Fixed at ``1``.
   * - ``sensor_id``
     - DTS
     - From ``sensor-id`` on each ``tenstorrent,pldm-numeric-sensor`` node.
   * - ``entity_type``
     - DTS
     - From ``entity-type``; defaults to ``135`` when omitted.
   * - ``entity_instance``
     - DTS
     - From ``entity-instance``; defaults to ``1`` when omitted.
   * - ``container_id``
     - DTS
     - From ``container-id``; defaults to ``0`` when omitted.
   * - ``sensor_auxiliary_names_pdr``
     - DTS-derived
     - Set when ``sensor-name`` is present and non-empty.
   * - ``base_unit``
     - DTS or DTS-derived
     - Uses ``base-unit`` when present; otherwise inferred from
       ``channel-type`` in code.
   * - ``unit_modifier``
     - DTS
     - From ``unit-modifier``; defaults to ``0`` when omitted.
   * - ``sensor_init``
     - Static code
     - Set to ``PLDM_NO_INIT``.
   * - ``rate_unit``
     - Static code
     - Set to ``PLDM_RATE_UNIT_NONE``.
   * - ``base_oem_unit_handle``
     - Static code
     - Set to ``0``.
   * - ``aux_unit``
     - Static code
     - Set to ``PLDM_SENSOR_UNIT_NONE``.
   * - ``aux_unit_modifier``
     - Static code
     - Set to ``0``.
   * - ``aux_rate_unit``
     - Static code
     - Set to ``PLDM_RATE_UNIT_NONE``.
   * - ``rel``
     - Static code
     - Set to ``0``.
   * - ``aux_oem_unit_handle``
     - Static code
     - Set to ``0``.
   * - ``is_linear``
     - Static code
     - Set to ``true``.
   * - ``sensor_data_size``
     - Static code
     - Set to ``PLDM_SENSOR_DATA_SIZE_SINT32``.
   * - ``resolution``
     - Static code
     - Fixed at ``1.0``.
   * - ``offset``
     - Static code
     - Fixed at ``0.0``.
   * - ``accuracy``
     - Static code
     - Fixed at ``0``.
   * - ``plus_tolerance`` / ``minus_tolerance``
     - Static code
     - Fixed at ``0``.
   * - ``hysteresis``
     - Static code
     - Fixed at ``0``.
   * - ``supported_thresholds``
     - Static code
     - Fixed at ``0``.
   * - ``threshold_and_hysteresis_volatility``
     - Static code
     - Fixed at ``0``.
   * - ``state_transition_interval`` / ``update_interval``
     - Static code
     - Fixed at ``0.0``.
   * - ``max_readable`` / ``min_readable``
     - Static code
     - Fixed at ``INT32_MAX`` and ``INT32_MIN``.
   * - ``range_field_format``
     - Static code
     - Set to ``PLDM_RANGE_FIELD_FORMAT_SINT32``.
   * - ``range_field_support``
     - Static code
     - Fixed at ``0``.

Sensor Auxiliary Names PDR
~~~~~~~~~~~~~~~~~~~~~~~~~~

This record is emitted only when the numeric sensor node provides
``sensor-name``.

.. list-table::
   :header-rows: 1

   * - Field
     - Source
     - Notes
   * - ``record_handle``
     - Static code
     - Assigned immediately after the owning Numeric Sensor PDR.
   * - ``version``
     - Static code
     - Set to ``1``.
   * - ``type``
     - Static code
     - Set to ``PLDM_SENSOR_AUXILIARY_NAMES_PDR``.
   * - ``record_change_num``
     - Static code
     - Fixed at ``TT_PLDM_PDR_RECORD_CHANGE_NUM``.
   * - ``length``
     - Static code
     - Computed from encoded locale and UTF-16BE name bytes.
   * - ``terminus_handle``
     - Static code
     - Fixed at ``1``.
   * - ``sensor_id``
     - DTS
     - Reuses the owning numeric sensor node's ``sensor-id``.
   * - ``sensor_count``
     - Static code
     - Fixed at ``1``.
   * - ``names`` locale tag
     - Static code
     - Fixed to ``en``.
   * - ``names`` string payload
     - DTS
     - Encoded from ``sensor-name`` as UTF-16BE.

Sensor Nodes
~~~~~~~~~~~~

Any Zephyr sensor device API can be used to source PLDM sensor values. The
binding from a sensor-compatible device to the PDR must be defined in DT in
order to be visible via PLDM.

``tenstorrent,bh-arc-telemetry`` acts as a DMC sensor driver to map SMC
telemetry tags to Zephyr sensor channels. Each child map entry binds:

- ``channel-type`` + ``channel-index``
- ``telemetry-tag``
- optional ``scale-micro`` (defaults to ``1000000``)

A single sensor can be the backend for multiple exported PLDM sensor nodes.

DTS example
~~~~~~~~~~~

.. code-block:: dts

   / {
     /* The I3C controller. In DMC, this will always use the STM I3C controller */
     i3c1 : i3c1 {
       status = "okay";
     };

     /* The MCTP binding. This will bind i3c1 to libmctp */
     mctp_i3c_target: mctp_i3c_target {
       compatible = "zephyr,mctp-i3c-target";
       i3c = <&i3c1>;
       endpoint-id = <11>;
     };

     /* The PLDM binding. This will bind the libmctp callbacks through to PLDM processing */
     pldm_mctp_responder: pldm_mctp_responder {
       compatible = "tenstorrent,pldm-mctp-responder";
       mctp-target = <&mctp_i3c_target>;
       pdr = <&pldm_pdr0>;
       pldm-tid = <11>;
     };

     /* The SMC sensor driver. This allows SMC sensor readings to be visible via a
      * Zephyr-style sensor driver.
      */
     smc_sensor: smc_sensor {
       compatible = "tenstorrent,bh-arc-telemetry";
       status = "okay";
       arc = <&chip0_arc>;
       vcore {
         channel-type = <14>; /* SENSOR_CHAN_VOLTAGE */
         channel-index = <0>;
         telemetry-tag = <TAG_VCORE>;
       };
       /* Add additional nodes here for more channel-> telemetry mappings */
     };

     /* The PDR repository builder. This links zephyr sensor devices to a PLDM_SENSOR_ID,
      * valid across all builds. The entries here are used both to assemble the PDR and also link
      * runtime sensor fetches of a given PLDM_SENSOR_ID to a given zephyr sensor.
      */
     pldm_pdr0: pldm_pdr0 {
       compatible = "tenstorrent,pldm-pdr";

       pldm_voltage0: pldm_voltage0 {
         compatible = "tenstorrent,pldm-numeric-sensor";
         sensor = <&zephyr_voltage_sensor0>;
         channel-type = <14>;              /* SENSOR_CHAN_VOLTAGE */
         channel-index = <0>;
         sensor-id = <PLDM_SENSOR_ID>;     /* Tenstorrent specific, obeying compatibility rules */
       };
     };
   };

This illustrates the phandle chain:
``mctp_i3c_target`` -> ``pldm_mctp_responder`` -> ``pldm_pdr0`` ->
``tenstorrent,pldm-numeric-sensor``.

Implemented PLDM Commands
~~~~~~~~~~~~~~~~~~~~~~~~~

The responder currently implements the following command sets.

Base
^^^^

- ``GetTID``
- ``SetTID``
- ``GetPLDMTypes``
- ``GetPLDMVersion``
- ``GetPLDMCommands``

Platform
^^^^^^^^

- ``GetPDRRepositoryInfo``
- ``GetPDR``
- ``GetSensorReading``

OEM
^^^

The responder may advertise Tenstorrent OEM type ``0x3F``. These requests use
the standard PLDM transport over MCTP-on-I3C, with the command code selecting
the OEM operation. Some OEM services may be unavailable depending on the
enabled Kconfig options of the DMC build.

.. list-table::
   :header-rows: 1

   * - Command
     - Command code
     - Request fields
     - Purpose
   * - ``ShellExec``
     - ``0x01``
     - ``request_id``, ``cmd_len``, ``cmd``
     - Queue a shell command for execution.
   * - ``ShellGetResult``
     - ``0x02``
     - ``request_id``, ``offset``, ``max_read_len``
     - Poll status and read back output chunks for an earlier request.
   * - ``ShellCancel``
     - ``0x03``
     - ``request_id``
     - Cancel a queued or running shell request when possible.

Examples
~~~~~~~~

SMC Voltage Read Flow

.. mermaid::

   sequenceDiagram
     participant BMC as BMC
     box rgb(240, 248, 255) DMC
       participant PLDM as PLDM Responder
       participant DRV as bh-arc-telemetry Driver
     end
     box rgb(245, 245, 245) SMBus
       participant SMB as SMBus
     end
     box rgb(248, 255, 248) SMC
       participant TM as SMC Telemetry
       participant ZS as Zephyr Sensor API
     end

     loop Background telemetry update
       TM->>ZS: sensor fetch
       ZS->>TM: sensor_value (int, frac) -> 16.16
     end
     BMC->>PLDM: GetSensorReading(sensor-id)
     PLDM->>DRV: sensor_sample_fetch + sensor_channel_get
     DRV->>TM: read telemetry tag
     TM-->>DRV: 16.16 voltage value
     DRV-->>PLDM: sensor_value (int, frac)
     PLDM-->>PLDM: scale by 10^-unit_modifier
     PLDM-->>BMC: present_reading

SMC Voltage Read - SMC timeout

.. mermaid::

   sequenceDiagram
     participant BMC as BMC
     box rgb(240, 248, 255) DMC
       participant PLDM as PLDM Responder
       participant DRV as bh-arc-telemetry Driver
     end
     box rgb(245, 245, 245) SMBus
       participant SMB as SMBus
     end
     box rgb(248, 255, 248) SMC
       participant TM as SMC Telemetry
       participant ZS as Zephyr Sensor API
     end

     loop Background telemetry update
       TM->>ZS: sensor fetch
       ZS--xTM: sensor fetch error
     end
     BMC->>PLDM: GetSensorReading(sensor-id)
     PLDM->>DRV: sensor_sample_fetch + sensor_channel_get
     DRV->>SMB: read telemetry request
     SMB--xDRV: timeout / NACK
     DRV-->>PLDM: sensor read error
     PLDM-->>BMC: GetSensorReading CC = PLDM_ERROR

Compatibility
-------------

To keep requester integrations stable across firmware revisions, this service
uses a fixed-ID compatibility model.

Compatibility policy:

- treat ``sensor-id`` as the stable external contract; IDs are defined and documented in firmware source
  and should not be reassigned
- use Sensor Auxiliary Names as presentation metadata only; names may evolve
  without changing the underlying ``sensor-id``
- do not reuse an existing ID for a different meaning, unit, or entity mapping
- when adding new sensors, allocate new IDs instead of repurposing old ones
- for incompatible semantic changes, introduce a new ID and deprecate the old
  ID during migration

Requester guidance:

- discover IDs from PDRs (``GetPDRRepositoryInfo`` + ``GetPDR``), then use
  those IDs for ``GetSensorReading``
- do not infer identity from record order or display name alone

Notes and limits
----------------

- ``GetPDR`` supports multipart transfer with CRC in final chunk.
- Current auxiliary name encoding is UTF-16BE with ``en`` language tag.

References
----------

1. `PLDM Base Specification (DSP0240 v1.2.0) <https://www.dmtf.org/sites/default/files/standards/documents/DSP0240_1.2.0.pdf>`_

2. `PLDM for Platform Monitoring and Control (DSP0248 v1.2.2) <https://www.dmtf.org/sites/default/files/standards/documents/DSP0248_1.2.2.pdf>`_

3. `Zephyr Sensor Subsystem <https://docs.zephyrproject.org/latest/hardware/peripherals/sensor/index.html>`_
