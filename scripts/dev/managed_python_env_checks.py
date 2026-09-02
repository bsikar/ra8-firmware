#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Host managed-environment consumer contracts and hostile QA fixtures."""

from __future__ import annotations

import json
import os
import re
import shutil
import tempfile
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path
from typing import NoReturn, Protocol


class CommandResultLike(Protocol):
    """Describe captured process fields used by the consumer runtime audit."""

    returncode: int


class CommandRunner(Protocol):
    """Describe the authority's absolute-command runner."""

    def __call__(
        self, arguments: list[str], environment: dict[str, str], timeout_seconds: int
    ) -> CommandResultLike:
        """Run one command and return its status."""


@dataclass(frozen=True)
class FilesystemHarness:
    """Provide the production authority operations used by hostile fixtures."""

    refresh: Callable[[], object]
    reject_environment: Callable[[str, Path, str], None]
    make_environment: Callable[[str], Path]
    cache_key: Callable[[str], str]
    fail: Callable[[str], NoReturn]
    receipt_name: str
    receipt_mode: int


def rewrite_receipt(
    environment: Path, update: dict[str, object], receipt_name: str, receipt_mode: int
) -> None:
    """Replace a selftest receipt while preserving its required final mode."""
    receipt = environment / receipt_name
    payload = json.loads(receipt.read_text(encoding="ascii"))
    payload.update(update)
    receipt.chmod(0o600)
    receipt.write_text(
        json.dumps(payload, sort_keys=True, indent=2, ensure_ascii=True) + "\n",
        encoding="ascii",
    )
    receipt.chmod(receipt_mode)


def _contracts() -> tuple[tuple[str, str], ...]:
    """Return every exact authority invocation each consumer must own once."""
    return (
        (
            "setup_python.sh",
            '/usr/bin/python3 -I "$MANAGED_ENV_AUTHORITY" verify --env "$selected_venv" '
            '--pyproject "$PYPROJECT" --lock "$LOCKFILE" --group ci --print-bin',
        ),
        (
            "tool_env.sh",
            '/usr/bin/python3 -I "${repo_root}/scripts/dev/managed_python_env.py" cache-key '
            '--env "${selected}" --pyproject "${repo_root}/pyproject.toml" '
            '--lock "${repo_root}/uv.lock" --group ci',
        ),
        (
            "tool_env.sh",
            '/usr/bin/python3 -I "${repo_root}/scripts/dev/managed_python_env.py" verify '
            '--env "${selected}" --pyproject "${repo_root}/pyproject.toml" '
            '--lock "${repo_root}/uv.lock" --group ci --print-bin',
        ),
        (
            "provision_dev_box_toolchain.sh",
            'as_root /usr/bin/python3 -I "${ROOT}/scripts/dev/managed_python_env.py" write '
            '--env "${venv}" --pyproject "${ROOT}/pyproject.toml" '
            '--lock "${ROOT}/uv.lock" --group ci',
        ),
        (
            "provision_dev_box_toolchain.sh",
            '/usr/bin/python3 -I "${ROOT}/scripts/dev/managed_python_env.py" verify '
            '--env "${python_venv}" --pyproject "${ROOT}/pyproject.toml" '
            '--lock "${ROOT}/uv.lock" --group ci',
        ),
    )


def _dockerfile_instructions(source: str) -> tuple[tuple[str, str], ...]:
    """Return normalized active Dockerfile instructions without comments."""
    instructions: list[tuple[str, str]] = []
    chunks: list[str] = []
    for raw_line in source.splitlines():
        stripped = raw_line.strip()
        if stripped.startswith("#") or (not chunks and not stripped):
            continue
        continued = raw_line.rstrip().endswith("\\")
        chunk = raw_line.rstrip()
        if continued:
            chunk = chunk[:-1]
        chunks.append(chunk.strip())
        if continued:
            continue
        logical = " ".join(" ".join(chunks).split())
        chunks.clear()
        keyword, separator, body = logical.partition(" ")
        instructions.append((keyword.upper(), body if separator else ""))
    if chunks:
        instructions.append(("INCOMPLETE", " ".join(chunks)))
    return tuple(instructions)


