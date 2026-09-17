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
from dataclasses import dataclass, replace
from pathlib import Path
from threading import Thread
from typing import Any, TextIO

sys.path.insert(0, str(Path(__file__).resolve().parent))

import fleet_model as fm
import fleet_mutation_lock as fml
import fleet_reconcile_aborted_selftest as frab
import fleet_reconcile_activation_selftest as frac
import fleet_reconcile_admission_selftest as frad
import fleet_reconcile_aging_selftest as fra
import fleet_reconcile_arc_selftest as fras
import fleet_reconcile_backoff_selftest as frb
import fleet_reconcile_blocking_selftest as frbl
import fleet_reconcile_budget_selftest as frbu
import fleet_reconcile_cascade_selftest as frc
import fleet_reconcile_drain_selftest as frd
import fleet_reconcile_dryrun_selftest as frdr
import fleet_reconcile_forfeit_selftest as frfo
import fleet_reconcile_freeze_selftest as frf
import fleet_reconcile_frozen_selftest as frfz
import fleet_reconcile_interrupt_selftest as fri
import fleet_reconcile_lift_selftest as frlt
import fleet_reconcile_locked_selftest as frlo
import fleet_reconcile_opener_selftest as frop
import fleet_reconcile_orphan_selftest as fro
import fleet_reconcile_park_escalation_selftest as frpe
import fleet_reconcile_parked_selftest as frpk
import fleet_reconcile_process as frp
import fleet_reconcile_prune_selftest as frpr
import fleet_reconcile_publish_selftest as frpu
import fleet_reconcile_reactivate_selftest as frrc
import fleet_reconcile_recovery_selftest as frr
import fleet_reconcile_release_selftest as frrl
import fleet_reconcile_reopen_selftest as frre
import fleet_reconcile_selftest as frs
import fleet_reconcile_serving_selftest as frsv
import fleet_reconcile_settle_selftest as frse
import fleet_reconcile_stopped_selftest as frsp
import fleet_reconcile_stranding_selftest as frst
import fleet_reconcile_unaccounted_selftest as fru
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
# A durable maintenance park is the OTHER way a host sits at zero admission,
# and its record only ever warned.  The restore that lifts a park is issued for
# a host whose own declaration verified THIS pass, so a parked host nothing
# reconciles is never reached at all: one whose read-only check keeps failing,
# the everyday shape of a host that is already down, or one held behind a
# failed producer, simply ages its record for ever while every pass exits 1
# exactly like a one-off failure.  That is the silence issue #888 went
# unnoticed in five times, so a park hold the same clock as a stranding: a host
# nothing has lifted off zero across this many consecutive passes needs an
# operator, whichever of the two records knows about it.
PARK_ESCALATION_PASSES = STRANDED_ESCALATION_PASSES
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
# The three verdicts that are about capacity this fleet is not serving: a host
# nobody could drain, a host held at zero across consecutive passes, and a pass
# that stopped mutating to keep the rest of the fleet up.  An administrative
# stop (TERM from a unit restart, HUP, an operator's Ctrl-C, a systemd runtime
# limit cutting a slow pass short) exits 128+signal, which reads exactly like a
# clean `systemctl stop` and tells alerting to retry later.  Over a fleet this
# controller has already taken to zero it hides the one verdict that mattered,
# and a pass stopped at the same point every time hides it for ever: issue #888
# went unnoticed about five times on silence of this shape.  These statuses
# therefore outrank the stop status on the way out.
ZERO_CAPACITY_STATUSES = (DRAIN_FAILED_STATUS, STRANDED_STATUS, CASCADE_STATUS)

# What this controller returns when it cannot run the pass it was asked for: a
# bad invocation, an unsafe state directory, a declaration it cannot parse.  It
# says "the operator has something to fix", never "the fleet is at zero", which
# is why a pass that dies holding capacity down has to outrank it.
FATAL_STATUS = 2

# Everything a pass can die of that is this controller's to report rather than
# raise: the mutation authority lost, the state directory or declaration
# unreadable, a fleet helper gone.  ``json.JSONDecodeError`` is a ``ValueError``
# and is named for the reader, not the matcher.
PASS_FAILURES = (
    fml.MutationLockError,
    OSError,
    TypeError,
    ValueError,
    json.JSONDecodeError,
    fm.FleetError,
)
# Every verb that can move a host's capacity.  A failure that issued none of
# them cannot have stranded the host, whatever else went wrong.
CAPACITY_MUTATION_VERBS = frozenset({"parked-apply", "quarantine", "restore", "activate"})
# The verbs that put capacity back into service.  Both are followed by a
# verifying check, and every path that fails one of those checks drains the
# host afterwards, so a transaction whose LAST capacity verb reopened capacity
# ended with the host serving and proven (issue #888).
CAPACITY_REOPEN_VERBS = frozenset({"restore", "activate"})
# ``cmd_restore`` refuses outright when the durable maintenance marker is
# absent, and that refusal is the one that means the park is no longer held:
# somebody else already lifted it and this controller's record is stale.
PARK_MARKER_ABSENT_RE = re.compile(r"cannot restore without a durable maintenance marker")
SHA256_RE = re.compile(r"[0-9a-f]{64}")
ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")


RECAP_RE = re.compile(
    r"^\s*([A-Za-z0-9_.-]+)\s+:\s+ok=(\d+)\s+changed=(\d+)\s+"
    r"unreachable=(\d+)\s+failed=(\d+)\s+skipped=(\d+)\s+"
    r"rescued=(\d+)\s+ignored=(\d+)\s*$"
)


# `capacity-restore` converges live admission to the host's CURRENT window
# target and says which one: the declared instance count on a host with no
# quiet-hours block, and the quiet-hours count inside a declared window, which
# infra/fleet.yml sets to ZERO instances for win-ci.  A restore that converged
# admission to zero therefore returns cleanly with no runner capacity in
# service at all, and a live scale is temporary by construction, so the
# declaration knows nothing about it and a check run after it verifies.
RESTORE_TARGET_RE = re.compile(r"restoring current window target (\d+)\b")


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
    """Check declared ARC authority while its rendered live ceiling stays zero.

    The activation check is a ``--check`` run, so the verifier reports work
    still outstanding by exit status rather than by a recap row:
    ``fw.APPLY_REQUIRED_STATUS`` means the check RAN and found one change, which
    is why ``inspect_host`` counts it as a clean read of one drifted task.  Read
    as a failed check instead, it made the sole ARC capacity opener unusable on
    exactly the host that expects an outstanding change: an ARC producer is
    checked against ``PRODUCER_HELD_CHECK_NOISE`` while admission is held at
    zero, so the status that reports that one change quarantined the host, the
    apply failed, and the pass recorded it at zero.  Every later pass repeated
    it, and the recovery hook reopens through this same activation sequence, so
    nothing could lift the host off zero: an ARC scale set still declared for
    all its runners with none of them in service, drained again every pass and
    escalating for ever, plus consumers released onto its frozen image once the
    record crossed ``PRODUCER_BLOCK_PASSES`` (issue #888, and the dry-run
    evidence in it).  Any other non-zero status is a genuine failure and still
    holds the host at zero.
    """
    result = run(fleet_command(host, "activation-check"))
    emit_result(result)
    if result.status == fw.APPLY_REQUIRED_STATUS:
        return True, 1
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


def restored_admission(output: str) -> int | None:
    """Return the live admission target one capacity restore reported converging to.

    ``None`` when the restore said nothing about it, which is what an older
    host-local copy of the capacity script does: nothing is then claimed in
    either direction and the caller's existing evidence stands alone.
    """
    targets = RESTORE_TARGET_RE.findall(ANSI_RE.sub("", output))
    return int(targets[-1]) if targets else None


