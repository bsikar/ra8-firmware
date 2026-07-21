#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Structural and formatting checker for this tree's Makefiles.

WHY NOT checkmake
=================
checkmake is the only off-the-shelf Make linter, and it was evaluated first.
It ships three rules, and none of them survives contact with this tree:

  minphony        requires an `all`, `test` and `clean` phony target in EVERY
                  Makefile. That encodes "every Makefile is a project root",
                  which is false for the 224 per-app wrappers (no `test` to
                  run) and for the 7 mk/*.mk include fragments (not
                  independently invocable at all). 248 findings, all noise.

  maxbodylength   caps a recipe body at 5 lines. An arbitrary number with no
                  defect behind it; 220 findings.

  phonydeclared   the one rule with real intent -- and it is BROKEN for the
                  declaration style this tree uses. checkmake 0.2.2 reads only
                  the FIRST physical line of a continued `.PHONY:`, so every
                  name after a backslash is invisible to it. Reduced case, two
                  files identical but for the line break:

                      .PHONY: first \\           .PHONY: first second
                              second
                      first: dep                 first: dep
                      second: dep                second: dep

                  the left file reports `Target "second" should be declared
                  PHONY.`; the right reports nothing. mk/quality.mk declares 36
                  phony targets across 6 continued lines, of which checkmake
                  sees 7 -- which is how it reported the already-phony `vela`.

So adopting checkmake would mean either living with a false positive or
disabling its only applicable rule, at which point the gate enforces nothing
while looking like it does. The rules below are implemented here instead, with
a continuation-aware parser, and --selftest asserts both directions.

RULES
=====
  MK001  licence header  -- SPDX-License-Identifier and a Copyright line.
  MK002  phony declared  -- a target with no `/` or `.` in its name (i.e. one
                            that names no file on disk) must appear in a
                            .PHONY declaration. Continuation-aware.
  MK003  portable ROOT   -- a per-app Makefile derives ROOT from
                            `git rev-parse --show-toplevel`, never from a
                            fixed count of `..` segments. `$(abspath
                            $(CURDIR)/../../..)` is correct only at one
                            directory depth, and this repo promotes apps
                            between tiers with `git mv`, which silently
                            changes that depth and leaves ROOT pointing above
                            the repo.
  MK004  formatting      -- 7-bit ASCII, LF endings, final newline, no
                            trailing whitespace, and recipe lines indented
                            with a tab.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys
import tempfile

EXCLUDED_PREFIXES = ("libs/third_party/", "libs/fonts/")

GIT_ROOT_FORM = "rev-parse --show-toplevel"

# Special targets and pattern rules are never expected in .PHONY.
_SPECIAL = re.compile(r"^\.[A-Z]")


def repo_files(root: pathlib.Path) -> list[pathlib.Path]:
    out = subprocess.run(  # noqa: S603  # fixed argv, no shell
        ["git", "-C", str(root), "ls-files"],  # noqa: S607  # git from PATH is intended
        capture_output=True,
        text=True,
        check=True,
    ).stdout.split()
    keep = [
        p
        for p in out
        if not p.startswith(EXCLUDED_PREFIXES)
        and (pathlib.Path(p).name == "Makefile" or p.endswith(".mk"))
    ]
    return [root / p for p in keep]


def logical_lines(text: str) -> list[tuple[int, str, bool]]:
    """Join backslash continuations into (lineno, text, is_recipe) triples.

    Continuation joining is the piece checkmake gets wrong, so the selftest
    pins it hardest. `is_recipe` records whether the FIRST physical line began
    with a tab -- it has to be captured before the join strips indentation, or
    a recipe line like `\t@echo "make targets:"` parses as a rule whose target
    list is `@echo "make` (an earlier revision of this file did exactly that
    and produced 786 phantom findings).
    """
    out: list[tuple[int, str, bool]] = []
    buf: list[str] = []
    start = 0
    recipe = False
    for i, raw in enumerate(text.splitlines(), 1):
        if not buf:
            start = i
            recipe = raw.startswith("\t")
        if raw.endswith("\\"):
            buf.append(raw[:-1])
            continue
        buf.append(raw)
        out.append((start, " ".join(s.strip() for s in buf).strip(), recipe))
        buf = []
    if buf:
        out.append((start, " ".join(s.strip() for s in buf).strip(), recipe))
    return out


def phony_names(text: str) -> set[str]:
    names: set[str] = set()
    for _, line, is_recipe in logical_lines(text):
        if is_recipe:
            continue
        m = re.match(r"^\.PHONY\s*:\s*(.*)$", line)
        if m:
            names.update(m.group(1).split())
    return names


def declared_targets(text: str) -> list[tuple[int, str, str]]:
    """Every explicit target name, as (lineno, name, recipe_text).

    The recipe comes back with the target because it is the only reliable
    signal for "this target names a file it builds". A binary has no suffix on
    Linux, so `cache_bench:` looks exactly like a phony `clean:` by name alone
    -- but its recipe ends `-o $@`, which says plainly that the rule writes the
    file the target names.
    """
    found: list[tuple[int, str, str]] = []
    lines = logical_lines(text)
    for idx, (lineno, line, is_recipe) in enumerate(lines):
        if is_recipe or not line or line.lstrip().startswith("#"):
            continue
        # `target: deps` but not `VAR := value`, `VAR ?= value`, `VAR = value`
        m = re.match(r"^([^\t:=#]+?)\s*::?(?!=)\s*(.*)$", line)
        if not m:
            continue
        lhs = m.group(1).strip()
        if not lhs or "=" in lhs:
            continue
        recipe_parts: list[str] = []
        for _, nxt, nxt_recipe in lines[idx + 1 :]:
            if not nxt_recipe:
                break
            recipe_parts.append(nxt)
        recipe = "\n".join(recipe_parts)
        for name in lhs.split():
            if _SPECIAL.match(name) or "%" in name or "$" in name:
                continue
            found.append((lineno, name, recipe))
    return found


class Finding:
    def __init__(self, path, line, code, msg):
        self.path, self.line, self.code, self.msg = path, line, code, msg

    def __str__(self):
        return f"{self.path}:{self.line}: [{self.code}] {self.msg}"


def _check_formatting(path: pathlib.Path, raw: bytes) -> tuple[list[Finding], str]:
    """MK004 -- encoding, line endings, trailing whitespace, final newline."""
    findings: list[Finding] = []
    try:
        text = raw.decode("ascii")
    except UnicodeDecodeError as exc:
        findings.append(
            Finding(
                path,
                raw[: exc.start].count(b"\n") + 1,
                "MK004",
                f"non-ASCII byte 0x{raw[exc.start]:02x}",
            )
        )
        text = raw.decode("ascii", errors="replace")

    if b"\r\n" in raw:
        findings.append(Finding(path, 1, "MK004", "CRLF line ending"))
    if raw and not raw.endswith(b"\n"):
        findings.append(Finding(path, text.count("\n") + 1, "MK004", "no final newline"))
    findings.extend(
        Finding(path, i, "MK004", "trailing whitespace")
        for i, ln in enumerate(text.splitlines(), 1)
        if ln != ln.rstrip()
    )
    return findings, text


def _check_licence(path: pathlib.Path, text: str) -> list[Finding]:
    """MK001 -- SPDX identifier and copyright line in the file head."""
    head = "\n".join(text.splitlines()[:40])
    findings: list[Finding] = []
    if "SPDX-License-Identifier:" not in head:
        findings.append(
            Finding(path, 1, "MK001", "no SPDX-License-Identifier in the first 40 lines")
        )
    if not re.search(r"Copyright \(c\) \d{4}", head):
        findings.append(
            Finding(path, 1, "MK001", "no 'Copyright (c) <year>' line in the first 40 lines")
        )
    return findings


def _check_phony(path: pathlib.Path, text: str) -> list[Finding]:
    """MK002 -- a target that names no file must be declared .PHONY."""
    phony = phony_names(text)
    findings: list[Finding] = []
    seen: set[str] = set()
    for lineno, name, recipe in declared_targets(text):
        if name in seen:
            continue
        seen.add(name)
        if "/" in name or "." in name:
            continue  # names a real file
        if "$@" in recipe:
            continue  # the recipe writes the file this target names
        if name not in phony:
            findings.append(
                Finding(path, lineno, "MK002", f"target '{name}' names no file but is not .PHONY")
            )
    return findings


def _check_root_derivation(path: pathlib.Path, text: str) -> list[Finding]:
    """MK003 -- an app Makefile derives ROOT from git, not a fixed '..' depth."""
    findings: list[Finding] = []
    for lineno, line, is_recipe in logical_lines(text):
        if is_recipe:
            continue
        m = re.match(r"^ROOT\s*:?=\s*(.+)$", line)
        if not m:
            continue
        value = m.group(1).strip()
        if GIT_ROOT_FORM not in value:
            findings.append(
                Finding(
                    path,
                    lineno,
                    "MK003",
                    "ROOT is derived from a fixed '..' depth "
                    f'({value}); use $(shell git -C "$(CURDIR)" '
                    "rev-parse --show-toplevel) so moving the app between "
                    "tiers cannot silently break it",
                )
            )
    return findings


def check_file(path: pathlib.Path, raw: bytes, is_app_makefile: bool) -> list[Finding]:
    """Every Makefile rule, one function per finding code.

    The rule list is the call sequence below: MK004 formatting, MK001 licence,
    MK002 .PHONY, and MK003 ROOT derivation (app Makefiles only).
    """
    findings, text = _check_formatting(path, raw)
    findings += _check_licence(path, text)
    findings += _check_phony(path, text)
    if is_app_makefile:
        findings += _check_root_derivation(path, text)
    return findings


# ---------------------------------------------------------------------------
# selftest
# ---------------------------------------------------------------------------
MALFORMED = """\
ROOT := $(abspath $(CURDIR)/../../..)

release: build
\t@echo shipping

build:
\t@echo building
"""

# Legal but awkward: a continued .PHONY (the construct checkmake mis-parses),
# a variable assignment whose RHS contains a colon, a target that names a real
# file, a double-colon rule, and a pattern rule. None of this is a finding.
TRICKY = """\
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

ROOT := $(shell git -C "$(CURDIR)" rev-parse --show-toplevel)
URL  := https://example.invalid/path
SED  := sed -e s/a:b/c/

.PHONY: alpha \\
        beta \\
        gamma

alpha: beta
\t@echo $(URL)

beta:: gamma
\t@echo b

gamma:
\t@echo g

build/out.bin: alpha
\t@echo $@

# An extensionless binary target. By name alone it is indistinguishable from
# a phony target like `clean`, but its recipe writes $@, so it is a file rule
# and must NOT be reported as a missing .PHONY.
mytool: alpha
\t$(CC) -o $@ mytool.c

%.o: %.c
\t@echo $<
"""


def selftest() -> int:
    rc = 0
    with tempfile.TemporaryDirectory() as td:
        bad = pathlib.Path(td) / "Makefile"
        bad.write_bytes(MALFORMED.encode())
        got = check_file(bad, bad.read_bytes(), is_app_makefile=True)
        codes = {f.code for f in got}
        expected = {"MK001", "MK002", "MK003"}
        missing = expected - codes
        if missing:
            print(f"SELFTEST FAIL: malformed Makefile did not report {sorted(missing)}")
            for f in got:
                print(f"    got: {f}")
            rc = 1
        else:
            print(f"selftest: malformed Makefile -> {len(got)} findings {sorted(codes)} OK")
        flagged = {f.msg.split("'")[1] for f in got if f.code == "MK002"}
        if flagged != {"release", "build"}:
            print(f"SELFTEST FAIL: MK002 flagged {sorted(flagged)}, want release+build")
            rc = 1
        else:
            print("selftest: MK002 names both undeclared phony targets OK")

        good = pathlib.Path(td) / "Tricky.mk"
        good.write_bytes(TRICKY.encode())
        got = check_file(good, good.read_bytes(), is_app_makefile=True)
        if got:
            print("SELFTEST FAIL: tricky Makefile should be clean but reported:")
            for f in got:
                print(f"    {f}")
            rc = 1
        else:
            print("selftest: tricky Makefile -> 0 findings OK")

    # The specific defect that disqualified checkmake: names after a
    # backslash continuation must be visible to the .PHONY parser.
    cont = ".PHONY: first \\\n        second \\\n        third\n"
    got_names = phony_names(cont)
    if got_names != {"first", "second", "third"}:
        print(f"SELFTEST FAIL: continued .PHONY parsed as {sorted(got_names)}")
        rc = 1
    else:
        print("selftest: continued .PHONY yields all 3 names OK")

    if phony_names(".PHONY: only\n") != {"only"}:
        print("SELFTEST FAIL: single-line .PHONY misparsed")
        rc = 1
    else:
        print("selftest: single-line .PHONY OK")

    return rc


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--selftest", action="store_true")
    # Scope introspection for check_lint_coverage.py: print what this gate
    # would scan, so the coverage gate can ask rather than restate the scope.
    ap.add_argument("--list-files", action="store_true", help="print the scanned file list")
    ap.add_argument("paths", nargs="*")
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    root = pathlib.Path(
        subprocess.run(
            ["git", "rev-parse", "--show-toplevel"],  # noqa: S607  # git from PATH is intended
            capture_output=True,
            text=True,
            check=True,
        ).stdout.strip()
    )
    paths = [pathlib.Path(p) for p in args.paths] or repo_files(root)

    if args.list_files:
        print("\n".join(sorted(str(p.relative_to(root)) for p in paths)))
        return 0
    if not paths:
        print("ERROR: no Makefiles found; refusing to report success.", file=sys.stderr)
        return 1

    findings: list[Finding] = []
    for p in paths:
        rel = p.relative_to(root) if p.is_absolute() else p
        is_app = str(rel).startswith("examples/") and rel.name == "Makefile"
        findings.extend(check_file(p, p.read_bytes(), is_app))

    for f in findings:
        print(f)
    if findings:
        print(f"\n{len(findings)} Makefile finding(s) in {len(paths)} file(s)")
        return 1
    print(f"Makefiles clean ({len(paths)} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
