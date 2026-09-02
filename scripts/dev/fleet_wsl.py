# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Stage and run declared fleet plays inside a Windows host's WSL distro."""

from __future__ import annotations

import base64
import hashlib
import json
import os
import shlex
import subprocess
import sys
import tempfile
import textwrap
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Protocol

import fleet_model as fm
import fleet_reach as fr
import fleet_typed_vars as ftv
import fleet_wsl_stage as fws

# Under the distro's ext4 root, never /mnt/c: drvfs is much slower for the
# many-small-files work performed by Ansible and the runner image.
WSL_STAGE = fws.WSL_STAGE
WSL_RUNNER_IMAGE_CACHE = fws.WSL_RUNNER_IMAGE_CACHE
WSL_MANAGED_ROOT = "/opt/ra8-python-tools"
WSL_MANAGED_CACHE = "/opt/ra8-python-tools-cache"
WSL_ANSIBLE_PLAYBOOK = "/opt/ra8-python-tools/bin/ansible-playbook"
WSL_SYSTEM_PYTHON = "/usr/bin/python3"
FAKE_ANSIBLE_FAILURE_STATUS = 23
FAKE_UV_FAILURE_STATUS = 31
FAKE_UV_ARGV_FAILURE_STATUS = 93


class CommandRunner(Protocol):
    """Signature of fleet.py's streaming command runner."""

    def __call__(
        self,
        argv: list[str],
        stdin: str | bytes | None = None,
        cwd: Path | None = None,
    ) -> int:
        """Run one command with optional streamed stdin and working directory."""
        ...


def _fail(message: str) -> int:
    """Print one transport failure and return the fleet precondition status."""
    print(f"fleet: error: {message}", file=sys.stderr)
    return 2


def _command_output(argv: list[str]) -> tuple[int, str]:
    """Run a read-only probe and return its status and stripped stdout."""
    proc = subprocess.run(  # noqa: S603 -- argv comes from the validated declaration
        argv,
        text=True,
        capture_output=True,
        check=False,
    )
    if proc.returncode:
        sys.stderr.write(proc.stderr)
    return proc.returncode, proc.stdout.strip()


@dataclass(frozen=True)
class _RunnerImageTransfer:
    """Carry one verified runner-image stream between declared fleet hosts."""

    source_ssh: list[str]
    target_ssh: list[str]
    target: dict[str, Any]
    distro: str
    source_archive: str
    run: CommandRunner


def _stream_runner_image(transfer: _RunnerImageTransfer) -> int:
    """Stream one canonical archive into the WSL cache's staging path."""
    source_ssh = transfer.source_ssh
    target_ssh = transfer.target_ssh
    target = transfer.target
    distro = transfer.distro
    source_archive = transfer.source_archive
    run = transfer.run
    source = subprocess.Popen(  # noqa: S603 -- declaration-derived argv, no shell
        [*source_ssh, "/usr/bin/sudo", "-n", "/usr/bin/cat", "--", source_archive],
        stdout=subprocess.PIPE,
    )
    if source.stdout is None:  # pragma: no cover -- PIPE guarantees this
        source.terminate()
        return _fail("could not open the canonical archive stream")
    receive = fws.cache_receive_command(distro)
    sink = subprocess.Popen(  # noqa: S603 -- declaration-derived argv, no shell
        [*target_ssh, receive], stdin=source.stdout
    )
    source.stdout.close()
    sink_rc = sink.wait()
    source_rc = source.wait()
    if source_rc or sink_rc:
        run(
            [*target_ssh, fm.remote_shell(target)],
            stdin=fws.cache_cleanup_script(),
        )
        return _fail(f"runner image stream failed (source rc={source_rc}, target rc={sink_rc})")
    return 0


def _sync_runner_image(data: dict[str, Any], name: str, run: CommandRunner) -> int:
    """Stream the canonical runner archive into a WSL-local durable cache."""
    image = data["runner_image"]
    source_name = str(image["source_host"])
    source_archive = str(image["archive"])
    source_ssh = fr.ssh_target(data, source_name)
    target_ssh = fr.ssh_target(data, name)
    target = data["hosts"][name]
    distro = str(target["connect"]["distro"])
    rc, source_line = _command_output(
        [*source_ssh, "/usr/bin/sudo", "-n", "/usr/bin/sha256sum", "--", source_archive]
    )
    if rc or not source_line:
        return _fail(f"cannot checksum canonical runner archive on {source_name}")
    source_sha = source_line.split()[0]
    rc = run(
        [*target_ssh, fm.remote_shell(target)],
        stdin=fws.cache_prepare_script(),
    )
    if rc:
        return rc
    cache_probe = (
        f"wsl -d {shlex.quote(distro)} -u root -e /usr/bin/env -i "
        "HOME=/root PATH=/usr/bin:/bin /usr/bin/sha256sum -- "
        f"{shlex.quote(WSL_RUNNER_IMAGE_CACHE)}"
    )
    cache_rc, cache_line = _command_output([*target_ssh, cache_probe])
    if cache_rc == 0 and cache_line.split()[0] == source_sha:
        print(f"==> WSL runner image cache already matches {source_name} ({source_sha[:12]})")
        return 0
    print(f"==> streaming canonical runner image {source_name} -> {name}")
    transfer = _RunnerImageTransfer(
        source_ssh,
        target_ssh,
        target,
        distro,
        source_archive,
        run,
    )
    rc = _stream_runner_image(transfer)
    if rc:
        return rc
    return run(
        [*target_ssh, fm.remote_shell(target)],
        stdin=fws.cache_publish_script(source_sha),
    )


