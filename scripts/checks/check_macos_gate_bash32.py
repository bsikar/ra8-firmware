#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: the shell a Mac runs for a native-host gate stays bash 3.2 clean.

The defect class
----------------
macOS ships bash **3.2.57** as ``/bin/bash`` and has since 2007; Apple never
shipped bash 4 because of its licence.  This tree pins that interpreter twice
over: ``justfile`` sets ``shell := ["/bin/bash", "-puc"]`` and ``scripts/ci.sh``
carries ``#!/bin/bash -p``.  Every Linux box in this suite runs bash 5.

So a bash-4 construct in the CI shell is invisible everywhere it is written and
fails only on the one machine #899 exists to serve.  That is the same shape as
the issue itself -- Linux-green, Mac-red -- and the macOS gate is the only gate
whose whole purpose is to be believed when it is the only thing that ran.

Two hazards, and they are not the same
--------------------------------------
**Syntax** (``declare -A``, ``${v^^}``, ``|&``, ``coproc``, ``${v@Q}``, a
nameref) is a *parse* error.  Bash reads a sourced file whole before it runs a
line of it, so one of these anywhere in a parsed file kills the dispatch before
the gate starts, whatever the gate was going to do.

**Builtins** (``mapfile``, ``readarray``, ``wait -n``) parse fine on 3.2 and
die at the moment they run: ``mapfile: command not found``.  They are therefore
only a hazard in code the Mac actually executes, which is why they are scoped
to the native-host gate bodies rather than to the whole tree.  ``mapfile`` is
used today in ``scripts/ci/gates/analysis.sh`` and ``scripts/ci/gates/tests.sh``
-- both parsed on the Mac, neither run there, and both correctly quiet here.

