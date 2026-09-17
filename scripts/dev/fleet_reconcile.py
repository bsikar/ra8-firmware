#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# FILE-SIZE-OK: safety-critical controller and transaction selftests stay reviewable together.
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
import fleet_reconcile_backoff_selftest as frb
import fleet_reconcile_blocking_selftest as frbl
import fleet_reconcile_cascade_selftest as frc
import fleet_reconcile_drain_selftest as frd
import fleet_reconcile_freeze_selftest as frf
import fleet_reconcile_interrupt_selftest as fri
import fleet_reconcile_process as frp
import fleet_reconcile_prune_selftest as frpr
import fleet_reconcile_recovery_selftest as frr
import fleet_reconcile_release_selftest as frrl
import fleet_reconcile_reopen_selftest as frre
import fleet_reconcile_selftest as frs
import fleet_reconcile_settle_selftest as frse
import fleet_reconcile_stranding_selftest as frst
import fleet_wsl as fw

SOURCE_DIGEST_FILE = ".ra8-source-sha256"
STATE_FILE = "state.json"
DEFAULT_FULL_INTERVAL = 7 * 24 * 60 * 60
DEFAULT_PRODUCER_INTERVAL = 24 * 60 * 60
PRIVATE_DIRECTORY_MODE = 0o700
SELFTEST_CHANGED_TOTAL = 3
SELFTEST_SIGNAL_BOUND = 8
SELFTEST_RECOVERY_APPLIES = 2
# ci_runner check mode empties and restages its build context: exactly these
# two tasks report changed on an otherwise-converged producer.
PRODUCER_CHECK_NOISE = 2
# While ARC admission is deliberately held at zero, its post-renderer removes
# the scale-set difference and only the context-restage check noise remains.
PRODUCER_HELD_CHECK_NOISE = 1
PRODUCER_APPLY_ATTEMPTS = 3
# One mutation fault that lasts seconds rather than milliseconds must not burn
# every attempt inside the same fault window and strand the fleet at zero.
APPLY_RETRY_BACKOFF_SECONDS = 15
APPLY_RETRY_BACKOFF_CAP_SECONDS = 120
APPLY_RETRY_SLICE_SECONDS = 1.0
# A host whose drain fails is the inverse of stranding: the controller believes
# it holds no capacity while it is still handing work to a failed mutation.
DRAIN_FAILED_STATUS = 3
# A fleet that fails the same way every pass has to get LOUDER, not quieter.
# Issue #888 counted about five separate strandings at zero capacity, and every
# one of those passes exited 1 exactly like a one-off failure, so nothing told
# an operator apart a fleet idling at zero from a pass that simply failed.
STRANDED_ESCALATION_PASSES = 3
STRANDED_STATUS = 4
# Holding every consumer back while the producer is drained protects them from
# converging onto an image that is being republished underneath them.  A
# DRAINED producer publishes nothing, so once it has held zero capacity across
# this many consecutive passes its image has been frozen at last-known-good for
# that whole time and the block protects nothing while costing the fleet
# everything: a consumer already sitting at zero is skipped every pass and can
# never be repaired, which is how issue #888's fleet stayed at zero.
PRODUCER_BLOCK_PASSES = 3
# A receipt earned while the consumers are released was converged against the
# producer's FROZEN last-known-good image, not against whatever the producer
# serves once it recovers.  It still stamps full_applied_at, so an ordinary
# receipt tells every later pass that this consumer needs nothing for a whole
# interval: the producer's recovery never reaches it and the controller reports
# a fully converged fleet while every consumer still runs the image from before
# the outage (issue #888).  Mark them, so a producer proven to be serving again
# expires them and the consumers converge onto what it now publishes.
RELEASED_RECEIPT_KEY = "released_against"
# Draining after a failed mutation is the right fail-closed reflex for ONE
# host.  Repeated across a pass it is not convergence, it is an evacuation: a
# provision that is broken in the declaration fails on every host that carries
# it, so the controller drains them one after another and empties the fleet in
# a single pass, then does it again on the next one (issue #888).  A fault that
# reproduces on host after host is in the snapshot, not in the fleet, and no
# amount of further draining can fix it, so one pass may take at most this
# share of the capacity-managed hosts to zero before it stops mutating and
# leaves the rest serving.
PASS_DRAIN_BUDGET_RATIO = 0.5
CASCADE_STATUS = 5
# Every verb that can move a host's capacity.  A failure that issued none of
# them cannot have stranded the host, whatever else went wrong.
CAPACITY_MUTATION_VERBS = frozenset({"parked-apply", "quarantine", "restore", "activate"})
SHA256_RE = re.compile(r"[0-9a-f]{64}")
ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")


