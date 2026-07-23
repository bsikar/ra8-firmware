#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: no first-party file may introduce a permanent anti-recovery brick.

Owner policy (2026-07-23): this is a personal, open-source project. It must
NEVER contain code that permanently disables device recovery on the RA8D2.
Nothing sets any of this today -- the point of this gate is to keep it that
way forever, by failing CI the moment a change would.

What is FORBIDDEN (a *violation* -- the gate fails)
---------------------------------------------------
Only *actions* that move an RA8D2 into a state its own recovery scripts cannot
undo. Three shapes, all as executed commands / programmatic writes, never as
prose:

  * Setting the ``ce`` Security Flag ("Disable Initialize Command") -- via
    ``rfp-cli`` (``-set-security-flag ce``), an option-byte / OSIS / security
    register write, or any ``set ce`` / ``ce = true`` / ``--disable-initialize``
    programmatic path. That flag permanently disables the boot-firmware
    Initialize (erase-chip) recovery that ``scripts/hil/dlm_reset.sh`` relies on.
  * Transitioning the DLM to a terminal/unrecoverable lock -- ``LCK_BOOT`` (or
    any lock that renders the SWD-DP permanently unresponsive), e.g.
    ``rfp-cli -dlm LCK_BOOT`` or a programmatic ``dlm_program(k_dlm_lck_boot)``.
  * Any ``rfp-cli -dlm <state>`` / security-flag write that moves the device to
    a state the recovery scripts (``scripts/hil/dlm_reset.sh`` /
    ``recover.sh``) cannot undo.

What is ALLOWED (must NOT be flagged -- these are pro-recovery / defensive)
--------------------------------------------------------------------------
  * The recovery scripts themselves (``scripts/hil/dlm_reset.sh``,
    ``dlm_reset_local.sh``, ``recover.sh``, ``reflash.sh``). They READ / CHECK
    that ``ce`` is unset and WARN if recovery failed -- the exact opposite of
    the forbidden action. They are excluded by path AND the patterns below do
    not fire on their content (the ``--selftest`` proves both).
  * Reading / checking DLM state (``rfp-cli -rfo``), and *recoverable* DLM
    transitions (``OEM_PL0`` / ``OEM_PL1`` debug-lock, which the Initialize
    command or an authenticated regression can undo).
  * Any comment, warning string, or negated phrasing that merely *discusses*
    the danger: "verify ``ce`` is unset", "``ce`` may be set", "must NOT have
    been set", "do NOT run ``rfp-cli -dlm LCK_BOOT``".

How the ACTION / not-an-action distinction is drawn
---------------------------------------------------
Comments are blanked before matching, so a comment that shows the forbidden
command as a warning cannot trip the gate. The patterns split into two tiers:

  * HARD -- unambiguous executed commands / writes (``rfp-cli -dlm LCK_BOOT``,
    ``set ce``, ``ce = true``, ``-set-security-flag ce``, ``--disable-initialize``).
    These fire on any non-comment occurrence; a comment nearby does not excuse
    an action.
  * SOFT -- occurrences of a terminal-state NAME near a set-verb (spaced
    "disable initialize", "... regress ... LCK_BOOT"). A NAME is a violation
    only when nothing on the same code line marks it as a check / negation
    (``DEFENSIVE_RE``). The defensive cues are word-boundaried, so a snake_case
    or camelCase identifier that merely contains "never" / "verify" does NOT
    earn the exemption -- a function that really does the transition is still a
    violation regardless of what it is named.

Documentation files (``.md`` / ``.txt``) are prose, not executed, so every
match in them is treated as SOFT: a doc that WARNS against the brick stays
clean, while a doc that advocates running it does not.

Run::

    check_no_antirecovery.py             # scan the whole tracked tree
    check_no_antirecovery.py FILE ...    # scan listed files
    check_no_antirecovery.py --selftest  # prove the detector fires on brick
                                         # actions and stays silent on the real
                                         # recovery scripts + defensive checks

Exit 0 when no anti-recovery action is found, 1 otherwise (or on a failing
selftest, or on an empty/unreadable scan set).
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

