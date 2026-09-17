#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Decide what a bench-contention run actually proved.

The two accounts, and which one is evidence
-------------------------------------------
A run produces two records of the same minutes:

* the **journal** -- ``/var/lib/ra8-bench/journal.ndjson``, written by the lock
  about itself. It says who acquired and released, and when.
* the **witness** -- ``lib/bench_witness.py`` sampling ``/proc`` on the bench
  host. It says which process had the J-Link, started by which machine, between
  which two instants, read from the kernel.

Only the second is evidence. An actor that silently skipped its work writes
exactly the same pair of journal lines as one that waited its turn and then did
the work, so no arrangement of journal lines can tell those apart. Every claim
below is therefore either decided by the witness, or is a claim ABOUT the
journal decided by checking it against the witness.

Attribution is derived twice, independently
-------------------------------------------
Each actor in a run programs a DIFFERENT app, and the J-Link commander script
the witness captured names the hex being programmed. That maps a hardware
interval to an actor through the payload. Separately, the ssh peer address maps
it to a machine through the kernel. The two are computed separately and then
required to agree: if the app-derived grouping and the peer-derived grouping
disagree, something is wrong with the experiment and the run is reported
inconclusive rather than green.

Intervals are widened, never narrowed
-------------------------------------
A process start instant is exact (``/proc/<pid>/stat`` field 22). Its end is
bracketed between the last sample that saw it and the first that did not. Every
interval is taken as ``[exact_start, gone_by]`` -- the PESSIMISTIC bound, which
can only make overlap more likely to be reported. Non-overlap concluded from
widened intervals is non-overlap.

Usage::

    bench_contention_verdict.py --dir RUNDIR --phases exclusion,death
    bench_contention_verdict.py --selftest

Exit 0 when every claim in the selected phases held, 1 when one did not, 3 when
no verdict could be established (a missing or empty artefact).
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from datetime import datetime
from pathlib import Path

EXIT_OK = 0
EXIT_FAIL = 1
EXIT_UNKNOWN = 3

# How long after a SIGKILL a queued waiter may take to get in before the
# release path is judged not to be prompt. The ssh-death selftest measures the
# flock dropping in well under a second; this allows for the waiter's own
# poll interval, its ssh round trip and a loaded Pi.
DEATH_HANDOVER_LIMIT_S = 30.0

# Slack when checking that a hardware interval sits inside its journal hold
# window. Both timestamps come from the bench host's clock, so this covers
# journal timestamps being whole seconds while witness ones are milliseconds.
CONTAINMENT_SLACK_S = 2.0

# Below this, a leftover hardware session that outlived its holder is just the
# tail of a process shutting down and is not worth narrating.
LINGER_NOTICEABLE_S = 1.0

# A collision needs two distinct machines. Named so the comparisons below read
# as "more than one machine" rather than as an unexplained 1.
ONE_MACHINE = 1

# An assignment line is "<actor> <app>"; anything shorter is not one.
ASSIGNMENT_FIELDS = 2