def restore_admission(host: str, result: frp.CommandResult) -> int | None:
    """Read what a restore put back in service, saying so when it put nothing.

    The controller took a clean ``capacity-restore`` as proof that a host is
    serving again, and on the docker arm confirmed it with an ordinary check.
    That check reads the DECLARATION, and a live capacity change is temporary
    by construction, so the declaration is converged either way and the check
    verifies whatever admission the host actually holds.  A restore converges
    admission to the host's current window target, which is zero inside a
    declared quiet-hours window, so a pass could restore a host to ZERO
    instances and then publish a receipt stamped ``full_applied_at``, clear the
    host's stranded-at-zero record as "serving again", and exit 0.  That is
    issue #888's own dry-run evidence, a fleet still DECLARED for its runners
    while it serves none of them, and the restore had already printed the
    number that said so.
    """
    target = restored_admission(result.stdout)
    if target != 0:
        return target
    print(
        f"fleet-reconcile: WARNING: {host}: capacity-restore converged live admission "
        "to its CURRENT window target of ZERO instances; the declaration is converged "
        "and NO runner capacity is in service, whatever this fleet declares for it",
        file=sys.stderr,
    )
    return 0


def report_reopen_without_capacity(host: str) -> None:
    """Refuse to call a reopen that put no capacity back in service a recovery.

    Reopening last-known-good capacity exists to undo a fail-closed drain that
    cost the fleet healthy capacity for a fault that never touched the host, so
    a reopen whose own target was zero recovered nothing: the host stays at
    zero admission and has to be RECORDED there, or the controller escalates
    nothing however long it sits (issue #888).  It is deliberately not drained
    again on the way out: the host already holds zero admission, and a durable
    maintenance park would also defeat the host-local window timer that raises
    it when the window ends.
    """
    print(
        f"fleet-reconcile: WARNING: {host}: last-known-good capacity was NOT reopened "
        "(the restore's own window target was zero instances), so this host is left "
        "recorded at ZERO capacity rather than reported as serving; it is not drained "
        "again, because a durable maintenance park would also hold it down past the "
        "window that would otherwise raise it",
        file=sys.stderr,
    )


