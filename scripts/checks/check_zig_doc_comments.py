#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: public Zig declarations carry doc comments, and that debt only shrinks.

Doxygen has a doc-attachment gate for C. Zig had none, which is the blind spot
#900 names: as libraries move from C to Zig behind unchanged C headers, the
implementation-side reference at ``/api/zig/<unit>/`` (ADR-0005) would render
pages of bare signatures and no CI gate would notice. ``zig build docs`` emits
whatever is there; it never asks whether anything was written.

What is checked, for every unit listed as ``documented`` in
``config/zig_doc_units.json``:

* every tracked ``<build_root>/src/**/*.zig`` opens with a ``//!`` module doc
  comment, because that text is the module's landing page in autodoc;
* every top-level ``pub`` declaration in those files has a ``///`` doc comment
  on the line immediately above it.

Scope is deliberate. Only ``src/`` is read: the ``docs`` step is built from the
unit's own module, so test roots never reach the site. Only column-zero ``pub``
lines count: a declaration nested inside a container is documented as part of
its parent, and anchoring at column zero keeps a ``pub`` inside a multiline
string literal from being read as code.

Today's undocumented declarations are listed in
``.github/zig-doc-comment-baseline.txt``. The list is a paydown ledger, not a
permission slip: a declaration in the baseline that has since been documented
is a failure, so the file can only shrink, and a new public declaration that
arrives undocumented fails on the spot.

Run::

    check_zig_doc_comments.py --check          # the gate
    check_zig_doc_comments.py --selftest       # gate probes, no repository state
    check_zig_doc_comments.py --emit-baseline  # the ledger as it should read now
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
MANIFEST_PATH = REPO_ROOT / "config" / "zig_doc_units.json"
BASELINE_PATH = REPO_ROOT / ".github" / "zig-doc-comment-baseline.txt"
TOOL = "check_zig_doc_comments.py"

BASELINE_HEADER = [
    "# Public Zig declarations that still have no /// doc comment (#900).",
    "#",
    "# Written by scripts/checks/check_zig_doc_comments.py --emit-baseline and",
    "# enforced by --check. This is a paydown ledger: document a declaration and",
    "# delete its row. A row that is documented again becomes a failure, so the",
    "# list can only shrink, and a new public declaration must arrive documented.",
    "#",
    "# <path>\t<declaration>",
]

# `pub` modifiers Zig allows between `pub` and the declaration keyword.
_MODIFIERS = r'(?:export\s+|extern\s+(?:"[^"]*"\s+)?|inline\s+|noinline\s+|threadlocal\s+|comptime\s+)*'
_DECL = re.compile(r"^pub\s+" + _MODIFIERS + r"(fn|const|var|usingnamespace)\b\s*([A-Za-z_][A-Za-z0-9_]*)?")


class Decl:
    """One top-level public declaration and whether prose is attached to it."""

    def __init__(self, path: str, name: str, line: int, documented: bool) -> None:
        self.path = path
        self.name = name
        self.line = line
        self.documented = documented

    @property
    def key(self) -> tuple[str, str]:
        return (self.path, self.name)


def load_manifest(path: Path) -> dict:
    """Return the parsed manifest, or exit 1 with a precise diagnostic."""
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        print(f"{TOOL}: {path} not found", file=sys.stderr)
        raise SystemExit(1)
    except json.JSONDecodeError as error:
        print(f"{TOOL}: {path}: {error}", file=sys.stderr)
        raise SystemExit(1)


def documented_sources(manifest: dict, root: Path) -> list[str]:
    """Tracked src/ Zig sources of every documented unit, manifest order."""
    roots = [entry.get("build_root", "") for entry in manifest.get("documented", [])]
    listing = subprocess.run(
        ["git", "ls-files", "*.zig"],
        cwd=root,
        check=True,
        capture_output=True,
        text=True,
    ).stdout.split()
    sources: list[str] = []
    for build_root in roots:
        if not build_root:
            continue
        prefix = f"{build_root}/src/"
        sources.extend(sorted(p for p in listing if p.startswith(prefix)))
    return sources


def module_doc_problem(path: str, text: str) -> str | None:
    """A source with no `//!` header has no landing prose in autodoc."""
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped:
            continue
        if stripped.startswith("//!"):
            return None
        return f"{path} has no //! module doc comment (autodoc renders it as the module's landing page)"
    return f"{path} is empty: a documented module needs a //! header"


