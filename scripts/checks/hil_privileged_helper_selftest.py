#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Offline behavioral tests for the root-owned HIL helper."""

from __future__ import annotations

import argparse
import importlib.util
import os
import resource
import signal
import sys
import tempfile
import time
from collections.abc import Callable, Iterator
from contextlib import contextmanager
from pathlib import Path
from types import ModuleType

REPO_ROOT = Path(__file__).resolve().parents[2]
HELPER = REPO_ROOT / "infra/ansible/roles/dev_box/files/ra8-hil-privileged.py"
EXPECTED_NETWORK_MUTATIONS = 3


def _member(module: ModuleType, name: str) -> Callable[..., object]:
    """Return one deliberately private helper seam by its exact name."""
    return vars(module)[name]


def _load_helper() -> ModuleType:
    """Load the checkout helper without invoking its CLI."""
    spec = importlib.util.spec_from_file_location("ra8_hil_privileged", HELPER)
    if spec is None or spec.loader is None:
        message = "cannot load HIL privileged helper"
        raise RuntimeError(message)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def _policy(module: ModuleType) -> dict[str, object]:
    """Return one internally authenticated physical-interface policy."""
    policy: dict[str, object] = {
        "board_iface": "eth0",
        "mac": "02:00:00:00:00:09",
        "phc_index": 0,
        "sysfs_device": "/sys/devices/platform/bench-ethernet",
        "version": 1,
    }
    policy["declaration_sha256"] = module.hashlib.sha256(
        _member(module, "_canonical_policy")(policy)
    ).hexdigest()
    return policy


def _rejects(module: ModuleType, callable_obj: object, *args: object, **kwargs: object) -> bool:
    """Return whether one pure request is rejected by policy."""
    try:
        callable_obj(*args, **kwargs)
    except module.PolicyError:
        return True
    return False


@contextmanager
def _patched(module: ModuleType, **replacements: object) -> Iterator[None]:
    """Temporarily replace helper boundaries for an offline fake backend."""
    originals = {name: getattr(module, name) for name in replacements}
    try:
        for name, value in replacements.items():
            setattr(module, name, value)
        yield
    finally:
        for name, value in originals.items():
            setattr(module, name, value)


class _FakeCycleOps:
    """In-memory journal/device backend with one injectable failure point."""

    def __init__(self, module: ModuleType, fail_at: str = "") -> None:
        self.module = module
        self.fail_at = fail_at
        self.events: list[str] = []
        self.pending = False

    def _event(self, value: str) -> None:
        self.events.append(value)
        if self.fail_at == value:
            message = f"injected {value}"
            raise self.module.PolicyError(message)

    def save(self, _path: Path, _state: dict[str, object]) -> None:
        self.pending = True
        self._event("save")

    def apply(self, action: object) -> None:
        value = "off" if action.args[-1] in {"off", "0"} else "on"
        self._event(value)

    def pause(self) -> None:
        self._event("pause")

    def clear(self, _path: Path) -> None:
        self._event("clear")
        self.pending = False


class _ProcessCycleOps:
    """File-backed backend used by one real signalled child process."""

    def __init__(
        self,
        events: Path,
        journal: Path,
        state: Path,
        fail_at: str,
        inflight_off: bool,
    ) -> None:
        self.events = events
        self.journal = journal
        self.state = state
        self.fail_at = fail_at
        self.inflight_off = inflight_off

    def _event(self, value: str) -> None:
        with self.events.open("a", encoding="ascii") as stream:
            stream.write(f"{value}\n")
            stream.flush()
            os.fsync(stream.fileno())
        if self.fail_at == value:
            message = f"injected {value}"
            raise RuntimeError(message)

    def save(self, _path: Path, _state: dict[str, object]) -> None:
        self.journal.write_text("pending\n", encoding="ascii")
        self._event("save")

    def apply(self, action: object) -> None:
        value = "off" if action.args[-1] in {"off", "0"} else "on"
        if value == "off" and self.inflight_off:
            self._delayed_off()
            return
        if self.fail_at == value:
            self._event(value)
        self.state.write_text(value, encoding="ascii")
        self._event(value)

    def _delayed_off(self) -> None:
        """Model a spawned off child that must not outlive restoration."""
        pid = os.fork()
        if pid == 0:
            time.sleep(0.2)
            self.state.write_text("off", encoding="ascii")
            self._event("off-child-completed")
            os._exit(0)
        self._event("off-child-spawned")
        _, status = os.waitpid(pid, 0)
        if os.waitstatus_to_exitcode(status) != 0:
            message = "in-flight off child failed"
            raise RuntimeError(message)

    def pause(self) -> None:
        self._event("pause")
        time.sleep(0.3)

    def clear(self, _path: Path) -> None:
        self._event("clear")
        self.journal.unlink()


