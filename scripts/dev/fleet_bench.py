# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Bind a mutating bench converge to the repository's bench lock."""

from __future__ import annotations

import argparse
import contextlib
import hashlib
import io
import json
import os
import pwd
import re
import subprocess
from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass, replace
from pathlib import Path

import bench_lock_capability as blc

LOCK_ID_RE = re.compile(r"[0-9a-f]{16}")
MAINTENANCE_VAR = "hil_bench_maintenance_lock_id"
HOLDER_PID_VAR = "hil_bench_maintenance_holder_pid"
HOLDER_START_VAR = "hil_bench_maintenance_holder_start_ticks"
HOLDER_TARGET_VAR = "hil_bench_maintenance_holder_target"
INHERITED_LOCK_ERROR = "inherited bench lock is not the live holder"
ARGPARSE_USAGE_ERROR = 2


class BenchGuardError(ValueError):
    """A mutating bench converge lacks a canonical live-hold identity."""


@dataclass(frozen=True)
class GuardRequest:
    """Everything needed to wrap one converge without reaching a host."""

    repo_root: Path
    fleet_script: Path
    original_argv: Sequence[str]
    host_class: str
    plays: Sequence[str]
    mode: str
    environment: Mapping[str, str]


@dataclass(frozen=True)
class FlowRequest:
    """The user-controlled selectors at the bench apply boundary."""

    host_class: str
    plays: Sequence[str]
    mode: str
    tags: str
    extra_vars: Sequence[str]
    trusted_tags: bool = False


LockAuthenticator = Callable[[Path, blc.Capability], bool]


def _live_lock_matches(repo_root: Path, capability: blc.Capability) -> bool:
    """Authenticate local wrapper ancestry and the remote kernel-held lock."""
    client = repo_root / "scripts/hil/lib/bench_client.sh"
    verifier = repo_root / "scripts/hil/lib/bench_lock_verify.py"
    broker = repo_root / "scripts/hil/lib/bench_lock_broker.py"
    host = repo_root / "scripts/hil/lib/bench_host.sh"
    try:
        resolved_client = client.resolve(strict=True)
        resolved_verifier = verifier.resolve(strict=True)
        resolved_broker = broker.resolve(strict=True)
        resolved_host = host.resolve(strict=True)
        blc.authenticate(capability, repo_root)
    except OSError:
        return False
    except blc.CapabilityError:
        return False
    sources = (client, verifier, broker, host)
    resolved = (resolved_client, resolved_verifier, resolved_broker, resolved_host)
    if any(
        path.absolute() != target or path.is_symlink() or not path.is_file()
        for path, target in zip(sources, resolved, strict=False)
    ):
        return False
    try:
        digest = hashlib.sha256(host.read_bytes()).hexdigest()
        broker_digest = hashlib.sha256(broker.read_bytes()).hexdigest()
    except OSError:
        return False
    script = '. "$1"; bench_verify_live "$2" wrapped "$3" "$4"'
    try:
        home = Path(pwd.getpwuid(os.getuid()).pw_dir).resolve(strict=True)
        result = subprocess.run(  # noqa: S603 -- fixed bash and authenticated source path
            [
                "/bin/bash",
                "--noprofile",
                "--norc",
                "-p",
                "-c",
                script,
                "ra8-lock",
                str(client),
                capability.lock_id,
                digest,
                broker_digest,
            ],
            env={"HOME": str(home), "PATH": "/usr/bin:/bin"},
            check=False,
            timeout=30,
        )
    except (OSError, subprocess.TimeoutExpired):
        return False
    return result.returncode == 0


def needs_guard(host_class: str, plays: Sequence[str], mode: str) -> bool:
    """Return whether this converge can mutate the physical bench host."""
    bench = host_class == "hil_bench" and "hil-bench" in plays
    delegated = host_class == "dev_box" and "dev-box" in plays
    return mode == "apply" and (bench or delegated)


def _lock_id(environment: Mapping[str, str]) -> str:
    """Return the inherited live-hold identity, rejecting ambiguous values."""
    lock_id = environment.get("RA8_BENCH_LOCK_ID", "")
    if lock_id and LOCK_ID_RE.fullmatch(lock_id) is None:
        message = "RA8_BENCH_LOCK_ID is not the canonical 16-hex identity"
        raise BenchGuardError(message)
    return lock_id


def _capability(environment: Mapping[str, str]) -> blc.Capability:
    """Parse every inherited capability field as one indivisible identity."""
    try:
        return blc.from_environment(dict(environment))
    except blc.CapabilityError as exc:
        raise BenchGuardError(str(exc)) from exc


def guarded_argv(
    request: GuardRequest,
    authenticate: LockAuthenticator = _live_lock_matches,
) -> list[str]:
    """Return a bench-lock wrapper argv, or empty once already guarded."""
    if not needs_guard(request.host_class, request.plays, request.mode):
        return []
    lock_id = _lock_id(request.environment)
    if lock_id:
        capability = _capability(request.environment)
        if not authenticate(request.repo_root, capability):
            raise BenchGuardError(INHERITED_LOCK_ERROR)
        return []
    return [
        "/bin/bash",
        "-p",
        str(request.repo_root / "scripts/hil/bench.sh"),
        "run",
        "--intent",
        "Ansible bench-affecting converge",
        "--for",
        "2h",
        "--wait",
        "2h",
        "--",
        str(request.fleet_script),
        *request.original_argv,
    ]