def public_decls(path: str, text: str) -> tuple[list[Decl], list[str]]:
    """Every column-zero `pub` declaration, plus lines that could not be read."""
    decls: list[Decl] = []
    problems: list[str] = []
    lines = text.splitlines()
    for index, line in enumerate(lines):
        if not line.startswith("pub "):
            continue
        match = _DECL.match(line)
        if match is None:
            problems.append(f"{path}:{index + 1}: cannot read the name of this public declaration: {line.strip()}")
            continue
        keyword, name = match.group(1), match.group(2)
        if keyword == "usingnamespace":
            name = "usingnamespace"
        elif not name:
            problems.append(f"{path}:{index + 1}: public {keyword} with no name: {line.strip()}")
            continue
        previous = lines[index - 1].strip() if index else ""
        decls.append(Decl(path, name, index + 1, previous.startswith("///")))
    return decls, problems


def parse_baseline(text: str) -> tuple[list[tuple[str, str]], list[str]]:
    """Rows of the paydown ledger, in file order, plus malformed-row reports."""
    rows: list[tuple[str, str]] = []
    problems: list[str] = []
    for number, line in enumerate(text.splitlines(), start=1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) != 2 or not fields[0].strip() or not fields[1].strip():
            problems.append(f"{BASELINE_PATH.name}:{number}: expected '<path>\\t<declaration>', got: {line}")
            continue
        rows.append((fields[0].strip(), fields[1].strip()))
    return rows, problems


def reconcile(decls: list[Decl], baseline: list[tuple[str, str]]) -> list[str]:
    """Every way the tree and the ledger can disagree, as plain sentences."""
    problems: list[str] = []
    seen = {decl.key: decl for decl in decls}
    listed = set(baseline)

    for key in sorted(listed - set(seen)):
        problems.append(
            f"{key[0]}: baseline lists '{key[1]}', which is no longer a public declaration there. Delete the row."
        )

    for decl in decls:
        if decl.documented and decl.key in listed:
            problems.append(
                f"{decl.path}:{decl.line}: '{decl.name}' is documented now. "
                "Delete its row from the baseline (the ledger only shrinks)."
            )
        elif not decl.documented and decl.key not in listed:
            problems.append(
                f"{decl.path}:{decl.line}: public '{decl.name}' has no /// doc comment. "
                "Autodoc would render it as a bare signature."
            )

    duplicates = sorted({key for key in listed if baseline.count(key) > 1})
    for key in duplicates:
        problems.append(f"{key[0]}: baseline lists '{key[1]}' more than once")

    return problems


def render_baseline(decls: list[Decl]) -> str:
    """The ledger as it should read for the tree in front of us."""
    rows = sorted({f"{decl.path}\t{decl.name}" for decl in decls if not decl.documented})
    return "\n".join(BASELINE_HEADER + rows) + "\n"


def collect(root: Path, manifest: dict) -> tuple[list[Decl], list[str]]:
    """Read every documented unit's sources into declarations and problems."""
    decls: list[Decl] = []
    problems: list[str] = []
    sources = documented_sources(manifest, root)
    if not sources:
        problems.append("no documented unit has any tracked src/*.zig source")
    for path in sources:
        text = (root / path).read_text(encoding="utf-8")
        missing = module_doc_problem(path, text)
        if missing:
            problems.append(missing)
        found, unreadable = public_decls(path, text)
        decls.extend(found)
        problems.extend(unreadable)
    return decls, problems


