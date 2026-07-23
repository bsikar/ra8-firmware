# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Regression tests for the annotation checker itself, on a synthetic tree.

Every defect guarded here is one this gate has actually shipped, and they share
a shape: the checker kept reporting a number, the number got smaller, and
smaller read as better.  So each assertion below is made in BOTH directions --
the rule fires on the broken fixture *and* stays quiet on the correct one --
because a rule can be "fixed" by defanging it and no single-direction test can
tell the difference.

The fixtures are laid out like ``libs/<module>/`` and ``tools/<tool>/`` so
``module_of()`` and ``is_first_party()`` resolve against them, with the
annotations spelled the way ``ra8_attributes.h`` lowers them so no project
header is needed.  Scope is pointed at the temporary tree through
:func:`annot_scope.override_repo_root`, never by rebinding a module global:
a rebinding is invisible to other modules' imports, which would leave this
suite asserting against the real checkout and passing without proving anything.
"""

from __future__ import annotations

import pathlib
import re
import sys
import tempfile

from annot_clang import cindex
from annot_loopbound import run_loopbound_selftest
from annot_model import AnnotatedSymbol, CallSite, ParseStats, Violation
from annot_rules import enforce_rules
from annot_scope import SOURCE_SUFFIXES, override_repo_root
from annot_walk import walk_tu

#: Synthetic TUs for run_selftest().
#:
#: Insertion order is load-bearing for the namesake case: it reproduces the
#: walk order that once took dev red. The namesake `static` is walked first,
#: so under a name-keyed table its USR was the one latched into the single
#: merged entry while that entry's `file` was overwritten by the RA8_PRIV
#: module's definition -- and a module's call to its own file-local helper
#: was reported as a cross-module RA8_PRIV call.
_SELFTEST_SOURCES: dict[str, str] = {
    # --- ra8_priv namesake resolution -----------------------------------
    # A namesake `static`, walked FIRST. Its call binds to its own
    # file-local copy and must never be attributed to mod_priv's RA8_PRIV
    # symbol. Three `internal_zero_bytes` and two `priv_byte_copy` have
    # this exact shape in-tree.
    "libs/mod_other/src/other.c": """
[[clang::annotate("ra8_internal")]] static void shared_helper(unsigned char* p, unsigned short n)
{
  for (unsigned short i = 0U; i < n; ++i) {
    p[i] = 0U;
  }
}

[[clang::annotate("ra8_priv")]] void other_caller(unsigned char* p);

void other_caller(unsigned char* p)
{
  shared_helper(p, 4U);
}
""",
    # The RA8_PRIV owner: external linkage, tagged the way a *_internal.h
    # does it. Clang propagates the attribute onto the definition in the
    # same TU, which is how the merged entry used to pick up this path.
    "libs/mod_priv/src/priv.c": """
[[clang::annotate("ra8_priv")]] void shared_helper(unsigned char* p, unsigned int n);
[[clang::annotate("ra8_priv")]] void priv_owner_caller(unsigned char* p);

void shared_helper(unsigned char* p, unsigned int n)
{
  for (unsigned int i = 0U; i < n; ++i) {
    p[i] = 0U;
  }
}

void priv_owner_caller(unsigned char* p)
{
  shared_helper(p, 4U);
}
""",
    # A genuine cross-module call to the external RA8_PRIV symbol. This one
    # MUST still be reported, otherwise the namesake fix would have
    # silenced the rule rather than made it accurate.
    "libs/mod_stranger/src/stranger.c": """
[[clang::annotate("ra8_priv")]] void stranger_caller(unsigned char* p);

void shared_helper(unsigned char* p, unsigned int n);

void stranger_caller(unsigned char* p)
{
  shared_helper(p, 4U);
}
""",
    # --- linkage rule: the four ways a definition passes ------------------
    "libs/mod_link/inc/mod_link.h": """
#pragma once
void link_public_api(void);
""",
    "libs/mod_link/src/mod_link_internal.h": """
#pragma once
void link_internal_declared(void);
[[clang::annotate("ra8_priv")]] void link_internal_annotated(void);
""",
    "libs/mod_link/src/pass.c": """
#include "mod_link.h"
#include "mod_link_internal.h"

/* Published by the library's public inc/ header: public API, no annotation. */
void link_public_api(void) {}