def _dockerfile_receipt_contract() -> tuple[str, str, str]:
    """Return lock cleanup, receipt seal, and inherited-image verification."""
    cleanup = 'rm -f -- "${PYTHON_TOOL_VENV}/.lock"'
    authority = "/usr/bin/python3 -I /opt/ra8-uv-bootstrap/managed_python_env.py"
    devcontainer_inputs = (
        '--env "${PYTHON_TOOL_VENV}" '
        "--pyproject /opt/ra8-python-project/pyproject.toml "
        "--lock /opt/ra8-python-project/uv.lock --group ci"
    )
    runner_inputs = (
        '--env "${RA8_TOOL_VENV}" '
        "--pyproject /opt/ra8-python-project/pyproject.toml "
        "--lock /opt/ra8-python-project/uv.lock --group ci"
    )
    verify = f"{authority} verify {devcontainer_inputs}"
    marker = "printf '%s\\n' 'localhost/ra8-ci-runner (infra/images/runner/Dockerfile)'"
    runner_verify = f"{marker} > /etc/ra8-ci-runner && {authority} verify {runner_inputs}"
    return cleanup, f"{authority} write {devcontainer_inputs} && {verify}", runner_verify


def _receipt_root_user(instructions: tuple[tuple[str, str], ...], end: int) -> str:
    """Return the effective Docker build user immediately before one instruction."""
    user = "root"
    for keyword, body in instructions[:end]:
        if keyword == "FROM":
            user = "root"
        elif keyword == "USER":
            user = body.split(maxsplit=1)[0]
    return user


def dockerfile_receipt_findings(source: str) -> list[str]:
    """Require one root-owned receipt seal after lock cleanup and Python use."""
    cleanup, receipt, _ = _dockerfile_receipt_contract()
    instructions = _dockerfile_instructions(source)
    managed_environment = (
        'RA8_TOOL_VENV="${PYTHON_TOOL_VENV}" '
        'RA8_UV_CACHE_ROOT="/opt/ra8-uv-cache" '
        'VIRTUAL_ENV="${PYTHON_TOOL_VENV}" '
        'UV_PYTHON_DOWNLOADS="never" '
        'PATH="${PYTHON_TOOL_VENV}/bin:${PATH}"'
    )
    receipt_indices = [
        index
        for index, (keyword, body) in enumerate(instructions)
        if keyword == "RUN" and body == receipt
    ]
    cleanup_indices = [
        index
        for index, (keyword, body) in enumerate(instructions)
        if keyword == "RUN" and body.endswith(cleanup)
    ]
    findings: list[str] = []
    if len(receipt_indices) != 1:
        findings.append("Dockerfile must own exactly one active exact managed receipt RUN")
    if len(cleanup_indices) != 1:
        findings.append("Dockerfile must remove the transient uv lock exactly once")
    if instructions.count(("ENV", managed_environment)) != 1:
        findings.append("Dockerfile must bind the exact inherited managed-environment authority")
    if len(receipt_indices) != 1 or len(cleanup_indices) != 1:
        return findings
    receipt_index = receipt_indices[0]
    if cleanup_indices[0] >= receipt_index:
        findings.append("Dockerfile must remove the transient uv lock before sealing the receipt")
    if _receipt_root_user(instructions, receipt_index) not in {"0", "0:0", "root"}:
        findings.append("Dockerfile must seal the managed receipt as root")
    if instructions[receipt_index + 1 :] != (("USER", "${USERNAME}"),):
        findings.append(
            "Dockerfile receipt seal must be followed only by exact non-root USER restoration"
        )
    return findings