def _request_checks(module: ModuleType) -> list[tuple[str, bool]]:
    """Return exact topology and argument-injection tests."""
    power_command = _member(module, "_usb_power_command")
    validate_action = _member(module, "_validate_action")
    validate_board_ip = _member(module, "_validate_board_ip")
    validate_port = _member(module, "_validate_port")
    expected = ["/usr/sbin/uhubctl", "-S", "-l", "2-1.3", "-p", "1", "-a", "off"]
    checks = [
        (
            "nominal port argv is exact",
            power_command("usb-port-power", ["1", "off"]) == expected,
        ),
        (
            "root power topology is fixed",
            power_command("usb-root-power", ["on"])[3] == "2-1",
        ),
        (
            "nominal board address accepted",
            validate_board_ip("192.168.1.42") == "192.168.1.42",
        ),
    ]
    checks.extend(
        (f"port {value!r} rejected", _rejects(module, validate_port, value))
        for value in ("1;id", "1 2", "../1", "-1", "3")
    )
    checks.extend(
        (f"action {value!r} rejected", _rejects(module, validate_action, value))
        for value in ("off;id", "on\n", "--help", "cycle")
    )
    checks.extend(
        (f"address {value!r} rejected", _rejects(module, validate_board_ip, value))
        for value in (
            "10.0.40.2",
            "192.168.1.1",
            "192.168.1.0",
            "192.168.1.255",
            "192.168.1.2;id",
        )
    )
    return checks


def _interface_checks(module: ModuleType) -> list[tuple[str, bool]]:
    """Prove only the fleet-declared permanent physical identity is accepted."""
    policy = _policy(module)
    validate = _member(module, "_validate_iface_facts")
    strict_policy = _member(module, "_strict_policy")
    facts_type = _member(module, "_LiveIfaceFacts")

    def facts(**changes: object) -> object:
        values = {
            "exists": True,
            "uplinks": {"wlan0"},
            "ipv4_addresses": set(),
            "mac": policy["mac"],
            "sysfs_device": policy["sysfs_device"],
            "phc_index": policy["phc_index"],
            **changes,
        }
        return facts_type(**values)

    return [
        *_interface_address_checks(module, policy, validate, facts),
        (
            "second valid-shaped interface cannot be selected",
            _rejects(module, strict_policy, {**policy, "board_iface": "eth1"}),
        ),
        (
            "dotted virtual interface cannot be selected",
            _rejects(module, strict_policy, {**policy, "board_iface": "eth0.42"}),
        ),
        (
            "permanent MAC drift rejected",
            _rejects(module, validate, policy, facts(mac="02:00:00:00:00:10")),
        ),
        (
            "canonical sysfs identity drift rejected",
            _rejects(
                module,
                validate,
                policy,
                facts(sysfs_device="/sys/devices/virtual/net/eth0"),
            ),
        ),
        (
            "PHC identity drift rejected",
            _rejects(module, validate, policy, facts(phc_index=1)),
        ),
    ]


def _interface_address_checks(
    module: ModuleType,
    policy: dict[str, object],
    validate: Callable[..., object],
    facts: Callable[..., object],
) -> list[tuple[str, bool]]:
    """Prove uplink and foreign-address facts fail closed."""
    return [
        ("exact permanent interface accepted", validate(policy, facts()) == "eth0"),
        (
            "exact helper-owned address is allowed during recovery",
            validate(
                policy,
                facts(ipv4_addresses={"192.168.1.1"}),
                {"192.168.1.1"},
            )
            == "eth0",
        ),
        (
            "foreign address beside helper ownership is rejected",
            _rejects(
                module,
                validate,
                policy,
                facts(ipv4_addresses={"192.168.1.1", "192.168.1.99"}),
                {"192.168.1.1"},
            ),
        ),
        (
            "IPv4 default uplink rejected",
            _rejects(module, validate, policy, facts(uplinks={"eth0"})),
        ),
        (
            "IPv6 default uplink rejected by same all-table census",
            _rejects(module, validate, policy, facts(uplinks={"eth0"})),
        ),
    ]


