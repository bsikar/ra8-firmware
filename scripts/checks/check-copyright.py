#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: one canonical copyright + SPDX preamble, same place in every file.

The preamble is the first content of every first-party file: an
``SPDX-License-Identifier`` line immediately followed by a ``Copyright`` line,
sitting right after the shebang when there is one and ahead of the descriptive
comment block / docstring.  One order, one text, one position -- so the header
reads the same whether you open a shell script, a firmware ``.c``, or a
``CMakeLists.txt``.

Why the check grew placement teeth
----------------------------------
The old form of this checker asked only whether the two strings appeared
*somewhere* in the file.  That let the tree drift into three different
conventions at once: scripts carried ``SPDX`` then ``Copyright`` right after
the shebang; the C tree carried ``@copyright`` then ``SPDX`` buried at the END
of the ``@file`` Doxygen block; and one shell gate (``scripts/emu/matrix.sh``)
had the pair stranded sixty lines deep, in the middle of its header comment.
Every one of those passed a presence-only check.  "Present somewhere" is not a
standard, so this gate now fixes the ORDER, the POSITION and the exact TEXT.

The canonical form, by comment syntax
-------------------------------------
Hash-comment files (``.sh`` ``.py`` ``.cmake`` ``.mk`` ``.yml`` ``.yaml``
``CMakeLists.txt`` ``Makefile``), immediately after the shebang if present::

    #!/usr/bin/env bash
    # SPDX-License-Identifier: MIT
    # Copyright (c) 2026 Brighton Sikarskie
    #
    # ... the descriptive block / module docstring follows ...

Block-comment files (``.c`` ``.h`` ``.cpp`` ``.hpp`` ``.ld``), as the very
first lines, ahead of the ``@file`` Doxygen block::

    /*
     * SPDX-License-Identifier: MIT
     * Copyright (c) 2026 Brighton Sikarskie
     */
    /**
     * @file ...
     */

Order + position, and why
-------------------------
``SPDX-License-Identifier`` leads because that is the SPDX / REUSE / Linux-
kernel convention -- a licence scanner expects it as the first line -- and the
copyright line pairs directly beneath it.  Both lead the file so the preamble
is found at a fixed offset (0, or 1 when a shebang occupies line 0) instead of
wherever a given file's header prose happens to end.  The text is taken
verbatim from ``LICENSE.txt`` (MIT; ``Copyright (c) 2026 Brighton Sikarskie``).

Scope
-----
First-party code and build files only, enumerated from ``git ls-files`` via
``lint_targets`` so a new top-level directory is covered the day it lands.
Vendored SOUP (``libs/third_party/``), generated tables (``libs/ra8_fonts/``,
generated font fixtures) and build output are out of scope, matching the
sibling gates.  ``.md`` is documentation, not code, and is not judged.

Non-vacuity
-----------
``--selftest`` asserts BOTH directions over crafted strings -- every wrong
shape (missing, reversed order, buried, wrong text) must fire and the canonical
shape must stay silent -- and ``--all`` refuses to report a clean tree when the
scan collapses below a floor.

Run::

    check-copyright.py FILE [FILE ...]   # check named files (pre-commit hook)
    check-copyright.py --all             # check every first-party file (gate)
    check-copyright.py --fix [FILE ...]  # rewrite headers to canonical form
    check-copyright.py --fix --all       # ... across the whole tree
    check-copyright.py --selftest        # prove the rules fire and stay quiet

Exit 0 clean, 1 on a violation or a failing selftest, 2 on a collapsed scan or
a usage error.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from lint_targets import files_for, is_build_output_path

REPO_ROOT = Path(__file__).resolve().parents[2]

EXIT_OK = 0
EXIT_FAIL = 1
EXIT_CONFIG = 2

# The canonical preamble text -- verbatim from LICENSE.txt.
SPDX_TEXT = "SPDX-License-Identifier: MIT"
COPYRIGHT_TEXT = "Copyright (c) 2026 Brighton Sikarskie"