def recover_last_known_good(
    data: dict[str, Any],
    host: str,
    run: CommandRunner,
    expected_check_changes: int,
    *,
    held_check_changes: int,
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
    if not reopen_capacity(
        data, host, run, serving_changes=expected_check_changes, held_changes=held_check_changes
    ):
        return False
    print(
        f"fleet-reconcile: WARNING: {host}: full verification FAILED while serving "
        "last-known-good capacity; this pass fails and retries on the next one",
        file=sys.stderr,
    )
    return True


def _activate_arc(
    data: dict[str, Any],
    host: str,
    run: CommandRunner,
    expected_changes: int,
    *,
    require_admission: bool = False,
) -> tuple[bool, int]:
    """Validate declared ARC authority at zero before the sole capacity opener.

    ``require_admission`` is set by a RECOVERY reopen, whose whole claim is
    that capacity is back in service: a restore that converged admission to
    zero has not recovered anything and must not be reported as though it had.
    An ordinary apply leaves it unset, because converging the declaration is
    what that transaction claims and a zero window target is the operator's own
    policy rather than a failure to converge.
    """
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
    if require_admission and restore_admission(host, restore) == 0:
        report_reopen_without_capacity(host)
        return False, 0
    return True, 0


def capacity_opener(data: dict[str, Any], host: str) -> str:
    """Return the capacity arm that can put one host's declared runners back in service."""
    return fm.CLASSES[data["hosts"][host]["class"]].capacity_kind


def reopen_capacity(
    data: dict[str, Any],
    host: str,
    run: CommandRunner,
    *,
    serving_changes: int,
    held_changes: int,
) -> bool:
    """Put a drained host back in service through the opener its class actually has.

    Recovery reopened every host with a bare ``restore``, which is the whole
    opener for a Docker host and only the last step of one for an ARC scale
    set: ``_activate_arc`` exists because ARC capacity is opened by declaring
    its authority, proving that declaration while admission is still held at
    zero, and only then restoring.  A parked apply tears that declaration down,
    so a ``restore`` issued on its own returns cleanly while the live ceiling
    stays at ZERO, and the ordinary check that followed it reads the
    DECLARATION rather than the live scale, so it verified.  The controller
    then reported the host as serving last-known-good capacity, wrote no
    stranded-at-zero record, cleared any record it already had, and failed the
    pass exactly like a one-off fault: an ARC host still declared for all its
    runners with none of them in service, and no escalation however long it
    stays that way (issue #888, and the dry-run evidence in it).  Reopen
    through the class's real opener, or hold the host at zero.
    """
    if capacity_opener(data, host) == "k8s":
        opened, _ = _activate_arc(data, host, run, held_changes, require_admission=True)
        return opened
    restore = run(fleet_command(host, "restore"))
    emit_result(restore)
    if restore.status or frp.interrupted_status():
        quarantine(host, run)
        return False
    if restore_admission(host, restore) == 0:
        report_reopen_without_capacity(host)
        return False
    clean, changed = inspect_host(data, host, run)
    if not clean or changed != serving_changes or frp.interrupted_status():
        print(
            f"fleet-reconcile: {host}: last-known-good capacity did not verify "
            f"(remaining changed={changed}); holding at zero",
            file=sys.stderr,
        )
        quarantine(host, run)
        return False
    return True


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
    restore_admission(host, restore)
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
        recovered = recover_last_known_good(
            data, host, run, expected_changes, held_check_changes=held_changes
        )
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


def capacity_reopened(verb: str) -> bool:
    """Return whether the last capacity verb a transaction issued put the host in service."""
    return verb in CAPACITY_REOPEN_VERBS


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


def age_stranding(stranding: dict[str, dict[str, int]], host: str, now: int, reason: str) -> bool:
    """Count one more pass that ended with an already-drained host still at zero.

    ``record_stranding`` only counts a pass that LOST capacity, which is the
    right guard on the claim that this pass drained a host (issue #888, the
    forged stranding record).  The record itself makes a longer-lived claim:
    that a host has been held at zero for so many consecutive passes that this
    fleet is not recovering on its own, which is what ``stranded_escalations``
    turns into a CRITICAL and ``STRANDED_STATUS``.  A pass that reaches a host
    already at zero and fails to lift it off zero without mutating anything
    therefore still has to age the record: a read-only check that keeps failing
    against a drained host, or a consumer skipped behind a failed producer,
    otherwise froze the counter wherever the last capacity-losing pass left it.
    Below the escalation threshold that is silent stranding for good, which is
    exactly the shape issue #888 went unnoticed in five times: capacity at zero,
    no recovery, and an exit status that reads like any one-off failure.
    """
    entry = stranding.get(host)
    if entry is None:
        return False
    entry["passes"] += 1
    print(
        f"fleet-reconcile: WARNING: {host}: STILL at ZERO capacity after this pass "
        f"({reason}); nothing this pass did lifted it off zero (consecutive "
        f"passes={entry['passes']}, {now - entry['since']}s since it was first drained)",
        file=sys.stderr,
    )
    return True


def clear_stranding(stranding: dict[str, dict[str, int]], host: str) -> bool:
    """Forget a host's stranding, reporting whether it was held at zero before.

    The answer tells one ordinary successful pass from a host climbing back off
    zero capacity, which is worth saying out loud.  It deliberately no longer
    decides whether the provisional receipts earned against this host's frozen
    image expire: a record can also be dropped by a pass that merely REOPENED
    last-known-good capacity, and gating the expiry on this answer meant the
    producer's eventual republish expired nothing at all (issue #888).
    """
    if stranding.pop(host, None) is None:
        return False
    print(
        f"fleet-reconcile: {host}: reconciled and serving again; clearing its "
        "stranded-at-zero record",
        file=sys.stderr,
    )
    return True


def clear_reopened_stranding(stranding: dict[str, dict[str, int]], host: str) -> bool:
    """Forget a stranded-at-zero record for a host this pass put back into service.

    A failed pass can still end with capacity REOPENED and verified: an apply
    that fails with no prior drift leaves the host running the declaration it
    was already converged on, so its last-known-good capacity is restored and
    proven instead of held at zero.  That pass fails, and every failure that
    cost no capacity aged the stranded-at-zero record, so a host drained once
    and then restored on every pass after it kept counting passes it spent
    SERVING.  Three of them and the controller escalated CRITICAL and
    ``STRANDED_STATUS`` over a host carrying work, which spends the loudness
    issue #888 exists to buy on a false alarm.  The stale record is also read
    as proof of capacity nobody has: its consumers earn provisional receipts
    against an image that is being served, and the pass drain budget treats a
    serving host as having nothing left to lose, so the next pass may take it
    to zero for free.  Only the record is cleared here: the pass still fails
    and the receipt is still gone, so the host converges again next pass.
    """
    entry = stranding.get(host)
    if entry is None:
        return False
    print(
        f"fleet-reconcile: WARNING: {host}: capacity was REOPENED and verified by this "
        f"pass after {entry['passes']} pass(es) recorded at ZERO; this pass still fails, "
        "but the host is serving last-known-good capacity, so it is no longer recorded "
        "at zero",
        file=sys.stderr,
    )
    del stranding[host]
    return True


def load_parked(document: dict[str, Any]) -> dict[str, dict[str, int]]:
    """Return the record of hosts this controller left holding a durable maintenance park.

    ``capacity-quarantine`` writes the host's durable maintenance marker BEFORE
    it drains (``park_maintenance`` is ``set_maintenance`` and only then the
    drain), so a drain the fleet entry point REFUSES still leaves the marker
    behind.  With that marker in place ``cmd_window``, the host-local timer
    that raises admission again when a quiet window ends, refuses to lift it
    ("maintenance: forcing target 0"), and ``capacity-restore`` is the only
    thing that removes it.  A refused drain therefore does not leave a host
    serving indefinitely: it leaves it parked and heading to zero on its own
    timer, and the controller knew nothing about it (issue #888).  This is a
    SEPARATE store from the stranded-at-zero record on purpose: that record
    claims this controller took the host to zero and proved it, and writing it
    here forged exactly the proof three later policies read back.  Anything
    malformed is replaced rather than trusted, and state written by an older
    controller starts empty.
    """
    stored = document.get("parked")
    entries = stored.items() if isinstance(stored, dict) else []
    parked = {
        host: {"since": entry["since"], "passes": entry["passes"]}
        for host, entry in entries
        if isinstance(entry, dict)
        and isinstance(entry.get("since"), int)
        and isinstance(entry.get("passes"), int)
    }
    document["parked"] = parked
    return parked


def record_park(parked: dict[str, dict[str, int]] | None, host: str, now: int) -> None:
    """Remember that a refused drain left a durable maintenance park on one host."""
    if parked is None:
        return
    previous = parked.get(host)
    entry = {
        "since": previous["since"] if previous else now,
        "passes": previous["passes"] + 1 if previous else 1,
    }
    parked[host] = entry
    print(
        f"fleet-reconcile: WARNING: {host}: its drain was REFUSED, but the durable "
        "maintenance park is written before the drain is attempted, so this host is "
        "left PARKED: the host-local window timer cannot raise its admission again "
        "and only a capacity restore clears it "
        f"(consecutive passes={entry['passes']}, first parked at {entry['since']})",
        file=sys.stderr,
    )


def clear_park(parked: dict[str, dict[str, int]] | None, host: str) -> bool:
    """Forget a durable park this pass proved closed by reopening the host.

    ``capacity-restore`` removes the maintenance marker as its last act, so a
    landed reopen verb is the one piece of evidence that the park is gone.
    """
    if parked is None or parked.pop(host, None) is None:
        return False
    print(
        f"fleet-reconcile: {host}: capacity was reopened, which clears its durable "
        "maintenance park; dropping the parked record",
        file=sys.stderr,
    )
    return True


def open_parked(
    document: dict[str, Any], hosts: Sequence[str], options: ReconcileOptions
) -> dict[str, dict[str, int]]:
    """Return the parked record this pass may reason about, one pass older.

    A park nothing has cleared outlives the pass that left it, so an apply pass
    counts the interval it is starting against every host still recorded.  A
    host outside the declaration is dropped for the same reason a stranded
    record is: this controller cannot restore what it does not manage, and a
    verdict that never goes back to zero is how loudness turns into noise.
    Check mode persists nothing, so it reads the record exactly as it stands.
    """
    parked = load_parked(document)
    if options.mode != "apply":
        return parked
    managed = set(hosts)
    for host in sorted(parked):
        if host not in managed:
            entry = parked.pop(host)
            print(
                f"fleet-reconcile: WARNING: {host} is no longer a capacity-managed host "
                f"in this fleet; dropping its parked record (consecutive passes="
                f"{entry['passes']}). If it is still deployed it may still hold a durable "
                "maintenance park: that is now the operator's to clear",
                file=sys.stderr,
            )
        else:
            parked[host]["passes"] += 1
    return parked


def report_durable_park(parked: dict[str, dict[str, int]], now: int) -> list[str]:
    """Name the hosts this controller holds parked but is not escalating yet, in either mode.

    The refused drain earns ``DRAIN_FAILED_STATUS`` on the pass it happens, and
    then nothing said anything at all: the host sits parked at zero admission
    while later passes report the declaration converged.  Saying it every pass
    is what separates a fleet that is fine from one this controller has left
    parked, and a read-only dry run has to say it too.  Past
    ``PARK_ESCALATION_PASSES`` the same host is named by
    ``parked_escalations`` instead, which is louder and carries a verdict, so
    this stays the below-threshold half exactly as ``report_recorded_zero`` is
    for the stranded-at-zero record.
    """
    held = [
        host for host, entry in sorted(parked.items()) if entry["passes"] < PARK_ESCALATION_PASSES
    ]
    for host in held:
        entry = parked[host]
        print(
            f"fleet-reconcile: WARNING: {host} still holds a durable maintenance park "
            f"this controller has not proven closed ({entry['passes']} pass(es), "
            f"{now - entry['since']}s since its drain was refused); its admission "
            "cannot come back until a capacity restore clears that park",
            file=sys.stderr,
        )
    return held


def parked_escalations(parked: dict[str, dict[str, int]], now: int) -> list[str]:
    """Name every host a durable park keeps at zero that this controller cannot lift.

    The park record was created to stop a refused drain from forging a
    stranded-at-zero record, and it does claim less: only that the drain wrote
    the host's durable maintenance marker before it was refused.  What it
    claims is still that the host is pinned at ZERO admission, because
    ``cmd_window`` refuses to raise admission while that marker is there and
    ``capacity-restore`` is the only thing that removes it.

    Nothing escalated it.  The restore that lifts a park is only issued for a
    host whose own declaration verified this pass, which is deliberately narrow
    and leaves the two everyday cases unreachable: a parked host whose
    read-only check keeps failing is never applied, and a parked consumer
    behind a failed producer is held before anything touches it.  Either way
    the record aged, printed one WARNING, and the pass exited 1 like any
    one-off failure, for as many passes as the fault lasted.  A fleet at zero
    that reads like a transient failure is issue #888 itself, so a park this
    controller has not been able to prove closed across
    ``PARK_ESCALATION_PASSES`` consecutive passes earns the same CRITICAL and
    ``STRANDED_STATUS`` a host held at zero by this controller's own drain does.
    """
    escalated: list[str] = []
    for host, entry in sorted(parked.items()):
        if entry["passes"] < PARK_ESCALATION_PASSES:
            continue
        escalated.append(host)
        print(
            f"fleet-reconcile: CRITICAL: {host} has held a durable maintenance park "
            f"across {entry['passes']} consecutive reconcile passes "
            f"({now - entry['since']}s since its drain was refused); its admission "
            "cannot come back until a capacity restore clears that park, and nothing "
            "this controller has done since has lifted it, so this fleet is not "
            "recovering on its own and needs operator intervention",
            file=sys.stderr,
        )
    return escalated


def park_marker_absent(output: str) -> bool:
    """Report whether a refused restore says the durable park was already lifted.

    ``cmd_restore`` refuses before it touches admission when the maintenance
    marker is absent, so that one refusal is evidence ABOUT the park rather
    than a failure to lift it: an operator who ran the restore themselves
    removed the marker, and the record kept here is simply stale.  Reading it
    is what keeps a park somebody else lifted from failing every later pass
    over a host that is fine.
    """
    return bool(PARK_MARKER_ABSENT_RE.search(ANSI_RE.sub("", output)))


def report_park_release_refused(host: str, status: int, entry: dict[str, int], now: int) -> None:
    """Name a durable park this controller issued the restore for and could not lift."""
    print(
        f"fleet-reconcile: CRITICAL: {host}: the capacity restore that lifts its durable "
        f"maintenance park was REFUSED (rc={status}); the host stays pinned at ZERO "
        "admission, its own window timer cannot raise it, and this controller has now "
        "proven it cannot reopen the host by itself "
        f"({entry['passes']} pass(es), {now - entry['since']}s parked)",
        file=sys.stderr,
    )


def park_release_changes(data: dict[str, Any], host: str) -> int:
    """Return the drift one parked host's held check reports while it is converged.

    The ARC opener proves the declaration while admission is still held at
    zero, and an ARC producer reports ``PRODUCER_HELD_CHECK_NOISE`` in that
    state where every other host reports none: the same expectation
    ``reconcile_host`` hands ``apply_host`` and the recovery hook.
    """
    producer = host == data["runner_image"]["source_host"]
    if not producer:
        return 0
    if capacity_opener(data, host) == "k8s":
        return PRODUCER_HELD_CHECK_NOISE
    return PRODUCER_CHECK_NOISE


def reopen_parked_arc(
    data: dict[str, Any], host: str, run: CommandRunner
) -> frp.CommandResult | None:
    """Re-declare ARC authority before the restore that lifts a durable park.

    Returns the restore's own result so the caller can read what it put back in
    service, or ``None`` when the declaration itself never verified and no
    restore was issued.  Unlike ``_activate_arc`` this never quarantines: the
    host already holds zero admission behind its maintenance marker, so
    draining it again buys nothing and a park is deliberately not drained on
    the way out.
    """
    activation = run(fleet_command(host, "activate"))
    emit_result(activation)
    if activation.status or frp.interrupted_status():
        return None
    clean, changed = inspect_activation_host(data, host, run)
    if not clean or changed != park_release_changes(data, host) or frp.interrupted_status():
        return None
    result = run(fleet_command(host, "restore"))
    emit_result(result)
    return result


def report_park_declaration_failed(host: str, entry: dict[str, int], now: int) -> None:
    """Name a parked ARC host whose authority would not re-declare."""
    print(
        f"fleet-reconcile: CRITICAL: {host}: its ARC authority did not re-declare, so the "
        "capacity restore that lifts its durable maintenance park was never issued; the "
        "host stays pinned at ZERO admission and this controller cannot reopen it by "
        f"itself ({entry['passes']} pass(es), {now - entry['since']}s parked)",
        file=sys.stderr,
    )


def open_parked_capacity(
    data: dict[str, Any], host: str, run: CommandRunner
) -> frp.CommandResult | None:
    """Issue the park-lifting restore through the opener this host's class has.

    A bare ``capacity-restore`` is the whole opener for a Docker host and only
    the last step of one for an ARC scale set.  ``_activate_arc`` exists
    because ARC capacity is opened by declaring its authority, proving that
    declaration while admission is held at zero, and only then restoring; a
    parked apply tears that declaration down, so a restore issued on its own
    returns cleanly while the live ceiling stays at ZERO.  This release path
    issued that bare restore for every class, so a parked ARC host had its park
    record CLEARED on a clean return with nothing put back in service, and once
    the record is gone nothing escalates the host, nothing counts its capacity
    as forfeit, and the next pass budgets it as though it were serving: an ARC
    scale set declared for all its runners with none of them in service, and
    issue #888's own headline arriving through the newest record.
    """
    if capacity_opener(data, host) == "k8s":
        return reopen_parked_arc(data, host, run)
    result = run(fleet_command(host, "restore"))
    emit_result(result)
    return result


def release_durable_park(  # the host plus the fleet its opener comes from
    data: dict[str, Any],
    host: str,
    run: CommandRunner,
    parked: dict[str, dict[str, int]],
    now: int,
) -> bool:
    """Issue the one verb that lifts a durable maintenance park, and prove it landed.

    A host this controller left parked is pinned at zero admission by its own
    marker: ``cmd_window``, the host-local timer, refuses to raise admission
    while it is there, and ``capacity-restore`` is the only thing that removes
    it.  The parked record was report-only and NOTHING ever issued that
    restore, so a later pass whose check came back CLEAN mutated nothing at
    all: it published a receipt, cleared no park, and exited 0 with the host
    held at zero for as long as that receipt stayed valid, which is a whole
    full-apply interval (seven days in production).  A fleet that cannot climb
    back out on its own is issue #888 itself, and this is it arriving through
    the newest record.  Reopening a host whose declaration verified this pass
    is exactly what ``recover_last_known_good`` already does after a failed
    apply, one pass later and with the same evidence.
    """
    entry = parked[host]
    print(
        f"fleet-reconcile: {host}: its declaration reconciled this pass while it still "
        f"holds a durable maintenance park ({entry['passes']} pass(es), "
        f"{now - entry['since']}s); issuing the capacity restore that lifts it",
        file=sys.stderr,
    )
    result = open_parked_capacity(data, host, run)
    if result is None:
        report_park_declaration_failed(host, entry, now)
        return False
    if not result.status:
        restore_admission(host, result)
        clear_park(parked, host)
        return True
    if park_marker_absent(f"{result.stdout}\n{result.stderr}"):
        print(
            f"fleet-reconcile: WARNING: {host}: the restore reports no durable maintenance "
            "marker, so this park was lifted outside this controller; dropping the record",
            file=sys.stderr,
        )
        clear_park(parked, host)
        return True
    report_park_release_refused(host, result.status, entry, now)
    return False


def reconciled_this_pass(receipts: dict[str, Any], host: str, now: int) -> bool:
    """Report whether THIS pass published a receipt for one host.

    ``invalidate_receipt`` drops a failed host's receipt and ``record_success``
    stamps a published one with this pass's clock, so the receipt is the
    evidence that this host's declaration verified just now.  Anything older
    belongs to an earlier pass and proves nothing about this one.
    """
    receipt = receipts.get(host)
    return isinstance(receipt, dict) and receipt.get("checked_at") == now


def release_durable_parks(  # noqa: PLR0913  # the pass's records plus the fleet it reopens in
    data: dict[str, Any],
    receipts: dict[str, Any],
    parked: dict[str, dict[str, int]],
    order: Sequence[str],
    options: ReconcileOptions,
    run: CommandRunner,
) -> list[str]:
    """Lift the park on every host this pass reconciled, naming what stayed held.

    Narrow on purpose.  Only a host whose own declaration verified THIS pass is
    reopened, so nothing is put back in service while a mutation is in flight
    or after an apply that failed; only hosts this fleet still manages are
    touched, because this controller cannot restore what it does not declare;
    and a read-only pass issues nothing at all, since the restore is a
    mutation and check mode makes none.
    """
    if options.mode != "apply":
        return []
    managed = set(order)
    return [
        host
        for host in sorted(parked)
        if host in managed
        and reconciled_this_pass(receipts, host, options.now)
        and not release_durable_park(data, host, run, parked, options.now)
    ]


def settle_pass_records(  # noqa: PLR0913  # the pass's records plus what it takes to settle them
    data: dict[str, Any],
    document: dict[str, Any],
    state_path: Path,
    stranding: dict[str, dict[str, int]],
    parked: dict[str, dict[str, int]],
    order: Sequence[str],
    options: ReconcileOptions,
    run: CommandRunner,
) -> list[str]:
    """Settle this pass's records after its hosts, then read them out loud.

    The park release runs here, after the per-host loop, because it is the one
    mutation a pass makes on behalf of a host it never had to touch, and
    persisting it is this function's own job.  A park this controller issued
    the restore for and could NOT lift escalates alongside the
    stranded-at-zero records: the host is held at zero, and a pass that has
    proven it cannot reopen the host by itself must not report that as an
    ordinary one-off failure.
    """
    refused = release_durable_parks(data, document["hosts"], parked, order, options, run)
    if options.mode == "apply":
        save_state(state_path, document)
    return sorted({*pass_escalations(stranding, order, options, parked), *refused})


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


def open_pass_records(
    document: dict[str, Any], order: Sequence[str], options: ReconcileOptions
) -> tuple[dict[str, dict[str, int]], dict[str, dict[str, int]]]:
    """Return the stranded-at-zero and parked records, dropping receipts that prove nothing.

    Every read settles what this pass is entitled to believe from the state
    file before it inspects a single host: a provisional receipt earned against
    a producer that no longer publishes this fleet's image, a stranded-at-zero
    record for a host this fleet no longer manages, and the durable maintenance
    parks a refused drain left behind.
    """
    expire_orphaned_releases(document["hosts"], order, options)
    return open_stranding(document, order, options), open_parked(document, order, options)


def invalidate_receipt(  # noqa: PLR0913  # the receipt plus every record one failure settles
    receipts: dict[str, Any],
    stranding: dict[str, dict[str, int]],
    drained: list[str],
    host: str,
    now: int,
    *,
    stranded: bool,
    at_zero: bool = True,
    reopened: bool = False,
    parked: dict[str, dict[str, int]] | None = None,
) -> None:
    """Drop a failed host's receipt, counting a pass that left it at zero.

    ``drained`` collects only the hosts THIS pass took from serving to zero,
    which is what the pass's drain budget is spent on.  Two failures record a
    stranding without spending it.  A host already carrying a record had no
    capacity left to lose, so draining it again costs the fleet nothing: the
    producer the consumers were released past (``PRODUCER_BLOCK_PASSES``) is
    drained again every pass and would otherwise halt the pass that finally
    repairs them.  And a pass that ended by REOPENING this host's capacity has
    proven it serving, so it clears the record instead of counting another pass
    at zero against a host that is carrying work.

    ``at_zero`` is false when the host's drain was REFUSED, and that host earns
    no zero-capacity record at all: the controller could not take it out of
    service, so nothing proved it left it.  Recording it forged exactly the
    proof three later policies read off that record (issue #888).  It is not
    nothing, though: the drain wrote the host's DURABLE maintenance park before
    it was refused, so the host is parked and only a capacity restore can lift
    it.  That goes in its own record, which claims only what this pass proved.
    """
    receipts.pop(host, None)
    if reopened:
        clear_park(parked, host)
    if not stranded:
        if reopened and clear_reopened_stranding(stranding, host):
            return
        age_stranding(stranding, host, now, "this pass failed without taking capacity down")
        return
    if not at_zero:
        if not reopened:
            record_park(parked, host, now)
        report_unaccounted_capacity(stranding, host)
        return
    newly_drained = host not in stranding
    record_stranding(stranding, host, now)
    if newly_drained:
        drained.append(host)


def report_unaccounted_capacity(stranding: dict[str, dict[str, int]], host: str) -> None:
    """Refuse to record zero capacity for a host this pass could not drain.

    Every failure path ends in a drain, and a drain the fleet entry point
    REFUSES leaves the host unaccounted for: it may well still be serving work
    against a mutation that stopped halfway, which is why it earns the loudest
    verdict this controller has (``DRAIN_FAILED_STATUS``).  The stranded-at-zero
    record makes the opposite claim, that this controller took the host to zero
    and never reopened it, and writing it on a refused drain forged that proof
    for three separate policies that later read it back with no idea where it
    came from (issue #888):

    * ``frozen_image_release`` reads a producer's record as proof that it
      publishes nothing, so a producer nobody could drain released every
      consumer onto its "frozen" image with a PROVISIONAL receipt, exactly the
      host most likely to be mid-republish after a half-finished mutation;
    * ``consumers_released`` spends ``PRODUCER_BLOCK_PASSES`` on those passes,
      so the first pass that genuinely drained the producer released the
      consumers immediately instead of holding them for the delay that exists
      to keep them off an image in flight;
    * ``serving_hosts`` and ``consumer_held`` treat a recorded host as having
      no capacity left to lose, so the pass drain budget neither counted nor
      protected a host that was still carrying work.

    The record is neither created nor advanced here: this pass proved nothing
    about what the host serves, and an existing record from a pass that really
    did drain it stands exactly as it was.
    """
    entry = stranding.get(host)
    standing = (
        f"its earlier record of {entry['passes']} pass(es) at zero stands unchanged"
        if entry
        else "no zero-capacity record is written against it"
    )
    print(
        f"fleet-reconcile: WARNING: {host}: this pass could not drain it, so nothing "
        f"proved it stopped serving; {standing}, and the host stays unaccounted for "
        f"(status {DRAIN_FAILED_STATUS}), which is louder than being known to sit at zero",
        file=sys.stderr,
    )


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


def expire_orphaned_releases(
    receipts: dict[str, Any], order: Sequence[str], options: ReconcileOptions
) -> list[str]:
    """Drop provisional receipts earned against a producer this fleet no longer publishes from.

    A receipt marked ``RELEASED_RECEIPT_KEY`` records convergence onto one
    named producer's FROZEN last-known-good image, and the only thing that ever
    expired it was that same producer being proven to serve again.  A producer
    can also leave the role: an operator swapping ``runner_image.source_host``
    to a healthy host, or retiring a producer that has sat at zero for days,
    is the ordinary human response to the outage this controller is escalating.
    Nothing then clears the mark, because the host whose recovery the expiry
    waits on is never the producer again, so every released consumer keeps a
    receipt that looks freshly converged and ``full_apply_due`` skips it for a
    whole interval while the pass exits 0.  The new producer's image never
    reaches the fleet and the controller reports it fully converged running the
    image the outage left behind (issue #888, the same stale pinning as the
    recovery path, arriving through the fix rather than the fault).  A mark
    naming anyone but the current producer is evidence about an image source
    this pass does not have, so it is dropped and the host converges again in
    this same pass.
    """
    if options.mode != "apply":
        return []
    producer = order[0]
    orphaned = [
        host
        for host in order
        if isinstance(receipts.get(host), dict)
        and RELEASED_RECEIPT_KEY in receipts[host]
        and receipts[host][RELEASED_RECEIPT_KEY] != producer
    ]
    for host in orphaned:
        against = receipts.pop(host)[RELEASED_RECEIPT_KEY]
        print(
            f"fleet-reconcile: WARNING: {host}: its receipt was earned against {against}'s "
            f"frozen image, and {producer} publishes this fleet's image now; that receipt "
            "proves nothing about the image in service, so it is expired and the host "
            "converges again in this pass",
            file=sys.stderr,
        )
    return orphaned


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


def managed_stranding(
    stranding: dict[str, dict[str, int]], hosts: Sequence[str]
) -> dict[str, dict[str, int]]:
    """Return the stranded-at-zero records for hosts this declaration still manages."""
    managed = set(hosts)
    return {host: entry for host, entry in stranding.items() if host in managed}


def report_recorded_zero(stranding: dict[str, dict[str, int]], now: int) -> list[str]:
    """Name the hosts this controller is holding at zero but not escalating yet.

    An apply pass says this for every recorded host as it goes, because it is
    the pass that records or ages the entry.  A read-only pass writes nothing,
    so without this it said nothing at all about a host below the escalation
    threshold: the record is the only state that tells a fleet that is fine
    from one this controller has already taken to zero.
    """
    held = [
        host
        for host, entry in sorted(stranding.items())
        if entry["passes"] < STRANDED_ESCALATION_PASSES
    ]
    for host in held:
        entry = stranding[host]
        print(
            f"fleet-reconcile: WARNING: {host} is recorded at ZERO capacity by this "
            f"controller ({entry['passes']} consecutive pass(es), "
            f"{now - entry['since']}s since it was first drained); whatever this fleet "
            "declares for it, that capacity is not in service",
            file=sys.stderr,
        )
    return held


def pass_escalations(
    stranding: dict[str, dict[str, int]],
    order: Sequence[str],
    options: ReconcileOptions,
    parked: dict[str, dict[str, int]] | None = None,
) -> list[str]:
    """Read both zero-capacity records out loud, whichever mode this pass ran in.

    The record was only ever read out by an apply pass, so ``--mode check``,
    the dry run an operator reaches for to ask what the fleet looks like,
    reported the declaration and nothing else: it never said that this
    controller had been holding hosts at zero, and it exited 0 while they sat
    there.  That is the diagnostic in issue #888's own evidence, where a later
    dry run showed ARC still declared for six and TrueNAS for one while both
    had been drained for days.  Draining a host is an override the declaration
    knows nothing about, so a parked host's read-only check can come back
    CURRENT and the record is the only thing that knows better.  Reporting it
    persists nothing, so a check pass can say it and earn the same verdict an
    apply pass would.

    The durable park is the other way a host sits at zero, so it is read out
    here too and escalates on the same clock: a park this controller has not
    proven closed for ``PARK_ESCALATION_PASSES`` passes is a fleet that is not
    recovering on its own, however ordinary each single pass's exit status
    looked.

    Scoped to the hosts in this pass's declaration on purpose, for both
    records: pruning records for hosts the fleet no longer manages is a write
    an apply pass makes, and a check pass must not turn one retired host's
    stale record into a permanently red dry run instead.
    """
    managed = managed_stranding(stranding, order)
    held_parked = managed_stranding(parked or {}, order)
    report_durable_park(held_parked, options.now)
    if options.mode != "apply":
        report_recorded_zero(managed, options.now)
    return sorted(
        {
            *stranded_escalations(managed, options.now),
            *parked_escalations(held_parked, options.now),
        }
    )


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


def frozen_image_release(
    stranding: dict[str, dict[str, int]], host: str, *, undrained: bool
) -> bool:
    """Return whether a producer that lost no capacity this pass still has a frozen image.

    A producer whose failure cost no capacity does not block its consumers:
    nothing was mutated, so the image they depend on did not move and holding
    them back would strand them for a fault that touched nothing (issue #888).
    That is right, and it said nothing at all about whether the producer is
    SERVING.  A producer an earlier pass drained carries a stranded-at-zero
    record and publishes nothing, so a read-only check that fails against it,
    the everyday shape of a host that is already down, released every consumer
    onto its FROZEN last-known-good image with an ORDINARY receipt stamped
    ``full_applied_at``.  Nothing then marked those consumers as provisional,
    so the producer's eventual recovery expired nothing, ``full_apply_due``
    skipped them for a whole interval and the pass exited 0 with the fleet
    reported converged on the image the outage left behind.  That is the same
    stale pinning the deliberate release is marked to avoid, reached without
    ever crossing ``PRODUCER_BLOCK_PASSES``.  A drain that was REFUSED proves
    nothing about what the host serves, so it never counts as frozen.
    """
    if undrained or host not in stranding:
        return False
    entry = stranding[host]
    print(
        f"fleet-reconcile: WARNING: producer {host} has been recorded at ZERO capacity "
        f"for {entry['passes']} consecutive pass(es), so it publishes nothing; the "
        "consumers reconciling past it earn PROVISIONAL receipts against its frozen "
        "last-known-good image",
        file=sys.stderr,
    )
    return True


def producer_block_state(
    stranding: dict[str, dict[str, int]], host: str, *, stranded: bool, undrained: bool
) -> tuple[bool, bool]:
    """Return whether a failed producer blocks its consumers, and whether it released them.

    Released is not merely the opposite of blocking: a producer that failed
    without losing capacity never blocked anyone, and while it is still serving
    its image is not frozen, so a consumer reconciling past it earns an
    ordinary receipt.  Convergence against an image no producer is publishing
    is what makes a receipt provisional, whether a deliberate release or a
    producer already sitting at zero let the consumer through.
    """
    if not stranded:
        print(
            f"fleet-reconcile: WARNING: producer {host} FAILED without "
            "losing capacity; consumers continue against last-known-good",
            file=sys.stderr,
        )
        return False, frozen_image_release(stranding, host, undrained=undrained)
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
    reopened: bool = False,
    parked: dict[str, dict[str, int]] | None = None,
) -> None:
    """Publish one reconciled host's receipt and settle the frozen-image records.

    A producer that reconciles has published, so every provisional receipt
    earned against its FROZEN last-known-good image is now evidence about an
    image nobody serves and has to expire.  That used to be gated on this pass
    clearing the producer's stranded-at-zero record, on the reasoning that
    climbing back off zero is what proves a republish.  A record is also
    dropped by a pass that only REOPENED last-known-good capacity, and by one
    that finds the host already serving, so a producer whose record went that
    way republished with every release still marked against it: ``full_apply_due``
    skipped those consumers for a whole interval and the pass exited 0 with the
    fleet reported converged on the image the outage left behind (issue #888).
    The receipts name the producer they were earned against, so the marks are
    the evidence and no record has to survive for them to be found.
    """
    producer = order[0]
    receipts[host] = released_receipt(receipt, host, producer) if index and released else receipt
    if reopened:
        clear_park(parked, host)
    clear_stranding(stranding, host)
    if not index:
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


