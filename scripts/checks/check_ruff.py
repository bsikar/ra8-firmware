#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: ruff lint + format for every first-party Python file in the tree.

Scope is derived, not hardcoded
-------------------------------
``git ls-files`` enumerates every tracked Python file -- by ``*.py`` suffix
and by ``#!...python`` shebang, so an extensionless executable script cannot
sit outside the gate -- and ``--force-exclude`` makes ruff honour the
``extend-exclude`` list in pyproject.toml (vendored SOUP, generated data,
build trees) even though the paths are handed to it explicitly.  Explicit
paths bypass exclusions without that flag, which would drag
``libs/third_party`` into the gate.

The previous revision hardcoded ``TARGETS = ("scripts", "tools", "tests")``.
That left seven first-party files -- ``esp32/tools/`` and the HIL fixture
generators under ``examples/`` -- unlinted and unformatted for the life of the
gate, hiding 33 lint findings and 3 unformatted files.  It is the same
hardcoded-scan-list defect as #358 / #332 / #296, so the list is derived here
rather than grown by three more entries: a new root is covered the day it is
added, with no allowlist to forget.

Non-vacuity
-----------
The dominant defect class in this tree is a gate that looks active and
enforces nothing.  This one is checkable in both directions: ``--selftest``
feeds ruff a deliberately non-conforming file and asserts that every rule
family the project claims to enforce actually fires, then feeds it a
legal-but-tricky file and asserts silence.  An emptied ``select`` list, a
config ruff stopped reading, or a rule family quietly dropped all turn the
must-fire half red instead of turning the tree green.

The file-count floor guards the other half of the same failure: a scope that
silently collapses (a broken ``git ls-files``, a runaway ``extend-exclude``)
reports a clean tree because it checked almost nothing.

Run::

    check_ruff.py             # gate (fail on any finding)
    check_ruff.py --require   # fail (not skip) if ruff is absent
    check_ruff.py --selftest  # prove the configured rule set is non-vacuous

Exit 0 if clean, exit 1 on findings, exit 2 on ruff error.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

# Floor on the number of files ruff reports it will check. The tree has 96
# first-party Python files today. This is not a target to keep in sync file by
# file -- it is a trip-wire for a scope that collapses wholesale. Lower it
# deliberately, with a reason, if first-party Python genuinely shrinks.
FILE_FLOOR = 80


def _find_ruff() -> str | None:
    env = os.environ.get("RUFF")
    if env and Path(env).exists():
        return env
    return shutil.which("ruff")


def _git_ls_files(*pathspec: str) -> list[str]:
    """Return tracked paths matching `pathspec`, relative to the repo root."""
    proc = subprocess.run(  # noqa: S603 -- fixed argv, trusted tool path
        ["git", "ls-files", "-z", *pathspec],  # noqa: S607 -- trusted: fixed git argv
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr)
        sys.stderr.write("check_ruff.py: FATAL -- `git ls-files` failed\n")
        sys.exit(2)
    return [p for p in proc.stdout.split("\0") if p]


def _has_python_shebang(rel: str) -> bool:
    """True when `rel` starts with a `#!...python...` line."""
    try:
        with (REPO_ROOT / rel).open("rb") as handle:
            first = handle.readline(200)
    except OSError:
        return False
    return first.startswith(b"#!") and b"python" in first


def _tracked_python_files() -> list[str]:
    """Every tracked Python file, by extension OR by shebang.

    Exclusions are ruff's job (``--force-exclude`` + ``extend-exclude``); this
    only decides what is *offered*, so a file cannot escape the gate by living
    in a directory nobody remembered to list.

    The shebang sweep exists because ``*.py`` alone is a scope that can be
    escaped by accident: an executable ``scripts/foo``  PATHREF-OK: placeholder
    with a python shebang and no extension is a Python file this project's
    rules apply to, and a
    glob-only list would never see it. There are none today -- which is
    exactly when to close the hole, rather than after one appears and sits
    unlinted.
    """
    by_extension = _git_ls_files("*.py")
    known = set(by_extension)
    by_shebang = [rel for rel in _git_ls_files() if rel not in known and _has_python_shebang(rel)]
    return sorted(known.union(by_shebang))