/* Declared in the *_internal.h AND tagged: the sanctioned cross-TU shape. */
void link_internal_annotated(void) {}

/* Tagged in place, declared nowhere: still fine, the tag is the statement. */
[[clang::annotate("ra8_test_helper")]] void link_test_hook(void) {}

/* static + RA8_INTERNAL: out of scope for the rule entirely. */
[[clang::annotate("ra8_internal")]] static void link_file_local(void) {}

int main(void)
{
  link_file_local();
  return 0;
}
""",
    # --- linkage rule: the two ways a definition fails --------------------
    "libs/mod_link/src/fail.c": """
#include "mod_link_internal.h"

/* Declared library-private but never classified -> wants RA8_PRIV. */
void link_internal_declared(void) {}

/* Nothing declares it and no table names it -> wants static or a header. */
void link_undeclared(void) {}
""",
    # --- linkage rule: the vector-table exemption -------------------------
    # Two byte-identical handlers. One is named by the table, one is not.
    # If the exemption were keyed on a name pattern instead of on table
    # membership, both would pass and the rule would be blind to any
    # handler-shaped symbol anyone chose to leave unwired.
    "libs/mod_link/src/vectors.c": """
void handler_tabled(void);
void handler_untabled(void);

void handler_tabled(void) {}
void handler_untabled(void) {}

void (*const g_vector_table[])(void) = {
    handler_tabled,
};
""",
    # --- NASA P10 Rule 3: firmware allocates -> reported ------------------
    # Both helpers are `static` so the linkage rule has no opinion on them
    # and the only thing under test is the allocation sweep.
    "libs/mod_alloc/src/alloc.c": """
void* malloc(unsigned long n);
void free(void* p);

/* Firmware, no waiver: both the malloc and the free must be reported. */
[[clang::annotate("ra8_internal")]] static void fw_untagged_allocator(void)
{
  void* p = malloc(16UL);
  free(p);
}

/* Firmware WITH the documented waiver: the sweep must leave it alone. */
[[clang::annotate("ra8_nasa_rule_3_ok")]] static void fw_waived_allocator(void)
{
  void* p = malloc(16UL);
  free(p);
}
""",
    # --- tools/ is in scope, and is host-only -----------------------------
    # This fixture carries the whole point of widening SCAN_DIRS to tools/,
    # in both directions at once. `host_unpublished` proves the linkage rule
    # genuinely reaches into tools/ -- if tools/ fell back out of SCAN_DIRS,
    # is_first_party() would drop it and the selftest fails. `host_allocator`
    # proves the Rule 3 sweep does NOT fire there, so the widening cannot be
    # "passed" by burying a host emulator under 216 unactionable findings.
    "tools/mod_host/src/host_tool.c": """
void* malloc(unsigned long n);
void free(void* p);

[[clang::annotate("ra8_internal")]] static void host_allocator(void)
{
  void* p = malloc(16UL);
  free(p);
}

/* Non-static, no header declares it: in scope, and a genuine gap. */
void host_unpublished(void)
{
  host_allocator();
}

/* RA8_PRIV inside tools/mod_host. Reaching this from another tool is the
   same boundary violation as one library calling another's private helper. */
[[clang::annotate("ra8_priv")]] void host_priv_helper(void);

void host_priv_helper(void) {}
""",
    # A second tool calling the first one's RA8_PRIV symbol. module_of() has
    # to resolve tools/<tool> as a module for this to be caught at all.
    "tools/mod_other_host/src/other_host.c": """
[[clang::annotate("ra8_priv")]] void other_host_caller(void);

void host_priv_helper(void);

