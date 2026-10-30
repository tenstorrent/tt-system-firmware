.. _ttzp_services_eth_330g:

330G Ethernet on Blackhole Galaxy
=================================

Blackhole Galaxy runs all of its Ethernet links at 200G by default. The links
inside the system can instead run at 330G. The QSFP-DD ports on the outside
of the system stay at 200G.

Switching to 330G takes two changes, and they always go together:

* the chips are set to auto-train with ``bh-mod``, and
* the retimers on each UBB are set to 90G through the BMC.

Both changes persist, so you only need to make them once. The retimer mode
is saved on the BMC and survives BMC reboots, system power cycles, and BMC
firmware updates. The ``bh-mod`` change is saved in each chip's SPI flash and
survives reboots, power cycles, and firmware updates.

Requirements
------------

* TT-System-Firmware 19.17.0 or later.
* ``bh-mod`` 2.4.1 or later, from the `luwen releases`_ page.
* Galaxy BMC firmware 5.22 or later. Earlier BMC releases can't switch the
  retimers to 90G.
* ``tt-flash`` 3.8.0 or later, so the ``bh-mod`` setting persists across
  firmware updates.

To check the BMC version, log in to the BMC and run
``grep VERSION_ID /etc/os-release``. Version 5.22 shows as ``0.05.22``.

Link speeds
-----------

.. list-table::
   :header-rows: 1

   * - Link
     - Default
     - 330G mode
   * - Chip-to-chip
     - 200G
     - 330G
   * - Chip-to-Examax, passive
     - 200G
     - 330G
   * - Chip-to-Examax, through a retimer
     - 200G
     - 330G
   * - QSFP-DD
     - 200G
     - 200G

The retimer links are on ASICs 4 and 8 of each UBB. They only come up at 330G
when the retimers are switched too, which is why both changes go together.

Running at 330G increases system power by 210 to 230 W.

Switching to 330G
-----------------

.. note::

   The retimer commands on this page come in two interchangeable forms:
   ``ipmitool`` commands that you run from the host, and ``agera2-app``
   commands that you run from a shell on the BMC. Each ``ipmitool`` command
   asks the BMC to run the matching ``agera2-app`` command, so both give the
   same result. Use whichever is more convenient. You don't need to run both.

**1. Set the retimers to 90G.**

From the host (``ipmitool``):

.. code-block:: bash

   # Set the retimers on UBB 0 to 3 to 90G (phy-mode 4)
   sudo ipmitool raw 0x30 0x85 0x01 0x01 0x01 0x04 0x00 0x00
   sudo ipmitool raw 0x30 0x85 0x01 0x01 0x01 0x04 0x01 0x00
   sudo ipmitool raw 0x30 0x85 0x01 0x01 0x01 0x04 0x02 0x00
   sudo ipmitool raw 0x30 0x85 0x01 0x01 0x01 0x04 0x03 0x00

   # Apply the new mode (takes about 10 seconds)
   sudo ipmitool raw 0x30 0x85 0x01 0x00 0x01 0x00 0x00 0x00

In the first four commands, the third-to-last number is the mode (``0x04`` is
90G) and the second-to-last number is the UBB.

Or from the BMC (``agera2-app``):

.. code-block:: bash

   # Set the retimers on UBB 0 to 3 to 90G (phy-mode 4)
   agera2-app --phy-mode-set --ubb-id 0 --phy-id 0 --phy-mode 4
   agera2-app --phy-mode-set --ubb-id 1 --phy-id 0 --phy-mode 4
   agera2-app --phy-mode-set --ubb-id 2 --phy-id 0 --phy-mode 4
   agera2-app --phy-mode-set --ubb-id 3 --phy-id 0 --phy-mode 4

   # Apply the new mode
   agera2-app --init-all-retimers-in-parallel --cfg-mode 1

If a command says the device is busy, wait a few seconds and try again. Only
one retimer command can run at a time.

**2. Set the chips to auto-train.**

.. code-block:: bash

   # Check that the setting is available
   ./bh-mod get eth_property_table.eth_speed_override

   # 0 = auto-train
   ./bh-mod set eth_property_table.eth_speed_override=0

   # Check that the override is set
   ./bh-mod get eth_property_table.eth_speed_override

The ``Override`` column should show ``0`` for every chip.

``bh-mod`` saves the setting in each chip's SPI flash, separately from the
firmware itself. It persists across reboots and firmware updates, so you don't
need to set it again after updating the firmware. Firmware updates need
``tt-flash`` 3.8.0 or later to keep it.

.. warning::

   Use ``0``, not ``330``. Setting ``330`` directly forces every port to 330G,
   including the QSFP-DD ports.

**3. Reset the Galaxy.**

.. code-block:: bash

   tt-smi -glx_reset

Switching back to 200G
----------------------

**1. Set the retimers back to their factory setting** (phy-mode 3). As
before, use either the host commands or the BMC commands, not both.

From the host (``ipmitool``):

.. code-block:: bash

   sudo ipmitool raw 0x30 0x85 0x01 0x01 0x01 0x03 0x00 0x00
   sudo ipmitool raw 0x30 0x85 0x01 0x01 0x01 0x03 0x01 0x00
   sudo ipmitool raw 0x30 0x85 0x01 0x01 0x01 0x03 0x02 0x00
   sudo ipmitool raw 0x30 0x85 0x01 0x01 0x01 0x03 0x03 0x00
   sudo ipmitool raw 0x30 0x85 0x01 0x00 0x01 0x00 0x00 0x00

Or from the BMC (``agera2-app``):

.. code-block:: bash

   agera2-app --phy-mode-set --ubb-id 0 --phy-id 0 --phy-mode 3
   agera2-app --phy-mode-set --ubb-id 1 --phy-id 0 --phy-mode 3
   agera2-app --phy-mode-set --ubb-id 2 --phy-id 0 --phy-mode 3
   agera2-app --phy-mode-set --ubb-id 3 --phy-id 0 --phy-mode 3
   agera2-app --init-all-retimers-in-parallel --cfg-mode 1

**2. Clear the chip setting.** The chips go back to their default of 200G.

.. code-block:: bash

   ./bh-mod res eth_property_table.eth_speed_override

**3. Reset the Galaxy.**

.. code-block:: bash

   tt-smi -glx_reset

Checking the setting
--------------------

**Chips:**

.. code-block:: bash

   ./bh-mod get eth_property_table.eth_speed_override

In 330G mode the override is ``0``. In the default mode, the chips run at 200G.

**Retimers:** check from either the host or the BMC.

From the host (``ipmitool``):

.. code-block:: console

   $ sudo ipmitool raw 0x30 0x85 0x01 0x04 0x01 0x00 0x00 0x00
    01 04 04 00

The third number in the reply is the retimer mode: ``04`` in 330G mode,
``03`` in the default mode. This checks UBB 0. To check the others, change the
second-to-last number to ``0x01``, ``0x02``, or ``0x03``.

Or from the BMC (``agera2-app``):

.. code-block:: bash

   agera2-app --phy-mode-get
   agera2-app --init-verify --cfg-mode 1

A good result ends with ``BCM initialized and verified successfully!!!``. In
330G mode, the output also shows ``lane_rate=90000``.

Known issues
------------

* The QSFP-DD ports stay at 200G. Running them faster needs cables from
  specific vendors, which are expensive and hard to source.
* At 330G, the links use manual equalization. Link training on its own gives
  worse error rates at these speeds.

.. _luwen releases: https://github.com/tenstorrent/luwen/releases
