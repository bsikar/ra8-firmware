# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Reviewed semantic fixtures for the uv cache policy checker."""

from __future__ import annotations

import ast


def bootstrap_module_mutations() -> tuple[tuple[str, str], ...]:
    """Return caller-side module mutation and append-only attacks."""
    return (
        (
            "import bootstrap_uv_exec\n",
            "import bootstrap_uv_exec\n"
            "bootstrap_uv_exec._require_trusted_system_root = lambda _descriptor: None\n",
        ),
        (
            "import bootstrap_uv_exec\n",
            "import bootstrap_uv_exec\n"
            'setattr(bootstrap_uv_exec, "_require_trusted_system_root", '
            "lambda _descriptor: None)\n",
        ),
        (
            "import bootstrap_uv_exec\n",
            "import bootstrap_uv_exec\n"
            'globals()["bootstrap_uv_exec"]._require_trusted_system_root = '
            "lambda _descriptor: None\n",
        ),
        (
            "import bootstrap_uv_exec\n",
            "import bootstrap_uv_exec\nUV_BOOTSTRAP_UNREVIEWED_SURFACE = True\n",
        ),
    )


def mode_module_mutations() -> tuple[tuple[str, str], ...]:
    """Return mode-runner rebind and append-only attacks."""
    anchor = "    cache_mode_readonly_and_windows_selftest()\n"
    return (
        (anchor, f"{anchor}run_mode_selftest = lambda: None\n"),
        (anchor, f'{anchor}globals()["run_mode_selftest"] = lambda: None\n'),
        (anchor, f"{anchor}UV_MODE_TEST_UNREVIEWED_SURFACE = True\n"),
    )


def mutate_named_function_once(source: str, name: str, old: str, new: str) -> str:
    """Apply one mutation only inside one named top-level Python function."""
    functions = [
        node
        for node in ast.parse(source).body
        if isinstance(node, ast.FunctionDef) and node.name == name
    ]
    if len(functions) != 1 or functions[0].end_lineno is None:
        message = f"selftest function anchor drifted: {name}"
        raise ValueError(message)
    function = functions[0]
    lines = source.splitlines(keepends=True)
    segment = "".join(lines[function.lineno - 1 : function.end_lineno])
    if segment.count(old) != 1:
        message = f"selftest mutation anchor count changed in {name}: {old!r}"
        raise ValueError(message)
    lines[function.lineno - 1 : function.end_lineno] = [segment.replace(old, new, 1)]
    return "".join(lines)


def portable_execution_references() -> dict[str, tuple[tuple[str, str, int], ...]]:
    """Return private-name snapshot and attack-test references."""
    return {
        "portable_readonly_fd_selftest": (
            ("importlib", "import_module", 1),
            ("controls", "fcntl", 1),
            ("b.bootstrap_uv_exec", "portable_named_exec_snapshot", 1),
            ("os", "fstat", 1),
            ("os", "pread", 1),
            ("os", "write", 1),
            ("os", "posix_spawn", 1),
            ("os", "waitpid", 1),
            ("b", "fail", 6),
        ),
        "run_portable_snapshot": (("b.bootstrap_uv_exec", "portable_named_exec_snapshot", 1),),
        "portable_snapshot_flags_selftest": (
            ("mock.patch", "object", 1),
            ("", "run_portable_snapshot", 1),
            ("b", "fail", 3),
        ),
        "portable_snapshot_path_attack_selftest": (
            ("os", "unlink", 1),
            ("os", "rename", 1),
            ("os", "symlink", 1),
            ("os", "link", 1),
            ("os", "chmod", 2),
            ("mock.patch", "object", 1),
            ("", "expect_exec_failure", 1),
            ("b", "fail", 1),
        ),
        "portable_snapshot_unlink_failure_selftest": (
            ("mock.patch", "object", 1),
            ("", "expect_exec_failure", 1),
            ("b", "fail", 1),
        ),
    }