@dataclass(frozen=True)
class ConvergeSpec:
    """Describe one validated WSL converge without exposing secret values."""

    data: dict[str, Any]
    name: str
    plays: list[str]
    extra: list[str]
    typed_vars: ftv.TypedVars | None
    mode: str
    stage: str = WSL_STAGE
    ansible_playbook: str = WSL_ANSIBLE_PLAYBOOK
    system_python: str = WSL_SYSTEM_PYTHON
    managed_root: str = WSL_MANAGED_ROOT
    managed_cache: str = WSL_MANAGED_CACHE


def _isolation_lines() -> list[str]:
    """Render the inherited environment scrubbing boundary."""
    return [
        "set -euo pipefail",
        "while IFS='=' read -r name _; do",
        '  case "$name" in ANSIBLE_*) unset "$name" ;; esac',
        '  case "$name" in PYTHONHOME|PYTHONPATH|PYTHONNOUSERSITE) unset "$name" ;; esac',
        '  case "$name" in UV_*) unset "$name" ;; esac',
        "done < <(env)",
        "export PYTHONNOUSERSITE=1",
    ]


def _managed_authority_paths(stage: str) -> tuple[tuple[str, ...], tuple[str, ...]]:
    """Return the managed directories and files proven by the WSL payload."""
    paths = (
        stage,
        f"{stage}/infra",
        f"{stage}/infra/ansible",
        f"{stage}/.ansible",
        f"{stage}/.ansible/collections",
        f"{stage}/.tools",
        f"{stage}/.tools/uv",
    )
    files = (
        f"{stage}/infra/ansible/ansible.cfg",
        f"{stage}/infra/ansible/requirements.yml",
        f"{stage}/pyproject.toml",
        f"{stage}/uv.lock",
        f"{stage}/scripts/dev/bootstrap_uv.py",
        f"{stage}/scripts/dev/bootstrap_uv_exec.py",
        f"{stage}/scripts/dev/fleet_runner_maintenance.py",
        f"{stage}/scripts/dev/fleet_path_authority.py",
        f"{stage}/scripts/dev/uv_release.json",
        f"{stage}/scripts/dev/verify_locked_environment.py",
        f"{stage}/scripts/checks/check_ansible_collections.py",
    )
    return paths, files


def _path_proof_lines(stage: str, managed_root: str, managed_cache: str) -> list[str]:
    """Render exact no-link checks for every executable authority."""
    source_root = fm.REPO_ROOT if stage == WSL_STAGE else Path(stage)
    bootstrap = source_root / "scripts/dev/bootstrap_uv.py"
    helper = source_root / "scripts/dev/bootstrap_uv_exec.py"
    bootstrap_digest = hashlib.sha256(bootstrap.read_bytes()).hexdigest()
    helper_digest = hashlib.sha256(helper.read_bytes()).hexdigest()
    paths, files = _managed_authority_paths(stage)
    return [
        "require_real_dir() {",
        '  [ -d "$1" ] && [ ! -L "$1" ] && [ "$(readlink -f -- "$1")" = "$1" ] || {',
        '    echo "unsafe or missing managed directory: $1" >&2; exit 1;',
        "  }",
        "}",
        "require_real_file() {",
        '  [ -f "$1" ] && [ ! -L "$1" ] && [ "$(readlink -f -- "$1")" = "$1" ] || {',
        '    echo "unsafe or missing managed file: $1" >&2; exit 1;',
        "  }",
        "}",
        "require_exact_file() {",
        '  require_real_file "$1"',
        '  [ "$(stat -c %a -- "$1")" = "$2" ] || {',
        '    echo "wrong managed file mode: $1" >&2; exit 1;',
        "  }",
        '  file_digest="$(sha256sum -- "$1")"',
        '  [ "${file_digest%% *}" = "$3" ] || {',
        '    echo "changed managed file bytes: $1" >&2; exit 1;',
        "  }",
        "}",
        "refuse_mount() {",
        '  ! /usr/bin/mountpoint -q -- "$1" || {',
        '    echo "refusing managed mount point: $1" >&2; exit 1;',
        "  }",
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
        *[f"require_real_dir {shlex.quote(path)}" for path in paths],
        *[f"require_real_file {shlex.quote(path)}" for path in files],
        f"require_exact_file {shlex.quote(stage + '/scripts/dev/bootstrap_uv.py')} "
        f"755 {bootstrap_digest}",
        f"require_exact_file {shlex.quote(stage + '/scripts/dev/bootstrap_uv_exec.py')} "
        f"644 {helper_digest}",
        f"managed_root={shlex.quote(managed_root)}",
        f"managed_cache={shlex.quote(managed_cache)}",
        'require_real_dir "$(dirname "$managed_root")"',
        'require_real_dir "$(dirname "$managed_cache")"',
    ]


