# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Offline adversarial tests for remote-GDB arguments, state, and broker."""

from __future__ import annotations

import argparse
import contextlib
import importlib.util
import io
import os
import socket
import subprocess
import sys
import tempfile
import threading
import traceback
from collections.abc import Callable
from pathlib import Path
from types import ModuleType, SimpleNamespace
from unittest import mock

WAIT_SECONDS = 5.0
RIG_PARITY_CORPUS = {
    "PI_HOST": (
        "star",
        "star.local",
        "1user@host",
        "user@001.002.003.004",
        "192.168.1.20",
        "-host",
        ".user@host",
        "host.",
        "bad_host",
        "1.2.3",
        "1.2.3.999",
        "host;command",
        "debug@[2001:db8::20]",
    ),
    "JLINK_SN": (
        "123456789",
        "J-Link_1.2",
        "_",
        "a" * 128,
        "",
        ".bad",
        "a" * 129,
        "bad+value",
        "bad value",
    ),
    "JLINK_DEVICE": (
        "R7KA8D2KF_CPU0",
        "Cortex-M85.rev_1",
        "_",
        "a" * 128,
        "",
        ".bad",
        "a" * 129,
        "bad+value",
        "bad;value",
    ),
}


def _load(path: Path, name: str) -> ModuleType:
    """Execute the exact selected source bytes in a fresh module namespace."""
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        msg = f"cannot load {path}"
        raise RuntimeError(msg)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def _api(module: ModuleType, name: str) -> object:
    """Select one exact internal boundary under adversarial test."""
    return vars(module)[name]


def _spawn_status(argv: list[str], environment: dict[str, str]) -> int:
    """Run one fixed offline shell boundary without subprocess search semantics."""
    actions = [
        (os.POSIX_SPAWN_OPEN, 1, "/dev/null", os.O_WRONLY, 0o600),
        (os.POSIX_SPAWN_OPEN, 2, "/dev/null", os.O_WRONLY, 0o600),
    ]
    pid = os.posix_spawn(argv[0], argv, environment, file_actions=actions)
    _waited, status = os.waitpid(pid, 0)
    return os.waitstatus_to_exitcode(status)


def _case(failures: list[str], name: str, action: Callable[[], object], *, must_fail: bool) -> None:
    """Run one both-direction case and record an unexpected result."""
    try:
        action()
    except Exception as exc:  # noqa: BLE001 - deliberate fail-closed attacks.
        if not must_fail:
            failures.append(f"{name}: unexpected {type(exc).__name__}: {exc}")
        return
    if must_fail:
        failures.append(f"{name}: unexpectedly passed")


def _host_cases(args: ModuleType, failures: list[str]) -> None:
    """Attack SSH host option, metacharacter, and control-byte forms."""
    for host in ("pi.local", "runner@192.168.1.20", "192.168.1.20"):
        _case(
            failures,
            f"valid host {host}",
            lambda value=host: args.validate_host(value),
            must_fail=False,
        )
    invalid_hosts = (
        "-V",
        "-p2222",
        "--help",
        "bench#host",
        "user@@host",
        "bad host",
        "bad\thost",
        "bad;host",
        "bad|host",
        "bad&host",
        "$(id)",
        "`id`",
        "bad\nhost",
        "bad\rhost",
        "bad\vhost",
        "bad\fhost",
        "'bad'",
        '"bad"',
        "bad>file",
        "bad*host",
        "bad(host)",
        "debug@[2001:db8::20]",
    )
    for value in invalid_hosts:
        _case(
            failures,
            f"invalid host {value!r}",
            lambda value=value: args.validate_host(value),
            must_fail=True,
        )


