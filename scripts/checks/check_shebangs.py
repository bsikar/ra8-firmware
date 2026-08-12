#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: every first-party shell script carries an ``#!/usr/bin/env <interp>``.

Why
---
A hardcoded interpreter path is a portability assertion the tree cannot keep.
``#!/bin/bash`` is wrong on any host whose bash is not at that path -- NixOS,
a Homebrew bash 5 on macOS (``/bin/bash`` there is the 3.2 that ships with the
OS and lacks ``mapfile``), FreeBSD, and a container built ``FROM scratch`` with
a busybox layout.  ``#!/usr/bin/env bash`` resolves through PATH instead, which
is the same discipline ``scripts/ci/lib/tool_env.sh`` applies to every other
tool this project runs: decide the interpreter by resolution, not by a literal
path baked into 15 files.

Two questions, two scopes
-------------------------
* **Presence** -- is there a shebang at all?  Required of every first-party
  *shell script* (``*.sh`` / ``*.bash``, and the extensionless files whose own
  shebang already declares them shell -- the ``scripts/git/*`` hooks).  The
  scope is :func:`lint_targets.files_for`'s ``shell`` set, the one definition
  this repo shares.  A ``.sh`` with no shebang is a real gap even when the file
  is only ever *sourced*: the shebang is what tells an editor and ShellCheck
  the dialect, and the moment someone runs it directly the interpreter is
  whatever the parent shell happens to be.
* **Form** -- is the shebang, if present, the env form?  Asked of *any*
  in-scope file that opens with ``#!`` (a Python or Perl helper as much as a
  shell one), because a hardcoded path is wrong wherever it appears.

Four defect shapes are rejected, all of which this tree has carried:

* **missing** -- a first-party shell script with no shebang on line 1.  The
  gate bodies under ``scripts/ci/gates/`` and the sourced libs under
  ``scripts/ci/lib/`` were exactly this until they were normalised.
* **non-env** -- ``#!/bin/bash``, ``#!/bin/sh``, ``#!/usr/bin/python3``.
* **malformed** -- ``# !/bin/bash``.  A space between ``#`` and ``!`` means the
  kernel never sees a shebang at all; the file is a comment followed by code,
  and executing it directly falls back to the caller's shell (or fails).  It
  reads as a shebang to a human and is not one, which is why it survives.
* **argument-bearing env** -- ``#!/usr/bin/env bash -x``.  Linux passes the
  whole remainder of the shebang line as ONE argument, so the interpreter is
  looked up as the literal name ``bash -x`` and the exec fails.  Put the flag
  in the script body (``set -x``) instead.

Scope is derived, not hardcoded
-------------------------------
``git ls-files --cached --others --exclude-standard`` enumerates every tracked
path AND every untracked-but-not-ignored one, and each is judged on its own
first line.  So a script cannot escape by living in a directory nobody listed,
by having no extension, or by not being committed yet -- which is the moment a
wrong shebang is actually introduced, and the moment the pre-commit hook asks.
Vendored SOUP, generated font tables, the ThreadX port and build output are
excluded, matching :mod:`check_shell`'s scope.

The presence rule does NOT require the executable bit: a sourced-only library
correctly carries a shebang and is correctly not executable, because nothing
execs it.  The shebang is a dialect declaration here, not a promise the file
is a program.

Non-vacuity
-----------
``--selftest`` asserts both directions over crafted fixtures -- every rejected
shape must fire and every accepted shape must stay silent -- and the scan
enforces a floor on both how many shebang-bearing files and how many shell
scripts it found, because a scope that collapses reports a clean tree having
examined almost nothing.

Run::

    check_shebangs.py             # gate (fail on any finding)
    check_shebangs.py --selftest  # prove the rules fire and stay quiet

Exit 0 if clean, 1 on findings or a failing selftest, 2 on a broken scan.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from lint_targets import files_for, is_build_output_path

REPO_ROOT = Path(__file__).resolve().parents[2]

EXIT_OK = 0
EXIT_FAIL = 1
EXIT_CONFIG = 2

EXCLUDE_FRAGMENTS = (
    "libs/third_party/",
    "libs/ra8_fonts/",
    "port/threadx/",
)

# Floor on shebang-bearing first-party files. The tree has 270 today. This is
# not kept in step file by file -- it is a trip-wire for a scope that collapses
# wholesale (a broken `git ls-files`, a runaway exclusion). Lower it
# deliberately, with a reason, if first-party scripting genuinely shrinks.
SHEBANG_FLOOR = 150

# Floor on first-party shell scripts subject to the PRESENCE rule. Measured
# 2026-08-02: 128. Same trip-wire, for the other scope: if the `shell` set
# collapses, the presence rule silently stops requiring anything and the gate
# reports a clean tree having demanded a shebang of almost nothing.
SHELL_FLOOR = 100

# The one accepted form: `#!/usr/bin/env` plus exactly one interpreter token.
# The trailing `$` is load-bearing -- it is what rejects `env bash -x`.
_GOOD_RE = re.compile(r"^#!/usr/bin/env [A-Za-z_][A-Za-z0-9_.+-]*$")

# A near-shebang: `#` then whitespace then `!`. Never a real shebang.
_MALFORMED_RE = re.compile(r"^#\s+!\s*\S")

# Bytes read from each file: enough for any shebang, cheap over 13k files.
_PROBE_BYTES = 256


def _git_ls() -> list[str]:
    """Return every tracked-or-untracked-but-not-ignored path, repo-relative.

    ``--others --exclude-standard`` is what puts a BRAND NEW script in scope.
    Tracked-only enumeration cannot see the file a developer just wrote, which
    is precisely the moment a wrong shebang gets introduced and the moment the
    pre-commit hook is asked about it. Ignored paths stay out, so locally
    present vendor drops and build trees never enter the gate.

    Returns:
        Paths as reported by ``git ls-files``.
    """
    proc = subprocess.run(
        ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"],  # noqa: S607
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr)
        sys.stderr.write("check_shebangs.py: FATAL -- `git ls-files` failed\n")
        sys.exit(EXIT_CONFIG)
    return [p for p in proc.stdout.split("\0") if p]


def _in_scope(rel: str) -> bool:
    """Return whether `rel` is first-party code this gate judges.

    Args:
        rel: Repo-relative path.

    Returns:
        True when the path is neither vendored, generated, nor build output.
    """
    if is_build_output_path(rel):
        return False
    return not any(frag in f"/{rel}" for frag in EXCLUDE_FRAGMENTS)


def _first_line(path: Path) -> str | None:
    """Return the first line of `path`, or None when it cannot be read as text.

    Args:
        path: File to probe.

    Returns:
        The first line without its newline, or None for unreadable/binary data.
    """
    try:
        with path.open("rb") as handle:
            raw = handle.readline(_PROBE_BYTES)
    except OSError:
        return None
    if b"\0" in raw:
        return None
    return raw.decode("utf-8", errors="replace").rstrip("\r\n")


def classify(line: str, *, require_shebang: bool) -> str | None:
    """Judge one first line, returning a violation reason or None when clean.

    Args:
        line: The file's first line, newline stripped.
        require_shebang: True for a first-party shell script, where the absence
            of a shebang is itself a finding. False elsewhere, where a file
            with no ``#!`` is simply not a script and is left alone.

    Returns:
        A short reason when the line is a bad (or, for a shell script, missing)
        shebang, else None. A malformed near-shebang fires regardless of
        `require_shebang`, because it is never a legitimate first line.
    """
    if _MALFORMED_RE.match(line):
        return "malformed: a space between '#' and '!' is not a shebang at all"
    if not line.startswith("#!"):
        if require_shebang:
            return "no shebang: a shell script must start with `#!/usr/bin/env <interp>`"
        return None
    if _GOOD_RE.match(line):
        return None
    if line.startswith("#!/usr/bin/env "):
        return (
            "env shebang with arguments: Linux passes them as one token; use `set -x` in the body"
        )
    return "hardcoded interpreter path: use `#!/usr/bin/env <interp>`"


def scan() -> tuple[list[tuple[str, str, str]], int, int]:
    """Judge every in-scope tracked file.

    The presence rule applies only to the first-party ``shell`` set from
    :func:`lint_targets.files_for` -- the single shared definition of "this is
    a shell script". The form rule applies to any in-scope file that opens with
    a ``#!``.

    Returns:
        A ``(findings, shebang_count, shell_count)`` triple, where each finding
        is ``(path, line, reason)``.
    """
    shell_set = set(files_for(("shell",))["shell"])
    findings: list[tuple[str, str, str]] = []
    shebangs = 0
    shell_seen = 0
    for rel in _git_ls():
        if not _in_scope(rel):
            continue
        require = rel in shell_set
        if require:
            shell_seen += 1
        line = _first_line(REPO_ROOT / rel)
        if line is None:
            if require:
                findings.append((rel, "<unreadable>", "shell script with an unreadable first line"))
            continue
        if line.startswith("#!"):
            shebangs += 1
        reason = classify(line, require_shebang=require)
        if reason is not None:
            findings.append((rel, line, reason))
    return findings, shebangs, shell_seen


# ---------------------------------------------------------------------------
# Selftest -- both directions, over crafted first lines. Nothing is written
# into the tree: a deliberately bad fixture stored as a real file would be
# picked up by this gate's own scan and fail it.
# ---------------------------------------------------------------------------

# Each case is (label, first_line, require_shebang): the third field is the
# scope flag the scan would pass, True where the line stands for a shell script.
MUST_FIRE: tuple[tuple[str, str, bool], ...] = (
    # form -- a bad shebang fires whether or not a shebang was required
    ("plain bash path", "#!/bin/bash", False),
    ("plain sh path", "#!/bin/sh", False),
    ("usr-bin python path", "#!/usr/bin/python3", False),
    ("space after the bang", "#! /bin/bash", False),
    ("space before the bang", "# !/bin/bash", False),
    ("env with a flag", "#!/usr/bin/env bash -x", False),
    ("env with nothing after it", "#!/usr/bin/env", False),
    ("env with a pathful interpreter", "#!/usr/bin/env /bin/bash", False),
    # presence -- a shell script with no shebang at all
    ("shell script missing its shebang", "# shellcheck shell=bash", True),
    ("shell script that opens with code", "echo hi", True),
    ("shell script with an empty first line", "", True),
)

MUST_STAY_QUIET: tuple[tuple[str, str, bool], ...] = (
    ("env bash", "#!/usr/bin/env bash", False),
    ("env sh", "#!/usr/bin/env sh", False),
    ("env python3", "#!/usr/bin/env python3", False),
    ("env perl", "#!/usr/bin/env perl", False),
    ("env bash where a shebang was required", "#!/usr/bin/env bash", True),
    ("a non-script file with no shebang", "# SPDX-License-Identifier: MIT", False),
    ("a C comment", "// SPDX-License-Identifier: MIT", False),
    ("a markdown heading", "# Title", False),
    ("an empty first line off the shell scope", "", False),
)


# Paths the scope rules must reject / keep. Both halves matter: a runaway
# exclusion that swallowed scripts/ would otherwise report a clean tree.
SCOPE_MUST_EXCLUDE = ("libs/third_party/threadx/scripts/x.sh", "libs/ra8_fonts/gen.sh")
SCOPE_MUST_INCLUDE = ("scripts/git/pre-commit", "coprocessor/esp32c6/build.sh")


def _selftest_scope() -> list[str]:
    """Assert the scope helper excludes vendored trees and keeps first-party.

    Returns:
        Failure descriptions; empty when the scope rules behave.
    """
    excluded = [
        f"  scope: {rel} should be excluded but is in scope"
        for rel in SCOPE_MUST_EXCLUDE
        if _in_scope(rel)
    ]
    included = [
        f"  scope: {rel} should be in scope but is excluded"
        for rel in SCOPE_MUST_INCLUDE
        if not _in_scope(rel)
    ]
    return excluded + included


def selftest() -> int:
    """Prove every rejected shape fires and every accepted shape stays silent.

    Returns:
        EXIT_OK when the classifier and the scope rules behave in both
        directions, EXIT_FAIL otherwise.
    """
    failures = [
        f"  must-fire: {label} was accepted: {line!r}"
        for label, line, req in MUST_FIRE
        if classify(line, require_shebang=req) is None
    ]
    failures += [
        f"  must-stay-quiet: {label} was rejected ({classify(line, require_shebang=req)}): {line!r}"
        for label, line, req in MUST_STAY_QUIET
        if classify(line, require_shebang=req) is not None
    ]
    failures += _selftest_scope()

    if failures:
        sys.stderr.write("check_shebangs.py --selftest: FAILED\n\n")
        sys.stderr.write("\n".join(failures) + "\n")
        sys.stderr.write("\nThe rules do not judge shebangs as claimed.\n")
        return EXIT_FAIL

    total = len(MUST_FIRE) + len(MUST_STAY_QUIET)
    print(
        f"check_shebangs.py --selftest: OK ({total} cases: "
        f"{len(MUST_FIRE)} must fire, {len(MUST_STAY_QUIET)} must stay quiet; scope both ways)."
    )
    return EXIT_OK


def main(argv: list[str]) -> int:
    """Run the selftest or scan the tree for missing / non-env shebangs.

    Returns:
        EXIT_OK when clean, EXIT_FAIL on any finding or a failing selftest,
        EXIT_CONFIG when either scan scope collapsed below its non-vacuity floor.
    """
    if "--selftest" in argv[1:]:
        return selftest()

    findings, shebangs, shell_seen = scan()

    if shebangs < SHEBANG_FLOOR or shell_seen < SHELL_FLOOR:
        sys.stderr.write(
            f"check_shebangs.py: FATAL -- {shebangs} shebang(s) (floor {SHEBANG_FLOOR}) "
            f"and {shell_seen} shell script(s) (floor {SHELL_FLOOR}) in scope.\n"
            "  A collapsed scope reports a clean tree because it checked nothing.\n"
        )
        return EXIT_CONFIG

    if not findings:
        print(
            f"check_shebangs.py: {shell_seen} shell script(s) all carry a shebang; "
            f"{shebangs} shebang(s) all use the env form."
        )
        return EXIT_OK

    sys.stderr.write(f"check_shebangs.py: {len(findings)} bad shebang(s):\n")
    for rel, line, reason in findings:
        sys.stderr.write(f"  {rel}: {line!r}\n      {reason}\n")
    sys.stderr.write(
        "\nEvery shell script needs `#!/usr/bin/env bash` (or sh) on line 1. The\n"
        "interpreter must be a bare name resolved through PATH, with no arguments\n"
        "on the shebang line.\n"
    )
    return EXIT_FAIL


if __name__ == "__main__":
    sys.exit(main(sys.argv))
