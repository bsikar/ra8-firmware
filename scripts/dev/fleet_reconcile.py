#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Continuously converge ordinary CI runner hosts from one trusted snapshot."""

from __future__ import annotations

import argparse
import json
import os
import re
import signal
import stat
import sys
import tempfile
import time
from collections.abc import Callable, Sequence
from dataclasses import dataclass
from pathlib import Path
from threading import Thread
from typing import Any, TextIO

sys.path.insert(0, str(Path(__file__).resolve().parent))

import fleet_model as fm
import fleet_mutation_lock as fml
import fleet_reconcile_arc_selftest as fras
import fleet_reconcile_process as frp
import fleet_reconcile_selftest as frs
import fleet_wsl as fw

SOURCE_DIGEST_FILE = ".ra8-source-sha256"
STATE_FILE = "state.json"
DEFAULT_FULL_INTERVAL = 7 * 24 * 60 * 60
DEFAULT_PRODUCER_INTERVAL = 24 * 60 * 60
PRIVATE_DIRECTORY_MODE = 0o700
SELFTEST_CHANGED_TOTAL = 3
SELFTEST_SIGNAL_BOUND = 8
# ci_runner check mode empties and restages its build context: exactly these
# two tasks report changed on an otherwise-converged producer.
PRODUCER_CHECK_NOISE = 2
# While ARC admission is deliberately held at zero, its post-renderer removes
# the scale-set difference and only the context-restage check noise remains.
PRODUCER_HELD_CHECK_NOISE = 1
SHA256_RE = re.compile(r"[0-9a-f]{64}")
ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")


RECAP_RE = re.compile(
    r"^\s*([A-Za-z0-9_.-]+)\s+:\s+ok=(\d+)\s+changed=(\d+)\s+"
    r"unreachable=(\d+)\s+failed=(\d+)\s+skipped=(\d+)\s+"
    r"rescued=(\d+)\s+ignored=(\d+)\s*$"
)


@dataclass(frozen=True)
class ReconcileOptions:
    """Runtime policy for one fleet reconciliation."""

    mode: str
    force: bool
    source_digest: str
    state_dir: Path
    full_interval: int
    producer_interval: int
    now: int


CommandRunner = Callable[[Sequence[str]], frp.CommandResult]


def recap_identity(data: dict[str, Any], host: str) -> str:
    """Return Ansible's recap name without changing the fleet control identity."""
    transport = fm.CLASSES[data["hosts"][host]["class"]].transport
    return "localhost" if transport == "wsl" else host


def runner_hosts(data: dict[str, Any]) -> list[str]:
    """Return capacity-managed hosts in producer-before-consumer order."""
    names = [name for name, host in data["hosts"].items() if host.get("runners")]
    producer = str(data["runner_image"]["source_host"])
    if producer not in names:
        msg = "runner_image.source_host is not a capacity-managed host"
        raise ValueError(msg)
    return [producer, *[name for name in names if name != producer]]


def parse_changed(output: str, host: str, expected_plays: int) -> int:
    """Return changed tasks from exact successful Ansible recap rows."""
    rows: list[tuple[int, int, int]] = []
    for raw in output.splitlines():
        match = RECAP_RE.fullmatch(ANSI_RE.sub("", raw))
        if match is None or match.group(1) != host:
            continue
        changed = int(match.group(3))
        unreachable = int(match.group(4))
        failed = int(match.group(5))
        rows.append((changed, unreachable, failed))
    if len(rows) != expected_plays:
        msg = f"{host}: expected {expected_plays} Ansible recap row(s), found {len(rows)}"
        raise ValueError(msg)
    if any(unreachable or failed for _, unreachable, failed in rows):
        msg = f"{host}: Ansible recap reported a failed or unreachable play"
        raise ValueError(msg)
    return sum(changed for changed, _, _ in rows)


def fleet_command(host: str, verb: str) -> list[str]:
    """Build one command against the fleet entry point in this snapshot."""
    if verb == "check":
        arguments = ["check", host]
    elif verb == "parked-check":
        arguments = ["reconcile-parked-check", host]
    elif verb == "activate":
        arguments = ["reconcile-activate", host]
    elif verb == "activation-check":
        arguments = ["reconcile-activation-check", host]
    elif verb == "parked-apply":
        arguments = ["reconcile-parked-apply", host]
    elif verb == "quarantine":
        arguments = ["capacity-quarantine", host]
    elif verb == "restore":
        arguments = ["capacity-restore", host]
    else:
        msg = f"unsupported fleet reconcile verb: {verb}"
        raise ValueError(msg)
    return [sys.executable, str(fm.REPO_ROOT / "scripts/dev/fleet.py"), *arguments]