def serving_hosts(
    order: Sequence[str],
    stranding: dict[str, dict[str, int]],
    parked: dict[str, dict[str, int]] | None = None,
) -> list[str]:
    """Return the hosts this pass starts with capacity it could still lose.

    A host recorded at zero has none.  Neither has one left holding a DURABLE
    maintenance park: ``park_maintenance`` writes that marker BEFORE the drain
    that was then refused, ``cmd_window`` refuses to raise the host's admission
    while it is there, and only a capacity restore removes it, so that host's
    admission is already forfeit and this controller is the only thing that can
    give it back.
    """
    held = parked or {}
    return [host for host in order if host not in stranding and host not in held]


def report_forfeit_capacity(
    order: Sequence[str],
    serving: Sequence[str],
    stranding: dict[str, dict[str, int]],
    parked: dict[str, dict[str, int]],
    budget: int,
) -> None:
    """Say how much capacity this pass starts without, and under which record."""
    at_zero = [host for host in order if host in stranding]
    held = [host for host in order if host not in stranding and host in parked]
    accounts = []
    if at_zero:
        accounts.append(f"recorded at ZERO capacity: {', '.join(at_zero)}")
    if held:
        accounts.append(f"holding a durable maintenance park: {', '.join(held)}")
    print(
        f"fleet-reconcile: WARNING: {len(order) - len(serving)} of {len(order)} host(s) hold "
        f"no capacity this budget can protect ({'; '.join(accounts)}), so this pass may take "
        f"at most {budget} of the {len(serving)} still serving "
        f"({', '.join(serving)}) to zero",
        file=sys.stderr,
    )