class Verdict:
    """Collected claims and their outcomes, rendered as one report."""

    def __init__(self) -> None:
        """Start with no claims recorded and nothing failed."""
        self.rows: list[tuple[str, str, str]] = []
        self.failed = 0
        self.unknown = 0

    def record(self, phase: str, claim: str, ok: bool | None, detail: str) -> None:
        """Log one claim. ``ok is None`` means no verdict could be established."""
        if ok is None:
            state = "UNKNOWN"
            self.unknown += 1
        elif ok:
            state = "PASS"
        else:
            state = "FAIL"
            self.failed += 1
        self.rows.append((f"{phase}/{claim}", state, detail))

    def note(self, phase: str, label: str, detail: str) -> None:
        """Record an OBSERVATION -- something measured, not something asserted.

        Wait distributions and grant orders are evidence a reader needs and
        nothing can fail on: there is no threshold at which a grant order is
        "wrong". Recording them as claims that happen to always pass would
        inflate the pass count with rows that can never fail, which is exactly
        how a suite drifts into looking stronger than it is.
        """
        self.rows.append((f"{phase}/{label}", "OBSERVED", detail))

    def report(self) -> int:
        """Print every claim and return the process exit code."""
        width = max((len(r[0]) for r in self.rows), default=10)
        print("\n=== bench contention verdict ===")
        for name, state, detail in self.rows:
            print(f"  {state:<7} {name:<{width}}  {detail}")
        if self.failed:
            print(f"\nVERDICT: FAIL -- {self.failed} claim(s) did not hold.")
            return EXIT_FAIL
        if self.unknown:
            print(f"\nVERDICT: UNKNOWN -- {self.unknown} claim(s) could not be established.")
            print("UNKNOWN is not a pass. Fix the artefact and re-run.")
            return EXIT_UNKNOWN
        print(f"\nVERDICT: PASS -- {len(self.rows)} claim(s) held.")
        return EXIT_OK


def read_ndjson(path: Path) -> list[dict]:
    """Every parsable object in an NDJSON file; [] when it is absent."""
    if not path.is_file():
        return []
    out = []
    for raw in path.read_text(errors="replace").splitlines():
        line = raw.strip()
        if not line:
            continue
        try:
            out.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    return out


def iso_epoch(text: str) -> float:
    """Seconds since the epoch for an ISO-8601 instant, or 0.0 when unparsable."""
    try:
        return datetime.fromisoformat(text).timestamp()
    except (ValueError, TypeError):
        return 0.0


class Hold:
    """One acquire/release pair out of the journal."""

    def __init__(self, lock_id: str, holder: str, start: float, note: str) -> None:
        """Open a hold at *start*; ``end`` stays infinite until a release lands."""
        self.lock_id = lock_id
        self.holder = holder
        self.start = start
        self.end = float("inf")
        self.note = note
        self.forced = False

    def __repr__(self) -> str:
        """Render as the holder and its window, for a failure message."""
        return f"<Hold {self.holder} {self.start:.0f}..{self.end:.0f}>"


def holds_from_journal(events: list[dict]) -> list[Hold]:
    """Reconstruct hold windows, in journal order, from acquire/release lines."""
    open_holds: dict[str, Hold] = {}
    holds: list[Hold] = []
    for ev in events:
        kind = ev.get("event", "")
        lock_id = ev.get("lock_id", "")
        when = iso_epoch(ev.get("at", ""))
        if kind == "acquire":
            hold = Hold(lock_id, ev.get("holder_name", ""), when, ev.get("note", ""))
            open_holds[lock_id] = hold
            holds.append(hold)
        elif kind == "release" and lock_id in open_holds:
            open_holds.pop(lock_id).end = when
        elif kind == "force-take" and lock_id in open_holds:
            open_holds[lock_id].forced = True
    return holds


class Interval:
    """One hardware-tool occupancy, widened to its pessimistic bounds."""

    def __init__(self, ev_start: dict, ev_end: dict | None) -> None:
        """Build the widened interval from a proc_start and its proc_end."""
        self.pid = ev_start.get("pid")
        self.tool = ev_start.get("tool", "")
        self.peer = ev_start.get("peer", "")
        self.script = ev_start.get("script", "")
        self.argv = ev_start.get("argv", "")
        self.start = float(ev_start.get("start_epoch") or ev_start.get("first_seen") or 0.0)
        self.end = float((ev_end or {}).get("gone_by") or ev_start.get("first_seen") or 0.0)
        self.actor = ""

    def overlaps(self, other: Interval) -> bool:
        """True when the two widened intervals share any instant."""
        return self.start < other.end and other.start < self.end

    def __repr__(self) -> str:
        """Render as tool, pid and machine, for a failure message."""
        return f"<{self.tool} pid={self.pid} peer={self.peer} actor={self.actor}>"


