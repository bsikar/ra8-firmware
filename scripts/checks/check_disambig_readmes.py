#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: every disambiguation README's machine-checkable claims still hold.

The tree carries several pairs of things a newcomer can plausibly pick the wrong
one of -- two filesystems, two firmware-update mechanisms, a facade and the
driver underneath it. Each pair gets one very small README answering "which do I
use, and why do both exist". Prose like that is exactly what rots: the library it
names gets renamed, the symbol it cites disappears, the "two apps use this one"
count silently becomes eleven, and nothing notices.

So each of those READMEs carries its load-bearing claims in a machine-readable
block, and this gate re-derives every one of them from the tree::

    <!-- disambig
    this: libs/ra8_fs
    that: libs/ra8_io
    symbol: ra8_fs_format
    symbol: ra8_io_vfs_open
    users: ra8_fs = 31
    users: ra8_io = 15
    files: libs/ra8_fs/src/*.c = 32
    -->

(abridged -- the real block carries more ``symbol`` rows, and the counts
shown are whatever the tree held when this was written. Only the block in
the README is checked; this one is here to show the syntax.)

``this``    the directory this README lives in and speaks for. Exactly one.
``that``    the thing it is being distinguished FROM. One or more.
``symbol``  an identifier the prose cites. Must still occur inside one of the
            declared paths, so a rename or a deletion fails the gate.
``users``   an identifier plus the number of ``examples/**/CMakeLists.txt`` that
            reference it. Re-counted here, so a number in the prose cannot drift.
``files``   a repo-relative glob plus how many files match it. The other way a
            number gets into this kind of prose ("116 bench apps"), and the other
            way it goes stale.

Registration is the block itself: a README containing the marker is in scope the
moment it is committed. There is no second list to keep in sync -- that is the
drift this gate exists to prevent, and a registry of anti-drift READMEs would
reintroduce it one level up.

Like every detector here it carries a **non-vacuity floor**. A scan that finds
almost no blocks, or almost no claims, did not walk the tree it meant to, and
"no drift found" against nothing is the silent pass this gate exists to stop.

``--selftest`` asserts both directions over throwaway trees: a broken path, a
vanished symbol, a stale count and a misfiled ``this`` each FIRE, an in-sync
tree stays QUIET, and a collapsed scan is caught.
"""

from __future__ import annotations

import argparse
import re
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

MARKER = "<!-- disambig"
"""Opening line of the claim block. Its presence is what puts a README in scope."""

BLOCK_RE = re.compile(r"<!--\s*disambig\s*\n(.*?)-->", re.DOTALL)
"""The claim block body, between the marker and the comment terminator."""

CLAIM_RE = re.compile(r"^\s*(this|that|symbol|users|files)\s*:\s*(.+?)\s*$")
"""One claim line. Anything else inside the block is a syntax error, not a comment."""

USERS_RE = re.compile(r"^(?P<token>[A-Za-z0-9_]+)\s*=\s*(?P<count>\d+)$")
"""Right-hand side of a ``users:`` claim: an identifier and the expected count."""

FILES_RE = re.compile(r"^(?P<glob>\S+)\s*=\s*(?P<count>\d+)$")
"""Right-hand side of a ``files:`` claim: a repo-relative glob and its file count."""

NEVER_WALK = {".git", "build", "build-cov", "_deps", "__pycache__", "node_modules"}
"""Directories with no authored content; never walked for any purpose."""

NOT_OURS = {"third_party", "ra8_fonts"}
"""Vendored and generated trees. Excluded from README DISCOVERY so a vendored
README can never be pulled into scope, but NOT from symbol resolution: a block
that says ``that: libs/third_party/filex`` is deliberately pointing there, and a
symbol claim about the vendored side has to be allowed to find its target."""

TEXT_SUFFIXES = {
    ".c",
    ".h",
    ".cpp",
    ".hpp",
    ".py",
    ".sh",
    ".cmake",
    ".txt",
    ".yml",
    ".yaml",
    ".ld",
    ".dox",
    ".conf",
}
"""Suffixes searched when resolving a ``symbol:`` claim.

