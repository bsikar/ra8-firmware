#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: every ``scripts/...`` path mentioned anywhere in the tree resolves.

``scripts/`` is referenced from workflows, Makefiles, CMake listfiles, the git
hooks, C comments, and ~90 Markdown documents.  Those references are plain
text: nothing type-checks them, so a ``git mv`` inside ``scripts/`` breaks them
silently.  The repository has already been bitten by exactly that -- a rename
left cross-references pointing at nothing and ``ci-fast`` did not notice,
because a dead path in a doc link or a hook comment produces no build error and
no test failure.  It surfaces later as a workflow step that cannot find its
driver.

This gate closes that class.  It reads every first-party text file, extracts
every token that looks like a path under ``scripts/``, and requires it to
resolve on disk.  Run it before a restructuring and it is green; run it after
and every reference you forgot to update is a named, located failure.

Reference forms understood
--------------------------
``scripts/ci.sh``                 repo-relative -- resolved against the repo root
``../../scripts/dev/flash.sh``        relative -- resolved against the citing file
``scripts/checks/``               trailing slash -- must be a directory
``scripts/hil/*.sh``              glob -- must match at least one path
``scripts/{flash,debug}.sh``      brace / ``$VAR`` interpolation -- the longest
                                  literal directory prefix must exist

The glob rule is the interesting one: it is what keeps a doc that says
"``scripts/hil/*.sh``" honest after those scripts move, instead of leaving a
pattern that matches nothing and reads as if it still does.

Scope
-----
Deliberately limited to the ``scripts/`` prefix.  Widening it to every
top-level directory was measured first: 12445 path-like tokens tree-wide, of
which 1223 distinct ones do not resolve -- build artifacts (``tests/build/``),
illustrative globs (``tests/test_*.c``), and third-party prose.  A gate that
starts 1223 findings in the red cannot be landed, and grandfathering them would
make it a gate that enforces nothing.  ``scripts/`` is both the tree being
restructured and the one whose inbound references are load-bearing (a broken
``scripts/`` path in a workflow is a broken CI job), so it is where the rule
pays for itself today.  Extending the same machinery outward is tracked
separately.

Per-line opt-out: append ``PATHREF-OK: <reason>`` to a line to suppress it
(mirrors the ``MAGIC-OK`` / ``CITES-OK`` / ``AI-OK`` family).  It exists for
prose that deliberately names a path that does not exist -- a checker docstring
illustrating a hypothetical path, or a historical note.  The marker has to sit
on the same line as the token it waives, which is why the example below carries
it inline rather than in the prose above:

    a checker docstring naming ``scripts/foo``   PATHREF-OK: hypothetical

Run::

    check_script_references.py             # gate
    check_script_references.py --selftest  # prove it fires and stays quiet

Exit 0 if every reference resolves, 1 on findings, 2 on tool error.
"""

from __future__ import annotations

import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

# The path prefix this gate validates. See the "Scope" note in the module
# docstring for why it is one prefix and not every top-level directory.
ROOT_PREFIX = "scripts"

# Per-line opt-out marker.
OPT_OUT = "PATHREF-OK"

# Vendored / generated / build trees. Their prose is the upstream maintainer's
# call, and several vendor script trees (mbedtls, zephyr) legitimately mention
# their OWN scripts/ paths, which do not exist here and never will.
EXCLUDE_FRAGMENTS = (
    "libs/third_party/",
    "libs/fonts/",
    "port/threadx/",
    "/build/",
    "/build-cov/",
    "/_deps/",
    "node_modules/",
    "tools/vela/generated/",
)

# Suffixes that are binary or generated payloads -- never scanned. Anything not
# listed here is scanned and simply yields nothing if it holds no path text, so
# this is an optimisation and a decode guard, not a scope decision.
BINARY_SUFFIXES = frozenset(
    {
        ".pdf",
        ".png",
        ".jpg",
        ".jpeg",
        ".gif",
        ".bmp",
        ".ico",
        ".webp",
        ".bin",
        ".hex",
        ".elf",
        ".o",
        ".a",
        ".so",
        ".dylib",
        ".map",
        ".ttf",
        ".otf",
        ".woff",
        ".woff2",
        ".epub",
        ".zip",
        ".gz",
        ".xz",
        ".jar",
        ".class",
        ".pyc",
        ".svg",
    }
)

# A path token: an optional run of ``../`` segments, then ``scripts/``, then
# path characters. The leading assertion keeps a ``myscripts/`` or
# ``tools/scripts/`` prefix from matching, while still allowing a leading run
# of parent-directory segments (whose match starts at the first dot, not at
# the slash that follows it).
_TOKEN_RE = re.compile(
    r"(?:(?<=^)|(?<=[^A-Za-z0-9_/.-]))"
    r"((?:\.\./)*" + re.escape(ROOT_PREFIX) + r"/[A-Za-z0-9_./*?{}$@,+\\-]+)"
)

# Trailing characters that are punctuation in the citing text, never part of a
# filename. A trailing ``/`` is meaningful (directory) and is NOT stripped.
_TRAILING_JUNK = ".,;:!?)`'\"|>"

# Characters that mean the token is a shell / make interpolation rather than a
# literal path.
_INTERPOLATION_CHARS = ("$", "{", "}")

# Characters that mean the token is a glob pattern.
_GLOB_CHARS = ("*", "?")


class Finding:
    """One unresolved reference: where it was written and what it said."""

    def __init__(self, rel_file: str, line_no: int, token: str, reason: str) -> None:
        self.rel_file = rel_file
        self.line_no = line_no
        self.token = token
        self.reason = reason

    def __str__(self) -> str:
        return f"{self.rel_file}:{self.line_no}: {self.token} -- {self.reason}"


def _git_ls_files(root: Path) -> list[str]:
    """Tracked plus untracked-but-not-ignored paths under `root`.

    A filesystem walk would sweep in git-excluded local trees (``recon/``,
    ``.claude/worktrees/``) that CI can never see, so the enumeration follows
    git's own view of the tree -- the same choice the sibling checkers make.
    """
    git_tool = shutil.which("git")
    if git_tool is None:
        sys.stderr.write("check_script_references.py: FATAL -- `git` not on PATH\n")
        sys.exit(2)
    proc = subprocess.run(  # noqa: S603 -- fixed argv, resolved tool path
        [git_tool, "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
        cwd=root,
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr)
        sys.stderr.write("check_script_references.py: FATAL -- `git ls-files` failed\n")
        sys.exit(2)
    return [rel for rel in proc.stdout.split("\0") if rel]


def _is_excluded(rel: str) -> bool:
    return any(frag in f"/{rel}" for frag in EXCLUDE_FRAGMENTS)


def _scannable(rel: str) -> bool:
    return not _is_excluded(rel) and Path(rel).suffix.lower() not in BINARY_SUFFIXES


def _literal_prefix(token: str) -> str:
    """The longest leading run of `token` segments free of glob / interpolation.

    ``scripts/checks/{a,b}.py`` -> ``scripts/checks``.  Used to salvage a
    partial check from a token whose tail cannot be resolved literally: the
    directory it lives in still has to exist, and that alone catches a moved
    tree.
    """
    kept: list[str] = []
    for segment in token.split("/"):
        if any(ch in segment for ch in (*_INTERPOLATION_CHARS, *_GLOB_CHARS)):
            break
        kept.append(segment)
    return "/".join(kept)


def _resolve_base(root: Path, rel_file: str, token: str) -> tuple[Path, str]:
    """Return the directory `token` is relative to, and the token minus ``../``.

    A bare ``scripts/...`` is repo-relative by convention.  A ``../../scripts/``
    in a Markdown link is relative to the citing document, which is how the
    qualification docs link to the tree.
    """
    if not token.startswith("../"):
        return root, token
    depth = 0
    rest = token
    while rest.startswith("../"):
        depth += 1
        rest = rest[3:]
    base = (root / rel_file).parent
    for _ in range(depth):
        base = base.parent
    return base, rest


def _check_interpolated(base: Path, rest: str) -> str | None:
    """A ``$VAR`` / brace token resolves as far as its literal prefix goes."""
    prefix = _literal_prefix(rest)
    if prefix and not (base / prefix).exists():
        return f"interpolated path whose literal prefix {prefix!r} does not exist"
    return None


def _check_glob(base: Path, rest: str) -> str | None:
    """A glob has to match something; one that matches nothing is a dead pattern."""
    if not any(base.glob(rest.rstrip("/"))):
        return "glob pattern matches nothing"
    return None


def _check_directory(base: Path, rest: str) -> str | None:
    """A trailing slash asserts a directory, so a file of that name is still wrong."""
    if not (base / rest.rstrip("/")).is_dir():
        return "directory does not exist"
    return None


def _check_literal(base: Path, rest: str, token: str) -> str | None:
    """The ordinary case: the path names something that must be on disk."""
    if not (base / rest).exists():
        return f"no such file (token {token!r})"
    return None


def _classify(base: Path, rest: str, token: str) -> str | None:
    """Return a failure reason for an unresolved reference, or None if it is OK.

    The four reference forms are checked by dedicated predicates so each one
    reads as its own rule, and the order is significant: interpolation and
    globs are recognised before the literal case, because a token carrying
    either cannot be resolved as a plain path.
    """
    if any(ch in rest for ch in _INTERPOLATION_CHARS):
        return _check_interpolated(base, rest)
    if any(ch in rest for ch in _GLOB_CHARS):
        return _check_glob(base, rest)
    if rest.endswith("/"):
        return _check_directory(base, rest)
    return _check_literal(base, rest, token)


def _unescape(token: str) -> str:
    r"""Undo regex escaping, and cut the token where escaping stops meaning a dot.

    ``check_ci_parity.py`` matches gate calls with a regex holding an escaped
    ``.sh`` followed by ``\s+``.  Left alone, the extracted token stops at the
    first backslash and reads as a dangling directory reference; blindly
    stripping every backslash instead welds the pattern's next atom onto the
    filename.  Both are noise on a correct file.  So ``\.`` -- the only escape
    that spells a character a real path can contain -- is unescaped, and the
    token is cut at any other backslash, which is where the filename provably
    ended.
    """
    token = token.replace("\\.", ".")
    head, _, _ = token.partition("\\")
    return head


def _scan_text(rel_file: str, text: str, root: Path = REPO_ROOT) -> list[Finding]:
    """Every unresolved ``scripts/...`` reference in one file's text."""
    findings: list[Finding] = []
    for line_no, line in enumerate(text.splitlines(), start=1):
        if OPT_OUT in line:
            continue
        for match in _TOKEN_RE.finditer(line):
            token = _unescape(match.group(1)).rstrip(_TRAILING_JUNK)
            if not token or (token.endswith("/") and token.count("/") == 1):
                continue  # a bare "scripts/" mention, not a reference
            base, rest = _resolve_base(root, rel_file, token)
            reason = _classify(base, rest, token)
            if reason is not None:
                findings.append(Finding(rel_file, line_no, token, reason))
    return findings


def _scan_file(root: Path, rel: str) -> list[Finding]:
    try:
        text = (root / rel).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    return _scan_text(rel, text, root)


def scan_repo(root: Path = REPO_ROOT) -> list[Finding]:
    """Every unresolved ``scripts/...`` reference in the first-party tree."""
    findings: list[Finding] = []
    for rel in _git_ls_files(root):
        if _scannable(rel):
            findings.extend(_scan_file(root, rel))
    return findings


# ---------------------------------------------------------------------------
# Selftest -- asserts BOTH directions before the real scan is trusted.
# ---------------------------------------------------------------------------

# Selftest fixture bodies are composed from ROOT_PREFIX rather than written
# as literal "scripts/..." text. A literal here would be a real reference in a
# real tracked file, so every deliberately-dead fixture would have to carry a
# PATHREF-OK -- fourteen opt-outs on the one file whose job is to make opt-outs
# rare. Composing them keeps the fixture invisible to the scanner while the
# assertion below still exercises the exact same code path.
_P = ROOT_PREFIX

# (file body, expected finding count, what the case proves)
_SELFTEST_CASES: tuple[tuple[str, int, str], ...] = (
    (f"see {_P}/ci.sh for the gates\n", 0, "a live repo-relative path is clean"),
    (f"see {_P}/nope_missing.sh for gates\n", 1, "a dead path is reported"),
    (f"run `{_P}/checks/check_shell.py`\n", 0, "a live nested path is clean"),
    (f"run `{_P}/checks/gone.py`\n", 1, "a dead nested path is reported"),
    (f"the {_P}/checks/ directory\n", 0, "a live directory reference is clean"),
    (f"the {_P}/nowhere/ directory\n", 1, "a dead directory reference is reported"),
    (f"all of {_P}/checks/check_*.py\n", 0, "a glob that matches is clean"),
    (f"all of {_P}/checks/zzz_*.py\n", 1, "a glob that matches nothing is reported"),
    (f"bash {_P}/checks/${{NAME}}.py\n", 0, "a live interpolated prefix is clean"),
    (f"bash {_P}/gonedir/${{NAME}}.py\n", 1, "a dead interpolated prefix is reported"),
    (f"{_P}/nope_missing.sh  {OPT_OUT}: prose\n", 0, "the opt-out suppresses"),
    (f"my{_P}/nope_missing.sh\n", 0, "a look-alike prefix is not a reference"),
    (f"tools/{_P}/nope_missing.sh\n", 0, "a nested scripts/ dir is not this tree"),
    (f"see {_P}/ci.sh.\n", 0, "trailing prose punctuation is stripped"),
    (f're.compile(r"{_P}/ci\\.sh")\n', 0, "regex-escaped dots are unescaped"),
    (
        f're.compile(r"{_P}/ci\\.sh\\s+--gate")\n',
        0,
        "a token is cut at a non-dot escape, not welded to the next atom",
    ),
)


def _selftest() -> int:
    """Assert the detector fires on dead references and stays quiet on live ones.

    A path checker that silently stopped matching would report a clean tree --
    the failure mode that makes a gate worse than no gate. Both directions are
    asserted here, and the gate body runs this before the real scan.
    """
    failures = 0
    for body, expected, description in _SELFTEST_CASES:
        got = len(_scan_text("docs/selftest_fixture.md", body))
        if got != expected:
            failures += 1
            print(
                f"  FAIL {description}: expected {expected} finding(s), got {got}"
                f"  [{body.strip()}]",
                file=sys.stderr,
            )

    # Relative-form resolution is file-position dependent, so it needs a real
    # citing path rather than the shared fixture above.
    rel_cases = (
        ("docs/qualification/SDP.md", f"../../{_P}/ci.sh", 0),
        ("docs/qualification/SDP.md", f"../../{_P}/nope_missing.sh", 1),
        ("docs/qualification/SDP.md", f"../{_P}/ci.sh", 1),
    )
    for citing, token, expected in rel_cases:
        got = len(_scan_text(citing, f"link to {token}\n"))
        if got != expected:
            failures += 1
            print(
                f"  FAIL relative form {token} from {citing}: expected {expected}, got {got}",
                file=sys.stderr,
            )

    # End-to-end: a real file in a real git tree must turn the whole gate red.
    # The unit cases above exercise _scan_text; this proves the enumeration
    # reaches a newly added file too, so a broken ls-files cannot report clean.
    with tempfile.TemporaryDirectory() as tmp:
        tmp_root = Path(tmp)
        git_tool = shutil.which("git") or "git"
        subprocess.run(  # noqa: S603 -- fixed argv, resolved tool path
            [git_tool, "init", "-q"], cwd=tmp_root, check=True, capture_output=True
        )
        (tmp_root / _P).mkdir()
        (tmp_root / _P / "live.sh").write_text("#!/bin/sh\n")
        (tmp_root / "README.md").write_text(f"ok: {_P}/live.sh\n")
        if scan_repo(tmp_root):
            failures += 1
            print("  FAIL end-to-end: a live tree reported findings", file=sys.stderr)
        (tmp_root / "README.md").write_text(f"bad: {_P}/dead.sh\n")
        if len(scan_repo(tmp_root)) != 1:
            failures += 1
            print("  FAIL end-to-end: a dead reference was not reported", file=sys.stderr)

    if failures:
        print(f"check_script_references.py: --selftest FAILED ({failures})", file=sys.stderr)
        return 1
    total = len(_SELFTEST_CASES) + len(rel_cases) + 2
    print(f"check_script_references.py: --selftest OK ({total} cases, both directions)")
    return 0


def main(argv: list[str]) -> int:
    if "--selftest" in argv[1:]:
        return _selftest()

    findings = scan_repo()
    if not findings:
        print(f"check_script_references.py: every {ROOT_PREFIX}/ reference resolves.")
        return 0

    print(
        f"check_script_references.py: {len(findings)} unresolved {ROOT_PREFIX}/ reference(s):\n",
        file=sys.stderr,
    )
    for finding in sorted(findings, key=lambda f: (f.rel_file, f.line_no)):
        print(f"  {finding}", file=sys.stderr)
    print(
        f"\nUpdate the reference, or -- if the path is deliberately "
        f"hypothetical prose -- append `{OPT_OUT}: <reason>` to the line.",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