def intervals_from_witness(events: list[dict]) -> list[Interval]:
    """Pair proc_start with proc_end into widened occupancy intervals."""
    starts: dict[int, dict] = {}
    ends: dict[int, dict] = {}
    for ev in events:
        if ev.get("ev") == "proc_start":
            starts[ev["pid"]] = ev
        elif ev.get("ev") == "proc_end":
            ends[ev["pid"]] = ev
    out = [Interval(s, ends.get(pid)) for pid, s in starts.items()]
    out.sort(key=lambda i: i.start)
    return out


def attribute_by_payload(intervals: list[Interval], assignment: dict[str, str]) -> None:
    """Name each interval's actor from the app its J-Link script programs.

    ``assignment`` maps actor name -> app name. The commander script contains
    ``loadfile /tmp/hil_<app>_mram.<pid>.hex``, so the app names the actor
    without consulting the lock at all. Read-only sessions carry no loadfile and
    stay unattributed by this route; the peer axis covers those.
    """
    by_app = {app: actor for actor, app in assignment.items()}
    for iv in intervals:
        for app, actor in by_app.items():
            if f"hil_{app}_mram" in iv.script:
                iv.actor = actor
                break


def check_attribution_axes(intervals: list[Interval]) -> tuple[bool, str]:
    """Require the payload-derived and kernel-derived attributions to agree.

    Every interval attributed to one actor must carry one peer address, and no
    two actors may share a peer. A disagreement means the experiment did not run
    the way it was described, and nothing downstream should be believed.
    """
    peers_by_actor: dict[str, set[str]] = {}
    for iv in intervals:
        if iv.actor:
            peers_by_actor.setdefault(iv.actor, set()).add(iv.peer)
    split = {a: p for a, p in peers_by_actor.items() if len(p) > 1}
    if split:
        return (False, f"an actor's work came from more than one machine: {split}")
    seen: dict[str, str] = {}
    for actor, peers in peers_by_actor.items():
        peer = next(iter(peers))
        if peer in seen:
            return (False, f"actors {seen[peer]} and {actor} share peer {peer}")
        seen[peer] = actor
    return (True, f"{len(peers_by_actor)} actor(s), one machine each: {seen}")


def overlapping_pairs(intervals: list[Interval]) -> list[tuple[Interval, Interval]]:
    """Every pair of intervals from DIFFERENT machines that share an instant."""
    out = []
    for i, first in enumerate(intervals):
        for second in intervals[i + 1 :]:
            if not first.peer or not second.peer or first.peer == second.peer:
                continue
            if first.overlaps(second):
                out.append((first, second))
    return out


def census_collisions(events: list[dict]) -> list[dict]:
    """Census samples that directly observed two machines on the board at once."""
    return [
        ev for ev in events if ev.get("ev") == "census" and len(ev.get("peers", [])) > ONE_MACHINE
    ]


def console_collisions(events: list[dict]) -> list[tuple[float, list[str]]]:
    """Instants at which two machines held the same board console at once.

    A second axis, and an independently damaging one: the J-Link OB's VCOM
    delivers each byte to exactly one reader, so two tails of one console each
    get roughly half the stream. #497 lists that as a concrete failure -- a HIL
    run silently pattern-matching against half its output -- and it is a
    collision the J-Link's own multiplexing does nothing to prevent.
    """
    per_instant: dict[tuple[float, str], set[str]] = {}
    for ev in events:
        if ev.get("ev") != "tty":
            continue
        key = (float(ev.get("t", 0.0)), str(ev.get("dev", "")))
        per_instant.setdefault(key, set()).add(str(ev.get("peer", "")))
    return [
        (when, sorted(peers))
        for (when, _dev), peers in sorted(per_instant.items())
        if len({p for p in peers if p}) > ONE_MACHINE
    ]


