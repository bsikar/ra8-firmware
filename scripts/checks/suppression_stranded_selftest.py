# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Both-direction fixtures for the stranded coverage-marker detector.

A `GCOVR_EXCL_BR_LINE` excludes only the branch on its own physical line, and
gcov attributes a decision to the line where the controlling expression starts.
So a marker pushed onto a continuation line by clang-format still reads as
correct in review while excluding nothing. These fixtures are what stop that
from recurring silently, which makes their own both-direction coverage
load-bearing rather than decorative.

Split out of `suppression_selftest.py` when that module reached the repository
1000-line file cap.
"""

from __future__ import annotations

from selftest_assert import expect
from suppression_inline_scan import stranded_branch_findings


def assert_stranded_branch_markers(failures: list[str]) -> None:
    """Assert a wrapped branch marker fires and an attached one stays quiet.

    Both directions plus the block-comment case: the backward walk must not
    mistake the interior of a multi-line ``/* ... */`` for an unfinished
    statement, which is how a correct marker would be reported as stranded.
    """
    wrapped = (
        "void f(void)\n"
        "{\n"
        "  ra8_err_t e = g();\n"
        "  RA8_RETURN_ON_ERROR(e,\n"
        "                      tag,\n"
        '                      "msg"); /* GCOVR_EXCL_BR_LINE -- hardware only */\n'
        "}\n"
    )
    attached = (
        "void f(void)\n"
        "{\n"
        "  ra8_err_t e = g();\n"
        '  RA8_RETURN_ON_ERROR(e, tag, "msg"); /* GCOVR_EXCL_BR_LINE -- hardware only */\n'
        "}\n"
    )
    after_comment = (
        "void f(void)\n"
        "{\n"
        "  /* a note that runs\n"
        "   * across several lines */\n"
        "  if (x) { /* GCOVR_EXCL_BR_LINE -- hardware only */\n"
        "    return;\n"
        "  }\n"
        "}\n"
    )
    expect(
        len(stranded_branch_findings("sample.c", wrapped, frozenset())) == 1,
        "must fire: a branch marker stranded on a wrapped call excludes nothing",
        failures,
    )
    expect(
        not stranded_branch_findings("sample.c", attached, frozenset()),
        "must stay quiet: a branch marker on its own statement line",
        failures,
    )
    expect(
        not stranded_branch_findings("sample.c", after_comment, frozenset({3, 4})),
        "must stay quiet: a marker following a multi-line block comment",
        failures,
    )
    assert_stranded_wrapping_shapes(failures)


#: One fixture per way a C statement can wrap, with the verdict the detector
#: must reach. ``True`` means the marker is stranded and the scan MUST fire;
#: ``False`` means the marker is attached and the scan MUST stay quiet. Every
#: wrapping shape seen in the tree is represented, because a shape with no
#: fixture is a shape the detector is free to stop recognizing (#790).
_WRAPPING_FIXTURES: tuple[tuple[str, bool, str], ...] = (
    (
        "wrapped-for-header",
        True,
        "void f(void)\n{\n"
        "  for (uint32_t i = 0U; i < limit;\n"
        "       i++) { /* GCOVR_EXCL_BR_LINE -- hardware only */\n"
        "    return;\n  }\n}\n",
    ),
    (
        "attached-for-header",
        False,
        "void f(void)\n{\n"
        "  for (uint32_t i = 0U; i < limit; i++) { /* GCOVR_EXCL_BR_LINE -- hardware only */\n"
        "    return;\n  }\n}\n",
    ),
    (
        "wrapped-if-condition",
        True,
        "void f(void)\n{\n"
        "  if ((a != 0) &&\n"
        "      (b != 0)) { /* GCOVR_EXCL_BR_LINE -- hardware only */\n"
        "    return;\n  }\n}\n",
    ),
    (
        "attached-if-condition",
        False,
        "void f(void)\n{\n"
        "  if ((a != 0) && (b != 0)) { /* GCOVR_EXCL_BR_LINE -- hardware only */\n"
        "    return;\n  }\n}\n",
    ),
    (
        "split-ternary",
        True,
        "void f(void)\n{\n"
        "  const uint32_t d = (ms > cap)\n"
        "                       ? cap\n"
        "                       : (uint32_t)ms; /* GCOVR_EXCL_BR_LINE -- hardware only */\n"
        "}\n",
    ),
    (
        "attached-ternary",
        False,
        "void f(void)\n{\n"
        "  const uint32_t d = (ms > cap) ? cap : ms; /* GCOVR_EXCL_BR_LINE -- hardware only */\n"
        "}\n",
    ),
    (
        "split-initializer-expression",
        True,
        "static const cfg_t c = {\n"
        "  .b = (x != 0)\n"
        "         ? 1\n"
        "         : 0, /* GCOVR_EXCL_BR_LINE -- hardware only */\n"
        "};\n",
    ),
    (
        "attached-initializer-expression",
        False,
        "static const cfg_t c = {\n"
        "  .b = (x != 0) ? 1 : 0, /* GCOVR_EXCL_BR_LINE -- hardware only */\n"
        "};\n",
    ),
    (
        "wrapped-macro-invocation",
        True,
        "void f(void)\n{\n"
        "  RA8_RETURN_ON_ERROR(e,\n"
        "                      tag,\n"
        '                      "msg"); /* GCOVR_EXCL_BR_LINE -- hardware only */\n'
        "}\n",
    ),
    (
        "attached-macro-invocation",
        False,
        "void f(void)\n{\n"
        '  RA8_RETURN_ON_ERROR(e, tag, "msg"); /* GCOVR_EXCL_BR_LINE -- hardware only */\n'
        "}\n",
    ),
    (
        "after-preprocessor-directive",
        False,
        "void f(void)\n{\n"
        "#endif\n"
        "  if (x) { /* GCOVR_EXCL_BR_LINE -- hardware only */\n"
        "    return;\n  }\n}\n",
    ),
    (
        "paren-in-string-literal",
        False,
        'void f(void)\n{\n  log(tag, "a) b"); /* GCOVR_EXCL_BR_LINE -- hardware only */\n}\n',
    ),
)


def assert_stranded_wrapping_shapes(failures: list[str]) -> None:
    """Drive one fixture per wrapping shape through the production scan.

    Args:
        failures: Accumulator every ``expect`` appends its message to.
    """
    for name, must_fire, text in _WRAPPING_FIXTURES:
        found = stranded_branch_findings("sample.c", text, frozenset())
        if must_fire:
            expect(
                len(found) == 1,
                f"must fire: {name} strands its branch marker",
                failures,
            )
        else:
            expect(
                not found,
                f"must stay quiet: {name} keeps its branch marker attached",
                failures,
            )
    # A multi-line block comment between the statement and the marker must not
    # be read as an unfinished statement -- the interior lines are prose.
    across_comment = (
        "void f(void)\n"
        "{\n"
        "  g();\n"
        "  /* a note that runs\n"
        "   * across several lines */\n"
        "  if (x) { /* GCOVR_EXCL_BR_LINE -- hardware only */\n"
        "    return;\n"
        "  }\n"
        "}\n"
    )
    expect(
        not stranded_branch_findings("sample.c", across_comment, frozenset({4, 5})),
        "must stay quiet: a marker separated from its statement by block-comment prose",
        failures,
    )


def assert_stranded_line_markers(failures: list[str]) -> None:
    """Assert a detached line marker fires and an attached one stays quiet.

    ``GCOVR_EXCL_LINE`` excludes only the physical line it sits on, so a
    marker pushed onto a comment-only line excludes a line gcov never
    counted, while the statement it was written for stays in the measured
    debt. That failure is invisible in review: the comment still sits
    beside the code it describes.

    Args:
        failures: Accumulator every ``expect`` appends its message to.
    """
    orphaned_line = (
        "void f(void)\n"
        "{\n"
        "  return;\n"
        "  /* explanation */ /* GCOVR_EXCL_LINE -- host cannot reach this */\n"
        "}\n"
    )
    attached_line = (
        "void f(void)\n{\n  return; /* GCOVR_EXCL_LINE -- host cannot reach this */\n}\n"
    )
    expect(
        len(stranded_branch_findings("sample.c", orphaned_line, frozenset())) == 1,
        "must fire: a line marker on a comment-only line excludes no counted line",
        failures,
    )
    expect(
        not stranded_branch_findings("sample.c", attached_line, frozenset()),
        "must stay quiet: a line marker on the statement it excludes",
        failures,
    )