# The two comment syntaxes this tree uses for a file header.
STYLE_HASH = "hash"  # `# ...`   -- shell, python, cmake, make
STYLE_BLOCK = "block"  # `/* ... */` -- C family, linker scripts

# lint_targets language -> comment style. Markdown is deliberately absent: it
# is documentation, not code. YAML carries the same hash-comment preamble as a
# shell script (minus the shebang).
LANG_STYLE = {
    "c": STYLE_BLOCK,
    "ld": STYLE_BLOCK,
    "shell": STYLE_HASH,
    "python": STYLE_HASH,
    "cmake": STYLE_HASH,
    "make": STYLE_HASH,
    "yaml": STYLE_HASH,
}
ENFORCED_LANGS = tuple(LANG_STYLE)

# Suffix / basename -> comment style, for the argv (pre-commit) path where a
# file is judged directly rather than enumerated by language.
_SUFFIX_STYLE = {
    ".c": STYLE_BLOCK,
    ".h": STYLE_BLOCK,
    ".cpp": STYLE_BLOCK,
    ".hpp": STYLE_BLOCK,
    ".cc": STYLE_BLOCK,
    ".cxx": STYLE_BLOCK,
    ".hh": STYLE_BLOCK,
    ".hxx": STYLE_BLOCK,
    ".ld": STYLE_BLOCK,
    ".py": STYLE_HASH,
    ".sh": STYLE_HASH,
    ".bash": STYLE_HASH,
    ".cmake": STYLE_HASH,
    ".mk": STYLE_HASH,
    ".yml": STYLE_HASH,
    ".yaml": STYLE_HASH,
}
_BASENAME_STYLE = {
    "CMakeLists.txt": STYLE_HASH,
    "Makefile": STYLE_HASH,
    "GNUmakefile": STYLE_HASH,
}

# Generated data that is not hand-authored, so exempt like libs/ra8_fonts.
_GENERATED_SUFFIXES_BASENAMES = ("font_fixture.h", "fixture_ahem.h")

# A tree this size cannot legitimately collapse to a handful of files. A scan
# that enumerates almost nothing reports a clean tree because it looked at
# almost nothing -- the same trip-wire the sibling gates carry. Measured
# 2026-08-02: ~3060 first-party header-bearing files in the enforced languages.
FILE_FLOOR = 1500

# Smallest closing-``*/`` index that still has a line above it worth tidying.
_CLOSE_NEEDS_BODY = 1


# ---------------------------------------------------------------------------
# The canonical form, as text and as a classifier.
# ---------------------------------------------------------------------------


def canonical_lines(style: str, shebang: str | None = None) -> list[str]:
    """Return the canonical preamble lines for a comment `style`.

    Args:
        style: ``STYLE_HASH`` or ``STYLE_BLOCK``.
        shebang: A shebang line to place first (hash style only), or None.

    Returns:
        The preamble as a list of lines without trailing newlines.
    """
    if style == STYLE_BLOCK:
        return ["/*", f" * {SPDX_TEXT}", f" * {COPYRIGHT_TEXT}", " */"]
    head = [shebang] if shebang else []
    return [*head, f"# {SPDX_TEXT}", f"# {COPYRIGHT_TEXT}"]


def _hashless(line: str) -> str:
    """Strip a leading ``#`` (and one space) from a hash-comment line."""
    return line.lstrip().removeprefix("#").strip()


def _starless(line: str) -> str:
    """Strip a block-comment ``*`` / ``/*`` / ``*/`` frame from a line."""
    body = line.strip()
    for lead in ("/*", "*/", "*"):
        if body.startswith(lead):
            body = body[len(lead) :]
            break
    return body.strip()


def _is_copyright(body: str) -> bool:
    """True when comment-stripped `body` is the copyright line.

    Accepts the bare form and the C ``@copyright`` Doxygen-tag prefix, so the
    fixer recognises the old in-``@file`` spelling it is replacing.
    """
    return body in {COPYRIGHT_TEXT, f"@copyright {COPYRIGHT_TEXT}"}


