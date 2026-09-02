# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Own and publish the WSL fleet's staged control bytes without path races."""

from __future__ import annotations

import os
import pwd
import shlex
import shutil
import subprocess
import sys
import tempfile
from collections.abc import Callable
from pathlib import Path
from typing import Any

import fleet_model as fm
import fleet_reach as fr
import fleet_runner_maintenance as frm

WSL_STAGE = "/opt/ra8-infra"
WSL_RUNNER_IMAGE_CACHE = "/opt/ra8-infra-cache/ra8-ci-runner.tar"
STAGE_OWNER = "ra8-firmware fleet WSL stage v1"
CACHE_OWNER = "ra8-firmware fleet WSL runner cache v1"
OWNER_FILE = ".ra8-fleet-owner"
STAGE_MEMBERS = (
    ".ansible/collections",
    ".tools/uv",
    "infra/ansible",
    "pyproject.toml",
    "scripts/checks/check_ansible_collections.py",
    "scripts/ci/fleet_capacity.sh",
    "scripts/dev/bootstrap_uv.py",
    "scripts/dev/bootstrap_uv_exec.py",
    "scripts/dev/fleet_runner_maintenance.py",
    "scripts/dev/fleet_path_authority.py",
    "scripts/dev/uv_release.json",
    "scripts/dev/verify_locked_environment.py",
    "uv.lock",
)

CommandRunner = Callable[..., int]


def _fail(message: str) -> int:
    """Print one transport failure and return the fleet precondition status."""
    print(f"fleet: error: {message}", file=sys.stderr)
    return 2


def _bootstrap_environment() -> dict[str, str]:
    """Return a local uv bootstrap environment without inherited controls."""
    clean = {
        "HOME": pwd.getpwuid(os.getuid()).pw_dir,
        "LANG": "C.UTF-8",
        "LC_ALL": "C.UTF-8",
        "PATH": "/usr/bin:/bin",
    }
    clean["PYTHONNOUSERSITE"] = "1"
    return clean


def _links_stay_within(root: Path) -> bool:
    """Accept only existing relative links whose targets stay below ``root``."""
    authority = root.resolve(strict=True)
    for entry in root.rglob("*"):
        if not entry.is_symlink():
            continue
        try:
            target = entry.resolve(strict=True)
        except (OSError, RuntimeError):
            return False
        if entry.readlink().is_absolute() or not target.is_relative_to(authority):
            return False
    return True


def _verify_stage_sources(mode: str) -> int:
    """Authenticate local staged authorities before any remote side effect."""
    try:
        frm.ansible_environment(os.environ, fm.ANSIBLE_DIR)
    except frm.MaintenanceError as exc:
        return _fail(str(exc))
    bootstrap = fm.REPO_ROOT / "scripts/dev/bootstrap_uv.py"
    action = "--ensure" if mode == "apply" else "--verify-cache"
    result = subprocess.run(  # noqa: S603 -- fixed repository tool and managed Python
        [sys.executable, str(bootstrap), action],
        cwd=fm.REPO_ROOT,
        env=_bootstrap_environment(),
        text=True,
        capture_output=True,
        check=False,
        timeout=120,
    )
    if result.returncode:
        sys.stderr.write(result.stderr)
        return _fail("pinned uv cache is unavailable for the WSL stage")
    for relative in STAGE_MEMBERS:
        path = fm.REPO_ROOT / relative
        if not path.exists() or path.is_symlink():
            return _fail(f"WSL stage authority is absent or linked: {relative}")
        if path.is_dir() and not _links_stay_within(path):
            return _fail(f"WSL stage authority contains an escaping link: {relative}")
    return 0


