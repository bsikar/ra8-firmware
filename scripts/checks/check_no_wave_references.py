#!/usr/bin/env python3
r"""check_no_wave_references.py -- ban session-bookkeeping "Wave N" references.

Rationale: comments and commit messages that cite "Wave 70 fixed FRDY" or
"see Wave 43b" leak internal session bookkeeping into the source tree.
Future readers do not care WHEN a fix was found, only WHY. Reference the
function or symbol or HUM section instead.

What this gate flags (case-insensitive):
  - "Wave 70", "wave 3", "WAVE-12", "wave_43b" -- a "wave" token immediately
    followed (after at most one [\\s_-]) by a digit. Optional trailing
    letter(s) for sub-numbering ("wave 43b").
  - "(Wave 12)" / "[wave-7]" -- same pattern wrapped in punctuation.

What this gate does NOT flag (legitimate domain usage):
  - "waveform", "wave shape", "saw wave", "sine wave" (no digit follows).
  - Identifiers like "k_ra8_pdg_wave_saw", "wave_select", "wave_table".
  - Renesas / MIPI / vendor symbols that happen to contain "wave".

Per-line opt-out: append "WAVE-OK: <reason>" on the offending line. Reserve
for unavoidable upstream-symbol citations (e.g. a Renesas register name
that literally encodes the wording).

Scope is DERIVED from ``git ls-files`` via ``lint_targets.first_party_paths``
rather than a hardcoded root list, so a new top-level directory (``tools/``,
``coprocessor/``, ``infra/`` and ``mk/`` were the ones the old list silently
omitted, #549) is covered the day it lands. ``--selftest`` proves the detector
fires and stays quiet, and that the derived scope clears its floor.

Exit code:
  0 -- gate clean
  1 -- violations exist
  2 -- the scope collapsed below FILE_FLOOR (a scan of almost nothing)

@copyright Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from lint_targets import first_party_paths
from selftest_assert import expect, report

SCAN_EXTS: frozenset[str] = frozenset(
    {
        ".c",
        ".h",
        ".cpp",
        ".hpp",
        ".cc",
        ".cmake",
        ".md",
        ".yml",
        ".yaml",
        ".sh",
        ".py",
        ".txt",
        ".mk",
    }
)

SCAN_BASENAMES: frozenset[str] = frozenset(
    {"Makefile", "Dockerfile", "CMakeLists.txt", "GNUmakefile"}
)

# Vendored / generated docs content that is not ours to police: committed
# datasheets and register maps under docs/reference/, and any generated Doxygen
# output (docs/**/doxygen, docs/**/html) that is tracked. first_party_paths
# already drops third_party/ and build output; these three are the docs-side
# equivalents the old SKIP_DIR_NAMES carried. Matched by path component, the
# same way the previous walk skipped them.
DOCS_VENDOR_DIRS: frozenset[str] = frozenset({"reference", "doxygen", "html"})

# A tree this size cannot legitimately collapse to a handful of files. A scan
# that enumerates almost nothing reports a clean tree because it read almost
# nothing -- the exact failure the gate-honesty epic (#190) exists to prevent.
# Measured 2026-08-02: 3416 first-party files in the derived scope. Same
# trip-wire as check_ruff.py.
FILE_FLOOR = 2500

# Maximum number of violations to print before truncating output.
MAX_FINDINGS_SHOWN = 50
# Maximum line length (chars) for a snippet printed in the report.
SNIPPET_MAX_LEN = 120
# Truncated snippet suffix consumes 3 chars ("..."), so trim to this length.
SNIPPET_TRIM_LEN = SNIPPET_MAX_LEN - 3

# "Wave 70", "wave-3", "WAVE_43b", "(Wave 12)" -- token + optional sep +
# digit(s) + optional letter(s).
WAVE_RE: re.Pattern[str] = re.compile(r"\b[Ww]ave[\s_\-]?\d+[A-Za-z]?\b")

OPTOUT_RE: re.Pattern[str] = re.compile(r"WAVE-OK\s*:")

SELF_EXEMPT_FILES: frozenset[str] = frozenset(
    {
        "scripts/checks/check_no_wave_references.py",
        "scripts/fix/fix_wave_references.py",
        "docs/STYLE_GUIDE.md",
        "CLAUDE.md",
    }
)


def iter_source_files(root: Path) -> list[Path]:
    """Every in-scope first-party file, derived from git rather than a root list.

    Enumeration goes through ``lint_targets.first_party_paths`` -- the shared
    derived-scope primitive -- so ``tools/``, ``coprocessor/``, ``infra/`` and
    ``mk/`` (the roots a hardcoded list silently dropped, #549) are covered. The
    only subtractions on top of what that primitive already exempts are the
    docs-side vendored/generated directories in ``DOCS_VENDOR_DIRS``.
    """
    rels = set(first_party_paths(tuple(SCAN_EXTS)))
    for name in SCAN_BASENAMES:
        rels |= {rel for rel in first_party_paths((name,)) if Path(rel).name == name}
    out: list[Path] = []
    for rel in sorted(rels):
        if set(Path(rel).parts) & DOCS_VENDOR_DIRS:
            continue
        out.append(root / rel)
    return out


def scan_file(path: Path, root: Path) -> list[tuple[Path, int, str]]:
    """Report every "Wave N" session reference in one file.

    Self-exempt files are skipped whole: this checker and the policy doc must
    spell the banned pattern to describe it, and tagging every such line
    individually would bury them.
    """
    rel = path.relative_to(root)
    if str(rel) in SELF_EXEMPT_FILES:
        return []
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError):
        return []
    out: list[tuple[Path, int, str]] = []
    for lineno, line in enumerate(text.splitlines(), start=1):
        if OPTOUT_RE.search(line):
            continue
        if WAVE_RE.search(line):
            out.append((rel, lineno, line.rstrip()))
    return out


def selftest() -> int:
    """Prove the detector fires on a "Wave N" tag, stays quiet otherwise, and scans real files.

    Both directions plus a scope probe: the numbered-session pattern must FIRE,
    the legitimate domain uses (``waveform``, ``sine wave``, ``wave_table``)
    must stay QUIET, the derived scope must clear ``FILE_FLOOR``, and it must
    reach the roots a hardcoded list had dropped (``infra/``, ``mk/``) -- a
    clean run over a scope that never sees those roots proves nothing.

    Returns:
        0 when every assertion held in both directions, 1 otherwise.
    """
    failures: list[str] = []
    for text, must_fire, label in (
        ("fixed in Wave 70", True, 'MUST FIRE: "Wave 70"'),
        ("see wave-43b for context", True, 'MUST FIRE: "wave-43b"'),
        ("the sine wave is smooth", False, 'MUST NOT FIRE: "sine wave"'),
        ("k_ra8_pdg_wave_saw selects the waveform", False, "MUST NOT FIRE: wave_saw / waveform"),
        ("wave_table[0] holds the sample", False, "MUST NOT FIRE: wave_table identifier"),
    ):
        fired = bool(WAVE_RE.search(text))
        expect(fired == must_fire, label, failures)

    root = Path(__file__).resolve().parents[2]
    files = iter_source_files(root)
    rels = {str(p.relative_to(root)) for p in files if p.is_relative_to(root)}
    expect(
        len(files) >= FILE_FLOOR,
        f"derived scope sees {len(files)} file(s) (floor {FILE_FLOOR})",
        failures,
    )
    for root_name in ("infra", "mk"):
        expect(
            any(rel.startswith(root_name + "/") for rel in rels),
            f"the derived scope reaches {root_name}/ (previously omitted)",
            failures,
        )
    return report(failures)


def main() -> int:
    """Ban session-bookkeeping "Wave N" references from the tree.

    These leak an internal working chronology into source that outlives it: a
    future reader cannot resolve "see Wave 43b" to anything, whereas the
    function, symbol or HUM section the fix touched stays findable.

    Returns 1 listing each reference, 0 when the tree is clean, 2 when the
    derived scope collapsed below FILE_FLOOR.
    """
    if "--selftest" in sys.argv[1:]:
        return selftest()
    root = Path(__file__).resolve().parents[2]
    files = iter_source_files(root)
    if len(files) < FILE_FLOOR:
        sys.stderr.write(
            f"check_no_wave_references.py: FATAL -- only {len(files)} file(s) in scope, "
            f"floor is {FILE_FLOOR}. A collapsed scope reports a clean tree because it "
            "scanned nothing.\n"
        )
        return 2
    findings: list[tuple[Path, int, str]] = []
    for f in files:
        findings.extend(scan_file(f, root))

    if not findings:
        print("no-wave-refs: 0 violations -- gate clean.")
        return 0

    print(f"no-wave-refs: {len(findings)} violations found.")
    for rel, lineno, line in findings[:MAX_FINDINGS_SHOWN]:
        snippet = line if len(line) <= SNIPPET_MAX_LEN else line[:SNIPPET_TRIM_LEN] + "..."
        print(f"  {rel}:{lineno} {snippet}")
    if len(findings) > MAX_FINDINGS_SHOWN:
        print(f"  ... {len(findings) - MAX_FINDINGS_SHOWN} more (truncated)")
    print()
    print('Per-line opt-out: append "WAVE-OK: <reason>" on the offending line.')
    print("Auto-fix helper: scripts/fix/fix_wave_references.py --apply")
    return 1


if __name__ == "__main__":
    sys.exit(main())