def _is_spdx(body: str) -> bool:
    """True when comment-stripped `body` is the SPDX line."""
    return body == SPDX_TEXT


def classify(lines: list[str], style: str) -> str | None:
    """Judge a file's leading lines, returning a violation reason or None.

    Args:
        lines: The file's lines (trailing newlines already stripped is fine;
            they are ignored).
        style: ``STYLE_HASH`` or ``STYLE_BLOCK``.

    Returns:
        None when the preamble is canonical; otherwise a short reason.
    """
    stripped = [ln.rstrip("\r\n") for ln in lines]
    if style == STYLE_BLOCK:
        return _classify_block(stripped)
    return _classify_hash(stripped)


def _classify_hash(lines: list[str]) -> str | None:
    """Judge a hash-comment file (shell / python / cmake / make)."""
    idx = 1 if lines and lines[0].startswith("#!") else 0
    want_spdx = f"# {SPDX_TEXT}"
    want_copy = f"# {COPYRIGHT_TEXT}"
    if len(lines) >= idx + 2 and lines[idx] == want_spdx and lines[idx + 1] == want_copy:
        return None
    return _diagnose(lines, idx)


def _classify_block(lines: list[str]) -> str | None:
    """Judge a block-comment file (C family / linker script)."""
    want = canonical_lines(STYLE_BLOCK)
    if [ln.rstrip() for ln in lines[:4]] == want:
        return None
    return _diagnose(lines, 0)


def _diagnose(lines: list[str], start: int) -> str:
    """Explain why a non-canonical header is wrong, for the error message.

    Args:
        lines: The file's leading lines.
        start: The index the preamble was expected at (after any shebang).

    Returns:
        A short human reason.
    """
    head = lines[: max(80, start + 2)]
    copy_at = spdx_at = None
    for i, ln in enumerate(head):
        body = _hashless(ln) if not ln.strip().startswith(("/*", "*")) else _starless(ln)
        if _is_copyright(body):
            copy_at = i
        elif _is_spdx(body):
            spdx_at = i
    if copy_at is None and spdx_at is None:
        return "missing the SPDX + copyright preamble entirely"
    if copy_at is None:
        return "missing the copyright line"
    if spdx_at is None:
        return "missing the SPDX-License-Identifier line"
    if spdx_at > copy_at:
        return (
            f"wrong order: copyright at line {copy_at + 1}, SPDX at line "
            f"{spdx_at + 1} -- SPDX must come first"
        )
    return (
        f"preamble is not the canonical leading block (SPDX at line "
        f"{spdx_at + 1}, copyright at line {copy_at + 1}; expected them at "
        f"line {start + 1})"
    )


# ---------------------------------------------------------------------------
# The fixer.
# ---------------------------------------------------------------------------


def _rewrite(text: str, style: str) -> str | None:
    """Return `text` with a canonical preamble, or None when already canonical.

    Removes any existing SPDX / copyright lines from the leading comment region
    (including the C ``@copyright`` in-``@file`` spelling), tidies an emptied
    frame, and prepends the canonical preamble after any shebang.
    """
    lines = text.splitlines()
    if classify(lines, style) is None:
        return None
    fixed = _rewrite_hash(lines) if style == STYLE_HASH else _rewrite_block(lines)
    trailing = "\n" if text.endswith("\n") else ""
    return "\n".join(fixed) + trailing


def _rewrite_hash(lines: list[str]) -> list[str]:
    """Rewrite a hash-comment file's header.

    Removes any SPDX / copyright comment line found anywhere in the LEADING
    comment region -- the contiguous run of comment / blank lines before the
    first code line -- not only a pair that already leads the file. That is
    what catches a preamble stranded mid-header, the ``scripts/emu/matrix.sh``
    case, rather than leaving a duplicate behind.
    """
    shebang = None
    body = lines
    if lines and lines[0].startswith("#!"):
        shebang = lines[0]
        body = lines[1:]
    kept: list[str] = []
    in_region = True
    for line in body:
        strip = line.strip()
        if in_region and (strip == "" or strip.startswith("#")):
            if strip.startswith("#") and (
                _is_spdx(_hashless(line)) or _is_copyright(_hashless(line))
            ):
                continue
            kept.append(line)
            continue
        in_region = False
        kept.append(line)
    return [*canonical_lines(STYLE_HASH, shebang), *kept]