def _stage_archive(mode: str) -> tuple[int, bytes]:
    """Build one authenticated local archive before touching the remote stage."""
    rc = _verify_stage_sources(mode)
    if rc:
        return rc, b""
    tar_tool = Path("/usr/bin/tar")
    if not tar_tool.is_file() or tar_tool.is_symlink() or not os.access(tar_tool, os.X_OK):
        return _fail("trusted /usr/bin/tar is unavailable"), b""
    tar = subprocess.run(  # noqa: S603 -- fixed argv and checkout paths
        [
            str(tar_tool),
            "--no-xattrs",
            "-czf",
            "-",
            "-C",
            str(fm.REPO_ROOT),
            *STAGE_MEMBERS,
        ],
        capture_output=True,
        check=False,
    )
    if tar.returncode:
        sys.stderr.write(tar.stderr.decode("utf-8", "replace"))
    return tar.returncode, tar.stdout


def _owned_shell(owner: str) -> list[str]:
    """Render reusable exact-owner and no-mount directory operations."""
    return [
        f"expected_owner={shlex.quote(owner)}",
        "owned_dir() {",
        '  [ -d "$1" ] && [ ! -L "$1" ] && ! /usr/bin/mountpoint -q -- "$1" &&',
        f'    [ -f "$1/{OWNER_FILE}" ] && [ ! -L "$1/{OWNER_FILE}" ] &&',
        f'    [ "$(cat -- "$1/{OWNER_FILE}")" = "$expected_owner" ]',
        "}",
        "sync_file() {",
        "  /usr/bin/python3 -I -S -c 'import os,sys; "
        "f=os.open(sys.argv[1],os.O_RDONLY|os.O_NOFOLLOW); "
        'os.fsync(f); os.close(f)\' "$1"',
        "}",
        "sync_dir() {",
        "  /usr/bin/python3 -I -S -c 'import os,sys; "
        "f=os.open(sys.argv[1],os.O_RDONLY|os.O_DIRECTORY); "
        'os.fsync(f); os.close(f)\' "$1"',
        "}",
        "remove_owned_dir() {",
        '  owned_dir "$1" || { echo "refusing unowned WSL path: $1" >&2; exit 1; }',
        '  parent="$(dirname -- "$1")"',
        '  rm -rf --one-file-system -- "$1"',
        '  sync_dir "$parent"',
        "}",
    ]


def stage_prepare_script(stage: str = WSL_STAGE) -> str:
    """Render deterministic recovery and fresh incoming-stage creation."""
    incoming = f"{stage}.incoming"
    previous = f"{stage}.previous"
    lines = ["set -euo pipefail", *_owned_shell(STAGE_OWNER)]
    lines.extend(
        [
            f"stage={shlex.quote(stage)}",
            f"incoming={shlex.quote(incoming)}",
            f"previous={shlex.quote(previous)}",
            'if [ -e "$previous" ] || [ -L "$previous" ]; then',
            '  owned_dir "$previous" || { echo "unowned previous WSL stage" >&2; exit 1; }',
            '  if [ -e "$stage" ] || [ -L "$stage" ]; then',
            '    owned_dir "$stage" || { echo "unowned current WSL stage" >&2; exit 1; }',
            '    remove_owned_dir "$previous"',
            "  else",
            '    mv -- "$previous" "$stage"',
            '    sync_dir "$(dirname -- "$stage")"',
            "  fi",
            "fi",
            'if [ -e "$stage" ] || [ -L "$stage" ]; then',
            '  owned_dir "$stage" || { echo "refusing unowned WSL stage" >&2; exit 1; }',
            "fi",
            'if [ -e "$incoming" ] || [ -L "$incoming" ]; then',
            '  remove_owned_dir "$incoming"',
            "fi",
            'install -d -m 0755 -- "$incoming"',
            f'printf \'%s\\n\' "$expected_owner" >"$incoming/{OWNER_FILE}"',
            f'chmod 0644 "$incoming/{OWNER_FILE}"',
            f'sync_file "$incoming/{OWNER_FILE}"',
            'sync_dir "$incoming"',
            'sync_dir "$(dirname -- "$incoming")"',
        ]
    )
    return "\n".join(lines) + "\n"


