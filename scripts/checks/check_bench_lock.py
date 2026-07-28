#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: everything that touches the bench takes the bench lock first.

Why this is a gate and not a convention
---------------------------------------
There is exactly ONE EK-RA8D2, and the owner, another maintainer, ~20
concurrent agents over ssh and a nightly CI job all reach it. Before #497
nothing serialised that at all -- no flock, no lockfile, no PID file anywhere
in the tree. ``scripts/hil/bench.sh`` fixed the mechanism; this fixes the
enforcement, because a lock nobody is forced to take is decoration.

The set of bench-touching entry points is DERIVED, never listed. Anything that
invokes ``JLinkExe``, ``JLinkGDBServer``, ``rfp-cli``, ``openocd``,
``esptool``, ``uhubctl`` or ``tapo_control.py``, opens a ``/dev/ttyACM*``
console, or shells into the bench host over ``ssh $PI_HOST``, is a bench
operation by construction. A hand-maintained list would go stale the first time
somebody added a script -- exactly the way the deleted HIL suite runner held a
stale table of 18 apps against 151 real ones.

What counts as guarded
----------------------
- the file sources ``lib/bench_lock.sh`` AND calls ``ra8_bench_require`` (or
  the ``_recovery`` form), or
- the file drives the hardware through ``bench.sh run --``, which is the same
  hold in wrapper form, or
- the file is on :data:`CARVE_OUTS` **with a stated reason**. A carve-out
  without a reason is a rejected carve-out; the reason is what a reviewer
  argues with.

Matching is invocation-shaped, not substring-shaped: a tool name has to appear
in COMMAND position (start of a line, or after a pipe, ``&&``, ``;``, ``sudo``,
``exec`` and friends). Every file in this tree that mentions ``JLinkExe`` in
prose -- and most of them do, because the comments are good -- would otherwise
be flagged, and a gate that cries wolf gets a blanket carve-out list within a
week.

Non-vacuity
-----------
This repo's dominant tooling defect is a checker that quietly stopped matching
and reported a clean tree forever; it has happened at least four times.
``--selftest`` therefore asserts three things, not one:

1. a synthetic script that shells out to JLinkExe with no guard must FAIL;
2. the same script with the guard call must PASS, as must one that only
   mentions the tool in a comment or an echo;
3. a DISCOVERY FLOOR -- the live scan must still find at least
   :data:`DISCOVERY_FLOOR` bench-touching files, and must still find every
   file in :data:`MUST_DISCOVER`. A regex that stopped matching turns those
   red instead of turning the tree green.

Run::

    check_bench_lock.py             # gate
    check_bench_lock.py --selftest  # prove the detector, both ways, plus floor
    check_bench_lock.py --list      # what it considers bench-touching, and why

