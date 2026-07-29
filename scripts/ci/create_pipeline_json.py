#!/usr/bin/env python3
#
# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

"""
Collect CI pipeline metadata from a GitHub Actions workflow run and produce
a .json file conforming to the Pipeline pydantic model.

Usage:
    python create_pipeline_json.py \
        --run-id 12345678 \
        --repo tenstorrent/tt-system-firmware \
        [--run-attempt 1] \
        [--output-dir ./output]

Requires GITHUB_TOKEN environment variable for API access.
"""

import argparse
import csv
import io
import json
import logging
import os
import re
import sys
import yaml
import zipfile
from datetime import datetime
from pathlib import Path
from typing import Any, Optional
from xml.etree import ElementTree

import requests

from pydantic_models import (
    BoardProductType,
    Job,
    JobStatus,
    Pipeline,
    PipelineStatus,
    Step,
    StressTestRecord,
    Test,
    WorkflowType,
)

logging.basicConfig(level=logging.INFO, format="%(levelname)s: %(message)s")
logger = logging.getLogger(__name__)

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
GITHUB_API = "https://api.github.com"

# Default timeout (seconds) for network requests so a stalled connection
# cannot hang the data-production workflow indefinitely.
REQUEST_TIMEOUT = 60

# Maps workflow 'name' field to WorkflowType enum
WORKFLOW_NAME_MAP = {
    "HW Smoke": WorkflowType.hw_smoke,
    "HW Soak": WorkflowType.hw_soak,
    "Metal 🤘": WorkflowType.metal,
    "Unit": WorkflowType.unit,
    "Create a Release": WorkflowType.release,
}

# Board names from ci_boards.json, used to detect board from job name
CI_BOARDS_PATH = REPO_ROOT / ".github" / "ci_boards.json"


# GitHub API helpers


def gh_headers(token: str) -> dict:
    return {
        "Authorization": f"Bearer {token}",
        "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28",
    }


def gh_get(url: str, token: str, params: Optional[dict] = None) -> dict:
    resp = requests.get(
        url, headers=gh_headers(token), params=params, timeout=REQUEST_TIMEOUT
    )
    resp.raise_for_status()
    return resp.json()


def gh_get_paginated(
    url: str, token: str, key: str = "jobs", per_page: int = 100
) -> list:
    """Paginate a GitHub API endpoint that wraps results in {key: [...], total_count: N}."""
    results = []
    page = 1
    while True:
        data = gh_get(url, token, params={"per_page": per_page, "page": page})
        if not data:
            break
        items = data.get(key, []) if isinstance(data, dict) else data
        if not items:
            break
        results.extend(items)
        if len(items) < per_page:
            break
        page += 1
    return results


# Timestamp helpers


def parse_ts(ts_str: Optional[str]) -> Optional[datetime]:
    """Parse an ISO 8601 timestamp string, returning None if absent."""
    if not ts_str:
        return None
    return datetime.fromisoformat(ts_str.replace("Z", "+00:00"))


# Version extraction


def parse_version_file(version_file: Path) -> Optional[str]:
    """Parse a VERSION file into a version string (major.minor.patch)."""
    if not version_file.exists():
        return None
    parts = {}
    with open(version_file) as f:
        for line in f:
            if "=" in line:
                key, val = line.strip().split("=", 1)
                parts[key.strip()] = val.strip()
    major = parts.get("VERSION_MAJOR", "0")
    minor = parts.get("VERSION_MINOR", "0")
    patch = parts.get("PATCHLEVEL", "0")
    ver = f"{major}.{minor}.{patch}"
    extra = parts.get("EXTRAVERSION", "")
    if extra:
        ver += f"-{extra}"
    return ver


def get_firmware_versions() -> dict:
    """Extract firmware version triple from VERSION files in the repo."""
    return {
        "firmware_root_version": parse_version_file(REPO_ROOT / "VERSION"),
        "firmware_smc_version": parse_version_file(
            REPO_ROOT / "app" / "smc" / "VERSION"
        ),
        "firmware_dmc_version": parse_version_file(
            REPO_ROOT / "app" / "dmc" / "VERSION"
        ),
    }