``.md`` is deliberately absent. With it in, a symbol claim was satisfied by the
very README that made it -- the claim block sits inside a ``.md`` file under the
``this:`` path, so ``symbol: anything_at_all`` matched itself and the check could
never fail. The selftest's rename case caught exactly that. A symbol claim has to
be backed by source, not by the prose citing it."""

MIN_READMES = 4
"""Non-vacuity floor: fewer blocks than this means the scan did not walk the tree."""

MIN_CLAIMS = 15
"""Non-vacuity floor on total claims, so a tree of empty blocks cannot pass."""

EXIT_OK = 0
EXIT_DRIFT = 1
EXIT_VACUOUS = 2


def _walk_files(root: Path, *, ours_only: bool) -> list[Path]:
    """Every file under ``root``, skipping directories with no authored content.

    Args:
        root: Directory to walk.
        ours_only: When True, also skip vendored and generated trees.

    Returns:
        Paths of regular files, in a stable sorted order.
    """
    skip = NEVER_WALK | NOT_OURS if ours_only else NEVER_WALK
    if not root.is_dir():
        return []
    return [
        path
        for path in sorted(root.rglob("*"))
        if not any(part in skip for part in path.parts) and path.is_file()
    ]


def _read(path: Path) -> str:
    """Read a file as text, tolerating undecodable bytes.

    Args:
        path: File to read.

    Returns:
        The file's contents, with undecodable bytes replaced.
    """
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def _word_re(token: str) -> re.Pattern[str]:
    """Compile an identifier-boundary match for ``token``.

    Args:
        token: Identifier to match.

    Returns:
        A pattern matching the token only at identifier boundaries.
    """
    return re.compile(rf"(?<![A-Za-z0-9_]){re.escape(token)}(?![A-Za-z0-9_])")


def find_readmes(root: Path) -> list[Path]:
    """Locate every first-party README carrying a claim block.

    Args:
        root: Repository root to search.

    Returns:
        Paths of in-scope READMEs, sorted.
    """
    files = _walk_files(root, ours_only=True)
    return [p for p in files if p.name == "README.md" and MARKER in _read(p)]


def parse_block(text: str) -> tuple[list[tuple[str, str]], list[str]]:
    """Extract the claim list from one README's text.

    Args:
        text: Full README contents.

    Returns:
        A ``(claims, errors)`` pair. ``claims`` are ``(kind, value)`` in file
        order; ``errors`` describe malformed blocks.
    """
    match = BLOCK_RE.search(text)
    if match is None:
        return [], ["claim block opens but never closes with -->"]
    claims: list[tuple[str, str]] = []
    errors: list[str] = []
    for raw in match.group(1).splitlines():
        if not raw.strip():
            continue
        claim = CLAIM_RE.match(raw)
        if claim is None:
            errors.append(f"unparsable claim line: {raw.strip()!r}")
            continue
        claims.append((claim.group(1), claim.group(2)))
    return claims, errors


def count_example_users(root: Path, token: str) -> int:
    """Count example listfiles that reference ``token`` as a whole identifier.

    Args:
        root: Repository root.
        token: Identifier to look for.

    Returns:
        Number of ``examples/**/CMakeLists.txt`` files mentioning it.
    """
    pattern = _word_re(token)
    hits = 0
    for path in _walk_files(root / "examples", ours_only=True):
        if path.name == "CMakeLists.txt" and pattern.search(_read(path)):
            hits += 1
    return hits


def count_glob(root: Path, pattern: str) -> int:
    """Count files matching a repo-relative glob.

    Args:
        root: Repository root.
        pattern: Glob such as ``examples/x/*/hil.conf``.

    Returns:
        Number of matching regular files.
    """
    return sum(1 for p in root.glob(pattern) if p.is_file())


def symbol_occurs(root: Path, paths: list[str], symbol: str) -> bool:
    """Report whether ``symbol`` still occurs inside any declared path.

    Args:
        root: Repository root.
        paths: Repo-relative ``this``/``that`` paths from the block.
        symbol: Identifier the README cites.

    Returns:
        True when at least one occurrence is found.
    """
    pattern = _word_re(symbol)
    for rel in paths:
        target = root / rel
        candidates = [target] if target.is_file() else _walk_files(target, ours_only=False)
        for path in candidates:
            if path.suffix in TEXT_SUFFIXES and pattern.search(_read(path)):
                return True
    return False


def _structure_problems(root: Path, readme: Path, this: list[str], that: list[str]) -> list[str]:
    """Check a block's shape: one owner, at least one counterpart, live paths.

    Args:
        root: Repository root.
        readme: The README being checked.
        this: Values of the ``this:`` claims.
        that: Values of the ``that:`` claims.

    Returns:
        Problems with the block's structure.
    """
    problems: list[str] = []
    if len(this) != 1:
        problems.append(f"needs exactly one 'this:' claim, found {len(this)}")
    if not that:
        problems.append("needs at least one 'that:' claim")
    owner = readme.parent.relative_to(root).as_posix()
    if this and this[0] != owner:
        problems.append(f"'this: {this[0]}' but the README lives in {owner}")
    problems += [
        f"declared path does not exist: {p}" for p in this + that if not (root / p).exists()
    ]
    return problems


def _symbol_problem(root: Path, live_paths: list[str], value: str) -> str | None:
    """Check one ``symbol:`` claim.

    Args:
        root: Repository root.
        live_paths: Declared paths that exist.
        value: The claimed identifier.

    Returns:
        A problem string, or None when the claim holds.
    """
    if not live_paths:
        return f"symbol '{value}' has no live path to search"
    if not symbol_occurs(root, live_paths, value):
        return f"symbol no longer occurs in the declared paths: {value}"
    return None


def _count_problem(root: Path, kind: str, value: str) -> str | None:
    """Check one ``users:`` or ``files:`` claim by recomputing it.

    Args:
        root: Repository root.
        kind: Either ``users`` or ``files``.
        value: The claim's right-hand side.

    Returns:
        A problem string, or None when the recomputed number matches.
    """
    pattern, shape = (USERS_RE, "TOKEN = N") if kind == "users" else (FILES_RE, "GLOB = N")
    spec = pattern.match(value)
    if spec is None:
        return f"{kind} claim must read '{shape}', got {value!r}"
    subject = spec.group(1)
    claimed = int(spec.group("count"))
    actual = count_example_users(root, subject) if kind == "users" else count_glob(root, subject)
    if actual != claimed:
        return (
            f"{kind} '{subject}' claims {claimed}, tree has {actual}"
            " -- update the README (and any number in its prose)"
        )
    return None


def check_readme(root: Path, readme: Path) -> list[str]:
    """Verify one README's claims against the tree.

    Args:
        root: Repository root.
        readme: The README to check.

    Returns:
        Human-readable problems, each prefixed with the README path; empty when
        every claim holds.
    """
    claims, syntax = parse_block(_read(readme))
    this = [v for k, v in claims if k == "this"]
    that = [v for k, v in claims if k == "that"]
    problems = syntax + _structure_problems(root, readme, this, that)

    live_paths = [p for p in this + that if (root / p).exists()]
    for kind, value in claims:
        found = None
        if kind == "symbol":
            found = _symbol_problem(root, live_paths, value)
        elif kind in ("users", "files"):
            found = _count_problem(root, kind, value)
        if found is not None:
            problems.append(found)

    rel = readme.relative_to(root).as_posix()
    return [f"{rel}: {p}" for p in problems]


def evaluate(root: Path) -> tuple[int, list[str], int, int]:
    """Check every disambiguation README under ``root``.

    Args:
        root: Repository root.

    Returns:
        ``(exit_code, problems, readme_count, claim_count)``.
    """
    readmes = find_readmes(root)
    problems: list[str] = []
    claim_total = 0
    for readme in readmes:
        claims, _ = parse_block(_read(readme))
        claim_total += len(claims)
        problems += check_readme(root, readme)
    if len(readmes) < MIN_READMES or claim_total < MIN_CLAIMS:
        problems.append(
            f"collapsed scan: {len(readmes)} README(s) and {claim_total} claim(s)"
            f" (floor {MIN_READMES}/{MIN_CLAIMS}) -- the scan did not reach the tree"
        )
        return EXIT_VACUOUS, problems, len(readmes), claim_total
    return (EXIT_DRIFT if problems else EXIT_OK), problems, len(readmes), claim_total


_GOOD_README = """# libs/thing -- thing vs other

Use `thing`. `other` exists because of history.

<!-- disambig
this: libs/thing
that: libs/other
symbol: thing_open
symbol: other_open
users: thing = 1
users: other = 0
files: examples/*/CMakeLists.txt = 1
-->
"""


def _make_tree(root: Path, readme_body: str) -> None:
    """Build a throwaway repository the checker can walk.

    Args:
        root: Directory to populate.
        readme_body: Contents of ``libs/thing/README.md``.
    """
    (root / "libs" / "thing").mkdir(parents=True)
    (root / "libs" / "other").mkdir(parents=True)
    (root / "examples" / "app").mkdir(parents=True)
    (root / "libs" / "thing" / "thing.c").write_text("void thing_open(void) {}\n")
    (root / "libs" / "other" / "other.c").write_text("void other_open(void) {}\n")
    (root / "examples" / "app" / "CMakeLists.txt").write_text("LIBS thing\n")
    (root / "libs" / "thing" / "README.md").write_text(readme_body)


def _selftest_cases() -> list[tuple[str, str, bool]]:
    """Fixtures asserting both directions.

    Returns:
        ``(name, readme_body, must_fire)`` triples.
    """
    return [
        ("MUST NOT FIRE: every claim holds", _GOOD_README, False),
        (
            "MUST FIRE: declared path is gone",
            _GOOD_README.replace("that: libs/other", "that: libs/vanished"),
            True,
        ),
        (
            "MUST FIRE: cited symbol was renamed away",
            _GOOD_README.replace("symbol: thing_open", "symbol: thing_opened"),
            True,
        ),
        (
            "MUST FIRE: user count drifted",
            _GOOD_README.replace("users: thing = 1", "users: thing = 7"),
            True,
        ),
        (
            "MUST FIRE: README misfiled against its own 'this'",
            _GOOD_README.replace("this: libs/thing", "this: libs/other"),
            True,
        ),
        (
            "MUST FIRE: claim line is not parsable",
            _GOOD_README.replace("symbol: thing_open\n", "symbol thing_open\n"),
            True,
        ),
        (
            "MUST FIRE: file-count claim drifted",
            _GOOD_README.replace("examples/*/CMakeLists.txt = 1", "examples/*/CMakeLists.txt = 9"),
            True,
        ),
        ("MUST FIRE: block never closes", _GOOD_README.replace("-->", ""), True),
    ]


def _selftest() -> int:
    """Prove the gate fires on drift, stays quiet in sync, and rejects vacuity.

    Returns:
        0 when every fixture behaves, 1 otherwise.
    """
    failures: list[str] = []
    # The non-vacuity floor would swallow every single-README fixture, so the
    # per-case runs measure PROBLEMS and the floor gets a dedicated case below.
    for name, body, must_fire in _selftest_cases():
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _make_tree(root, body)
            fired = bool(check_readme(root, root / "libs" / "thing" / "README.md"))
        if fired != must_fire:
            failures.append(f"  {name}: fired={fired}, expected={must_fire}")
        else:
            print(f"  ok   {name}")

    with tempfile.TemporaryDirectory() as tmp:
        empty = Path(tmp)
        (empty / "examples").mkdir(parents=True)
        code, _, _, _ = evaluate(empty)
    if code != EXIT_VACUOUS:
        failures.append(f"  MUST FIRE: a tree with no blocks is vacuous, got exit {code}")
    else:
        print("  ok   MUST FIRE: a scan that finds nothing is vacuous, not clean")

    live, _, live_readmes, live_claims = evaluate(REPO_ROOT)
    if live == EXIT_VACUOUS:
        failures.append("  the live tree trips the non-vacuity floor")
    else:
        print(f"  ok   live scope: {live_readmes} README(s), {live_claims} claim(s)")

    if failures:
        print("check_disambig_readmes selftest FAILED:", file=sys.stderr)
        print("\n".join(failures), file=sys.stderr)
        return 1
    print("check_disambig_readmes: selftest passed (both directions + floor).")
    return 0


def main(argv: list[str] | None = None) -> int:
    """Entry point.

    Args:
        argv: Command line, defaulting to ``sys.argv[1:]``.

    Returns:
        0 when every claim holds, 1 on drift, 2 on a collapsed scan.
    """
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--selftest", action="store_true", help="prove both directions fire")
    args = parser.parse_args(argv)
    if args.selftest:
        return _selftest()

    code, problems, readmes, claims = evaluate(REPO_ROOT)
    if code == EXIT_OK:
        print(f"check_disambig_readmes: {readmes} README(s), {claims} claim(s) all still hold.")
        return EXIT_OK
    label = "collapsed scan" if code == EXIT_VACUOUS else "stale claim(s)"
    print(f"check_disambig_readmes: {label}:", file=sys.stderr)
    for problem in problems:
        print(f"  {problem}", file=sys.stderr)
    return code


if __name__ == "__main__":
    sys.exit(main())
