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
from doxy_style import _floor_failure, audit_text

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
/**
 * @brief Return one value unchanged.
 * @details Exercises a public definition whose included header owns the contract.
 * @param[in] value Value to return.
 * @return The original value.
 * @retval 0 The input was zero.
 * @pre @p value is initialised.
 * @pre The caller accepts the unchanged result.
 * @post No global state is mutated.
 * @post The result equals @p value.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
int hdr_documented(int value);

int hdr_undocumented(int value);
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

/* A public forward declaration is likewise only an ordering device. The
   C23 attribute exercises the live Cortex-M handler definition spelling. */
void good_attr_forward(void);

/**
 * @brief Handle a synthetic weak interrupt.
 * @details Exercises a documented attributed definition after a bare prototype.
 * @pre The synthetic interrupt is active.
 * @pre The caller accepts the handler side effects.
 * @post The synthetic interrupt has been handled.
 * @post Control returns to the caller.
 * @note Synthetic self-test fixture only.
 * @since 0.1.0
 */
[[gnu::weak]] void good_attr_forward(void)
{
}

/* A matching prototype must stay quiet even if the RA8_WEAK definition is
   bare; the definition itself must still fire so the gate keeps its teeth. */
void bad_weak_forward(void);

RA8_WEAK void bad_weak_forward(void)
{
}

/* A lookalike definition with a different parameter type must not silence the
   declaration. This pair should fire at the unmatched prototype. */
void bad_mismatched_forward(int value);

RA8_WEAK void bad_mismatched_forward(unsigned value)
{
  (void)value;
}

/* Definition-site policy: a non-static definition in a .c carries no block --
   the header owns the contract. */
int hdr_documented(int a)
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

/* Public definitions are waived at the definition site, but their bare
   header declarations must still be reported by the ordinary header audit. */
int hdr_undocumented(int value)
{
  return value;
}
""",
    # Definition-site association must be exact. Only the first declaration
    # below is a valid contract for the corresponding bare source definition.
    "libs/mod_doc/src/contract_internal.h": """
#pragma once
/**
 * @brief Accept a byte array.
 * @details Exercises name-independent array-to-pointer signature matching.
 * @param[in] arguments Bytes accepted by the function.
 * @return Whether the first byte is nonzero.
 * @retval true The first byte is nonzero.
 * @retval false The first byte is zero.
 * @pre @p arguments points to one readable byte.
 * @pre The caller retains ownership of @p arguments.
 * @post No memory is modified.
 * @post The result depends only on the first byte.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] static bool contract_documented(const unsigned char arguments[]);

/**
 * @brief Incomplete on purpose.
 * @details Missing the required contract tail on purpose.
 * @param[in] value Input value.
 * @return The original value.
 * @retval 0 The input was zero.
 */
static int contract_incomplete(int value);