RECAP_RE = re.compile(
    r"^\s*([A-Za-z0-9_.-]+)\s+:\s+ok=(\d+)\s+changed=(\d+)\s+"
    r"unreachable=(\d+)\s+failed=(\d+)\s+skipped=(\d+)\s+"
    r"rescued=(\d+)\s+ignored=(\d+)\s*$"
)


class DrainFailedError(RuntimeError):
    """Raised when a host could not be drained after a failed mutation."""

    def __init__(self, host: str, status: int) -> None:
        """Name the host that stayed in service and the status it refused with."""
        super().__init__(
            f"{host}: capacity-quarantine failed (rc={status}); the host was NOT "
            "drained and may still be accepting work after a failed mutation"
        )
        self.host = host
        self.status = status


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
    """Drain a host after failed mutation so it cannot accept new work.

    A drain that fails was only logged as a warning, so every caller carried on
    reporting the host as drained.  That is the inverse of the stranding in
    issue #888 and the more dangerous half: the controller shows zero capacity
    for a host that is still accepting jobs against a mutation that failed
    halfway.  Refuse to return normally from a drain that did not land.
    """
    print(f"fleet-reconcile: quarantining {host} at zero capacity", file=sys.stderr)
    result = run(fleet_command(host, "quarantine"))
    emit_result(result)
    if result.status:
        raise DrainFailedError(host, result.status)


def recover_last_known_good(
    data: dict[str, Any], host: str, run: CommandRunner, expected_check_changes: int
) -> bool:
    """Reopen a drained host that was already converged before its apply failed.

    A periodic full verification that fails on a transient fault (a locked
    dependency download timing out mid image build) leaves the host running the
    declaration it was already converged on.  Draining it is the correct
    fail-closed reflex, but leaving it drained removes healthy last-known-good
    capacity for a fault that never touched the host.  Reopen it and prove it
    serves, or hold it at zero.
    """
    print(
        f"fleet-reconcile: {host}: apply failed with no prior drift; "
        "reopening last-known-good capacity",
        file=sys.stderr,
    )
    restore = run(fleet_command(host, "restore"))
    emit_result(restore)
    if restore.status or frp.interrupted_status():
        quarantine(host, run)
        return False
    clean, changed = inspect_host(data, host, run)
    if not clean or changed != expected_check_changes or frp.interrupted_status():
        print(
            f"fleet-reconcile: {host}: last-known-good capacity did not verify "
            f"(remaining changed={changed}); holding at zero",
            file=sys.stderr,
        )
        quarantine(host, run)
        return False
    print(
        f"fleet-reconcile: WARNING: {host}: full verification FAILED while serving "
        "last-known-good capacity; this pass fails and retries on the next one",
        file=sys.stderr,
    )
    return True


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


def retry_delay(attempt: int) -> int:
    """Return the pause before one more parked apply, doubling per failed attempt."""
    if attempt < 1:
        msg = "mutation attempts are numbered from one"
        raise ValueError(msg)
    growth = APPLY_RETRY_BACKOFF_SECONDS * 2 ** (attempt - 1)
    return min(growth, APPLY_RETRY_BACKOFF_CAP_SECONDS)


def wait_before_retry(seconds: float, sleep: Callable[[float], None] = time.sleep) -> bool:
    """Pause between mutation attempts in slices an administrative stop can cut."""
    remaining = float(seconds)
    while remaining > 0:
        if frp.interrupted_status():
            return False
        taken = min(APPLY_RETRY_SLICE_SECONDS, remaining)
        sleep(taken)
        remaining -= taken
    return not frp.interrupted_status()


