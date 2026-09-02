# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Run lock-policy uv operations through the authenticated bootstrap boundary."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import os
import platform
import shlex
import shutil
import subprocess
import sys
import tempfile
from collections.abc import Mapping
from dataclasses import dataclass, field
from pathlib import Path
from types import ModuleType


@dataclass(frozen=True)
class AuthenticatedUv:
    """Describe one bootstrap-owned uv execution authority."""

    bootstrap: Path
    manifest: Path
    cache_root: Path
    extra_environment: Mapping[str, str] = field(default_factory=dict)

    def run(
        self,
        arguments: list[str],
        *,
        cwd: Path | None = None,
        env: Mapping[str, str] | None = None,
        timeout: int = 30,
    ) -> subprocess.CompletedProcess[str]:
        """Run exact uv arguments without receiving or executing a cache path."""
        environment = {**(os.environ if env is None else env), **self.extra_environment}
        return subprocess.run(  # noqa: S603 -- fixed interpreter/bootstrap and exact argv.
            [
                "/usr/bin/python3",
                "-I",
                "-S",
                str(self.bootstrap),
                "--manifest",
                str(self.manifest),
                "--cache-root",
                str(self.cache_root),
                "--run",
                *arguments,
            ],
            cwd=cwd,
            check=False,
            capture_output=True,
            text=True,
            timeout=timeout,
            env=environment,
        )


def find_uv(
    root: Path,
    version: str,
    manifest: Path,
    cache_roots: tuple[Path, ...],
) -> AuthenticatedUv:
    """Resolve an authenticated bootstrap runner without returning a uv path."""
    bootstrap = root / "scripts/dev/bootstrap_uv.py"
    for cache_root in cache_roots:
        candidate = AuthenticatedUv(bootstrap, manifest, cache_root)
        probe = candidate.run(["--version"], timeout=10)
        if probe.returncode == 0 and probe.stdout.split()[:2] == ["uv", version]:
            return candidate
    message = "authenticated pinned uv is unavailable; run just setup"
    raise ValueError(message)


def export_findings(
    root: Path,
    exports: Mapping[str, Path],
    uv: AuthenticatedUv,
) -> list[str]:
    """Offline-regenerate every managed-target export and byte-compare it."""
    findings: list[str] = []
    environment = {**os.environ, "UV_PYTHON_DOWNLOADS": "never"}
    lock_check = uv.run(
        ["--directory", str(root), "--no-config", "lock", "--check", "--offline"],
        env=environment,
    )
    if lock_check.returncode != 0:
        return [f"uv.lock is stale: {lock_check.stderr.strip()}"]
    with tempfile.TemporaryDirectory(prefix="ra8-uv-policy-") as raw:
        temp = Path(raw)
        shutil.copy2(root / "pyproject.toml", temp / "pyproject.toml")
        shutil.copy2(root / "uv.lock", temp / "uv.lock")
        for group, relative in exports.items():
            findings.extend(_one_export_findings(root, temp, group, relative, uv))
    return findings


def _one_export_findings(
    root: Path,
    temp: Path,
    group: str,
    relative: Path,
    uv: AuthenticatedUv,
) -> list[str]:
    """Generate and compare one locked requirements export."""
    output = temp / relative
    output.parent.mkdir(parents=True, exist_ok=True)
    arguments = [
        "--no-config",
        "export",
        "--offline",
        "--locked",
        "--only-group",
        group,
        "--no-emit-project",
        "--format",
        "requirements-txt",
        "--output-file",
        str(relative),
    ]
    environment = {**os.environ, "UV_PYTHON_DOWNLOADS": "never"}
    result = uv.run(arguments, cwd=temp, env=environment)
    if result.returncode != 0:
        return [f"{group} export failed: {result.stderr.strip()}"]
    if output.read_bytes() != (root / relative).read_bytes():
        return [f"{relative} is stale versus uv.lock group {group}"]
    return []