# Board/runner detection


def load_ci_boards() -> list[dict]:
    if not CI_BOARDS_PATH.exists():
        return []
    with open(CI_BOARDS_PATH) as f:
        data = json.load(f)
    if isinstance(data, dict):
        return [
            {
                **entry,
                "product": product,
                "runs-on": entry.get("runs-on") or entry.get("board"),
            }
            for product, entries in data.items()
            for entry in entries
        ]
    return data


def detect_board_from_job(
    job_name: str, job_label: str, ci_boards: list[dict]
) -> tuple[Optional[str], Optional[BoardProductType], Optional[dict]]:
    """Detect the board from a job name or runner label using ci_boards.json.

    Returns (board_name, board_product_type, ci_boards_entry).
    """
    for entry in ci_boards:
        board_name = entry["board"]
        runs_on = entry.get("runs-on", "")
        if board_name in job_name or (runs_on and runs_on == job_label):
            product = entry.get("product")
            return (
                board_name,
                BoardProductType(product) if product else None,
                entry,
            )
    return None, None, None


# twister.json parsing


def parse_twister_json(
    data: dict, job_start_ts: Optional[datetime] = None
) -> tuple[list[Test], list[str], bool]:
    """Parse a twister.json file into Test objects and run metadata.

    Returns (tests, tags, device_testing) extracted from
    the twister environment block and test suites.
    """
    env = data.get("environment", {})
    options = env.get("options", {})
    tags = options.get("tag", [])
    device_testing = options.get("device_testing", False)
    base_ts = parse_ts(env.get("run_date")) or job_start_ts

    tests = []

    for suite in data.get("testsuites", []):
        suite_name = suite.get("name", "")
        platform = suite.get("platform", "")
        for tc in suite.get("testcases", []):
            identifier = tc.get("identifier", "")
            status = tc.get("status", "")
            reason = tc.get("reason", "")
            raw_et = tc.get("execution_time")
            execution_time = float(raw_et) if raw_et is not None else None

            success = status == "passed"
            skipped = status == "skipped"

            tests.append(
                Test(
                    test_start_ts=base_ts,
                    test_end_ts=base_ts,
                    test_execution_time=execution_time,
                    test_case_name=identifier,
                    filepath=suite.get("path", ""),
                    category=suite_name,
                    error_message=reason if reason and not success else None,
                    success=success,
                    skipped=skipped,
                    full_test_name=f"{platform}/{identifier}",
                    platform=platform,
                    build_only=status == "filtered",
                )
            )

    return tests, tags, device_testing


# JUnit XML parsing


def parse_junit_xml(xml_content: str, base_ts: Optional[datetime] = None) -> list[Test]:
    """Parse a JUnit XML file into Test objects.

    Returns an empty list (rather than raising) when the XML is malformed or
    when no base timestamp is available, so a single bad artifact cannot abort
    the entire pipeline generation. Test.test_start_ts/test_end_ts are required
    datetimes, so a valid base_ts is needed to build Test objects.
    """
    tests = []
    if base_ts is None:
        logger.warning("Skipping JUnit XML parse: no base timestamp available")
        return tests
    try:
        root = ElementTree.fromstring(xml_content)
    except ElementTree.ParseError as e:
        logger.warning("Skipping malformed JUnit XML: %s", e)
        return tests

    # Handle both <testsuites> and <testsuite> as root
    suites = root.findall(".//testsuite")
    if not suites and root.tag == "testsuite":
        suites = [root]

    for suite in suites:
        suite_name = suite.get("name", "")
        for tc in suite.findall("testcase"):
            tc_name = tc.get("name", "")
            classname = tc.get("classname", "")
            raw_time = tc.get("time")
            time_s = float(raw_time) if raw_time is not None else None

            failure = tc.find("failure")
            error = tc.find("error")
            skip = tc.find("skipped")

            success = failure is None and error is None and skip is None
            skipped = skip is not None
            error_msg = None
            if failure is not None:
                error_msg = failure.get("message", failure.text or "")[:500]
            elif error is not None:
                error_msg = error.get("message", error.text or "")[:500]

            tests.append(
                Test(
                    test_start_ts=base_ts,
                    test_end_ts=base_ts,
                    test_execution_time=time_s,
                    test_case_name=tc_name,
                    filepath=classname,
                    category=suite_name,
                    error_message=error_msg,
                    success=success,
                    skipped=skipped,
                    full_test_name=f"{classname}::{tc_name}" if classname else tc_name,
                    build_only=False,
                )
            )
    return tests


