.. _ttzp_getting_started:

Getting Started
===============

This guide will walk you through setting up your development environment, building and flashing your first firmware.

Prerequisites
-------------

If you're new to Zephyr, we recommend starting with the official
`Zephyr Getting Started Guide <https://docs.zephyrproject.org/latest/develop/getting_started/index.html>`_
to familiarize yourself with the framework.

Before continuing, ensure you have installed the
`Zephyr system requirements <https://docs.zephyrproject.org/latest/develop/getting_started/index.html#install-dependencies>`_
for your operating system.

Setting Up Your Development Environment
---------------------------------------

Follow these steps to set up your TT System Firmware development environment:

.. important::
   TT System Firmware requires **Python 3.12**. Although upstream Zephyr documentation references Python 3.10, ensure you install and use Python 3.12 before creating your virtual environment.

On Ubuntu, you can install Python 3.12 and its tooling with:

.. code-block:: shell

   sudo add-apt-repository ppa:deadsnakes/ppa
   sudo apt update
   sudo apt install python3.12 python3.12-dev python3.12-venv

Verify that ``python3.12`` is available on your ``PATH`` before continuing.

.. code-block:: shell

   # Step 1: Create and activate a Python 3.12 virtual environment
   python3.12 -m venv ~/tt-system-firmware-work/.venv
   source ~/tt-system-firmware-work/.venv/bin/activate

   # Step 2: Install West (Zephyr's meta-tool)
   pip install west

   # Step 3: Initialize the workspace with TT System Firmware
   west init -m https://github.com/tenstorrent/tt-system-firmware ~/tt-system-firmware-work

   # Step 4: Navigate to the workspace directory
   cd ~/tt-system-firmware-work

   # Step 5: Download all required Zephyr modules
   west update

   # Step 6: Install additional Python dependencies
   west packages pip --install

   # Step 7: Install the Zephyr SDK (required for building)
   west sdk install

   # Step 8: Download binary blobs (firmware components)
   west blobs fetch tt-system-firmware

.. note::
   The setup process may take several minutes to complete, especially when downloading the Zephyr SDK and modules.

Building and Flashing SMC Firmware
-----------------------------------

The System Management Controller (SMC) firmware manages system-level operations. Follow these steps to build and deploy it:

**Step 1: Choose your board revision and build the firmware**

Select the appropriate command for your board revision:

.. tabs::
    .. group-tab:: p100a
        .. code-block:: shell

            west build --sysbuild -p -b tt_blackhole@p100a/tt_blackhole/smc app/smc -- -DCONFIG_SHELL=y

    .. group-tab:: p150a
        .. code-block:: shell

            west build --sysbuild -p -b tt_blackhole@p150a/tt_blackhole/smc app/smc -- -DCONFIG_SHELL=y

    .. group-tab:: p150b
        .. code-block:: shell

            west build --sysbuild -p -b tt_blackhole@p150b/tt_blackhole/smc app/smc -- -DCONFIG_SHELL=y

    .. group-tab:: p150c
        .. code-block:: shell

            west build --sysbuild -p -b tt_blackhole@p150c/tt_blackhole/smc app/smc -- -DCONFIG_SHELL=y

    .. group-tab:: p300a
        .. code-block:: shell

            west build --sysbuild -p -b tt_blackhole@p300a/tt_blackhole/smc app/smc -- -DCONFIG_SHELL=y

    .. group-tab:: p300b
        .. code-block:: shell

            west build --sysbuild -p -b tt_blackhole@p300b/tt_blackhole/smc app/smc -- -DCONFIG_SHELL=y

    .. group-tab:: p300c
        .. code-block:: shell

            west build --sysbuild -p -b tt_blackhole@p300c/tt_blackhole/smc app/smc -- -DCONFIG_SHELL=y

**Step 2: Complete the deployment process**

After the build completes successfully, run these commands to deploy and connect to your board:

.. code-block:: shell

    # Flash the firmware to your board
    west flash -r tt_flash --force

    # Build the TT firmware terminal utility (for board communication)
    make -j -C scripts/tooling OUTDIR=/tmp tt-fw-terminal

    # Connect to the board console
    /tmp/tt-fw-terminal

.. tip::
   The ``--sysbuild`` option automatically builds both SMC and DMC firmware together, which is the recommended approach for most users.