def _cycle_case(module: ModuleType, fail_at: str) -> tuple[list[str], bool, bool]:
    """Run one rootless cycle with an injected boundary failure."""
    state, off = _member(module, "_cycle_request")("usb-port-cycle", ["1"])
    ops = _FakeCycleOps(module, fail_at)
    failed = _rejects(module, _member(module, "_perform_cycle"), Path("unused"), state, off, ops)
    return ops.events, ops.pending, failed


def _interrupt_cycle_case(
    module: ModuleType, interrupt_type: type[BaseException]
) -> tuple[list[str], bool, bool]:
    """Interrupt a cycle delay and record restoration before propagation."""
    state, off = _member(module, "_cycle_request")("usb-port-cycle", ["1"])
    ops = _FakeCycleOps(module)

    def interrupt() -> None:
        ops.events.append("pause")
        raise interrupt_type

    ops.pause = interrupt
    propagated = False
    try:
        _member(module, "_perform_cycle")(Path("unused"), state, off, ops)
    except interrupt_type:
        propagated = True
    return ops.events, ops.pending, propagated


def _cycle_checks(module: ModuleType) -> list[tuple[str, bool]]:
    """Prove restoration on off/delay faults and journaling on restore faults."""
    nominal = _cycle_case(module, "")
    off = _cycle_case(module, "off")
    pause = _cycle_case(module, "pause")
    on = _cycle_case(module, "on")
    clear = _cycle_case(module, "clear")
    keyboard_interrupted = _interrupt_cycle_case(module, KeyboardInterrupt)
    system_exit_interrupted = _interrupt_cycle_case(module, SystemExit)
    checks = [
        (
            "nominal cycle journals, restores, and clears",
            nominal == (["save", "off", "pause", "on", "clear"], False, False),
        ),
        (
            "off failure still restores before reporting",
            off == (["save", "off", "on", "clear"], False, True),
        ),
        (
            "delay failure still restores before reporting",
            pause == (["save", "off", "pause", "on", "clear"], False, True),
        ),
        (
            "on failure leaves pending restore journal",
            on == (["save", "off", "pause", "on"], True, True),
        ),
        (
            "clear failure leaves conservative restore journal",
            clear == (["save", "off", "pause", "on", "clear"], True, True),
        ),
        (
            "KeyboardInterrupt restores on before propagating",
            keyboard_interrupted == (["save", "off", "pause", "on", "clear"], False, True),
        ),
        (
            "SystemExit restores on before propagating",
            system_exit_interrupted == (["save", "off", "pause", "on", "clear"], False, True),
        ),
        (
            "persistent off remains explicit and separate",
            _rejects(
                module,
                _member(module, "_cycle_request"),
                "usb-port-cycle",
                ["1", "off"],
            ),
        ),
    ]
    checks.append(("interrupted cycle is restored before a later mutation", _recovers(module)))
    return checks


def _recovers(module: ModuleType) -> bool:
    """Return whether a later mutation restores one persisted cycle journal."""
    recovery_ops = _FakeCycleOps(module)
    recovery_state = {
        "kind": "port-power",
        "port": "1",
        "restore": "on",
        "version": 1,
    }
    with _patched(module, _load_restore=lambda _path: recovery_state):
        _member(module, "_recover_restore")(Path("unused"), recovery_ops)
    return recovery_ops.events == ["on", "clear"] and not recovery_ops.pending


def _signal_child(
    events: Path,
    journal: Path,
    device_state: Path,
    fail_at: str,
    inflight_off: bool,
) -> int:
    """Run the exact cycle boundary inside a signallable child process."""
    module = _load_helper()
    state, off = _member(module, "_cycle_request")("usb-port-cycle", ["1"])
    try:
        _member(module, "_perform_cycle")(
            journal,
            state,
            off,
            _ProcessCycleOps(events, journal, device_state, fail_at, inflight_off),
        )
    except RuntimeError:
        return 7
    except KeyboardInterrupt:
        return 8 if signal.getsignal(signal.SIGINT) is signal.default_int_handler else 9
    return 0


