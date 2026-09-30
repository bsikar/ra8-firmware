#!/usr/bin/env python3
"""Keep a migrated Zig library from quietly regrowing a C implementation.

Every zig-port change is supposed to say whether it deletes the C it replaced or
deliberately keeps it, so the tree never ends up carrying two implementations of
the same library forever. Nothing enforced that: a .c file surviving inside a
library that already has a build.zig was invisible, and a parallel tree is the
one thing a half-finished port should never leave behind.

A library counts as migrated when it has a build.zig. Inside one, a .c file is
allowed when either:

  - it sits under tests/, because a C test fixture is how the Zig gets checked
    against the C ABI it has to keep. Those are deliberately C and always will
    be; making somebody list each one would be noise.
  - it has a row in the allow-list naming the library, the path and a real
    reason. That is the "deliberately keeps it" case, written down.

Anything else is an error. Adding a .c back into a migrated library's src/ now
costs one line in the allow-list and a sentence about why, which is exactly the
decision this is meant to force into the open.

A branch with no migrated library at all passes: that is the base branch's
correct state, not something to complain about. An allow-list row for a library
that is not migrated on this branch is tolerated for the same reason, since the
C tree and the Zig tree are different branches.

Usage:
    check_zig_parallel_trees.py
    check_zig_parallel_trees.py --root <path>
    check_zig_parallel_trees.py --selftest
"""

from __future__ import annotations

import argparse
import sys
import tempfile
from collections.abc import Callable
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
ALLOWLIST_NAME = "zig-parallel-tree-allowlist.tsv"
ALLOWLIST_FIELDS = 3
MIN_REASON_CHARS = 20
TEST_DIR = "tests"


class ParallelTreeError(Exception):
    """A condition that must fail the check rather than quietly pass."""


def parse_allowlist(text: str, name: str = ALLOWLIST_NAME) -> dict[tuple[str, str], str]:
    """Map (library, path-within-library) to the recorded reason."""
    rows: dict[tuple[str, str], str] = {}
    for lineno, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        fields = [f.strip() for f in line.split("\t") if f.strip()]
        if len(fields) != ALLOWLIST_FIELDS:
            msg = (
                f"{name}:{lineno}: expected {ALLOWLIST_FIELDS} tab-separated fields "
                f"(lib, path, reason), got {len(fields)}"
            )
            raise ParallelTreeError(msg)
        lib, path, reason = fields
        if len(reason) < MIN_REASON_CHARS:
            msg = (
                f"{name}:{lineno}: reason for {lib}/{path} is {len(reason)} chars; "
                f"at least {MIN_REASON_CHARS} are needed to say why the C stays"
            )
            raise ParallelTreeError(msg)
        rows[(lib, path)] = reason
    return rows


def migrated_libs(libs_dir: Path) -> list[Path]:
    """Return every library directory carrying a build.zig."""
    if not libs_dir.is_dir():
        msg = f"{libs_dir} is not a directory; cannot report a pass"
        raise ParallelTreeError(msg)
    return sorted(d for d in libs_dir.iterdir() if d.is_dir() and (d / "build.zig").is_file())


def stray_c_files(lib: Path) -> list[str]:
    """Return .c paths inside one library, excluding test fixtures."""
    out = []
    for c in sorted(lib.rglob("*.c")):
        rel = c.relative_to(lib).as_posix()
        if rel.split("/")[0] == TEST_DIR:
            continue
        out.append(rel)
    return out


def audit(root: Path) -> tuple[list[str], int, int]:
    """Return (errors, migrated library count, allowed-C count)."""
    allowlist_path = root / ".github" / ALLOWLIST_NAME
    if not allowlist_path.is_file():
        msg = f"{allowlist_path} is missing; cannot report a pass"
        raise ParallelTreeError(msg)
    allowed = parse_allowlist(allowlist_path.read_text())

    errors: list[str] = []
    libs = migrated_libs(root / "libs")
    kept = 0
    for lib in libs:
        for rel in stray_c_files(lib):
            key = (lib.name, rel)
            if key in allowed:
                kept += 1
                continue
            errors.append(
                f"{lib.name}/{rel}: C file inside a migrated library "
                f"(libs/{lib.name}/build.zig exists) and not under {TEST_DIR}/. "
                f"Delete it, or add a row to .github/{ALLOWLIST_NAME} saying why "
                f"the C stays."
            )
    return errors, len(libs), kept


REASON = "kept on purpose for a documented reason"


def _scaffold(tmp: Path, allow: str = "") -> Path:
    """Write a minimal tree with an allow-list and an empty libs/."""
    (tmp / ".github").mkdir(parents=True, exist_ok=True)
    (tmp / ".github" / ALLOWLIST_NAME).write_text(allow)
    (tmp / "libs").mkdir(exist_ok=True)
    return tmp