# First-party scope: everything git tracks, minus vendored / generated / binary
# reference trees and the recovery scripts, which are pro-recovery by design.
EXCLUDED_PREFIXES: tuple[str, ...] = (
    "libs/third_party/",
    "libs/fonts/",
    "docs/reference/",
)

# The recovery scripts READ/CHECK ce and WARN on failure -- the opposite of the
# forbidden action. Excluded by path; --selftest additionally proves the
# patterns do not fire on their content.
RECOVERY_SCRIPTS: frozenset[str] = frozenset(
    {
        "scripts/hil/dlm_reset.sh",
        "scripts/hil/dlm_reset_local.sh",
        "scripts/hil/recover.sh",
        "scripts/hil/reflash.sh",
    }
)

# This checker must spell every forbidden pattern (in the regexes and in the
# selftest fixtures) to describe them, so it exempts itself -- exactly as
# check_no_wave_references.py exempts itself and the policy doc.
SELF_PATH = "scripts/checks/check_no_antirecovery.py"

EXCLUDED_FILES: frozenset[str] = RECOVERY_SCRIPTS | {SELF_PATH}

# Comment style per language. "hash": # to EOL. "c": /* */ blocks and // to EOL.
# "xml": <!-- --> blocks. "prose": no comment stripping, and every match is
# treated as SOFT (docs are described, not executed). "none": no comment style.
HASH_EXTS: frozenset[str] = frozenset(
    {".sh", ".bash", ".py", ".yml", ".yaml", ".cmake", ".toml", ".cfg", ".conf", ".ini", ".mk"}
)
C_EXTS: frozenset[str] = frozenset({".c", ".h", ".cpp", ".hpp", ".cc", ".hh", ".cxx", ".ld"})
XML_EXTS: frozenset[str] = frozenset({".xml", ".html", ".svg", ".rpj"})
PROSE_EXTS: frozenset[str] = frozenset({".md", ".markdown", ".txt", ".rst"})
NONE_EXTS: frozenset[str] = frozenset({".json"})

HASH_BASENAMES: frozenset[str] = frozenset({"Makefile", "CMakeLists.txt", "Dockerfile"})

# Extensionless basenames that are still shell/text worth scanning (the git
# hooks: scripts/git/pre-commit, pre-push).
HASH_STEM_HINTS: frozenset[str] = frozenset({"pre-commit", "pre-push"})

# Extension -> comment/prose class, assembled once.
_EXT_LANG: dict[str, str] = {
    **dict.fromkeys(HASH_EXTS, "hash"),
    **dict.fromkeys(C_EXTS, "c"),
    **dict.fromkeys(XML_EXTS, "xml"),
    **dict.fromkeys(PROSE_EXTS, "prose"),
    **dict.fromkeys(NONE_EXTS, "none"),
}

# --- Forbidden ACTION patterns -----------------------------------------------
#
# Each entry: (rule, compiled regex, hard). `hard` patterns fire on any
# non-comment occurrence. Non-`hard` (SOFT) patterns are exempted when the same
# code line carries a DEFENSIVE_RE cue.

# A terminal DLM lock. Only LCK_BOOT is known to render the SWD-DP permanently
# unresponsive on the RA8D2; the recoverable OEM_PL0/PL1 debug-locks are
# deliberately NOT here. Keep this a named token so a future terminal state is a
# one-line addition.
_TERMINAL = r"lck[_-]?boot"

# A single `=` assignment operator, excluding the comparison operators (==, !=,
# <=, >=). Used so `x = LCK_BOOT` (an action) fires while `x == LCK_BOOT` (a
# check) does not.
_ASSIGN = r"(?<![=!<>])=(?!=)"

# The truthy right-hand sides that set a flag.
_TRUE = r"(?:1\b|true\b|on\b|yes\b|set\b|enabled?\b)"

# A set/transition verb. The lookarounds treat `_`, whitespace and punctuation
# as boundaries but NOT letters, so the verb matches as an underscore-delimited
# identifier component too -- `dlm_program(...)` and `never_regress_to_...` both
# expose their verb -- while `reset`, `offset`, `clock`, `remove` do not.
_SET_VERB = r"set|enable|program|transition|move|write|lock|enter|switch|regress|activate|go\s*to"
_VERB = rf"(?<![A-Za-z])(?:{_SET_VERB})(?![A-Za-z])"

