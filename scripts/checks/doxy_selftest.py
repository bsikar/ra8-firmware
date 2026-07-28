# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Both-direction regression tests for the auditor, for both enforcing modes.

Run before the real check in the gate, and for the reason this repository keeps
rediscovering: a parser-driven gate that stops recognising a construct reports
nothing and looks exactly like a documented tree.  Every fixture below asserts
that a real gap FIRES and that a correctly documented form of the same
construct stays SILENT, so the auditor cannot be "fixed" by blinding it.
"""

from __future__ import annotations

import sys
import tempfile
from pathlib import Path

from doxy_functions import audit_file
from doxy_members import audit_members_file
from doxy_scope import override_repo_root
from doxy_style import _floor_failure, audit_text, partition

# --------------------------------------------------------------------------
# Self-test
# --------------------------------------------------------------------------
#: Synthetic fixtures for run_selftest(). Both enforcing modes get both
#: directions: every defect class must be reported, and every legal-but-tricky
#: form must stay clean. A documentation gate that silently stops reporting
#: looks exactly like a fully documented tree, which is the failure mode this
#: repo keeps rediscovering.
_SELFTEST_SOURCES: dict[str, str] = {
    # --- function mode: the shapes that MUST be reported ------------------
    "libs/mod_doc/src/bad.c": """
static int bad_no_block(int a)
{
  return a;
}

/**
 * @brief Adds two numbers.
 * @details Returns the arithmetic sum of its two arguments.
 * @return The sum.
 * @retval 0 Both arguments were zero.
 */
static int bad_thin_block(int a, int b)
{
  return a + b;
}
""",
    "libs/mod_doc/inc/mod_doc.h": """
#pragma once
int hdr_undocumented(int a);
""",
    # --- function mode: the shapes that must stay CLEAN --------------------
    "libs/mod_doc/src/good.c": """
#include "mod_doc.h"

/**
 * @brief Doubles a value.
 * @details Multiplies the input by two and returns it, saturating nothing.
 * @param[in] a Value to double.
 * @return The doubled value.
 * @retval 0 The input was zero.
 * @pre @p a is initialised.
 * @pre The caller is single-threaded.
 * @post No global state is mutated.
 * @post The result is exactly 2*a.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
static int good_full_block(int a)
{
  return a * 2;
}

/* A static forward prototype: an ordering device, not a second contract.
   Demanding a block here too would force the identical block to be written
   twice, which check_doc_attachment.py rejects as a duplicate. */
static int good_fwd_declared(int a);

/**
 * @brief Triples a value.
 * @details Multiplies the input by three and returns it.
 * @param[in] a Value to triple.
 * @return The tripled value.
 * @retval 0 The input was zero.
 * @pre @p a is initialised.
 * @pre The caller is single-threaded.
 * @post No global state is mutated.
 * @post The result is exactly 3*a.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
static int good_fwd_declared(int a)
{
  return a * 3;
}

/* Definition-site policy: a non-static definition in a .c carries no block --
   the header owns the contract. */
int hdr_undocumented(int a)
{
  int total = 0;
  /* `else if (...)` is the shape NON_FUNC_NAMES exists for: the regex sees
     `else` as a return-type token and `if` as the function name. Same for
     `__asm__ volatile("...")`, which parses as a function named `volatile`.
     A bare `if (...)` would NOT exercise this -- it has no type token in
     front of it, so FUNC_RE never matches it in the first place. */
  if (a > 0)
  {
    total = good_full_block(a);
  }
  else if (a < 0)
  {
    total = 0 - a;
  }
  while (total > 100) {
    total = total - 1;
  }
  __asm__ volatile("nop");
  return total + good_fwd_declared(a);
}
""",
    # --- member mode: both directions in one file -------------------------
    "libs/mod_doc/inc/members.h": """
#pragma once

/** @brief A documented macro. */
#define MOD_DOC_GOOD_MACRO 1

#define MOD_DOC_BAD_MACRO 2