def emit_result(result: frp.CommandResult, stream: TextIO = sys.stdout) -> None:
    """Emit captured evidence without losing stderr attribution."""
    stream.write(result.stdout)
    sys.stderr.write(result.stderr)


def load_state(path: Path) -> dict[str, Any]:
    """Read prior receipts, refusing malformed state."""
    if not path.exists():
        return {"version": 1, "hosts": {}}
    if path.is_symlink() or not path.is_file():
        msg = f"state path is not a regular file: {path}"
        raise ValueError(msg)
    document = json.loads(path.read_text(encoding="ascii"))
    if not isinstance(document, dict) or document.get("version") != 1:
        msg = "fleet reconciliation state has an unsupported schema"
        raise ValueError(msg)
    hosts = document.get("hosts")
    if not isinstance(hosts, dict):
        msg = "fleet reconciliation state has no host receipt map"
        raise TypeError(msg)
    return document


def save_state(path: Path, document: dict[str, Any]) -> None:
    """Atomically publish reconciliation receipts."""
    encoded = (json.dumps(document, indent=2, sort_keys=True) + "\n").encode("ascii")
    fd, raw = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    temporary = Path(raw)
    try:
        os.fchmod(fd, 0o600)
        with os.fdopen(fd, "wb") as stream:
            stream.write(encoded)
            stream.flush()
            os.fsync(stream.fileno())
        temporary.replace(path)
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        temporary.unlink(missing_ok=True)


def full_apply_due(receipt: object, options: ReconcileOptions, interval: int) -> bool:
    """Decide whether periodic full convergence is due for one host."""
    if options.force or not isinstance(receipt, dict):
        return True
    if receipt.get("source_digest") != options.source_digest:
        return True
    applied = receipt.get("full_applied_at")
    return not isinstance(applied, int) or options.now - applied >= interval


def inspect_host(
    data: dict[str, Any], host: str, run: CommandRunner, *, parked: bool = False
) -> tuple[bool, int]:
    """Run a read-only host check and return success plus drift count."""
    verb = "parked-check" if parked else "check"
    result = run(fleet_command(host, verb))
    emit_result(result)
    if result.status == fw.APPLY_REQUIRED_STATUS:
        return True, 1
    if result.status:
        return False, 0
    try:
        changed = parse_changed(
            result.stdout, recap_identity(data, host), len(data["hosts"][host]["provisions"])
        )
    except ValueError as error:
        print(f"fleet-reconcile: {error}", file=sys.stderr)
        return False, 0
    return True, changed


def inspect_activation_host(
    data: dict[str, Any], host: str, run: CommandRunner
) -> tuple[bool, int]:
    """Check declared ARC authority while its rendered live ceiling stays zero."""
    result = run(fleet_command(host, "activation-check"))
    emit_result(result)
    if result.status:
        return False, 0
    try:
        changed = parse_changed(
            result.stdout,
            recap_identity(data, host),
            len(data["hosts"][host]["provisions"]),
        )
    except ValueError as error:
        print(f"fleet-reconcile: {error}", file=sys.stderr)
        return False, 0
    return True, changed


def quarantine(host: str, run: CommandRunner) -> None:
    """Drain a host after failed mutation so it cannot accept new work."""
    print(f"fleet-reconcile: quarantining {host} at zero capacity", file=sys.stderr)
    result = run(fleet_command(host, "quarantine"))
    emit_result(result)
    if result.status:
        print(
            f"fleet-reconcile: WARNING: could not quarantine {host} (rc={result.status})",
            file=sys.stderr,
        )


def _activate_arc(
    data: dict[str, Any], host: str, run: CommandRunner, expected_changes: int
) -> tuple[bool, int]:
    """Validate declared ARC authority at zero before the sole capacity opener."""
    activation = run(fleet_command(host, "activate"))
    emit_result(activation)
    if activation.status or frp.interrupted_status():
        quarantine(host, run)
        return False, 0
    clean, changed = inspect_activation_host(data, host, run)
    if not clean or frp.interrupted_status() or changed != expected_changes:
        quarantine(host, run)
        return False, changed
    restore = run(fleet_command(host, "restore"))
    emit_result(restore)
    if restore.status or frp.interrupted_status():
        quarantine(host, run)
        return False, 0
    return True, 0