def report_double_refusal(
    host: str, drain_failure: DrainFailedError | None, recovery_failure: DrainFailedError
) -> None:
    """Name every refused drain when recovery's own drain is refused as well.

    Recovering last-known-good capacity drains again when the reopen does not
    verify, and that second drain can be refused too.  It raised straight out
    of ``settle_exhausted_mutation``, so a first refusal recorded moments
    earlier was dropped on the floor: the operator saw one status code for a
    host the controller had failed to drain twice, with nothing tying the two
    attempts together.  Issue #888 went unnoticed five times because a fleet
    held at zero read like a one-off failure, so a host nobody can drain says
    so in full.
    """
    if drain_failure is None:
        print(
            f"fleet-reconcile: CRITICAL: {host}: the drain that followed an "
            f"unverified reopen was REFUSED (rc={recovery_failure.status}); the "
            "first drain landed, but capacity was reopened after it, so this "
            "host is unaccounted for",
            file=sys.stderr,
        )
        return
    print(
        f"fleet-reconcile: CRITICAL: {host}: drain REFUSED twice "
        f"(after the exhausted mutation rc={drain_failure.status}, after the "
        f"unverified reopen rc={recovery_failure.status}); nothing proved what "
        "this host is serving and nothing could take it out of service",
        file=sys.stderr,
    )


def settle_exhausted_mutation(
    host: str, run: CommandRunner, on_mutation_exhausted: Callable[[], bool] | None
) -> None:
    """Drain a host whose mutation is exhausted, then account for its capacity.

    Drain first, always: that is the fail-closed reflex.  Raising on a refused
    drain used to leave ``apply_host`` before the recovery hook could run, so a
    host that was converged before its apply failed was reported as unaccounted
    for while proving it still serves last-known-good capacity was right there
    (issue #888).  Attempt the drain, then recover, then decide which verdict
    the host has earned.
    """
    drain_failure: DrainFailedError | None = None
    try:
        quarantine(host, run)
    except DrainFailedError as error:
        drain_failure = error
    try:
        accounted = on_mutation_exhausted() if on_mutation_exhausted is not None else False
    except DrainFailedError as recovery_failure:
        report_double_refusal(host, drain_failure, recovery_failure)
        if drain_failure is None:
            raise
        raise recovery_failure from drain_failure
    if drain_failure is None:
        return
    if not accounted:
        raise drain_failure
    print(
        f"fleet-reconcile: WARNING: {host}: drain was REFUSED "
        f"(rc={drain_failure.status}); last-known-good capacity verified instead, "
        "so the host is accounted for and this pass still fails",
        file=sys.stderr,
    )


def apply_host(  # noqa: PLR0913  # transaction inputs plus its recovery hook
    data: dict[str, Any],
    host: str,
    run: CommandRunner,
    *,
    expected_check_changes: int,
    apply_attempts: int = 1,
    on_mutation_exhausted: Callable[[], bool] | None = None,
    sleep: Callable[[float], None] = time.sleep,
) -> tuple[bool, int]:
    """Apply one host and prove the resulting declaration is idempotent."""
    applied = False
    for attempt in range(1, apply_attempts + 1):
        result = run(fleet_command(host, "parked-apply"))
        emit_result(result)
        if not result.status and not frp.interrupted_status():
            applied = True
            break
        if frp.interrupted_status():
            break
        if attempt < apply_attempts:
            delay = retry_delay(attempt)
            print(
                f"fleet-reconcile: {host}: parked apply failed "
                f"(attempt {attempt}/{apply_attempts}); waiting {delay}s before "
                "retrying while capacity stays zero",
                file=sys.stderr,
            )
            if not wait_before_retry(delay, sleep):
                break
    if not applied:
        settle_exhausted_mutation(host, run, on_mutation_exhausted)
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


