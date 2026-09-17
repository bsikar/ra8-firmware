# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Focused transport fixtures shared by the hook-parity selftest."""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path
from typing import Any

from scripts.dev.git_environment import sanitized_git_environment


def _write(path: Path, text: str, *, executable: bool = False) -> None:
    """Write one private fixture file."""
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    if executable:
        path.chmod(0o755)


def transport_gate_text() -> str:
    """Return the real-Just fixture gate used to exercise owner transport."""
    return """#!/bin/bash -p
set -euo pipefail
[[ "${RA8_TRANSPORT_FIXTURE:-0}" == "1" && -f policy-mode ]] || exit 64
mode="$(<policy-mode)"
case "$mode" in
  success)
    [[ "$PATH" == *":${RA8_SELFTEST_VENV_BIN:?}:"* ]]
    ;;
  inspect)
    [[ -f "path with spaces/added.txt" && ! -e delete-me && ! -e resurrect-me ]]
    [[ -x mode.sh && -L alias && "$(readlink alias)" == link-target ]]
    [[ -f ignored-dir/tracked.txt && ! -e untracked.txt && "${OLDPWD:-}" == "$PWD" ]]
    ;;
  failure) exit 42 ;;
  hang)
    printf 'ready\\n' >"${RA8_SELFTEST_READY:?}"
    trap 'exit 0' HUP INT QUIT TERM
    sleep 60
    printf 'continued\\n' >"${RA8_SELFTEST_CONTINUED:?}"
    ;;
  *) exit 64 ;;
esac
"""


def write_transport_justfiles(root: Path) -> None:
    """Install a minimal real-Just module graph around the audited recipe."""
    root_just = """set shell := ["/bin/bash", "-puc"]
export BASH_ENV := "/dev/null"
export ENV := "/dev/null"
export PYTHONHOME := ""
export PYTHONPATH := ""
mod git_hooks "just/hooks.just"
mod quality "quality.just"
hooks:
    /bin/bash -p scripts/git/install-hooks.sh
"""
    quality = 'set working-directory := "."\nmod local "quality_local.just"\n'
    quality_local = """set working-directory := "."
gate name:
    /bin/bash -p fixture/bin/transport_gate.sh "{{ name }}"
"""
    _write(root / "justfile", root_just)
    _write(root / "quality.just", quality)
    _write(root / "quality_local.just", quality_local)
    _write(root / "fixture/bin/transport_gate.sh", transport_gate_text(), executable=True)
    for name in (
        "check_mcdc_block.py",
        "check_new_compound_has_mcdc.py",
        "check_obsolete_standards.py",
    ):
        _write(
            root / f"scripts/checks/{name}",
            "#!/usr/bin/python3\nraise SystemExit(0)\n",
            executable=True,
        )


def install_venv_wrappers(root: Path, marker: Path) -> None:
    """Install ignored mutable owner-tool wrappers that must not run."""
    # bash and just only. A python3 wrapper cannot prove anything here: the
    # candidate policy is deliberately allowed to use the ignored .venv after
    # immutable validation, so the marker would fire on designed behaviour.
    # The owner-side property is pinned structurally instead -- see the
    # OWNER_PYTHON tokens in check_hook_parity._check_snapshot_dispatch.
    for name, target in (
        ("bash", "/bin/bash -p"),
        ("just", "just"),
    ):
        text = f'#!/bin/sh\nprintf "invoked\\n" >>"$RA8_SELFTEST_VENV"\nexec {target} "$@"\n'
        _write(root / f".venv/bin/{name}", text, executable=True)
    marker.unlink(missing_ok=True)


def transport_environment(_base: Path, root: Path) -> dict[str, str]:
    """Return the production-like environment for one transport fixture."""
    environment = sanitized_git_environment()
    environment["RA8_TRANSPORT_FIXTURE"] = "1"
    environment["RA8_SELFTEST_VENV_BIN"] = str(root / ".venv/bin")
    return environment


def run_bootstrap_validator_case(
    base: Path,
    callbacks: tuple[Callable[..., Any], ...],
) -> None:
    """Prove an older HEAD permits only an exact 6/27 policy population."""
    make_fixture, git, source_state, run_owner, fail = callbacks
    root, temp_root = base / "bootstrap", base / "bootstrap-tmp"
    root.mkdir()
    temp_root.mkdir()
    make_fixture(root, "success", "success")
    validator = root / "scripts/dev/git_environment.py"
    current = validator.read_text(encoding="utf-8")
    legacy = current.replace('"--check-attributes"', '"--check-" "attributes"')
    legacy = legacy.replace(
        'parser.error("--commit requires --check-attributes")',
        'parser.error("--commit requires attribute checking")',
    )
    if legacy == current or "--check-attributes" in legacy:
        fail("bootstrap selftest did not hide the new validator capability from HEAD")
    _write(validator, legacy, executable=True)
    git(root, "add", str(validator.relative_to(root)))
    git(root, "commit", "--quiet", "--amend", "--no-edit")
    _write(validator, current, executable=True)
    git(root, "add", str(validator.relative_to(root)))
    marker = base / "bootstrap.venv"
    install_venv_wrappers(root, marker)
    environment = transport_environment(base, root)
    environment["RA8_SELFTEST_VENV"] = str(marker)
    before = source_state(root)
    result = run_owner(root, temp_root, environment)
    if result.returncode or source_state(root) != before or tuple(temp_root.iterdir()):
        fail(
            f"exact bootstrap policy population failed: {result.returncode}: "
            f"stdout={result.stdout!r} stderr={result.stderr!r}"
        )
    if marker.exists():
        fail("bootstrap validation selected a mutable source-tree owner tool")
    ignore = root / ".gitignore"
    ignore.write_text(ignore.read_text(encoding="utf-8") + "candidate-only/\n", encoding="utf-8")
    git(root, "add", ".gitignore")
    before = source_state(root)
    result = run_owner(root, temp_root, environment)
    if result.returncode == 0 or "pre-validator bootstrap" not in result.stderr:
        fail("changed bootstrap policy population passed an older HEAD validator")
    if source_state(root) != before or tuple(temp_root.iterdir()):
        fail("bootstrap rejection mutated source state or left snapshot residue")
