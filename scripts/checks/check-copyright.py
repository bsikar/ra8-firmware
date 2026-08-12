#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: one canonical copyright + SPDX attribution, same place in every file.

Two forms, because this tree has two comment conventions and the attribution
belongs where each convention already puts its metadata.

C family (a Doxygen ``@file`` block)
------------------------------------
The attribution lives INSIDE the file-header block, as its closing tag group,
never in a separate comment above it::

    /**
     * @file foo.c
     * @brief ...
     * @details ...
     *
     * @author Brighton Sikarskie          <- kept when present
     * @date 2026-04-29                    <- kept when present
     * @copyright Copyright (c) 2026 Brighton Sikarskie
     * SPDX-License-Identifier: MIT
     * @since 0.1.0                        <- kept when present
     */

``@copyright`` immediately followed by the SPDX line is the invariant this
gate enforces, placed as the closing group of the block (before ``@since``
when the file carries one). ``@author`` / ``@date`` / ``@since`` are PRESERVED
exactly as written and are never invented: 2190 of the 2297 C-family files
have never carried ``@author`` or ``@date``, and manufacturing them would be
fabricated provenance, not a header standard.

A standalone ``/* SPDX ... */`` block above the ``@file`` block is a
violation, and ``--fix`` merges it back into the block rather than leaving the
attribution split across two comments.

Linker scripts use a plain (non-Doxygen) leading block, so they carry the bare
``Copyright`` line followed by the SPDX line at the end of that block.

Hash-comment files (shell, python, cmake, make, yaml)
-----------------------------------------------------
No doc-comment convention to live inside, so the attribution leads the file,
immediately after the shebang when there is one::

    #!/usr/bin/env bash
    # SPDX-License-Identifier: MIT
    # Copyright (c) 2026 Brighton Sikarskie

Scope is derived from ``lint_targets`` (``git ls-files``), so a new top-level
directory is covered the day it lands. Vendored SOUP, generated tables and
build output are out of scope; ``.md`` is documentation, not code.

Run::

    check-copyright.py FILE [FILE ...]   # named files (pre-commit hook)
    check-copyright.py --all             # every first-party file (gate)
    check-copyright.py --fix --all       # rewrite headers to canonical form
    check-copyright.py --selftest        # prove the rules fire and stay quiet

Exit 0 clean, 1 on a violation or failing selftest, 2 on a collapsed scan.
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

COPY_TEXT = "Copyright (c) 2026 Brighton Sikarskie"
COPY_TAG = f"@copyright {COPY_TEXT}"
SPDX_TEXT = "SPDX-License-Identifier: MIT"

STYLE_HASH = "hash"
STYLE_DOXY = "doxy"  # C family: attribution inside the @file block
STYLE_PLAIN = "plain"  # linker scripts: bare lines in the leading block

LANG_STYLE = {
    "c": STYLE_DOXY,
    "ld": STYLE_PLAIN,
    "shell": STYLE_HASH,
    "python": STYLE_HASH,
    "cmake": STYLE_HASH,
    "make": STYLE_HASH,
    "yaml": STYLE_HASH,
}
ENFORCED_LANGS = tuple(LANG_STYLE)