def reconcile_host(  # noqa: PLR0913  # transaction inputs plus injectable retry pacing
    data: dict[str, Any],
    host: str,
    receipt: object,
    options: ReconcileOptions,
    run: CommandRunner,
    *,
    sleep: Callable[[float], None] = time.sleep,
) -> tuple[bool, bool, dict[str, Any]]:
    """Inspect and optionally converge one host, reporting whether it is drained."""
    clean, changed = inspect_host(data, host, run)
    if frp.interrupted_status():
        return False, True, {}
    if not clean:
        # A read-only check mutates nothing: the host keeps serving whatever
        # capacity it already had, and no consumer's image dependency moved.
        # Reporting this as stranded blocked every consumer for a fault that
        # touched nothing, so a consumer already sitting at zero could never
        # be repaired while the producer check kept failing (issue #888).
        print(
            f"fleet-reconcile: WARNING: {host}: read-only check FAILED; "
            "capacity is untouched, so this pass fails without draining it "
            "or holding back the rest of the fleet",
            file=sys.stderr,
        )
        return False, False, {}
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
        return not actionable_changes, False, {}
    if not actionable_changes and not due:
        print(f"fleet-reconcile: {host}: current; no full converge due")
        previous = receipt if isinstance(receipt, dict) else {}
        return True, False, {**previous, "checked_at": options.now}
    why = "drift" if actionable_changes else "periodic full verification"
    print(f"fleet-reconcile: {host}: applying ({why}, changed={changed})")
    held_arc = producer and fm.CLASSES[data["hosts"][host]["class"]].capacity_kind == "k8s"
    held_changes = PRODUCER_HELD_CHECK_NOISE if held_arc else expected_changes
    attempts = PRODUCER_APPLY_ATTEMPTS if producer else 1
    recovered = False

    def reopen_last_known_good() -> bool:
        """Reopen capacity only when the failed apply followed a clean check.

        Returns whether this host's capacity ended up accounted for, which is
        what lets a refused drain be downgraded from an unaccounted-for host to
        a failed pass against verified last-known-good capacity.
        """
        nonlocal recovered
        if actionable_changes or frp.interrupted_status():
            return False
        recovered = recover_last_known_good(data, host, run, expected_changes)
        return recovered

    applied, _ = apply_host(
        data,
        host,
        run,
        expected_check_changes=held_changes,
        apply_attempts=attempts,
        on_mutation_exhausted=reopen_last_known_good,
        sleep=sleep,
    )
    if not applied:
        return False, not recovered, {}
    return (
        True,
        False,
        {
            "checked_at": options.now,
            "full_applied_at": options.now,
            "source_digest": options.source_digest,
        },
    )


def capacity_mutation(verb: str) -> bool:
    """Return whether one issued fleet verb can move a host's capacity."""
    return verb in CAPACITY_MUTATION_VERBS


def capacity_lost(host: str, *, stranded: bool, mutated: bool) -> bool:
    """Return whether this pass actually took a failed host's capacity down.

    ``stranded`` is the fail-closed verdict a failure earns: it decides whether
    a failed producer holds its consumers back for the rest of the pass, and it
    is deliberately true for an administrative stop, which can arrive at any
    point including one the controller cannot see past.  The stranded-at-zero
    RECORD is a different claim, and a much stronger one: escalation counts it
    and the consumer release reads it as proof the producer's image is frozen
    because the producer was drained.  A stop that arrives while a host is only
    being READ issues no mutation at all, so recording it forged exactly that
    proof, and repeated stops would escalate a fleet that never left service
    and release every consumer onto a producer that may be mid-republish
    (issue #888).  Only a pass that actually issued a capacity mutation may
    claim it lost capacity.
    """
    if not stranded or mutated:
        return stranded
    print(
        f"fleet-reconcile: WARNING: {host}: this pass failed before it issued any "
        "capacity mutation, so the host keeps whatever it was serving; not counted "
        "as stranded at zero",
        file=sys.stderr,
    )
    return False


def load_stranding(document: dict[str, Any]) -> dict[str, dict[str, int]]:
    """Return the record of hosts this controller drained and never reopened.

    A failed pass deliberately drops the host's receipt, so the state file
    remembered nothing at all about a host being held at zero and every pass
    looked like the first one.  This map survives receipt invalidation and is
    the only thing that can tell one transient failure from a fleet that has
    been sitting at zero capacity for days (issue #888).  Anything malformed is
    replaced rather than trusted, and state written by an older controller
    simply starts empty.
    """
    stored = document.get("stranded")
    entries = stored.items() if isinstance(stored, dict) else []
    stranding = {
        host: {"since": entry["since"], "passes": entry["passes"]}
        for host, entry in entries
        if isinstance(entry, dict)
        and isinstance(entry.get("since"), int)
        and isinstance(entry.get("passes"), int)
    }
    document["stranded"] = stranding
    return stranding


def record_stranding(stranding: dict[str, dict[str, int]], host: str, now: int) -> None:
    """Count one more consecutive pass that ended with a host at zero."""
    previous = stranding.get(host)
    entry = {
        "since": previous["since"] if previous else now,
        "passes": previous["passes"] + 1 if previous else 1,
    }
    stranding[host] = entry
    print(
        f"fleet-reconcile: WARNING: {host}: left at ZERO capacity by this pass "
        f"(consecutive passes={entry['passes']}, first drained at {entry['since']})",
        file=sys.stderr,
    )


def clear_stranding(stranding: dict[str, dict[str, int]], host: str) -> bool:
    """Forget a host's stranding, reporting whether it was held at zero before.

    The answer is what tells one ordinary successful pass from a host climbing
    back off zero capacity, which is the only evidence that a producer has
    republished since its consumers were released onto a frozen image.
    """
    if stranding.pop(host, None) is None:
        return False
    print(
        f"fleet-reconcile: {host}: reconciled and serving again; clearing its "
        "stranded-at-zero record",
        file=sys.stderr,
    )
    return True