# recording.csv (stress tests) parsing


def parse_recording_csv(csv_content: str) -> list[StressTestRecord]:
    """Parse a stress test recording.csv file.

    Handles both column naming conventions:
      - snake_case: fail_count, test_name, total_tries
      - Title Case: Fail Count, Test Name, Total Tries, Failure Percentage
    """
    records = []
    reader = csv.DictReader(io.StringIO(csv_content))
    for row in reader:
        try:
            fail_count = int(row.get("fail_count", row.get("Fail Count", 0)))
            total_tries = int(row.get("total_tries", row.get("Total Tries", 0)))
            test_name = row.get("test_name", row.get("Test Name", ""))

            pct = None
            pct_str = row.get("Failure Percentage")
            if pct_str:
                pct = float(pct_str.strip("%"))
            elif total_tries > 0:
                pct = (fail_count / total_tries) * 100.0

            records.append(
                StressTestRecord(
                    test_name=test_name,
                    fail_count=fail_count,
                    total_tries=total_tries,
                    failure_percentage=pct,
                )
            )
        except (ValueError, KeyError) as e:
            logger.warning("Skipping malformed recording.csv row: %s", e)
    return records


# Artifact download & processing


def download_and_process_artifacts(
    repo: str, run_id: int, token: str
) -> dict[str, Any]:
    """Download artifacts from a workflow run and extract raw data.

    Returns a dict mapping artifact name to raw content for parsing.
    Raw content includes twister JSON dicts, JUnit XML strings, and CSV strings.
    """
    url = f"{GITHUB_API}/repos/{repo}/actions/runs/{run_id}/artifacts"
    artifacts = gh_get_paginated(url, token, key="artifacts", per_page=100)

    results: dict[str, Any] = {}

    for artifact in artifacts:
        name = artifact["name"]
        download_url = artifact["archive_download_url"]

        logger.info("Downloading artifact: %s", name)
        resp = requests.get(
            download_url, headers=gh_headers(token), timeout=REQUEST_TIMEOUT
        )
        resp.raise_for_status()

        with zipfile.ZipFile(io.BytesIO(resp.content)) as zf:
            raw = {"twister_jsons": [], "junit_xmls": [], "recording_csvs": []}

            for entry in zf.namelist():
                if entry.endswith("twister.json"):
                    try:
                        raw["twister_jsons"].append(json.loads(zf.read(entry)))
                    except (json.JSONDecodeError, KeyError) as e:
                        logger.warning("Failed to read %s in %s: %s", entry, name, e)

                elif entry.endswith(".xml"):
                    try:
                        raw["junit_xmls"].append(zf.read(entry).decode("utf-8"))
                    except Exception as e:
                        logger.warning("Failed to read %s in %s: %s", entry, name, e)

                elif entry.endswith("recording.csv"):
                    try:
                        raw["recording_csvs"].append(zf.read(entry).decode("utf-8"))
                    except Exception as e:
                        logger.warning("Failed to read %s in %s: %s", entry, name, e)

            results[name] = raw

    return results


def parse_artifact_for_job(
    artifact_raw: dict[str, Any], job_start_ts: datetime
) -> tuple[
    list[Test],
    list[StressTestRecord],
    list[str],
    bool,
]:
    """Parse raw artifact data into tests, stress records, and twister run metadata.

    Returns (tests, stress_records, tags, device_testing).
    """
    tests: list[Test] = []
    stress_records: list[StressTestRecord] = []
    tags: list[str] = []
    device_testing = False

    for twister_data in artifact_raw.get("twister_jsons", []):
        tw_tests, tw_tags, tw_dev = parse_twister_json(
            twister_data, job_start_ts=job_start_ts
        )
        tests.extend(tw_tests)
        tags = tw_tags or tags
        device_testing = device_testing or tw_dev

    for xml_content in artifact_raw.get("junit_xmls", []):
        tests.extend(parse_junit_xml(xml_content, base_ts=job_start_ts))

    for csv_content in artifact_raw.get("recording_csvs", []):
        stress_records.extend(parse_recording_csv(csv_content))

    return tests, stress_records, tags, device_testing