Exit 0 clean, 1 on a violation or a failing selftest, 2 when the tree cannot
be read.
"""

from __future__ import annotations

import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

EXIT_OK = 0
EXIT_FAIL = 1
EXIT_CONFIG = 2

# Files whose CONTENT is scanned. Everything else in the tree either cannot
# invoke a tool (headers, C sources, fixtures) or reaches the hardware only by
# calling one of these -- and the delegate is what carries the guard.
SCANNED_SUFFIXES = (".sh", ".py", ".mk", ".yml", ".yaml")
SCANNED_BASENAMES = ("Makefile",)

# Directories that are somebody else's code, or generated, or not first-party.
EXCLUDE_FRAGMENTS = (
    "libs/third_party/",
    "port/threadx/",
    "coprocessor/esp32c6/esp-hosted-mcu/",
    "recon/",
)

# The tools. Each is matched only in COMMAND position -- see _command_word_re.
BENCH_TOOLS = (
    "JLinkExe",
    "JLinkGDBServer",
    "rfp-cli",
    "openocd",
    "uhubctl",
    "esptool",
    "esptool.py",
    "tapo_control.py",
)

# Words that may legitimately precede a command without changing the fact that
# it is being invoked. `command` is deliberately NOT here: `command -v JLinkExe`
# is an availability test, and treating it as an invocation would flag every
# script that checks for its own dependency.
_PREFIXES = (
    r"(?:sudo(?:\s+-n)?|nohup|setsid|exec|time|timeout\s+\S+|env"
    r"|python3?\s+-m|bash|sh)"
)
# What can appear immediately before a command word. A bare `(` is NOT here:
# `# ... power-cycle (uhubctl) then flash` is prose, and subshell-wrapped
# invocations of these tools do not occur in this tree.
_CMD_START = r"(?:^|[|;&`]|\$\(|&&|\|\||\bthen\b|\bdo\b|\bif\b|\belif\b|\bcmd:|--\s)"


def _command_word_re(tool: str) -> re.Pattern[str]:
    """A regex matching *tool* in command position, allowing a path prefix."""
    esc = re.escape(tool)
    return re.compile(rf"{_CMD_START}\s*!?\s*(?:{_PREFIXES}\s+)*(?:[\w./$'\"{{}}-]*/)?{esc}\b")


TOOL_RES = tuple((t, _command_word_re(t)) for t in BENCH_TOOLS)

# Availability tests, which are not invocations. `if ! command -v JLinkExe`
# appears in most of these scripts and means the opposite of driving the board.
PRESENCE_RE = re.compile(
    r"\b(?:command\s+-v|which|type\s+-?\w?)\s+\S*(?:"
    + "|".join(re.escape(t) for t in BENCH_TOOLS)
    + r")\b"
)

# A console OPEN, not a mention. `for d in /dev/ttyACM*` in a presence test is
# not an open; `stty -F /dev/ttyACM0` and `cat /dev/ttyACM0` are.
TTY_OPEN_RE = re.compile(
    r"(?:\b(?:stty|cat|dd|fuser|tee|screen|minicom|picocom)\b[^\n]*|[<>]\s*)"
    r"/dev/ttyACM"
)

# Shelling into the bench host. The remote command is a bench operation by
# definition -- that host exists to hold the board.
SSH_PI_RE = re.compile(r"\bssh\b[^\n]*\$\{?PI_HOST\b|\bssh\b[^\n]*\"\$PI\"")

# The guard, in any of its accepted forms.
GUARD_RE = re.compile(
    r"\bra8_bench_require(?:_recovery)?\b"
    r"|\bbench\.sh\s+(?:run|acquire)\b"
    r"|\bbench_host\.sh\s+hold\b"
)

# --------------------------------------------------------------------------
# Carve-outs. Every entry states WHY, because the reason is the thing a
# reviewer argues with; a bare path is an exemption nobody can evaluate.
# --------------------------------------------------------------------------
CARVE_OUTS: dict[str, str] = {
    # The lock itself. Guarding the guard is a deadlock, not a safety property.
    "scripts/hil/bench.sh": "IS the lock CLI -- it cannot take a lock to take a lock",
    "scripts/hil/lib/bench_client.sh": (
        "IS the lock's client transport; its ssh to the bench host is how a "
        "hold is taken in the first place"
    ),
    # Bootstrap: this is how JLINK_SN gets into .env in the first place, so it
    # must work before any rig config exists. ShowEmuList enumerates attached
    # probes; it does not connect to, halt, or program the target.
    "scripts/hil/find_jlink.sh": (
        "bootstrap -- enumerates attached probes to populate .env, before the "
        "rig is configured at all; it never connects to the target"
    ),
    # The one recovery path that must survive the rig being down.
    "scripts/hil/dlm_reset_local.sh": (
        "the board has been physically moved off the rig onto this "
        "workstation, so the bench host is not in the path; requiring a lock "
        "held on that host would make recovery impossible exactly when the "
        "rig is the thing that is broken"
    ),
    "scripts/checks/check_no_antirecovery.py": (
        "a checker whose PATTERNS name the tools; it invokes nothing"
    ),
    # The contention harness. It must NOT hold the lock: it drives several
    # independent machines that compete for it, and a driver holding the thing
    # under test would prevent the very contention it exists to measure. It
    # touches no hardware itself -- it ships a read-only /proc witness to the
    # bench host, reads the journal, and leaves every actual bench operation to
    # the actors, each of which goes through the guard.
    "scripts/hil/bench_contention.sh": (
        "drives the contention EXPERIMENT; holding the lock would prevent the "
        "contention it measures. It never touches hardware -- the actors it "
        "launches each take the lock through the ordinary guard"
    ),
    # The negative control, and the only file in the tree allowed to reach the
    # bench unguarded. Without it, "the witness saw no collision" could equally
    # mean "the witness sees nothing", which is this repo's most common tooling
    # failure. It is read-only, refuses to run without an explicit opt-in, and
    # refuses outright while anybody holds the lock.
    "scripts/hil/bench_unguarded_probe.sh": (
        "IS the negative control -- it proves the bench witness can see two "
        "machines on the board at once, which is what makes a clean guarded "
        "run mean anything. Read-only, gated behind "
        "RA8_BENCH_NEGATIVE_CONTROL=1, and refuses while the bench is held"
    ),
}

# --------------------------------------------------------------------------
# Anti-rot. A detector that stops matching must FAIL, not report a clean tree.
# --------------------------------------------------------------------------
DISCOVERY_FLOOR = 20

MUST_DISCOVER = (
    "scripts/hil/flash.sh",
    "scripts/hil/run_direct.sh",
    "scripts/hil/recover.sh",
    "scripts/hil/erase.sh",
    "scripts/hil/dlm_reset.sh",
    "scripts/hil/flash_retry.sh",
    "scripts/hil/probe.sh",
    "scripts/hil/jlink_memprobe.sh",
    "scripts/hil/ppps.sh",
    "scripts/dev/flash.sh",
    "scripts/dev/debug.sh",
    "scripts/dev/openocd_flash.sh",
    "coprocessor/esp32c6/flash.sh",
)


def _tracked_files() -> list[str]:
    """Every tracked path, so a file that is not committed is not in scope."""
    out = subprocess.run(
        ["git", "ls-files"],  # noqa: S607 -- git from PATH is intended
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    if out.returncode != 0:
        sys.stderr.write("check_bench_lock.py: `git ls-files` failed\n")
        raise SystemExit(EXIT_CONFIG)
    return [line for line in out.stdout.splitlines() if line]


def _in_scope(rel: str) -> bool:
    if any(frag in rel for frag in EXCLUDE_FRAGMENTS):
        return False
    name = rel.rsplit("/", 1)[-1]
    return name in SCANNED_BASENAMES or rel.endswith(SCANNED_SUFFIXES)


def _strip_comments(text: str) -> list[tuple[int, str]]:
    """Return (1-based line number, code) for lines that are not pure comments.

    Only whole-line comments are dropped. A trailing comment is left alone --
    stripping it properly needs a shell parser, and the command-position match
    below does not fire on prose anyway.
    """
    kept: list[tuple[int, str]] = []
    for n, line in enumerate(text.splitlines(), start=1):
        stripped = line.lstrip()
        if stripped.startswith("#"):
            continue
        kept.append((n, line))
    return kept


def bench_touches(text: str) -> list[tuple[int, str]]:
    """Every (line number, reason) at which *text* drives the bench."""
    hits: list[tuple[int, str]] = []
    for n, line in _strip_comments(text):
        if PRESENCE_RE.search(line):
            continue
        for tool, rx in TOOL_RES:
            if rx.search(line):
                hits.append((n, f"invokes {tool}"))
                break
        else:
            if TTY_OPEN_RE.search(line):
                hits.append((n, "opens a /dev/ttyACM* console"))
            elif SSH_PI_RE.search(line):
                hits.append((n, "runs a command on the bench host over ssh"))
    return hits


def is_guarded(text: str) -> bool:
    """True when *text* takes the bench lock in any of its accepted forms."""
    return any(GUARD_RE.search(line) for _, line in _strip_comments(text))


def scan(files: list[str]) -> tuple[dict[str, list[tuple[int, str]]], list[str]]:
    """Return {rel: hits} for every bench-touching file, and the unguarded ones."""
    touching: dict[str, list[tuple[int, str]]] = {}
    unguarded: list[str] = []
    for rel in files:
        if not _in_scope(rel):
            continue
        path = REPO_ROOT / rel
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        hits = bench_touches(text)
        if not hits:
            continue
        touching[rel] = hits
        if rel in CARVE_OUTS:
            continue
        if not is_guarded(text):
            unguarded.append(rel)
    return touching, unguarded


# --------------------------------------------------------------------------
# selftest
# --------------------------------------------------------------------------

_FIRE_UNGUARDED = """#!/usr/bin/env bash
set -euo pipefail
APP="$1"
JLinkExe -nogui 1 -CommanderScript /tmp/x.jlink
"""

_QUIET_GUARDED = """#!/usr/bin/env bash
set -euo pipefail
source "$_hil_dir/lib/bench_lock.sh"
ra8_bench_require "flash $1" || exit $?
JLinkExe -nogui 1 -CommanderScript /tmp/x.jlink
"""

_QUIET_RECOVERY = """#!/usr/bin/env bash
source "$_hil_dir/lib/bench_lock.sh"
ra8_bench_require_recovery "erase" || exit $?
rfp-cli -d ra -erase-chip
"""

_QUIET_WRAPPED = """#!/usr/bin/env bash
bash scripts/hil/bench.sh run --intent "probe" -- JLinkExe -nogui 1
"""

_QUIET_PROSE = """#!/usr/bin/env bash
# JLinkExe is what scripts/dev/flash.sh runs; rfp-cli does the DLM reset.
echo "  flash        runs JLinkExe via scripts/dev/flash.sh"
bash "$ROOT/scripts/dev/flash.sh" "$HEX"
"""

_QUIET_TTY_PRESENCE = """#!/usr/bin/env bash
for _d in /dev/ttyACM*; do
  [ -e "$_d" ] && return 0