def prune_unmanaged_stranding(
    stranding: dict[str, dict[str, int]], hosts: Sequence[str], now: int
) -> list[str]:
    """Forget stranded-at-zero records for hosts this fleet no longer manages.

    The record deliberately survives receipt invalidation, and nothing but the
    host reconciling ever clears it.  A runner pulled from the declaration
    while it was stranded, which is exactly what an operator does with a host
    that has sat at zero for days, therefore keeps its record for good: every
    later pass escalates it, prints CRITICAL and returns ``STRANDED_STATUS``
    with the whole remaining fleet healthy.  A verdict that never goes back to
    zero is how the loudness this controller gained for issue #888 turns into
    noise nobody reads, and the next real stranding arrives on a timer that has
    already been failing for weeks.  The controller cannot inspect, drain or
    repair a host outside the declaration, so it says once that it has stopped
    tracking the host and then stops claiming it.
    """
    managed = set(hosts)
    pruned = [host for host in sorted(stranding) if host not in managed]
    for host in pruned:
        entry = stranding.pop(host)
        print(
            f"fleet-reconcile: WARNING: {host} is no longer a capacity-managed host in "
            f"this fleet; dropping its stranded-at-zero record (consecutive passes="
            f"{entry['passes']}, {now - entry['since']}s since it was first drained). "
            "If that host is still deployed its capacity is now the operator's to "
            "check: this controller has stopped tracking it",
            file=sys.stderr,
        )
    return pruned


def open_stranding(
    document: dict[str, Any], hosts: Sequence[str], options: ReconcileOptions
) -> dict[str, dict[str, int]]:
    """Return the stranded-at-zero record this pass may reason about.

    Pruning is a write, so a check pass, which persists nothing at all, reads
    the record exactly as it stands rather than announcing a cleanup it is not
    going to make.
    """
    stranding = load_stranding(document)
    if options.mode == "apply":
        prune_unmanaged_stranding(stranding, hosts, options.now)
    return stranding


def invalidate_receipt(  # noqa: PLR0913  # the receipt plus every record one failure settles
    receipts: dict[str, Any],
    stranding: dict[str, dict[str, int]],
    drained: list[str],
    host: str,
    now: int,
    *,
    stranded: bool,
    at_zero: bool = True,
) -> None:
    """Drop a failed host's receipt, counting a pass that left it at zero.

    ``drained`` collects only the hosts THIS pass took from serving to zero,
    which is what the pass's drain budget is spent on.  Two failures record a
    stranding without spending it.  A host already carrying a record had no
    capacity left to lose, so draining it again costs the fleet nothing: the
    producer the consumers were released past (``PRODUCER_BLOCK_PASSES``) is
    drained again every pass and would otherwise halt the pass that finally
    repairs them.  And a host whose drain was REFUSED is unaccounted for rather
    than at zero, since it may well still be serving, so it must not stop the
    rest of the fleet reconciling either.
    """
    receipts.pop(host, None)
    if not stranded:
        return
    newly_drained = host not in stranding
    record_stranding(stranding, host, now)
    if newly_drained and at_zero:
        drained.append(host)


def released_receipt(receipt: dict[str, Any], host: str, producer: str) -> dict[str, Any]:
    """Mark a receipt converged against a frozen last-known-good image."""
    print(
        f"fleet-reconcile: WARNING: {host}: reconciled against {producer}'s FROZEN "
        "last-known-good image; its receipt is provisional until that producer is "
        "serving again",
        file=sys.stderr,
    )
    return {**receipt, RELEASED_RECEIPT_KEY: producer}


def expire_released_receipts(
    receipts: dict[str, Any], hosts: Sequence[str], producer: str
) -> list[str]:
    """Make every consumer released onto a frozen image converge again.

    Dropping the provisional receipt is what forces the full apply: the
    consumer is reconciled later in this same pass, so the image the recovered
    producer publishes reaches the fleet immediately instead of waiting out a
    full interval the frozen-image receipt had already claimed.
    """
    expired = [
        host
        for host in hosts
        if isinstance(receipts.get(host), dict)
        and receipts[host].get(RELEASED_RECEIPT_KEY) == producer
    ]
    for host in expired:
        receipts.pop(host, None)
    if expired:
        print(
            f"fleet-reconcile: producer {producer} is serving again; expiring the "
            "provisional receipt(s) earned against its frozen image so they converge "
            f"onto what it publishes now: {', '.join(expired)}",
            file=sys.stderr,
        )
    return expired