# Match artifacts to jobs


def build_upload_step_map(workflows_dir: Path) -> dict[str, list[str]]:
    """Parse workflow YAML files to map upload step names to artifact name
    templates.

    Returns a dict like:
        {"Upload SMC Smoke Tests": ["SMC Smoke test results (${{ matrix.config.board }})"]}
    """
    step_map: dict[str, list[str]] = {}

    for wf_path in sorted(workflows_dir.glob("*.yml")):
        try:
            with open(wf_path) as f:
                wf = yaml.safe_load(f)
        except Exception:
            continue
        if not wf or "jobs" not in wf:
            continue
        for job_def in wf["jobs"].values():
            for step in job_def.get("steps", []):
                if not isinstance(step, dict):
                    continue
                if "upload-artifact" in step.get("uses", ""):
                    step_name = step.get("name", "")
                    artifact_name = step.get("with", {}).get("name", "")
                    if step_name and artifact_name:
                        templates = step_map.setdefault(step_name, [])
                        if artifact_name not in templates:
                            templates.append(artifact_name)

    return step_map


def resolve_artifact_template(template: str, job_data: dict) -> str:
    """Resolve GitHub Actions expressions in an artifact name template.

    Resolves ${{ matrix.config.X }} and ${{ matrix.board }} from the job's
    detected ci_boards entry. Other expressions (e.g. ${{ env.X }} and
    ${{ steps.X.outputs.Y }}) cannot be reconstructed from the API data alone
    and are replaced with an empty string, so artifact names that depend on
    them will not resolve to a downloaded artifact.
    """

    # Extract matrix config from our detected board entry or job name
    def replacer(match: re.Match) -> str:
        expr = match.group(1).strip()

        # matrix.config.board, matrix.config.runs-on, etc.
        mc_match = re.match(r"matrix\.config\.(\S+)", expr)
        if mc_match:
            key = mc_match.group(1)
            matrix_config = job_data.get("_board_entry") or {}
            return str(matrix_config.get(key, ""))

        # matrix.board
        if expr == "matrix.board":
            board = job_data.get("_board_name", "")
            return board

        # For env vars and steps outputs, we can't reliably resolve these
        # from the API data alone, so return empty string
        return ""

    return re.sub(r"\$\{\{\s*(.+?)\s*\}\}", replacer, template)


def get_expected_artifacts_for_job(
    job_steps: list[dict],
    job_data: dict,
    upload_step_map: dict[str, list[str]],
    available_artifacts: set[str],
) -> list[str]:
    """Determine which artifacts a job uploaded by matching its steps against
    the upload step map parsed from workflow YAML files.

    When a step name maps to multiple artifact templates (e.g. two jobs share
    the same upload step name), all templates are resolved and only those that
    exist in the downloaded artifact data are returned.
    """
    expected = []
    for step in job_steps:
        step_name = step.get("name", "")
        for template in upload_step_map.get(step_name, []):
            resolved = resolve_artifact_template(template, job_data)
            if resolved and resolved in available_artifacts:
                expected.append(resolved)
    return expected


# Map GitHub API status to enums


def map_pipeline_status(
    status: str, conclusion: Optional[str]
) -> Optional[PipelineStatus]:
    if conclusion:
        try:
            return PipelineStatus(conclusion)
        except ValueError:
            pass
    if status == "completed":
        return PipelineStatus.completed
    return None


def map_job_status(conclusion: Optional[str]) -> Optional[JobStatus]:
    if conclusion:
        try:
            return JobStatus(conclusion)
        except ValueError:
            return JobStatus.unknown
    return JobStatus.unknown


# Main pipeline construction