def _toolchain_sync_lines(stage: str, mode: str, system_python: str) -> list[str]:
    """Render apply-only lock sync and read-only verification for other modes."""
    if mode not in {"apply", "check", "remove"}:
        message = f"unsupported WSL convergence mode: {mode}"
        raise ValueError(message)
    bootstrap = f"{stage}/scripts/dev/bootstrap_uv.py"
    manifest = f"{stage}/scripts/dev/uv_release.json"
    uv_cache = f"{stage}/.tools/uv"
    sync_flags = (
        f"--no-config --directory {shlex.quote(stage)} sync --locked --only-group infra "
        "--no-install-project "
        f"--python {shlex.quote(system_python)}"
    )
    return [
        f"mode={shlex.quote(mode)}",
        f"cd {shlex.quote(stage)}",
        "uv_run() {",
        f"  {shlex.quote(system_python)} {shlex.quote(bootstrap)} "
        f"--manifest {shlex.quote(manifest)} --cache-root {shlex.quote(uv_cache)} "
        '--run "$@"',
        "}",
        'authority_digest="$(sha256sum -- '
        f"{shlex.quote(stage + '/pyproject.toml')} {shlex.quote(stage + '/uv.lock')} "
        f"{shlex.quote(manifest)} {shlex.quote(stage + '/infra/ansible/requirements.yml')} "
        '| sha256sum)"',
        "authority_digest=${authority_digest%% *}",
        'if [ "$mode" = apply ]; then',
        '  if [ -e "$managed_root" ] || [ -L "$managed_root" ]; then',
        '    require_real_dir "$managed_root"',
        '    refuse_mount "$managed_root"',
        "  else",
        '    install -d -m 0755 -- "$managed_root"',
        '    refuse_mount "$managed_root"',
        "  fi",
        '  if [ -e "$managed_cache" ] || [ -L "$managed_cache" ]; then',
        '    require_real_dir "$managed_cache"',
        '    refuse_mount "$managed_cache"',
        "  else",
        '    install -d -m 0755 -- "$managed_cache"',
        '    refuse_mount "$managed_cache"',
        "  fi",
        '  UV_PROJECT_ENVIRONMENT="$managed_root" UV_PYTHON_DOWNLOADS=never '
        'UV_CACHE_DIR="$managed_cache" uv_run '
        f"{sync_flags}",
        "else",
        '  require_real_dir "$managed_root"',
        '  refuse_mount "$managed_root"',
        '  [ -f "$managed_root/.ra8-infra-lock.sha256" ] && '
        '    [ ! -L "$managed_root/.ra8-infra-lock.sha256" ] && '
        '    [ "$(cat "$managed_root/.ra8-infra-lock.sha256")" = "$authority_digest" ] || {',
        '    echo "managed WSL Python environment is absent or stale; run infra apply" >&2;',
        "    exit 1;",
        "  }",
        '  UV_PROJECT_ENVIRONMENT="$managed_root" UV_PYTHON_DOWNLOADS=never '
        'UV_CACHE_DIR="$managed_cache" uv_run --offline --no-cache '
        f"{sync_flags} --check",
        "fi",
    ]