def open_drain_budget(
    order: Sequence[str],
    stranding: dict[str, dict[str, int]],
    parked: dict[str, dict[str, int]] | None = None,
) -> int:
    """Return how much of the capacity STILL SERVING this pass may take to zero.

    The budget protects capacity, so it has to be measured against the capacity
    that exists, not against the declaration.  A host already recorded at zero
    is deliberately exempt from the budget, because it has nothing left to lose
    and repairing it is the recovery this controller exists for; but it was
    still counted in the fleet size the budget was derived from, so every host
    an earlier pass drained quietly raised the number of SERVING hosts this
    pass was allowed to drain after it.  A fleet already half down therefore
    handed the next pass a budget big enough to empty everything that was left,
    which is issue #888 arriving one pass at a time instead of all at once: no
    single pass looks like an evacuation, and the fleet still ends at zero.

    A DURABLE maintenance park is the other way a host holds no capacity, and
    it was counted in that fleet size too.  ``consumer_held`` exempts a parked
    host from the budget for exactly the reason a stranded one is exempt, so
    the same asymmetry came back through the newer record: a fleet whose hosts
    were parked one by one by refused drains kept handing every later pass a
    budget derived from hosts whose admission their own window timer can no
    longer raise, and the pass was allowed to drain everything still serving
    behind them.
    """
    serving = serving_hosts(order, stranding, parked)
    budget = drain_budget(len(serving))
    if len(serving) != len(order):
        report_forfeit_capacity(order, serving, stranding, parked or {}, budget)
    return budget


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