_SUFFIX_STYLE = {
    ".c": STYLE_DOXY,
    ".h": STYLE_DOXY,
    ".cpp": STYLE_DOXY,
    ".hpp": STYLE_DOXY,
    ".cc": STYLE_DOXY,
    ".cxx": STYLE_DOXY,
    ".hh": STYLE_DOXY,
    ".hxx": STYLE_DOXY,
    ".ld": STYLE_PLAIN,
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

_GENERATED = ("font_fixture.h", "fixture_ahem.h", "epub_fixture.h")

FILE_FLOOR = 1500


# ---------------------------------------------------------------------------
# Comment-frame helpers
# ---------------------------------------------------------------------------


def _hashless(line: str) -> str:
    return line.lstrip().removeprefix("#").strip()


def _starless(line: str) -> str:
    body = line.strip()
    for lead in ("/**", "/*", "*/", "*"):
        if body.startswith(lead):
            body = body[len(lead) :]
            break
    return body.removesuffix("*/").strip()


def _block_span(lines: list[str], *, doxygen: bool) -> tuple[int, int] | None:
    """Span of the file-header comment block, or None.

    ``doxygen`` selects the first ``/**`` block -- the ``@file`` header -- and
    is what the C family uses. A one-line ``/* SPDX ... */`` above that block
    is NOT the header: treating it as one is what once inserted the tag pair
    outside any comment and left dangling ``* @copyright`` lines at file
    scope. Linker scripts have no Doxygen block, so they take the first
    MULTI-line ``/*`` block instead, single-line comments skipped for the same
    reason.
    """
    start = None
    for i, ln in enumerate(lines):
        stripped = ln.strip()
        if start is None:
            if doxygen:
                if stripped.startswith("/**"):
                    start = i
                    if "*/" in stripped[3:]:
                        return start, i
                continue
            if stripped.startswith("/*"):
                if "*/" in stripped[2:]:
                    continue  # a one-line comment is not the header block
                start = i
            continue
        if "*/" in ln:
            return start, i
    return None


#: Tag prefixes that belong to the attribution group, alongside the copyright
#: and SPDX lines themselves.
_GROUP_PREFIXES = ("@author", "@date", "@since")


def _group_indices(body: list[str], want_copy: str) -> list[int]:
    """Indices of the attribution tags present in a header block.

    Args:
        body: Comment-stripped lines of the file-header block.
        want_copy: The copyright spelling this comment style expects.

    Returns:
        Sorted indices of every attribution tag found, possibly empty.
    """
    return [
        i
        for i, b in enumerate(body)
        if b.startswith(_GROUP_PREFIXES) or b in {want_copy, COPY_TEXT, COPY_TAG, SPDX_TEXT}
    ]


def _group_blank_split(body: list[str], want_copy: str) -> int | None:
    """Index of the first blank line splitting the attribution group, or None.

    The group runs from the first attribution tag through the last, and must
    read as one unbroken stanza. A blank line BEFORE the group -- the one that
    separates it from the ``@details`` prose above -- is deliberately allowed;
    only a break INSIDE it is a finding.

    Args:
        body: Comment-stripped lines of the file-header block.
        want_copy: The copyright spelling this comment style expects.

    Returns:
        The offending index, or None when the group is contiguous.
    """
    idxs = _group_indices(body, want_copy)
    if not idxs:
        return None
    return next((i for i in range(idxs[0], idxs[-1]) if not body[i]), None)


def _attribution_outside(lines: list[str], span: tuple[int, int]) -> list[int]:
    """Indices of attribution lines living OUTSIDE the header block.

    Covers both rejected shapes: the standalone ``/* SPDX ... */`` block V1
    added above the ``@file`` block, and the pair of one-line ``/* ... */``
    comments some headers carried above it.
    """
    start, end = span
    out = []
    for i, ln in enumerate(lines):
        if start <= i <= end:
            continue
        body = _starless(ln)
        if body in {SPDX_TEXT, COPY_TEXT, COPY_TAG}:
            out.append(i)
    return out


# ---------------------------------------------------------------------------
# Classification
# ---------------------------------------------------------------------------


def classify(lines: list[str], style: str) -> str | None:
    """Judge a file's header, returning a violation reason or None."""
    stripped = [ln.rstrip("\r\n") for ln in lines]
    if style == STYLE_HASH:
        return _classify_hash(stripped)
    return _classify_block(stripped, style)


def _classify_hash(lines: list[str]) -> str | None:
    idx = 1 if lines and lines[0].startswith("#!") else 0
    want = [f"# {SPDX_TEXT}", f"# {COPY_TEXT}"]
    if lines[idx : idx + 2] == want:
        return None
    joined = "\n".join(lines[:60])
    if SPDX_TEXT not in joined and COPY_TEXT not in joined:
        return "missing the SPDX + copyright preamble entirely"
    if SPDX_TEXT not in joined:
        return "missing the SPDX-License-Identifier line"
    if COPY_TEXT not in joined:
        return "missing the copyright line"
    return (
        f"preamble is not the canonical leading pair (expected '# {SPDX_TEXT}' "
        f"then '# {COPY_TEXT}' at line {idx + 1})"
    )


def _classify_block(lines: list[str], style: str) -> str | None:
    doxygen = style == STYLE_DOXY
    span = _block_span(lines, doxygen=doxygen)
    if span is None:
        return (
            "no @file Doxygen block to carry the attribution"
            if doxygen
            else "no leading comment block to carry the attribution"
        )
    if _attribution_outside(lines, span):
        return (
            "attribution sits in a comment outside the file-header block; it "
            "belongs INSIDE that block as its closing tag group"
        )
    start, end = span
    want_copy = COPY_TAG if doxygen else COPY_TEXT
    return _classify_group([_starless(x) for x in lines[start : end + 1]], want_copy, doxygen)


def _classify_group(body: list[str], want_copy: str, doxygen: bool) -> str | None:
    """Judge the attribution tag group inside a file-header block.

    Args:
        body: Comment-stripped lines of the block.
        want_copy: The copyright spelling this comment style expects.
        doxygen: Whether the block is a Doxygen ``@file`` header.

    Returns:
        A short reason when the group is wrong, else None.
    """
    ci = si = None
    for i, b in enumerate(body):
        if b == want_copy:
            ci = i
        elif b == SPDX_TEXT:
            si = i
    if ci is None and si is None:
        return "the file-header block carries no copyright or SPDX line"
    if ci is None:
        kind = "@copyright" if doxygen else "copyright"
        return f"the file-header block has no {kind} line"
    if si is None:
        return "the file-header block has no SPDX-License-Identifier line"
    if si != ci + 1:
        return (
            f"copyright and SPDX are not adjacent in the file-header block "
            f"(copyright at block line {ci + 1}, SPDX at {si + 1})"
        )
    blank = _group_blank_split(body, want_copy)
    if blank is not None:
        return (
            f"a blank line at block line {blank + 1} splits the attribution "
            "group; @author / @date / @copyright / SPDX / @since must be "
            "consecutive lines"
        )
    return None


# ---------------------------------------------------------------------------
# Fixer
# ---------------------------------------------------------------------------


def _rewrite(text: str, style: str) -> str | None:
    lines = text.splitlines()
    if classify(lines, style) is None:
        return None
    fixed = _rewrite_hash(lines) if style == STYLE_HASH else _rewrite_block(lines, style)
    if fixed is None:
        return None
    trailing = "\n" if text.endswith("\n") else ""
    return "\n".join(fixed) + trailing


def _rewrite_hash(lines: list[str]) -> list[str]:
    shebang = None
    body = lines
    if lines and lines[0].startswith("#!"):
        shebang, body = lines[0], lines[1:]
    kept: list[str] = []
    in_region = True
    for line in body:
        s = line.strip()
        if in_region and (s == "" or s.startswith("#")):
            if s.startswith("#") and _hashless(line) in {SPDX_TEXT, COPY_TEXT}:
                continue
            kept.append(line)
            continue
        in_region = False
        kept.append(line)
    head = [shebang] if shebang else []
    return [*head, f"# {SPDX_TEXT}", f"# {COPY_TEXT}", *kept]


def _rewrite_block(lines: list[str], style: str) -> list[str] | None:
    """Merge the attribution INTO the file-header block, in canonical order."""
    doxygen = style == STYLE_DOXY
    span = _block_span(lines, doxygen=doxygen)
    if span is None:
        return None
    want_copy = COPY_TAG if doxygen else COPY_TEXT
    # Drop every attribution line, wherever it lives -- inside the block, or
    # in the standalone/one-line comments above it -- then re-insert the pair
    # inside the block. That repairs the split, reversed and outside-the-block
    # shapes alike, and keeps the fixer idempotent. Blank comment lines left
    # stranded by removing an outside comment are dropped with it.
    drop = set(_attribution_outside(lines, span))
    start, end = span
    for i in range(start + 1, end):
        if _starless(lines[i]) in {want_copy, COPY_TEXT, COPY_TAG, SPDX_TEXT}:
            drop.add(i)
    kept = [ln for i, ln in enumerate(lines) if i not in drop]
    span = _block_span(kept, doxygen=doxygen)
    if span is None:
        return None
    start, end = span
    pair = [f" * {want_copy}", f" * {SPDX_TEXT}"]
    # Insert before @since when the block has one, else just before the close.
    insert_at = end
    for i in range(start + 1, end):
        if _starless(kept[i]).startswith("@since"):
            insert_at = i
            break
    merged = kept[:insert_at] + pair + kept[insert_at:]
    return _close_group_gaps(merged, want_copy, doxygen)


def _close_group_gaps(lines: list[str], want_copy: str, doxygen: bool) -> list[str]:
    """Delete blank comment lines that split the attribution group.

    The group must read as one unbroken stanza. Removing the pair and
    re-inserting it can strand the blank line that used to sit between the
    pair and ``@since``, so this runs as the last step of every repair.

    Args:
        lines: The file's lines after the pair has been placed.
        want_copy: The copyright spelling this comment style expects.
        doxygen: Whether the header block is a Doxygen ``@file`` block.

    Returns:
        The lines with any intra-group blank comment lines removed.
    """
    span = _block_span(lines, doxygen=doxygen)
    if span is None:
        return lines
    start, end = span
    body = [_starless(x) for x in lines[start : end + 1]]
    idxs = _group_indices(body, want_copy)
    if not idxs:
        return lines
    drop = {start + i for i in range(idxs[0], idxs[-1]) if not body[i]}
    # Collapse a run of blank comment lines directly above the group to a
    # single separator. Lifting the old pair out strands the blank that used
    # to sit above it, which would otherwise leave two ` *` lines stacked.
    above = []
    cursor = idxs[0] - 1
    while cursor > 0 and not body[cursor]:
        above.append(cursor)
        cursor -= 1
    drop.update(start + i for i in above[1:])
    return [ln for i, ln in enumerate(lines) if i not in drop]


# ---------------------------------------------------------------------------
# Scope
# ---------------------------------------------------------------------------


def _style_for(path: Path) -> str | None:
    if path.name in _BASENAME_STYLE:
        return _BASENAME_STYLE[path.name]
    return _SUFFIX_STYLE.get(path.suffix.lower())


def _is_generated(rel: str) -> bool:
    return rel.endswith(_GENERATED)


def enumerate_all() -> list[tuple[str, str]]:
    """Every first-party file this gate judges, as ``(path, style)`` pairs."""
    out: list[tuple[str, str]] = []
    for lang, rels in files_for(ENFORCED_LANGS).items():
        style = LANG_STYLE[lang]
        out.extend((rel, style) for rel in rels if not _is_generated(rel))
    return sorted(out)


def _check_one(rel: str, style: str) -> str | None:
    path = REPO_ROOT / rel if not Path(rel).is_absolute() else Path(rel)
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError as exc:
        return f"unreadable: {exc}"
    return classify(lines, style)


# ---------------------------------------------------------------------------
# Selftest -- both directions, in-memory only.
# ---------------------------------------------------------------------------

_GOOD_DOXY = [
    "/**",
    " * @file x.c",
    " * @brief y",
    " *",
    " * @author Brighton Sikarskie",
    " * @date 2026-04-29",
    f" * {COPY_TAG}",
    f" * {SPDX_TEXT}",
    " * @since 0.1.0",
    " */",
]
_GOOD_DOXY_MINIMAL = ["/**", " * @file x.c", f" * {COPY_TAG}", f" * {SPDX_TEXT}", " */"]
_GOOD_PLAIN = ["/*", " * linker script", f" * {COPY_TEXT}", f" * {SPDX_TEXT}", " */"]
_GOOD_HASH = ["#!/usr/bin/env bash", f"# {SPDX_TEXT}", f"# {COPY_TEXT}", "#", "# body"]
_GOOD_HASH_NOSB = [f"# {SPDX_TEXT}", f"# {COPY_TEXT}", "", "x = 1"]

MUST_STAY_QUIET: tuple[tuple[str, str, list[str]], ...] = (
    ("doxy full tag group", STYLE_DOXY, _GOOD_DOXY),
    ("doxy minimal pair", STYLE_DOXY, _GOOD_DOXY_MINIMAL),
    ("plain linker block", STYLE_PLAIN, _GOOD_PLAIN),
    ("hash with shebang", STYLE_HASH, _GOOD_HASH),
    ("hash without shebang", STYLE_HASH, _GOOD_HASH_NOSB),
)

MUST_FIRE: tuple[tuple[str, str, list[str]], ...] = (
    (
        "V1 standalone block above @file",
        STYLE_DOXY,
        ["/*", f" * {SPDX_TEXT}", f" * {COPY_TEXT}", " */", "/**", " * @file x.c", " */"],
    ),
    (
        # The shape that once broke 12 files: a one-line /* ... */ comment
        # above the block is NOT the header block. Treating it as one put the
        # tag pair outside any comment, leaving ` * @copyright` at file scope.
        "one-line attribution comments above @file",
        STYLE_DOXY,
        [
            f"/* {SPDX_TEXT} */",
            f"/* {COPY_TEXT} */",
            "/**",
            " * @file x.c",
            " */",
        ],
    ),
    (
        "doxy reversed pair",
        STYLE_DOXY,
        ["/**", " * @file x.c", f" * {SPDX_TEXT}", f" * {COPY_TAG}", " */"],
    ),
    (
        "doxy split pair",
        STYLE_DOXY,
        ["/**", " * @file x.c", f" * {COPY_TAG}", " * @since 0.1.0", f" * {SPDX_TEXT}", " */"],
    ),
    (
        # A blank comment line inside the group: the pair, then ` *`, then
        # @since. The tags are individually right and the stanza is still torn.
        "blank line between the pair and @since",
        STYLE_DOXY,
        [
            "/**",
            " * @file x.c",
            f" * {COPY_TAG}",
            f" * {SPDX_TEXT}",
            " *",
            " * @since 0.1.0",
            " */",
        ],
    ),
    (
        "blank line between @date and the pair",
        STYLE_DOXY,
        [
            "/**",
            " * @file x.c",
            " * @author Brighton Sikarskie",
            " * @date 2026-04-29",
            " *",
            f" * {COPY_TAG}",
            f" * {SPDX_TEXT}",
            " * @since 0.1.0",
            " */",
        ],
    ),
    ("doxy missing spdx", STYLE_DOXY, ["/**", " * @file x.c", f" * {COPY_TAG}", " */"]),
    ("doxy missing both", STYLE_DOXY, ["/**", " * @file x.c", " * @brief y", " */"]),
    ("plain missing spdx", STYLE_PLAIN, ["/*", " * ld", f" * {COPY_TEXT}", " */"]),
    (
        "hash reversed",
        STYLE_HASH,
        ["#!/usr/bin/env bash", f"# {COPY_TEXT}", f"# {SPDX_TEXT}"],
    ),
    (
        "hash buried",
        STYLE_HASH,
        ["#!/usr/bin/env bash", "# prose", f"# {SPDX_TEXT}", f"# {COPY_TEXT}"],
    ),
    ("hash missing both", STYLE_HASH, ["#!/usr/bin/env bash", "# just prose", "echo hi"]),
)


def _selftest_fix() -> list[str]:
    out: list[str] = []
    for label, style, lines in MUST_FIRE:
        text = "\n".join(lines) + "\n"
        fixed = _rewrite(text, style)
        if fixed is None:
            out.append(f"  fix: {label} was already canonical (unexpected)")
            continue
        if classify(fixed.splitlines(), style) is not None:
            out.append(f"  fix: {label} still non-canonical:\n{fixed}")
            continue
        if fixed.count(SPDX_TEXT) != 1 or fixed.count(COPY_TEXT) != 1:
            out.append(f"  fix: {label} left a duplicate:\n{fixed}")
            continue
        if _rewrite(fixed, style) is not None:
            out.append(f"  fix: {label} is not idempotent")
    # Preservation: @author/@date/@since must survive a fix untouched.
    src = "\n".join(
        [
            "/**",
            " * @file x.c",
            " * @author Someone Else",
            " * @date 2020-01-02",
            f" * {SPDX_TEXT}",
            f" * {COPY_TAG}",
            " * @since 0.1.0",
            " */",
        ]
    )
    fixed = _rewrite(src + "\n", STYLE_DOXY) or ""
    out.extend(
        f"  fix: dropped '{needle}' -- provenance must be preserved"
        for needle in ("@author Someone Else", "@date 2020-01-02", "@since 0.1.0")
        if needle not in fixed
    )
    if f"{COPY_TAG}\n * {SPDX_TEXT}" not in fixed:
        out.append(f"  fix: pair not adjacent in order after repair:\n{fixed}")
    return out


def selftest() -> int:
    """Prove every wrong shape fires and every canonical shape stays silent."""
    failures = [
        f"  must-stay-quiet: {label} rejected ({classify(lines, style)})"
        for label, style, lines in MUST_STAY_QUIET
        if classify(lines, style) is not None
    ]
    failures += [
        f"  must-fire: {label} accepted as canonical"
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
        f"check-copyright.py --selftest: OK ({total} cases: {len(MUST_FIRE)} must fire, "
        f"{len(MUST_STAY_QUIET)} must stay quiet; fixer merges, preserves "
        "@author/@date/@since, and is idempotent)."
    )
    return EXIT_OK


# ---------------------------------------------------------------------------
# Drivers
# ---------------------------------------------------------------------------


def _process(targets: list[tuple[str, str]], fix: bool) -> int:
    if fix:
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
                changed += 1
        print(f"check-copyright.py --fix: {changed} file(s) rewritten.")
        return EXIT_OK
    failures = [(rel, r) for rel, style in targets if (r := _check_one(rel, style)) is not None]
    if not failures:
        print(f"check-copyright.py: {len(targets)} file(s) scanned, all headers canonical.")
        return EXIT_OK
    sys.stderr.write(f"check-copyright.py: {len(failures)} file(s) with a non-canonical header:\n")
    for rel, reason in sorted(failures):
        sys.stderr.write(f"  {rel}: {reason}\n")
    sys.stderr.write(
        "\nC family: the attribution lives INSIDE the @file block as its closing\n"
        f"group -- '{COPY_TAG}' then '{SPDX_TEXT}'.\n"
        f"Hash files: '# {SPDX_TEXT}' then '# {COPY_TEXT}' after any shebang.\n"
        "  Run:  python3 scripts/checks/check-copyright.py --fix --all\n"
    )
    return EXIT_FAIL


def main(argv: list[str]) -> int:
    """Dispatch on the flags described in the module docstring."""
    args = argv[1:]
    if "--selftest" in args:
        return selftest()
    fix = "--fix" in args
    if "--all" in args:
        targets = enumerate_all()
        if len(targets) < FILE_FLOOR:
            sys.stderr.write(
                f"check-copyright.py: FATAL -- only {len(targets)} file(s) in scope, "
                f"floor is {FILE_FLOOR}. A collapsed scan reports a clean tree "
                "because it scanned nothing.\n"
            )
            return EXIT_CONFIG
        return _process(targets, fix)
    files = [a for a in args if not a.startswith("-")]
    if not files:
        sys.stderr.write("usage: check-copyright.py FILE ... | --all | --fix --all | --selftest\n")
        return EXIT_CONFIG
    targets = []
    for raw in files:
        path = Path(raw)
        rel = raw
        if path.is_absolute() and path.is_relative_to(REPO_ROOT):
            rel = str(path.relative_to(REPO_ROOT))
        if is_build_output_path(rel) or _is_generated(rel):
            continue
        style = _style_for(path)
        if style is not None:
            targets.append((rel, style))
    return _process(targets, fix)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