def _rewrite_block(lines: list[str]) -> list[str]:
    """Rewrite a block-comment file's header.

    Two shapes are repaired: a dedicated leading ``/* ... */`` licence block
    (replaced wholesale) and the old in-``@file`` spelling where ``@copyright``
    and ``SPDX`` sit at the tail of the first Doxygen block (those two lines
    removed, plus a now-dangling blank ``*`` line, and the canonical block
    prepended).
    """
    body = list(lines)
    # Case 1: an existing dedicated top licence block `/* ... */` whose only
    # payload is the SPDX/copyright pair -- drop it whole.
    if body and body[0].strip() == "/*":
        end = _find_block_end(body)
        if end is not None:
            inner = [_starless(x) for x in body[1:end]]
            if all(v == "" or _is_spdx(v) or _is_copyright(v) for v in inner):
                body = body[end + 1 :]
    # Case 2: strip SPDX/copyright lines anywhere in the FIRST comment block
    # (the @file block), then remove a trailing blank `*` left behind.
    body = _strip_preamble_from_first_block(body)
    return [*canonical_lines(STYLE_BLOCK), *body]


def _find_block_end(lines: list[str]) -> int | None:
    """Index of the line closing the first ``/* ... */`` block, or None."""
    for i, ln in enumerate(lines):
        if i > 0 and "*/" in ln:
            return i
    return None


def _strip_preamble_from_first_block(lines: list[str]) -> list[str]:
    """Strip SPDX / copyright lines and a stranded blank star from the first block."""
    end = _find_block_end(lines) if lines and lines[0].strip().startswith("/*") else None
    if end is None:
        return lines
    kept: list[str] = []
    for i, ln in enumerate(lines):
        if 0 < i < end:
            body = _starless(ln)
            if _is_spdx(body) or _is_copyright(body):
                continue
        kept.append(ln)
    # Drop a blank ` *` immediately before the closing ` */` if the removal
    # left one stranded (need a line before the close to inspect at all).
    new_end = _find_block_end(kept)
    if new_end is not None and new_end > _CLOSE_NEEDS_BODY and kept[new_end - 1].strip() == "*":
        kept.pop(new_end - 1)
    return kept


# ---------------------------------------------------------------------------
# Scope + file enumeration.
# ---------------------------------------------------------------------------


def _style_for(path: Path) -> str | None:
    """Comment style for a path judged directly (argv path), or None."""
    if path.name in _BASENAME_STYLE:
        return _BASENAME_STYLE[path.name]
    return _SUFFIX_STYLE.get(path.suffix.lower())


def _is_generated(rel: str) -> bool:
    """True for generated data files that are exempt like libs/ra8_fonts."""
    return rel.endswith(_GENERATED_SUFFIXES_BASENAMES)


def enumerate_all() -> list[tuple[str, str]]:
    """Every first-party enforced file as ``(rel_path, style)`` pairs."""
    grouped = files_for(ENFORCED_LANGS)
    out: list[tuple[str, str]] = []
    for lang, rels in grouped.items():
        style = LANG_STYLE[lang]
        for rel in rels:
            if _is_generated(rel):
                continue
            out.append((rel, style))
    return sorted(out)


def _check_one(rel: str, style: str) -> str | None:
    """Read and classify one file; returns a reason or None."""
    path = REPO_ROOT / rel if not Path(rel).is_absolute() else Path(rel)
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError as exc:
        return f"unreadable: {exc}"
    return classify(lines, style)