def runner_dockerfile_receipt_findings(source: str) -> list[str]:
    """Require the final ARC image to authenticate the inherited receipt."""
    _, _, verify = _dockerfile_receipt_contract()
    instructions = _dockerfile_instructions(source)
    verify_indices = [
        index
        for index, (keyword, body) in enumerate(instructions)
        if keyword == "RUN" and body == verify
    ]
    findings: list[str] = []
    if len(verify_indices) != 1:
        findings.append("runner Dockerfile must own one active exact receipt verification RUN")
        return findings
    verify_index = verify_indices[0]
    if _receipt_root_user(instructions, verify_index) not in {"0", "0:0", "root"}:
        findings.append("runner Dockerfile must verify the managed receipt as root")
    expected_tail = (
        ("ENV", "RUNNER_MANUALLY_TRAP_SIG=1 ACTIONS_RUNNER_PRINT_LOG_TO_STDOUT=1"),
        ("WORKDIR", "/home/runner"),
        ("USER", "runner"),
        ("ENTRYPOINT", "[]"),
        ("CMD", '["/bin/bash"]'),
    )
    if instructions[verify_index + 1 :] != expected_tail:
        findings.append("runner receipt verification must precede only its exact runtime metadata")
    if any(
        keyword == "ENV" and "RA8_TOOL_VENV" in body
        for keyword, body in instructions[:verify_index]
    ):
        findings.append("runner Dockerfile must not redirect the inherited RA8_TOOL_VENV authority")
    return findings


def consumer_findings(root: Path) -> list[str]:
    """Return source-contract drift between all managed-environment consumers."""
    paths = {
        "setup_python.sh": root / "scripts/dev/setup_python.sh",
        "tool_env.sh": root / "scripts/ci/lib/tool_env.sh",
        "Dockerfile": root / ".devcontainer/Dockerfile",
        "runner Dockerfile": root / "infra/images/runner/Dockerfile",
        "provision_dev_box_toolchain.sh": root / "scripts/dev/provision_dev_box_toolchain.sh",
    }
    sources = {label: path.read_text(encoding="ascii") for label, path in paths.items()}
    flattened = {
        label: " ".join(content.replace("\\\n", " ").split()) for label, content in sources.items()
    }
    findings = [
        f"{label} is missing the exact authenticated managed-environment invocation"
        for label, invocation in _contracts()
        if flattened[label].count(invocation) != 1
    ]
    findings.extend(dockerfile_receipt_findings(sources["Dockerfile"]))
    findings.extend(runner_dockerfile_receipt_findings(sources["runner Dockerfile"]))
    weak_patterns = (
        r"RA8_TOOL_VENV[^\n]{0,160}bin/python3[^\n]{0,40}-x",
        r"\[\[?\s+-x\s+[^\n]*RA8_TOOL_VENV",
    )
    for label in ("setup_python.sh", "tool_env.sh"):
        findings.extend(
            f"{label} still accepts an executable-only managed environment"
            for pattern in weak_patterns
            if re.search(pattern, sources[label])
        )
    return findings


def consumer_runtime_findings(root: Path, run_command: CommandRunner) -> list[str]:
    """Drive direct setup and real root-Just evaluation in both directions."""
    findings: list[str] = []
    with tempfile.TemporaryDirectory(prefix="ra8-managed-forged-") as tmp:
        forged = Path(tmp) / "managed"
        (forged / "bin").mkdir(parents=True)
        shutil.copy2("/bin/true", forged / "bin/python3")
        base_env = os.environ.copy()
        base_env.update({"BASH_ENV": "/dev/null", "ENV": "/dev/null", "PATH": "/usr/bin:/bin"})
        base_env.pop("PYTHONHOME", None)
        base_env.pop("PYTHONPATH", None)
        setup = root / "scripts/dev/setup_python.sh"
        normal_env = dict(base_env)
        normal_env["RA8_TOOL_VENV"] = ""
        normal = run_command(["/bin/bash", "-p", str(setup), "--print-path"], normal_env, 30)
        if normal.returncode != 0:
            findings.append("setup_python.sh rejects the native no-managed-environment path")
        hostile_env = dict(base_env)
        hostile_env["RA8_TOOL_VENV"] = str(forged)
        hostile = run_command(["/bin/bash", "-p", str(setup), "--print-path"], hostile_env, 30)
        if hostile.returncode == 0:
            findings.append("setup_python.sh accepted an arbitrary executable managed environment")
        just = shutil.which("just", path="/usr/local/bin:/usr/bin:/opt/homebrew/bin")
        if just is None:
            findings.append("cannot exercise the real root Justfile because just is missing")
        else:
            command = [just, "--justfile", str(root / "justfile"), "--evaluate", "PATH"]
            if run_command(command, normal_env, 30).returncode != 0:
                findings.append("real root Just evaluation rejects the native environment")
            if run_command(command, hostile_env, 30).returncode == 0:
                findings.append("real root Just evaluation accepted a forged managed environment")
    return findings