done
"""

_FIRE_TTY_OPEN = """#!/usr/bin/env bash
stty -F /dev/ttyACM0 115200 raw -echo
cat /dev/ttyACM0 > /tmp/log
"""

_FIRE_SSH = """#!/usr/bin/env bash
ssh "$PI_HOST" "rfp-cli -d ra -erase-chip"
"""

_FIRE_SUDO_PREFIX = """#!/usr/bin/env bash
sudo -n uhubctl -l 2-1.3 -p 2 -a off
"""

SELFTEST_CASES: tuple[tuple[str, str, bool, bool, str], ...] = (
    # (name, body, must_be_detected, must_be_guarded, label)
    ("fire_unguarded.sh", _FIRE_UNGUARDED, True, False, "a bare JLinkExe call is caught"),
    ("quiet_guarded.sh", _QUIET_GUARDED, True, True, "ra8_bench_require satisfies it"),
    (
        "quiet_recovery.sh",
        _QUIET_RECOVERY,
        True,
        True,
        "the recovery guard satisfies it",
    ),
    ("quiet_wrapped.sh", _QUIET_WRAPPED, True, True, "`bench.sh run --` satisfies it"),
    ("quiet_prose.sh", _QUIET_PROSE, False, False, "prose and echo text do not fire"),
    (
        "quiet_tty_presence.sh",
        _QUIET_TTY_PRESENCE,
        False,
        False,
        "a /dev/ttyACM* presence test is not an open",
    ),
    ("fire_tty_open.sh", _FIRE_TTY_OPEN, True, False, "opening a console is caught"),
    ("fire_ssh.sh", _FIRE_SSH, True, False, "ssh onto the bench host is caught"),
    (
        "fire_sudo_prefix.sh",
        _FIRE_SUDO_PREFIX,
        True,
        False,
        "a sudo-prefixed uhubctl is caught",
    ),
)


def _selftest_detector() -> list[str]:
    failures: list[str] = []
    with tempfile.TemporaryDirectory():
        for name, body, want_hit, want_guard, label in SELFTEST_CASES:
            hits = bench_touches(body)
            if bool(hits) != want_hit:
                verb = "did not fire" if want_hit else "fired"
                failures.append(f"  detector {verb} (unexpected): {name} -- {label}")
                continue
            if not want_hit:
                continue
            if is_guarded(body) != want_guard:
                verb = "not recognised as guarded" if want_guard else "wrongly guarded"
                failures.append(f"  guard {verb}: {name} -- {label}")
    return failures


def _selftest_floor(touching: dict[str, list[tuple[int, str]]]) -> list[str]:
    failures: list[str] = []
    if len(touching) < DISCOVERY_FLOOR:
        failures.append(
            f"  DISCOVERY FLOOR: found {len(touching)} bench-touching file(s), "
            f"floor is {DISCOVERY_FLOOR}. The detector has stopped matching; "
            f"a clean tree is NOT the explanation."
        )
    missing = [rel for rel in MUST_DISCOVER if rel not in touching]
    if missing:
        failures.append(
            "  MUST_DISCOVER: these drive the bench and were not detected -- "
            "the detector is broken, or they were renamed:"
        )
        failures.extend(f"    {rel}" for rel in missing)
    return failures


def run_selftest() -> int:
    """Prove the detector fires, stays quiet, and still discovers the tree."""
    touching, _ = scan(_tracked_files())
    failures = _selftest_detector() + _selftest_floor(touching)
    if failures:
        sys.stderr.write("check_bench_lock.py: --selftest FAILED:\n")
        sys.stderr.write("\n".join(failures) + "\n")
        return EXIT_FAIL
    print(
        f"check_bench_lock.py: --selftest OK "
        f"({len(SELFTEST_CASES)} detector cases both directions; live scan finds "
        f"{len(touching)} bench-touching file(s), floor {DISCOVERY_FLOOR}, "
        f"{len(MUST_DISCOVER)} named files all present)."
    )
    return EXIT_OK


def main(argv: list[str]) -> int:
    """Gate entry point: --selftest, --list, or the scan itself."""
    if "--selftest" in argv:
        return run_selftest()

    touching, unguarded = scan(_tracked_files())

    if "--list" in argv:
        for rel in sorted(touching):
            mark = "CARVE-OUT" if rel in CARVE_OUTS else "guarded  "
            if rel in unguarded:
                mark = "UNGUARDED"
            print(f"{mark}  {rel}")
            for line_no, why in touching[rel][:3]:
                print(f"             line {line_no}: {why}")
            if rel in CARVE_OUTS:
                print(f"             reason: {CARVE_OUTS[rel]}")
        return EXIT_OK

    # A carve-out for a file that no longer drives the bench is dead weight
    # that hides the next real one, so it is an error in its own right.
    stale = [rel for rel in CARVE_OUTS if rel not in touching]
    stale = [rel for rel in stale if (REPO_ROOT / rel).exists()]

    if unguarded or stale:
        if unguarded:
            sys.stderr.write(
                "check_bench_lock.py: file(s) drive the bench without taking the lock:\n"
            )
            for rel in sorted(unguarded):
                sys.stderr.write(f"\n  {rel}\n")
                for line_no, why in touching[rel][:4]:
                    sys.stderr.write(f"    line {line_no}: {why}\n")
            sys.stderr.write(
                "\nAdd the guard immediately before the first hardware operation:\n"
                "    # shellcheck source=scripts/hil/lib/bench_lock.sh\n"
                '    source "$_hil_dir/lib/bench_lock.sh"\n'
                '    ra8_bench_require "<what you are doing>" || exit $?\n'
                "\nRecovery paths (erase, DLM reset, power cycle) use\n"
                "ra8_bench_require_recovery instead -- recovery is MORE\n"
                "destructive than a normal flash, not less, so it is not exempt.\n"
                "\nIf it genuinely cannot take the lock, add it to CARVE_OUTS in\n"
                "this file WITH A REASON. A reason a reviewer would reject is not\n"
                "a reason.\n"
            )
        if stale:
            sys.stderr.write(
                "\ncheck_bench_lock.py: stale carve-out(s) -- these no longer drive\n"
                "the bench, so the exemption is hiding nothing but itself:\n"
            )
            for rel in sorted(stale):
                sys.stderr.write(f"  {rel}\n")
        return EXIT_FAIL

    print(
        f"check_bench_lock.py: {len(touching)} bench-touching file(s), all guarded "
        f"or carved out with a reason ({len(CARVE_OUTS)} carve-out(s))."
    )
    return EXIT_OK


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