def _checked_files(ruff: str, files: list[str]) -> list[str]:
    """Ask ruff which of `files` survive the configured exclusions."""
    proc = subprocess.run(  # noqa: S603 -- fixed argv, trusted tool path
        [ruff, "check", "--show-files", "--force-exclude", *files],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr)
        sys.stderr.write(f"ruff check --show-files failed (exit {proc.returncode})\n")
        sys.exit(2)
    return [line.strip() for line in proc.stdout.splitlines() if line.strip()]


def _rel(filename: str) -> str:
    path = Path(filename)
    if path.is_relative_to(REPO_ROOT):
        return str(path.relative_to(REPO_ROOT))
    return filename


def _run_check(ruff: str, files: list[str]) -> dict[str, dict[str, int]]:
    proc = subprocess.run(  # noqa: S603 -- fixed argv, trusted tool path
        [ruff, "check", "--force-exclude", "--output-format=json", *files],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode not in (0, 1):
        sys.stderr.write(proc.stderr)
        sys.stderr.write(f"ruff check failed (exit {proc.returncode})\n")
        sys.exit(2)
    findings: dict[str, dict[str, int]] = {}
    for item in json.loads(proc.stdout or "[]"):
        rel = _rel(item["filename"])
        code = item.get("code") or "SYNTAX"
        findings.setdefault(rel, {})
        findings[rel][code] = findings[rel].get(code, 0) + 1
    return findings


def _run_format(ruff: str, files: list[str]) -> list[str]:
    proc = subprocess.run(  # noqa: S603 -- fixed argv, trusted tool path
        [ruff, "format", "--check", "--force-exclude", *files],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    lines = (proc.stdout + proc.stderr).splitlines()
    return sorted(
        _rel(line.split(":", 1)[1].strip()) for line in lines if line.startswith("Would reformat:")
    )


def _report(lint: dict[str, dict[str, int]], fmt: list[str]) -> None:
    if lint:
        sys.stderr.write("check_ruff.py: ruff lint finding(s):\n")
        for relfile in sorted(lint):
            for code, count in sorted(lint[relfile].items()):
                sys.stderr.write(f"  {relfile}: {code} x{count}\n")
    if fmt:
        sys.stderr.write("check_ruff.py: file(s) need `ruff format`:\n")
        for relfile in fmt:
            sys.stderr.write(f"  {relfile}\n")
    sys.stderr.write("\nFix the finding, or run `ruff format`.\n")


# ---------------------------------------------------------------------------
# Selftest
#
# Both fixtures are fed to ruff on stdin under the REAL pyproject.toml, so what
# is proven is the shipped configuration, not a copy of it. Nothing is written
# into the tree -- a deliberately non-conforming fixture stored as a real file
# would be picked up by the gate's own scan and fail it.
# ---------------------------------------------------------------------------

# A generated 60-statement body: PLR0915 (too-many-statements) is called out by
# name in #360, and a fixture that only *looks* long would not reach the limit.
_MANY_STATEMENTS = "\n".join(f"    v{i} = {i}" for i in range(60))

BAD_FIXTURE = f'''"""Module docstring so the fixture fails on the rules under test only."""

import subprocess
import os
import sys
import pdb
import datetime


def shadowing(list, id):
    """Shadow two builtins and read a naive timestamp."""
    _ = list, id
    return datetime.datetime.now()


def CamelCaseName(items=[]):
    """Do a thing.

    Args:
        items: things.

    Returns:
        The count.
    """
    unused_local = os.path.join("a", "b")
    with open("f") as handle:
        data = handle.read()
    if len(data) > 1337:
        result = 1
    else:
        result = 2
    try:
        subprocess.run("ls", shell=True, check=False)
    except Exception:
        pass
    return result


def long_body():
    """Trip the statement-count limit."""
{_MANY_STATEMENTS}
    return v0


# result = long_body()
'''

GOOD_FIXTURE = '''"""Legal-but-tricky fixture: this must stay completely silent.

Everything here is a construct a careless rule set flags but the project
genuinely uses: a sorted import block, pathlib over os.path, a narrow except
that re-raises with a named error, and a documented subprocess call.
"""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

TIMEOUT_SECONDS = 30


def read_manifest(root: Path) -> dict[str, str]:
    """Return the manifest under `root` as a mapping.

    Args:
        root: Directory expected to contain `manifest.txt`.

    Returns:
        Mapping of key to value; empty when the manifest is absent.
    """
    manifest = root / "manifest.txt"
    if not manifest.is_file():
        return {}
    entries = {}
    for line in manifest.read_text(encoding="utf-8").splitlines():
        key, _, value = line.partition("=")
        if key:
            entries[key.strip()] = value.strip()
    return entries


def git_head(root: Path) -> str:
    """Return the short HEAD sha of the repository at `root`.

    Args:
        root: Repository working tree.

    Returns:
        The abbreviated commit hash.

    Raises:
        RuntimeError: When git is absent or exits non-zero.
    """
    git = shutil.which("git")
    if git is None:
        message = "git is not on PATH"
        raise RuntimeError(message)
    proc = subprocess.run(  # noqa: S603 -- fixed argv, trusted tool path
        [git, "rev-parse", "--short", "HEAD"],
        cwd=root,
        capture_output=True,
        text=True,
        check=False,
        timeout=TIMEOUT_SECONDS,
    )
    if proc.returncode != 0:
        message = f"git rev-parse failed in {root}"
        raise RuntimeError(message)
    return proc.stdout.strip()
'''

# Rule families the project claims to enforce, each pinned to a code the bad
# fixture provably triggers. Keyed by family so a dropped `select` entry names
# itself in the failure text instead of surfacing as a bare missing code.
EXPECTED_CODES: dict[str, str] = {
    "F (pyflakes)": "F401",
    "N (pep8-naming)": "N802",
    "I (isort)": "I001",
    "B (bugbear)": "B006",
    "PTH (use-pathlib)": "PTH123",
    "PL (pylint: magic value)": "PLR2004",
    "PL (pylint: too-many-statements)": "PLR0915",
    "SIM (simplify)": "SIM108",
    "S (bandit)": "S602",
    "BLE (blind-except)": "BLE001",
    "A (builtin shadowing)": "A002",
    "DTZ (naive datetime)": "DTZ005",
    "T10 (debugger left in)": "T100",
    "F (unused local)": "F841",
}


def _lint_stdin(ruff: str, source: str, filename: str) -> set[str]:
    """Return the set of rule codes ruff reports for `source`."""
    proc = subprocess.run(  # noqa: S603 -- fixed argv, trusted tool path
        [ruff, "check", "--no-cache", "--output-format=json", "--stdin-filename", filename, "-"],
        cwd=REPO_ROOT,
        input=source,
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode not in (0, 1):
        sys.stderr.write(proc.stderr)
        sys.stderr.write(f"ruff check (stdin) failed (exit {proc.returncode})\n")
        sys.exit(2)
    return {item.get("code") or "SYNTAX" for item in json.loads(proc.stdout or "[]")}


def _format_stdin_would_reformat(ruff: str, source: str, filename: str) -> bool:
    """True when `ruff format --check` would rewrite `source`."""
    proc = subprocess.run(  # noqa: S603 -- fixed argv, trusted tool path
        [ruff, "format", "--check", "--stdin-filename", filename, "-"],
        cwd=REPO_ROOT,
        input=source,
        capture_output=True,
        text=True,
        check=False,
    )
    return proc.returncode == 1


# Virtual filenames handed to `ruff --stdin-filename`. Ruff resolves per-file
# configuration against the name, so it has to look like a first-party .py
# path; nothing is ever created on disk at either location.
BAD_FIXTURE_NAME = "scripts/checks/ruff_selftest_bad.py"  # PATHREF-OK: virtual
GOOD_FIXTURE_NAME = "scripts/checks/ruff_selftest_good.py"  # PATHREF-OK: virtual
FMT_FIXTURE_NAME = "scripts/ruff_fmt_bad.py"  # PATHREF-OK: virtual


def selftest(ruff: str) -> int:
    """Prove the configured rule set fires where it must and is quiet where it must."""
    failures: list[str] = []

    fired = _lint_stdin(ruff, BAD_FIXTURE, BAD_FIXTURE_NAME)
    for family, code in sorted(EXPECTED_CODES.items()):
        if code not in fired:
            failures.append(f"  must-fire: {family} did not report {code} on the bad fixture")

    quiet = _lint_stdin(ruff, GOOD_FIXTURE, GOOD_FIXTURE_NAME)
    failures.extend(f"  must-stay-quiet: good fixture reported {code}" for code in sorted(quiet))

    # The formatter half of the gate needs the same proof: a mangled fixture
    # must be seen as needing a rewrite, and the clean one must not be.
    if not _format_stdin_would_reformat(ruff, "x = {  'a' :1,'b':2 }\n", FMT_FIXTURE_NAME):
        failures.append("  must-fire: `ruff format --check` accepted a mangled fixture")
    if _format_stdin_would_reformat(ruff, GOOD_FIXTURE, GOOD_FIXTURE_NAME):
        failures.append("  must-stay-quiet: `ruff format --check` rejected the clean fixture")

    if failures:
        sys.stderr.write("check_ruff.py --selftest: FAILED\n")
        sys.stderr.write("\n".join(failures) + "\n")
        sys.stderr.write(
            "\nThe configured rule set is not enforcing what it advertises.\n"
            "Check `select` in pyproject.toml before trusting a clean run.\n"
        )
        return 1

    print(
        f"check_ruff.py --selftest: OK "
        f"({len(EXPECTED_CODES)} rule families fire, good fixture silent, formatter both ways)."
    )
    return 0


def main(argv: list[str]) -> int:
    args = argv[1:]
    ruff = _find_ruff()
    if not ruff:
        msg = "check_ruff.py: ruff not found"
        if "--require" in args or "--selftest" in args or "--list-files" in args:
            sys.stderr.write(msg + " -- required by --require/--selftest/--list-files\n")
            sys.exit(1)
        print(msg + " -- skipping (install ruff to enforce locally).")
        sys.exit(0)

    if "--selftest" in args:
        return selftest(ruff)

    tracked = _tracked_python_files()
    files = _checked_files(ruff, tracked) if tracked else []

    # Scope introspection for check_lint_coverage.py: report exactly the files
    # this gate would lint and format, after ruff's own exclusions. The
    # coverage gate asks every checker this rather than restating its scope,
    # so the two cannot disagree about what is covered.
    # Repo-relative: ruff reports absolute paths, and every other checker's
    # list mode speaks repo-relative paths.
    if "--list-files" in args:
        print("\n".join(sorted(_rel(f) for f in files)))
        return 0

    if len(files) < FILE_FLOOR:
        sys.stderr.write(
            f"check_ruff.py: FATAL -- only {len(files)} Python file(s) in scope, "
            f"floor is {FILE_FLOOR}.\n"
            "  A collapsed scope reports a clean tree because it checked nothing.\n"
        )
        return 2

    lint = _run_check(ruff, tracked)
    fmt = _run_format(ruff, tracked)
    if not lint and not fmt:
        print(f"check_ruff.py: clean ({len(files)} files, no lint or format findings).")
        return 0
    _report(lint, fmt)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