def apply_host(
    data: dict[str, Any], host: str, run: CommandRunner, *, expected_check_changes: int
) -> tuple[bool, int]:
    """Apply one host and prove the resulting declaration is idempotent."""
    result = run(fleet_command(host, "parked-apply"))
    emit_result(result)
    if result.status or frp.interrupted_status():
        quarantine(host, run)
        return False, 0
    clean, changed = inspect_host(data, host, run, parked=True)
    if not clean or changed != expected_check_changes or frp.interrupted_status():
        print(
            f"fleet-reconcile: {host} did not reach an idempotent parked state "
            f"(remaining changed={changed})",
            file=sys.stderr,
        )
        quarantine(host, run)
        return False, changed
    host_class = fm.CLASSES[data["hosts"][host]["class"]]
    if host_class.capacity_kind == "k8s":
        return _activate_arc(data, host, run, expected_check_changes)
    restore = run(fleet_command(host, "restore"))
    emit_result(restore)
    if restore.status or frp.interrupted_status():
        quarantine(host, run)
        return False, 0
    clean, changed = inspect_host(data, host, run)
    if not clean or changed != expected_check_changes or frp.interrupted_status():
        quarantine(host, run)
        return False, changed
    return True, 0


def reconcile_host(
    data: dict[str, Any],
    host: str,
    receipt: object,
    options: ReconcileOptions,
    run: CommandRunner,
) -> tuple[bool, dict[str, Any]]:
    """Inspect and optionally converge one normal runner host."""
    clean, changed = inspect_host(data, host, run)
    if not clean or frp.interrupted_status():
        return False, {}
    producer = host == data["runner_image"]["source_host"]
    interval = options.producer_interval if producer else options.full_interval
    due = full_apply_due(receipt, options, interval)
    expected_changes = PRODUCER_CHECK_NOISE if producer else 0
    actionable_changes = changed != expected_changes
    if options.mode == "check":
        state = "CHECK-NOISE" if producer and not actionable_changes else "CURRENT"
        if actionable_changes:
            state = "DRIFT"
        print(f"fleet-reconcile: {host}: {state} (changed={changed})")
        return not actionable_changes, {}
    if not actionable_changes and not due:
        print(f"fleet-reconcile: {host}: current; no full converge due")
        previous = receipt if isinstance(receipt, dict) else {}
        return True, {**previous, "checked_at": options.now}
    why = "drift" if actionable_changes else "periodic full verification"
    print(f"fleet-reconcile: {host}: applying ({why}, changed={changed})")
    held_arc = producer and fm.CLASSES[data["hosts"][host]["class"]].capacity_kind == "k8s"
    held_changes = PRODUCER_HELD_CHECK_NOISE if held_arc else expected_changes
    applied, _ = apply_host(data, host, run, expected_check_changes=held_changes)
    if not applied:
        return False, {}
    return True, {
        "checked_at": options.now,
        "full_applied_at": options.now,
        "source_digest": options.source_digest,
    }


def reconcile(
    data: dict[str, Any], options: ReconcileOptions, run: CommandRunner = frp.command_runner
) -> int:
    """Reconcile producer then consumers, preserving dependency safety."""
    state_path = options.state_dir / STATE_FILE
    document = load_state(state_path)
    receipts = document["hosts"]
    failures = 0
    producer_failed = False
    for index, host in enumerate(runner_hosts(data)):
        if frp.interrupted_status():
            break
        if index and producer_failed:
            print(f"fleet-reconcile: {host}: BLOCKED by producer failure", file=sys.stderr)
            failures += 1
            continue

        receipt_invalidated = False

        def transaction_run(argv: Sequence[str], target: str = host) -> frp.CommandResult:
            nonlocal receipt_invalidated
            verb, _command_host = _command_identity(argv)
            if verb == "parked-apply" and not receipt_invalidated:
                receipts.pop(target, None)
                save_state(state_path, document)
                receipt_invalidated = True
            return run(argv)

        ok, receipt = reconcile_host(data, host, receipts.get(host), options, transaction_run)
        if ok and options.mode == "apply":
            receipts[host] = receipt
        if not ok:
            failures += 1
            producer_failed = index == 0 and options.mode == "apply"
            if options.mode == "apply":
                receipts.pop(host, None)
                save_state(state_path, document)
    if options.mode == "apply":
        save_state(state_path, document)
    return 1 if failures else 0