def hold_overlaps(holds: list[Hold]) -> list[tuple[Hold, Hold]]:
    """Every pair of journal hold windows that overlap in time."""
    out = []
    for i, first in enumerate(holds):
        out.extend(
            (first, second)
            for second in holds[i + 1 :]
            if first.start < second.end and second.start < first.end
        )
    return out


class PhaseData:
    """The three artefacts of one phase, loaded together."""

    def __init__(self, root: Path, phase: str) -> None:
        """Load one phase's journal slice and witness trace from *root*."""
        self.phase = phase
        self.journal = read_ndjson(root / f"journal-{phase}.ndjson")
        self.witness = read_ndjson(root / f"witness-{phase}.ndjson")
        self.holds = holds_from_journal(self.journal)
        self.intervals = intervals_from_witness(self.witness)

    @property
    def usable(self) -> bool:
        """False when an artefact is missing, which is UNKNOWN and not a pass."""
        return bool(self.witness)


def _load_assignment(root: Path, phase: str) -> dict[str, str]:
    """Actor name -> app name, as the driver assigned it for *phase*.

    Per phase, because the phases assign differently -- `death` leaves its
    victim out -- and a single shared file let the last phase to run rewrite
    the record the earlier ones are judged against, which read as an actor
    having done no work when it had flashed the board perfectly well.
    """
    path = root / f"assignment-{phase}.txt"
    out: dict[str, str] = {}
    if not path.is_file():
        return out
    for line in path.read_text().splitlines():
        parts = line.split()
        if len(parts) >= ASSIGNMENT_FIELDS:
            out[parts[0]] = parts[1]
    return out


def claim_exclusion(v: Verdict, root: Path, roster: list[str]) -> None:
    """Mutual exclusion, journal truthfulness, and no physical interleaving."""
    data = PhaseData(root, "exclusion")
    phase = "exclusion"
    if not data.usable:
        v.record(phase, "artefacts", None, "no witness output for this phase")
        return
    assignment = _load_assignment(root, "exclusion")
    attribute_by_payload(data.intervals, assignment)

    ok, detail = check_attribution_axes(data.intervals)
    v.record(phase, "attribution-agrees", ok, detail)

    collisions = census_collisions(data.witness)
    pairs = overlapping_pairs(data.intervals)
    v.record(
        phase,
        "no-physical-overlap",
        not collisions and not pairs,
        f"{len(data.intervals)} tool session(s); {len(collisions)} census collision(s); "
        f"{len(pairs)} overlapping pair(s)" + (f" -- {pairs[0]}" if pairs else ""),
    )

    console = console_collisions(data.witness)
    v.record(
        phase,
        "no-console-collision",
        not console,
        "no instant had two machines reading the board console"
        if not console
        else f"{len(console)} instant(s) with two console readers, first {console[0]}",
    )

    worked = {iv.actor for iv in data.intervals if iv.actor}
    missing = [a for a in roster if a not in worked]
    v.record(
        phase,
        "every-actor-really-worked",
        not missing,
        f"programmed the board: {sorted(worked)}"
        + (f"; NO hardware evidence for {missing}" if missing else ""),
    )

    overlaps = hold_overlaps(data.holds)
    v.record(
        phase,
        "holds-serialised",
        not overlaps,
        f"{len(data.holds)} hold(s) in the journal, none overlapping"
        if not overlaps
        else f"overlapping holds: {overlaps[0]}",
    )
    _claim_containment(v, phase, data)