RULES: list[tuple[str, re.Pattern[str], bool]] = [
    # -- HARD: executed / programmatic ce ("Disable Initialize") set ----------
    ("ce-set", re.compile(r"\bset[_ -]ce\b", re.IGNORECASE), True),
    ("ce-set", re.compile(rf"\bce\b\s*(?:{_ASSIGN}|:)\s*{_TRUE}", re.IGNORECASE), True),
    (
        "ce-set",
        re.compile(
            r"(?:set[_ -]security[_ -]flags?|--?security[_ -]flags?[= ])[^\n]{0,24}\bce\b",
            re.IGNORECASE,
        ),
        True,
    ),
    ("ce-set", re.compile(r"--disable[_-]initialize\b", re.IGNORECASE), True),
    (
        "ce-set",
        re.compile(rf"\bdisable[_-]initialize\b\s*(?:{_ASSIGN}|:)\s*{_TRUE}", re.IGNORECASE),
        True,
    ),
    # -- HARD: executed terminal DLM transition (rfp-cli -dlm LCK_BOOT) --------
    ("dlm-terminal", re.compile(rf"--?dlm[= ]+['\"]?\s*{_TERMINAL}\b", re.IGNORECASE), True),
    # -- SOFT: terminal-state / flag NAME governed by a set-verb (guarded) -----
    (
        "ce-set",
        re.compile(rf"{_VERB}[^\n]{{0,24}}\bdisable[ _-]?initialize\b", re.IGNORECASE),
        False,
    ),
    (
        "dlm-terminal",
        re.compile(rf"{_VERB}[^\n]{{0,24}}{_TERMINAL}\b", re.IGNORECASE),
        False,
    ),
    # -- SOFT: assignment target is the terminal state (`dlm = LCK_BOOT`) ------
    (
        "dlm-terminal",
        re.compile(rf"{_ASSIGN}\s*[^\n;{{}}]{{0,24}}?{_TERMINAL}\b", re.IGNORECASE),
        False,
    ),
]

# A code line that CHECKS / NEGATES / WARNS / COMPARES rather than acts. The
# word cues are word-boundaried so that an identifier merely containing one of
# them (e.g. `never_regress_to_lck_boot`) does NOT earn the exemption -- a
# function that does the transition is a violation whatever it is named. The
# comparison operators keep a state check (`if (dlm == LCK_BOOT)`) from reading
# as an action. Guards SOFT rules only (and, for prose files, HARD rules too).
DEFENSIVE_RE = re.compile(
    r"==|!=|"
    r"\b(?:unset|must\s+not|must\s+never|may\s+be\s+set|may\s+have\s+been\s+set|"
    r"not\s+have\s+been\s+set|not\s+be\s+set|do\s+not|don't|never|should\s+not|"
    r"shall\s+not|verify|verifies|ensure|is\s+set|was\s+set|been\s+set)\b",
    re.IGNORECASE,
)

MAX_FINDINGS_SHOWN = 50
SNIPPET_MAX_LEN = 120


def lang_of(rel: str) -> str:
    """Classify a tracked path's comment style / prose-ness for scanning.

    Returns one of "hash", "c", "xml", "prose", "none", or "skip" (binary /
    out-of-scope). Extension wins; a few known extensionless basenames (the git
    hooks) map to "hash".
    """
    p = Path(rel)
    lang = _EXT_LANG.get(p.suffix.lower())
    if lang is not None:
        return lang
    if p.name in HASH_BASENAMES:
        return "hash"
    if p.suffix == "" and p.name in HASH_STEM_HINTS:
        return "hash"
    return "skip"