def _lib(tmp: Path, name: str, *, migrated: bool, files: tuple[str, ...]) -> Path:
    """Create one library directory, optionally migrated, with the given files."""
    lib = tmp / "libs" / name
    lib.mkdir(parents=True, exist_ok=True)
    if migrated:
        (lib / "build.zig").write_text("")
    for rel in files:
        target = lib / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text("")
    return lib


def _check_src_c(record: Callable[[str, bool], None]) -> None:
    """A .c in src/ is an error until it is allow-listed."""
    with tempfile.TemporaryDirectory() as td:
        tmp = _scaffold(Path(td))
        _lib(tmp, "ra8_thing", migrated=True, files=("src/thing.zig",))
        errors, libs, kept = audit(tmp)
        record("a fully ported library passes", errors == [] and libs == 1 and kept == 0)

        _lib(tmp, "ra8_thing", migrated=True, files=("src/thing.c",))
        errors, _, _ = audit(tmp)
        record("a stray .c in src/ is an error", len(errors) == 1)
        record("the error names the file", bool(errors) and "ra8_thing/src/thing.c" in errors[0])

    with tempfile.TemporaryDirectory() as td:
        tmp = _scaffold(Path(td), f"ra8_thing\tsrc/thing.c\t{REASON}\n")
        _lib(tmp, "ra8_thing", migrated=True, files=("src/thing.c",))
        errors, _, kept = audit(tmp)
        record("an allow-listed .c passes and is counted", errors == [] and kept == 1)


def _check_exemptions(record: Callable[[str, bool], None]) -> None:
    """Test fixtures, unmigrated libraries and stale rows are all left alone."""
    with tempfile.TemporaryDirectory() as td:
        tmp = _scaffold(Path(td))
        _lib(tmp, "ra8_thing", migrated=True, files=("tests/abi_fixture.c",))
        errors, _, kept = audit(tmp)
        record("a C test fixture needs no row", errors == [] and kept == 0)

    with tempfile.TemporaryDirectory() as td:
        tmp = _scaffold(Path(td))
        _lib(tmp, "ra8_plain", migrated=False, files=("src/plain.c",))
        errors, libs, _ = audit(tmp)
        record("an unmigrated library is left alone", errors == [] and libs == 0)

    with tempfile.TemporaryDirectory() as td:
        tmp = _scaffold(Path(td), f"gone_lib\tsrc/gone.c\t{REASON}\n")
        errors, libs, _ = audit(tmp)
        record("a row for a library not migrated here is tolerated", errors == [] and libs == 0)


def _check_refusals(record: Callable[[str, bool], None]) -> None:
    """Malformed input and a missing tree must error, never pass."""

    def refuses(call: Callable[[], object]) -> bool:
        try:
            call()
        except ParallelTreeError:
            return True
        return False

    record("a short row errors", refuses(lambda: parse_allowlist("lib\tpath")))
    record("a thin reason errors", refuses(lambda: parse_allowlist("lib\tpath\ttoo short")))
    with tempfile.TemporaryDirectory() as td:
        bare = Path(td)
        record("a missing allow-list errors", refuses(lambda: audit(bare)))
    with tempfile.TemporaryDirectory() as td:
        tmp = _scaffold(Path(td))
        (tmp / "libs").rmdir()
        record("a missing libs/ directory errors", refuses(lambda: audit(tmp)))


def selftest() -> int:
    """Exercise both directions on synthetic trees."""
    failures: list[str] = []

    def record(label: str, cond: bool) -> None:
        print(f"  [{'ok' if cond else 'FAIL'}] {label}")
        if not cond:
            failures.append(label)

    _check_src_c(record)
    _check_exemptions(record)
    _check_refusals(record)

    print(f"selftest: {len(failures)} failure(s)")
    return 1 if failures else 0


def main(argv: list[str] | None = None) -> int:
    """Audit migrated libraries for C that nobody chose to keep."""
    parser = argparse.ArgumentParser(description="Zig parallel-tree audit.")
    parser.add_argument("--root", type=Path, default=REPO_ROOT)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)

    if args.selftest:
        return selftest()

    try:
        errors, libs, kept = audit(args.root)
    except ParallelTreeError as exc:
        print(f"zig-parallel-trees: {exc}", file=sys.stderr)
        return 2

    for err in errors:
        print(f"zig-parallel-trees: {err}", file=sys.stderr)
    if errors:
        return 1
    print(
        f"zig-parallel-trees: {libs} migrated librarie(s), "
        f"{kept} C file(s) deliberately kept and recorded, no parallel trees"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
