# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Selftest fixtures for the privileged raw-digest runtime cases."""

from __future__ import annotations

import hil_convergence_safety_image_lock_digest as digest

INPUT_BY_PATH = {
    digest.DEVCONTAINER_IMAGE_PATH: "devcontainer_image",
    digest.DEVCONTAINER_IMAGE_LOCK_RECEIPTS_PATH: "devcontainer_image_lock_receipts",
    digest.DEVCONTAINER_IMAGE_LOCK_SELFTEST_PATH: "devcontainer_image_lock_selftest",
    digest.DEVCONTAINER_IMAGE_SELFTEST_PATH: "devcontainer_image_selftest",
    digest.DEVCONTAINER_IMAGE_BOUND_EXIT_SELFTEST_PATH: ("devcontainer_image_bound_exit_selftest"),
    digest.DEVCONTAINER_IMAGE_SELFTEST_CASES_PATH: "devcontainer_image_selftest_cases",
    digest.DEVCONTAINER_IMAGE_SIGNAL_SELFTEST_PATH: "devcontainer_image_signal_selftest",
    digest.DEVCONTAINER_IMAGE_SELFTEST_PROCESS_PATH: "devcontainer_image_selftest_process",
    digest.DEVCONTAINER_IMAGE_SELFTEST_SUPERVISOR_PATH: ("devcontainer_image_selftest_supervisor"),
    digest.DEVCONTAINER_IMAGE_SELFTEST_SUPERVISOR_CASES_PATH: (
        "devcontainer_image_selftest_supervisor_cases"
    ),
    digest.RAW_DIGEST_CONTROLS_PATH: "raw_digest_controls",
}

PRE_CLOSE_FAILURE = "injected pre-close observer failure"
ROOT_OPEN_FAILURE = "injected root-open observer failure"