def hold_at_zero(
    stranding: dict[str, dict[str, int]], host: str, options: ReconcileOptions
) -> bool:
    """Age a skipped host's stranded-at-zero record, on a pass that persists it.

    A held host is left exactly as it is, which for a host already at zero
    means another whole pass at zero.  Check mode persists nothing, so it
    reports the record as it stands rather than counting a pass it will not
    write down.
    """
    if options.mode != "apply":
        return False
    return age_stranding(stranding, host, options.now, "held back by this pass, not inspected")


def consumer_held(  # noqa: PLR0913  # one hold decision over the whole pass's state
    host: str,
    stranding: dict[str, dict[str, int]],
    halted: list[str],
    *,
    blocking: bool,
    drained: Sequence[str],
    budget: int,
    parked: dict[str, dict[str, int]] | None = None,
) -> bool:
    """Return whether a consumer is skipped before anything touches it.

    Two holds, both leaving the host exactly as it is: the producer block, and
    the pass's drain budget.  A host that is ALREADY recorded at zero is never
    held by the budget, because it has no capacity left for the budget to
    protect and repairing it is the recovery issue #888 is about; the budget
    only stops a pass from emptying hosts that are still serving.  A host left
    holding a durable maintenance park by a refused drain is exempt for the
    same reason and needs it more: its admission cannot come back until a
    capacity restore clears that park, and this pass is the only thing that
    issues one.  The producer block is deliberately NOT waived, because a
    consumer let past a failed producer would converge onto an image that may
    be in flight.
    """
    if blocking:
        print(f"fleet-reconcile: {host}: BLOCKED by producer failure", file=sys.stderr)
        return True
    if len(drained) < budget or host in stranding or host in (parked or {}):
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