def _claim_containment(v: Verdict, phase: str, data: PhaseData) -> None:
    """Every hardware interval sat inside the hold the journal attributes to it.

    This is where the journal is checked against reality in both directions: no
    hardware activity outside a hold (the lock did not miss anything), and each
    actor's activity inside ITS OWN hold (the journal named the right actor).
    """
    named = [iv for iv in data.intervals if iv.actor]
    if not named:
        v.record(phase, "journal-matches-hardware", None, "no attributable hardware activity")
        return
    outside, misattributed = [], []
    for iv in named:
        window = next(
            (
                h
                for h in data.holds
                if h.start - CONTAINMENT_SLACK_S <= iv.start
                and iv.end <= h.end + CONTAINMENT_SLACK_S
            ),
            None,
        )
        if window is None:
            outside.append(iv)
        elif iv.actor not in window.holder:
            misattributed.append((iv, window.holder))
    v.record(
        phase,
        "journal-matches-hardware",
        not outside and not misattributed,
        f"{len(named)} programming session(s), each inside the hold the journal "
        f"attributes to the same actor"
        if not (outside or misattributed)
        else f"unlocked activity: {outside}; misattributed: {misattributed}",
    )


def claim_negative_control(v: Verdict, root: Path) -> None:
    """The witness must be able to SEE a collision, or nothing else means much."""
    data = PhaseData(root, "negctl")
    phase = "negative-control"
    if not data.usable:
        v.record(phase, "artefacts", None, "no witness output for this phase")
        return
    collisions = census_collisions(data.witness)
    pairs = overlapping_pairs(data.intervals)
    peers = sorted({iv.peer for iv in data.intervals if iv.peer})
    v.record(
        phase,
        "witness-detects-collision",
        bool(collisions or pairs),
        f"unguarded run from {len(peers)} machine(s) {peers}: "
        f"{len(collisions)} census collision(s), {len(pairs)} overlapping pair(s)"
        + (
            ""
            if (collisions or pairs)
            else " -- the witness saw NO collision, so a clean guarded run proves nothing"
        ),
    )
    console = console_collisions(data.witness)
    v.record(
        phase,
        "witness-detects-console-collision",
        bool(console),
        f"{len(console)} instant(s) with two machines on one console"
        + (
            f", up to {max(len(p) for _, p in console)} at once" if console else " -- axis unproven"
        ),
    )
    v.record(
        phase,
        "no-lock-was-held",
        not data.holds,
        f"{len(data.holds)} hold(s) in the journal during the unguarded phase",
    )


def _kill_instant(root: Path) -> float:
    """The instant the victim's ssh client was SIGKILLed, from the kill log."""
    path = root / "kill-marker.txt"
    if not path.is_file():
        return 0.0
    match = re.search(r"KILLED at ([0-9.]+)", path.read_text())
    return float(match.group(1)) if match else 0.0


def _claim_death_release(v: Verdict, data: PhaseData, killed: float) -> None:
    """The flock really dropped when the holder's ssh client was SIGKILLed."""
    released = [h for h in data.holds if h.end != float("inf") and h.end >= killed - 1]
    if not released:
        v.record(
            "death", "lock-released-on-death", ok=False, detail="no release followed the SIGKILL"
        )
        return
    # The journal stamps whole seconds (`date -Iseconds`) while the kill instant
    # is sub-second, so a release inside the same second can read as very
    # slightly negative. Clamp rather than print a negative delay that invites a
    # reader to distrust the whole table.
    gap = max(0.0, min(h.end for h in released) - killed)
    v.record(
        "death",
        "lock-released-on-death",
        gap <= DEATH_HANDOVER_LIMIT_S,
        f"the journal recorded a release within {gap:.0f}-1s of the SIGKILL "
        "(journal resolution is one second), with no trap having run",
    )