def mode_execution_references() -> dict[str, tuple[tuple[str, str, int], ...]]:
    """Return cache execution/status/mode selftest references."""
    return {
        "cache_exact_fd_execution_selftest": (
            ("destination", "rename", 1),
            ("replacement", "replace", 1),
            ("subprocess", "run", 1),
            ("b", "run_cached_uv", 1),
            ("b", "expect_bootstrap_error", 1),
        ),
        "cache_same_inode_execution_selftest": (
            ("os", "fsync", 1),
            ("subprocess", "run", 1),
            ("b", "run_cached_uv", 1),
            ("b", "expect_bootstrap_error", 1),
        ),
        "cache_run_exit_status_selftest": (
            ("sys", "executable", 1),
            ("os", "posix_spawn", 1),
            ("os", "waitpid", 1),
            ("b", "fail", 1),
        ),
        "cache_run_signal_status_selftest": (
            ("sys", "executable", 1),
            ("os", "posix_spawn", 1),
            ("os", "waitpid", 1),
            ("os", "WIFSIGNALED", 1),
            ("os", "WTERMSIG", 1),
            ("b", "fail", 2),
        ),
        "cache_mode_path_attack_selftest": (
            ("os", "fchmod", 1),
            ("b", "normalize_cached_modes", 1),
        ),
        "bootstrap_run_status": (
            ("sys", "executable", 1),
            ("os", "posix_spawn", 1),
            ("os", "waitpid", 1),
            ("b", "fail", 1),
        ),
        "cache_status_contract_selftest": (
            ("b", "verify_cached_uv", 3),
            ("", "bootstrap_run_status", 3),
        ),
        "cache_mode_readonly_and_windows_selftest": (
            ("b", "verify_cached_uv", 2),
            ("b", "ensure_uv", 1),
        ),
    }


def _bootstrap_io_expected_bodies() -> dict[str, str]:
    """Return reviewed cache-open and bounded-read bodies."""
    return {
        "write_atomic": """
def write_atomic(path, payload, executable=False):
    if os.name != "posix":
        fail("authenticated uv cache writes require POSIX; use WSL on Windows")
    mode = PRIVATE_EXECUTABLE_MODE if executable else PRIVATE_ARCHIVE_MODE
    try:
        bootstrap_uv_exec.write_atomic_nofollow(path, payload, mode)
    except bootstrap_uv_exec.UvExecError as exc:
        fail(str(exc))
""",
        "open_cache_fd": """
def open_cache_fd(path):
    try:
        return bootstrap_uv_exec.open_regular_nofollow(path)
    except bootstrap_uv_exec.UvExecError as exc:
        fail(str(exc))
""",
        "read_stable_fd": """
def read_stable_fd(descriptor, path, maximum):
    before = os.fstat(descriptor)
    try:
        with os.fdopen(os.dup(descriptor), "rb") as source:
            payload = source.read(maximum + 1)
        after = os.fstat(descriptor)
    except OSError as exc:
        fail(f"cannot read cached uv artifact {path}: {exc}")
    stable = ("st_dev", "st_ino", "st_size", "st_mtime_ns", "st_ctime_ns", "st_nlink")
    if any(getattr(before, field) != getattr(after, field) for field in stable):
        fail(f"cached uv artifact changed while authenticating: {path}")
    if len(payload) > maximum:
        fail(f"cached uv artifact exceeds policy: {path}")
    return payload, after
""",
    }