def _transport_cases(args: ModuleType, failures: list[str]) -> None:
    """Attack serial, device, and port boundary forms."""
    hostile = (
        "123 456",
        "123\t456",
        "123;456",
        "123|456",
        "123&456",
        "$(id)",
        "`id`",
        "123\n456",
        "123'456",
        '123"456',
        "123>456",
        "123#456",
        "123*456",
        "123(456)",
        "123\r456",
        "123\v456",
        "123\f456",
        r"123\;456",
    )
    for value in hostile:
        _case(
            failures,
            f"invalid serial {value!r}",
            lambda value=value: args.validate_serial(value),
            must_fail=True,
        )
        _case(
            failures,
            f"invalid device {value!r}",
            lambda value=value: args.validate_device(value),
            must_fail=True,
        )
    _case(failures, "valid serial", lambda: args.validate_serial("123456789"), must_fail=False)
    _case(
        failures,
        "valid SEGGER device",
        lambda: args.validate_device("R7KA8D2KF_CPU0"),
        must_fail=False,
    )
    for port in ("1024", "2331", "65535"):
        _case(
            failures,
            f"valid port {port}",
            lambda value=port: args.validate_port(value),
            must_fail=False,
        )
    for port in ("", "0", "1023", "65536", "-1", "+2331", "23 31"):
        _case(
            failures,
            f"invalid port {port!r}",
            lambda value=port: args.validate_port(value),
            must_fail=True,
        )


def _remote_parse(command: str) -> tuple[int, tuple[str, ...]]:
    """Drive a generated command through the OpenSSH remote-shell parse layer."""
    input_read, input_write = os.pipe()
    output_read, output_write = os.pipe()
    actions = [
        (os.POSIX_SPAWN_DUP2, input_read, 0),
        (os.POSIX_SPAWN_DUP2, output_write, 1),
        (os.POSIX_SPAWN_CLOSE, input_write),
        (os.POSIX_SPAWN_CLOSE, output_read),
    ]
    pid = os.posix_spawn("/bin/sh", ["/bin/sh", "-c", command], os.environ, file_actions=actions)
    os.close(input_read)
    os.close(output_write)
    os.write(
        input_write,
        b'import os,sys\nos.write(1,b"\\0".join(v.encode("ascii") for v in sys.argv[1:]))\n',
    )
    os.close(input_write)
    raw = bytearray()
    while chunk := os.read(output_read, 4096):
        raw.extend(chunk)
    os.close(output_read)
    _waited, status = os.waitpid(pid, 0)
    fields = tuple(field.decode("ascii") for field in bytes(raw).split(b"\0") if field)
    return os.waitstatus_to_exitcode(status), fields


def _serialization_cases(args: ModuleType, failures: list[str]) -> None:
    """Prove exact remote argv after the unavoidable shell serialization."""
    cases = (
        (
            "R7KA8D2KF_CPU0",
            "123456789",
            "2331",
            ("--", "R7KA8D2KF_CPU0", "123456789", "2331"),
        ),
        (
            "Cortex-M85.rev_1",
            "0000123",
            "65535",
            ("--", "Cortex-M85.rev_1", "0000123", "65535"),
        ),
    )
    for device, serial, port, expected in cases:
        result, observed = _remote_parse(args.remote_command(serial, port, device=device))
        if result != 0 or observed != expected:
            failures.append(f"remote serialization: {result}, {observed!r}")


def _remote_cli_cases(args: ModuleType, failures: list[str]) -> None:
    """Require all three fields on the sole remote-supervisor command."""
    parser = _api(args, "_parser")()
    valid = (
        [
            "remote-command",
            "--device",
            "R7KA8D2KF_CPU0",
            "--serial",
            "123456789",
            "--port",
            "2331",
        ],
    )
    invalid = (
        ["remote-command", "--serial", "123456789", "--port", "2331"],
        ["remote-command", "cleanup", "--serial", "123456789", "--port", "2331"],
    )
    for argv in valid:
        with contextlib.redirect_stderr(io.StringIO()):
            try:
                parser.parse_args(argv)
            except SystemExit as exc:
                failures.append(f"valid remote CLI rejected {argv!r}: {exc.code}")
    for argv in invalid:
        with contextlib.redirect_stderr(io.StringIO()):
            try:
                parser.parse_args(argv)
            except SystemExit:
                continue
        failures.append(f"invalid remote CLI accepted {argv!r}")