def validate_installed_authority(root: Path) -> str:
    """Authenticate the root-owned snapshot selected by the systemd unit."""
    lexical = root.absolute()
    resolved = lexical.resolve(strict=True)
    metadata = lexical.lstat()
    if lexical != resolved or stat.S_ISLNK(metadata.st_mode) or not stat.S_ISDIR(metadata.st_mode):
        msg = "installed reconciliation source is linked or not a real directory"
        raise ValueError(msg)
    if metadata.st_uid != 0 or metadata.st_mode & 0o022:
        msg = "installed reconciliation source must be root-owned and not group/world writable"
        raise ValueError(msg)
    marker = root / SOURCE_DIGEST_FILE
    marker_metadata = marker.lstat()
    if (
        stat.S_ISLNK(marker_metadata.st_mode)
        or not stat.S_ISREG(marker_metadata.st_mode)
        or marker_metadata.st_uid != 0
        or marker_metadata.st_mode & 0o222
    ):
        msg = "installed reconciliation source digest has unsafe ownership or mode"
        raise ValueError(msg)
    digest = marker.read_text(encoding="ascii").strip()
    if SHA256_RE.fullmatch(digest) is None:
        msg = "installed reconciliation source digest is malformed"
        raise ValueError(msg)
    return digest


def prepare_state_dir(path: Path) -> None:
    """Create or validate the caller-private state directory."""
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    metadata = path.lstat()
    if path.is_symlink() or not path.is_dir() or metadata.st_uid != os.getuid():
        msg = f"state directory is not caller-owned: {path}"
        raise ValueError(msg)
    if stat.S_IMODE(metadata.st_mode) != PRIVATE_DIRECTORY_MODE:
        msg = f"state directory is not mode 0700: {path}"
        raise ValueError(msg)


def _recap(host: str, changed: int = 0, failed: int = 0, unreachable: int = 0) -> str:
    """Build one Ansible recap fixture line."""
    return (
        f"{host} : ok=9 changed={changed} unreachable={unreachable} failed={failed} "
        "skipped=1 rescued=0 ignored=0\n"
    )


def _selftest_data() -> dict[str, Any]:
    """Return one fleet containing a producer, consumer, and excluded bench."""
    return {
        "runner_image": {"source_host": "producer"},
        "hosts": {
            "consumer": {
                "class": "docker_wsl",
                "runners": {"instances": 1},
                "provisions": ["one"],
            },
            "bench": {"class": "hil_bench", "provisions": ["bench"]},
            "producer": {
                "class": "docker_linux",
                "runners": {"instances": 1},
                "provisions": ["one", "two"],
            },
        },
    }


def _selftest_options(state_dir: Path, *, mode: str = "apply") -> ReconcileOptions:
    """Return deterministic policy inputs for controller tests."""
    return ReconcileOptions(
        mode=mode,
        force=False,
        source_digest="a" * 64,
        state_dir=state_dir,
        full_interval=100,
        producer_interval=50,
        now=1000,
    )


def _command_identity(argv: Sequence[str]) -> tuple[str, str]:
    """Return the fleet verb and host from a generated test command."""
    identities = {
        "capacity-quarantine": "quarantine",
        "capacity-restore": "restore",
        "reconcile-parked-apply": "parked-apply",
        "reconcile-parked-check": "parked-check",
        "reconcile-activate": "activate",
        "reconcile-activation-check": "activation-check",
    }
    return identities.get(argv[2], argv[2]), argv[-1]


def _check_result(data: dict[str, Any], host: str, changed: int = 0) -> frp.CommandResult:
    """Return a successful check with the declared recap-row count."""
    rows = [_recap(recap_identity(data, host), changed)]
    rows.extend(_recap(recap_identity(data, host)) for _ in data["hosts"][host]["provisions"][1:])
    return frp.CommandResult(0, "".join(rows), "")


def _clean_check_result(data: dict[str, Any], host: str) -> frp.CommandResult:
    """Return the exact accepted check result for one host class."""
    producer = host == data["runner_image"]["source_host"]
    return _check_result(data, host, PRODUCER_CHECK_NOISE if producer else 0)


def _selftest_wsl_status_mapping(data: dict[str, Any], failures: list[str]) -> None:
    """Prove safe WSL drift is actionable while probe failures stay fatal."""

    def apply_required(_argv: Sequence[str]) -> frp.CommandResult:
        return frp.CommandResult(fw.APPLY_REQUIRED_STATUS, "", "")

    if inspect_host(data, "consumer", apply_required) != (True, 1):
        failures.append("authenticated WSL stage drift did not request an apply")

    def fatal_probe(_argv: Sequence[str]) -> frp.CommandResult:
        return frp.CommandResult(5, "", "")

    if inspect_host(data, "consumer", fatal_probe) != (False, 0):
        failures.append("fatal WSL inspection error was treated as repairable drift")


