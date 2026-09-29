# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Regression selftests for escalating a durable park nothing lifts (#888).

``capacity-quarantine`` writes a host's durable maintenance marker before it
attempts the drain, so a drain the fleet entry point REFUSES leaves the host
PARKED: ``cmd_window``, the host-local timer that raises admission when a quiet
window ends, refuses to lift it while the marker is there, and
``capacity-restore`` is the only thing that removes it.  The controller records
that park, and the restore that lifts it is issued for a host whose own
declaration verified THIS pass.

That is deliberately narrow, and it leaves the two everyday cases unreachable.
A parked host whose read-only check keeps failing is never applied, so no
receipt is published for it and no restore is ever issued; a parked consumer
behind a failed producer is held before anything touches it.  In both, the
record aged, printed one WARNING, and the pass exited 1 exactly like any
one-off failure, pass after pass, with the host pinned at ZERO admission the
whole time.  A fleet at zero that reads like a transient fault is issue #888
itself, which went unnoticed about five times on silence of this shape.

These tests pin the park onto the same escalation clock as the
stranded-at-zero record:

* a park nothing lifts escalates CRITICAL and earns ``STRANDED_STATUS`` once it
  has survived ``PARK_ESCALATION_PASSES`` consecutive passes;
* below that threshold the pass stays an ordinary failure and the host is named
  by the warning instead, so the loudness is not spent early;
* a park this pass actually lifts never escalates, however many passes it had;
* the read-only dry run, which is the diagnostic in issue #888's own evidence,
  earns the same verdict off the stored record and still writes nothing;
* a record for a host this fleet no longer manages escalates nothing, so one
  retired host cannot make every later dry run permanently red.
"""

from __future__ import annotations

import contextlib
import io
import json
import tempfile
from collections.abc import Sequence
from pathlib import Path
from types import ModuleType
from typing import Any

import fleet_reconcile_process as frp

PRODUCER = "producer"
CONSUMER = "consumer-a"
RETIRED = "retired"
DRAIN_REFUSED_STATUS = 7
APPLY_FAILURE_STATUS = 1
CHECK_FAILURE_STATUS = 1
ORDINARY_FAILURE_STATUS = 1
FIRST_PASS = 1000
PASS_INTERVAL = 100
FULL_INTERVAL = 100
PRODUCER_INTERVAL = 50
DIGEST = "a" * 64
RESTORE_OUTPUT = "restoring current window target 2\n"


def _data() -> dict[str, Any]:
    """Return a producer and one consumer, both capacity-managed docker hosts."""
    return {
        "runner_image": {"source_host": PRODUCER},
        "hosts": {
            PRODUCER: {
                "class": "docker_linux",
                "runners": {"instances": 2},
                "provisions": ["one", "two"],
            },
            CONSUMER: {
                "class": "docker_linux",
                "runners": {"instances": 2},
                "provisions": ["one"],
            },
        },
    }


def _recap(host: str, changed: int = 0) -> str:
    """Build one successful Ansible recap row."""
    return f"{host} : ok=9 changed={changed} unreachable=0 failed=0 skipped=1 rescued=0 ignored=0\n"


def _check(data: dict[str, Any], host: str, changed: int) -> frp.CommandResult:
    """Return a successful check carrying this host's declared recap rows."""
    rows = [_recap(host, changed)]
    rows.extend(_recap(host) for _ in data["hosts"][host]["provisions"][1:])
    return frp.CommandResult(0, "".join(rows), "")


def _clean(controller: ModuleType, data: dict[str, Any], host: str) -> frp.CommandResult:
    """Return the exact accepted check result for one host."""
    noise = controller.PRODUCER_CHECK_NOISE if host == PRODUCER else 0
    return _check(data, host, noise)


def _options(
    controller: ModuleType, state_dir: Path, *, mode: str = "apply", now: int = FIRST_PASS
) -> object:
    """Return deterministic policy inputs for one pass."""
    return controller.ReconcileOptions(
        mode=mode,
        force=False,
        source_digest=DIGEST,
        state_dir=state_dir,
        full_interval=FULL_INTERVAL,
        producer_interval=PRODUCER_INTERVAL,
        now=now,
    )


def _seed(controller: ModuleType, state_dir: Path, document: dict[str, Any]) -> None:
    """Write one pass's starting state."""
    controller.save_state(state_dir / controller.STATE_FILE, document)


def _stored(controller: ModuleType, state_dir: Path) -> dict[str, Any]:
    """Read the state this pass persisted."""
    return json.loads((state_dir / controller.STATE_FILE).read_text(encoding="ascii"))