def _signal_process_case(
    signum: int,
    fail_at: str = "",
    *,
    wait_for: str = "pause",
    inflight_off: bool = False,
) -> tuple[list[str], int, bool, str]:
    """Send a real terminating signal after a child records transient off."""
    with tempfile.TemporaryDirectory(prefix="ra8-hil-signal-") as raw:
        root = Path(raw)
        events = root / "events"
        journal = root / "journal"
        device_state = root / "device-state"
        pid = os.fork()
        if pid == 0:
            signal.signal(signal.SIGHUP, signal.SIG_DFL)
            signal.signal(signal.SIGINT, signal.default_int_handler)
            signal.signal(signal.SIGQUIT, signal.SIG_DFL)
            signal.signal(signal.SIGTERM, signal.SIG_DFL)
            resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
            os._exit(_signal_child(events, journal, device_state, fail_at, inflight_off))
        deadline = time.monotonic() + 3
        returncode: int | None = None
        observed: list[str] = []
        while time.monotonic() < deadline:
            observed = events.read_text(encoding="ascii").splitlines() if events.exists() else []
            waited, status = os.waitpid(pid, os.WNOHANG)
            if waited:
                returncode = os.waitstatus_to_exitcode(status)
                break
            if wait_for in observed:
                break
            time.sleep(0.01)
        if returncode is None and wait_for in observed:
            os.kill(pid, signum)
        if returncode is None:
            returncode = _wait_child(pid)
        observed = events.read_text(encoding="ascii").splitlines() if events.exists() else []
        final_state = device_state.read_text(encoding="ascii") if device_state.exists() else ""
        return observed, returncode, journal.exists(), final_state


def _wait_child(pid: int) -> int:
    """Return one child's status, killing only a wedged selftest child."""
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        waited, status = os.waitpid(pid, os.WNOHANG)
        if waited:
            return os.waitstatus_to_exitcode(status)
        time.sleep(0.01)
    os.kill(pid, signal.SIGKILL)
    _, status = os.waitpid(pid, 0)
    return os.waitstatus_to_exitcode(status)


def _signal_cycle_checks() -> list[tuple[str, bool]]:
    """Prove real signal deferral and conservative failure journaling."""
    nominal = ["save", "off", "pause", "on", "clear"]
    term = _signal_process_case(signal.SIGTERM)
    hup = _signal_process_case(signal.SIGHUP)
    inflight = ["save", "off-child-spawned", "off-child-completed", "pause", "on", "clear"]
    interrupt = _signal_process_case(
        signal.SIGINT,
        wait_for="off-child-spawned",
        inflight_off=True,
    )
    quit_signal = _signal_process_case(
        signal.SIGQUIT,
        wait_for="off-child-spawned",
        inflight_off=True,
    )
    restore_failed = _signal_process_case(signal.SIGTERM, "on")
    clear_failed = _signal_process_case(signal.SIGHUP, "clear")
    return [
        (
            "SIGTERM restores then retains signal exit",
            term == (nominal, -signal.SIGTERM, False, "on"),
        ),
        (
            "SIGHUP restores then retains signal exit",
            hup == (nominal, -signal.SIGHUP, False, "on"),
        ),
        (
            "in-flight off completes before SIGINT propagation and final on",
            interrupt == (inflight, 8, False, "on"),
        ),
        (
            "in-flight off completes before SIGQUIT exit and final on",
            quit_signal == (inflight, -signal.SIGQUIT, False, "on"),
        ),
        (
            "restore failure surfaces before SIGTERM redelivery and keeps journal",
            restore_failed == (["save", "off", "pause", "on"], 7, True, "off"),
        ),
        (
            "clear failure surfaces before SIGHUP redelivery and keeps journal",
            clear_failed == ([*nominal], 7, True, "on"),
        ),
    ]