def selftest() -> int:
    """Probe the gate itself: every rule must fire, and must not over-fire."""
    failures: list[str] = []

    documented_src = '//! module\n\n/// Does a thing.\npub fn work() void {}\n'
    decls, unreadable = public_decls("u/src/a.zig", documented_src)
    if unreadable or len(decls) != 1 or not decls[0].documented or decls[0].name != "work":
        failures.append("a /// comment directly above a pub fn should attach to it")
    if module_doc_problem("u/src/a.zig", documented_src) is not None:
        failures.append("a //! header should satisfy the module doc rule")
    if reconcile(decls, []):
        failures.append("a documented declaration with an empty baseline should pass")

    detached = '//! module\n\n/// Does a thing.\n\npub fn work() void {}\n'
    decls, _ = public_decls("u/src/a.zig", detached)
    if decls[0].documented:
        failures.append("a /// comment separated by a blank line should not attach")
    if not reconcile(decls, []):
        failures.append("an undocumented declaration missing from the baseline should fail")
    if reconcile(decls, [("u/src/a.zig", "work")]):
        failures.append("an undocumented declaration listed in the baseline should pass")

    plain_comment = '//! module\n// Does a thing.\npub const Thing = struct {};\n'
    decls, _ = public_decls("u/src/a.zig", plain_comment)
    if decls[0].documented:
        failures.append("an ordinary // comment should not count as documentation")

    decls, _ = public_decls("u/src/a.zig", documented_src)
    if not reconcile(decls, [("u/src/a.zig", "work")]):
        failures.append("a baseline row for a now-documented declaration should fail as stale")
    if not reconcile(decls, [("u/src/gone.zig", "vanished")]):
        failures.append("a baseline row for a declaration that no longer exists should fail")
    if not reconcile(decls, [("u/src/a.zig", "work"), ("u/src/a.zig", "work")]):
        failures.append("a duplicated baseline row should fail")

    nested = '//! module\n\n/// A container.\npub const Holder = struct {\n    pub fn inner() void {}\n};\n'
    decls, _ = public_decls("u/src/a.zig", nested)
    if len(decls) != 1:
        failures.append("only column-zero declarations are top level")

    modifiers = (
        '//! module\n'
        '/// One.\npub export fn exported() void {}\n'
        '/// Two.\npub extern "c" fn imported() void;\n'
        '/// Three.\npub inline fn inlined() void {}\n'
        '/// Four.\npub threadlocal var counter: u32 = 0;\n'
        '/// Five.\npub usingnamespace @import("other.zig");\n'
    )
    decls, unreadable = public_decls("u/src/a.zig", modifiers)
    names = [decl.name for decl in decls]
    if unreadable or names != ["exported", "imported", "inlined", "counter", "usingnamespace"]:
        failures.append(f"every pub modifier form should be read: got {names} {unreadable}")

    _, unreadable = public_decls("u/src/a.zig", "//! module\npub 42 = nonsense;\n")
    if not unreadable:
        failures.append("an unreadable public declaration should be reported, not skipped")

    if module_doc_problem("u/src/a.zig", "const std = @import(\"std\");\n") is None:
        failures.append("a source with no //! header should fail")
    if module_doc_problem("u/src/a.zig", "") is None:
        failures.append("an empty source should fail")

    rows, row_problems = parse_baseline("# comment\n\nu/src/a.zig\twork\n")
    if rows != [("u/src/a.zig", "work")] or row_problems:
        failures.append("comments and blank lines should be skipped, rows kept")
    _, row_problems = parse_baseline("u/src/a.zig work\n")
    if not row_problems:
        failures.append("a row that is not tab separated should be reported")

    rendered = render_baseline(
        [Decl("u/src/a.zig", "work", 5, False), Decl("u/src/a.zig", "done", 9, True)]
    )
    if "\tdone" in rendered or not rendered.endswith("u/src/a.zig\twork\n"):
        failures.append("--emit-baseline should list exactly the undocumented declarations")

    for failure in failures:
        print(f"{TOOL}: selftest: {failure}", file=sys.stderr)
    if failures:
        return 1
    print(f"{TOOL}: selftest OK")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="run the gate")
    parser.add_argument("--selftest", action="store_true", help="probe the gate itself")
    parser.add_argument("--emit-baseline", action="store_true", help="print the ledger for this tree")
    args = parser.parse_args(argv)

    if args.selftest:
        return selftest()

    manifest = load_manifest(MANIFEST_PATH)
    decls, problems = collect(REPO_ROOT, manifest)

    if args.emit_baseline:
        sys.stdout.write(render_baseline(decls))
        return 0

    try:
        baseline_text = BASELINE_PATH.read_text(encoding="utf-8")
    except FileNotFoundError:
        print(f"{TOOL}: {BASELINE_PATH} not found", file=sys.stderr)
        return 1
    rows, row_problems = parse_baseline(baseline_text)
    problems.extend(row_problems)
    problems.extend(reconcile(decls, rows))

    for problem in problems:
        print(f"{TOOL}: {problem}", file=sys.stderr)
    if problems:
        print(f"{TOOL}: regenerate the ledger with --emit-baseline once the prose is written", file=sys.stderr)
        return 1
    undocumented = sum(1 for decl in decls if not decl.documented)
    print(
        f"{TOOL}: OK ({len(decls)} public declarations, "
        f"{len(decls) - undocumented} documented, {undocumented} on the baseline)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
