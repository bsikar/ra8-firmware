# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Own and publish the WSL fleet's staged control bytes without path races."""

from __future__ import annotations

import hashlib
import json
import os
import pwd
import shlex
import shutil
import stat
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
SHA256_HEX_LENGTH = 64
ROOT_READ_MODE = 0o444
WSL_RUNNER_IMAGE_CACHE = "/opt/ra8-infra-cache/ra8-ci-runner.tar"
STAGE_OWNER = "ra8-firmware fleet WSL stage v1"
CACHE_OWNER = "ra8-firmware fleet WSL runner cache v1"
OWNER_FILE = ".ra8-fleet-owner"
GENERATION_FILE = ".ra8-stage-generation"
REMOTE_APPLY_REQUIRED_STATUS = 42
STAGE_DIRECTORY_MEMBERS = (
    ".ansible/collections",
    ".tools/uv",
    "infra/ansible",
)
STAGE_MEMBERS = (
    *STAGE_DIRECTORY_MEMBERS,
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


def _installed_snapshot() -> bool:
    """Return whether this source is the root-owned immutable service snapshot."""
    marker = fm.REPO_ROOT / ".ra8-source-sha256"
    try:
        root_metadata = fm.REPO_ROOT.lstat()
        metadata = marker.lstat()
        digest = marker.read_text(encoding="ascii").strip()
    except OSError:
        return False
    return (
        not fm.REPO_ROOT.is_symlink()
        and root_metadata.st_uid == 0
        and stat.S_ISREG(metadata.st_mode)
        and not marker.is_symlink()
        and metadata.st_uid == 0
        and stat.S_IMODE(metadata.st_mode) == ROOT_READ_MODE
        and len(digest) == SHA256_HEX_LENGTH
        and all(character in "0123456789abcdef" for character in digest)
    )


def _bootstrap_action(mode: str, installed: bool) -> str:
    """Select mutation only for an operator-owned apply checkout."""
    return "--ensure" if mode == "apply" and not installed else "--verify-cache"


def verify_stage_sources(mode: str) -> int:
    """Authenticate local staged authorities before any remote side effect."""
    try:
        frm.ansible_environment(os.environ, fm.ANSIBLE_DIR)
    except frm.MaintenanceError as exc:
        return _fail(str(exc))
    bootstrap = fm.REPO_ROOT / "scripts/dev/bootstrap_uv.py"
    action = _bootstrap_action(mode, _installed_snapshot())
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


def _generation_records(root: Path) -> list[tuple[str, str, int, str]]:
    """Describe every staged path by name, type, mode, and authenticated payload."""
    records: list[tuple[str, str, int, str]] = []
    for member in STAGE_MEMBERS:
        start = root / member
        paths = [start]
        if start.is_dir():
            paths.extend(sorted(start.rglob("*"), key=lambda path: path.as_posix()))
        for path in paths:
            metadata = path.lstat()
            relative = path.relative_to(root).as_posix()
            mode = stat.S_IMODE(metadata.st_mode)
            if path.is_symlink():
                records.append((relative, "l", mode, str(path.readlink())))
            elif path.is_dir():
                records.append((relative, "d", mode, ""))
            elif path.is_file():
                digest = hashlib.sha256(path.read_bytes()).hexdigest()
                records.append((relative, "f", mode, digest))
            else:
                msg = f"unsupported WSL stage authority type: {relative}"
                raise ValueError(msg)
    return records


def stage_generation(root: Path = fm.REPO_ROOT) -> str:
    """Return the canonical complete-generation digest for staged authorities."""
    encoded = json.dumps(_generation_records(root), separators=(",", ":")).encode("ascii")
    return hashlib.sha256(encoded).hexdigest()


def _generation_probe_command(root: str) -> str:
    """Render a read-only complete-generation digest probe for the remote stage."""
    code = """import hashlib,json,os,stat,sys
from pathlib import Path
root=Path(sys.argv[1])
members=json.loads(sys.argv[2])
records=[]
for member in members:
    start=root/member
    paths=[start]
    if start.is_dir():
        paths.extend(sorted(start.rglob("*"),key=lambda path:path.as_posix()))
    for path in paths:
        metadata=path.lstat()
        relative=path.relative_to(root).as_posix()
        mode=stat.S_IMODE(metadata.st_mode)
        if path.is_symlink():
            records.append((relative,"l",mode,os.readlink(path)))
        elif path.is_dir():
            records.append((relative,"d",mode,""))
        elif path.is_file():
            records.append((relative,"f",mode,hashlib.sha256(path.read_bytes()).hexdigest()))
        else:
            raise SystemExit("unsupported staged path type: "+relative)
print(hashlib.sha256(json.dumps(records,separators=(",",":")).encode("ascii")).hexdigest())
"""
    members = json.dumps(STAGE_MEMBERS)
    return (
        f"/usr/bin/python3 -I -S -c {shlex.quote(code)} {shlex.quote(root)} {shlex.quote(members)}"
    )


def _stage_archive(mode: str) -> tuple[int, bytes]:
    """Build one authenticated local archive before touching the remote stage."""
    rc = verify_stage_sources(mode)
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


def _owned_shell(owner: str, owner_uid: int = 0) -> list[str]:
    """Render reusable exact-owner and no-mount directory operations."""
    return [
        f"expected_owner={shlex.quote(owner)}",
        f"expected_owner_uid={owner_uid}",
        "owned_dir() {",
        '  [ -d "$1" ] && [ ! -L "$1" ] && ! /usr/bin/mountpoint -q -- "$1" &&',
        '    [ "$(stat -c %u -- "$1")" = "$expected_owner_uid" ] &&',
        '    [ "$(stat -c %a -- "$1")" = 755 ] &&',
        f'    [ -f "$1/{OWNER_FILE}" ] && [ ! -L "$1/{OWNER_FILE}" ] &&',
        f'    [ "$(stat -c %u -- "$1/{OWNER_FILE}")" = "$expected_owner_uid" ] &&',
        f'    [ "$(stat -c %a -- "$1/{OWNER_FILE}")" = 644 ] &&',
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


def transaction_lock_lines(
    exclusive: bool, lock_root: str = "/run/lock", owner_uid: int = 0
) -> list[str]:
    """Render a no-write host-local reader or writer lock acquisition."""
    option = "-x" if exclusive else "-s"
    return [
        f"lock_root={shlex.quote(lock_root)}",
        '[ -d "$lock_root" ] && [ ! -L "$lock_root" ] &&',
        '  [ "$(readlink -f -- "$lock_root")" = "$lock_root" ] &&',
        f'  [ "$(stat -c %u -- "$lock_root")" = {owner_uid} ] || {{',
        '  echo "unsafe WSL lock authority" >&2; exit 1;',
        "}",
        'case "$(stat -c %a -- "$lock_root")" in 755|775) ;;',
        '  *) echo "unsafe WSL lock authority mode" >&2; exit 1 ;;',
        "esac",
        'exec 9<"$lock_root"',
        f"/usr/bin/flock {option} 9",
    ]


def stage_probe_lines(expected: str, stage: str = WSL_STAGE) -> list[str]:
    """Render authenticated read-only classification of one installed generation."""
    marker = f"{stage}/{GENERATION_FILE}"
    probe = _generation_probe_command(stage)
    return [
        f"stage={shlex.quote(stage)}",
        f"generation_marker={shlex.quote(marker)}",
        'if [ ! -e "$stage" ] && [ ! -L "$stage" ]; then',
        f'  echo "WSL stage is missing; apply required" >&2; exit {REMOTE_APPLY_REQUIRED_STATUS}',
        "fi",
        *_owned_shell(STAGE_OWNER, 0 if stage == WSL_STAGE else os.getuid()),
        'owned_dir "$stage" || { echo "unsafe WSL stage authority" >&2; exit 1; }',
        'if [ ! -e "$generation_marker" ] && [ ! -L "$generation_marker" ]; then',
        f'  echo "WSL generation manifest is missing; apply required" >&2; '
        f"exit {REMOTE_APPLY_REQUIRED_STATUS}",
        "fi",
        '[ -f "$generation_marker" ] && [ ! -L "$generation_marker" ] &&',
        '  [ "$(stat -c %u -- "$generation_marker")" = "$expected_owner_uid" ] &&',
        '  [ "$(stat -c %a -- "$generation_marker")" = 444 ] || {',
        '  echo "unsafe WSL generation manifest" >&2; exit 1;',
        "}",
        'installed_generation="$(cat -- "$generation_marker")"',
        f'if [ "$installed_generation" != {shlex.quote(expected)} ]; then',
        f'  echo "WSL stage is stale; apply required" >&2; exit {REMOTE_APPLY_REQUIRED_STATUS}',
        "fi",
        f'actual_generation="$({probe})" || {{',
        '  echo "could not authenticate WSL stage generation" >&2; exit 1;',
        "}",
        '[ "$actual_generation" = "$installed_generation" ] || {',
        '  echo "WSL stage generation authentication failed" >&2; exit 1;',
        "}",
    ]


def stage_prepare_script(stage: str = WSL_STAGE) -> str:
    """Render deterministic recovery and fresh incoming-stage creation."""
    incoming = f"{stage}.incoming"
    previous = f"{stage}.previous"
    owner_uid = 0 if stage == WSL_STAGE else os.getuid()
    lines = ["set -euo pipefail", *_owned_shell(STAGE_OWNER, owner_uid)]
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


def stage_seal_script(generation: str, stage: str = WSL_STAGE) -> str:
    """Authenticate an incoming generation and durably seal its manifest."""
    incoming = f"{stage}.incoming"
    marker = f"{incoming}/{GENERATION_FILE}"
    probe = _generation_probe_command(incoming)
    return "\n".join(
        [
            "set -euo pipefail",
            *_owned_shell(STAGE_OWNER, 0 if stage == WSL_STAGE else os.getuid()),
            f"incoming={shlex.quote(incoming)}",
            'owned_dir "$incoming" || { echo "incoming WSL stage is not owned" >&2; exit 1; }',
            f'actual_generation="$({probe})"',
            f'[ "$actual_generation" = {shlex.quote(generation)} ] || {{',
            '  echo "incoming WSL stage generation mismatch" >&2; exit 1;',
            "}",
            f"printf '%s\\n' {shlex.quote(generation)} >{shlex.quote(marker)}",
            f"chmod 0444 {shlex.quote(marker)}",
            f"sync_file {shlex.quote(marker)}",
            'sync_dir "$incoming"',
            "",
        ]
    )


def stage_publish_script(stage: str = WSL_STAGE, generation: str | None = None) -> str:
    """Render atomic stage publication with deterministic rollback."""
    incoming = f"{stage}.incoming"
    previous = f"{stage}.previous"
    generation = generation or ""
    owner_uid = 0 if stage == WSL_STAGE else os.getuid()
    lines = ["set -euo pipefail", *_owned_shell(STAGE_OWNER, owner_uid)]
    lines.extend(
        [
            f"stage={shlex.quote(stage)}",
            f"incoming={shlex.quote(incoming)}",
            f"previous={shlex.quote(previous)}",
            'owned_dir "$incoming" || { echo "incoming WSL stage is not owned" >&2; exit 1; }',
            f"generation_marker={shlex.quote(incoming + '/' + GENERATION_FILE)}",
            '[ -f "$generation_marker" ] && [ ! -L "$generation_marker" ] &&',
            '  [ "$(stat -c %u -- "$generation_marker")" = "$expected_owner_uid" ] &&',
            '  [ "$(stat -c %a -- "$generation_marker")" = 444 ] || {',
            '  echo "incoming WSL generation manifest is unsafe" >&2; exit 1;',
            "}",
            *(
                [
                    f'[ "$(cat -- "$generation_marker")" = {shlex.quote(generation)} ] || {{',
                    '  echo "incoming WSL generation manifest is stale" >&2; exit 1;',
                    "}",
                ]
                if generation
                else []
            ),
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
            *_owned_shell(STAGE_OWNER, 0 if stage == WSL_STAGE else os.getuid()),
            f"incoming={shlex.quote(incoming)}",
            'if [ -e "$incoming" ] || [ -L "$incoming" ]; then',
            '  remove_owned_dir "$incoming"',
            "fi",
            "",
        ]
    )


def prepare(data: dict[str, Any], name: str, mode: str, run: CommandRunner) -> tuple[int, str]:
    """Transfer and authenticate an incoming generation without publishing it."""
    tar_rc, archive = _stage_archive(mode)
    if tar_rc:
        return tar_rc, ""
    generation = stage_generation()
    host = data["hosts"][name]
    ssh = fr.ssh_target(data, name)
    shell = fm.remote_shell(host)
    rc = run([*ssh, shell], stdin=stage_prepare_script())
    if rc:
        return rc, ""
    distro = str(host["connect"]["distro"])
    incoming = f"{WSL_STAGE}.incoming"
    unpack = (
        f"wsl -d {shlex.quote(distro)} -u root -e /usr/bin/env -i "
        f"HOME=/root PATH=/usr/bin:/bin /usr/bin/tar -xzf - -C {shlex.quote(incoming)}"
    )
    rc = run([*ssh, unpack], stdin=archive)
    if not rc:
        rc = run([*ssh, shell], stdin=stage_seal_script(generation))
    if rc:
        run([*ssh, shell], stdin=stage_cleanup_script())
        return rc, ""
    return 0, generation


def push(data: dict[str, Any], name: str, mode: str, run: CommandRunner) -> int:
    """Atomically publish authenticated control inputs to the WSL distro."""
    rc, generation = prepare(data, name, mode, run)
    if rc:
        return rc
    host = data["hosts"][name]
    ssh = fr.ssh_target(data, name)
    shell = fm.remote_shell(host)
    script = "\n".join(
        [
            "set -euo pipefail",
            *transaction_lock_lines(exclusive=True),
            stage_publish_script(generation=generation),
        ]
    )
    rc = run([*ssh, shell], stdin=script)
    if rc:
        run([*ssh, shell], stdin=stage_cleanup_script())
    return rc


def cache_prepare_script(cache: str = WSL_RUNNER_IMAGE_CACHE) -> str:
    """Render exact cache ownership and no-follow staging preparation."""
    root = str(Path(cache).parent)
    part = f"{cache}.part"
    owner_uid = 0 if cache == WSL_RUNNER_IMAGE_CACHE else os.getuid()
    lines = ["set -euo pipefail", *_owned_shell(CACHE_OWNER, owner_uid)]
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
            *_owned_shell(CACHE_OWNER, 0 if cache == WSL_RUNNER_IMAGE_CACHE else os.getuid()),
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
    owner_uid = 0 if cache == WSL_RUNNER_IMAGE_CACHE else os.getuid()
    lines = ["set -euo pipefail", *_owned_shell(CACHE_OWNER, owner_uid)]
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
    generation = "a" * SHA256_HEX_LENGTH
    marker = incoming / GENERATION_FILE
    marker.write_text(f"{generation}\n", encoding="ascii")
    marker.chmod(ROOT_READ_MODE)
    if (
        _run_shell(stage_publish_script(str(stage), generation)).returncode
        or not (stage / "new").is_file()
    ):
        failures.append("owned WSL stage did not publish atomically")
    previous = Path(f"{stage}.previous")
    stage.rename(previous)
    if _run_shell(stage_prepare_script(str(stage))).returncode or not (stage / "new").is_file():
        failures.append("interrupted WSL publication did not recover the last-good generation")
    _run_shell(stage_cleanup_script(str(stage)))
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


def _probe_selftest(root: Path) -> list[str]:
    """Prove missing/stale drift and unsafe metadata remain distinct."""
    failures: list[str] = []
    stage = root / "probe-stage"
    missing = _run_shell("\n".join(["set -euo pipefail", *stage_probe_lines("0" * 64, str(stage))]))
    if missing.returncode != REMOTE_APPLY_REQUIRED_STATUS:
        failures.append("missing WSL stage was not classified apply-required")
    _write_owner(stage, STAGE_OWNER)
    for member in STAGE_MEMBERS:
        target = stage / member
        if member in STAGE_DIRECTORY_MEMBERS:
            target.mkdir(parents=True, exist_ok=True)
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(f"fixture:{member}\n", encoding="ascii")
            target.chmod(0o644)
    generation = stage_generation(stage)
    marker = stage / GENERATION_FILE
    marker.write_text(f"{generation}\n", encoding="ascii")
    marker.chmod(ROOT_READ_MODE)
    probe = "\n".join(["set -euo pipefail", *stage_probe_lines(generation, str(stage))])
    if _run_shell(probe).returncode:
        failures.append("matching authenticated WSL generation was refused")
    marker.chmod(0o644)
    marker.write_text(f"{'1' * SHA256_HEX_LENGTH}\n", encoding="ascii")
    marker.chmod(ROOT_READ_MODE)
    if _run_shell(probe).returncode != REMOTE_APPLY_REQUIRED_STATUS:
        failures.append("stale WSL generation was not classified apply-required")
    marker.chmod(0o644)
    marker.write_text(f"{generation}\n", encoding="ascii")
    marker.chmod(0o666)
    if _run_shell(probe).returncode != 1:
        failures.append("unsafe WSL generation metadata was classified as drift")
    return failures


def _transaction_lock_selftest(root: Path) -> list[str]:
    """Prove readers coexist, exclude writers, and leave no check residue."""
    failures: list[str] = []
    lock_root = root / "lock"
    lock_root.mkdir(mode=0o755)
    reader_lines = transaction_lock_lines(
        exclusive=False, lock_root=str(lock_root), owner_uid=os.getuid()
    )
    reader_script = "\n".join(["set -euo pipefail", *reader_lines, "echo READY", "read -r _"])
    holder = subprocess.Popen(  # noqa: S603 -- fixed Bash runs generated offline selftest
        ["/bin/bash", "-c", reader_script],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if holder.stdout is None or holder.stdin is None:
        holder.kill()
        holder.wait(timeout=5)
        return ["WSL reader lock selftest did not create its observation pipes"]
    if holder.stdout.readline().strip() != "READY":
        failures.append("WSL reader lock did not acquire")
    writer_lines = transaction_lock_lines(
        exclusive=True, lock_root=str(lock_root), owner_uid=os.getuid()
    )
    writer_lines[-1] = "/usr/bin/flock -xn 9"
    if _run_shell("\n".join(["set -euo pipefail", *writer_lines])).returncode == 0:
        failures.append("WSL writer entered while a reader held the generation")
    holder.terminate()
    holder.wait(timeout=5)
    if _run_shell("\n".join(["set -euo pipefail", *writer_lines])).returncode:
        failures.append("WSL writer did not enter after readers exited")
    if any(lock_root.iterdir()):
        failures.append("WSL check lock left durable residue")
    if holder.returncode == 0:
        failures.append("killed WSL check did not terminate its lock holder")
    return failures


def run_selftest() -> list[str]:
    """Exercise offline ownership and atomic-publication boundaries."""
    with tempfile.TemporaryDirectory(prefix="ra8-wsl-stage-") as raw:
        root = Path(raw)
        failures = (
            _stage_selftest(root)
            + _cache_selftest(root)
            + _link_selftest(root)
            + _probe_selftest(root)
            + _transaction_lock_selftest(root)
        )
        if _bootstrap_action("apply", installed=False) != "--ensure":
            failures.append("mutable operator apply lost its authenticated ensure path")
        if _bootstrap_action("apply", installed=True) != "--verify-cache":
            failures.append("installed WSL apply attempted to mutate root-owned uv inputs")
        if _bootstrap_action("check", installed=False) != "--verify-cache":
            failures.append("WSL check attempted to mutate uv inputs")
        return failures