def _selftest_order_parsing_and_schedule(failures: list[str]) -> None:
    """Prove runner selection, strict recap parsing, and interval decisions."""
    data = _selftest_data()
    if runner_hosts(data) != ["producer", "consumer"]:
        failures.append("producer ordering or non-runner exclusion drifted")
    if (
        parse_changed(_recap("producer", 1) + _recap("producer", 2), "producer", 2)
        != SELFTEST_CHANGED_TOTAL
    ):
        failures.append("changed recap sum drifted")
    wsl_calls: list[tuple[str, str]] = []

    def wsl_run(argv: Sequence[str]) -> frp.CommandResult:
        wsl_calls.append(_command_identity(argv))
        return _check_result(data, "consumer")

    if inspect_host(data, "consumer", wsl_run) != (True, 0):
        failures.append("WSL localhost recap was not mapped to its fleet identity")

    _selftest_wsl_status_mapping(data, failures)
    if wsl_calls != [("check", "consumer")]:
        failures.append("WSL recap mapping changed its declared control identity")
    try:
        parse_changed(_recap("consumer"), recap_identity(data, "consumer"), 1)
        failures.append("WSL declared name was accepted as its local recap identity")
    except ValueError:
        pass
    try:
        parse_changed(_recap("producer", failed=1), "producer", 1)
        failures.append("failed recap was accepted")
    except ValueError:
        pass
    try:
        parse_changed(_recap("producer", unreachable=1), "producer", 1)
        failures.append("unreachable recap was accepted")
    except ValueError:
        pass
    options = _selftest_options(Path("/unused"))
    fresh = {"source_digest": "a" * 64, "full_applied_at": 950}
    if full_apply_due(fresh, options, options.full_interval):
        failures.append("fresh matching receipt forced a full apply")
    if not full_apply_due(fresh, options, options.producer_interval):
        failures.append("expired producer receipt skipped its full apply")
    stale_source = {**fresh, "source_digest": "b" * 64}
    if not full_apply_due(stale_source, options, options.full_interval):
        failures.append("source change did not force a full apply")


def _selftest_check_mode(failures: list[str]) -> None:
    """Prove producer staging noise is tolerated but consumer drift is not."""
    data = _selftest_data()
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-reconcile-") as raw:
        state_dir = Path(raw)
        options = _selftest_options(state_dir, mode="check")
        calls: list[tuple[str, str]] = []

        def fake_run(argv: Sequence[str]) -> frp.CommandResult:
            verb, host = _command_identity(argv)
            calls.append((verb, host))
            return _clean_check_result(data, host)

        if reconcile(data, options, fake_run):
            failures.append("producer check-mode staging noise failed the controller")
        if calls != [("check", "producer"), ("check", "consumer")]:
            failures.append("check mode invoked a mutation or reordered hosts")

        def drift_run(argv: Sequence[str]) -> frp.CommandResult:
            _, host = _command_identity(argv)
            if host == "consumer":
                return _check_result(data, host, 1)
            return _clean_check_result(data, host)

        if reconcile(data, options, drift_run) != 1:
            failures.append("consumer drift passed check mode")

        def producer_drift_run(argv: Sequence[str]) -> frp.CommandResult:
            _, host = _command_identity(argv)
            return _check_result(data, host, 3 if host == "producer" else 0)

        if reconcile(data, options, producer_drift_run) != 1:
            failures.append("producer drift was mistaken for staging noise")


def _selftest_apply_and_receipt(failures: list[str]) -> None:
    """Prove drift is applied, rechecked, and recorded atomically."""
    data = _selftest_data()
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-reconcile-") as raw:
        state_dir = Path(raw)
        options = _selftest_options(state_dir)
        receipt = {"source_digest": "a" * 64, "full_applied_at": 975}
        save_state(
            state_dir / STATE_FILE,
            {"version": 1, "hosts": {"producer": receipt, "consumer": receipt}},
        )
        calls: list[tuple[str, str]] = []
        consumer_checks = 0

        def fake_run(argv: Sequence[str]) -> frp.CommandResult:
            nonlocal consumer_checks
            verb, host = _command_identity(argv)
            calls.append((verb, host))
            if verb == "check" and host == "consumer":
                consumer_checks += 1
                return _check_result(data, host, 2 if consumer_checks == 1 else 0)
            if verb in {"check", "parked-check"}:
                return _clean_check_result(data, host)
            return frp.CommandResult(0, "", "")

        if reconcile(data, options, fake_run):
            failures.append("repairable consumer drift failed reconciliation")
        expected = [
            ("check", "producer"),
            ("check", "consumer"),
            ("parked-apply", "consumer"),
            ("parked-check", "consumer"),
            ("restore", "consumer"),
            ("check", "consumer"),
        ]
        if calls != expected:
            failures.append("consumer repair did not follow check/apply/recheck order")
        stored = load_state(state_dir / STATE_FILE)["hosts"]["consumer"]
        if stored.get("full_applied_at") != options.now:
            failures.append("successful repair did not publish a receipt")