def _network_success(
    module: ModuleType, fail_save: int = 0, fail_run: int = 0
) -> tuple[list[str], bool]:
    """Run prepare against injected state and ip boundaries."""
    phases: list[str] = []
    counters = {"save": 0, "run": 0, "route": 0}

    def save(_path: Path, state: dict[str, object], _policy: dict[str, object]) -> None:
        counters["save"] += 1
        phases.append(f"{state['address_phase']}/{state['link_phase']}/{state['route_phase']}")
        if counters["save"] == fail_save:
            message = "injected save"
            raise module.PolicyError(message)

    def run(_argv: list[str], *, check: bool = True) -> object:
        _ = check
        counters["run"] += 1
        if counters["run"] == fail_run:
            message = "injected ip"
            raise module.PolicyError(message)
        return _member(module, "_CommandResult")(0, "", "")

    def route(_board: str, _iface: str) -> bool:
        counters["route"] += 1
        return counters["route"] > 1

    replacements = {
        "_network_cleanup": lambda _path, _policy: None,
        "_validate_live_iface": lambda _policy: ("eth0", False),
        "_route_present": route,
        "_address_present": lambda _iface: counters["run"] >= 1,
        "_link_is_up": lambda _iface: counters["run"] >= EXPECTED_NETWORK_MUTATIONS - 1,
        "_save_state": save,
        "_run": run,
    }
    with _patched(module, **replacements):
        failed = _rejects(
            module,
            _member(module, "_network_prepare"),
            Path("unused"),
            _policy(module),
            "192.168.1.42",
        )
    return phases, failed


def _network_checks(module: ModuleType) -> list[tuple[str, bool]]:
    """Prove pending/applied ordering across every save and mutation boundary."""
    phases, failed = _network_success(module)
    expected = [
        "absent/absent/absent",
        "pending/absent/absent",
        "applied/absent/absent",
        "applied/pending/absent",
        "applied/applied/absent",
        "applied/applied/pending",
        "applied/applied/applied",
    ]
    checks = [
        (
            "network success checkpoints pending then applied",
            phases == expected and not failed,
        )
    ]
    for boundary in range(1, len(expected) + 1):
        saved, did_fail = _network_success(module, fail_save=boundary)
        checks.append((f"save fault {boundary} fails closed", did_fail and len(saved) == boundary))
    for boundary in range(1, 4):
        saved, did_fail = _network_success(module, fail_run=boundary)
        checks.append((f"ip mutation fault {boundary} leaves journal", did_fail and bool(saved)))
    checks.extend(_network_cleanup_checks(module))
    checks.extend(_network_identity_checks(module))
    return checks


def _network_cleanup_checks(module: ModuleType) -> list[tuple[str, bool]]:
    """Prove ambiguous cleanup stops and partial cleanup is checkpointed."""
    policy = _policy(module)
    pending = _member(module, "_new_state")("eth0", "192.168.1.42")
    pending["route_phase"] = "pending"
    touched: list[str] = []
    with _patched(
        module,
        _load_state=lambda _path, _policy: pending,
        _cleanup_route=lambda *_args: touched.append("route"),
    ):
        pending_rejected = _rejects(
            module, _member(module, "_network_cleanup"), Path("unused"), policy
        )
    applied = {**pending, "route_phase": "applied", "link_phase": "applied"}
    snapshots: list[dict[str, object]] = []
    runs = 0

    def run(_argv: list[str], *, check: bool = True) -> object:
        nonlocal runs
        _ = check
        runs += 1
        if runs == EXPECTED_NETWORK_MUTATIONS - 1:
            message = "injected later cleanup failure"
            raise module.PolicyError(message)
        return _member(module, "_CommandResult")(0, "", "")

    routes = iter((True, False))
    replacements = {
        "_load_state": lambda _path, _policy: applied,
        "_authenticate_cleanup_iface": lambda _policy, _state: ("eth0", True),
        "_route_present": lambda _board, _iface: next(routes),
        "_link_is_up": lambda _iface: True,
        "_address_present": lambda _iface: False,
        "_save_state": lambda _path, state, _policy: snapshots.append(dict(state)),
        "_run": run,
    }
    with _patched(module, **replacements):
        later_failed = _rejects(module, _member(module, "_network_cleanup"), Path("unused"), policy)
    return [
        (
            "pending cleanup requires trusted recovery before mutation",
            pending_rejected and not touched,
        ),
        (
            "completed cleanup leg is checkpointed before a later fault",
            later_failed and snapshots and snapshots[0]["route_phase"] == "absent",
        ),
    ]