def stranded_escalations(stranding: dict[str, dict[str, int]], now: int) -> list[str]:
    """Name every host the fleet has failed to lift off zero, loudly."""
    escalated: list[str] = []
    for host, entry in sorted(stranding.items()):
        if entry["passes"] < STRANDED_ESCALATION_PASSES:
            continue
        escalated.append(host)
        print(
            f"fleet-reconcile: CRITICAL: {host} has held ZERO capacity across "
            f"{entry['passes']} consecutive reconcile passes "
            f"({now - entry['since']}s since it was first drained); this fleet is "
            "not recovering on its own and needs operator intervention",
            file=sys.stderr,
        )
    return escalated


def consumers_released(
    stranding: dict[str, dict[str, int]], producer: str, *, undrained: bool
) -> bool:
    """Return whether consumers may reconcile while the producer sits at zero.

    The block exists so a consumer cannot converge onto an image the producer
    is republishing underneath it.  A producer that was DRAINED publishes
    nothing, so a stranding that has survived ``PRODUCER_BLOCK_PASSES`` passes
    has left the image frozen at last-known-good for that whole time: the
    consumers would pull exactly what they already run.  Holding them back past
    that point is the half of issue #888 where the fleet never climbs out, since
    a consumer stranded at zero by an earlier pass is skipped every pass and
    never repaired.  A producer that could not be drained is unaccounted for and
    may still be publishing, so it keeps its consumers back however long it
    stays that way.
    """
    if undrained:
        return False
    entry = stranding.get(producer)
    if entry is None or entry["passes"] < PRODUCER_BLOCK_PASSES:
        return False
    print(
        f"fleet-reconcile: CRITICAL: producer {producer} has held ZERO capacity "
        f"across {entry['passes']} consecutive passes; its image is frozen at "
        "last-known-good, so the consumers are released to reconcile against it "
        "rather than sit at zero waiting for a producer that is not recovering",
        file=sys.stderr,
    )
    return True


def producer_block_state(
    stranding: dict[str, dict[str, int]], host: str, *, stranded: bool, undrained: bool
) -> tuple[bool, bool]:
    """Return whether a failed producer blocks its consumers, and whether it released them.

    Released is not merely the opposite of blocking: a producer that failed
    without losing capacity never blocked anyone and its image is not frozen,
    so a consumer reconciling past it earns an ordinary receipt.  Only a
    deliberate release onto a drained producer's frozen image makes one
    provisional.
    """
    if not stranded:
        print(
            f"fleet-reconcile: WARNING: producer {host} FAILED without "
            "losing capacity; consumers continue against last-known-good",
            file=sys.stderr,
        )
        return False, False
    released = consumers_released(stranding, host, undrained=undrained)
    return not released, released


def record_success(  # noqa: PLR0913  # the receipt plus every record one success settles
    receipts: dict[str, Any],
    stranding: dict[str, dict[str, int]],
    order: Sequence[str],
    host: str,
    receipt: dict[str, Any],
    *,
    index: int,
    released: bool,
) -> None:
    """Publish one reconciled host's receipt and settle the frozen-image records."""
    producer = order[0]
    receipts[host] = released_receipt(receipt, host, producer) if index and released else receipt
    if clear_stranding(stranding, host) and not index:
        expire_released_receipts(receipts, order[1:], producer)


def report_uninspected(remaining: Sequence[str]) -> None:
    """Name every host an administrative stop left unexamined this pass.

    A pass cut short printed nothing at all about the hosts it never reached,
    so a fleet with a host already sitting at zero read exactly like a fleet
    that had just been looked at end to end.  Issue #888 went unnoticed five
    times on silence of this shape.
    """
    print(
        "fleet-reconcile: WARNING: administrative stop ended this pass with host(s) "
        f"never inspected: {', '.join(remaining)}; anything already at zero capacity "
        "was not looked at, so this pass cannot have repaired it",
        file=sys.stderr,
    )


def drain_budget(total: int) -> int:
    """Return how many serving hosts one pass may take to zero."""
    return max(1, int(total * PASS_DRAIN_BUDGET_RATIO))