def _toolchain_verify_lines(stage: str, ansible_playbook: str) -> list[str]:
    """Render exact-set, collection, and durable-authority verification."""
    verifier = f"{stage}/scripts/dev/verify_locked_environment.py"
    collection_checker = f"{stage}/scripts/checks/check_ansible_collections.py"
    return [
        'require_real_file "$managed_root/bin/python3"',
        f"require_real_file {shlex.quote(ansible_playbook)}",
        'require_real_file "$managed_root/bin/ansible-galaxy"',
        "(",
        '  locked_export="$(mktemp "$managed_root/.ra8-infra-export.XXXXXX")"',
        "  trap 'rm -f -- \"$locked_export\"' EXIT",
        "  uv_run --no-config --directory "
        + shlex.quote(stage)
        + " export --locked --offline --only-group infra "
        '--no-emit-project --no-header >"$locked_export"',
        '  "$managed_root/bin/python3" ' + shlex.quote(verifier) + ' "$locked_export"',
        ")",
        "ANSIBLE_COLLECTIONS_PATH=" + shlex.quote(stage + "/.ansible/collections") + " "
        '"$managed_root/bin/ansible-galaxy" collection list --format json | '
        '"$managed_root/bin/python3" '
        + shlex.quote(collection_checker)
        + " --stdin --root "
        + shlex.quote(stage + "/.ansible/collections"),
        'if [ "$mode" = apply ]; then',
        '  marker="$managed_root/.ra8-infra-lock.sha256.tmp.$$"',
        "  trap 'rm -f -- \"$marker\"' EXIT",
        '  printf \'%s\\n\' "$authority_digest" >"$marker"',
        '  chmod 0644 "$marker"',
        '  mv -f -- "$marker" "$managed_root/.ra8-infra-lock.sha256"',
        '  sync_file "$managed_root/.ra8-infra-lock.sha256"',
        '  sync_dir "$managed_root"',
        "  trap - EXIT",
        "fi",
        f"cd {shlex.quote(stage + '/infra/ansible')}",
        'export ANSIBLE_CONFIG="$PWD/ansible.cfg"',
        'export ANSIBLE_COLLECTIONS_PATH="$PWD/../../.ansible/collections"',
        "export ANSIBLE_COLLECTIONS_SCAN_SYS_PATH=false",
    ]


def _ansible_environment_lines(spec: ConvergeSpec) -> list[str]:
    """Render the exact remote managed-tool and Ansible boundary."""
    return [
        *_isolation_lines(),
        *_path_proof_lines(spec.stage, spec.managed_root, spec.managed_cache),
        *_toolchain_sync_lines(spec.stage, spec.mode, spec.system_python),
        *_toolchain_verify_lines(spec.stage, spec.ansible_playbook),
    ]


def render_converge(spec: ConvergeSpec) -> tuple[str, list[str]]:
    """Render the stdin script and secret-free summaries for a WSL converge."""
    data = spec.data
    name = spec.name
    plays = spec.plays
    extra = spec.extra
    typed_vars = spec.typed_vars
    stage = spec.stage
    ansible_playbook = spec.ansible_playbook
    host = data["hosts"][name]
    role_variables = fm.role_vars(data, name, host)
    role_variables["ci_runner_docker_image_source_local_archive"] = WSL_RUNNER_IMAGE_CACHE
    lines = _ansible_environment_lines(spec)
    if typed_vars is not None:
        encoded = base64.b64encode(typed_vars.content).decode("ascii")
        lines.extend(
            [
                "umask 077",
                f'ra8_vars_file="$(mktemp {shlex.quote(stage + "/.ansible-vars.XXXXXX")})"',
                'cleanup_ra8_vars() { rm -f -- "$ra8_vars_file"; }',
                "trap cleanup_ra8_vars EXIT",
                "trap 'exit 130' INT",
                "trap 'exit 143' TERM HUP",
                "base64 -d >\"$ra8_vars_file\" <<'RA8_TYPED_VARS_EOF'",
                *textwrap.wrap(encoded, width=76),
                "RA8_TYPED_VARS_EOF",
                'typed_args=(-e "@$ra8_vars_file")',
            ]
        )
    else:
        lines.append("typed_args=()")
    summaries: list[str] = []
    for play in plays:
        playbook = fm.PLAYS[play].playbook
        argv = [
            ansible_playbook,
            "--connection=local",
            "-i",
            "localhost,",
            f"playbooks/{playbook}",
            "-e",
            json.dumps(role_variables),
            "-e",
            f"wsl_ci_host_id={name}",
            "-e",
            f"fleet_capacity_src={stage}/scripts/ci/fleet_capacity.sh",
            *extra,
        ]
        lines.append(shlex.join(argv) + ' "${typed_args[@]}"')
        summaries.append(f"ansible-playbook {playbook} (WSL host {name})")
    return "\n".join(lines) + "\n", summaries