def _selftest_failure_quarantine(failures: list[str]) -> None:
    """Prove failed producer mutation drains it and blocks every consumer."""
    data = _selftest_data()
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-reconcile-") as raw:
        options = _selftest_options(Path(raw))
        calls: list[tuple[str, str]] = []

        def fake_run(argv: Sequence[str]) -> frp.CommandResult:
            verb, host = _command_identity(argv)
            calls.append((verb, host))
            if verb in {"check", "parked-check"}:
                return _clean_check_result(data, host)
            return frp.CommandResult(1 if verb == "parked-apply" else 0, "", "")

        if reconcile(data, options, fake_run) != 1:
            failures.append("producer mutation failure did not fail reconciliation")
        expected = [
            ("check", "producer"),
            ("parked-apply", "producer"),
            ("quarantine", "producer"),
        ]
        if calls != expected:
            failures.append("producer failure did not drain before blocking consumers")


def _selftest_failed_repair_retries(failures: list[str]) -> None:
    """Prove a failed repair invalidates success and forces the next full apply."""
    data = _selftest_data()
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-reconcile-") as raw:
        state_dir = Path(raw)
        options = _selftest_options(state_dir)
        receipt = {"source_digest": options.source_digest, "full_applied_at": 975}
        save_state(
            state_dir / STATE_FILE,
            {"version": 1, "hosts": {"producer": receipt, "consumer": receipt}},
        )
        consumer_checks = 0

        class SimulatedHardKill(BaseException):
            """Model controller death at the parked-child launch boundary."""

        def fail_repair(argv: Sequence[str]) -> frp.CommandResult:
            nonlocal consumer_checks
            verb, host = _command_identity(argv)
            if verb == "check" and host == "consumer":
                consumer_checks += 1
                return _check_result(data, host, 1)
            if verb in {"check", "parked-check"}:
                return _clean_check_result(data, host)
            if verb == "parked-apply":
                persisted = load_state(state_dir / STATE_FILE)["hosts"]
                if "consumer" in persisted:
                    failures.append("receipt was still successful when parked apply began")
                raise SimulatedHardKill
            return frp.CommandResult(0, "", "")

        try:
            reconcile(data, options, fail_repair)
            failures.append("simulated hard kill returned through reconciliation")
        except SimulatedHardKill:
            pass
        stored = load_state(state_dir / STATE_FILE)["hosts"]
        if "consumer" in stored:
            failures.append("failed repair retained its prior success receipt")
        retry_calls: list[tuple[str, str]] = []

        def retry(argv: Sequence[str]) -> frp.CommandResult:
            verb, host = _command_identity(argv)
            retry_calls.append((verb, host))
            if verb in {"check", "parked-check"}:
                return _clean_check_result(data, host)
            return frp.CommandResult(0, "", "")

        if reconcile(data, options, retry):
            failures.append("immediate retry after failed repair did not converge")
        if ("parked-apply", "consumer") not in retry_calls:
            failures.append("missing receipt did not force the next consumer repair")


def _selftest_postcheck_quarantine(failures: list[str]) -> None:
    """Prove a consumer that remains changed is drained after its repair."""
    data = _selftest_data()
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-reconcile-") as raw:
        state_dir = Path(raw)
        options = _selftest_options(state_dir)
        receipt = {"source_digest": "a" * 64, "full_applied_at": 975}
        save_state(
            state_dir / STATE_FILE,
            {"version": 1, "hosts": {"producer": receipt, "consumer": receipt}},
        )
        calls: list[tuple[str, str]] = []

        def fake_run(argv: Sequence[str]) -> frp.CommandResult:
            verb, host = _command_identity(argv)
            calls.append((verb, host))
            if verb in {"check", "parked-check"}:
                return (
                    _check_result(data, host, 1)
                    if host == "consumer"
                    else _clean_check_result(data, host)
                )
            return frp.CommandResult(0, "", "")

        if reconcile(data, options, fake_run) != 1:
            failures.append("non-idempotent consumer repair passed")
        expected_tail = [
            ("parked-apply", "consumer"),
            ("parked-check", "consumer"),
            ("quarantine", "consumer"),
        ]
        if calls[-3:] != expected_tail:
            failures.append("non-idempotent consumer was not quarantined")


