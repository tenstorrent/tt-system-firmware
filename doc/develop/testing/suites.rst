.. _ttzp_test_suites:

Hardware Test Suites
====================

This page describes the test content of the hardware suites run by
:ref:`Continuous Integration Tests <ttzp_ci_tests>`. For instructions on
running the same suites on a developer machine, see
:ref:`Local Hardware Testing <ttzp_local_testing>`.

.. _ttzp_suite_smoke_e2e:

Hardware Smoke & End-to-End Suite
---------------------------------

Triggered on every pull request and merge queue run by
`hardware-smoke.yml <https://github.com/tenstorrent/tt-system-firmware/blob/main/.github/workflows/hardware-smoke.yml>`_.

On-Target Smoke Tests
*********************

``scripts/ci/run-smoke.sh`` runs Twister with the ``smoke`` tag against Zephyr
tests and samples plus this repository's tests and samples, using the overlays
in ``test-conf``. CI runs the DMC suite, then the SMC suite, on Blackhole
cards with ``supports_smoke`` set in
`ci_boards.json <https://github.com/tenstorrent/tt-system-firmware/blob/main/.github/ci_boards.json>`_
(``p100a``, ``p150a``, ``p300a``).

* **DMC smoke**: GPIO expansion (``gpiox`` / PCA9554/PCA9555), MAX6639
  fan/thermal control, and PWM API coverage.
* **SMC smoke**: Clock control, PVT sensors, ``bh_fwtable`` (build-only),
  ``noc2axi_broadcast``, GPIO, SPI flash (common and performance), and DMA
  transfers (scatter-gather, loop, channel block-length, and channel-link).

Blackhole End-to-End Smoke
**************************

``scripts/ci/run-e2e.sh`` runs Twister with the ``e2e-flash`` tag, which
selects ``app.e2e-smoke-flash`` in ``app/smc/sample.yaml`` and executes
``app/smc/pytest/e2e_smoke.py`` and the smoke cases in
``app/smc/pytest/e2e_bh_mod.py`` against production firmware flashed with
``tt-flash``:

.. automodule:: smc.pytest.e2e_smoke
   :members:
   :undoc-members:
   :noindex:

.. automodule:: smc.pytest.e2e_bh_mod
   :members:
   :pytest-mark: smoke
   :undoc-members:
   :noindex:

Wormhole End-to-End Smoke
*************************

``app/wh/pytest/e2e_smoke.py`` runs on Wormhole entries with
``supports_smoke`` set (``n150``) when the pull request has the ``wh-tests``
label or changes ``zephyr/blobs/fw_pack-wormhole.tar.gz``
(``test_upgrade_from_19_0`` is skipped in CI):

.. automodule:: wh.pytest.e2e_smoke
   :members:
   :undoc-members:
   :noindex:

.. _ttzp_suite_soak:

Hardware Soak & Manufacturing Suite
-----------------------------------

Triggered nightly at 05:00 UTC, by the ``long-tests`` pull request label, or
via manual workflow dispatch by
`hardware-long.yml <https://github.com/tenstorrent/tt-system-firmware/blob/main/.github/workflows/hardware-long.yml>`_.

End-to-End Stress
*****************

``scripts/ci/run-stress.sh`` runs Twister with the ``e2e-stress`` tag across
all Blackhole boards in ``ci_boards.json`` (``p100a``, ``p150a``, ``p300a``,
``quietbox2``, ``loudbox``, ``bh-galaxy-revc``). That tag covers:

* ``app.stress-test`` (``app/smc/pytest/e2e_stress.py`` and the stress cases
  in ``app/smc/pytest/e2e_bh_mod.py``): Multi-iteration stress tests. Per-test
  failure-rate CSVs (``recording.csv``) are uploaded for the CI stats report.
* ``app.e2e-prod-assert``: The full ``e2e_smoke.py`` and ``e2e_bh_mod.py``
  smoke suite against production firmware compiled with ``CONFIG_ASSERT=y``
  (on ``p100a``, ``p150a``, and ``p300a``).

.. automodule:: smc.pytest.e2e_stress
   :members:
   :undoc-members:
   :noindex:

.. automodule:: smc.pytest.e2e_bh_mod
   :members:
   :pytest-mark: stress
   :undoc-members:
   :noindex:

Tensix Column Validation
************************

Runs on the ``p150a`` runner. The job installs
``tt-update-tensix-disable-count``, patches the combined firmware bundle,
flashes it with ``tt-flash``, and verifies via ``pyluwen`` telemetry that all
140 Tensix columns are enabled.

Manufacturing Bring-Up
**********************

``scripts/ci/run-manufacturing-test.sh`` runs the factory bring-up sequence on
standalone PCIe cards (``p100a``, ``p150a``, ``p300a``): SPI flash erase,
STM32 preflash image, assembly MCUBoot and assembly-test firmware
(``CONFIG_TT_ASSEMBLY_TEST=y``), then the production firmware bundle. See
:ref:`Local Hardware Testing <ttzp_local_testing>` for building the artifacts
and running the same flow locally.