def _blank_hash_line(line: str) -> str:
    """Blank a shell/python/yaml ``#`` comment, respecting quotes.

    A ``#`` starts a comment only outside quotes and at a word boundary (line
    start after whitespace, or preceded by whitespace) -- so ``${x#y}`` and
    ``http://a#b`` are left intact while ``foo # note`` is trimmed.
    """
    in_s = in_d = False
    i, n = 0, len(line)
    while i < n:
        c = line[i]
        if in_s:
            if c == "'":
                in_s = False
        elif in_d:
            if c == "\\":
                i += 2
                continue
            if c == '"':
                in_d = False
        elif c == "'":
            in_s = True
        elif c == '"':
            in_d = True
        elif c == "#" and (i == 0 or line[i - 1].isspace()):
            return line[:i] + " " * (n - i)
        i += 1
    return line


def _blank_c_line(line: str) -> str:
    """Blank a C ``//`` line comment, respecting quotes."""
    in_s = in_d = False
    i, n = 0, len(line)
    while i < n:
        c = line[i]
        if in_s:
            if c == "\\":
                i += 2
                continue
            if c == "'":
                in_s = False
        elif in_d:
            if c == "\\":
                i += 2
                continue
            if c == '"':
                in_d = False
        elif c == "'":
            in_s = True
        elif c == '"':
            in_d = True
        elif c == "/" and i + 1 < n and line[i + 1] == "/":
            return line[:i] + " " * (n - i)
        i += 1
    return line


def _blank_blocks(text: str, open_tok: str, close_tok: str) -> str:
    """Blank ``open_tok ... close_tok`` spans, preserving newlines and offsets."""
    out: list[str] = []
    i, n = 0, len(text)
    while i < n:
        start = text.find(open_tok, i)
        if start < 0:
            out.append(text[i:])
            break
        out.append(text[i:start])
        end = text.find(close_tok, start + len(open_tok))
        end = n if end < 0 else end + len(close_tok)
        out.append(re.sub(r"[^\n]", " ", text[start:end]))
        i = end
    return "".join(out)


def blank_comments(text: str, lang: str) -> str:
    """Return `text` with comments blanked (strings preserved), for `lang`.

    Strings are deliberately kept so a quoted CLI argument like
    ``-dlm "LCK_BOOT"`` is still seen. Prose files are returned unchanged --
    every match in them is treated as SOFT by the caller.
    """
    if lang == "c":
        text = _blank_blocks(text, "/*", "*/")
        return "\n".join(_blank_c_line(ln) for ln in text.split("\n"))
    if lang == "hash":
        return "\n".join(_blank_hash_line(ln) for ln in text.split("\n"))
    if lang == "xml":
        return _blank_blocks(text, "<!--", "-->")
    return text


def scan_text(raw: str, rel: str, lang: str) -> list[dict]:
    """Find forbidden anti-recovery ACTIONS in one file's text.

    `lang == "prose"` forces every rule to be treated as SOFT (documentation is
    described, not executed), so a doc that warns against the brick stays clean.
    """
    code = blank_comments(raw, lang)
    prose = lang == "prose"
    found: list[dict] = []
    for lineno, line in enumerate(code.split("\n"), start=1):
        defended = DEFENSIVE_RE.search(line) is not None
        for rule, pattern, hard in RULES:
            m = pattern.search(line)
            if not m:
                continue
            if (not hard or prose) and defended:
                continue
            found.append(
                {
                    "path": rel,
                    "line": lineno,
                    "rule": rule,
                    "match": m.group(0).strip(),
                    "text": line.strip(),
                }
            )
            break
    return found


def tracked_files(explicit: list[str]) -> list[Path]:
    """Enumerate the tracked first-party files in scope.

    Tracked via ``git ls-files``, not globbed: a glob also sweeps build output
    and changes the verdict depending on whether the caller has built. Excluded
    prefixes, the recovery scripts, and this checker are dropped here.
    """
    if explicit:
        return [Path(p) for p in explicit]
    try:
        listed = subprocess.run(
            ["git", "ls-files", "-z"],  # noqa: S607  # trusted: fixed git argv
            capture_output=True,
            text=True,
            check=True,
            cwd=REPO_ROOT,
        ).stdout
    except (OSError, subprocess.CalledProcessError) as exc:
        sys.exit(
            f"check_no_antirecovery.py: FATAL -- cannot list tracked files: {exc}\n"
            "  This gate enumerates via git and must not fall back to a glob."
        )
    out: list[Path] = []
    for name in listed.split("\0"):
        if not name:
            continue
        if name in EXCLUDED_FILES:
            continue
        if any(name.startswith(pfx) for pfx in EXCLUDED_PREFIXES):
            continue
        out.append(REPO_ROOT / name)
    return out