def _accepts(action: Callable[[], object]) -> bool:
    try:
        action()
    except Exception:  # noqa: BLE001 - parity compares fail-closed outcomes.
        return False
    return True


def _rig_contract_parity(args: ModuleType, root: Path, failures: list[str]) -> None:
    """Bind defensive Python parsing to the sole public rig contract corpus."""
    contract = root / "scripts/hil/lib/rig_contract.sh"
    environment = {
        "BASH_ENV": "/nonexistent",
        "ENV": "/nonexistent",
        "HOME": "/nonexistent",
        "LC_ALL": "C",
        "PATH": "/usr/bin:/bin",
    }
    if _spawn_status(["/bin/bash", "-p", str(contract), "--selftest"], environment) != 0:
        failures.append("public rig contract selftest failed")
    validators = {
        "PI_HOST": args.validate_host,
        "JLINK_SN": args.validate_serial,
        "JLINK_DEVICE": args.validate_device,
    }
    for field, values in RIG_PARITY_CORPUS.items():
        for value in values:
            public = (
                _spawn_status(
                    ["/bin/bash", "-p", str(contract), "--validate", field, value],
                    environment,
                )
                == 0
            )
            defensive = _accepts(lambda value=value, field=field: validators[field](value))
            if public != defensive:
                failures.append(
                    f"rig contract parity mismatch {field}={value!r}: "
                    f"public={public} defensive={defensive}"
                )