def report_cascade_halt(host: str, drained: Sequence[str], budget: int) -> None:
    """Say why a host was left serving instead of converged."""
    print(
        f"fleet-reconcile: CRITICAL: {host}: NOT reconciled by this pass, which has "
        f"already taken {len(drained)} serving host(s) to ZERO capacity "
        f"({', '.join(drained)}) against a budget of {budget}. A fault reproducing on "
        "host after host is in this snapshot, not in the fleet, so the capacity still "
        "serving is left alone rather than drained after it; fix the declaration",
        file=sys.stderr,
    )


def consumer_held(  # noqa: PLR0913  # one hold decision over the whole pass's state
    host: str,
    stranding: dict[str, dict[str, int]],
    halted: list[str],
    *,
    blocking: bool,
    drained: Sequence[str],
    budget: int,
) -> bool:
    """Return whether a consumer is skipped before anything touches it.

    Two holds, both leaving the host exactly as it is: the producer block, and
    the pass's drain budget.  A host that is ALREADY recorded at zero is never
    held by the budget, because it has no capacity left for the budget to
    protect and repairing it is the recovery issue #888 is about; the budget
    only stops a pass from emptying hosts that are still serving.
    """
    if blocking:
        print(f"fleet-reconcile: {host}: BLOCKED by producer failure", file=sys.stderr)
        return True
    if len(drained) < budget or host in stranding:
        return False
    report_cascade_halt(host, drained, budget)
    halted.append(host)
    return True


def pass_verdict(
    undrained: Sequence[str], escalated: Sequence[str], halted: Sequence[str], failures: int
) -> int:
    """Rank what one pass has to report, the least accounted-for capacity first."""
    if undrained:
        print(
            "fleet-reconcile: CRITICAL: pass finished with host(s) that failed to "
            f"drain and may still be serving work: {', '.join(undrained)}",
            file=sys.stderr,
        )
        return DRAIN_FAILED_STATUS
    if escalated:
        return STRANDED_STATUS
    if halted:
        print(
            "fleet-reconcile: CRITICAL: pass STOPPED mutating to keep the rest of the "
            f"fleet serving; host(s) deliberately left untouched: {', '.join(halted)}",
            file=sys.stderr,
        )
        return CASCADE_STATUS
    return 1 if failures else 0


def reconcile(
    data: dict[str, Any],
    options: ReconcileOptions,
    run: CommandRunner = frp.command_runner,
    sleep: Callable[[float], None] = time.sleep,
) -> int:
    """Reconcile producer then consumers, preserving dependency safety.

    Returns 0 for a clean pass, 1 for an ordinary failure, ``CASCADE_STATUS``
    when the pass stopped mutating to keep the rest of the fleet serving,
    ``STRANDED_STATUS`` when a host has been held at zero capacity for several
    consecutive passes, and ``DRAIN_FAILED_STATUS`` when a host could not be
    drained at all, which is louder still because that host is unaccounted for.
    """
    state_path = options.state_dir / STATE_FILE
    document = load_state(state_path)
    receipts = document["hosts"]
    order = runner_hosts(data)
    stranding = open_stranding(document, order, options)
    budget = drain_budget(len(order))
    failures = 0
    producer_blocking = False
    producer_released = False
    undrained: list[str] = []
    drained: list[str] = []
    halted: list[str] = []
    for index, host in enumerate(order):
        if frp.interrupted_status():
            report_uninspected(order[index:])
            break
        if index and consumer_held(
            host, stranding, halted, blocking=producer_blocking, drained=drained, budget=budget
        ):
            failures += 1
            continue

        receipt_invalidated = False
        mutated = False

        def transaction_run(argv: Sequence[str], target: str = host) -> frp.CommandResult:
            nonlocal receipt_invalidated, mutated
            verb, _command_host = _command_identity(argv)
            mutated = mutated or capacity_mutation(verb)
            if verb == "parked-apply" and not receipt_invalidated:
                receipts.pop(target, None)
                save_state(state_path, document)
                receipt_invalidated = True
            return run(argv)

        try:
            ok, stranded, receipt = reconcile_host(
                data, host, receipts.get(host), options, transaction_run, sleep=sleep
            )
        except DrainFailedError as error:
            print(
                f"fleet-reconcile: CRITICAL: {error}; operator intervention required "
                "before this host is trusted again",
                file=sys.stderr,
            )
            undrained.append(host)
            # Unaccounted for, not proven safe: keep the fail-closed verdict so a
            # producer in this state still holds its consumers back.
            ok, stranded, receipt = False, True, {}
        if ok and options.mode == "apply":
            record_success(
                receipts, stranding, order, host, receipt, index=index, released=producer_released
            )
        if not ok:
            failures += 1
            if options.mode == "apply":
                lost = capacity_lost(host, stranded=stranded, mutated=mutated)
                invalidate_receipt(
                    receipts,
                    stranding,
                    drained,
                    host,
                    options.now,
                    stranded=lost,
                    at_zero=host not in undrained,
                )
                save_state(state_path, document)
            if index == 0 and options.mode == "apply":
                producer_blocking, producer_released = producer_block_state(
                    stranding, host, stranded=stranded, undrained=host in undrained
                )
    escalated: list[str] = []
    if options.mode == "apply":
        save_state(state_path, document)
        escalated = stranded_escalations(stranding, options.now)
    return pass_verdict(undrained, escalated, halted, failures)


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