def _parked_pass(
    controller: ModuleType, data: dict[str, Any], state_dir: Path, now: int, *, drifting: bool
) -> tuple[int, str, list[tuple[str, str]]]:
    """Run one pass over a consumer that parks, then keeps failing its own check."""
    calls: list[tuple[str, str]] = []

    def consumer_result(verb: str, host: str) -> frp.CommandResult | None:
        """Answer for the consumer that parks and then keeps failing its own check."""
        if host != CONSUMER:
            return None
        if verb == "check":
            if drifting:
                return _check(data, host, 1)
            return frp.CommandResult(CHECK_FAILURE_STATUS, "", "check failed\n")
        if verb == "parked-apply":
            return frp.CommandResult(APPLY_FAILURE_STATUS, "", "apply failed\n")
        if verb == "quarantine":
            return frp.CommandResult(DRAIN_REFUSED_STATUS, "", "drain refused\n")
        return None

    def run(argv: Sequence[str]) -> frp.CommandResult:
        verb, host = controller._command_identity(argv)  # noqa: SLF001
        calls.append((verb, host))
        answer = consumer_result(verb, host)
        if answer is not None:
            return answer
        if verb in {"check", "parked-check"}:
            return _clean(controller, data, host)
        if verb == "restore":
            return frp.CommandResult(0, RESTORE_OUTPUT, "")
        return frp.CommandResult(0, "", "")

    stream = io.StringIO()
    with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(stream):
        status = controller.reconcile(
            data,
            _options(controller, state_dir, now=now),
            run,
            controller._no_wait,  # noqa: SLF001
        )
    return status, stream.getvalue(), calls


def _a_park_nothing_lifts_escalates(controller: ModuleType, failures: list[str]) -> None:
    """A park no pass can lift gets LOUDER, instead of exiting 1 for ever."""
    data = _data()
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-park-escalation-") as raw:
        state_dir = Path(raw)
        verdicts: list[int] = []
        escalated: list[bool] = []
        for index in range(controller.PARK_ESCALATION_PASSES + 1):
            status, text, calls = _parked_pass(
                controller,
                data,
                state_dir,
                FIRST_PASS + index * PASS_INTERVAL,
                drifting=index == 0,
            )
            verdicts.append(status)
            escalated.append("CRITICAL" in text and "durable maintenance park" in text)
            if index and ("restore", CONSUMER) in calls:
                failures.append(
                    f"park-escalation: a host that never reconciled was restored anyway: {calls}"
                )
        if verdicts[0] != controller.DRAIN_FAILED_STATUS:
            failures.append(
                f"park-escalation: the refused drain returned {verdicts[0]}, not the "
                "loudest verdict"
            )
        threshold = controller.PARK_ESCALATION_PASSES
        if verdicts[threshold - 1] != controller.STRANDED_STATUS:
            failures.append(
                f"park-escalation: {threshold} passes parked returned "
                f"{verdicts[threshold - 1]}, not STRANDED_STATUS"
            )
        if not escalated[threshold - 1]:
            failures.append("park-escalation: the pass that reached the threshold said nothing")
        if verdicts[threshold] != controller.STRANDED_STATUS:
            failures.append(
                f"park-escalation: the pass after the threshold went quiet again "
                f"({verdicts[threshold]})"
            )
        parked = _stored(controller, state_dir).get("parked", {})
        if parked.get(CONSUMER, {}).get("passes") != threshold + 1:
            failures.append(f"park-escalation: the record stopped counting passes: {parked}")
        if _stored(controller, state_dir).get("stranded"):
            failures.append("park-escalation: escalating a park forged a zero-capacity record")


def _below_the_threshold_stays_ordinary(controller: ModuleType, failures: list[str]) -> None:
    """Loudness is not spent early: one pass short of the threshold still exits 1."""
    data = _data()
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-park-escalation-") as raw:
        state_dir = Path(raw)
        short = controller.PARK_ESCALATION_PASSES - 1
        _seed(
            controller,
            state_dir,
            {
                "version": 1,
                "hosts": {},
                "stranded": {},
                "parked": {CONSUMER: {"since": FIRST_PASS - PASS_INTERVAL, "passes": short - 1}},
            },
        )
        status, text, _calls = _parked_pass(controller, data, state_dir, FIRST_PASS, drifting=False)
        if status != ORDINARY_FAILURE_STATUS:
            failures.append(f"park-escalation: {short} pass(es) parked already returned {status}")
        if "CRITICAL" in text:
            failures.append("park-escalation: a park below the threshold escalated")
        if "still holds a durable maintenance park" not in text:
            failures.append("park-escalation: a park below the threshold was not named at all")


def _a_lifted_park_never_escalates(controller: ModuleType, failures: list[str]) -> None:
    """A park this pass actually lifts is gone, however many passes it had."""
    data = _data()
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-park-escalation-") as raw:
        state_dir = Path(raw)
        now = FIRST_PASS + PASS_INTERVAL
        fresh = {
            "checked_at": FIRST_PASS,
            "full_applied_at": now - 10,
            "source_digest": DIGEST,
        }
        _seed(
            controller,
            state_dir,
            {
                "version": 1,
                "hosts": {PRODUCER: dict(fresh), CONSUMER: dict(fresh)},
                "stranded": {},
                "parked": {
                    CONSUMER: {
                        "since": FIRST_PASS,
                        "passes": controller.PARK_ESCALATION_PASSES + 2,
                    }
                },
            },
        )
        calls: list[tuple[str, str]] = []

        def run(argv: Sequence[str]) -> frp.CommandResult:
            verb, host = controller._command_identity(argv)  # noqa: SLF001
            calls.append((verb, host))
            if verb == "restore":
                return frp.CommandResult(0, RESTORE_OUTPUT, "")
            return _clean(controller, data, host)

        stream = io.StringIO()
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(stream):
            status = controller.reconcile(
                data,
                _options(controller, state_dir, now=now),
                run,
                controller._no_wait,  # noqa: SLF001
            )
        if ("restore", CONSUMER) not in calls:
            failures.append(f"park-escalation: the park release was never issued: {calls}")
        if status:
            failures.append(f"park-escalation: a lifted park still returned {status}")
        if "CRITICAL" in stream.getvalue():
            failures.append("park-escalation: a park lifted by this pass escalated anyway")
        if _stored(controller, state_dir).get("parked"):
            failures.append("park-escalation: a lifted park kept its record")