def _bootstrap_cache_expected_bodies() -> dict[str, str]:
    """Return reviewed cache authentication and stability bodies."""
    return {
        "authenticated_cache_fds": """
def authenticated_cache_fds(archive_path, destination, asset_name, digest):
    archive_fd = open_cache_fd(archive_path)
    destination_fd = -1
    try:
        destination_fd = open_cache_fd(destination)
        payload, archive_state = read_stable_fd(archive_fd, archive_path, MAX_ASSET_BYTES)
        verify_payload(payload, digest)
        binary = executable_bytes(payload, asset_name)
        installed, installed_state = read_stable_fd(
            destination_fd, destination, MAX_UV_BINARY_BYTES
        )
        if not binary or installed != binary:
            fail(f"cached uv executable differs from verified archive: {destination}")
        yield archive_fd, destination_fd, binary, archive_state, installed_state
    finally:
        if destination_fd >= 0:
            os.close(destination_fd)
        os.close(archive_fd)
""",
        "verify_fd_unchanged": """
def verify_fd_unchanged(path, descriptor, expected):
    current = os.fstat(descriptor)
    stable = ("st_dev", "st_ino", "st_size", "st_mtime_ns", "st_ctime_ns", "st_nlink")
    if any(getattr(expected, field) != getattr(current, field) for field in stable):
        fail(f"cached uv artifact changed after authenticating: {path}")
""",
        "verify_fd_path": """
def verify_fd_path(path, descriptor, mode):
    descriptor_state = os.fstat(descriptor)
    reopened = -1
    try:
        reopened = open_cache_fd(path)
        path_state = os.fstat(reopened)
    except (BootstrapError, OSError) as exc:
        fail(f"cached uv path moved during permission repair: {path}: {exc}")
    finally:
        if reopened >= 0:
            os.close(reopened)
    same_file = (descriptor_state.st_dev, descriptor_state.st_ino) == (
        path_state.st_dev,
        path_state.st_ino,
    )
    if not stat.S_ISREG(path_state.st_mode) or path_state.st_nlink != 1 or not same_file:
        fail(f"cached uv path moved during permission repair: {path}")
    if stat.S_IMODE(descriptor_state.st_mode) != mode:
        fail(f"cached uv permissions did not converge: {path}")
""",
    }


def _bootstrap_probe_expected_bodies() -> dict[str, str]:
    """Return reviewed anonymous probe and nested selftest bodies."""
    return {
        "probe_authenticated_uv": """
def probe_authenticated_uv(binary, version):
    try:
        completed = bootstrap_uv_exec.run_uv_snapshot(
            binary, ["--version"], capture_output=True, timeout=10
        )
    except bootstrap_uv_exec.UvExecError as exc:
        fail(str(exc))
    if completed.returncode != 0 or completed.stdout.split()[:2] != ["uv", version]:
        fail(f"installed uv failed its version probe: {completed.stderr.strip()}")
""",
        "cache_mode_selftest": """
def cache_mode_selftest():
    namespace = runpy.run_path(
        str(Path(__file__).with_name("bootstrap_uv_mode_selftest.py")),
        init_globals={"bootstrap": sys.modules[__name__]},
    )
    runner = namespace.get("run_mode_selftest")
    if not callable(runner):
        fail("uv mode selftest module has no runner")
    runner()
""",
    }