def _no_wait(_seconds: float) -> None:
    """Take retry pacing out of the controller selftests without skipping it."""


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
    """Prove a drifting producer that cannot apply drains and blocks consumers.

    The producer must be genuinely drifting here.  A converged producer whose
    periodic apply fails is reopened at last-known-good capacity instead, which
    fleet_reconcile_recovery_selftest.py pins (issue #888).
    """
    data = _selftest_data()
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-reconcile-") as raw:
        options = _selftest_options(Path(raw))
        calls: list[tuple[str, str]] = []

        def fake_run(argv: Sequence[str]) -> frp.CommandResult:
            verb, host = _command_identity(argv)
            calls.append((verb, host))
            if verb == "check" and host == "producer":
                return _check_result(data, host, PRODUCER_CHECK_NOISE + 1)
            if verb in {"check", "parked-check"}:
                return _clean_check_result(data, host)
            return frp.CommandResult(1 if verb == "parked-apply" else 0, "", "")

        if reconcile(data, options, fake_run, _no_wait) != 1:
            failures.append("producer mutation failure did not fail reconciliation")
        expected = [
            ("check", "producer"),
            ("parked-apply", "producer"),
            ("parked-apply", "producer"),
            ("parked-apply", "producer"),
            ("quarantine", "producer"),
        ]
        if calls != expected:
            failures.append("producer failure did not drain before blocking consumers")


def _selftest_transient_producer_retry(failures: list[str]) -> None:
    """Prove one transient producer mutation failure does not strand capacity."""
    data = _selftest_data()
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-reconcile-") as raw:
        options = _selftest_options(Path(raw))
        calls: list[tuple[str, str]] = []
        producer_applies = 0

        def fake_run(argv: Sequence[str]) -> frp.CommandResult:
            nonlocal producer_applies
            verb, host = _command_identity(argv)
            calls.append((verb, host))
            if verb in {"check", "parked-check", "activation-check"}:
                return _clean_check_result(data, host)
            if verb == "parked-apply" and host == "producer":
                producer_applies += 1
                return frp.CommandResult(1 if producer_applies == 1 else 0, "", "")
            return frp.CommandResult(0, "", "")

        if reconcile(data, options, fake_run, _no_wait):
            failures.append("transient producer mutation failure did not recover")
        if producer_applies < SELFTEST_RECOVERY_APPLIES:
            failures.append("transient producer mutation failure was not retried")
        if ("quarantine", "producer") in calls:
            failures.append("recovered producer mutation was quarantined")
        if ("check", "consumer") not in calls:
            failures.append("recovered producer mutation still blocked consumers")


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
    _selftest_transient_producer_retry(failures)
    _selftest_failed_repair_retries(failures)
    _selftest_postcheck_quarantine(failures)
    _selftest_restore_quarantine(failures)
    failures.extend(fras.run(apply_host))
    failures.extend(frr.run(sys.modules[__name__]))
    failures.extend(frb.run(sys.modules[__name__]))
    failures.extend(frbl.run(sys.modules[__name__]))
    failures.extend(frc.run(sys.modules[__name__]))
    failures.extend(frd.run(sys.modules[__name__]))
    failures.extend(frre.run(sys.modules[__name__]))
    failures.extend(frst.run(sys.modules[__name__]))
    failures.extend(frrl.run(sys.modules[__name__]))
    failures.extend(frse.run(sys.modules[__name__]))
    failures.extend(fri.run(sys.modules[__name__]))
    failures.extend(frf.run(sys.modules[__name__]))
    failures.extend(frpr.run(sys.modules[__name__]))
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
