#!/usr/bin/env python3
#
# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

"""
Definition of the pydantic models used by tt-system-firmware CI for data
production.
"""

from datetime import datetime
from enum import Enum
from typing import List, Optional

from pydantic import BaseModel, Field


class WorkflowType(str, Enum):
    """CI workflows that produce data, matching .github/workflows/ names."""

    hw_smoke = "hw_smoke"
    hw_soak = "hw_soak"
    metal = "metal"
    unit = "unit"
    release = "release"


class BoardProductType(str, Enum):
    """Product types from ci_boards.json 'product' field."""

    bh = "bh"  # Blackhole
    wh = "wh"  # Wormhole


class PipelineStatus(str, Enum):
    success = "success"
    failure = "failure"
    skipped = "skipped"
    cancelled = "cancelled"
    neutral = "neutral"
    timed_out = "timed_out"
    action_required = "action_required"
    completed = "completed"
    stale = "stale"


class JobStatus(str, Enum):
    success = "success"
    failure = "failure"
    skipped = "skipped"
    cancelled = "cancelled"
    neutral = "neutral"
    unknown = "unknown"
    timed_out = "timed_out"
    action_required = "action_required"


class Step(BaseModel):
    """A single step within a CI/CD job."""

    name: Optional[str] = Field(description="Name of the step.")
    status: Optional[str] = Field(description="Status of the step.")
    conclusion: Optional[str] = Field(description="Conclusion of the step.")
    number: int = Field(description="Step number.")
    started_at: Optional[datetime] = Field(
        description="Timestamp when the step started."
    )
    completed_at: Optional[datetime] = Field(
        description="Timestamp when the step ended."
    )


class Test(BaseModel):
    """A single test from twister.json output.

    Twister tests are identified by platform + test scenario name.
    Platform strings look like 'tt_blackhole@p150a/tt_blackhole/smc'.
    Test names come from sample.yaml (e.g. 'app.e2e-smoke-flash').
    """

    test_start_ts: datetime = Field(
        description="Timestamp when the test execution started. "
        "Set to the twister run_date when exact start time is unavailable.",
    )
    test_end_ts: datetime = Field(
        description="Timestamp when the test execution ended. "
        "Set to the twister run_date when exact end time is unavailable.",
    )
    test_execution_time: Optional[float] = Field(
        None,
        description="Test execution duration in seconds (from twister execution_time).",
    )
    test_case_name: str = Field(
        description="Twister test scenario name (e.g. 'app.e2e-smoke-flash')."
    )
    filepath: str = Field(description="Test file path and name.")
    category: str = Field(description="Name of the test category.")
    group: Optional[str] = Field(None, description="Name of the test group.")
    owner: Optional[str] = Field(None, description="Developer of the test.")
    error_message: Optional[str] = Field(
        None, description="Succinct error string, such as exception type."
    )
    success: bool = Field(description="Test execution success.")
    skipped: bool = Field(description="Whether the test was skipped.")
    full_test_name: str = Field(description="Test name plus config.")
    platform: Optional[str] = Field(
        None,
        description="Zephyr platform string (e.g. 'tt_blackhole@p150a/tt_blackhole/smc').",
    )
    build_only: Optional[bool] = Field(
        None,
        description="Whether this was a build-only test (no execution on hardware).",
    )


class StressTestRecord(BaseModel):
    """A single row from a stress test recording.csv file.

    Stress tests (e2e-stress tag) produce recording.csv with per-subtest
    pass/fail counts.
    """

    test_name: str = Field(description="Name of the stress sub-test.")
    fail_count: int = Field(description="Number of failures.")
    total_tries: int = Field(description="Total attempts.")
    failure_percentage: Optional[float] = Field(
        None, description="Failure percentage (0-100)."
    )