/**
 * @enum mod_doc_state_t
 * @brief States.
 * @details The states this fixture can be in.
 */
typedef enum : unsigned char {
  k_mod_doc_good = 0, /**< A documented enum value. */
  k_mod_doc_bad = 1,
} mod_doc_state_t;

/**
 * @struct mod_doc_cfg_t
 * @brief Config.
 * @details The configuration this fixture accepts.
 */
typedef struct {
  int good_member; /**< A documented struct member. */
  int bad_member;
} mod_doc_cfg_t;
""",
}

#: Function-mode offenders the gate must report, by function name.
_SELFTEST_FUNC_EXPECTED = frozenset({"bad_no_block", "bad_thin_block", "hdr_undocumented"})

#: Function-mode definitions the gate must leave alone. ``hdr_undocumented``
#: is deliberately in both sets: it is a gap at its *header prototype* and
#: clean at its *definition* in good.c, which is exactly the definition-site
#: policy. The assertions below therefore key on (file, name), not name.
_SELFTEST_FUNC_CLEAN = frozenset({"good_full_block", "good_fwd_declared", "hdr_undocumented"})

#: Member-mode offenders the gate must report, as (kind, name).
_SELFTEST_MEMBER_EXPECTED = frozenset(
    {("macro", "MOD_DOC_BAD_MACRO"), ("enum", "k_mod_doc_bad"), ("struct", "bad_member")}
)

#: Member-mode names that are documented and must never be reported.
_SELFTEST_MEMBER_CLEAN = frozenset({"MOD_DOC_GOOD_MACRO", "k_mod_doc_good", "good_member"})


def _audit_synthetic(root: Path) -> tuple[list, list]:
    """Write the fixture tree under ``root`` and audit it in both modes."""
    for rel, body in _SELFTEST_SOURCES.items():
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(body, encoding="ascii")
    func_rows, member_rows = [], []
    for rel in _SELFTEST_SOURCES:
        path = root / rel
        func_rows.extend(audit_file(path))
        member_rows.extend(audit_members_file(path))
    return func_rows, member_rows


def _check_function_mode(func_rows: list) -> list[str]:
    """The function gate must report every gap and spare every legal form.

    The legal-but-tricky forms are the load-bearing half. A `.c` definition of
    a function the header already documents must stay clean (CLAUDE.md,
    "Definition-site comments"), a static forward prototype whose definition
    carries the block must stay clean (otherwise this gate and
    check_doc_attachment.py contradict each other and no file can satisfy
    both), and `if` / `while` / `switch` must never be mistaken for
    declarations.
    """
    gaps = {(r[0], r[2]) for r in func_rows if r[3]}
    gap_names = {name for _f, name in gaps}
    failures = [
        f"function gate went toothless: '{name}' is undocumented and was not reported"
        for name in sorted(_SELFTEST_FUNC_EXPECTED - gap_names)
    ]
    failures.extend(
        f"function gate false positive: '{name}' in {path} is a legal form "
        f"(fully documented, forward prototype, or a definition whose header "
        f"owns the contract) but was reported"
        for path, name in sorted(gaps)
        if name in _SELFTEST_FUNC_CLEAN and path.endswith("good.c")
    )
    parsed = {r[2] for r in func_rows}
    failures.extend(
        f"selftest fixture did not parse: '{name}' never reached the auditor, "
        f"so its shape was never exercised"
        for name in sorted(_SELFTEST_FUNC_EXPECTED | _SELFTEST_FUNC_CLEAN)
        if name not in parsed
    )
    # Keywords are not declarations. `else if (...)` and `__asm__ volatile(...)`
    # both match FUNC_RE structurally -- the keyword in front reads as a return
    # type -- and are rejected only by NON_FUNC_NAMES. If that filter erodes,
    # the gate buries real findings under noise until somebody switches it off.
    failures.extend(
        f"function gate false positive: keyword '{bogus}' was parsed as a "
        f"function declaration (NON_FUNC_NAMES no longer filters it)"
        for bogus in ("if", "volatile")
        if bogus in parsed
    )
    return failures


def _check_member_mode(member_rows: list) -> list[str]:
    """The member gate must report undocumented members and spare documented ones."""
    member_hits = {(r[2], r[3]) for r in member_rows}
    member_names = {name for _k, name in member_hits}
    failures = [
        f"member gate went toothless: undocumented {kind} '{name}' was not reported"
        for kind, name in sorted(_SELFTEST_MEMBER_EXPECTED)
        if name not in member_names
    ]
    failures.extend(
        f"member gate false positive: '{name}' carries a doc comment but was reported"
        for name in sorted(_SELFTEST_MEMBER_CLEAN & member_names)
    )
    return failures


#: Style-mode fixtures: (path, source, the rule codes the gate MUST report).
#: An empty expectation is the other direction -- a legal form the gate must
#: leave alone. Every rule appears in both directions, because a tag gate that
#: silently stopped matching looks exactly like a compliant tree.
_STYLE_FIXTURES: tuple[tuple[str, str, frozenset[str]], ...] = (
    (
        "libs/mod_sty/src/no_block.c",
        "/* not a doxygen block */\nint f(void) { return 0; }\n",
        frozenset({"FILE_BLOCK_MISSING"}),
    ),
    (
        "libs/mod_sty/src/wrong_name.c",
        "/**\n * @file some_other_file.c\n * @brief B.\n * @details D.\n */\n",
        frozenset({"FILE_TAG_MISMATCH"}),
    ),
    (
        "libs/mod_sty/src/no_brief.c",
        "/**\n * @file no_brief.c\n * @details D.\n */\n",
        frozenset({"BRIEF_MISSING"}),
    ),
    (
        "libs/mod_sty/src/no_details.c",
        "/**\n * @file no_details.c\n * @brief B.\n */\n",
        frozenset({"DETAILS_MISSING"}),
    ),
    (
        "libs/mod_sty/src/bare_param.c",
        "/**\n * @file bare_param.c\n * @brief B.\n * @details D.\n */\n"
        "/** @brief g. @param a The input. */\n",
        frozenset({"PARAM_NO_DIRECTION"}),
    ),
    (
        "libs/mod_sty/src/bad_dir.c",
        "/**\n * @file bad_dir.c\n * @brief B.\n * @details D.\n */\n"
        "/** @brief g. @param[inout] a The input. */\n",
        frozenset({"PARAM_BAD_DIRECTION"}),
    ),
    # --- the forms that must stay CLEAN -----------------------------------
    (
        "libs/mod_sty/src/good.c",
        "/**\n * @file good.c\n * @brief B.\n * @details D.\n */\n"
        "/** @brief g. @param[in] a In. @param[out] b Out. @param[in,out] c Both. */\n",
        frozenset(),
    ),
    (
        # Doxygen's other command prefix. port/mbedtls carries upstream's
        # backslash form, so a rule keyed on '@' alone exempts it silently.
        "libs/mod_sty/src/backslash.c",
        "/**\n * \\file backslash.c\n * \\brief B.\n * \\details D.\n */\n"
        "/** \\brief g. \\param[in] a In. */\n",
        frozenset(),
    ),
    (
        # \file's argument is same-line-only; 17 headers here push the name to
        # the next line, which doxygen reads as "this file". Not a mismatch.
        "libs/mod_sty/src/continued.c",
        "/**\n * @file\n * libs/mod_sty/src/continued.c\n * @brief B.\n * @details D.\n */\n",
        frozenset(),
    ),
    (
        # The full repo-relative spelling, which hundreds of files here use.
        "libs/mod_sty/src/full_path.c",
        "/**\n * @file libs/mod_sty/src/full_path.c\n * @brief B.\n * @details D.\n */\n",
        frozenset(),
    ),
    (
        # A @param inside a PLAIN comment is prose, not documentation.
        "libs/mod_sty/src/plain_comment.c",
        "/**\n * @file plain_comment.c\n * @brief B.\n * @details D.\n */\n"
        "/* @param a is written like this in the style guide's own examples */\n",
        frozenset(),
    ),
)


def _check_style_mode() -> list[str]:
    """The style gate must fire on each defect and spare each legal spelling."""
    failures = []
    for rel, source, expected in _STYLE_FIXTURES:
        rows, _seen = audit_text(rel, source)
        codes = {row[2] for row in rows}
        failures.extend(
            f"style gate went toothless: {rel} should report {code} and did not"
            for code in sorted(expected - codes)
        )
        failures.extend(
            f"style gate false positive: {rel} is a legal form but reported {code}"
            for code in sorted(codes - expected)
        )
    return failures


def _check_style_ratchet() -> list[str]:
    """The @details ratchet must excuse frozen debt, and only frozen debt."""
    rows = [
        ("libs/frozen.c", 1, "DETAILS_MISSING", "no @details"),
        ("libs/fresh.c", 1, "DETAILS_MISSING", "no @details"),
        ("libs/fresh.c", 9, "PARAM_NO_DIRECTION", "plain @param"),
    ]
    violations, stale = partition(rows, {"libs/frozen.c", "libs/written.c"})
    offenders = {(row[0], row[2]) for row in violations}
    failures = []
    if ("libs/frozen.c", "DETAILS_MISSING") in offenders:
        failures.append("style ratchet: frozen @details debt was reported as a new violation")
    if ("libs/fresh.c", "DETAILS_MISSING") not in offenders:
        failures.append("style ratchet: a file outside the baseline escaped the @details rule")
    if ("libs/fresh.c", "PARAM_NO_DIRECTION") not in offenders:
        failures.append("style ratchet: the baseline excused a rule it does not cover")
    if stale != ["libs/written.c"]:
        failures.append(f"style ratchet: stale baseline entry not reported (got {stale})")
    return failures


def _check_style_floor() -> list[str]:
    """The vacuity floors must fire on a collapsed scan and pass a real one."""
    failures = []
    if _floor_failure([], 0) is None:
        failures.append("style gate: an EMPTY file list did not trip the vacuity floor")
    if _floor_failure(["x.c"] * 9999, 0) is None:
        failures.append("style gate: ZERO @param tags did not trip the vacuity floor")
    if _floor_failure(["x.c"] * 9999, 99999) is not None:
        failures.append("style gate: a plausible scan was rejected by the vacuity floor")
    return failures


def run_selftest() -> int:
    """Regression-test the auditor itself. Returns a process exit code.

    ``--check``, ``--members --check`` and ``--style`` are all enforcing gates,
    and a parser-driven gate has two ways to fail silently. It can stop
    recognising a construct, in which case the offenders inside it vanish and
    the tree looks documented; or it can start matching things that are not
    declarations at all, in which case it reports noise until somebody
    switches it off. Both directions are asserted, for all three modes.
    """
    with tempfile.TemporaryDirectory() as td:
        root = Path(td).resolve()
        with override_repo_root(root):
            func_rows, member_rows = _audit_synthetic(root)

    failures = [
        *_check_function_mode(func_rows),
        *_check_member_mode(member_rows),
        *_check_style_mode(),
        *_check_style_ratchet(),
        *_check_style_floor(),
    ]
    if failures:
        for f in failures:
            sys.stderr.write(f"[FAIL] doxy_audit selftest: {f}\n")
        sys.stderr.write(f"doxy_audit selftest: {len(failures)} failure(s)\n")
        return 1
    print(
        "doxy_audit selftest: OK (function gate reports bare and thin blocks and "
        "spares definition-site, forward-prototype and control-flow forms; member "
        "gate reports undocumented macros, enum values and struct members and "
        "spares documented ones; style gate reports a missing/stale file header, "
        "a missing @brief/@details and a directionless @param, spares the "
        "backslash, continued-name and full-path spellings, excuses only frozen "
        "@details debt, and refuses a vacuous scan)"
    )
    return 0