def _load_bootstrap(bootstrap_path: Path) -> ModuleType:
    """Load the reviewed bootstrap only to build offline adversarial fixtures."""
    spec = importlib.util.spec_from_file_location("ra8_uv_runner_fixture", bootstrap_path)
    if spec is None or spec.loader is None:
        message = "cannot load uv bootstrap fixture helper"
        raise RuntimeError(message)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def _attack_runner(
    root: Path,
    bootstrap_path: Path,
    mode: str,
) -> tuple[AuthenticatedUv, Path]:
    """Build one exact-cache runner whose trusted payload mutates its cache."""
    bootstrap = _load_bootstrap(bootstrap_path)
    key = bootstrap.asset_key(platform.system(), platform.machine())
    asset_name = bootstrap.expected_asset_name(key)
    victim = root / "unauthenticated-executed"
    binary = _attack_script(mode, victim)
    payload = bootstrap.synthetic_archive(asset_name, binary)
    manifest = {
        "schema": 1,
        "repository": "astral-sh/uv",
        "version": "0.0.0",
        "assets": {key: {"name": asset_name, "sha256": hashlib.sha256(payload).hexdigest()}},
    }
    manifest_path = root / "uv_release.json"
    manifest_path.write_text(json.dumps(manifest), encoding="ascii")
    destination = bootstrap.cache_destination(root / "cache", "0.0.0", asset_name)
    destination.parent.mkdir(parents=True)
    archive = destination.parent / asset_name
    archive.write_bytes(payload)
    destination.write_bytes(binary)
    archive.chmod(bootstrap.PUBLIC_ARCHIVE_MODE)
    destination.chmod(bootstrap.PUBLIC_EXECUTABLE_MODE)
    replacement = root / "replacement"
    replacement.write_bytes(_victim_script(victim))
    replacement.chmod(bootstrap.PUBLIC_EXECUTABLE_MODE)
    environment = {
        "RA8_UV_ATTACK_CACHE": str(destination),
        "RA8_UV_ATTACK_REPLACEMENT": str(replacement),
    }
    return AuthenticatedUv(bootstrap_path, manifest_path, root / "cache", environment), victim


def _attack_script(mode: str, victim: Path) -> bytes:
    """Return a pinned test uv that attacks its cache only after execution starts."""
    mutation = (
        'cat "$RA8_UV_ATTACK_REPLACEMENT" >"$RA8_UV_ATTACK_CACHE"'
        if mode == "inode"
        else (
            'mv "$RA8_UV_ATTACK_CACHE" "$RA8_UV_ATTACK_CACHE.displaced"\n'
            'mv "$RA8_UV_ATTACK_REPLACEMENT" "$RA8_UV_ATTACK_CACHE"'
        )
    )
    return (
        "#!/bin/sh\nset -eu\n"
        f"{mutation}\n"
        'if [ "${1:-}" = --version ]; then echo uv 0.0.0; fi\n'
        f"test ! -e {shlex.quote(str(victim))}\n"
    ).encode("ascii")


def _victim_script(victim: Path) -> bytes:
    """Return the unauthenticated replacement that must never execute."""
    return f"#!/bin/sh\ntouch {victim}\necho uv 0.0.0\n".encode("ascii")


def execution_attack_selftest(
    bootstrap: Path,
    exports: Mapping[str, Path],
) -> list[str]:
    """Prove actual lock/export calls reject path and same-inode cache races."""
    failures: list[str] = []
    for mode in ("path", "inode"):
        with tempfile.TemporaryDirectory(prefix=f"ra8-uv-runner-{mode}-") as raw:
            root = Path(raw)
            (root / "pyproject.toml").write_text("[project]\nname='fixture'\n", encoding="ascii")
            (root / "uv.lock").write_text("version = 1\n", encoding="ascii")
            for relative in exports.values():
                target = root / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(b"")
            runner, victim = _attack_runner(root, bootstrap, mode)
            findings = export_findings(root, exports, runner)
            if not findings:
                failures.append(f"post-auth {mode} cache attack passed lock/export execution")
            if victim.exists():
                failures.append(f"post-auth {mode} replacement executed through lock/export")
    return failures