def _bootstrap_run_expected_bodies() -> dict[str, str]:
    """Return the reviewed cache-to-anonymous execution body."""
    return {
        "propagate_child_status": """
def propagate_child_status(status):
    if status >= 0:
        return status
    signum = -status
    unmask = getattr(signal, "pthread_sigmask", None)
    if signum >= signal.NSIG or unmask is None:
        fail("authenticated uv child returned an unsupported signal status")
    uncatchable = (signal.SIGKILL, signal.SIGSTOP)
    try:
        if signum not in uncatchable:
            signal.signal(signum, signal.SIG_DFL)
        unmask(signal.SIG_UNBLOCK, {signum})
        os.kill(os.getpid(), signum)
    except (OSError, ValueError) as exc:
        fail(f"cannot propagate authenticated uv child signal: {exc}")
    fail("authenticated uv child signal did not terminate the wrapper")
""",
        "run_cached_uv": """
def run_cached_uv(manifest_path, cache_root, arguments, *, ensure):
    if not arguments:
        fail("authenticated uv execution requires at least one uv argument")
    if os.name != "posix":
        fail("authenticated uv execution requires POSIX; use WSL on Windows")
    destination = (
        ensure_uv(manifest_path, cache_root)
        if ensure
        else verify_cached_uv(manifest_path, cache_root)
    )
    manifest = load_manifest(manifest_path)
    _, asset_name, digest = select_asset(manifest)
    archive_path = destination.parent / asset_name
    with authenticated_cache_fds(archive_path, destination, asset_name, digest) as descriptors:
        archive_fd, destination_fd, binary, archive_state, installed_state = descriptors
        verify_fd_mode(archive_path, archive_fd, PUBLIC_ARCHIVE_MODE)
        verify_fd_mode(destination, destination_fd, PUBLIC_EXECUTABLE_MODE)
        try:
            completed = bootstrap_uv_exec.run_uv_snapshot(binary, arguments)
        except bootstrap_uv_exec.UvExecError as exc:
            fail(str(exc))
        verify_fd_unchanged(archive_path, archive_fd, archive_state)
        verify_fd_unchanged(destination, destination_fd, installed_state)
        verify_fd_path(archive_path, archive_fd, PUBLIC_ARCHIVE_MODE)
        verify_fd_path(destination, destination_fd, PUBLIC_EXECUTABLE_MODE)
    return propagate_child_status(completed.returncode)
""",
    }


def _bootstrap_cli_expected_bodies() -> dict[str, str]:
    """Return the reviewed run-mode CLI parser body."""
    return {
        "parse_args": """
def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--ensure", action="store_true", help="install and print pinned uv")
    mode.add_argument(
        "--verify-cache",
        action="store_true",
        help="authenticate an existing cache without downloads or writes",
    )
    mode.add_argument(
        "--check-cache-modes",
        action="store_true",
        help="check shared POSIX cache modes without authenticating or writing",
    )
    mode.add_argument("--print-path", action="store_true", help="print cache path without writes")
    mode.add_argument(
        "--run", nargs=argparse.REMAINDER, metavar="UV_ARG",
        help="run uv from an authenticated existing-cache snapshot",
    )
    mode.add_argument(
        "--ensure-and-run", nargs=argparse.REMAINDER, metavar="UV_ARG",
        help="ensure the cache, then run uv from an authenticated snapshot",
    )
    mode.add_argument("--selftest", action="store_true", help="run offline fail-closed tests")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--cache-root", type=Path, default=DEFAULT_CACHE)
    return parser.parse_args()
""",
    }


def _bootstrap_main_expected_bodies() -> dict[str, str]:
    """Return the reviewed mode-dispatch body."""
    return {
        "main": """
def main():
    args = parse_args()
    if args.selftest:
        run_selftest()
        status = 0
    elif args.run is not None:
        status = run_cached_uv(args.manifest, args.cache_root, args.run, ensure=False)
    elif args.ensure_and_run is not None:
        status = run_cached_uv(args.manifest, args.cache_root, args.ensure_and_run, ensure=True)
    else:
        manifest = load_manifest(args.manifest)
        _, asset_name, _ = select_asset(manifest)
        version = manifest["version"]
        if not isinstance(version, str):
            fail("uv manifest version is not a string")
        destination = cache_destination(args.cache_root, version, asset_name)
        if args.print_path:
            print(destination)
        elif args.check_cache_modes:
            verify_cached_modes(destination.parent / asset_name, destination)
            print(destination)
        elif args.verify_cache:
            print(verify_cached_uv(args.manifest, args.cache_root))
        else:
            print(ensure_uv(args.manifest, args.cache_root))
        status = 0
    return status
""",
    }


def bootstrap_expected_bodies() -> dict[str, str]:
    """Return every reviewed bootstrap semantic body."""
    return {
        **_bootstrap_io_expected_bodies(),
        **_bootstrap_cache_expected_bodies(),
        **_bootstrap_probe_expected_bodies(),
        **_bootstrap_run_expected_bodies(),
        **_bootstrap_cli_expected_bodies(),
        **_bootstrap_main_expected_bodies(),
    }