def _claim_death_handover(v: Verdict, data: PhaseData, killed: float, first: Hold) -> None:
    """A waiter got in promptly ONCE THE BOARD WAS ACTUALLY IDLE.

    A waiter must not be judged against the SIGKILL instant. The victim's J-Link
    session lives on the bench host, over its own ssh, and does not die with the
    hold; the bench host deliberately withholds the board until that leftover
    finishes. Time spent in that interlock is the interlock WORKING.
    """
    in_flight = [iv for iv in data.intervals if iv.start <= killed <= iv.end]
    idle_at = max([killed, *[iv.end for iv in in_flight]])
    lingered = idle_at - killed
    v.note(
        "death",
        "board-quiesced-before-handover",
        f"the dead holder's hardware ran {lingered:.1f}s past its own release; "
        f"the bench host withheld the board until it stopped"
        if lingered > LINGER_NOTICEABLE_S
        else "no hardware outlived the dead holder",
    )
    delay = max(0.0, first.start - idle_at)
    v.record(
        "death",
        "waiter-gets-in",
        delay <= DEATH_HANDOVER_LIMIT_S,
        f"{first.holder} acquired {delay:.1f}s after the board went idle "
        f"({first.start - killed:.1f}s after the SIGKILL, of which {lingered:.1f}s was the "
        f"leftover session); limit {DEATH_HANDOVER_LIMIT_S:.0f}s",
    )


def _claim_death_work(v: Verdict, data: PhaseData, root: Path, first: Hold) -> None:
    """The waiter really used the board, and never at the same time as the dead one."""
    attribute_by_payload(data.intervals, _load_assignment(root, "death"))
    worked = [
        iv
        for iv in data.intervals
        if iv.start >= first.start - CONTAINMENT_SLACK_S
        and iv.end <= first.end + CONTAINMENT_SLACK_S
    ]
    v.record(
        "death",
        "waiter-really-used-the-board",
        bool(worked),
        f"{len(worked)} tool session(s) inside {first.holder}'s hold"
        if worked
        else f"{first.holder} took the lock but no hardware activity is recorded inside it",
    )
    pairs = overlapping_pairs(data.intervals)
    v.record(
        "death",
        "no-overlap-across-the-handover",
        not pairs,
        "the dead holder's work and the waiter's never coincided"
        if not pairs
        else f"OVERLAP across the handover: {pairs[0]}",
    )


def claim_death(v: Verdict, root: Path) -> None:
    """A waiter really gets the board after the holder is killed."""
    data = PhaseData(root, "death")
    if not data.usable:
        v.record("death", "artefacts", None, "no witness output for this phase")
        return
    killed = _kill_instant(root)
    if killed <= 0:
        v.record("death", "kill-recorded", None, "no KILLED marker in kill-marker.txt")
        return
    _claim_death_release(v, data, killed)
    after = [h for h in data.holds if h.start >= killed]
    if not after:
        v.record(
            "death",
            "waiter-gets-in",
            ok=False,
            detail=f"nothing acquired the bench after the SIGKILL at {killed:.2f} -- "
            "the queue did not move",
        )
        return
    first = min(after, key=lambda h: h.start)
    _claim_death_handover(v, data, killed, first)
    _claim_death_work(v, data, root, first)


def _fairness_rows(root: Path, roster: list[str]) -> dict[str, list[float]]:
    """Per-actor wait times, from each actor's own request/acquire timeline.

    The request instant is the actor's own claim, which is the one thing here
    that cannot be taken from the kernel -- a machine's intent to ask is not
    visible on the bench. It is a safe claim to accept: overstating it can only
    make an actor look like it waited LESS, so it cannot manufacture fairness.
    """
    waits: dict[str, list[float]] = {a: [] for a in roster}
    journal = read_ndjson(root / "journal-fairness.ndjson")
    acquires = [
        (iso_epoch(e.get("at", "")), e.get("holder_name", ""))
        for e in journal
        if e.get("event") == "acquire"
    ]
    for actor in roster:
        log = root / f"fairness-{actor}.log"
        if not log.is_file():
            continue
        mine = sorted(t for t, who in acquires if actor in who)
        requests = sorted(
            float(m.group(1))
            for m in re.finditer(
                rf"ROUND \d+ {re.escape(actor)} request ([0-9.]+)", log.read_text()
            )
        )
        # Requests and grants are both in time order and strictly alternate for
        # one actor -- it cannot ask again before its previous turn is over --
        # so pair them by consuming the grant list as the requests are walked.
        # An index-into-a-filtered-list did this before and matched only every
        # other round, which under-reported the number of waits measured.
        pending = list(mine)
        for req in requests:
            grant = next((t for t in pending if t >= req - 1), None)
            if grant is None:
                continue
            pending.remove(grant)
            waits[actor].append(grant - req)
    return waits