def _dockerfile_receipt_contract_selftest(dockerfile: Path, good: str) -> list[str]:
    """Return failures from independent hostile Docker receipt mutations."""
    cleanup, receipt, _ = _dockerfile_receipt_contract()
    managed_environment = next(
        line for line in good.splitlines() if line.startswith("ENV RA8_TOOL_VENV=")
    )
    mutations = (
        (f"RUN {receipt}\n", "RUN true\n", "a deleted receipt step"),
        (
            f"RUN true; {cleanup}\n{managed_environment}\nRUN {receipt}\n",
            f"RUN {receipt}\nRUN true; {cleanup}\n{managed_environment}\n",
            "a receipt moved before transient-lock cleanup",
        ),
        ("--group ci", "--group dev", "a weakened dependency-group binding"),
        (
            'RA8_TOOL_VENV="${PYTHON_TOOL_VENV}"',
            'RA8_TOOL_VENV=""',
            "a redirected inherited managed-environment authority",
        ),
        (f"RUN {receipt}", f"# RUN {receipt}", "receipt tokens present only in a comment"),
        (
            f"RUN {receipt}\nUSER ${{USERNAME}}",
            f"USER ${{USERNAME}}\nRUN {receipt}",
            "a non-root receipt writer",
        ),
        (
            f"RUN {receipt}\nUSER ${{USERNAME}}",
            f"RUN {receipt}\nRUN git config --global probe true\nUSER ${{USERNAME}}",
            "a later RUN after receipt sealing",
        ),
        (
            f"RUN {receipt}\nUSER ${{USERNAME}}",
            f"RUN {receipt}\nCOPY uv.lock /opt/ra8-python-project/uv.lock\nUSER ${{USERNAME}}",
            "a later COPY after receipt sealing",
        ),
        (
            f"RUN {receipt}\nUSER ${{USERNAME}}",
            f"RUN {receipt}\nONBUILD RUN true\nUSER ${{USERNAME}}",
            "a deferred ONBUILD mutation after receipt sealing",
        ),
        (
            f"RUN {receipt}\nUSER ${{USERNAME}}",
            f"RUN {receipt}\nUSER root",
            "a root final image user",
        ),
    )
    failures: list[str] = []
    for old, new, label in mutations:
        dockerfile.write_text(good.replace(old, new, 1), encoding="ascii")
        if not consumer_findings(dockerfile.parents[1]):
            failures.append(f"consumer selftest missed {label}")
    return failures