def analyse(files: list[Path]) -> list[dict]:
    """Return every forbidden-action finding across the given files."""
    findings: list[dict] = []
    for path in files:
        rel = str(path.relative_to(REPO_ROOT)) if path.is_absolute() else str(path)
        lang = lang_of(rel)
        if lang == "skip":
            continue
        try:
            raw = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        findings.extend(scan_text(raw, rel, lang))
    return findings


# --- selftest ----------------------------------------------------------------
# (fixture, filename, should_fire). Filenames carry a real suffix so lang_of
# classifies them the way the corresponding production file would be.
SELFTEST_CASES: list[tuple[str, str, bool]] = [
    # ---- must FIRE: brick actions -------------------------------------------
    (
        "rfp-cli -d ra -t jlink:$SN -if swd -s 1000000 -dlm LCK_BOOT\n",
        "brick_dlm.sh",
        True,
    ),
    (
        "# the ce flag disables Initialize -- do the deed below\nset ce\n",
        "brick_set_ce.sh",
        True,
    ),
    (
        'rfp-cli -d ra -t "jlink:$SN" -if swd -set-security-flag ce\n',
        "brick_secflag.sh",
        True,
    ),
    (
        "disable_initialize = true\n",
        "brick_config.toml",
        True,
    ),
    (
        "rfp-cli -d ra --disable-initialize\n",
        "brick_cli_flag.sh",
        True,
    ),
    (
        "  dlm_program(k_dlm_lck_boot);\n",
        "brick_program.c",
        True,
    ),
    (
        "ra8_security.ce = true;\n",
        "brick_field.c",
        True,
    ),
    (
        'rfp-cli -d ra -t jlink -dlm "LCK_BOOT"\n',
        "brick_dlm_quoted.sh",
        True,
    ),
    (
        "static void never_regress_to_lck_boot(void) { program_dlm(k_dlm_lck_boot); }\n",
        "brick_misnamed.c",
        True,
    ),
    # ---- must STAY SILENT: defensive / recoverable / prose ------------------
    (
        "# verify the ce security flag is unset before any OEM_PLx transition\n"
        'warn "ce flag may be set -- recovery would then fail"\n'
        "# the ce flag must NOT have been set\n"
        "# do NOT run: rfp-cli -dlm LCK_BOOT  (that permanently bricks the board)\n"
        'echo "policy: never regress the DLM to LCK_BOOT on this project"\n'
        'if ! rfp-cli -rfo | grep -q "DLM State: OEM_PL2"; then echo bad; fi\n',
        "defensive_check.sh",
        False,
    ),
    (
        "rfp-cli -d ra -t jlink -dlm OEM_PL1\n",
        "recoverable_dlm.sh",
        False,
    ),
    (
        "This project must NEVER run `rfp-cli -dlm LCK_BOOT`; recovery relies on\n"
        "the ce flag staying unset so Initialize can regress OEM_PL0 -> OEM_PL2.\n",
        "policy_doc.md",
        False,
    ),
    (
        'ok "Boundary cleared; DLM regressed OEM_PL0 -> OEM_PL2"\n',
        "recovered_ok.sh",
        False,
    ),
]


