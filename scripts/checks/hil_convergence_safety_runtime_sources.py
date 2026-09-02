# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Publish digest-rebound image-supervisor sources for runtime proofs."""

from __future__ import annotations

import hashlib
import re
from pathlib import Path

CASES_PIN_PATTERN = re.compile(r'CASES_RAW_SHA256 = "[0-9a-f]{64}"')
PROCESS_PIN_PATTERN = re.compile(r'PROCESS_RAW_SHA256 = "[0-9a-f]{64}"')
SOURCE_MODE = 0o644


class RuntimeSourceError(ValueError):
    """Report ambiguous runtime source binding or publication."""


def _rebind_pin(source: str, payload: str, pattern: re.Pattern[str], name: str) -> str:
    """Replace one exact embedded source digest or refuse ambiguous bytes."""
    replacement = f'{name}_RAW_SHA256 = "{hashlib.sha256(payload.encode()).hexdigest()}"'
    rebound, count = pattern.subn(replacement, source, count=1)
    if count != 1 or len(pattern.findall(source)) != 1:
        message = f"supervisor {name.lower()} digest assignment is not unique"
        raise RuntimeSourceError(message)
    return rebound


def publish(
    root: Path,
    supervisor: str,
    process_source: str,
    cases_source: str,
) -> tuple[Path, Path, Path]:
    """Write one exact main/process/cases source bundle under a private root."""
    rebound = _rebind_pin(supervisor, process_source, PROCESS_PIN_PATTERN, "PROCESS")
    rebound = _rebind_pin(rebound, cases_source, CASES_PIN_PATTERN, "CASES")
    main_path = root / "supervisor.py"
    process_path = root / "supervisor_process.py"
    cases_path = root / "supervisor_cases.py"
    main_path.write_text(rebound, encoding="utf-8")
    process_path.write_text(process_source, encoding="utf-8")
    cases_path.write_text(cases_source, encoding="utf-8")
    for path in (main_path, process_path, cases_path):
        path.chmod(SOURCE_MODE)
    return main_path, process_path, cases_path
