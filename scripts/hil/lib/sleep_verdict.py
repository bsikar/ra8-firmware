#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Decide what a UART capture proves about a core that was supposed to sleep.

The defect this exists for (#517)
---------------------------------
The HIL harness gates on UART banner output. A sleeping core emits nothing by
definition, so to ``run_direct.sh`` / ``run_local.sh`` "asleep" and "hung" are
the same observation: silence until the timeout, reported as FAIL. Four
``lpm_*`` apps are parked in ``examples/ek_ra8d2/hil_needs_revalidation/`` for
that reason and not because they fail -- ``lpm_periodic_idle``'s own
``hil.conf`` records SILICON-VALIDATED. One harness defect carried as four app
regressions.

Silence is not one observation, it is three
-------------------------------------------
A capture from an app whose success condition IS being asleep splits cleanly
once the app announces its intent before the sleep instruction:

* it never announced        -> it died before ``WFI``. Genuinely broken.
* it announced, then quiet  -> that is what asleep looks like. It is ALSO what
  hung-inside-``WFI`` looks like from a UART. Undecidable without a rig probe.
* it announced, then kept talking -> it did not sleep. The run proved nothing
  about sleep, whatever else the banners said.

So this module never reports PASS on silence, and never reports FAIL on it
either. The middle case gets its own verdict, ``SLEEPING_UNPROBED``, which the
harness surfaces as "needs a probe" rather than folding into the failure count.
That is the whole point: a verdict the tier can carry honestly, so
``hil_needs_revalidation`` stops conflating "fails on silicon" with "cannot be
observed by this harness".

What decides the undecidable case
---------------------------------
Only the rig can. ``--probe-report`` takes a JSON record from a logic-level
probe (the Analog Discovery 2 provisioned by
``infra/ansible/roles/ad2_tools``): a line asserted before ``WFI`` and released
in the wake ISR, sampled by the rig. With that record the middle case resolves
to PASS or FAIL. Without it the verdict stays ``SLEEPING_UNPROBED`` -- this
module will not upgrade an unobserved sleep to a pass on the strength of a
banner ordering.

Ordering, not timing
--------------------
The capture is raw bytes with no timestamps, so nothing here measures how long
the core was quiet. Every claim is about ORDER and PRESENCE of markers, which
is what the bytes actually support. A duration claim would need the reader to
timestamp its capture; it does not, so none is made.

Exit status, which is the harness contract::

    0  PASS                 announced, then wake evidence (banner or probe)
    1  FAIL                 negative banner, never announced, or probe says awake
    3  SLEEPING_UNPROBED    announced, then quiet, no probe record
    4  INCONCLUSIVE         announced, then kept talking: it did not sleep
    2  usage / broken input

Usage::

    python3 scripts/hil/lib/sleep_verdict.py --capture FILE --enter STR \
        [--wake STR] [--negative REGEX] [--probe-report FILE] [--app NAME]
    python3 scripts/hil/lib/sleep_verdict.py --selftest
"""

from __future__ import annotations

import argparse
import json
import re
import sys

PASS = 0
FAIL = 1
USAGE = 2
SLEEPING_UNPROBED = 3
INCONCLUSIVE = 4

VERDICT_NAME = {
    PASS: "PASS",
    FAIL: "FAIL",
    SLEEPING_UNPROBED: "SLEEPING_UNPROBED",
    INCONCLUSIVE: "INCONCLUSIVE",
}

# Bytes after the enter marker that do not count as the core still talking.
# A UART in flight when the core stops clocking can trail a partial line, and
# line endings arrive after the marker by construction. Anything longer than
# this that is not the wake marker is the core demonstrably still running.
TRAILING_SLACK = 16


def _decode(raw: bytes) -> str:
    """Render a capture as text without losing byte offsets to a decode error."""
    return raw.decode("utf-8", errors="replace")


def classify(
    capture: bytes,
    enter: str,
    wake: str = "",
    negative: str = "",
    probe: dict | None = None,
) -> tuple[int, str]:
    """Return ``(exit_status, one_line_reason)`` for one sleep-app capture."""
    if not enter:
        return USAGE, "no enter marker given: nothing states where sleep began"

    text = _decode(capture)

    if negative:
        hit = re.search(negative, text, re.IGNORECASE)
        if hit:
            return FAIL, f"matched negative pattern {negative!r}: {hit.group(0)!r}"

    at = text.find(enter)
    if at < 0:
        return FAIL, (
            f"never announced sleep entry ({enter!r} absent): "
            "the core did not reach the sleep instruction"
        )

    after = text[at + len(enter) :]

    wake_at = after.find(wake) if wake else -1
    if wake_at >= 0:
        return PASS, f"announced sleep entry, then woke: saw {wake!r} after {enter!r}"

    if probe is not None:
        if not isinstance(probe, dict):
            return USAGE, "probe report is not a JSON object"
        if "wake_observed" not in probe:
            return USAGE, "probe report has no 'wake_observed' field"
        if probe.get("wake_observed") is True:
            asleep = probe.get("asleep_observed")
            detail = "" if asleep is None else f", asleep_observed={asleep}"
            return PASS, f"rig probe observed the wake{detail}"
        return FAIL, "rig probe was attached and observed no wake"

    residue = after.strip()
    if residue and len(residue) > TRAILING_SLACK:
        return INCONCLUSIVE, (
            f"kept emitting {len(residue)} bytes after {enter!r} and never "
            f"{'woke' if wake else 'reported a wake'}: it did not sleep"
        )

    return SLEEPING_UNPROBED, (
        f"announced sleep entry ({enter!r}) and went quiet; a UART cannot tell "
        "that from hung-inside-WFI -- needs a rig probe (--probe-report)"
    )


def _load_probe(path: str) -> dict | None:
    with open(path, "rb") as handle:
        return json.loads(handle.read().decode("utf-8"))


# ---------------------------------------------------------------------------
# Selftest: every branch, in both directions.
# ---------------------------------------------------------------------------
def _selftest() -> int:
    enter = "lpm_ulpt: sleeping"
    wake = "lpm_ulpt: wake"
    neg = r"HardFault|hw_init_failed"

    cases: list[tuple[str, bytes, dict, int]] = [
        (
            "announced then woke -> PASS",
            b"boot\nlpm_ulpt: sleeping\nlpm_ulpt: wake count=1\n",
            {"wake": wake, "negative": neg},
            PASS,
        ),
        (
            "announced then quiet -> SLEEPING_UNPROBED, not FAIL",
            b"boot\nlpm_ulpt: sleeping\n",
            {"wake": wake, "negative": neg},
            SLEEPING_UNPROBED,
        ),
        (
            "quiet with a trailing partial line is still unprobed",
            b"boot\nlpm_ulpt: sleeping\r\n\x00\x00",
            {"wake": wake, "negative": neg},
            SLEEPING_UNPROBED,
        ),
        (
            "never announced -> FAIL (died before WFI)",
            b"boot\ncgc: ok\n",
            {"wake": wake, "negative": neg},
            FAIL,
        ),
        (
            "negative banner outranks a wake banner",
            b"lpm_ulpt: sleeping\nHardFault\nlpm_ulpt: wake\n",
            {"wake": wake, "negative": neg},
            FAIL,
        ),
        (
            "kept talking after announcing -> INCONCLUSIVE, not PASS",
            b"lpm_ulpt: sleeping\nmain loop tick 1\nmain loop tick 2\n",
            {"wake": wake, "negative": neg},
            INCONCLUSIVE,
        ),
        (
            "probe says woke -> PASS with no wake banner at all",
            b"lpm_ulpt: sleeping\n",
            {
                "wake": wake,
                "negative": neg,
                "probe": {"wake_observed": True, "asleep_observed": True},
            },
            PASS,
        ),
        (
            "probe attached and saw no wake -> FAIL",
            b"lpm_ulpt: sleeping\n",
            {"wake": wake, "negative": neg, "probe": {"wake_observed": False}},
            FAIL,
        ),
        (
            "probe report missing its field -> usage, never a silent pass",
            b"lpm_ulpt: sleeping\n",
            {"wake": wake, "probe": {"asleep_observed": True}},
            USAGE,
        ),
        (
            "no wake marker configured, quiet -> unprobed",
            b"lpm_ulpt: sleeping\n",
            {"negative": neg},
            SLEEPING_UNPROBED,
        ),
        (
            "empty enter marker -> usage",
            b"anything\n",
            {"enter": "", "wake": wake},
            USAGE,
        ),
        (
            "undecodable bytes do not crash the classifier",
            b"\xff\xfelpm_ulpt: sleeping\n",
            {"wake": wake, "negative": neg},
            SLEEPING_UNPROBED,
        ),
    ]

    failures = 0
    for name, capture, kwargs, want in cases:
        kwargs = dict(kwargs)
        marker = kwargs.pop("enter", enter)
        got, reason = classify(capture, marker, **kwargs)
        if got != want:
            failures += 1
            print(
                f"  FAIL {name}: want {VERDICT_NAME.get(want, want)}, "
                f"got {VERDICT_NAME.get(got, got)} ({reason})"
            )

    # The verdict codes are a contract with the shell harness: they must stay
    # distinct, and SLEEPING_UNPROBED must not collide with a shell failure
    # status that all.sh would count as a failing app.
    codes = [PASS, FAIL, USAGE, SLEEPING_UNPROBED, INCONCLUSIVE]
    if len(set(codes)) != len(codes):
        failures += 1
        print("  FAIL verdict codes are not distinct")

    if failures:
        print(f"sleep_verdict.py --selftest: FAIL ({failures} case(s))")
        return 1
    print(
        f"sleep_verdict.py --selftest: PASS ({len(cases)} cases, "
        "every verdict branch in both directions)"
    )
    return 0


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(add_help=True, description=__doc__.splitlines()[0])
    ap.add_argument("--capture", help="path to the raw UART capture")
    ap.add_argument("--enter", default="", help="banner emitted just before the sleep instruction")
    ap.add_argument("--wake", default="", help="banner that proves the core woke")
    ap.add_argument("--negative", default="", help="regex whose match fails the run outright")
    ap.add_argument("--probe-report", default="", help="JSON record from a rig logic probe")
    ap.add_argument("--app", default="", help="app name, for the printed verdict line")
    ap.add_argument("--selftest", action="store_true", help="run the synthetic fixtures")
    args = ap.parse_args(argv)

    if args.selftest:
        return _selftest()

    if not args.capture:
        ap.print_usage(sys.stderr)
        print("error: --capture is required (or --selftest)", file=sys.stderr)
        return USAGE

    try:
        with open(args.capture, "rb") as handle:
            capture = handle.read()
    except OSError as exc:
        print(f"error: cannot read capture {args.capture}: {exc}", file=sys.stderr)
        return USAGE

    probe = None
    if args.probe_report:
        try:
            probe = _load_probe(args.probe_report)
        except (OSError, ValueError) as exc:
            print(f"error: cannot read probe report {args.probe_report}: {exc}", file=sys.stderr)
            return USAGE

    status, reason = classify(
        capture,
        args.enter,
        wake=args.wake,
        negative=args.negative,
        probe=probe,
    )
    label = args.app or "sleep app"
    print(f"[sleep_verdict] {label}: {VERDICT_NAME.get(status, status)} -- {reason}")
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