def stage_publish_script(stage: str = WSL_STAGE) -> str:
    """Render atomic stage publication with deterministic rollback."""
    incoming = f"{stage}.incoming"
    previous = f"{stage}.previous"
    lines = ["set -euo pipefail", *_owned_shell(STAGE_OWNER)]
    lines.extend(
        [
            f"stage={shlex.quote(stage)}",
            f"incoming={shlex.quote(incoming)}",
            f"previous={shlex.quote(previous)}",
            'owned_dir "$incoming" || { echo "incoming WSL stage is not owned" >&2; exit 1; }',
            '[ ! -e "$previous" ] && [ ! -L "$previous" ] || {',
            '  echo "previous WSL stage was not recovered" >&2; exit 1;',
            "}",
            'if [ -e "$stage" ] || [ -L "$stage" ]; then',
            '  owned_dir "$stage" || { echo "refusing unowned WSL stage" >&2; exit 1; }',
            '  mv -- "$stage" "$previous"',
            '  sync_dir "$(dirname -- "$stage")"',
            "fi",
            'if ! mv -- "$incoming" "$stage"; then',
            '  [ ! -e "$previous" ] || mv -- "$previous" "$stage"',
            '  sync_dir "$(dirname -- "$stage")"',
            "  exit 1",
            "fi",
            'sync_dir "$(dirname -- "$stage")"',
            'if [ -e "$previous" ]; then remove_owned_dir "$previous"; fi',
        ]
    )
    return "\n".join(lines) + "\n"


def stage_cleanup_script(stage: str = WSL_STAGE) -> str:
    """Render cleanup limited to the exact owned incoming directory."""
    incoming = f"{stage}.incoming"
    return "\n".join(
        [
            "set -euo pipefail",
            *_owned_shell(STAGE_OWNER),
            f"incoming={shlex.quote(incoming)}",
            'if [ -e "$incoming" ] || [ -L "$incoming" ]; then',
            '  remove_owned_dir "$incoming"',
            "fi",
            "",
        ]
    )


def push(data: dict[str, Any], name: str, mode: str, run: CommandRunner) -> int:
    """Atomically publish authenticated control inputs to the WSL distro."""
    tar_rc, archive = _stage_archive(mode)
    if tar_rc:
        return tar_rc
    host = data["hosts"][name]
    ssh = fr.ssh_target(data, name)
    shell = fm.remote_shell(host)
    rc = run([*ssh, shell], stdin=stage_prepare_script())
    if rc:
        return rc
    distro = str(host["connect"]["distro"])
    incoming = f"{WSL_STAGE}.incoming"
    unpack = (
        f"wsl -d {shlex.quote(distro)} -u root -e /usr/bin/env -i "
        f"HOME=/root PATH=/usr/bin:/bin /usr/bin/tar -xzf - -C {shlex.quote(incoming)}"
    )
    rc = run([*ssh, unpack], stdin=archive)
    if rc:
        run([*ssh, shell], stdin=stage_cleanup_script())
        return rc
    rc = run([*ssh, shell], stdin=stage_publish_script())
    if rc:
        run([*ssh, shell], stdin=stage_cleanup_script())
    return rc


def cache_prepare_script(cache: str = WSL_RUNNER_IMAGE_CACHE) -> str:
    """Render exact cache ownership and no-follow staging preparation."""
    root = str(Path(cache).parent)
    part = f"{cache}.part"
    lines = ["set -euo pipefail", *_owned_shell(CACHE_OWNER)]
    lines.extend(
        [
            f"cache_root={shlex.quote(root)}",
            f"dest={shlex.quote(cache)}",
            f"part={shlex.quote(part)}",
            'if [ -e "$cache_root" ] || [ -L "$cache_root" ]; then',
            '  owned_dir "$cache_root" || { echo "refusing unowned runner cache" >&2; exit 1; }',
            "else",
            '  install -d -m 0755 -- "$cache_root"',
            f'  printf \'%s\\n\' "$expected_owner" >"$cache_root/{OWNER_FILE}"',
            f'  chmod 0644 "$cache_root/{OWNER_FILE}"',
            f'  sync_file "$cache_root/{OWNER_FILE}"',
            '  sync_dir "$cache_root"',
            '  sync_dir "$(dirname -- "$cache_root")"',
            "fi",
            'for path in "$dest" "$part"; do',
            '  if [ -e "$path" ] || [ -L "$path" ]; then',
            '    [ -f "$path" ] && [ ! -L "$path" ] && ! /usr/bin/mountpoint -q -- "$path" || {',
            '      echo "refusing linked or non-file runner cache path: $path" >&2; exit 1;',
            "    }",
            "  fi",
            "done",
            'if [ -e "$part" ]; then',
            '  rm -f -- "$part"',
            '  sync_dir "$cache_root"',
            "fi",
        ]
    )
    return "\n".join(lines) + "\n"