class Job(BaseModel):
    """A single CI/CD job within a firmware pipeline."""

    github_job_id: Optional[int] = Field(
        None, description="GitHub Actions job identifier."
    )
    github_job_link: Optional[str] = Field(
        None, description="Link to the GitHub Actions job."
    )
    name: str = Field(description="Name of the job.")
    job_submission_ts: Optional[datetime] = Field(
        None, description="Timestamp when the job was submitted."
    )
    job_start_ts: Optional[datetime] = Field(
        None, description="Timestamp when the job execution started."
    )
    job_end_ts: Optional[datetime] = Field(
        None, description="Timestamp when the job execution ended."
    )
    job_success: bool = Field(description="Job execution success.")
    job_status: Optional[JobStatus] = Field(None, description="Job execution status.")
    job_label: Optional[str] = Field(
        None, description="GitHub CI runner label for the job."
    )
    tt_smi_version: Optional[str] = Field(
        None,
        description="Version of tt-smi tool (base schema compatibility).",
    )
    docker_image: Optional[str] = Field(
        None,
        description="Docker/container image used (e.g. ghcr.io/tenstorrent/tt-metal/...).",
    )
    is_build_job: bool = Field(description="Whether this job is a software build.")
    job_matrix_config: Optional[dict] = Field(
        None, description="Full matrix config from ci_boards.json for this job."
    )
    host_name: Optional[str] = Field(description="Unique host name.")
    card_type: Optional[str] = Field(description="Card type and version.")
    os: Optional[str] = Field(description="Operating system of the host.")
    location: Optional[str] = Field(description="Host location.")
    failure_signature: Optional[str] = Field(None, description="Failure signature.")
    failure_description: Optional[str] = Field(None, description="Failure description.")
    runner_label: Optional[str] = Field(
        None, description="GitHub CI runner label (e.g. 'p150a')."
    )
    board_name: Optional[str] = Field(
        None, description="Board identifier (e.g. 'p150a', 'bh-galaxy', 'n150')."
    )
    board_product_type: Optional[BoardProductType] = Field(
        None,
        description="Product type from ci_boards.json ('bh' for Blackhole, 'wh' for Wormhole).",
    )
    board_revision: Optional[str] = Field(
        None, description="Board revision (e.g. 'revc' for galaxy_revc)."
    )
    twister_tag: Optional[str] = Field(
        None,
        description="Twister tag filter used for this job (e.g. 'e2e-flash', 'smoke').",
    )
    kmd_version: Optional[str] = Field(
        None,
        description="KMD version used for this job (from ci_boards.json kmd_build).",
    )
    metal_target: Optional[str] = Field(
        None,
        description="Metal test target (e.g. 'blackhole', 'blackhole_p300', 'blackhole_glx').",
    )

    tests: List[Test] = Field(default_factory=list)
    steps: Optional[List[Step]] = Field(None, description="Steps of the job.")
    stress_test_records: Optional[List[StressTestRecord]] = Field(
        None,
        description="Stress test recording.csv data, if this is a stress test job.",
    )


class Pipeline(BaseModel):
    """Top-level model for a tt-system-firmware CI/CD pipeline run."""

    github_pipeline_id: Optional[int] = Field(
        None, description="GitHub Actions workflow run identifier."
    )
    github_pipeline_link: Optional[str] = Field(
        None, description="Link to the GitHub Actions workflow run."
    )
    pipeline_submission_ts: datetime = Field(
        description="Timestamp when the pipeline was submitted."
    )
    pipeline_start_ts: datetime = Field(
        description="Timestamp when the pipeline execution started."
    )
    pipeline_end_ts: datetime = Field(
        description="Timestamp when the pipeline execution ended."
    )
    pipeline_status: Optional[PipelineStatus] = Field(
        None, description="Pipeline execution status."
    )
    name: str = Field(description="Name of the pipeline/workflow.")
    project: Optional[str] = Field(None, description="Software project name.")
    trigger: Optional[str] = Field(
        None,
        description="Trigger type (push, pull_request, schedule, merge_group, workflow_dispatch).",
    )
    vcs_platform: Optional[str] = Field(None, description="Version control platform.")
    repository_url: str = Field(description="URL of the code repository.")
    git_branch_name: Optional[str] = Field(
        description="Git branch tested by the pipeline."
    )
    git_commit_hash: str = Field(description="Git commit that triggered the pipeline.")
    git_author: str = Field(description="Author of the Git commit.")
    orchestrator: Optional[str] = Field(
        None, description="CI/CD orchestration platform."
    )
    workflow_type: Optional[WorkflowType] = Field(
        None,
        description="Which CI workflow produced this pipeline.",
    )
    firmware_root_version: Optional[str] = Field(
        None,
        description="Root firmware version (from VERSION file, e.g. '19.8.99').",
    )
    firmware_smc_version: Optional[str] = Field(
        None,
        description="SMC firmware version (from app/smc/VERSION, e.g. '0.30.99').",
    )
    firmware_dmc_version: Optional[str] = Field(
        None,
        description="DMC firmware version (from app/dmc/VERSION, e.g. '0.24.99').",
    )
    jobs: List[Job] = Field(default_factory=list)