Scope, derived rather than listed
---------------------------------
The parse set starts at ``scripts/ci.sh``, adds the gate bodies it sources with
an unconditional glob, adds the libraries ``just/ci_gate.just`` sources, and
follows every statically resolvable ``source`` / ``.`` from there.  The run set
is the body of each gate declared in ``RA8_NATIVE_HOST_GATES``
(``scripts/ci/lib/native_host_gates.sh``, #1073) plus the libraries that body
and ``ci.sh``'s startup source.  Add a native-host gate row and its body is
gated the same day; nothing has to be remembered.

Run::

    check_macos_gate_bash32.py             # gate (fail on any finding)
    check_macos_gate_bash32.py --selftest  # prove it fires AND stays quiet
    check_macos_gate_bash32.py --roster    # print the derived scope

Exit 0 if clean, exit 1 on findings, exit 2 on an internal error.
"""

from __future__ import annotations

import re
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

CI_ENTRY = "scripts/ci.sh"
GATE_DIR = "scripts/ci/gates"
JUST_ENTRY = "just/ci_gate.just"
NATIVE_HOST_LIB = "scripts/ci/lib/native_host_gates.sh"

# "name|uname -s|uname -m|why" -- a row needs at least the name and the OS.
NATIVE_HOST_ROW_MIN_FIELDS = 2

# `${SCRIPT_DIR}` in scripts/ci.sh is the directory holding it.
VAR_EXPANSIONS = {
    "SCRIPT_DIR": "scripts",
    "_RA8_CONTAINER_LIB_DIR": "scripts/ci/lib",
    "_RA8_GATE_DIR": "scripts/ci/gates",
}

_SOURCE_RE = re.compile(r"^\s*(?:source|\.)\s+(\S+)")
_VAR_RE = re.compile(r"\$\{?([A-Za-z_][A-Za-z_0-9]*)\}?")


class Hazard:
    """One bash-4 construct: how to spot it, and what to say about it."""

    def __init__(self, name: str, pattern: str, since: str, advice: str) -> None:
        """Record one construct, the bash it needs, and the 3.2-safe way out."""
        self.name = name
        self.regex = re.compile(pattern)
        self.since = since
        self.advice = advice


# Parse-time: bash 3.2 cannot READ these, so the whole file dies.
SYNTAX_HAZARDS = [
    Hazard(
        "associative array",
        r"\b(?:declare|local|typeset)\s+(?:-[a-zA-Z]*A[a-zA-Z]*)\b",
        "4.0",
        "use two indexed arrays, or a newline-separated 'key|value' table like RA8_GATE_REGISTRY",
    ),
    Hazard(
        "nameref",
        r"\b(?:declare|local|typeset)\s+(?:-[a-zA-Z]*n[a-zA-Z]*)\b",
        "4.3",
        "pass the value, or print it and capture with $(...)",
    ),
    Hazard(
        "global declare",
        r"\bdeclare\s+(?:-[a-zA-Z]*g[a-zA-Z]*)\b",
        "4.2",
        "assign at file scope instead",
    ),
    Hazard(
        "case-modifying expansion",
        r"\$\{[A-Za-z_][A-Za-z_0-9]*(?:\[[^]]*\])?(?:\^\^?|,,?)\}",
        "4.0",
        "pipe through tr '[:upper:]' '[:lower:]'",
    ),
    Hazard(
        "parameter transformation",
        r"\$\{[A-Za-z_][A-Za-z_0-9]*(?:\[[^]]*\])?@[QEPAKauL]\}",
        "4.4",
        "use printf %q for the quoting case",
    ),
    Hazard(
        "negative array index",
        r"\$\{[A-Za-z_][A-Za-z_0-9]*\[-[0-9]+\]\}",
        "4.2",
        "index with ${#arr[@]} arithmetic",
    ),
    Hazard(
        "append-redirect &>>",
        r"&>>",
        "4.0",
        "use >>file 2>&1",
    ),
    Hazard(
        "pipe-stderr |&",
        r"(?<![|&])\|&(?!\|)",
        "4.0",
        "use 2>&1 | instead",
    ),
    Hazard(
        "coproc",
        r"^\s*coproc\b",
        "4.0",
        "use a fifo or a temporary file",
    ),
    Hazard(
        "-v defined test",
        r"(?:\[\[|\btest\b|\[)\s+-v\s+[A-Za-z_]",
        "4.2",
        'use [ -n "${var+set}" ]',
    ),
    Hazard(
        "automatic fd allocation",
        r"\bexec\s+\{[A-Za-z_][A-Za-z_0-9]*\}[<>]",
        "4.1",
        "pick an explicit descriptor number",
    ),
    Hazard(
        "printf time format",
        r"%\([^)]*\)T",
        "4.2",
        "shell out to date",
    ),
]

# Run-time: these parse on 3.2 and fail the instant they execute.
BUILTIN_HAZARDS = [
    Hazard(
        "mapfile/readarray",
        r"^\s*(?:mapfile|readarray)\b",
        "4.0",
        "read a line at a time: while IFS= read -r line; do ... done < file",
    ),
    Hazard(
        "wait -n",
        r"\bwait\s+-n\b",
        "4.3",
        "wait for the specific pids you started",
    ),
    Hazard(
        "shopt globstar",
        r"\bshopt\s+-[su]\s+globstar\b",
        "4.0",
        "use find",
    ),
    Hazard(
        "shopt lastpipe",
        r"\bshopt\s+-[su]\s+lastpipe\b",
        "4.2",
        "restructure so the loop is not in a subshell",
    ),
]


def _read(rel: str) -> str:
    return (REPO_ROOT / rel).read_text(encoding="utf-8", errors="replace")


def _strip_comment(line: str) -> str:
    """Drop a trailing comment, honouring quotes well enough for this scan."""
    out: list[str] = []
    quote = ""
    prev = ""
    for ch in line:
        if quote:
            if ch == quote and prev != "\\":
                quote = ""
        elif ch in "'\"":
            quote = ch
        elif ch == "#" and (not out or out[-1].isspace()):
            break
        out.append(ch)
        prev = ch
    return "".join(out)


def _resolve(raw: str) -> str | None:
    """Turn a `source <word>` operand into a repo-relative path, or None."""
    word = raw.strip().strip('"').strip("'")
    if not word or word.startswith("$(") or "*" in word:
        return None

    def sub(match: re.Match[str]) -> str:
        name = match.group(1)
        if name in VAR_EXPANSIONS:
            return VAR_EXPANSIONS[name]
        return "\x00"

    word = _VAR_RE.sub(sub, word)
    if "\x00" in word or word.startswith("/"):
        return None
    candidate = (REPO_ROOT / word).resolve()
    try:
        rel = candidate.relative_to(REPO_ROOT).as_posix()
    except ValueError:
        return None
    return rel if (REPO_ROOT / rel).is_file() else None


def _sourced_by(rel: str) -> list[str]:
    found: list[str] = []
    for line in _read(rel).splitlines():
        match = _SOURCE_RE.match(_strip_comment(line))
        if not match:
            continue
        target = _resolve(match.group(1))
        if target and target != rel:
            found.append(target)
    return found


def parse_set() -> list[str]:
    """Every shell file bash reads whole when a gate is dispatched on a Mac."""
    seeds = [CI_ENTRY]
    # ci.sh sources its gate bodies with an unconditional directory glob, so
    # every one of them is parsed even though only one gate then runs.
    seeds += sorted(
        p.relative_to(REPO_ROOT).as_posix() for p in (REPO_ROOT / GATE_DIR).glob("*.sh")
    )
    # `just quality::local::gate` is the documented Mac entry point (#1073).
    if (REPO_ROOT / JUST_ENTRY).is_file():
        seeds += _sourced_by(JUST_ENTRY)

    seen: list[str] = []
    queue = list(seeds)
    while queue:
        rel = queue.pop(0)
        if rel in seen or not (REPO_ROOT / rel).is_file():
            continue
        seen.append(rel)
        queue.extend(_sourced_by(rel))
    return sorted(seen)


def native_host_gates() -> list[str]:
    """Gate names declared native-host-only in RA8_NATIVE_HOST_GATES (#1073)."""
    if not (REPO_ROOT / NATIVE_HOST_LIB).is_file():
        return []
    names: list[str] = []
    inside = False
    for line in _read(NATIVE_HOST_LIB).splitlines():
        if "RA8_NATIVE_HOST_GATES=" in line:
            inside = True
            continue
        if inside:
            if line.strip().startswith(("'", '"')) and line.strip().rstrip("'\"") == "":
                break
            stripped = _strip_comment(line).strip().strip("'\"")
            if stripped in ("", ")"):
                if stripped == ")":
                    break
                continue
            row = stripped.split("|")
            if len(row) >= NATIVE_HOST_ROW_MIN_FIELDS and row[0] and " " not in row[0]:
                names.append(row[0])
    return names


def gate_body(name: str) -> tuple[str, str] | None:
    """The shell text of gate_<name>, and the file it lives in."""
    fn = "gate_" + name.replace("-", "_") + "()"
    for path in sorted((REPO_ROOT / GATE_DIR).glob("*.sh")):
        rel = path.relative_to(REPO_ROOT).as_posix()
        text = _read(rel)
        start = text.find(fn)
        if start < 0:
            continue
        end = text.find("\n)\n", start)
        end = len(text) if end < 0 else end + 3
        return text[start:end], rel
    return None


def run_set() -> list[tuple[str, str, str]]:
    """(label, file, text) for every shell a Mac actually EXECUTES."""
    units: list[tuple[str, str, str]] = []
    # ci.sh's startup sources these for every dispatch, whichever gate runs.
    units.extend((rel, rel, _read(rel)) for rel in _sourced_by(CI_ENTRY))
    for name in native_host_gates():
        found = gate_body(name)
        if found is None:
            continue
        body, rel = found
        units.append((f"gate {name} (in {rel})", rel, body))
        units.extend((lib, lib, _read(lib)) for lib in _sourced_by(rel))
    return units


def _scan(text: str, hazards: list[Hazard], label: str) -> list[str]:
    findings: list[str] = []
    for number, raw in enumerate(text.splitlines(), start=1):
        line = _strip_comment(raw)
        if not line.strip():
            continue
        findings.extend(
            f"{label}:{number}: {hazard.name} needs bash {hazard.since}; "
            f"macOS /bin/bash is 3.2 -- {hazard.advice}\n      {raw.strip()}"
            for hazard in hazards
            if hazard.regex.search(line)
        )
    return findings


def scan_tree() -> list[str]:
    """Every finding on the macOS path: parse hazards first, then runtime ones."""
    findings: list[str] = []
    for rel in parse_set():
        findings.extend(_scan(_read(rel), SYNTAX_HAZARDS, rel))
    for label, _rel, text in run_set():
        findings.extend(_scan(text, BUILTIN_HAZARDS, label))
    return findings


# --------------------------------------------------------------------------
# selftest
# --------------------------------------------------------------------------

_BAD_SYNTAX = """#!/bin/bash
declare -A table
local -n alias_of=table
name="${raw^^}"
quoted="${raw@Q}"
last="${items[-1]}"
build &>>log.txt
producer |& consumer
coproc reader { cat; }
if [[ -v RA8_THING ]]; then :; fi
exec {fd}<file
printf '%(%Y)T\\n' -1
declare -g shared=1
"""

_GOOD_SYNTAX = """#!/bin/bash
# Every 3.2-safe equivalent the advice points at.
rows="alpha|1
beta|2"
name="$(printf '%s' "$raw" | tr '[:lower:]' '[:upper:]')"
quoted="$(printf '%q' "$raw")"
count=${#items[@]}
last="${items[$((count - 1))]}"
build >>log.txt 2>&1
producer 2>&1 | consumer
if [ -n "${RA8_THING+set}" ]; then :; fi
exec 9<file
now="$(date +%Y)"
# A comment about declare -A and ${v^^} must not fire.
echo "not a hazard: mapfile is a builtin, checked elsewhere"
"""

_BAD_BUILTIN = """#!/bin/bash
mapfile -t rows < input
readarray -d '' files < manifest
wait -n
shopt -s globstar
shopt -s lastpipe
"""

_GOOD_BUILTIN = """#!/bin/bash
while IFS= read -r line; do rows="${rows}${line}"; done < input
wait "$pid"
find . -name '*.sh'
"""


def _selftest_hazards() -> list[str]:
    """Every hazard fires on a fixture using it, and none fires on the way out."""
    failures: list[str] = []

    for label, fixture, hazards in (
        ("syntax", _BAD_SYNTAX, SYNTAX_HAZARDS),
        ("builtin", _BAD_BUILTIN, BUILTIN_HAZARDS),
    ):
        hits = _scan(fixture, hazards, "bad")
        names = {h.name for h in hazards}
        fired = {n for n in names if any(f": {n} needs" in hit for hit in hits)}
        missed = sorted(names - fired)
        if missed:
            failures.append(f"must-fire: no {label} finding for {', '.join(missed)}")

    for label, fixture, hazards in (
        ("syntax", _GOOD_SYNTAX, SYNTAX_HAZARDS),
        ("builtin", _GOOD_BUILTIN, BUILTIN_HAZARDS),
    ):
        quiet = _scan(fixture, hazards, "good")
        if quiet:
            failures.append(
                f"must-be-silent: the 3.2-safe {label} fixture produced {len(quiet)} finding(s)"
            )

    # The two kinds must stay apart. A builtin is not a parse error, and scoping
    # it to the parse set would condemn analysis.sh and tests.sh, which a Mac
    # reads but never runs.
    if _scan(_BAD_BUILTIN, SYNTAX_HAZARDS, "x"):
        failures.append("scope: a runtime builtin was reported as a parse error")
    if _scan(_BAD_SYNTAX, BUILTIN_HAZARDS, "x"):
        failures.append("scope: a parse error was reported as a runtime builtin")

    # The hazards must be found in code, not in prose.
    if _scan("# declare -A table\n", SYNTAX_HAZARDS, "c"):
        failures.append("comment: fired on a commented-out construct")
    if not _scan('echo "declare -A"  # tail comment\n', SYNTAX_HAZARDS, "c"):
        failures.append("comment: a tail comment swallowed the code before it")
    return failures


def _selftest_roster(parsed: list[str]) -> list[str]:
    """The scope is derived from the tree: it finds real files and invents none."""
    failures: list[str] = []
    required_files = (
        CI_ENTRY,
        "scripts/ci/gates/manual.sh",
        "scripts/ci/lib/lang_toolchains.sh",
        "scripts/ci/lib/native_host_gates.sh",
    )
    failures.extend(
        f"roster: {required} missing from the parse set"
        for required in required_files
        if required not in parsed
    )
    failures.extend(
        f"roster: {rel} does not exist" for rel in parsed if not (REPO_ROOT / rel).is_file()
    )

    with tempfile.TemporaryDirectory(dir=REPO_ROOT) as tmp:
        leaf = Path(tmp) / "leaf.sh"
        leaf.write_text("echo leaf\n", encoding="utf-8")
        mid = Path(tmp) / "mid.sh"
        mid.write_text(f". {leaf.relative_to(REPO_ROOT).as_posix()}\n", encoding="utf-8")
        if leaf.relative_to(REPO_ROOT).as_posix() not in _sourced_by(
            mid.relative_to(REPO_ROOT).as_posix()
        ):
            failures.append("roster: a plain `. path` source was not followed")
        missing = Path(tmp) / "missing.sh"
        # A deliberately absent target, so the resolver has something to refuse.
        missing.write_text(  # PATHREF-OK: must not exist; that is the case
            ". scripts/ci/lib/not_a_real_lib.sh\n", encoding="utf-8"
        )
        if _sourced_by(missing.relative_to(REPO_ROOT).as_posix()):
            failures.append("roster: resolved a source target that does not exist")
    return failures


def _selftest_registry(gates: list[str]) -> list[str]:
    """The native-host gate list is read from #1073's registry, not assumed."""
    failures: list[str] = []
    if "macos-host-build" not in gates:
        failures.append("registry: macos-host-build not read from RA8_NATIVE_HOST_GATES")
    if gate_body("macos-host-build") is None:
        failures.append("registry: the macos-host-build body was not located")
    if gate_body("not-a-real-gate-name") is not None:
        failures.append("registry: located a body for a made-up gate")
    return failures


def _selftest() -> int:
    """Prove the detector fires, stays quiet, derives its scope, and is clean now."""
    parsed = parse_set()
    gates = native_host_gates()
    failures = _selftest_hazards() + _selftest_roster(parsed) + _selftest_registry(gates)

    live = scan_tree()
    if live:
        failures.append(f"live: {len(live)} finding(s) on the current tree")

    for line in failures:
        sys.stderr.write(f"check_macos_gate_bash32.py: SELFTEST FAIL -- {line}\n")
    if failures:
        return 1
    sys.stdout.write(
        f"check_macos_gate_bash32.py: selftest OK ({len(SYNTAX_HAZARDS)} syntax and "
        f"{len(BUILTIN_HAZARDS)} builtin hazards fire, 3.2-safe forms stay quiet, "
        f"{len(parsed)} parsed file(s), {len(gates)} native-host gate(s))\n"
    )
    return 0


def main() -> int:
    """Entry point: `--selftest` proves non-vacuity, otherwise gate the tree."""
    args = sys.argv[1:]
    if "--selftest" in args:
        return _selftest()
    if "--roster" in args:
        sys.stdout.write("parsed on a Mac for any gate dispatch:\n")
        for rel in parse_set():
            sys.stdout.write(f"  {rel}\n")
        sys.stdout.write("executed on a Mac for a native-host gate:\n")
        for label, _rel, _text in run_set():
            sys.stdout.write(f"  {label}\n")
        return 0
    findings = scan_tree()
    if findings:
        sys.stderr.write("check_macos_gate_bash32.py: bash 4 construct(s) on the macOS path:\n\n")
        for finding in findings:
            sys.stderr.write(f"  {finding}\n\n")
        sys.stderr.write(
            f"{len(findings)} finding(s). macOS ships bash 3.2.57 as /bin/bash, which\n"
            "justfile and scripts/ci.sh both pin, so these read as correct on every\n"
            "Linux box in this suite and break only the one machine #899 is about.\n"
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