# ---------------------------------------------------------------------------
# Selftest -- both directions, over crafted strings. Nothing is written into
# the tree: a bad fixture stored as a real file would be found by the scan.
# ---------------------------------------------------------------------------

_GOOD_HASH = ["#!/usr/bin/env bash", f"# {SPDX_TEXT}", f"# {COPYRIGHT_TEXT}", "#", "# body"]
_GOOD_HASH_NOSB = [f"# {SPDX_TEXT}", f"# {COPYRIGHT_TEXT}", "", "x = 1"]
_GOOD_BLOCK = ["/*", f" * {SPDX_TEXT}", f" * {COPYRIGHT_TEXT}", " */", "/**", " * @file x.c", " */"]

MUST_STAY_QUIET: tuple[tuple[str, str, list[str]], ...] = (
    ("hash with shebang", STYLE_HASH, _GOOD_HASH),
    ("hash without shebang", STYLE_HASH, _GOOD_HASH_NOSB),
    ("block comment", STYLE_BLOCK, _GOOD_BLOCK),
)

MUST_FIRE: tuple[tuple[str, str, list[str]], ...] = (
    (
        "hash reversed order",
        STYLE_HASH,
        ["#!/usr/bin/env bash", f"# {COPYRIGHT_TEXT}", f"# {SPDX_TEXT}"],
    ),
    ("hash missing spdx", STYLE_HASH, ["#!/usr/bin/env bash", f"# {COPYRIGHT_TEXT}", "# body"]),
    ("hash missing both", STYLE_HASH, ["#!/usr/bin/env bash", "# just a description", "echo hi"]),
    (
        "hash buried after prose",
        STYLE_HASH,
        [
            "#!/usr/bin/env bash",
            "# matrix.sh -- long",
            "# more prose",
            f"# {SPDX_TEXT}",
            f"# {COPYRIGHT_TEXT}",
        ],
    ),
    (
        "hash preamble one line late",
        STYLE_HASH,
        ["#!/usr/bin/env bash", "#", f"# {SPDX_TEXT}", f"# {COPYRIGHT_TEXT}"],
    ),
    ("block reversed order", STYLE_BLOCK, ["/*", f" * {COPYRIGHT_TEXT}", f" * {SPDX_TEXT}", " */"]),
    (
        "block at tail of @file",
        STYLE_BLOCK,
        [
            "/**",
            " * @file x.c",
            " * @brief y",
            f" * @copyright {COPYRIGHT_TEXT}",
            f" * {SPDX_TEXT}",
            " */",
        ],
    ),
    ("block missing both", STYLE_BLOCK, ["/**", " * @file x.c", " */"]),
)


def _selftest_fix() -> list[str]:
    """Assert the fixer turns each must-fire shape canonical and is idempotent."""
    failures: list[str] = []
    for label, style, lines in MUST_FIRE:
        text = "\n".join(lines) + "\n"
        fixed = _rewrite(text, style)
        if fixed is None:
            failures.append(f"  fix: {label} was already canonical (unexpected)")
            continue
        if classify(fixed.splitlines(), style) is not None:
            failures.append(f"  fix: {label} still non-canonical after --fix:\n{fixed}")
            continue
        if fixed.count(SPDX_TEXT) != 1 or fixed.count(COPYRIGHT_TEXT) != 1:
            failures.append(f"  fix: {label} left a duplicate preamble line:\n{fixed}")
            continue
        if _rewrite(fixed, style) is not None:
            failures.append(f"  fix: {label} is not idempotent under --fix")
    return failures