def _selftest_restore_quarantine(failures: list[str]) -> None:
    """Prove a failed capacity restore is driven back to zero before return."""
    data = _selftest_data()
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-reconcile-") as raw:
        options = _selftest_options(Path(raw))
        calls: list[tuple[str, str]] = []

        def fake_run(argv: Sequence[str]) -> frp.CommandResult:
            verb, host = _command_identity(argv)
            calls.append((verb, host))
            if verb in {"check", "parked-check"}:
                return _clean_check_result(data, host)
            return frp.CommandResult(1 if verb == "restore" else 0, "", "")

        if reconcile(data, options, fake_run) != 1:
            failures.append("failed capacity restore passed reconciliation")
        expected_tail = [
            ("parked-apply", "producer"),
            ("parked-check", "producer"),
            ("restore", "producer"),
            ("quarantine", "producer"),
        ]
        if calls[-4:] != expected_tail:
            failures.append("failed restore did not drive the host back to zero")


def _selftest_state_safety(failures: list[str]) -> None:
    """Prove private state, symlink refusal, locking, and atomic round trips."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-reconcile-") as raw:
        state_dir = Path(raw)
        prepare_state_dir(state_dir)
        receipt = {"source_digest": "a" * 64, "full_applied_at": 950}
        state_path = state_dir / STATE_FILE
        save_state(state_path, {"version": 1, "hosts": {"producer": receipt}})
        if load_state(state_path)["hosts"]["producer"] != receipt:
            failures.append("atomic receipt round trip drifted")
        state_path.unlink()
        target = state_dir / "target.json"
        target.write_text('{"version": 1, "hosts": {}}\n', encoding="ascii")
        state_path.symlink_to(target)
        try:
            load_state(state_path)
            failures.append("linked state file was accepted")
        except ValueError:
            pass


def _selftest_timeout(failures: list[str]) -> None:
    """Prove a timed-out mutation returns through the quarantine path."""
    result = frp.command_runner(
        [
            sys.executable,
            "-c",
            "import sys,time; print('partial stdout', flush=True); "
            "print('partial stderr', file=sys.stderr, flush=True); time.sleep(60)",
        ],
        timeout_seconds=0.05,
    )
    if result.status != frp.TIMEOUT_STATUS or "command timed out" not in result.stderr:
        failures.append("a timed-out fleet command escaped fail-closed handling")
    if result.stdout != "partial stdout\n":
        failures.append("timed-out command evidence was discarded")


def _selftest_signal_quarantine(failures: list[str]) -> None:
    """Prove TERM stops the owned process group and completes quarantine."""
    data = _selftest_data()
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-reconcile-signal-") as raw:
        marker = Path(raw) / "quarantine"
        ready = marker.with_suffix(".ready")

        def request_stop() -> None:
            deadline = time.monotonic() + 5
            while not ready.exists() and time.monotonic() < deadline:
                time.sleep(0.01)
            if ready.exists():
                os.kill(os.getpid(), signal.SIGTERM)

        def fake_run(argv: Sequence[str]) -> frp.CommandResult:
            verb, _host = _command_identity(argv)
            if verb == "parked-apply":
                code = (
                    "import pathlib,signal,subprocess,sys,time; "
                    "child=subprocess.Popen(['sleep','60']); "
                    "signal.signal(signal.SIGTERM, lambda *_: "
                    "(child.terminate(), child.wait(), sys.exit(143))); "
                    f"pathlib.Path({str(ready)!r}).write_text(str(child.pid)); "
                    "time.sleep(60)"
                )
                return frp.command_runner(
                    [sys.executable, "-c", code], timeout_seconds=SELFTEST_SIGNAL_BOUND
                )
            if verb == "quarantine":
                marker.write_text("quarantined", encoding="ascii")
                return frp.CommandResult(0, "", "")
            return frp.CommandResult(2, "", "unexpected signal selftest command\n")

        sender = Thread(target=request_stop, name="fleet-reconcile-signal-selftest")
        sender.start()
        started = time.monotonic()
        with frp.stop_handlers():
            applied, _changed = apply_host(data, "consumer", fake_run, expected_check_changes=0)
        elapsed = time.monotonic() - started
        sender.join(timeout=1)
        if sender.is_alive():
            failures.append("signal selftest sender did not finish")
        if applied or frp.interrupted_status() != 128 + signal.SIGTERM:
            failures.append("TERM did not preserve the interrupted service status")
        if elapsed >= SELFTEST_SIGNAL_BOUND:
            failures.append("interrupted controller did not finish its handler")
        if not marker.exists():
            failures.append("TERM during a mutation did not route through quarantine")
        if not ready.exists():
            failures.append("signal selftest mutation never started")
            return
        worker_pid = int(ready.read_text(encoding="ascii"))
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            try:
                os.kill(worker_pid, 0)
            except ProcessLookupError:
                break
            time.sleep(0.01)
        else:
            failures.append("TERM left an owned mutation child running")


def selftest() -> int:
    """Exercise the unattended controller's safety-critical decisions."""
    failures: list[str] = []
    _selftest_order_parsing_and_schedule(failures)
    _selftest_check_mode(failures)
    _selftest_apply_and_receipt(failures)
    _selftest_failure_quarantine(failures)
    _selftest_failed_repair_retries(failures)
    _selftest_postcheck_quarantine(failures)
    _selftest_restore_quarantine(failures)
    failures.extend(fras.run(apply_host))
    _selftest_state_safety(failures)
    failures.extend(fml.run_selftest())
    _selftest_timeout(failures)
    failures.extend(frp.run_selftest())
    _selftest_signal_quarantine(failures)
    failures.extend(frs.runtime_inventory_selftest(fm))
    failures.extend(frs.run(fm.REPO_ROOT))
    for failure in failures:
        print(f"fleet_reconcile.py --selftest: FAIL: {failure}", file=sys.stderr)
    if failures:
        return 1
    print("fleet_reconcile.py --selftest: PASS")
    return 0


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    """Parse the offline selftest and live reconciliation boundaries."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("--mode", choices=("apply", "check"), default="apply")
    parser.add_argument("--force", action="store_true")
    parser.add_argument("--require-installed-authority", action="store_true")
    parser.add_argument("--state-dir", type=Path)
    parser.add_argument("--full-interval", type=int, default=DEFAULT_FULL_INTERVAL)
    parser.add_argument("--producer-interval", type=int, default=DEFAULT_PRODUCER_INTERVAL)
    return parser.parse_args(argv)


def _early_status(args: argparse.Namespace) -> int | None:
    """Handle non-live modes and reject invalid option combinations."""
    if args.selftest:
        return selftest()
    if args.force and args.mode != "apply":
        print("fleet-reconcile: --force requires --mode apply", file=sys.stderr)
        return 2
    if args.full_interval <= 0 or args.producer_interval <= 0:
        print("fleet-reconcile: convergence intervals must be positive", file=sys.stderr)
        return 2
    return None


def main(argv: Sequence[str] | None = None) -> int:
    """Enter selftest or one locked reconciliation transaction."""
    args = parse_args(argv)
    early_status = _early_status(args)
    if early_status is not None:
        return early_status
    try:
        source_digest = (
            validate_installed_authority(fm.REPO_ROOT)
            if args.require_installed_authority
            else "manual-operator-checkout"
        )
        state_dir = args.state_dir or Path.home() / ".local/state/ra8-fleet-reconcile"
        prepare_state_dir(state_dir)
        if args.require_installed_authority:
            fm.validate_runtime_inventory(state_dir)
        data = fm.load()
        options = ReconcileOptions(
            args.mode,
            args.force,
            source_digest,
            state_dir,
            args.full_interval,
            args.producer_interval,
            int(time.time()),
        )
        if args.mode == "check":
            with frp.stop_handlers():
                status = reconcile(data, options)
                return frp.interrupted_status() or status
        with (
            fml.mutation_lock(data, installed_local=args.require_installed_authority),
            frp.stop_handlers(),
        ):

            def guarded_runner(argv: Sequence[str]) -> frp.CommandResult:
                return frp.command_runner(argv, guardian=True)

            status = reconcile(data, options, guarded_runner)
            return frp.interrupted_status() or status
    except fml.MutationLockBusyError as error:
        print(f"fleet-reconcile: {error}", file=sys.stderr)
        return fml.LOCK_BUSY_STATUS
    except (
        fml.MutationLockError,
        OSError,
        TypeError,
        ValueError,
        json.JSONDecodeError,
        fm.FleetError,
    ) as error:
        print(f"fleet-reconcile: FATAL: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