def claim_fairness(v: Verdict, root: Path, roster: list[str], rounds: int) -> None:
    """Report the wait distribution honestly, and fail only on real starvation."""
    phase = "fairness"
    journal = read_ndjson(root / "journal-fairness.ndjson")
    if not journal:
        v.record(phase, "artefacts", None, "no journal slice for this phase")
        return
    holds = holds_from_journal(journal)
    counts = {a: sum(1 for h in holds if a in h.holder) for a in roster}
    starved = [a for a, n in counts.items() if n == 0]
    v.record(
        phase,
        "nobody-starved",
        not starved,
        f"acquires per actor: {counts} (asked for {rounds} each)"
        + (f"; STARVED: {starved}" if starved else ""),
    )
    v.record(
        phase,
        "all-requests-served",
        all(n >= rounds for n in counts.values()),
        f"every actor got all {rounds} of its turns"
        if all(n >= rounds for n in counts.values())
        else f"short of {rounds} turns: { {a: n for a, n in counts.items() if n < rounds} }",
    )
    waits = _fairness_rows(root, roster)
    summary = {
        a: f"n={len(w)} max={max(w):.1f}s avg={sum(w) / len(w):.1f}s" for a, w in waits.items() if w
    }
    v.note(phase, "wait-distribution", f"{summary}")
    order = [h.holder.split(":")[-1] for h in sorted(holds, key=lambda h: h.start)]
    v.note(phase, "grant-order", " -> ".join(order))
    v.record(
        phase,
        "no-overlapping-holds",
        not hold_overlaps(holds),
        f"{len(holds)} hold(s), none overlapping",
    )


def _fake_interval(pid: int, peer: str, start: float, end: float) -> Interval:
    """A synthetic tool occupancy, for the selftest."""
    return Interval(
        {"pid": pid, "tool": "JLinkExe", "peer": peer, "start_epoch": start, "script": ""},
        {"gone_by": end},
    )


def _selftest_overlap() -> list[str]:
    """Overlap must be found when it is there, and not when it is not."""
    failures = []
    clean = [
        _fake_interval(1, "10.0.0.1", 100.0, 110.0),
        _fake_interval(2, "10.0.0.2", 110.5, 120.0),
    ]
    if overlapping_pairs(clean):
        failures.append("overlapping_pairs reported an overlap between disjoint intervals")
    dirty = [
        _fake_interval(1, "10.0.0.1", 100.0, 110.0),
        _fake_interval(2, "10.0.0.2", 109.9, 120.0),
    ]
    if not overlapping_pairs(dirty):
        failures.append("overlapping_pairs MISSED a 0.1s overlap between two machines")
    same = [
        _fake_interval(1, "10.0.0.1", 100.0, 110.0),
        _fake_interval(2, "10.0.0.1", 105.0, 120.0),
    ]
    if overlapping_pairs(same):
        failures.append("overlapping_pairs flagged one machine overlapping itself")
    if not census_collisions([{"ev": "census", "peers": ["a", "b"]}]):
        failures.append("census_collisions missed a two-peer census")
    if census_collisions([{"ev": "census", "peers": ["a"]}]):
        failures.append("census_collisions flagged a single-peer census")
    return failures