def selftest(tmp: Path) -> int:
    """Assert the detector fires on brick actions and stays silent otherwise.

    Runs BOTH directions -- synthetic brick files must be detected, and the
    REAL recovery scripts plus a defensive-check fixture must produce nothing.
    The recovery scripts are scanned by CONTENT here (bypassing the path
    exclusion) so the assertion proves the PATTERNS keep them silent, not
    merely that they are excluded.
    """
    failures: list[str] = []

    for idx, (body, fname, should_fire) in enumerate(SELFTEST_CASES):
        p = tmp / f"{idx:02d}_{fname}"
        p.write_text(body, encoding="utf-8")
        fired = bool(scan_text(body, fname, lang_of(fname)))
        if fired != should_fire:
            verb = "did not fire" if should_fire else "fired"
            failures.append(f"  synthetic [{fname}]: gate {verb} (unexpected)")

    # The real recovery scripts, by content, must be silent on the PATTERNS.
    for rel in sorted(RECOVERY_SCRIPTS):
        f = REPO_ROOT / rel
        if not f.is_file():
            failures.append(f"  recovery script missing: {rel}")
            continue
        hits = scan_text(f.read_text(encoding="utf-8"), rel, lang_of(rel))
        failures.extend(f"  recovery script {rel}:{h['line']} matched {h['match']!r}" for h in hits)

    if failures:
        print("check_no_antirecovery.py --selftest: FAILED\n", file=sys.stderr)
        print("\n".join(failures), file=sys.stderr)
        return 1

    fires = sum(1 for c in SELFTEST_CASES if c[2])
    total = len(SELFTEST_CASES)
    print(
        f"check_no_antirecovery.py --selftest: PASS "
        f"({total} synthetic cases: {fires} must fire, {total - fires} must stay silent; "
        f"{len(RECOVERY_SCRIPTS)} real recovery scripts silent by pattern)"
    )
    return 0


def _report(findings: list[dict]) -> None:
    """List every forbidden action found, then explain the policy."""
    print(
        f"check_no_antirecovery.py: {len(findings)} anti-recovery action(s) found:\n",
        file=sys.stderr,
    )
    for f in sorted(findings, key=lambda v: (v["path"], v["line"]))[:MAX_FINDINGS_SHOWN]:
        snippet = f["text"]
        if len(snippet) > SNIPPET_MAX_LEN:
            snippet = snippet[: SNIPPET_MAX_LEN - 3] + "..."
        print(f"  [{f['rule']}] {f['path']}:{f['line']}  {snippet}", file=sys.stderr)
    if len(findings) > MAX_FINDINGS_SHOWN:
        print(f"  ... {len(findings) - MAX_FINDINGS_SHOWN} more (truncated)", file=sys.stderr)
    print(
        "\nOwner policy (2026-07-23): this project must NEVER permanently disable\n"
        "device recovery. Forbidden actions:\n"
        "  [ce-set]        setting the ce Security Flag / Disable Initialize Command\n"
        "                  (rfp-cli -set-security-flag ce, set ce, ce = true,\n"
        "                  --disable-initialize, an OSIS / option-byte write).\n"
        "  [dlm-terminal]  transitioning the DLM to LCK_BOOT (or any lock that\n"
        "                  renders the SWD-DP permanently unresponsive).\n"
        "\nRecoverable OEM_PL0/PL1 debug-lock -- which Initialize or an\n"
        "authenticated regression can undo -- is allowed, as is reading/checking\n"
        "state (rfp-cli -rfo) and warning about the danger. If you are writing a\n"
        "recovery/check path, phrase it as a check (it is then not an action), or\n"
        "put it in scripts/hil/{dlm_reset,dlm_reset_local,recover,reflash}.sh.",
        file=sys.stderr,
    )


def main(argv: list[str]) -> int:
    """Scan first-party files for anti-recovery brick actions, or run the selftest.

    CI runs ``--selftest`` before the scan: a detector whose patterns stopped
    matching would report a clean tree, and only an assertion in both directions
    tells that apart from a genuinely clean tree.

    Returns 0 when no forbidden action is found, 1 on a finding, on an empty
    scan set, or on a failing selftest.
    """
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("files", nargs="*", help="specific files to scan")
    ap.add_argument(
        "--selftest",
        action="store_true",
        help="prove the detector fires on brick actions and not on recovery/defensive code",
    )
    args = ap.parse_args(argv[1:])

    if args.selftest:
        with tempfile.TemporaryDirectory() as td:
            return selftest(Path(td))

    files = tracked_files(args.files)
    if not files:
        print(
            "check_no_antirecovery.py: FATAL -- no tracked files in scope.\n"
            "  Run this from the repository root.",
            file=sys.stderr,
        )
        return 1

    findings = analyse(files)
    if not findings:
        print(
            f"check_no_antirecovery.py: OK ({len(files)} files scanned, no anti-recovery actions)"
        )
        return 0

    _report(findings)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