def create_pipeline_json(
    repo: str,
    run_id: int,
    token: str,
    run_attempt: int = 1,
    output_dir: Optional[Path] = None,
) -> Path:
    """Fetch workflow run data from GitHub API and produce pipeline JSON."""

    # 1. Get workflow run metadata
    run_url = f"{GITHUB_API}/repos/{repo}/actions/runs/{run_id}"
    if run_attempt > 1:
        run_url += f"/attempts/{run_attempt}"
    run_data = gh_get(run_url, token)

    workflow_name = run_data.get("name", "")
    workflow_type = WORKFLOW_NAME_MAP.get(workflow_name)

    logger.info(
        "Processing workflow: %s (run %d, attempt %d)",
        workflow_name,
        run_id,
        run_attempt,
    )

    # 2. Get jobs for this run
    jobs_url = (
        f"{GITHUB_API}/repos/{repo}/actions/runs/{run_id}/attempts/{run_attempt}/jobs"
    )
    jobs_data = gh_get_paginated(jobs_url, token)

    # 3. Download and process artifacts
    artifact_data = download_and_process_artifacts(repo, run_id, token)

    # 4. Load ci_boards.json for board detection
    ci_boards = load_ci_boards()

    # 5. Get firmware versions from the checked-out repo
    fw_version = get_firmware_versions()

    # 6. Parse workflow yamls get mapping of build upload step to artifact name
    workflows_dir = REPO_ROOT / ".github" / "workflows"
    upload_step_map = build_upload_step_map(workflows_dir)

    # Runner labels that ci_boards.json keys off of, used to pick the
    # meaningful label out of GitHub's generic ones (self-hosted, Linux, X64).
    ci_boards_runs_on = {
        entry.get("runs-on") for entry in ci_boards if entry.get("runs-on")
    }

    # Run-level timestamp fallback, guaranteed present for any real run, so a
    # job missing its own started_at (e.g. skipped jobs) still yields a valid
    # timestamp for building Test records.
    run_start_fallback = parse_ts(
        run_data.get("run_started_at") or run_data.get("created_at")
    )

    # 7. Build job models
    firmware_jobs = []
    for j in jobs_data:
        job_name = j.get("name", "")
        runner_label = ""
        runner_data = j.get("runner_name") or ""
        labels = j.get("labels", [])
        if labels:
            # Prefer a label that matches a ci_boards "runs-on" value; fall back
            # to the first label only if none match.
            runner_label = next(
                (label for label in labels if label in ci_boards_runs_on),
                labels[0],
            )

        # Detect board from job name/runner label
        board_name, board_product_type, board_entry = detect_board_from_job(
            job_name, runner_label, ci_boards
        )

        # KMD version from ci_boards.json
        kmd_ver = board_entry.get("kmd_build") if board_entry else None

        # Metal target
        metal_target = board_entry.get("metal-target") if board_entry else None

        # Parse timestamps
        started_at = j.get("started_at") or run_data.get("run_started_at")
        completed_at = j.get("completed_at") or run_data.get("updated_at")
        created_at = j.get("created_at") or run_data.get("created_at")

        # Build steps
        raw_steps = j.get("steps", [])
        steps = []
        for idx, s in enumerate(raw_steps):
            steps.append(
                Step(
                    name=s.get("name"),
                    status=s.get("status"),
                    conclusion=s.get("conclusion"),
                    number=s.get("number", idx + 1),
                    started_at=parse_ts(s.get("started_at")),
                    completed_at=parse_ts(s.get("completed_at")),
                )
            )

        conclusion = j.get("conclusion")

        # Match artifacts to this job and extract test data + twister metadata
        job_tests: list[Test] = []
        job_stress_records: list[StressTestRecord] = []
        twister_tag: Optional[str] = None
        device_testing = False

        job_resolve_data = {
            "_board_entry": board_entry,
            "_board_name": board_name or "",
        }
        expected_artifacts = get_expected_artifacts_for_job(
            raw_steps,
            job_resolve_data,
            upload_step_map,
            set(artifact_data.keys()),
        )
        job_start = parse_ts(started_at) or run_start_fallback
        for art_name in expected_artifacts:
            art_raw = artifact_data.get(art_name)
            if art_raw:
                tests, stress, tags, dev_test = parse_artifact_for_job(
                    art_raw, job_start
                )
                job_tests.extend(tests)
                job_stress_records.extend(stress)
                if tags:
                    twister_tag = tags[0]
                device_testing = device_testing or dev_test
            else:
                logger.debug(
                    "Expected artifact %r not found for job %r", art_name, job_name
                )

        firmware_jobs.append(
            Job(
                github_job_id=j.get("id"),
                github_job_link=j.get("html_url"),
                name=job_name,
                job_submission_ts=parse_ts(created_at),
                job_start_ts=parse_ts(started_at),
                job_end_ts=parse_ts(completed_at),
                job_success=conclusion == "success",
                job_status=map_job_status(conclusion),
                job_label=runner_label or None,
                is_build_job=not device_testing and not job_tests,
                job_matrix_config=board_entry,
                host_name=runner_data or None,
                card_type=board_product_type.value if board_product_type else None,
                os=None,
                location=None,
                runner_label=runner_label or None,
                board_name=board_name,
                board_product_type=board_product_type,
                twister_tag=twister_tag,
                kmd_version=kmd_ver,
                metal_target=metal_target,
                tests=job_tests,
                steps=steps,
                stress_test_records=job_stress_records or None,
            )
        )

    # 8. Build pipeline model
    run_started = run_data.get("run_started_at") or run_data.get("created_at")
    run_updated = run_data.get("updated_at") or run_started
    run_created = run_data.get("created_at") or run_started

    conclusion = run_data.get("conclusion")
    status = run_data.get("status", "")

    pipeline = Pipeline(
        github_pipeline_id=run_id,
        github_pipeline_link=run_data.get("html_url"),
        pipeline_submission_ts=parse_ts(run_created),
        pipeline_start_ts=parse_ts(run_started),
        pipeline_end_ts=parse_ts(run_updated),
        pipeline_status=map_pipeline_status(status, conclusion),
        name=workflow_name,
        project="tt-system-firmware",
        trigger=run_data.get("event"),
        vcs_platform="github",
        repository_url=run_data.get("repository", {}).get(
            "html_url", "https://github.com/tenstorrent/tt-system-firmware"
        ),
        git_branch_name=run_data.get("head_branch"),
        git_commit_hash=run_data.get("head_sha", ""),
        git_author=run_data.get("actor", {}).get("login", ""),
        orchestrator="github_actions",
        workflow_type=workflow_type,
        **fw_version,
        jobs=firmware_jobs,
    )

    # 9. Write output
    if output_dir is None:
        output_dir = Path(".")
    output_dir.mkdir(parents=True, exist_ok=True)

    ts = pipeline.pipeline_start_ts.strftime("%Y%m%dT%H%M%SZ")
    filename = f"pipeline_{run_id}_a{run_attempt}_{ts}.json"
    output_path = output_dir / filename

    with open(output_path, "w") as f:
        f.write(pipeline.model_dump_json(indent=2))

    logger.info("Pipeline JSON written to %s", output_path)
    return output_path


def main():
    parser = argparse.ArgumentParser(
        description="Produce pipeline JSON from a GitHub Actions workflow run.",
        allow_abbrev=False,
    )
    parser.add_argument(
        "--run-id", type=int, required=True, help="GitHub Actions workflow run ID."
    )
    parser.add_argument(
        "--repo",
        type=str,
        default="tenstorrent/tt-system-firmware",
        help="GitHub repository (owner/name).",
    )
    parser.add_argument(
        "--run-attempt", type=int, default=1, help="Workflow run attempt number."
    )
    parser.add_argument(
        "--output-dir", type=str, default=".", help="Output directory for JSON."
    )
    args = parser.parse_args()

    token = os.environ.get("GITHUB_TOKEN")
    if not token:
        logger.error("GITHUB_TOKEN environment variable is required.")
        sys.exit(1)

    output_path = create_pipeline_json(
        repo=args.repo,
        run_id=args.run_id,
        token=token,
        run_attempt=args.run_attempt,
        output_dir=Path(args.output_dir),
    )
    print(output_path)


if __name__ == "__main__":
    main()