def _selftest_console() -> list[str]:
    """Two machines on ONE console at ONE instant, and nothing weaker."""

    def tty(when: float, dev: str, peer: str) -> dict:
        return {"ev": "tty", "t": when, "dev": dev, "peer": peer}

    failures = []
    if console_collisions([tty(1.0, "/dev/ttyACM1", "a"), tty(2.0, "/dev/ttyACM1", "b")]):
        failures.append("console_collisions flagged two readers that were not simultaneous")
    if not console_collisions([tty(1.0, "/dev/ttyACM1", "a"), tty(1.0, "/dev/ttyACM1", "b")]):
        failures.append("console_collisions MISSED two machines on one console at one instant")
    if console_collisions([tty(1.0, "/dev/ttyACM1", "a"), tty(1.0, "/dev/ttyACM1", "a")]):
        failures.append("console_collisions flagged one machine holding a console twice")
    if console_collisions([tty(1.0, "/dev/ttyACM1", "a"), tty(1.0, "/dev/ttyACM2", "b")]):
        failures.append("console_collisions flagged two DIFFERENT consoles as a collision")
    return failures


def _selftest_journal() -> list[str]:
    """Journal reconstruction, and the timestamp parser it rests on."""
    failures = []
    events = [
        {"at": "2026-01-01T00:00:00+00:00", "event": "acquire", "lock_id": "a", "holder_name": "x"},
        {"at": "2026-01-01T00:00:10+00:00", "event": "release", "lock_id": "a", "holder_name": "x"},
        {"at": "2026-01-01T00:00:05+00:00", "event": "acquire", "lock_id": "b", "holder_name": "y"},
        {"at": "2026-01-01T00:00:20+00:00", "event": "release", "lock_id": "b", "holder_name": "y"},
    ]
    if not hold_overlaps(holds_from_journal(events)):
        failures.append("hold_overlaps missed two journal holds that overlap")
    if iso_epoch("2026-01-01T00:00:00+00:00") <= 0:
        failures.append("iso_epoch could not parse a journal timestamp")
    if iso_epoch("nonsense") != 0.0:
        failures.append("iso_epoch accepted a non-timestamp")
    return failures


def selftest() -> int:
    """Prove the analyser calls a collision a collision, and a clean run clean.

    A verdict tool that returned PASS on any input would be the most expensive
    kind of green in this repository. Both directions are asserted on synthetic
    witness data, so the check does not depend on a bench being present.
    """
    failures = _selftest_overlap() + _selftest_console() + _selftest_journal()
    for text in failures:
        print(f"bench_contention_verdict: SELFTEST FAIL -- {text}", file=sys.stderr)
    if failures:
        return EXIT_FAIL
    print("bench_contention_verdict: selftest OK -- overlap detected and non-overlap not")
    return EXIT_OK


def main(argv: list[str]) -> int:
    """Run every requested phase's claims and print one verdict."""
    parser = argparse.ArgumentParser(description="bench contention verdict")
    parser.add_argument("--dir", type=Path, help="run artefact directory")
    parser.add_argument("--phases", default="exclusion,negative-control,death,fairness")
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)

    if args.selftest:
        return selftest()
    if args.dir is None or not args.dir.is_dir():
        print(
            "bench_contention_verdict: --dir must name an existing run directory", file=sys.stderr
        )
        return EXIT_UNKNOWN

    roster_file = args.dir / "roster.txt"
    if not roster_file.is_file():
        print("bench_contention_verdict: no roster.txt in the run directory", file=sys.stderr)
        return EXIT_UNKNOWN
    roster = [line.strip() for line in roster_file.read_text().splitlines() if line.strip()]

    v = Verdict()
    phases = [p.strip() for p in args.phases.split(",") if p.strip()]
    if "exclusion" in phases:
        claim_exclusion(v, args.dir, roster)
    if "negative-control" in phases:
        claim_negative_control(v, args.dir)
    if "death" in phases:
        claim_death(v, args.dir)
    if "fairness" in phases:
        claim_fairness(v, args.dir, roster, args.rounds)
    if not v.rows:
        print("bench_contention_verdict: no claims were evaluated", file=sys.stderr)
        return EXIT_UNKNOWN
    return v.report()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