def ansible_extra(
    host_class: str,
    plays: Sequence[str],
    mode: str,
    environment: Mapping[str, str],
) -> list[str]:
    """Bind the validated outer hold to the remote role transaction."""
    if not needs_guard(host_class, plays, mode):
        return []
    lock_id = _lock_id(environment)
    if not lock_id:
        message = "mutating bench converge is outside the bench lock"
        raise BenchGuardError(message)
    capability = _capability(environment)
    values = {
        HOLDER_PID_VAR: capability.holder_pid,
        HOLDER_START_VAR: capability.holder_start_ticks,
        HOLDER_TARGET_VAR: capability.target,
        MAINTENANCE_VAR: lock_id,
    }
    return ["-e", json.dumps(values, sort_keys=True)]


def control_flow_refusal(request: FlowRequest) -> str:
    """Reject user-controlled Ansible flow/variables before the outer lock."""
    if not needs_guard(request.host_class, request.plays, request.mode):
        return ""
    if request.tags and not request.trusted_tags:
        return "bench-affecting applies do not accept --tags"
    if request.extra_vars:
        return "bench-affecting applies do not accept raw --extra-var"
    return ""


def _control_selftest() -> list[str]:
    """Prove bench-affecting selectors are rejected before any wrapper."""
    failures = []
    for tags, extra_vars, trusted, label in (
        ("hil-runner", (), False, "user tag selector"),
        (
            "",
            ("dev_box_hil_runner_service=harmless.service",),
            False,
            "service override",
        ),
        ("", ("dev_box_hil_runner_bench_alias=other",), False, "bench override"),
        ("", ("hil_bench_lock_dir=/tmp/fake",), False, "lock-path override"),
    ):
        request = FlowRequest("dev_box", ["dev-box"], "apply", tags, extra_vars, trusted)
        if not control_flow_refusal(request):
            failures.append(f"{label} was accepted before the bench wrapper")
    trusted = FlowRequest("dev_box", ["dev-box"], "apply", "hil-runner", (), trusted_tags=True)
    if control_flow_refusal(trusted):
        failures.append("typed register-hil tag was refused")
    return failures


def parser_selftest(factory: Callable[[], argparse.ArgumentParser]) -> list[str]:
    """Prove skip/start selectors are not public fleet arguments."""
    failures: list[str] = []
    for option in ("--skip-tags", "--start-at-task"):
        with contextlib.redirect_stderr(io.StringIO()):
            try:
                factory().parse_args(["apply", "star", option, "anything"])
            except SystemExit as exc:
                if exc.code == ARGPARSE_USAGE_ERROR:
                    continue
        failures.append(f"bench-affecting selector {option} was accepted")
    return failures


def _privileged_bash_selftest() -> list[str]:
    """Execute the authentication shell prefix and require privileged mode."""
    probe = 'case "$-" in *p*) printf "%s\\n" "$-" ;; *) exit 41 ;; esac'
    try:
        result = subprocess.run(  # noqa: S603 -- exact Bash argv is the subject under test
            [
                "/bin/bash",
                "--noprofile",
                "--norc",
                "-p",
                "-c",
                probe,
            ],
            env={"HOME": "/", "PATH": "/usr/bin:/bin"},
            check=False,
            capture_output=True,
            text=True,
            timeout=5,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        return [f"privileged Bash execution probe failed: {exc}"]
    if result.returncode != 0 or "p" not in result.stdout.strip():
        return ["live-lock Bash argv did not enter privileged mode"]
    return []


def run_selftest() -> list[str]:
    """Exercise both sides of the lock decision without contacting a host."""
    root = Path("/repo")
    script = root / "scripts/dev/fleet.py"
    bench = GuardRequest(root, script, ["apply", "star"], "hil_bench", ["hil-bench"], "apply", {})
    wrapped = guarded_argv(bench)
    failures = _privileged_bash_selftest()
    expected_wrapper = ["/bin/bash", "-p", str(root / "scripts/hil/bench.sh"), "run"]
    if wrapped[:4] != expected_wrapper:
        failures.append("unguarded bench apply did not use the fixed privileged Bash wrapper")
    if wrapped[-3:] != [str(script), "apply", "star"]:
        failures.append("wrapper did not preserve the exact fleet argv")
    held = {
        "RA8_BENCH_HOLDER_PID": "41",
        "RA8_BENCH_HOLDER_START_TICKS": "1000",
        "RA8_BENCH_HOLDER_TARGET": "star.local",
        "RA8_BENCH_LOCK_ID": "0123456789abcdef",
    }
    if guarded_argv(replace(bench, environment=held), lambda _root, _cap: True):
        failures.append("already-held bench apply recursed into a second lock")
    try:
        guarded_argv(replace(bench, environment=held), lambda _root, _cap: False)
    except BenchGuardError:
        pass
    else:
        failures.append("well-formed forged or stale lock identity bypassed the wrapper")
    expected = [
        "-e",
        '{"hil_bench_maintenance_holder_pid": 41, '
        '"hil_bench_maintenance_holder_start_ticks": 1000, '
        '"hil_bench_maintenance_holder_target": "star.local", '
        '"hil_bench_maintenance_lock_id": "0123456789abcdef"}',
    ]
    if ansible_extra("hil_bench", ["hil-bench"], "apply", held) != expected:
        failures.append("live hold identity did not bind to Ansible")
    if guarded_argv(replace(bench, original_argv=["check", "star"], mode="check")):
        failures.append("read-only check incorrectly required a live bench hold")
    dev = GuardRequest(root, script, ["apply", "dev"], "dev_box", ["dev-box"], "apply", {})
    if not guarded_argv(dev):
        failures.append("delegated dev-box bench mutation was not wrapped")
    try:
        ansible_extra("hil_bench", ["hil-bench"], "apply", {"RA8_BENCH_LOCK_ID": "bad"})
    except BenchGuardError:
        pass
    else:
        failures.append("malformed inherited lock identity was accepted")
    failures.extend(_control_selftest())
    return failures