def converge(
    spec: ConvergeSpec,
    sync_image: bool,
    run: CommandRunner,
) -> int:
    """Stage and run WSL plays without putting secret values in process argv."""
    data = spec.data
    name = spec.name
    host = data["hosts"][name]
    rc = fws.push(data, name, spec.mode, run)
    if rc:
        return rc
    if sync_image:
        rc = _sync_runner_image(data, name, run)
        if rc:
            return rc
    script, summaries = render_converge(spec)
    for summary in summaries:
        print(f"==> {summary}")
    return run([*fr.ssh_target(data, name), fm.remote_shell(host)], stdin=script)


@dataclass(frozen=True)
class _SelftestFixture:
    """Paths used to observe one offline WSL transport simulation."""

    stage: Path
    managed_root: Path
    managed_cache: Path
    system_python: Path
    args_log: Path
    vars_path_log: Path
    mode_log: Path
    uv_log: Path
    python_log: Path
    injected_path: Path
    hostile_executed: Path


def _write_executable(path: Path, source: str) -> None:
    """Write one executable selftest helper."""
    path.write_text(source, encoding="ascii")
    path.chmod(0o755)


def _remote_boundary_selftest(data: dict[str, Any], root: Path) -> list[str]:
    """Prove BASH_ENV cannot execute before the streamed WSL payload."""
    failures: list[str] = []
    marker = root / "bash-env-ran"
    startup = root / "hostile-bash-env"
    startup.write_text(f"touch {shlex.quote(str(marker))}\n", encoding="ascii")
    boundary = fm.remote_shell(data["hosts"]["win-ci"])
    expected = "-u root -e /usr/bin/env -i HOME=/root PATH=/usr/bin:/bin /bin/bash -s"
    if expected not in boundary:
        failures.append("WSL actual remote boundary is not env-empty before Bash")
    result = subprocess.run(
        [
            "/usr/bin/env",
            "-i",
            "HOME=/root",
            "PATH=/usr/bin:/bin",
            "/bin/bash",
            "-s",
        ],
        input="true\n",
        env={"BASH_ENV": str(startup), "PATH": "/hostile"},
        text=True,
        check=False,
    )
    if result.returncode or marker.exists():
        failures.append("env-empty Bash boundary executed hostile BASH_ENV")
    return failures


def _write_stage_authorities(stage: Path) -> None:
    """Create the exact authority shape consumed by the rendered shell."""
    (stage / "infra" / "ansible").mkdir(parents=True)
    (stage / ".ansible" / "collections").mkdir(parents=True)
    (stage / ".tools" / "uv").mkdir(parents=True)
    for relative, content in (
        ("infra/ansible/ansible.cfg", "[defaults]\n"),
        ("infra/ansible/requirements.yml", "collections: []\n"),
        ("pyproject.toml", "[project]\nname='fixture'\n"),
        ("uv.lock", "version = 1\n"),
        ("scripts/dev/bootstrap_uv.py", "# fixture\n"),
        ("scripts/dev/bootstrap_uv_exec.py", "# fixture\n"),
        ("scripts/dev/fleet_runner_maintenance.py", "# fixture\n"),
        ("scripts/dev/fleet_path_authority.py", "# fixture\n"),
        ("scripts/dev/uv_release.json", "{}\n"),
        ("scripts/dev/verify_locked_environment.py", "# fixture\n"),
        ("scripts/checks/check_ansible_collections.py", "# fixture\n"),
    ):
        path = stage / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="ascii")
    (stage / "scripts/dev/bootstrap_uv.py").chmod(0o755)
    (stage / "scripts/dev/bootstrap_uv_exec.py").chmod(0o644)