def _runner_receipt_contract_selftest(dockerfile: Path, good: str) -> list[str]:
    """Return failures from hostile final ARC-image receipt mutations."""
    _, _, verify = _dockerfile_receipt_contract()
    mutations = (
        (f"RUN {verify}\n", "RUN true\n", "a deleted runner receipt verification"),
        (f"RUN {verify}", f"# RUN {verify}", "runner verify tokens only in a comment"),
        ("--group ci", "--group dev", "a weakened runner dependency-group binding"),
        (
            '--env "${RA8_TOOL_VENV}"',
            '--env "${PYTHON_TOOL_VENV}"',
            "a parent-only ARG used instead of the inherited environment authority",
        ),
        (
            "USER root\nRUN true\nRUN " + verify,
            "USER root\nRUN true\nUSER runner\nRUN " + verify,
            "a non-root runner receipt verifier",
        ),
        (
            f"RUN {verify}\nENV RUNNER_MANUALLY_TRAP_SIG=1",
            f"RUN {verify}\nRUN printf later\nENV RUNNER_MANUALLY_TRAP_SIG=1",
            "a later runner-image RUN",
        ),
        (
            f"RUN {verify}\nENV RUNNER_MANUALLY_TRAP_SIG=1",
            f"RUN {verify}\nCOPY uv.lock /opt/ra8-python-project/uv.lock\n"
            "ENV RUNNER_MANUALLY_TRAP_SIG=1",
            "a later runner-image COPY",
        ),
        (
            f"RUN {verify}\nENV RUNNER_MANUALLY_TRAP_SIG=1",
            f"RUN {verify}\nONBUILD RUN true\nENV RUNNER_MANUALLY_TRAP_SIG=1",
            "a deferred runner-image mutation",
        ),
        (
            "USER root\nRUN true\nRUN " + verify,
            'USER root\nENV RA8_TOOL_VENV="/tmp/forged"\nRUN ' + verify,
            "a redirected inherited environment authority before verification",
        ),
        ("\nUSER runner\nENTRYPOINT", "\nUSER root\nENTRYPOINT", "a root ARC image user"),
    )
    failures: list[str] = []
    for old, new, label in mutations:
        dockerfile.write_text(good.replace(old, new, 1), encoding="ascii")
        if not consumer_findings(dockerfile.parents[3]):
            failures.append(f"consumer selftest missed {label}")
    return failures


def consumer_contract_selftest() -> list[str]:
    """Return failures from positive and hostile static-consumer fixtures."""
    with tempfile.TemporaryDirectory(prefix="ra8-managed-consumers-") as tmp:
        root = Path(tmp)
        paths = {
            "setup_python.sh": "scripts/dev/setup_python.sh",
            "tool_env.sh": "scripts/ci/lib/tool_env.sh",
            "Dockerfile": ".devcontainer/Dockerfile",
            "runner Dockerfile": "infra/images/runner/Dockerfile",
            "provision_dev_box_toolchain.sh": "scripts/dev/provision_dev_box_toolchain.sh",
        }
        grouped = {label: [] for label in paths}
        for label, invocation in _contracts():
            grouped[label].append(invocation)
        for label, relative in paths.items():
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("\n".join(grouped[label]) + "\n", encoding="ascii")
        cleanup, receipt, verify = _dockerfile_receipt_contract()
        managed_environment = (
            'ENV RA8_TOOL_VENV="${PYTHON_TOOL_VENV}" '
            'RA8_UV_CACHE_ROOT="/opt/ra8-uv-cache" '
            'VIRTUAL_ENV="${PYTHON_TOOL_VENV}" '
            'UV_PYTHON_DOWNLOADS="never" '
            'PATH="${PYTHON_TOOL_VENV}/bin:${PATH}"\n'
        )
        good_dockerfile = (
            f"FROM ubuntu:24.04\nRUN true; {cleanup}\n{managed_environment}"
            f"RUN {receipt}\nUSER ${{USERNAME}}\n"
        )
        dockerfile = root / ".devcontainer/Dockerfile"
        dockerfile.write_text(good_dockerfile, encoding="ascii")
        good_runner = (
            f"FROM base\nUSER root\nRUN true\nRUN {verify}\n"
            "ENV RUNNER_MANUALLY_TRAP_SIG=1 \\\n"
            "    ACTIONS_RUNNER_PRINT_LOG_TO_STDOUT=1\n"
            'WORKDIR /home/runner\nUSER runner\nENTRYPOINT []\nCMD ["/bin/bash"]\n'
        )
        runner_dockerfile = root / "infra/images/runner/Dockerfile"
        runner_dockerfile.write_text(good_runner, encoding="ascii")
        findings = consumer_findings(root)
        (root / "scripts/dev/setup_python.sh").write_text(
            '[[ -x "$RA8_TOOL_VENV/bin/python3" ]]\n', encoding="ascii"
        )
        if not consumer_findings(root):
            findings.append("consumer selftest missed an executable-only managed environment")
        (root / "scripts/dev/setup_python.sh").write_text(
            "\n".join(grouped["setup_python.sh"]) + "\n", encoding="ascii"
        )
        findings.extend(_dockerfile_receipt_contract_selftest(dockerfile, good_dockerfile))
        dockerfile.write_text(good_dockerfile, encoding="ascii")
        runner_dockerfile.write_text(good_runner, encoding="ascii")
        findings.extend(_runner_receipt_contract_selftest(runner_dockerfile, good_runner))
        return findings