def selftest() -> int:
    """Prove every wrong shape fires and the canonical shape stays silent."""
    failures = [
        f"  must-stay-quiet: {label} was rejected ({classify(lines, style)})"
        for label, style, lines in MUST_STAY_QUIET
        if classify(lines, style) is not None
    ]
    failures += [
        f"  must-fire: {label} was accepted as canonical"
        for label, style, lines in MUST_FIRE
        if classify(lines, style) is None
    ]
    failures += _selftest_fix()

    if failures:
        sys.stderr.write("check-copyright.py --selftest: FAILED\n\n")
        sys.stderr.write("\n".join(failures) + "\n")
        return EXIT_FAIL

    total = len(MUST_FIRE) + len(MUST_STAY_QUIET)
    print(
        f"check-copyright.py --selftest: OK ({total} cases: {len(MUST_FIRE)} must "
        f"fire, {len(MUST_STAY_QUIET)} must stay quiet; fixer canonicalises and is "
        "idempotent)."
    )
    return EXIT_OK


# ---------------------------------------------------------------------------
# Drivers.
# ---------------------------------------------------------------------------


def _run_all(fix: bool) -> int:
    """Check (or fix) every first-party file. The gate's entry point."""
    targets = enumerate_all()
    if len(targets) < FILE_FLOOR:
        sys.stderr.write(
            f"check-copyright.py: FATAL -- only {len(targets)} file(s) in scope, "
            f"floor is {FILE_FLOOR}. A collapsed scan reports a clean tree because "
            "it scanned nothing.\n"
        )
        return EXIT_CONFIG
    return _process(targets, fix)


def _run_files(paths: list[str], fix: bool) -> int:
    """Check (or fix) the files named on argv. The pre-commit entry point."""
    targets: list[tuple[str, str]] = []
    for raw in paths:
        path = Path(raw)
        rel = raw
        if path.is_absolute() and path.is_relative_to(REPO_ROOT):
            rel = str(path.relative_to(REPO_ROOT))
        if is_build_output_path(rel) or _is_generated(rel):
            continue
        style = _style_for(path)
        if style is None:
            continue
        targets.append((rel, style))
    return _process(targets, fix)


def _process(targets: list[tuple[str, str]], fix: bool) -> int:
    """Shared body: check every target, or fix it, and report."""
    if fix:
        return _fix_targets(targets)
    failures = []
    for rel, style in targets:
        reason = _check_one(rel, style)
        if reason is not None:
            failures.append((rel, reason))
    if not failures:
        print(f"check-copyright.py: {len(targets)} file(s) scanned, all headers canonical.")
        return EXIT_OK
    sys.stderr.write(f"check-copyright.py: {len(failures)} file(s) with a non-canonical header:\n")
    for rel, reason in sorted(failures):
        sys.stderr.write(f"  {rel}: {reason}\n")
    sys.stderr.write(
        "\nThe preamble must be, right after the shebang if any:\n"
        f"    # {SPDX_TEXT}\n    # {COPYRIGHT_TEXT}\n"
        "  (block-comment files use the /* * */ form at the very top.)\n"
        "  Run:  python3 scripts/checks/check-copyright.py --fix --all\n"
    )
    return EXIT_FAIL


def _fix_targets(targets: list[tuple[str, str]]) -> int:
    """Rewrite each non-canonical target in place."""
    changed = 0
    for rel, style in targets:
        path = REPO_ROOT / rel if not Path(rel).is_absolute() else Path(rel)
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        fixed = _rewrite(text, style)
        if fixed is not None and fixed != text:
            path.write_text(fixed, encoding="utf-8")
            print(f"  fixed {rel}")
            changed += 1
    print(f"check-copyright.py --fix: {changed} file(s) rewritten.")
    return EXIT_OK


def main(argv: list[str]) -> int:
    """Dispatch on the flags described in the module docstring."""
    args = argv[1:]
    if "--selftest" in args:
        return selftest()
    fix = "--fix" in args
    do_all = "--all" in args
    files = [a for a in args if not a.startswith("-")]
    if do_all:
        return _run_all(fix)
    if files:
        return _run_files(files, fix)
    sys.stderr.write(
        "usage: check-copyright.py FILE [FILE ...] | --all | --fix [...] | --selftest\n"
    )
    return EXIT_CONFIG


if __name__ == "__main__":
    sys.exit(main(sys.argv))
