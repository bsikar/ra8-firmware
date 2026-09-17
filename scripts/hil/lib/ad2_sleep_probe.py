#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Produce the rig probe report ``sleep_verdict.py`` consumes (#517).

The half that was missing
-------------------------
``scripts/hil/lib/sleep_verdict.py`` splits a sleeping app's silence into three
readings and refuses to call the middle one either way: announced then quiet is
``SLEEPING_UNPROBED``, because a UART cannot tell asleep from hung-inside-WFI.
Its own docstring names what settles it, a JSON record from a logic-level probe
passed as ``--probe-report``. Nothing in the tree wrote that record, so
``SLEEPING_UNPROBED`` was the ceiling for every sleeping app and the four
``lpm_*`` apps in ``hil_needs_revalidation/`` stayed parked. This module is the
producer.

What it reads
-------------
One digital line that the app drives: asserted just before the sleep
instruction, released in the wake ISR. Two sources:

* ``--csv FILE`` -- a WaveForms digital export already on disk. Offline, no
  instrument, and the only path exercised by ``--selftest``.
* ``--capture-ad2`` -- sample the line live through ``libdwf`` (the Analog
  Discovery 2 the ``ad2_tools`` Ansible role provisions, ``0403:6014`` on the
  rig's port 2 per ``scripts/hil/ppps.sh``).

A report is a claim, not a log
------------------------------
This is the rule the whole module is built around. ``sleep_verdict.py`` scores
``wake_observed: false`` as **FAIL**, so writing that field on a capture that
merely ran out of samples would turn a truncated recording into a failing app,
which is the exact defect #517 exists to remove. So a capture that never showed
an assertion, or that ends with the line still asserted, writes **no report**
and exits ``INCONCLUSIVE``. The consumer then keeps its honest
``SLEEPING_UNPROBED`` instead of inheriting a fabricated verdict.

``--strict-assert`` is the one way to get ``wake_observed: false`` out of this
module, and it is the caller declaring that the capture window provably covers
the whole sleep plus the wake stimulus. Then a line still asserted at the end
of the window is a real "it never woke", and the report says so.

Polarity is a caller convention
-------------------------------
``--active-low`` says which level means asleep. A capture carries no evidence
of how the probe was wired, so this module cannot detect a miswiring: read an
active-high capture as active-low and the *idle* stretch is what gets reported
as the sleep, with no error anywhere. A selftest case pins that reading rather
than pretending to catch it. The level the line sits at while the core is
asleep is the only convention this file has.

Ordering and duration
---------------------
Unlike the UART capture the consumer reads, samples here are evenly spaced, so
a duration is supported by the data: ``asleep_us`` is the sample distance from
the debounced assertion to the debounced release divided by the sample rate.
When the capture begins mid-assertion the start is unknown, so ``asleep_us`` is
``null`` rather than a number measured from an arbitrary sample 0.

Exit status::

    0  REPORT         a report was written (wake_observed true or, under
                      --strict-assert, a grounded false)
    2  USAGE          bad arguments, unreadable input, or no such channel
    3  INCONCLUSIVE   the capture does not support any claim; no report written

Usage::

    python3 scripts/hil/lib/ad2_sleep_probe.py --csv CAP.csv --channel 0 \
        [--sample-hz HZ] [--active-low] [--settle-samples N] \
        [--strict-assert] [--out probe.json]
    python3 scripts/hil/lib/ad2_sleep_probe.py --capture-ad2 --channel 0 \
        --sample-hz 1000 --seconds 3 [--out probe.json]
    python3 scripts/hil/lib/ad2_sleep_probe.py --selftest
"""

from __future__ import annotations

import argparse
import csv
import importlib.util
import io
import json
import os
import sys
import time

REPORT = 0
USAGE = 2
INCONCLUSIVE = 3

STATUS_NAME = {REPORT: "REPORT", USAGE: "USAGE", INCONCLUSIVE: "INCONCLUSIVE"}

# Version the consumer side can key on if this record ever grows a field.
SCHEMA = "ra8-hil-sleep-probe/1"

# Samples a level must hold before it counts as a real edge. Two is enough to
# drop a single-sample glitch without swallowing a genuine transition; the rig
# samples far faster than a core takes to enter or leave WFI.
DEFAULT_SETTLE_SAMPLES = 2

# libdwf reports failure as 0 and success as non-zero (its own convention).
DWF_FAILURE = 0
DWF_BAD_HANDLE = 0
DWF_ERROR_BUFFER_BYTES = 512
# FDwfDigitalInStatus device state: 2 == DwfStateDone.
DWF_STATE_DONE = 2
# AD2 digital-in single-shot buffer. Beyond this the SDK needs record mode,
# which streams and can overflow; this module refuses rather than silently
# truncating a window the caller believes it captured.
AD2_MAX_BUFFER_SAMPLES = 4096
AD2_STATUS_POLL_S = 0.01


def _asserted(level: int, active_low: bool) -> bool:
    """Read one sampled level as asserted, under the caller's polarity."""
    return level == 0 if active_low else level == 1


def _first_stable(levels: list[int], want: bool, start: int, active_low: bool, settle: int) -> int:
    """Index where ``want`` first holds for ``settle`` consecutive samples.

    Args:
        levels: Sampled line levels, 0 or 1, in sample order.
        want: True to look for the asserted state, False for released.
        start: First index to consider.
        active_low: Caller's polarity convention.
        settle: Samples the state must hold.

    Returns:
        The index of the first sample of that run, or -1 when no such run
        exists at or after ``start``.
    """
    run = 0
    for index in range(max(start, 0), len(levels)):
        if _asserted(levels[index], active_low) is want:
            run += 1
            if run >= settle:
                return index - settle + 1
        else:
            run = 0
    return -1


def classify_probe(
    levels: list[int],
    sample_hz: float,
    active_low: bool = False,
    settle_samples: int = DEFAULT_SETTLE_SAMPLES,
    strict_assert: bool = False,
) -> tuple[int, dict | None, str]:
    """Turn a sampled probe line into a report, or into a refusal to claim one.

    Args:
        levels: Sampled line levels, 0 or 1, in sample order.
        sample_hz: Sampling rate the levels were taken at.
        active_low: True when the line sits low while the core is asleep.
        settle_samples: Samples a level must hold to count as an edge.
        strict_assert: True when the caller guarantees the window covers the
            whole sleep plus the wake stimulus, which is what makes a line
            still asserted at the end mean "never woke" instead of "ran out of
            capture".

    Returns:
        ``(status, report_or_None, reason)``. A report is returned only with
        status ``REPORT``; every other status deliberately returns None so no
        caller can write a claim this capture does not support.
    """
    if sample_hz <= 0:
        return USAGE, None, "sample rate must be positive"
    if settle_samples < 1:
        return USAGE, None, "settle-samples must be at least 1"
    if not levels:
        return INCONCLUSIVE, None, "empty capture: no samples to read"

    began_asserted = _asserted(levels[0], active_low)
    assert_at = 0 if began_asserted else _first_stable(levels, True, 0, active_low, settle_samples)
    if assert_at < 0:
        return (
            INCONCLUSIVE,
            None,
            "the probe line was never asserted in this window: nothing marks a "
            "sleep, and a report claiming no wake would be scored as a failure",
        )

    release_at = _first_stable(levels, False, assert_at + 1, active_low, settle_samples)
    if release_at < 0:
        if not strict_assert:
            return (
                INCONCLUSIVE,
                None,
                "capture ends with the line still asserted: a core still asleep "
                "and a truncated capture look identical, so no claim is written",
            )
        report = _report(
            wake_observed=False,
            asleep_observed=True,
            assert_sample=None if began_asserted else assert_at,
            release_sample=None,
            asleep_us=None,
            sample_hz=sample_hz,
            samples=len(levels),
            active_low=active_low,
            settle_samples=settle_samples,
            reason=(
                "line still asserted at the end of a window the caller declared "
                "complete (--strict-assert): the core did not wake"
            ),
        )
        return REPORT, report, report["reason"]

    asleep_us = None
    if not began_asserted:
        asleep_us = (release_at - assert_at) * 1_000_000.0 / sample_hz

    reason = (
        "capture began mid-assertion, so the sleep start is unknown; the "
        "release is real and the core woke"
        if began_asserted
        else "line asserted, then released: the core slept and woke"
    )
    report = _report(
        wake_observed=True,
        asleep_observed=True,
        assert_sample=None if began_asserted else assert_at,
        release_sample=release_at,
        asleep_us=asleep_us,
        sample_hz=sample_hz,
        samples=len(levels),
        active_low=active_low,
        settle_samples=settle_samples,
        reason=reason,
    )
    return REPORT, report, reason


def _report(**fields: object) -> dict:
    """Assemble one probe record, schema tag first."""
    record: dict = {"schema": SCHEMA}
    record.update(fields)
    return record


def parse_waveforms_csv(text: str, channel: int) -> tuple[list[int], float | None, str]:
    """Read a WaveForms digital export into levels plus its derived rate.

    Args:
        text: The CSV file's contents.
        channel: DIO channel number to extract.

    Returns:
        ``(levels, sample_hz_or_None, error)``. ``error`` is non-empty exactly
        when the file could not be read as a capture of that channel, and the
        rate is None when the export carries no usable time column.
    """
    rows = [row for row in csv.reader(io.StringIO(text)) if row and not row[0].startswith("#")]
    if not rows:
        return [], None, "no data rows"

    header = [cell.strip() for cell in rows[0]]
    column = _channel_column(header, channel)
    if column < 0:
        return [], None, f"no column for DIO {channel} in header {header!r}"
    time_column = _time_column(header)

    levels: list[int] = []
    times: list[float] = []
    for row in rows[1:]:
        if len(row) <= column:
            continue
        level = _level(row[column])
        if level is None:
            return [], None, f"cell {row[column]!r} is not a digital level"
        levels.append(level)
        if time_column >= 0 and len(row) > time_column:
            try:
                times.append(float(row[time_column]))
            except ValueError:
                times.append(float("nan"))

    if not levels:
        return [], None, "header only, no samples"

    sample_hz = None
    if len(times) >= 2:
        span = times[-1] - times[0]
        if span > 0:
            sample_hz = (len(times) - 1) / span
    return levels, sample_hz, ""


def _channel_column(header: list[str], channel: int) -> int:
    """Locate the column holding one DIO channel, across export spellings."""
    wanted = {f"dio {channel}", f"dio{channel}", f"d{channel}", f"channel {channel}"}
    for index, cell in enumerate(header):
        if cell.strip().lower() in wanted:
            return index
    return -1


def _time_column(header: list[str]) -> int:
    """Locate the time column, or -1 when the export carries none."""
    for index, cell in enumerate(header):
        if cell.strip().lower().startswith("time"):
            return index
    return -1


def _level(cell: str) -> int | None:
    """Read one exported cell as 0 or 1, or None when it is neither."""
    text = cell.strip().lower()
    if text in {"1", "high", "true"}:
        return 1
    if text in {"0", "low", "false"}:
        return 0
    return None


# ---------------------------------------------------------------------------
# Live capture. Never executed in CI or in any sandbox: it needs an AD2.
# ---------------------------------------------------------------------------
def capture_ad2(channel: int, sample_hz: float, seconds: float) -> tuple[list[int], str]:
    """Sample one DIO line with the Analog Discovery 2 through ``libdwf``.

    Args:
        channel: DIO channel to read.
        sample_hz: Requested sampling rate.
        seconds: Capture window.

    Returns:
        ``(levels, error)``. ``error`` is non-empty exactly when no capture was
        taken, and names which of load / open / configure / acquire failed so
        the failure states its own fix.
    """
    import ctypes  # local: the offline path must not need the SDK present.

    samples = int(sample_hz * seconds)
    if samples < 1:
        return [], "capture window is shorter than one sample"
    if samples > AD2_MAX_BUFFER_SAMPLES:
        return [], (
            f"{samples} samples exceeds the AD2 single-shot buffer "
            f"({AD2_MAX_BUFFER_SAMPLES}); lower --sample-hz or --seconds"
        )

    try:
        dwf = ctypes.CDLL("libdwf.so")
    except OSError as exc:
        return [], f"cannot load libdwf.so ({exc}); re-run the ad2_tools Ansible role"

    handle = ctypes.c_int()
    if dwf.FDwfDeviceOpen(ctypes.c_int(-1), ctypes.byref(handle)) == DWF_FAILURE:
        return [], f"no AD2 opened: {_dwf_error(dwf)}"
    if handle.value == DWF_BAD_HANDLE:
        return [], "no AD2 opened: device handle is null"

    try:
        base_hz = ctypes.c_double()
        if dwf.FDwfDigitalInInternalClockInfo(handle, ctypes.byref(base_hz)) == DWF_FAILURE:
            return [], f"cannot read the digital-in clock: {_dwf_error(dwf)}"
        divider = max(int(base_hz.value / sample_hz), 1)
        ok = (
            dwf.FDwfDigitalInDividerSet(handle, ctypes.c_int(divider)) != DWF_FAILURE
            and dwf.FDwfDigitalInSampleFormatSet(handle, ctypes.c_int(16)) != DWF_FAILURE
            and dwf.FDwfDigitalInBufferSizeSet(handle, ctypes.c_int(samples)) != DWF_FAILURE
            and dwf.FDwfDigitalInConfigure(handle, ctypes.c_int(0), ctypes.c_int(1)) != DWF_FAILURE
        )
        if not ok:
            return [], f"cannot configure the digital-in instrument: {_dwf_error(dwf)}"

        deadline = time.monotonic() + seconds + 1.0
        state = ctypes.c_byte()
        while time.monotonic() < deadline:
            if dwf.FDwfDigitalInStatus(handle, ctypes.c_int(1), ctypes.byref(state)) == DWF_FAILURE:
                return [], f"acquisition failed: {_dwf_error(dwf)}"
            if state.value == DWF_STATE_DONE:
                break
            time.sleep(AD2_STATUS_POLL_S)
        else:
            return [], "acquisition never reached the done state within the window"

        raw = (ctypes.c_uint16 * samples)()
        if dwf.FDwfDigitalInStatusData(handle, raw, ctypes.c_int(samples * 2)) == DWF_FAILURE:
            return [], f"cannot read back the captured buffer: {_dwf_error(dwf)}"
        mask = 1 << channel
        return [1 if word & mask else 0 for word in raw], ""
    finally:
        dwf.FDwfDeviceClose(handle)


def _dwf_error(dwf: object) -> str:
    """Read the most recent libdwf error message."""
    import ctypes

    buf = ctypes.create_string_buffer(DWF_ERROR_BUFFER_BYTES)
    dwf.FDwfGetLastErrorMsg(buf)  # type: ignore[attr-defined]
    return buf.value.decode(errors="replace").strip() or "(no message)"


# ---------------------------------------------------------------------------
# Selftest: every branch, in both directions, plus the consumer tie.
# ---------------------------------------------------------------------------
def _levels(pattern: str) -> list[int]:
    """Build a level list from a compact ``"0001110"`` string."""
    return [1 if char == "1" else 0 for char in pattern]


def _load_consumer() -> object | None:
    """Import the sleep_verdict module this producer feeds, if it is present."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sleep_verdict.py")
    spec = importlib.util.spec_from_file_location("sleep_verdict", path)
    if spec is None or spec.loader is None:
        return None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _selftest_cases() -> list[tuple[str, dict, int, dict]]:
    """Return ``(name, kwargs, want_status, want_fields)`` for every branch."""
    return [
        (
            "asserted then released -> REPORT, wake observed",
            {"levels": _levels("0011111100"), "sample_hz": 1000.0},
            REPORT,
            {"wake_observed": True, "assert_sample": 2, "release_sample": 8, "asleep_us": 6000.0},
        ),
        (
            "never asserted -> INCONCLUSIVE, no report (not a false FAIL)",
            {"levels": _levels("0000000000"), "sample_hz": 1000.0},
            INCONCLUSIVE,
            {},
        ),
        (
            "still asserted at the end -> INCONCLUSIVE, no report",
            {"levels": _levels("0011111111"), "sample_hz": 1000.0},
            INCONCLUSIVE,
            {},
        ),
        (
            "still asserted under --strict-assert -> REPORT, grounded no-wake",
            {"levels": _levels("0011111111"), "sample_hz": 1000.0, "strict_assert": True},
            REPORT,
            {"wake_observed": False, "release_sample": None, "asleep_us": None},
        ),
        (
            "active-low capture read active-low -> REPORT",
            {"levels": _levels("1100000011"), "sample_hz": 1000.0, "active_low": True},
            REPORT,
            {"wake_observed": True, "assert_sample": 2, "release_sample": 8},
        ),
        (
            "polarity is a caller convention: read the wrong way round, the idle "
            "stretch is what gets reported as the sleep",
            {"levels": _levels("0011111100"), "sample_hz": 1000.0, "active_low": True},
            REPORT,
            {"wake_observed": True, "assert_sample": None, "asleep_us": None},
        ),
        (
            "a one-sample glitch is not an assertion",
            {"levels": _levels("0000100000"), "sample_hz": 1000.0},
            INCONCLUSIVE,
            {},
        ),
        (
            "a one-sample glitch inside the sleep is not a release",
            {"levels": _levels("0011101111000"), "sample_hz": 1000.0},
            REPORT,
            {"wake_observed": True, "assert_sample": 2, "release_sample": 10},
        ),
        (
            "settle-samples 1 does count that glitch, both directions",
            {"levels": _levels("0000100000"), "sample_hz": 1000.0, "settle_samples": 1},
            REPORT,
            {"wake_observed": True, "assert_sample": 4, "release_sample": 5},
        ),
        (
            "capture begins mid-assertion -> REPORT with no duration claimed",
            {"levels": _levels("1111110000"), "sample_hz": 1000.0},
            REPORT,
            {"wake_observed": True, "assert_sample": None, "asleep_us": None, "release_sample": 6},
        ),
        (
            "empty capture -> INCONCLUSIVE",
            {"levels": [], "sample_hz": 1000.0},
            INCONCLUSIVE,
            {},
        ),
        (
            "non-positive sample rate -> USAGE",
            {"levels": _levels("0011110000"), "sample_hz": 0.0},
            USAGE,
            {},
        ),
        (
            "settle-samples below one -> USAGE",
            {"levels": _levels("0011110000"), "sample_hz": 1000.0, "settle_samples": 0},
            USAGE,
            {},
        ),
    ]


def _selftest_classify(failures: list[str]) -> None:
    """Run every classification case and record the ones that disagree."""
    for name, kwargs, want, want_fields in _selftest_cases():
        status, report, reason = classify_probe(**kwargs)  # type: ignore[arg-type]
        if status != want:
            failures.append(
                f"{name}: want {STATUS_NAME.get(want, want)}, "
                f"got {STATUS_NAME.get(status, status)} ({reason})"
            )
            continue
        if status == REPORT and report is None:
            failures.append(f"{name}: REPORT with no record")
            continue
        if status != REPORT and report is not None:
            failures.append(f"{name}: {STATUS_NAME[status]} must not write a record")
            continue
        for key, want_value in want_fields.items():
            got = report.get(key) if report else None
            if got != want_value:
                failures.append(f"{name}: {key} want {want_value!r}, got {got!r}")


def _selftest_csv(failures: list[str]) -> None:
    """Parse a WaveForms-shaped export, both a good one and three bad ones."""
    good = "\n".join(
        [
            "# Digilent WaveForms Logic Analyzer",
            "Time (s),DIO 0,DIO 1",
            "0.000,0,1",
            "0.001,0,1",
            "0.002,1,1",
            "0.003,1,0",
            "0.004,0,0",
            "0.005,0,0",
        ]
    )
    levels, rate, err = parse_waveforms_csv(good, 0)
    if err or levels != [0, 0, 1, 1, 0, 0]:
        failures.append(f"csv: good export read as {levels!r} err={err!r}")
    if rate is None or abs(rate - 1000.0) > 1.0:
        failures.append(f"csv: derived rate {rate!r}, want about 1000")
    levels1, _, err1 = parse_waveforms_csv(good, 1)
    if err1 or levels1 != [1, 1, 1, 0, 0, 0]:
        failures.append(f"csv: channel 1 read as {levels1!r} err={err1!r}")
    for text, channel, what in [
        (good, 7, "absent channel"),
        ("Time (s),DIO 0\n0.000,maybe\n", 0, "non-level cell"),
        ("Time (s),DIO 0\n", 0, "header only"),
    ]:
        _, _, bad = parse_waveforms_csv(text, channel)
        if not bad:
            failures.append(f"csv: {what} was accepted")


def _selftest_consumer(failures: list[str]) -> None:
    """Tie the produced record to the verdict module that consumes it.

    The two halves are only useful together, so the producer proves its record
    actually moves the consumer off ``SLEEPING_UNPROBED``, in both directions.
    """
    consumer = _load_consumer()
    if consumer is None:
        failures.append("consumer tie: sleep_verdict.py could not be imported")
        return

    capture = b"lpm_ulpt: sleeping\n"
    marker = "lpm_ulpt: sleeping"

    unprobed, _ = consumer.classify(capture, marker)
    if unprobed != consumer.SLEEPING_UNPROBED:
        failures.append("consumer tie: unprobed silence is no longer SLEEPING_UNPROBED")

    _, woke, _ = classify_probe(_levels("0011111100"), 1000.0)
    status, reason = consumer.classify(capture, marker, probe=json.loads(json.dumps(woke)))
    if status != consumer.PASS:
        failures.append(f"consumer tie: a wake report did not pass ({reason})")

    _, never, _ = classify_probe(_levels("0011111111"), 1000.0, strict_assert=True)
    status, reason = consumer.classify(capture, marker, probe=json.loads(json.dumps(never)))
    if status != consumer.FAIL:
        failures.append(f"consumer tie: a grounded no-wake report did not fail ({reason})")

    for status_code, record, _ in [
        classify_probe(_levels("0000000000"), 1000.0),
        classify_probe(_levels("0011111111"), 1000.0),
    ]:
        if status_code == REPORT or record is not None:
            failures.append("consumer tie: a refusal leaked a record the consumer would score")


def _selftest() -> int:
    """Run every case and print one verdict line."""
    failures: list[str] = []
    _selftest_classify(failures)
    _selftest_csv(failures)
    _selftest_consumer(failures)

    for line in failures:
        print(f"  FAIL {line}")
    if failures:
        print(f"ad2_sleep_probe.py --selftest: FAIL ({len(failures)} case(s))")
        return 1
    print(
        f"ad2_sleep_probe.py --selftest: PASS ({len(_selftest_cases())} classification cases, "
        "4 export cases, and the sleep_verdict.py tie, every branch in both directions)"
    )
    return 0


def _emit(report: dict, out: str) -> int:
    """Write the record to ``out`` or stdout, and return the REPORT status."""
    text = json.dumps(report, indent=2, sort_keys=False) + "\n"
    if out:
        with open(out, "w", encoding="utf-8") as handle:
            handle.write(text)
        print(f"[ad2_sleep_probe] wrote {out}: {report['reason']}")
    else:
        sys.stdout.write(text)
    return REPORT


def main(argv: list[str]) -> int:
    """Parse arguments, obtain a capture, and write a report or refuse to."""
    ap = argparse.ArgumentParser(add_help=True, description=__doc__.splitlines()[0])
    ap.add_argument("--csv", default="", help="WaveForms digital export to read offline")
    ap.add_argument("--capture-ad2", action="store_true", help="sample live through libdwf")
    ap.add_argument("--channel", type=int, default=0, help="DIO channel carrying the probe line")
    ap.add_argument("--sample-hz", type=float, default=0.0, help="sampling rate")
    ap.add_argument("--seconds", type=float, default=2.0, help="live capture window")
    ap.add_argument("--active-low", action="store_true", help="the line sits low while asleep")
    ap.add_argument(
        "--settle-samples",
        type=int,
        default=DEFAULT_SETTLE_SAMPLES,
        help="samples a level must hold to count as an edge",
    )
    ap.add_argument(
        "--strict-assert",
        action="store_true",
        help="the window provably covers the sleep and the wake stimulus",
    )
    ap.add_argument("--out", default="", help="write the report here instead of stdout")
    ap.add_argument("--selftest", action="store_true", help="run the synthetic fixtures")
    args = ap.parse_args(argv)

    if args.selftest:
        return _selftest()
    if bool(args.csv) == bool(args.capture_ad2):
        ap.print_usage(sys.stderr)
        print("error: pass exactly one of --csv or --capture-ad2", file=sys.stderr)
        return USAGE

    sample_hz = args.sample_hz
    if args.csv:
        try:
            with open(args.csv, "r", encoding="utf-8", errors="replace") as handle:
                text = handle.read()
        except OSError as exc:
            print(f"error: cannot read capture {args.csv}: {exc}", file=sys.stderr)
            return USAGE
        levels, derived, err = parse_waveforms_csv(text, args.channel)
        if err:
            print(f"error: {args.csv}: {err}", file=sys.stderr)
            return USAGE
        if sample_hz <= 0:
            if derived is None:
                print("error: export carries no time column; pass --sample-hz", file=sys.stderr)
                return USAGE
            sample_hz = derived
    else:
        if sample_hz <= 0:
            print("error: --capture-ad2 needs --sample-hz", file=sys.stderr)
            return USAGE
        levels, err = capture_ad2(args.channel, sample_hz, args.seconds)
        if err:
            print(f"error: {err}", file=sys.stderr)
            return USAGE

    status, report, reason = classify_probe(
        levels,
        sample_hz,
        active_low=args.active_low,
        settle_samples=args.settle_samples,
        strict_assert=args.strict_assert,
    )
    if status != REPORT or report is None:
        print(f"[ad2_sleep_probe] {STATUS_NAME.get(status, status)}: {reason}", file=sys.stderr)
        return status
    report["source"] = "csv" if args.csv else "ad2"
    report["channel"] = args.channel
    return _emit(report, args.out)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