def nested_tree_selftest(environment: Path, harness: FilesystemHarness) -> None:
    """Reject writable or changed package and command bytes below trusted roots."""
    site_packages = next(environment.glob("lib/python*/site-packages"))
    package_file = site_packages / "ra8_managed_env_selftest.py"
    command_file = environment / "bin/ra8-managed-env-selftest"
    package_file.write_text("VALUE = 1\n", encoding="ascii")
    command_file.write_text("#!/bin/sh\nexit 0\n", encoding="ascii")
    command_file.chmod(0o755)
    harness.refresh()
    package_file.chmod(0o666)
    harness.reject_environment("a group/other-writable nested package file", environment, "ci")
    package_file.chmod(0o644)
    site_packages.chmod(0o775)
    harness.reject_environment("a group-writable nested package directory", environment, "ci")
    site_packages.chmod(0o755)
    command_file.chmod(0o775)
    harness.reject_environment("a group-writable managed command", environment, "ci")
    command_file.chmod(0o755)
    package_file.write_text("VALUE = 2\n", encoding="ascii")
    harness.reject_environment(
        "changed nested package bytes with unchanged distribution metadata", environment, "ci"
    )


def stale_receipt_selftest(
    root: Path,
    environment: Path,
    pyproject: Path,
    lockfile: Path,
    harness: FilesystemHarness,
) -> None:
    """Exercise lock, group, copied-receipt, and symlink-root rejection."""
    stale_lock = lockfile.read_text(encoding="ascii")
    lockfile.write_text("version = 2\n", encoding="ascii")
    harness.reject_environment("a receipt stale against uv.lock", environment, "ci")
    lockfile.write_text(stale_lock, encoding="ascii")
    stale_project = pyproject.read_text(encoding="ascii")
    pyproject.write_text("[dependency-groups]\nci=['stale']\n", encoding="ascii")
    harness.reject_environment("a receipt stale against pyproject.toml", environment, "ci")
    pyproject.write_text(stale_project, encoding="ascii")
    harness.reject_environment("the wrong dependency group", environment, "dev")
    arbitrary = harness.make_environment("arbitrary")
    harness.reject_environment(
        "an arbitrary executable environment without a receipt", arbitrary, "ci"
    )
    copied = harness.make_environment("copied")
    shutil.copy2(environment / harness.receipt_name, copied / harness.receipt_name)
    harness.reject_environment("a copied receipt at another path", copied, "ci")
    alias = root / "managed-link"
    alias.symlink_to(environment, target_is_directory=True)
    harness.reject_environment("a symlinked environment root", alias, "ci")


def cache_key_selftest(
    environment: Path, pyproject: Path, lockfile: Path, harness: FilesystemHarness
) -> None:
    """Prove every source and receipt mutation invalidates the warm key."""
    key = harness.cache_key("ci")
    for label, path in (("pyproject.toml", pyproject), ("uv.lock", lockfile)):
        original = path.read_bytes()
        path.write_bytes(original + b"\n")
        if key == harness.cache_key("ci"):
            harness.fail(f"selftest cache key ignored changed {label} bytes")
        path.write_bytes(original)
    if key == harness.cache_key("dev"):
        harness.fail("selftest cache key ignored the dependency group")
    receipt = environment / harness.receipt_name
    receipt_bytes = receipt.read_bytes()
    receipt.chmod(0o600)
    receipt.write_bytes(receipt_bytes + b" ")
    receipt.chmod(harness.receipt_mode)
    if key == harness.cache_key("ci"):
        harness.fail("selftest cache key ignored changed receipt bytes")
    receipt.chmod(0o600)
    receipt.write_bytes(receipt_bytes)
    receipt.chmod(harness.receipt_mode)