def _network_identity_checks(module: ModuleType) -> list[tuple[str, bool]]:
    """Prove stale cleanup paths authenticate before any root mutation."""
    policy = _policy(module)
    state = _member(module, "_new_state")("eth0", "192.168.1.42")
    state.update(address_phase="applied", link_phase="applied", route_phase="applied")
    touched: list[str] = []

    def reject_auth(_policy: object, _state: object) -> None:
        message = "injected permanent identity drift"
        raise module.PolicyError(message)

    common = {
        "_load_state": lambda _path, _policy: state,
        "_authenticate_cleanup_iface": reject_auth,
        "_run": lambda *_args, **_kwargs: touched.append("run"),
    }
    with _patched(module, **common, _cleanup_route=lambda *_args: touched.append("route")):
        cleanup_rejected = _rejects(
            module, _member(module, "_network_cleanup"), Path("unused"), policy
        )
    with _patched(module, **common):
        neigh_rejected = _rejects(
            module, _member(module, "_network_neigh_flush"), Path("unused"), policy
        )
    validated: list[str] = []

    def reject_cleanup(_path: Path, _policy: object) -> None:
        message = "stale state identity drift"
        raise module.PolicyError(message)

    with _patched(
        module,
        _network_cleanup=reject_cleanup,
        _validate_live_iface=lambda _policy: validated.append("validated"),
    ):
        prepare_rejected = _rejects(
            module,
            _member(module, "_network_prepare"),
            Path("unused"),
            policy,
            "192.168.1.42",
        )
    return [
        ("cleanup rejects physical drift before mutation", cleanup_rejected and not touched),
        ("neighbour flush rejects physical drift before mutation", neigh_rejected and not touched),
        ("prepare cannot bypass stale-state authentication", prepare_rejected and not validated),
    ]


def _state_and_sysfs_checks(module: ModuleType) -> list[tuple[str, bool]]:
    """Return policy/state parser and USB symlink escape tests."""
    policy = _policy(module)
    new_state = _member(module, "_new_state")
    strict_policy = _member(module, "_strict_policy")
    strict_state = _member(module, "_strict_state")
    resolve_usb_device = _member(module, "_resolve_usb_device")
    valid = new_state("eth0", "192.168.1.42")
    checks = [
        ("valid policy accepted", strict_policy(policy) == policy),
        (
            "stale policy digest rejected",
            _rejects(module, strict_policy, {**policy, "declaration_sha256": "0" * 64}),
        ),
        ("valid phased state accepted", strict_state(valid, policy) == valid),
        (
            "missing state field rejected",
            _rejects(module, strict_state, {"version": 1}, policy),
        ),
        (
            "pending state is recognized",
            strict_state({**valid, "route_phase": "pending"}, policy)["route_phase"] == "pending",
        ),
    ]
    with tempfile.TemporaryDirectory(prefix="ra8-hil-sysfs-") as raw:
        root = Path(raw)
        devices = root / "sys/devices"
        bus = root / "sys/bus/usb/devices"
        valid_device = devices / "platform/usb2/2-1/2-1.3/2-1.3.1"
        escaped = root / "outside/2-1.3.2"
        valid_device.mkdir(parents=True)
        escaped.mkdir(parents=True)
        bus.mkdir(parents=True)
        (bus / "2-1.3.1").symlink_to(valid_device)
        (bus / "2-1.3.2").symlink_to(escaped)
        checks.append(
            (
                "kernel-style USB symlink accepted",
                resolve_usb_device(bus, devices, "2-1.3.1") == valid_device.resolve(),
            )
        )
        checks.append(
            (
                "USB parent escape rejected",
                _rejects(module, resolve_usb_device, bus, devices, "2-1.3.2"),
            )
        )
    return checks


def main(argv: list[str] | None = None) -> int:
    """Run every offline helper policy and fault-injection check."""
    args_list = sys.argv[1:] if argv is None else argv
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(args_list)
    if not args.selftest:
        parser.error("--selftest is required")
    module = _load_helper()
    checks = (
        _request_checks(module)
        + _interface_checks(module)
        + _cycle_checks(module)
        + _signal_cycle_checks()
        + _network_checks(module)
        + _state_and_sysfs_checks(module)
    )
    for label, passed in checks:
        print(f"  [{'PASS' if passed else 'FAIL'}] {label}")
    ok = all(passed for _, passed in checks)
    print(f"hil_privileged_helper_selftest.py: {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
