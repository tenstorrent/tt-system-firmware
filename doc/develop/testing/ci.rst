.. _ttzp_ci_tests:

Continuous Integration Tests
============================

GitHub Actions automatically builds and validates changes on every pull request
and runs extended hardware suites every night.

Per-PR Checks
-------------

Every pull request (and merge queue run) is validated across four categories:

* **Firmware & Tooling Builds** (`build-fw.yml <https://github.com/tenstorrent/tt-system-firmware/blob/main/.github/workflows/build-fw.yml>`_, `build-native.yml <https://github.com/tenstorrent/tt-system-firmware/blob/main/.github/workflows/build-native.yml>`_):
  Builds SMC and DMC firmware bundles for all Blackhole board revisions in
  `boards.json <https://github.com/tenstorrent/tt-system-firmware/blob/main/.github/boards.json>`_,
  along with recovery, preflash, and assembly-test images, Mimir and Grendel
  targets, build-only Twister configurations, and host tooling
  (``tt-fw-terminal``, ``tt-tracing``). Compiler warnings are treated as errors.
* **Simulated Unit Tests** (`run-unit-tests.yml <https://github.com/tenstorrent/tt-system-firmware/blob/main/.github/workflows/run-unit-tests.yml>`_):
  Runs Twister unit and simulation test suites on ``unit_testing`` and
  ``native_sim`` targets without requiring physical hardware.
* :ref:`Hardware Smoke & End-to-End Tests <ttzp_suite_smoke_e2e>` (`hardware-smoke.yml <https://github.com/tenstorrent/tt-system-firmware/blob/main/.github/workflows/hardware-smoke.yml>`_):
  Runs on-target DMC/SMC smoke tests and production firmware end-to-end tests
  on Blackhole cards (``p100a``, ``p150a``, ``p300a``). Wormhole (``n150``)
  end-to-end tests also run when the ``wh-tests`` label is applied or Wormhole
  firmware blobs change.
* **Static, Compliance & Doc Checks** (`compliance.yml <https://github.com/tenstorrent/tt-system-firmware/blob/main/.github/workflows/compliance.yml>`_, `copyright-check.yml <https://github.com/tenstorrent/tt-system-firmware/blob/main/.github/workflows/copyright-check.yml>`_, `license-check.yml <https://github.com/tenstorrent/tt-system-firmware/blob/main/.github/workflows/license-check.yml>`_, `verify-blob.yml <https://github.com/tenstorrent/tt-system-firmware/blob/main/.github/workflows/verify-blob.yml>`_, `docs.yml <https://github.com/tenstorrent/tt-system-firmware/blob/main/.github/workflows/docs.yml>`_):
  Checks code formatting (``clang-format``), Zephyr compliance and Kconfig
  rules, copyright headers, license scanning, binary blob checksums, and Sphinx
  documentation builds.

Nightly Tests
-------------

Longer-running hardware suites run on a nightly schedule against ``main``. They
can also be triggered on a pull request by adding the corresponding label or
via manual workflow dispatch.

* :ref:`Hardware Soak & Manufacturing <ttzp_suite_soak>` (`hardware-long.yml <https://github.com/tenstorrent/tt-system-firmware/blob/main/.github/workflows/hardware-long.yml>`_ — nightly at 05:00 UTC, or PR label ``long-tests``):
  End-to-end stress runs across all Blackhole systems in
  `ci_boards.json <https://github.com/tenstorrent/tt-system-firmware/blob/main/.github/ci_boards.json>`_
  (``p100a``, ``p150a``, ``p300a``, ``quietbox2``, ``loudbox``,
  ``bh-galaxy-revc``), assertion-enabled firmware smoke runs and manufacturing
  bring-up on ``p100a``, ``p150a``, and ``p300a``, and Tensix column validation
  on ``p150a``.

* **tt-metal Integration** (`metal.yml <https://github.com/tenstorrent/tt-system-firmware/blob/main/.github/workflows/metal.yml>`_ — nightly at 06:00 UTC, or PR label ``metal-tests``):
  Runs upstream ``tt-metal`` test suites along with DRAM and PCIe deployment
  tests across all Blackhole cards and systems (``p100a``, ``p150a``,
  ``p300a``, ``quietbox2``, ``loudbox``, ``bh-galaxy-revc``), plus Ethernet
  deployment tests on ``bh-galaxy-revc``.

Further Details
---------------

* :ref:`Hardware Test Suites <ttzp_test_suites>` — Breakdown of the cases in
  the smoke, end-to-end, stress, and manufacturing suites.
* :ref:`Local Hardware Testing <ttzp_local_testing>` — Instructions for
  running those suites on a developer machine.
* `boards.json <https://github.com/tenstorrent/tt-system-firmware/blob/main/.github/boards.json>`_
  and `ci_boards.json <https://github.com/tenstorrent/tt-system-firmware/blob/main/.github/ci_boards.json>`_
  — Board matrices that control which revisions are built and which hardware
  runners execute each suite.
* `GitHub Actions Workflows <https://github.com/tenstorrent/tt-system-firmware/tree/main/.github/workflows>`_
  — Full workflow definitions, including release packaging and repository
  maintenance jobs.