def _the_dry_run_earns_the_same_verdict(controller: ModuleType, failures: list[str]) -> None:
    """The read-only diagnostic says it too, and still writes nothing."""
    data = _data()
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-park-escalation-") as raw:
        state_dir = Path(raw)
        entry = {"since": FIRST_PASS, "passes": controller.PARK_ESCALATION_PASSES}
        document = {"version": 1, "hosts": {}, "stranded": {}, "parked": {CONSUMER: dict(entry)}}
        _seed(controller, state_dir, document)

        def run(argv: Sequence[str]) -> frp.CommandResult:
            verb, host = controller._command_identity(argv)  # noqa: SLF001
            if verb != "check":
                failures.append(f"park-escalation: the dry run issued {verb} against {host}")
            return _clean(controller, data, host)

        stream = io.StringIO()
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(stream):
            status = controller.reconcile(
                data,
                _options(controller, state_dir, mode="check", now=FIRST_PASS + PASS_INTERVAL),
                run,
                controller._no_wait,  # noqa: SLF001
            )
        if status != controller.STRANDED_STATUS:
            failures.append(f"park-escalation: the dry run over a held park returned {status}")
        if "CRITICAL" not in stream.getvalue():
            failures.append("park-escalation: the dry run never named the park it read")
        if _stored(controller, state_dir).get("parked") != {CONSUMER: entry}:
            failures.append("park-escalation: the dry run rewrote the parked record")


def _an_unmanaged_park_escalates_nothing(controller: ModuleType, failures: list[str]) -> None:
    """One retired host's stale record cannot make every later dry run red."""
    data = _data()
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-park-escalation-") as raw:
        state_dir = Path(raw)
        _seed(
            controller,
            state_dir,
            {
                "version": 1,
                "hosts": {},
                "stranded": {},
                "parked": {
                    RETIRED: {"since": FIRST_PASS, "passes": controller.PARK_ESCALATION_PASSES}
                },
            },
        )

        def run(argv: Sequence[str]) -> frp.CommandResult:
            _verb, host = controller._command_identity(argv)  # noqa: SLF001
            return _clean(controller, data, host)

        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            status = controller.reconcile(
                data,
                _options(controller, state_dir, mode="check", now=FIRST_PASS + PASS_INTERVAL),
                run,
                controller._no_wait,  # noqa: SLF001
            )
        if status:
            failures.append(
                f"park-escalation: a park record outside the declaration returned {status}"
            )


def _the_threshold_is_read_off_the_record(controller: ModuleType, failures: list[str]) -> None:
    """Below the threshold warns, at or above it escalates, and never both."""
    below = {CONSUMER: {"since": FIRST_PASS, "passes": controller.PARK_ESCALATION_PASSES - 1}}
    at = {CONSUMER: {"since": FIRST_PASS, "passes": controller.PARK_ESCALATION_PASSES}}
    over = {CONSUMER: {"since": FIRST_PASS, "passes": controller.PARK_ESCALATION_PASSES + 5}}
    with contextlib.redirect_stderr(io.StringIO()):
        if controller.parked_escalations(below, FIRST_PASS):
            failures.append("park-escalation: a park below the threshold escalated")
        if controller.parked_escalations(at, FIRST_PASS) != [CONSUMER]:
            failures.append("park-escalation: the threshold pass escalated nothing")
        if controller.parked_escalations(over, FIRST_PASS) != [CONSUMER]:
            failures.append("park-escalation: a long-held park escalated nothing")
        if controller.report_durable_park(below, FIRST_PASS) != [CONSUMER]:
            failures.append("park-escalation: a park below the threshold was not warned about")
        if controller.report_durable_park(at, FIRST_PASS):
            failures.append("park-escalation: an escalated park was warned about as well")
        if controller.parked_escalations({}, FIRST_PASS):
            failures.append("park-escalation: an empty record escalated something")


def run(controller: ModuleType) -> list[str]:
    """Return every durable-park escalation failure."""
    failures: list[str] = []
    _a_park_nothing_lifts_escalates(controller, failures)
    _below_the_threshold_stays_ordinary(controller, failures)
    _a_lifted_park_never_escalates(controller, failures)
    _the_dry_run_earns_the_same_verdict(controller, failures)
    _an_unmanaged_park_escalates_nothing(controller, failures)
    _the_threshold_is_read_off_the_record(controller, failures)
    return failures