def _write_fake_toolchain(root: Path, fixture: _SelftestFixture) -> None:
    """Create fake uv/Python/Ansible programs with observation logs."""
    managed_bin = fixture.managed_root / "bin"
    managed_bin.mkdir(parents=True)
    fake_uv = root / "verified-uv"
    _write_executable(
        fixture.system_python,
        "#!/usr/bin/env bash\nset -eu\n"
        '[ -z "${UV_CONFIG_FILE:-}" ]\n[ -z "${UV_INDEX_URL:-}" ]\n'
        '[ "$1" = "$RA8_TEST_STAGE/scripts/dev/bootstrap_uv.py" ]\nshift\n'
        '[ "$1" = --manifest ]\n'
        '[ "$2" = "$RA8_TEST_STAGE/scripts/dev/uv_release.json" ]\nshift 2\n'
        '[ "$1" = --cache-root ]\n'
        '[ "$2" = "$RA8_TEST_STAGE/.tools/uv" ]\nshift 2\n'
        '[ "$1" = --run ]\nshift\n(($#))\n'
        'exec "$RA8_TEST_UV" "$@"\n',
    )
    _write_executable(
        fake_uv,
        "#!/usr/bin/python3\n"
        "import os\n"
        "import sys\n"
        "from pathlib import Path\n"
        "stage = os.environ['RA8_TEST_STAGE']\n"
        "args = sys.argv[1:]\n"
        "if Path.cwd().resolve() != Path(stage).resolve():\n"
        "    raise SystemExit(92)\n"
        "base = ['--no-config', '--directory', stage]\n"
        "system_python = os.environ['RA8_TEST_SYSTEM_PYTHON']\n"
        "sync = [*base, 'sync', '--locked', '--only-group', 'infra', "
        "'--no-install-project', '--python', system_python]\n"
        "check = ['--offline', '--no-cache', *sync, '--check']\n"
        "export = [*base, 'export', '--locked', '--offline', '--only-group', "
        "'infra', '--no-emit-project', '--no-header']\n"
        "if args not in (sync, check, export):\n"
        f"    raise SystemExit({FAKE_UV_ARGV_FAILURE_STATUS})\n"
        "operation = 'export' if args == export else 'sync'\n"
        "with Path(os.environ['RA8_TEST_UV_LOG']).open('a', encoding='ascii') as stream:\n"
        "    stream.write('|'.join(args) + '\\n')\n"
        "if os.environ.get('RA8_TEST_UV_FAIL') == operation:\n"
        f"    raise SystemExit({FAKE_UV_FAILURE_STATUS})\n"
        "if operation == 'export':\n"
        "    for index in range(10):\n"
        "        print(f'package-{index}==1.{index} \\\\')\n"
        "        print(f'    --hash=sha256:{index:064d}')\n",
    )
    python = managed_bin / "python3"
    _write_executable(
        python,
        "#!/usr/bin/env bash\nset -eu\n"
        'printf \'%s\\n\' "$*" >>"$RA8_TEST_PYTHON_LOG"\n'
        'case "$1" in *check_ansible_collections.py) cat >/dev/null ;; esac\n',
    )
    galaxy = managed_bin / "ansible-galaxy"
    _write_executable(galaxy, "#!/usr/bin/env bash\nprintf '{}\\n'\n")


def _write_fake_playbook(fixture: _SelftestFixture, sentinel: str) -> None:
    """Create the managed failing Ansible executable used by the transport test."""
    fake = fixture.managed_root / "bin" / "ansible-playbook"
    _write_executable(
        fake,
        "#!/usr/bin/env bash\nset -eu\n"
        ': >"$RA8_TEST_ARGS"\nvars_file=\nfor arg in "$@"; do\n'
        '  printf \'%s\\n\' "$arg" >>"$RA8_TEST_ARGS"\n'
        '  case "$arg" in @*) vars_file=${arg#@} ;; esac\ndone\n'
        '[ -n "$vars_file" ]\n'
        'printf \'%s\\n\' "$vars_file" >"$RA8_TEST_VARS_PATH"\n'
        'stat -c \'%a\' "$vars_file" >"$RA8_TEST_MODE"\n'
        '[ "$ANSIBLE_CONFIG" = "$PWD/ansible.cfg" ]\n'
        '[ "$ANSIBLE_COLLECTIONS_PATH" = "$PWD/../../.ansible/collections" ]\n'
        '[ "$ANSIBLE_COLLECTIONS_SCAN_SYS_PATH" = false ]\n'
        '[ "$PYTHONNOUSERSITE" = 1 ]\n'
        '[ -z "${PYTHONHOME:-}" ]\n[ -z "${PYTHONPATH:-}" ]\n'
        '[ -z "${ANSIBLE_ROLES_PATH:-}" ]\n'
        f'grep -q {shlex.quote(sentinel)} "$vars_file"\n'
        f"exit {FAKE_ANSIBLE_FAILURE_STATUS}\n",
    )


def _make_fixture(root: Path, sentinel: str) -> _SelftestFixture:
    """Create a fake failing Ansible executable and its observation paths."""
    stage = root / "stage with spaces"
    _write_stage_authorities(stage)
    managed_root = root / "managed tools"
    managed_cache = root / "managed cache" / "uv"
    managed_cache.mkdir(parents=True)
    system_bin = root / "system"
    system_bin.mkdir()
    fixture = _SelftestFixture(
        stage,
        managed_root,
        managed_cache,
        system_bin / "python3",
        root / "args.log",
        root / "vars-path.log",
        root / "mode.log",
        root / "uv.log",
        root / "python.log",
        root / "SHOULD_NOT_EXIST",
        root / "HOSTILE_PATH_RAN",
    )
    _write_fake_toolchain(root, fixture)
    _write_fake_playbook(fixture, sentinel)
    hostile_bin = root / "hostile"
    hostile_bin.mkdir()
    _write_executable(
        hostile_bin / "ansible-playbook",
        f"#!/bin/sh\ntouch {shlex.quote(str(fixture.hostile_executed))}\nexit 99\n",
    )
    return fixture


