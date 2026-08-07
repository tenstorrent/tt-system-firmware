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