def cache_receive_command(distro: str, cache: str = WSL_RUNNER_IMAGE_CACHE) -> str:
    """Return a no-follow receiver that exclusively creates the part file."""
    code = (
        "import os,shutil,sys;"
        "fd=os.open(sys.argv[1],os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600);"
        "out=os.fdopen(fd,'wb');shutil.copyfileobj(sys.stdin.buffer,out);"
        "out.flush();os.fsync(out.fileno());out.close()"
    )
    part = f"{cache}.part"
    return (
        f"wsl -d {shlex.quote(distro)} -u root -e /usr/bin/env -i "
        "HOME=/root PATH=/usr/bin:/bin /usr/bin/python3 -I -S "
        f"-c {shlex.quote(code)} {shlex.quote(part)}"
    )


def cache_cleanup_script(cache: str = WSL_RUNNER_IMAGE_CACHE) -> str:
    """Remove only a regular part file below an exact owned cache root."""
    root = str(Path(cache).parent)
    part = f"{cache}.part"
    return "\n".join(
        [
            "set -euo pipefail",
            *_owned_shell(CACHE_OWNER),
            f"cache_root={shlex.quote(root)}",
            f"part={shlex.quote(part)}",
            'owned_dir "$cache_root" || { echo "runner cache ownership lost" >&2; exit 1; }',
            'if [ -e "$part" ] || [ -L "$part" ]; then',
            '  [ -f "$part" ] && [ ! -L "$part" ] || { exit 1; }',
            '  rm -f -- "$part"',
            '  sync_dir "$cache_root"',
            "fi",
            "",
        ]
    )


def cache_publish_script(source_sha: str, cache: str = WSL_RUNNER_IMAGE_CACHE) -> str:
    """Authenticate and atomically publish an owned runner-image part."""
    root = str(Path(cache).parent)
    part = f"{cache}.part"
    lines = ["set -euo pipefail", *_owned_shell(CACHE_OWNER)]
    lines.extend(
        [
            f"cache_root={shlex.quote(root)}",
            f"dest={shlex.quote(cache)}",
            f"part={shlex.quote(part)}",
            'owned_dir "$cache_root" || { echo "runner cache ownership lost" >&2; exit 1; }',
            '[ -f "$part" ] && [ ! -L "$part" ] || { echo "runner cache part lost" >&2; exit 1; }',
            'if [ -e "$dest" ] || [ -L "$dest" ]; then',
            '  [ -f "$dest" ] && [ ! -L "$dest" ] || {',
            '    echo "runner cache dest unsafe" >&2; exit 1;',
            "  }",
            "fi",
            'actual=$(sha256sum -- "$part")',
            "actual=${actual%% *}",
            f'if [ "$actual" != {shlex.quote(source_sha)} ]; then',
            '  echo "runner image checksum mismatch" >&2',
            "  exit 1",
            "fi",
            'chmod 0644 "$part"',
            'sync_file "$part"',
            'mv -f -- "$part" "$dest"',
            'sync_dir "$cache_root"',
        ]
    )
    return "\n".join(lines) + "\n"


def _run_shell(script: str) -> subprocess.CompletedProcess[str]:
    """Run one offline transaction selftest shell."""
    return subprocess.run(["/bin/bash"], input=script, text=True, capture_output=True, check=False)