def _run_script(
    script: str,
    fixture: _SelftestFixture,
    uv_failure: str = "",
    expected_system_python: str = "",
) -> subprocess.CompletedProcess[str]:
    """Run the offline WSL shell with only a fake Ansible executable."""
    env = {
        **os.environ,
        "PATH": f"{fixture.stage.parent / 'hostile'}:/usr/bin:/bin",
        "RA8_TEST_ARGS": str(fixture.args_log),
        "RA8_TEST_SYSTEM_PYTHON": expected_system_python or str(fixture.system_python),
        "RA8_TEST_VARS_PATH": str(fixture.vars_path_log),
        "RA8_TEST_MODE": str(fixture.mode_log),
        "RA8_TEST_UV": str(fixture.stage.parent / "verified-uv"),
        "RA8_TEST_UV_LOG": str(fixture.uv_log),
        "RA8_TEST_UV_FAIL": uv_failure,
        "RA8_TEST_STAGE": str(fixture.stage),
        "RA8_TEST_PYTHON_LOG": str(fixture.python_log),
        "ANSIBLE_CONFIG": str(fixture.stage.parent / "hostile.cfg"),
        "ANSIBLE_ROLES_PATH": str(fixture.stage.parent / "hostile-roles"),
        "PYTHONHOME": str(fixture.stage.parent / "hostile-python-home"),
        "PYTHONPATH": str(fixture.stage.parent / "hostile-python-path"),
        "UV_CONFIG_FILE": str(fixture.stage.parent / "hostile-uv.toml"),
        "UV_INDEX_URL": "https://hostile.invalid/simple",
    }
    return subprocess.run(
        ["/bin/bash"], input=script, text=True, env=env, capture_output=True, check=False
    )


def _check_result(
    result: subprocess.CompletedProcess[str],
    fixture: _SelftestFixture,
    attack: str,
    sentinel: str,
) -> list[str]:
    """Check argv integrity, mode, redaction, and cleanup after fake failure."""
    if result.returncode != FAKE_ANSIBLE_FAILURE_STATUS:
        message = (
            f"fake failing Ansible returned {result.returncode}, "
            f"expected {FAKE_ANSIBLE_FAILURE_STATUS}"
        )
        return [message]
    failures: list[str] = []
    argv_text = fixture.args_log.read_text(encoding="utf-8")
    if attack not in argv_text.splitlines() or fixture.injected_path.exists():
        failures.append("WSL argument quoting did not preserve a metacharacter-bearing tag")
    if sentinel in argv_text:
        failures.append("WSL secret appeared in ansible-playbook argv")
    remote_vars = Path(fixture.vars_path_log.read_text(encoding="utf-8").strip())
    if fixture.mode_log.read_text(encoding="utf-8").strip() != "600":
        failures.append("WSL temporary vars file was not mode 0600")
    if remote_vars.exists():
        failures.append("WSL temporary vars file survived Ansible failure")
    if fixture.hostile_executed.exists():
        failures.append("WSL used a hostile PATH ansible-playbook")
    return failures


def _render_fixture(
    data: dict[str, Any], fixture: _SelftestFixture, typed: ftv.TypedVars, attack: str, mode: str
) -> tuple[str, list[str]]:
    """Render one offline WSL converge against only fixture-owned paths."""
    spec = ConvergeSpec(
        data,
        "win-ci",
        ["wsl-ci-host"],
        ["--tags", attack],
        typed,
        mode,
        str(fixture.stage),
        str(fixture.managed_root / "bin" / "ansible-playbook"),
        str(fixture.system_python),
        str(fixture.managed_root),
        str(fixture.managed_cache),
    )
    return render_converge(spec)