def locked_out_options(options: ReconcileOptions) -> ReconcileOptions:
    """Return this pass's policy as what a pass without the mutation lock really is.

    An apply pass names every recorded host as it goes, because it is the pass
    that records or ages the entry.  A pass that never took the lock ages
    nothing and must write nothing, so its records are read out the way a dry
    run reads them, below-threshold entries included.
    """
    return replace(options, mode="check")


def locked_out_verdict(data: dict[str, Any], options: ReconcileOptions) -> int:
    """Read out the zero-capacity records a pass that never got the lock cannot act on.

    Another controller owning the fleet mutation lock is ordinary and expected:
    an operator's own ``just infra::apply``, a capacity change by hand, or the
    previous reconcile pass still finishing.  The pass returned
    ``fml.LOCK_BUSY_STATUS`` before it read anything at all, though, and that
    status is EX_TEMPFAIL: retry later, somebody else is working.  Over a fleet
    this controller has already taken to ZERO capacity it says the wrong thing
    entirely.  Both zero-capacity records exist because a fleet held at zero has
    to get LOUDER rather than quieter, and a lock holder that is stuck, crashed,
    or merely long-running silenced them for as long as it lasts: every pass
    exited 75, said nothing whatever about the hosts sitting at zero, and
    nothing ever escalated.  A fleet at zero that reads like somebody else's
    work in progress is issue #888 itself, arriving before the pass even starts.

    Reading the records persists nothing and mutates nothing, so it needs no
    lock at all, which is exactly the reasoning that lets ``--mode check`` read
    them and earn a verdict off them.  Nothing here ages, prunes, records or
    clears an entry: the lock holder may be writing the state file underneath
    this pass, so what the records already say is read out as it stands, and
    state this pass cannot read leaves the busy lock as the only thing it can
    honestly report.
    """
    try:
        order = runner_hosts(data)
        document = load_state(options.state_dir / STATE_FILE)
        stranding = load_stranding(document)
        parked = load_parked(document)
    except (OSError, TypeError, ValueError) as error:
        print(
            "fleet-reconcile: WARNING: the zero-capacity records could not be read while "
            f"another controller holds the mutation lock ({error}); this pass can report "
            "only the busy lock, so anything already held at zero goes unreported",
            file=sys.stderr,
        )
        return fml.LOCK_BUSY_STATUS
    escalated = pass_escalations(stranding, order, locked_out_options(options), parked)
    if not escalated:
        return fml.LOCK_BUSY_STATUS
    print(
        "fleet-reconcile: CRITICAL: another controller holds the mutation lock, so this "
        "pass could not even attempt to lift the host(s) it is holding at ZERO capacity: "
        f"{', '.join(escalated)}; a lock holder that never finishes leaves this fleet at "
        "zero for as long as it lasts",
        file=sys.stderr,
    )
    return STRANDED_STATUS


def aborted_verdict(data: dict[str, Any], options: ReconcileOptions) -> int:
    """Read out the zero-capacity records a pass that died mid-transaction left behind.

    A pass can fail outright once it is under way: the state file becomes
    unreadable underneath it, a write to the state directory runs out of space,
    the mutation authority is lost rather than merely held by somebody else, a
    fleet helper the pass shells out to has gone.  Every one of those ends in
    ``FATAL`` and status 2, which in this controller's own vocabulary is what
    ``--force`` outside apply mode and a negative convergence interval return:
    the operator has asked for something impossible, go and fix the invocation.

    Over a fleet this controller has already taken to ZERO capacity it says the
    wrong thing entirely, and it is the same silence the busy lock used to
    report.  Both zero-capacity records live in the state file across passes
    precisely so a fleet held at zero gets LOUDER rather than quieter, and they
    are already on disk before this pass ever starts: a fault that keeps
    arriving at the same point therefore hid every one of them behind a
    configuration error for as long as it lasted, named no host, logged no
    CRITICAL, and escalated nothing however many passes it repeated.  A fleet
    at zero that reads like a typo in the unit file is issue #888 itself,
    arriving on the way out of a pass that did start.

    Reading the records persists nothing and mutates nothing, which is what
    lets ``--mode check`` read them and earn a verdict off them, so it is safe
    on a path where the pass has already proven it cannot be trusted to write.
    Nothing here ages, prunes, records or clears an entry: this pass failed
    partway through its own transaction, so what the records already say is
    read out as it stands, and state that cannot be read leaves the failure as
    the only thing the pass can honestly report.
    """
    try:
        order = runner_hosts(data)
        document = load_state(options.state_dir / STATE_FILE)
        stranding = load_stranding(document)
        parked = load_parked(document)
    except (OSError, TypeError, ValueError) as error:
        print(
            "fleet-reconcile: WARNING: the zero-capacity records could not be read after "
            f"this pass failed ({error}); it can report only that failure, so anything "
            "this fleet is already holding at zero goes unreported",
            file=sys.stderr,
        )
        return FATAL_STATUS
    escalated = pass_escalations(stranding, order, locked_out_options(options), parked)
    if not escalated:
        return FATAL_STATUS
    print(
        "fleet-reconcile: CRITICAL: this pass failed before it could finish, so it never "
        "attempted to lift the host(s) it is holding at ZERO capacity: "
        f"{', '.join(escalated)}; a failure that keeps arriving at the same point leaves "
        "this fleet at zero for as long as it lasts",
        file=sys.stderr,
    )
    return STRANDED_STATUS


