.. _ttzp_testing:

Hardware Testing
================

This page documents support for executing tests on developer systems. These
tests are also run on CI systems, but developers can execute them locally to
debug failures or validate new testcases.

Prerequisites
-------------

A development environment should be set up following
:ref:`Getting Started<ttzp_getting_started>`

All commands on this page use paths relative to the ``tt-system-firmware``
repository root (the checkout created by
:ref:`Getting Started<ttzp_getting_started>`). Run them from that directory:

.. code-block:: shell

   cd ~/tt-system-firmware-work/tt-system-firmware

A device under test (DUT) should be connected to your system, with
the following attached:

* Blackhole debug board
* ST-Link debug probe
* JLink debug probe

Test Types
----------

The following tests are executed on hardware:

* End-to-end system integration tests
* Self-contained unit tests

All tests are executed using Zephyr's test manager, twister. Custom shell
scripts are defined to invoke twister for each test.

Unit Tests
**********

Unit tests are self-contained tests that validate a specific component of the
firmware. They typically do not require any interaction from the host,
and run entirely on the DUT.

In order to execute unit tests, the following command can be used:

.. code-block:: shell

   scripts/ci/run-smoke.sh <board_name> -- --clobber-output

Note that on P300 systems, ``-p 1`` should be added as tests execute on the
second ASIC.

End-to-End Tests
****************

End-to-end tests utilize the production firmware, and validate that the
host and DUT can communicate and perform the expected operations.

In order to execute end-to-end tests, the following command can be used:

.. code-block:: shell

   scripts/ci/run-e2e.sh <board_name> -- --clobber-output

Note that on P300 systems, ``-p 1`` should be added as tests execute on the
second ASIC.

Running Tests Outside of Twister
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

For some cases (IE test development), the developer may want to run end-to-end
test cases directly via pytest. After manually flashing the firmware to be
tested, this is possible with the following command:

.. code-block:: shell

   pytest app/smc/pytest/e2e_smoke.py

Pytest additionally supports running a single test instead of all, for example:

.. code-block:: shell

   pytest app/smc/pytest/e2e_smoke.py::test_boot_status

Some tests might require additional parameters and might be skipped if these are not provided.
For example, ``--board`` is required for some tests to run. You can get a list of the test options
tenstorrent exposes by looking under ``Custom options:`` of the output of the ``--help``.

.. code-block:: shell

   pytest app/smc/pytest/e2e_smoke.py --help

Stress Tests
************

Stress tests are an extended version of the end-to-end tests, which
validate that the platform is stable over many iterations of the same testsuite.

In order to execute stress tests, the following command can be used:

.. code-block:: shell

   scripts/ci/run-stress.sh <board_name> -- --clobber-output

Note that these tests can take up to 90 minutes to execute. To reduce their
execution time, consider editing ``MAX_TEST_ITERATIONS`` in ``e2e_stress.py``
to a lower value.

Manufacturing Test
------------------

The manufacturing test reproduces the factory bring-up sequence for a
standalone PCIe card (p100a, p150a/b/c, or p300a/b/c).

.. warning::
   This flow is destructive. The test erases the DUT's SPI and reflashes the
   preflash and assembly test firmware, then flashes the production
   firmware bundle. A failure part-way through the sequence can leave
   the DUT in an intermediate state without PCIe enumeration. To recover the
   DUT, see ``scripts/tooling/blackhole_recovery/README.md``.

Building the artifacts locally
******************************

The test consumes four artifact sets that CI normally produces in separate
build jobs. To build them all locally instead of downloading them from a
previous CI run, use:

.. code-block:: shell

   scripts/build-manufacturing-artifacts.sh <board_name>

.. note::
   The ``artifacts/`` directory is created relative to your *current working
   directory*, and the test (below) looks for artifacts relative to *its*
   current working directory. Run the build and the test from the same
   directory so the paths line up. Running both from the ``tt-system-firmware``
   repository root (as assumed on this page) places the artifacts in
   ``artifacts/`` there and lets the test's default paths resolve.

   Alternatively, you can pass an explicit output directory to the build with
   ``-o <dir>`` and the matching paths to the test with ``-p``/``-m``/``-a``/``-f``:

   .. code-block:: shell

      scripts/build-manufacturing-artifacts.sh -o <dir> <board_name>
      scripts/ci/run-manufacturing-test.sh \
          -p <dir>/preflash \
          -m <dir>/assembly-mcuboot \
          -a <dir>/assembly-fw \
          -f <dir>/fwbundle \
          <board_name>

This derives the correct preflash revision and assembly test firmware family
for the DUT from ``scripts/ci/board-map.sh`` (for example, a p100a DUT uses the
p150a preflash and the p150 assembly test firmware), then builds and lays them
out under ``artifacts/``:

* ``artifacts/preflash/preflash-*.ihex``
* ``artifacts/assembly-mcuboot/zephyr.elf``
* ``artifacts/assembly-fw/zephyr.signed.hex``
* ``artifacts/fwbundle/fw_pack-local.fwbundle``

It also sets up the prerequisites the ``pyocd`` preflash step depends on: the
``STM32G0B1CEUx`` CMSIS pack and the Blackhole flash loader module (FLM, built
into ``scripts/tooling/blackhole_recovery/data/bh_flm/build/``).

To build the artifacts and immediately run the test, pass ``-r``:

.. code-block:: shell

   scripts/build-manufacturing-artifacts.sh -r <board_name>

Running the test
****************

Once the artifacts are in place, run the manufacturing test directly:

.. code-block:: shell

   scripts/ci/run-manufacturing-test.sh <board_name>

By default it looks for artifacts under ``artifacts/preflash``,
``artifacts/assembly-mcuboot``, ``artifacts/assembly-fw``, and
``artifacts/fwbundle``.
