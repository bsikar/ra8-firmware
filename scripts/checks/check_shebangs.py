#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: every first-party shebang uses the ``#!/usr/bin/env <interp>`` form.

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

Three defect shapes are rejected, all of which this tree has carried:

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
by having no extension (the ``scripts/git/*`` hooks are exactly that shape), or
by not being committed yet -- which is the moment a wrong shebang is actually
introduced, and the moment the pre-commit hook asks.  Vendored SOUP, generated
font tables, the ThreadX port and build output are excluded, matching
:mod:`check_shell`'s scope.

A shebang on a non-executable file is deliberately NOT a finding: the sourced
libraries under ``scripts/hil/lib/`` and ``scripts/builders/`` carry one so
editors and ShellCheck know the dialect, and they are correctly not executable
because nothing execs them.

Non-vacuity
-----------
``--selftest`` asserts both directions over crafted fixtures -- every rejected
shape must fire and every accepted shape must stay silent -- and the scan
enforces a floor on how many shebang-bearing files it found, because a scope
that collapses reports a clean tree having examined almost nothing.

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

from lint_targets import is_build_output_path

REPO_ROOT = Path(__file__).resolve().parents[2]

EXIT_OK = 0
EXIT_FAIL = 1
EXIT_CONFIG = 2

EXCLUDE_FRAGMENTS = (
    "libs/third_party/",
    "libs/ra8_fonts/",
    "port/threadx/",
)

# Floor on shebang-bearing first-party files. The tree has 200 today. This is
# not kept in step file by file -- it is a trip-wire for a scope that collapses
# wholesale (a broken `git ls-files`, a runaway exclusion). Lower it
# deliberately, with a reason, if first-party scripting genuinely shrinks.
SHEBANG_FLOOR = 150

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


def classify(line: str) -> str | None:
    """Judge one first line, returning a violation reason or None when clean.

    Args:
        line: The file's first line, newline stripped.

    Returns:
        A short reason when the line is a bad shebang, else None. A line that
        is not a shebang at all (and is not a malformed near-shebang) is clean.
    """
    if _MALFORMED_RE.match(line):
        return "malformed: a space between '#' and '!' is not a shebang at all"
    if not line.startswith("#!"):
        return None
    if _GOOD_RE.match(line):
        return None
    if line.startswith("#!/usr/bin/env "):
        return (
            "env shebang with arguments: Linux passes them as one token; use `set -x` in the body"
        )
    return "hardcoded interpreter path: use `#!/usr/bin/env <interp>`"


def scan() -> tuple[list[tuple[str, str, str]], int]:
    """Judge every in-scope tracked file.

    Returns:
        A ``(findings, shebang_count)`` pair, where each finding is
        ``(path, line, reason)``.
    """
    findings: list[tuple[str, str, str]] = []
    shebangs = 0
    for rel in _git_ls():
        if not _in_scope(rel):
            continue
        line = _first_line(REPO_ROOT / rel)
        if line is None:
            continue
        if line.startswith("#!"):
            shebangs += 1
        reason = classify(line)
        if reason is not None:
            findings.append((rel, line, reason))
    return findings, shebangs


# ---------------------------------------------------------------------------
# Selftest -- both directions, over crafted first lines. Nothing is written
# into the tree: a deliberately bad fixture stored as a real file would be
# picked up by this gate's own scan and fail it.
# ---------------------------------------------------------------------------

MUST_FIRE: tuple[tuple[str, str], ...] = (
    ("plain bash path", "#!/bin/bash"),
    ("plain sh path", "#!/bin/sh"),
    ("usr-bin python path", "#!/usr/bin/python3"),
    ("space after the bang", "#! /bin/bash"),
    ("space before the bang", "# !/bin/bash"),
    ("env with a flag", "#!/usr/bin/env bash -x"),
    ("env with nothing after it", "#!/usr/bin/env"),
    ("env with a pathful interpreter", "#!/usr/bin/env /bin/bash"),
)

MUST_STAY_QUIET: tuple[tuple[str, str], ...] = (
    ("env bash", "#!/usr/bin/env bash"),
    ("env sh", "#!/usr/bin/env sh"),
    ("env python3", "#!/usr/bin/env python3"),
    ("env perl", "#!/usr/bin/env perl"),
    ("not a script at all", "# SPDX-License-Identifier: MIT"),
    ("a C comment", "// SPDX-License-Identifier: MIT"),
    ("a markdown heading", "# Title"),
    ("an empty first line", ""),
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
        for label, line in MUST_FIRE
        if classify(line) is None
    ]
    failures += [
        f"  must-stay-quiet: {label} was rejected ({classify(line)}): {line!r}"
        for label, line in MUST_STAY_QUIET
        if classify(line) is not None
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
    """Run the selftest or scan the tree for non-env shebangs.

    Returns:
        EXIT_OK when clean, EXIT_FAIL on any finding or a failing selftest,
        EXIT_CONFIG when the scan collapsed below its non-vacuity floor.
    """
    if "--selftest" in argv[1:]:
        return selftest()

    findings, shebangs = scan()

    if shebangs < SHEBANG_FLOOR:
        sys.stderr.write(
            f"check_shebangs.py: FATAL -- only {shebangs} shebang(s) in scope, "
            f"floor is {SHEBANG_FLOOR}.\n"
            "  A collapsed scope reports a clean tree because it checked nothing.\n"
        )
        return EXIT_CONFIG

    if not findings:
        print(f"check_shebangs.py: {shebangs} shebang(s) scanned, all use the env form.")
        return EXIT_OK

    sys.stderr.write(f"check_shebangs.py: {len(findings)} bad shebang(s):\n")
    for rel, line, reason in findings:
        sys.stderr.write(f"  {rel}: {line!r}\n      {reason}\n")
    sys.stderr.write(
        "\nUse `#!/usr/bin/env bash` (or sh / python3). The interpreter must be a\n"
        "bare name resolved through PATH, with no arguments on the shebang line.\n"
    )
    return EXIT_FAIL


if __name__ == "__main__":
    sys.exit(main(sys.argv))