def stopped_verdict(status: int) -> int:
    """Return what a pass cut short by an administrative stop has to report.

    A stop is ordinary: a unit restart, an operator's Ctrl-C, a runtime limit
    cutting a slow pass short, and ``128 + signal`` is the honest account of
    one, so it stays the verdict for a pass that was merely interrupted.  It is
    the WRONG account of a pass that has already found capacity at zero.  The
    controller's fail-closed reflex drains the host it was working on when a
    stop arrives, the per-host records are written before the pass ends and
    ``settle_pass_records`` still reads them out, yet every zero-capacity
    verdict those records earn was then thrown away for the signal status: a
    pass stopped at the same point on every run (a systemd runtime limit, a
    maintenance window, the locked dependency downloads in issue #888's own
    evidence timing out) reported nothing but "killed by TERM" while the fleet
    sat at zero, which is what a clean ``systemctl stop`` looks like.  The
    louder verdict wins, and the stop is named alongside it so nothing about
    why the pass ended is lost.
    """
    interrupted = frp.interrupted_status()
    if not interrupted or status not in ZERO_CAPACITY_STATUSES:
        return interrupted or status
    print(
        f"fleet-reconcile: CRITICAL: this pass was ended by an administrative stop "
        f"(status {interrupted}) but it is reporting {status}: capacity in this fleet "
        "is unaccounted for or held at zero, and a stop that keeps arriving at the "
        "same point would otherwise report that as an ordinary shutdown for ever",
        file=sys.stderr,
    )
    return status


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
    A read-only pass mutates nothing, but it reports the stranded-at-zero
    record and earns ``STRANDED_STATUS`` off it exactly as an apply pass does.
    """
    state_path = options.state_dir / STATE_FILE
    document = load_state(state_path)
    receipts = document["hosts"]
    order = runner_hosts(data)
    stranding, parked = open_pass_records(document, order, options)
    budget = open_drain_budget(order, stranding, parked)
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
            host,
            stranding,
            halted,
            blocking=producer_blocking,
            drained=drained,
            budget=budget,
            parked=parked,
        ):
            failures += 1
            hold_at_zero(stranding, host, options)
            continue

        receipt_invalidated = False
        last_mutation = ""

        def transaction_run(argv: Sequence[str], target: str = host) -> frp.CommandResult:
            nonlocal receipt_invalidated, last_mutation
            verb, _command_host = _command_identity(argv)
            last_mutation = verb if capacity_mutation(verb) else last_mutation
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
                receipts,
                stranding,
                order,
                host,
                receipt,
                index=index,
                released=producer_released,
                reopened=capacity_reopened(last_mutation),
                parked=parked,
            )
        if not ok:
            failures += 1
            if options.mode == "apply":
                lost = capacity_lost(host, stranded=stranded, mutated=bool(last_mutation))
                invalidate_receipt(
                    receipts,
                    stranding,
                    drained,
                    host,
                    options.now,
                    stranded=lost,
                    at_zero=host not in undrained,
                    reopened=capacity_reopened(last_mutation),
                    parked=parked,
                )
                save_state(state_path, document)
            if index == 0 and options.mode == "apply":
                producer_blocking, producer_released = producer_block_state(
                    stranding, host, stranded=stranded, undrained=host in undrained
                )
    if options.mode == "apply":
        save_state(state_path, document)
    escalated = settle_pass_records(
        data, document, state_path, stranding, parked, order, options, run
    )
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
    failures.extend(frac.run(sys.modules[__name__]))
    failures.extend(frad.run(sys.modules[__name__]))
    failures.extend(fra.run(sys.modules[__name__]))
    failures.extend(fras.run(apply_host))
    failures.extend(frr.run(sys.modules[__name__]))
    failures.extend(frb.run(sys.modules[__name__]))
    failures.extend(frbl.run(sys.modules[__name__]))
    failures.extend(frbu.run(sys.modules[__name__]))
    failures.extend(frc.run(sys.modules[__name__]))
    failures.extend(frd.run(sys.modules[__name__]))
    failures.extend(frdr.run(sys.modules[__name__]))
    failures.extend(frre.run(sys.modules[__name__]))
    failures.extend(frst.run(sys.modules[__name__]) + frsp.run(sys.modules[__name__]))
    failures.extend(frrl.run(sys.modules[__name__]))
    failures.extend(frse.run(sys.modules[__name__]))
    failures.extend(fri.run(sys.modules[__name__]))
    failures.extend(frf.run(sys.modules[__name__]))
    failures.extend(frfo.run(sys.modules[__name__]))
    failures.extend(frfz.run(sys.modules[__name__]))
    failures.extend(frpu.run(sys.modules[__name__]))
    failures.extend(frpr.run(sys.modules[__name__]))
    failures.extend(frop.run(sys.modules[__name__]))
    failures.extend(fro.run(sys.modules[__name__]))
    failures.extend(frpe.run(sys.modules[__name__]))
    failures.extend(frpk.run(sys.modules[__name__]))
    failures.extend(frlt.run(sys.modules[__name__]))
    failures.extend(frlo.run(sys.modules[__name__]) + frab.run(sys.modules[__name__]))
    failures.extend(frsv.run(sys.modules[__name__]))
    failures.extend(fru.run(sys.modules[__name__]))
    failures.extend(frrc.run(sys.modules[__name__]))
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


def open_pass(args: argparse.Namespace) -> tuple[dict[str, Any], ReconcileOptions]:
    """Authenticate this pass's source and state, returning what it reconciles.

    Split out of ``main`` so the two failures cannot be confused.  Nothing here
    has touched a host yet, so a failure is the operator's to fix and status 2
    is the honest account of one.  Once this returns, the pass owns a
    declaration and a state directory, which is what ``aborted_verdict`` needs
    to read the zero-capacity records off a pass that dies later on: making the
    binding structural means that path can never reach for a name the failure
    left unset.
    """
    source_digest = (
        validate_installed_authority(fm.REPO_ROOT)
        if args.require_installed_authority
        else "manual-operator-checkout"
    )
    state_dir = args.state_dir or Path.home() / ".local/state/ra8-fleet-reconcile"
    prepare_state_dir(state_dir)
    if args.require_installed_authority:
        fm.validate_runtime_inventory(state_dir)
    return fm.load(), ReconcileOptions(
        args.mode,
        args.force,
        source_digest,
        state_dir,
        args.full_interval,
        args.producer_interval,
        int(time.time()),
    )


def main(argv: Sequence[str] | None = None) -> int:
    """Enter selftest or one locked reconciliation transaction."""
    args = parse_args(argv)
    early_status = _early_status(args)
    if early_status is not None:
        return early_status
    try:
        data, options = open_pass(args)
    except PASS_FAILURES as error:
        print(f"fleet-reconcile: FATAL: {error}", file=sys.stderr)
        return FATAL_STATUS
    try:
        if args.mode == "check":
            with frp.stop_handlers():
                status = reconcile(data, options)
                return stopped_verdict(status)
        with (
            fml.mutation_lock(data, installed_local=args.require_installed_authority),
            frp.stop_handlers(),
        ):

            def guarded_runner(argv: Sequence[str]) -> frp.CommandResult:
                return frp.command_runner(argv, guardian=True)

            status = reconcile(data, options, guarded_runner)
            return stopped_verdict(status)
    except fml.MutationLockBusyError as error:
        print(f"fleet-reconcile: {error}", file=sys.stderr)
        return locked_out_verdict(data, options)
    except PASS_FAILURES as error:
        print(f"fleet-reconcile: FATAL: {error}", file=sys.stderr)
        return aborted_verdict(data, options)


if __name__ == "__main__":
    sys.exit(main())