Building DMC Firmware (Advanced Users)
--------------------------------------

.. important::
   **Most users can skip this section.** When you build SMC firmware with ``--sysbuild`` (as shown above),
   the DMC (Device Management Controller) firmware is automatically built and flashed along with it.

**When you might need to build DMC separately:**

- You're a developer who needs to update MCUBoot
- You're working specifically on DMC firmware modifications
- You need to debug DMC-specific issues

.. warning::
   Updating MCUBoot is not required or recommended for end users. Only proceed if you have specific development needs.

**Manual DMC build process:**

.. code-block:: shell

   # Build DMC firmware (replace p100a with your board revision)
   west build -b tt_blackhole@p100a/tt_blackhole/dmc app/dmc

   # Flash the bootloader and application
   west flash

   # Open the Real-Time Transfer (RTT) viewer to see output
   west rtt

**Expected console output:**

When DMC firmware boots successfully, you should see output similar to this:

.. code-block:: shell

   *** Booting MCUboot v2.1.0-rc1-389-g4eba8087fa60 ***
   *** Using Zephyr OS build v4.2.0-rc3 ***
   I: Starting bootloader
   I: Primary image: magic=good, swap_type=0x2, copy_done=0x1, image_ok=0x1
   I: Secondary image: magic=unset, swap_type=0x1, copy_done=0x3, image_ok=0x3
   I: Boot source: none
   I: Image index: 0, Swap type: none
   I: Bootloader chainload address offset: 0xc000
   I: Image version: v0.9.99
   I: Jumping to the first image slot
            .:.                 .:
         .:-----:..             :+++-.
      .:------------:.          :++++++=:
    :------------------:..      :+++++++++
    :----------------------:.   :+++++++++
    :-------------------------:.:+++++++++
    :--------:  .:-----------:. :+++++++++
    :--------:     .:----:.     :+++++++++
    .:-------:         .        :++++++++-
       .:----:                  :++++=:.
           .::                  :+=:
             .:.               ::
             .===-:        .-===-
             .=======:. :-======-
             .==================-
             .==================-
              ==================:
               :-==========-:.
                   .:====-.

   *** Booting tt_blackhole with Zephyr OS v4.2.0-rc3 ***
   *** APP_BUILD_VERSION v18.6.0-78-gf104f347ff0f ***
   *** SDK_VERSION zephyr sdk 0.17.2 ***
   DMFW VERSION 0.9.99

Pulling in New Code from ``main``
---------------------------------

After pulling in the latest code from ``main``, you will avoid many mysterious build / functional issues by executing these commands:

.. code-block:: shell

   # Update Zephyr and other modules to the version in the manifest
   west update


Testing Your Setup
------------------

Once you've successfully built and flashed firmware, you can run tests to verify everything is working correctly.

**Testing SMC firmware:**

.. note::
   Some users may need to patch their OpenOCD binaries to support Segger's RTT on RISC-V and ARC
   architectures. For more information, see
   `this GitHub PR <https://github.com/zephyrproject-rtos/openocd/pull/66>`_.

.. code-block:: shell

   # Run a basic "Hello World" test on SMC (replace p100a with your board revision)
   twister -i -p tt_blackhole@p100a/tt_blackhole/smc --device-testing --west-flash \
     --device-serial-pty rtt --west-runner /opt/tenstorrent/bin/openocd-rtt \
     -s samples/hello_world/sample.basic.helloworld.rtt

**Testing DMC firmware:**

.. code-block:: shell

   # Run a basic "Hello World" test on DMC (replace p100a with your board revision)
   twister -i -p tt_blackhole@p100a/tt_blackhole/dmc --device-testing --west-flash \
     --device-serial-pty rtt --west-runner openocd \
     -s samples/hello_world/sample.basic.helloworld.rtt

.. tip::
   If tests pass successfully, your development environment is properly configured and ready for development!

Setting Up Development Tools (Optional)
---------------------------------------

If you plan to contribute code to the project, we recommend setting up git hooks to automatically check your code for formatting and compliance issues.

**Install git hooks:**

.. code-block:: shell

   # Run this script from your workspace directory
   tt-system-firmware/scripts/add-git-hooks.sh

.. note::
   These hooks will automatically run before commits and pushes to ensure your code meets project standards.