void other_host_caller(void)
{
  host_priv_helper();
}
""",
}

#: What run_selftest() expects the linkage rule to report, by symbol name.
#: ``handler_untabled`` is here because it is byte-identical to a handler
#: the table does name: table membership is the only thing separating them.
#: ``host_unpublished`` lives under ``tools/`` and is the scope assertion:
#: the linkage rule only judges files is_first_party() accepts, so this name
#: goes missing the moment ``tools`` drops out of SCAN_DIRS.
_SELFTEST_LINKAGE_EXPECTED = frozenset(
    {"link_internal_declared", "link_undeclared", "handler_untabled", "host_unpublished"}
)

#: Definitions the linkage rule must leave alone -- one per passing shape,
#: plus the un-tabled handler's twin that proves the exemption is keyed on
#: table membership rather than on what the function looks like.
_SELFTEST_LINKAGE_CLEAN = (
    "link_public_api",
    "link_internal_annotated",
    "link_test_hook",
    "link_file_local",
    "main",
    "handler_tabled",
)

#: What _selftest_parse() returns: the symbol table, the call list and the
#: set of USRs a vector table names.
_SelftestParse = tuple[dict[str, AnnotatedSymbol], list[CallSite], set[str]]


def _selftest_parse(root: pathlib.Path) -> _SelftestParse:
    """Write the synthetic tree under ``root`` and walk every TU in it."""
    tu_paths: list[pathlib.Path] = []
    for rel, body in _SELFTEST_SOURCES.items():
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(body)
        if path.suffix in SOURCE_SUFFIXES:
            tu_paths.append(path)

    args = [
        "-std=c23",
        "-x",
        "c",
        f"-I{root}/libs/mod_link/inc",
        f"-I{root}/libs/mod_link/src",
    ]
    symbols: dict[str, AnnotatedSymbol] = {}
    calls: list[CallSite] = []
    vector_entries: set[str] = set()
    index = cindex.Index.create()
    for path in tu_paths:
        tu = index.parse(
            str(path),
            args=args,
            options=cindex.TranslationUnit.PARSE_DETAILED_PROCESSING_RECORD,
        )
        walk_tu(tu, path, symbols, calls, ParseStats(), vector_entries)
    return symbols, calls, vector_entries


def _names_matching(violations: list[Violation], rule: str, pattern: str) -> set[str]:
    """Pull the symbol names out of every ``rule`` finding matching ``pattern``."""
    out: set[str] = set()
    for v in violations:
        if v.rule != rule:
            continue
        m = re.search(pattern, v.message)
        if m:
            out.add(m.group(1))
    return out


def _check_priv_namesakes(
    violations: list[Violation], symbols: dict[str, AnnotatedSymbol]
) -> list[str]:
    """RA8_PRIV must separate namesakes by USR without going toothless."""
    offenders = {pathlib.Path(v.file).name for v in violations if v.rule == "ra8_priv"}
    failures: list[str] = []
    if "other.c" in offenders:
        failures.append(
            "namesake regression: a module's call to its own file-local static "
            "'shared_helper' was reported as a cross-module RA8_PRIV call"
        )
    if "priv.c" in offenders:
        failures.append(
            "same-module regression: mod_priv calling its own RA8_PRIV symbol was reported"
        )
    if "stranger.c" not in offenders:
        failures.append(
            "ra8_priv went toothless: the genuine cross-module call to RA8_PRIV "
            "'shared_helper' from mod_stranger was NOT reported"
        )
    if "other_host.c" not in offenders:
        failures.append(
            "ra8_priv is blind under tools/: mod_other_host calls mod_host's "
            "RA8_PRIV 'host_priv_helper' across a module boundary and it was NOT "
            "reported -- module_of() is not resolving tools/<tool> as a module, so "
            "every RA8_PRIV tag under tools/ is decorative"
        )

    # The merged symbol table is the underlying defect, so assert its shape
    # directly too: the namesakes must stay distinct entries.
    namesakes = [s for s in symbols.values() if s.name == "shared_helper"]
    expected_namesakes = 2  # one external (mod_priv) + one static (mod_other)
    if len(namesakes) != expected_namesakes:
        failures.append(
            f"symbol table merged namesakes: expected {expected_namesakes} distinct "
            f"'shared_helper' entries, found {len(namesakes)}"
        )
    return failures


def _check_linkage(violations: list[Violation]) -> list[str]:
    """The linkage rule must catch both gap shapes and exempt only tabled handlers."""
    linkage = _names_matching(violations, "ra8_linkage", r"'([^']+)'")
    failures = [
        f"ra8_linkage went toothless: '{name}' has external linkage that "
        f"nothing justifies, and the rule did not report it"
        for name in sorted(_SELFTEST_LINKAGE_EXPECTED - linkage)
    ]
    failures.extend(
        f"ra8_linkage false positive: '{name}' is a justified definition but the rule reported it"
        for name in sorted(linkage - _SELFTEST_LINKAGE_EXPECTED)
    )
    if "handler_untabled" not in linkage:
        failures.append(
            "vector-table exemption over-matches: 'handler_untabled' is byte-identical "
            "to a tabled handler but appears in no table, so it must still be reported"
        )
    return failures


def _check_rule3(violations: list[Violation]) -> list[str]:
    """NASA P10 Rule 3, both axes: the waiver and the firmware/host boundary."""
    allocators = _names_matching(violations, "ra8_nasa_rule_3_ok", r"from '([^']+)'")
    failures: list[str] = []
    if "fw_untagged_allocator" not in allocators:
        failures.append(
            "ra8_nasa_rule_3_ok went toothless: firmware function "
            "'fw_untagged_allocator' calls malloc/free with no waiver and was not reported"
        )
    if "fw_waived_allocator" in allocators:
        failures.append(
            "ra8_nasa_rule_3_ok false positive: 'fw_waived_allocator' carries "
            "RA8_NASA_RULE_3_OK, which is exactly the documented waiver"
        )
    if "host_allocator" in allocators:
        failures.append(
            "ra8_nasa_rule_3_ok false positive: 'host_allocator' is under tools/, "
            "which the host toolchain compiles and no firmware image contains -- "
            "Rule 3 is a claim about firmware"
        )
    return failures


def _check_fixtures_parsed(symbols: dict[str, AnnotatedSymbol]) -> list[str]:
    """Every fixture the clean-shape assertions rely on must have parsed."""
    seen = {s.name for s in symbols.values()}
    return [
        f"selftest fixture did not parse: '{name}' is missing from the "
        f"symbol table, so its linkage shape was never exercised"
        for name in _SELFTEST_LINKAGE_CLEAN
        if name not in seen
    ]


def run_selftest() -> int:
    """Regression-test the checker itself. Returns a process exit code.

    Four classes of defect are guarded, all of which this gate has shipped:

    * **Namesake merging.** Keying the symbol table by bare name merged
      distinct same-named functions into one entry, so a module calling its
      own file-local ``static`` was reported for calling another module's
      RA8_PRIV symbol.
    * **A rule that cannot fire.** The linkage rule turns "nothing declares
      this" into a failure, and a rule which reports nothing looks exactly
      like a clean tree. The synthetic module contains one definition of
      every passing shape and one of every failing shape.
    * **A root silently out of scope.** ``tools/`` was absent from SCAN_DIRS,
      so board_sim, media_dl and ra8_viewer were never checked and the gate
      reported a clean tree over code it had not read. The ``tools/`` fixture
      pins the scope from inside the rules rather than by reading the
      constant: ``host_unpublished`` is only reachable if is_first_party()
      accepts the root.
    * **NASA P10 Rule 3 scope and waiver.** Rule 3 is a claim about firmware,
      so it is asserted along both axes -- untagged firmware allocation
      fires, the documented waiver does not, and host-only code under
      ``tools/`` does not. Getting the second axis wrong is what turns a real
      gate into 216 findings nobody can act on.
    """
    with tempfile.TemporaryDirectory() as td:
        root = pathlib.Path(td).resolve()
        with override_repo_root(root):
            symbols, calls, vector_entries = _selftest_parse(root)
            violations = enforce_rules(symbols, calls, vector_entries)

    failures = [
        *_check_priv_namesakes(violations, symbols),
        *_check_linkage(violations),
        *_check_rule3(violations),
        *_check_fixtures_parsed(symbols),
        # The loop-bound scan is textual and libclang-free, so it self-tests on
        # synthetic source strings rather than the parsed synthetic tree.
        *run_loopbound_selftest(),
    ]
    if failures:
        for f in failures:
            sys.stderr.write(f"[FAIL] check_annotations selftest: {f}\n")
        sys.stderr.write(f"check_annotations selftest: {len(failures)} failure(s)\n")
        return 1
    print(
        "check_annotations selftest: OK (namesakes resolved by USR; linkage rule "
        "catches both gap shapes, reaches tools/, and exempts only tabled handlers; "
        "NASA rule 3 fires on untagged firmware allocation and stays quiet on the "
        "documented waiver and on host-only code; loop-bound scan fires on a "
        "mis-attached marker and a stale RA8_BOUNDED_LOOP statement, stays quiet on "
        "correct markers and on #define/comment/string mentions)"
    )
    return 0