def _write_owner(path: Path, owner: str) -> None:
    """Create one fixture-owned directory and exact marker."""
    path.mkdir(parents=True)
    (path / OWNER_FILE).write_text(f"{owner}\n", encoding="ascii")


def _stage_selftest(root: Path) -> list[str]:
    """Prove unowned preservation, transfer cleanup, and atomic replacement."""
    failures: list[str] = []
    stage = root / "stage"
    stage.mkdir()
    sentinel = stage / "preserve"
    sentinel.write_text("unowned\n", encoding="ascii")
    if _run_shell(stage_prepare_script(str(stage))).returncode == 0 or not sentinel.exists():
        failures.append("unowned WSL stage was replaced")
    shutil.rmtree(stage)
    _write_owner(stage, STAGE_OWNER)
    sentinel = stage / "last-good"
    sentinel.write_text("keep\n", encoding="ascii")
    if _run_shell(stage_prepare_script(str(stage))).returncode:
        failures.append("owned WSL stage preparation failed")
        return failures
    incoming = Path(f"{stage}.incoming")
    (incoming / "partial").write_text("partial\n", encoding="ascii")
    if _run_shell(stage_cleanup_script(str(stage))).returncode or not sentinel.exists():
        failures.append("failed transfer did not preserve the last-good WSL stage")
    if _run_shell(stage_prepare_script(str(stage))).returncode:
        failures.append("second owned WSL stage preparation failed")
        return failures
    incoming = Path(f"{stage}.incoming")
    (incoming / "new").write_text("new\n", encoding="ascii")
    if _run_shell(stage_publish_script(str(stage))).returncode or not (stage / "new").is_file():
        failures.append("owned WSL stage did not publish atomically")
    return failures


def _cache_selftest(root: Path) -> list[str]:
    """Prove unowned cache and planted-part links are preserved/refused."""
    failures: list[str] = []
    cache_root = root / "cache"
    cache = cache_root / "runner.tar"
    cache_root.mkdir()
    sentinel = cache_root / "preserve"
    sentinel.write_text("unowned\n", encoding="ascii")
    if _run_shell(cache_prepare_script(str(cache))).returncode == 0 or not sentinel.exists():
        failures.append("unowned runner cache was claimed or removed")
    shutil.rmtree(cache_root)
    _write_owner(cache_root, CACHE_OWNER)
    outside = root / "outside"
    outside.write_text("keep\n", encoding="ascii")
    Path(f"{cache}.part").symlink_to(outside)
    if _run_shell(cache_prepare_script(str(cache))).returncode == 0:
        failures.append("planted runner-cache part symlink was accepted")
    if outside.read_text(encoding="ascii") != "keep\n":
        failures.append("planted runner-cache part symlink target was changed")
    Path(f"{cache}.part").unlink()
    cache.symlink_to(outside)
    if _run_shell(cache_prepare_script(str(cache))).returncode == 0:
        failures.append("planted runner-cache destination symlink was accepted")
    if outside.read_text(encoding="ascii") != "keep\n":
        failures.append("planted runner-cache destination target was changed")
    return failures


def _link_selftest(root: Path) -> list[str]:
    """Prove installed internal links pass while external links fail closed."""
    failures: list[str] = []
    authority = root / "authority"
    authority.mkdir()
    (authority / "target").write_text("owned\n", encoding="ascii")
    (authority / "internal").symlink_to("target")
    if not _links_stay_within(authority):
        failures.append("internal staged-authority symlink was refused")
    outside = root / "outside-link-target"
    outside.write_text("external\n", encoding="ascii")
    (authority / "escaping").symlink_to(outside)
    if _links_stay_within(authority):
        failures.append("escaping staged-authority symlink was accepted")
    return failures


def run_selftest() -> list[str]:
    """Exercise offline ownership and atomic-publication boundaries."""
    with tempfile.TemporaryDirectory(prefix="ra8-wsl-stage-") as raw:
        root = Path(raw)
        return _stage_selftest(root) + _cache_selftest(root) + _link_selftest(root)