def _app_cases(args: ModuleType, root: Path, failures: list[str]) -> None:
    """Prove option termination and exact single-result app authority."""
    observed: list[list[str]] = []

    def valid(argv: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
        observed.append(argv)
        return subprocess.CompletedProcess(argv, 0, "ek_ra8d2::hw_validated::hil::blinky\n", "")

    result = args.canonical_app(root, "--help", valid)
    if result != "ek_ra8d2::hw_validated::hil::blinky" or observed[0][-2:] != [
        "--",
        "--help",
    ]:
        failures.append("canonical app did not bind option terminator")

    def multiline(argv: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
        return subprocess.CompletedProcess(argv, 0, "one\ntwo\n", "")

    _case(
        failures,
        "multi-line app",
        lambda: args.canonical_app(root, "x", multiline),
        must_fail=True,
    )
    _case(
        failures,
        "live canonical app",
        lambda: args.canonical_app(root, "board::stand_alone::ra8d2-ereader"),
        must_fail=False,
    )
    for selector in ("--help", "bad app", "bad;app", "bad\napp", "../app"):
        _case(
            failures,
            f"hostile app {selector!r}",
            lambda value=selector: args.canonical_app(root, value),
            must_fail=True,
        )


def _workspace(temp: Path) -> tuple[Path, Path, Path]:
    """Create isolated canonical-looking script and private runtime authorities."""
    root = (temp / "workspace").resolve()
    script = root / "scripts/dev/remote_gdb_server.sh"
    script.parent.mkdir(parents=True)
    script.write_text("#!/bin/bash -p\nexit 0\n", encoding="ascii")
    script.chmod(0o700)
    runtime = temp / "runtime"
    runtime.mkdir(mode=0o700)
    return root, script, runtime


def _request(guard: ModuleType, temp: Path, parent: int = 4242) -> tuple[object, Path]:
    """Construct one macOS-shaped request with a private injected runtime."""
    root, script, runtime = _workspace(temp)
    request = guard.BrokerRequest(
        root, script, "2331", "", parent, runtime_base=runtime, platform="darwin"
    )
    return request, _api(guard, "_state_dir")(request)


def _start_broker(
    guard: ModuleType, request: object, parent_ref: list[int], signals: list[int]
) -> tuple[threading.Thread, list[Exception], object]:
    """Start the production broker in a thread with non-signalling hooks."""
    errors: list[Exception] = []
    hooks = guard.BrokerHooks(
        getppid=lambda: parent_ref[0],
        signal_parent=signals.append,
        pid_alive=lambda pid: pid == os.getpid(),
    )

    def target() -> None:
        try:
            _api(guard, "_run_broker")(request, hooks)
        except Exception as exc:  # noqa: BLE001 - returned to the self-test thread.
            errors.append(RuntimeError(f"{exc}\n{traceback.format_exc()}"))

    thread = threading.Thread(target=target, daemon=True)
    thread.start()
    try:
        _api(guard, "_await_broker")(request, os.getpid(), WAIT_SECONDS)
    except Exception as exc:
        try:
            _api(guard, "_request_broker")(
                _api(guard, "_state_dir")(request), "status", request=request
            )
            detail = "status unexpectedly passed"
        except Exception as status_exc:  # noqa: BLE001 - diagnostic only.
            detail = f"{type(status_exc).__name__}: {status_exc}"
        message = f"broker did not start; thread errors: {errors!r}; status={detail}"
        raise RuntimeError(message) from exc
    return thread, errors, hooks


def _broker_cases(guard: ModuleType, failures: list[str]) -> dict[str, object]:
    """Exercise stop, release, parent death, live exclusion, and cleanup."""
    retained: dict[str, object] = {}
    with tempfile.TemporaryDirectory(prefix="ra8-gdb-broker-", dir="/tmp") as directory:
        request, state_dir = _request(guard, Path(directory))
        parent_ref = [request.parent_pid]
        signals: list[int] = []
        thread, errors, hooks = _start_broker(guard, request, parent_ref, signals)
        retained.update(_api(guard, "_read_record")(state_dir).value)
        _case(
            failures,
            "second live broker",
            lambda: _api(guard, "_run_broker")(request, hooks),
            must_fail=True,
        )
        _api(guard, "_control_broker")(request, "stop")
        thread.join(WAIT_SECONDS)
        if thread.is_alive() or errors or signals != [request.parent_pid]:
            failures.append(
                f"broker stop failed: alive={thread.is_alive()} "
                f"errors={errors!r} signals={signals!r}"
            )
        if (state_dir / guard.STATE_NAME).exists() or (state_dir / guard.SOCKET_NAME).exists():
            failures.append("broker stop left state or socket")

    with tempfile.TemporaryDirectory(prefix="ra8-gdb-parent-death-", dir="/tmp") as directory:
        request, state_dir = _request(guard, Path(directory))
        parent_ref = [request.parent_pid]
        signals = []
        thread, errors, _hooks = _start_broker(guard, request, parent_ref, signals)
        parent_ref[0] = 1
        thread.join(WAIT_SECONDS)
        _api(guard, "_control_broker")(request, "stop")
        if thread.is_alive() or errors or signals:
            failures.append(
                f"parent death retained broker: alive={thread.is_alive()} "
                f"errors={errors!r} signals={signals!r}"
            )
        if (state_dir / guard.STATE_NAME).exists() or (state_dir / guard.SOCKET_NAME).exists():
            failures.append("parent death did not atomically clean state")

    with tempfile.TemporaryDirectory(prefix="ra8-gdb-release-", dir="/tmp") as directory:
        request, _state_dir = _request(guard, Path(directory))
        parent_ref = [request.parent_pid]
        signals = []
        thread, errors, _hooks = _start_broker(guard, request, parent_ref, signals)
        _api(guard, "_control_broker")(request, "release")
        thread.join(WAIT_SECONDS)
        if thread.is_alive() or errors or signals:
            failures.append("release did not stop broker without signalling")
    return retained


def _protocol_cases(guard: ModuleType, failures: list[str]) -> None:
    """Reject malformed protocol, wrong nonce, trailing data, and absent credentials."""
    nonce = "a" * 64
    good = _api(guard, "_request_bytes")("status", nonce)
    if _api(guard, "_parse_request")(good, nonce) != "status":
        failures.append("valid protocol request did not round trip")
    attacks = (
        good + b"{}\n",
        good.replace(b'"version":1', b'"version":1,"version":1'),
        _api(guard, "_request_bytes")("status", "b" * 64),
        b"x" * (guard.MAX_REQUEST_BYTES + 1),
    )
    for index, raw in enumerate(attacks):
        _case(
            failures,
            f"protocol attack {index}",
            lambda raw=raw: _api(guard, "_parse_request")(raw, nonce),
            must_fail=True,
        )

    class Peer:
        def getpeereid(self) -> tuple[int, int]:
            return os.getuid(), os.getgid()

    if _api(guard, "_peer_uid")(Peer()) != os.getuid():
        failures.append("getpeereid peer UID was not accepted")
    credentials = {
        name: getattr(socket, name)
        for name in ("SO_PEERCRED", "LOCAL_PEERCRED")
        if hasattr(socket, name)
    }
    for name in credentials:
        delattr(socket, name)
    try:
        _case(
            failures,
            "missing peer credential primitive",
            lambda: _api(guard, "_peer_uid")(object()),
            must_fail=True,
        )
    finally:
        for name, credential in credentials.items():
            setattr(socket, name, credential)


def _record_fixture(
    guard: ModuleType, temp: Path, value: dict[str, object]
) -> tuple[object, Path, dict[str, object]]:
    """Copy a valid record into a new canonical namespace."""
    request, state_dir = _request(guard, temp)
    updated = dict(value)
    root_stat = request.root.stat()
    script_stat, script_digest = _api(guard, "_regular_identity")(request.script)
    helper_digest = _api(guard, "_helper_digest")()
    updated.update(
        platform="darwin",
        root=str(request.root),
        root_dev=root_stat.st_dev,
        root_ino=root_stat.st_ino,
        script=str(request.script),
        script_dev=script_stat.st_dev,
        script_ino=script_stat.st_ino,
        script_sha256=script_digest,
        helper_sha256=helper_digest,
        port="2331",
    )
    return request, state_dir, updated


def _stale_cases(guard: ModuleType, value: dict[str, object], failures: list[str]) -> None:
    """Clean only identity-matching dead state and preserve ambiguity."""
    with tempfile.TemporaryDirectory(prefix="ra8-gdb-stale-", dir="/tmp") as directory:
        request, state_dir, updated = _record_fixture(guard, Path(directory), value)
        old = _api(guard, "_publish_record")(state_dir, updated)
        hooks = guard.BrokerHooks(getppid=lambda: request.parent_pid, pid_alive=lambda _pid: False)
        _api(guard, "_prepare_state")(request, hooks)
        if (state_dir / guard.STATE_NAME).exists():
            failures.append("identity-matching stale state was not cleaned")
        current = _api(guard, "_publish_record")(state_dir, updated)
        _api(guard, "_unlink_exact")(state_dir, current)
        replaced = dict(updated)
        replaced["app_arg"] = "changed"
        _api(guard, "_publish_record")(state_dir, replaced)
        if _api(guard, "_unlink_exact")(state_dir, old):
            failures.append("stale cleanup removed a replaced record")

    with tempfile.TemporaryDirectory(prefix="ra8-gdb-socket-mismatch-", dir="/tmp") as directory:
        request, state_dir, updated = _record_fixture(guard, Path(directory), value)
        control = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        socket_path = state_dir / guard.SOCKET_NAME
        control.bind(str(socket_path))
        socket_path.chmod(0o600)
        updated["socket_ino"] = int(updated["socket_ino"]) + 1
        _api(guard, "_publish_record")(state_dir, updated)
        hooks = guard.BrokerHooks(getppid=lambda: request.parent_pid, pid_alive=lambda _pid: False)
        _case(
            failures,
            "ambiguous stale socket",
            lambda: _api(guard, "_prepare_state")(request, hooks),
            must_fail=True,
        )
        if not socket_path.exists() or not (state_dir / guard.STATE_NAME).exists():
            failures.append("ambiguous stale state was modified")
        control.close()


def _filesystem_cases(guard: ModuleType, failures: list[str]) -> None:
    """Reject links, permissive directories, special records, and duplicate JSON."""
    with tempfile.TemporaryDirectory(prefix="ra8-gdb-runtime-", dir="/tmp") as directory:
        temp = Path(directory)
        runtime = temp / "runtime"
        runtime.mkdir(mode=0o755)
        _case(
            failures,
            "permissive runtime",
            lambda: _api(guard, "_runtime_directory")(temp, base=runtime),
            must_fail=True,
        )
        runtime.chmod(0o700)
        victim = temp / "victim"
        victim.mkdir(mode=0o700)
        (runtime / "ra8-remote-gdb").symlink_to(victim, target_is_directory=True)
        _case(
            failures,
            "linked runtime namespace",
            lambda: _api(guard, "_runtime_directory")(temp, base=runtime),
            must_fail=True,
        )

    for kind in ("symlink", "fifo", "permissive", "duplicate"):
        with tempfile.TemporaryDirectory(prefix=f"ra8-gdb-{kind}-", dir="/tmp") as directory:
            _request_value, state_dir = _request(guard, Path(directory))
            state = state_dir / guard.STATE_NAME
            if kind == "symlink":
                victim = Path(directory) / "victim"
                victim.write_text("preserve\n", encoding="ascii")
                state.symlink_to(victim)
            elif kind == "fifo":
                os.mkfifo(state, 0o600)
            elif kind == "permissive":
                state.write_text("{}\n", encoding="ascii")
                state.chmod(0o644)
            else:
                state.write_text('{"version":1,"version":1}\n', encoding="ascii")
                state.chmod(0o600)
            _case(
                failures,
                f"unsafe record {kind}",
                lambda state_dir=state_dir: _api(guard, "_read_record")(state_dir),
                must_fail=True,
            )


def _platform_cases(guard: ModuleType, failures: list[str]) -> None:
    """Prove the macOS fallback is private and never a shared TMPDIR."""
    with tempfile.TemporaryDirectory(prefix="ra8-gdb-home-", dir="/tmp") as directory:
        home = Path(directory).resolve() / "home"
        home.mkdir(mode=0o700)
        selected = _api(guard, "_home_runtime")(os.getuid(), home)
        if home not in selected.parents or selected.stat().st_mode & 0o077:
            failures.append("macOS-shaped home fallback is not private")
        linked = Path(directory) / "linked-home"
        linked.symlink_to(home, target_is_directory=True)
        _case(
            failures,
            "linked home fallback",
            lambda: _api(guard, "_home_runtime")(os.getuid(), linked),
            must_fail=True,
        )
        if "TMPDIR" in str(selected):
            failures.append("runtime fallback used shared TMPDIR")


def _write_proc(proc: Path, pid: int, root: Path, script: Path, ticks: int) -> None:
    """Build a synthetic Linux procfs identity without creating a process."""
    process = proc / str(pid)
    (process / "fd").mkdir(parents=True, exist_ok=True)
    tail = ["S", *(["0"] * 18), str(ticks)]
    (process / "stat").write_text(f"{pid} (bash fixture) {' '.join(tail)}\n", encoding="ascii")
    (process / "status").write_text(
        f"Uid:\t{os.getuid()}\t{os.getuid()}\t{os.getuid()}\t{os.getuid()}\n",
        encoding="ascii",
    )
    argv = ["/bin/bash", "-p", "--", str(script), "run", "2331"]
    (process / "cmdline").write_bytes(b"\0".join(field.encode("ascii") for field in argv) + b"\0")
    links = {
        process / "exe": Path("/bin/bash").resolve(),
        process / "cwd": root,
        process / "fd/255": script,
    }
    for link, target in links.items():
        if not link.exists() and not link.is_symlink():
            link.symlink_to(target)


def _process_cases(guard: ModuleType, failures: list[str]) -> None:
    """Attack Linux start-time, argv, and open-script process bindings."""
    with tempfile.TemporaryDirectory(prefix="ra8-gdb-proc-", dir="/tmp") as directory:
        root, script, _runtime = _workspace(Path(directory))
        proc = Path(directory).resolve() / "proc"
        proc.mkdir()
        pid = 4242
        ticks = 998877
        _write_proc(proc, pid, root, script, ticks)
        request = guard.BrokerRequest(
            root, script, "2331", "", pid, proc_root=proc, platform="linux"
        )
        _case(
            failures,
            "valid Linux parent proof",
            lambda: _api(guard, "_process_proof")(pid, request),
            must_fail=False,
        )
        record = SimpleNamespace(value={"broker_pid": pid, "broker_start_ticks": ticks})
        stat_path = proc / str(pid) / "stat"
        stat_path.write_text(
            f"{pid} (bash fixture) {' '.join(['S', *(['0'] * 18), str(ticks + 1)])}\n",
            encoding="ascii",
        )
        if _api(guard, "_broker_live")(request, record):
            failures.append("PID start-time reuse passed live identity proof")
        _write_proc(proc, pid, root, script, ticks)
        cmdline = proc / str(pid) / "cmdline"
        cmdline.write_bytes(cmdline.read_bytes()[:-1] + b"forged\0")
        _case(
            failures,
            "forged Linux parent argv",
            lambda: _api(guard, "_process_proof")(pid, request),
            must_fail=True,
        )
        _write_proc(proc, pid, root, script, ticks)
        (proc / str(pid) / "fd/255").unlink()
        _case(
            failures,
            "missing Linux script descriptor",
            lambda: _api(guard, "_process_proof")(pid, request),
            must_fail=True,
        )


def _schema_cases(guard: ModuleType, value: dict[str, object], failures: list[str]) -> None:
    """Reject duplicate, extra, missing, Boolean, and wrong-type record fields."""
    mutations = (
        lambda item: item.update(version=True),
        lambda item: item.update(parent_pid=True),
        lambda item: item.update(parent_argv="not-a-list"),
        lambda item: item.update(extra="field"),
        lambda item: item.pop("nonce"),
    )
    for index, mutate in enumerate(mutations):
        with tempfile.TemporaryDirectory(prefix="ra8-gdb-schema-", dir="/tmp") as directory:
            _request_value, state_dir = _request(guard, Path(directory))
            changed = dict(value)
            mutate(changed)
            _case(
                failures,
                f"record schema mutation {index}",
                lambda changed=changed, state_dir=state_dir: _api(guard, "_publish_record")(
                    state_dir, changed
                ),
                must_fail=True,
            )


def _pidfd_order_case(guard: ModuleType, failures: list[str]) -> None:
    """Model the old reuse bug, then prove capability acquisition occurs first."""
    generation = [1]
    authenticated = generation[0]
    generation[0] = 2
    opened = generation[0]
    if authenticated == opened:
        failures.append("negative PID-reuse control did not model different identities")
    events: list[str] = []
    request = guard.BrokerRequest(Path("/x"), Path("/x/s"), "2331", "", 42, platform="linux")

    def open_capability(_request: object) -> tuple[Callable[[int], None], int]:
        events.append("open")
        return lambda _pid: None, os.open("/dev/null", os.O_RDONLY)

    def reject(_pid: int, _request: object) -> object:
        events.append("authenticate")
        message = "modeled PID reuse"
        raise guard.GuardError(message)

    def live(_fd: int) -> bool:
        return True

    with (
        mock.patch.object(guard, "_signal_authority", open_capability),
        mock.patch.object(guard, "_process_proof", reject),
        mock.patch.object(guard, "_pidfd_live", live),
    ):
        _case(
            failures,
            "pidfd-bound authentication reuse",
            lambda: _api(guard, "_parent_authority")(
                request, guard.BrokerHooks(getppid=lambda: 42)
            ),
            must_fail=True,
        )
    if events != ["open", "authenticate"]:
        failures.append(f"pidfd ordering was not open-before-authenticate: {events!r}")


def _shell_boundary(root: Path, failures: list[str]) -> None:
    """Drive sanitizer and cwd checks through the exact production wrapper."""
    script = root / "scripts/dev/remote_gdb_server.sh"
    with tempfile.TemporaryDirectory(prefix="ra8-gdb-bash-func-", dir="/tmp") as directory:
        marker = Path(directory) / "imported"
        environment = {
            "BASH_ENV": str(Path(directory) / "missing"),
            "BASH_FUNC_cd%%": f'() {{ /usr/bin/touch {marker}; builtin cd "$@"; }}',
            "BASH_FUNC_ra8_remote_gdb_probe%%": f"() {{ /usr/bin/touch {marker}; }}",
            "HOME": directory,
            "PATH": "/usr/bin:/bin",
        }
        control = _spawn_status(["/bin/bash", "-c", "ra8_remote_gdb_probe"], environment)
        if control != 0 or not marker.exists():
            failures.append("raw exported-function attack control was vacuous")
        marker.unlink(missing_ok=True)
        result = _spawn_status(["/bin/bash", "-p", str(script), "invalid-action"], environment)
        if result == 0 or marker.exists():
            failures.append("raw BASH_FUNC entry ran at privileged wrapper boundary")
        previous = os.open(".", os.O_RDONLY)
        try:
            os.chdir(directory)
            descendant = _spawn_status(
                ["/bin/bash", "-p", str(script), "--boundary-selftest"], environment
            )
        finally:
            os.fchdir(previous)
            os.close(previous)
        if descendant != 0 or marker.exists():
            failures.append("production wrapper leaked a function to an ordinary Bash descendant")


def _remote_program_cases(remote_helper: Path, root: Path, failures: list[str]) -> None:
    """Run the remote supervisor selftest and reject PID-sweep regressions."""
    remote = _load(remote_helper, "ra8_remote_gdb_remote")
    if remote.selftest() != 0:
        failures.append("remote direct-child supervisor selftest failed")
    source = (root / "scripts/dev/remote_gdb_server.sh").read_text(encoding="ascii")
    failures.extend(
        f"remote PID-sweep residue remains: {forbidden}"
        for forbidden in ("/proc/[0-9]*", "REMOTE_CLEANUP", "server_pids")
        if forbidden in source
    )


def selftest(helper: Path, args_helper: Path, remote_helper: Path, root: Path) -> int:
    """Run all offline both-direction tests without real signals or network."""
    guard = _load(helper, "ra8_remote_gdb_guard")
    args = _load(args_helper, "ra8_remote_gdb_args")
    failures: list[str] = []
    _host_cases(args, failures)
    _transport_cases(args, failures)
    _serialization_cases(args, failures)
    _remote_cli_cases(args, failures)
    _rig_contract_parity(args, root, failures)
    _app_cases(args, root, failures)
    record = _broker_cases(guard, failures)
    _protocol_cases(guard, failures)
    _stale_cases(guard, record, failures)
    _filesystem_cases(guard, failures)
    _platform_cases(guard, failures)
    _process_cases(guard, failures)
    _schema_cases(guard, record, failures)
    _pidfd_order_case(guard, failures)
    _shell_boundary(root, failures)
    _remote_program_cases(remote_helper, root, failures)
    if failures:
        for failure in failures:
            print(f"  [FAIL] {failure}", file=sys.stderr)
        return 1
    print("remote_gdb_guard_selftest.py: PASS (transport/state/broker/platform/PID-reuse)")
    return 0


def main() -> int:
    """Parse exact authorities and run the offline suite."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--helper", type=Path, required=True)
    parser.add_argument("--args-helper", type=Path, required=True)
    parser.add_argument("--remote-helper", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    options = parser.parse_args()
    return selftest(
        options.helper.resolve(strict=True),
        options.args_helper.resolve(strict=True),
        options.remote_helper.resolve(strict=True),
        options.root.resolve(strict=True),
    )


if __name__ == "__main__":
    sys.exit(main())