def _toolchain_mode_selftest(
    data: dict[str, Any], fixture: _SelftestFixture, typed: ftv.TypedVars, attack: str
) -> tuple[list[str], str, list[str], str]:
    """Exercise apply sync, check-only verification, and uv failure handling."""
    failures: list[str] = []
    apply_script, summaries = _render_fixture(data, fixture, typed, attack, "apply")
    apply_result = _run_script(apply_script, fixture)
    failures.extend(_check_result(apply_result, fixture, attack, "fleet-secret-sentinel"))
    marker = fixture.managed_root / ".ra8-infra-lock.sha256"
    if not marker.is_file() or marker.is_symlink():
        failures.append("WSL apply did not publish its exact lock marker")
    check_script, _ = _render_fixture(data, fixture, typed, attack, "check")
    check_result = _run_script(check_script, fixture)
    failures.extend(_check_result(check_result, fixture, attack, "fleet-secret-sentinel"))
    if fixture.uv_log.is_file():
        sync_calls = [
            line
            for line in fixture.uv_log.read_text(encoding="ascii").splitlines()
            if "sync" in line
        ]
        expected_sync_calls = 2
        if (
            len(sync_calls) != expected_sync_calls
            or "--check" in sync_calls[0]
            or "--check" not in sync_calls[1]
        ):
            failures.append(
                "WSL apply/check modes did not preserve sync versus verify-only behavior"
            )
    else:
        failures.append("WSL apply/check did not invoke the authenticated uv fixture")
    hardcoded_python = _run_script(
        apply_script,
        fixture,
        expected_system_python="/usr/bin/python3",
    )
    if hardcoded_python.returncode != FAKE_UV_ARGV_FAILURE_STATUS:
        failures.append(
            "WSL did not reject a hardcoded system Python path: "
            f"expected {FAKE_UV_ARGV_FAILURE_STATUS}, got "
            f"{hardcoded_python.returncode}"
        )
    sync_failure = _run_script(apply_script, fixture, "sync")
    if sync_failure.returncode != FAKE_UV_FAILURE_STATUS:
        failures.append("WSL masked an authenticated uv sync failure")
    export_failure = _run_script(check_script, fixture, "export")
    if export_failure.returncode != FAKE_UV_FAILURE_STATUS:
        failures.append("WSL masked an authenticated uv export failure")
    return failures, apply_script, summaries, check_script


def _toolchain_authority_selftest(
    fixture: _SelftestFixture,
    mode_result: tuple[list[str], str, list[str], str],
) -> tuple[list[str], str, list[str]]:
    """Exercise bootstrap-helper identity and staged-config link rejection."""
    failures, apply_script, summaries, check_script = mode_result

    helper = fixture.stage / "scripts/dev/bootstrap_uv_exec.py"
    helper.chmod(0o755)
    wrong_mode = _run_script(check_script, fixture)
    if wrong_mode.returncode == FAKE_ANSIBLE_FAILURE_STATUS:
        failures.append("WSL accepted a wrong-mode bootstrap execution helper")
    helper.chmod(0o644)
    original_helper = helper.read_bytes()
    helper.write_bytes(original_helper + b"changed\n")
    changed_helper = _run_script(check_script, fixture)
    if changed_helper.returncode == FAKE_ANSIBLE_FAILURE_STATUS:
        failures.append("WSL accepted changed bootstrap execution-helper bytes")
    helper.write_bytes(original_helper)
    helper.chmod(0o644)

    config = fixture.stage / "infra" / "ansible" / "ansible.cfg"
    real_config = config.with_name("real.cfg")
    config.rename(real_config)
    config.symlink_to(real_config)
    linked_result = _run_script(check_script, fixture)
    if linked_result.returncode == FAKE_ANSIBLE_FAILURE_STATUS:
        failures.append("WSL accepted a symlinked staged Ansible config")
    return failures, apply_script, summaries


def _toolchain_selftest(
    data: dict[str, Any], fixture: _SelftestFixture, typed: ftv.TypedVars, attack: str
) -> tuple[list[str], str, list[str]]:
    """Exercise managed-tool modes and authority rejection in original order."""
    mode_result = _toolchain_mode_selftest(data, fixture, typed, attack)
    return _toolchain_authority_selftest(fixture, mode_result)


def run_selftest(data: dict[str, Any]) -> list[str]:
    """Exercise quoted WSL transport, redaction, and failure cleanup."""
    sentinel = "fleet-secret-sentinel"
    content = f"ci_runner_docker_registration_token: {sentinel}\n".encode()
    typed = ftv.TypedVars(Path("/captured/registration.yml"), content)
    stage_failures = fws.run_selftest()
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-wsl-") as scratch:
        fixture = _make_fixture(Path(scratch), sentinel)
        attack = f"capacity; touch {fixture.injected_path}"
        failures, script, summaries = _toolchain_selftest(data, fixture, typed, attack)
        if not fixture.uv_log.is_file():
            failures.append("WSL managed-tool mode selftest did not execute")
        config = fixture.stage / "infra" / "ansible" / "ansible.cfg"
        if not config.is_symlink():
            failures.append("WSL managed-authority selftest did not execute")
        failures.extend(_remote_boundary_selftest(data, Path(scratch)))
        failures[:0] = stage_failures
        encoded = base64.b64encode(content).decode("ascii")
        summary = "\n".join(summaries)
        if sentinel in script or sentinel in summary or encoded in summary:
            failures.append("WSL secret appeared in raw script text or rendered summaries")
        return failures