/**
 * @brief Document a different function.
 * @details A complete contract with the wrong name must not mask a definition.
 * @param[in] value Input value.
 * @return The original value.
 * @retval 0 The input was zero.
 * @pre @p value is initialised.
 * @pre The caller accepts the result.
 * @post No memory is modified.
 * @post The result equals @p value.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
static int contract_other_name(int value);

/**
 * @brief Public-linkage lookalike.
 * @details A non-static declaration must not own a static definition contract.
 * @param[in] value Input value.
 * @return The original value.
 * @retval 0 The input was zero.
 * @pre @p value is initialised.
 * @pre The caller accepts the result.
 * @post No memory is modified.
 * @post The result equals @p value.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
int contract_static_mismatch(int value);

/**
 * @brief Wrong-signature lookalike.
 * @details A declaration with another parameter type cannot own the contract.
 * @param[in] text Input text.
 * @return Whether text was supplied.
 * @retval true Text was supplied.
 * @retval false Text was null.
 * @pre @p text may be null.
 * @pre The caller retains ownership of @p text.
 * @post No memory is modified.
 * @post The result depends only on @p text.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
static bool contract_signature_mismatch(const char* text);

#include "contract_transitive_internal.h"
""",
    "libs/mod_doc/src/contract_transitive_internal.h": """
#pragma once
/**
 * @brief Document a transitively visible lookalike.
 * @details A nested include must not own a bare definition's contract.
 * @param[in] value Input value.
 * @return The original value.
 * @retval 0 The input was zero.
 * @pre @p value is initialised.
 * @pre The caller accepts the result.
 * @post No memory is modified.
 * @post The result equals @p value.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
static int contract_transitive(int value);
""",
    "libs/mod_doc/src/contract_inactive_internal.h": """
#pragma once
/**
 * @brief Document an inactive lookalike.
 * @details A disabled include must not own a bare definition's contract.
 * @param[in] value Input value.
 * @return The original value.
 * @retval 0 The input was zero.
 * @pre @p value is initialised.
 * @pre The caller accepts the result.
 * @post No memory is modified.
 * @post The result equals @p value.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
static int contract_inactive(int value);
""",
    "libs/mod_doc/src/contracts.c": """
#include "contract_internal.h"
#if 0
#include "contract_inactive_internal.h"
#endif

static bool contract_documented(const unsigned char* arg0)
{
  return arg0[0] != 0;
}

static int contract_incomplete(int value)
{
  return value;
}

static int contract_wrong_name(int value)
{
  return value;
}

static int contract_cross_scope(int value)
{
  return value;
}

static int contract_static_mismatch(int value)
{
  return value;
}

static bool contract_signature_mismatch(int value)
{
  return value != 0;
}

static int contract_transitive(int value)
{
  return value;
}

static int contract_inactive(int value)
{
  return value;
}
""",
    # This declaration is complete and has the exact private signature, but
    # lives outside contracts.c's include graph and therefore cannot mask it.
    "libs/other/src/cross_internal.h": """
#pragma once
/**
 * @brief Cross-module lookalike.
 * @details Must remain irrelevant unless the source explicitly includes it.
 * @param[in] value Input value.
 * @return The original value.
 * @retval 0 The input was zero.
 * @pre @p value is initialised.
 * @pre The caller accepts the result.
 * @post No memory is modified.
 * @post The result equals @p value.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
static int contract_cross_scope(int value);
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

#: Basic function-mode offenders the gate must report, by function name.
_SELFTEST_FUNC_EXPECTED = frozenset(
    {
        "bad_no_block",
        "bad_mismatched_forward",
        "bad_thin_block",
        "bad_weak_forward",
        "contract_incomplete",
        "hdr_undocumented",
    }
)

#: Function-mode definitions the gate must leave alone.
_SELFTEST_FUNC_CLEAN = frozenset(
    {
        "good_full_block",
        "good_fwd_declared",
        "good_attr_forward",
        "hdr_documented",
        "contract_documented",
    }
)

# Each same-file forward fixture must produce one prototype and one definition.
_SELFTEST_FORWARD_ROW_COUNT = 2

#: Bare definitions that an incomplete or non-matching declaration must not mask.
_SELFTEST_CONTRACT_GAPS = frozenset(
    {
        "contract_incomplete",
        "contract_wrong_name",
        "contract_cross_scope",
        "contract_static_mismatch",
        "contract_signature_mismatch",
        "contract_transitive",
        "contract_inactive",
    }
)

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


def _check_same_file_forward_mode(func_rows: list) -> list[str]:
    """Verify exact same-file prototype/definition association in both directions."""
    forward_rows = {
        name: [row for row in func_rows if row[2] == name]
        for name in ("good_attr_forward", "bad_weak_forward", "bad_mismatched_forward")
    }
    failures = [
        f"same-file forward selftest did not parse both rows for '{name}'"
        for name, rows in forward_rows.items()
        if len(rows) != _SELFTEST_FORWARD_ROW_COUNT
    ]
    good_forward_gaps = [row for row in forward_rows["good_attr_forward"] if row[3]]
    if good_forward_gaps:
        failures.append(
            "same-file forward false positive: documented attributed definition "
            "or its bare prototype was reported"
        )
    bad_forward_gaps = [row for row in forward_rows["bad_weak_forward"] if row[3]]
    if len(bad_forward_gaps) != 1 or (
        bad_forward_gaps
        and bad_forward_gaps[0][1] != max(row[1] for row in forward_rows["bad_weak_forward"])
    ):
        failures.append(
            "same-file forward gate went toothless: a bare RA8_WEAK definition "
            "must fire exactly once at the definition, never at its prototype"
        )
    mismatched_gaps = [row for row in forward_rows["bad_mismatched_forward"] if row[3]]
    if len(mismatched_gaps) != 1 or (
        mismatched_gaps
        and mismatched_gaps[0][1] != min(row[1] for row in forward_rows["bad_mismatched_forward"])
    ):
        failures.append(
            "same-file forward signature match went toothless: a prototype with "
            "no exact definition must fire exactly once at the prototype"
        )
    return failures


def _check_header_contract_mode(gaps: set[tuple[str, str]]) -> list[str]:
    """Verify private header contracts associate only with exact definitions."""
    contract_source_gaps = {name for path, name in gaps if path == "libs/mod_doc/src/contracts.c"}
    failures = [
        f"header-contract lookup went toothless: bare definition '{name}' was "
        "masked by a missing, incomplete, wrong-name, wrong-signature, "
        "wrong-linkage, inactive, transitive, or out-of-scope declaration"
        for name in sorted(_SELFTEST_CONTRACT_GAPS - contract_source_gaps)
    ]
    contract_documented_row = ("libs/mod_doc/src/contracts.c", "contract_documented")
    if contract_documented_row in gaps:
        failures.append(
            "header-contract lookup false positive: a complete included static "
            "declaration with a compatible array/pointer signature was not associated"
        )
    return failures


def _check_function_mode(func_rows: list) -> list[str]:
    """The function gate must report every gap and spare every legal form.

    This includes header-owned definitions, same-file forward prototypes, and
    control-flow keywords that must never be mistaken for declarations.
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
        if name in _SELFTEST_FUNC_CLEAN
    )
    parsed = {r[2] for r in func_rows}
    parsed_pairs = {(r[0], r[2]) for r in func_rows}
    failures.extend(
        f"selftest fixture did not parse: '{name}' never reached the auditor, "
        f"so its shape was never exercised"
        for name in sorted(_SELFTEST_FUNC_EXPECTED | _SELFTEST_FUNC_CLEAN)
        if name not in parsed
    )
    required_source_rows = {
        ("libs/mod_doc/src/contracts.c", "contract_documented"),
        ("libs/mod_doc/src/good.c", "hdr_undocumented"),
    }
    failures.extend(
        f"selftest fixture did not parse source row: '{path}:{name}'"
        for path, name in sorted(required_source_rows - parsed_pairs)
    )
    if ("libs/mod_doc/src/good.c", "hdr_undocumented") in gaps:
        failures.append(
            "definition-site policy false positive: a public .c definition was "
            "reported instead of its undocumented header declaration"
        )
    failures.extend(_check_same_file_forward_mode(func_rows))
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

    failures.extend(_check_header_contract_mode(gaps))
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


def _check_style_strict() -> list[str]:
    """The closed @details debt must stay strict with no baseline."""
    rows = [
        ("libs/frozen.c", 1, "DETAILS_MISSING", "no @details"),
        ("libs/fresh.c", 1, "DETAILS_MISSING", "no @details"),
        ("libs/fresh.c", 9, "PARAM_NO_DIRECTION", "plain @param"),
    ]
    offenders = {(row[0], row[2]) for row in rows}
    failures = []
    if ("libs/frozen.c", "DETAILS_MISSING") not in offenders:
        failures.append("strict style gate hid a formerly baselined @details gap")
    if ("libs/fresh.c", "DETAILS_MISSING") not in offenders:
        failures.append("strict style gate hid a fresh @details gap")
    if ("libs/fresh.c", "PARAM_NO_DIRECTION") not in offenders:
        failures.append("strict style gate hid a directionless @param")
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
        *_check_style_strict(),
        *_check_style_floor(),
    ]
    if failures:
        for f in failures:
            sys.stderr.write(f"[FAIL] doxy_audit selftest: {f}\n")
        sys.stderr.write(f"doxy_audit selftest: {len(failures)} failure(s)\n")
        return 1
    print(
        "doxy_audit selftest: OK (function gate reports bare and thin blocks and "
        "spares definition-site, forward-prototype and control-flow forms; static "
        "header contracts match only by direct unconditional include, name, linkage and compatible "
        "signature while incomplete and lookalike declarations fail; member gate "
        "reports undocumented macros, enum values and struct members and spares "
        "documented ones; style gate reports a missing/stale file header, a missing "
        "@brief/@details and a directionless @param, spares the backslash, "
        "continued-name and full-path spellings, keeps @details strict without a baseline, "
        "and refuses a vacuous scan)"
    )
    return 0
