# shellcheck shell=bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/ci/gates/checks.sh -- The first-party checker suites: check_*.py, annotations, docs, citations.
#
# SOURCED, NEVER EXECUTED. scripts/ci.sh sources every file in this directory
# and is the only entry point; RA8_GATE_REGISTRY -- the single list of what
# gates exist -- stays there too. These files hold gate BODIES only, so there
# is still exactly one home for a gate's definition and exactly one command
# for a workflow to call (bash scripts/ci.sh --gate <name>). Adding a second
# registry here would recreate the drift the single-definition rule exists to
# prevent.
#
# Gates in this file: pre-commit-checks, annotations, doc-attachment, cite-check, hil-sil-parity

# --- pre-commit-checks ----------------------------------------------------
# The check_*.py gate suite. Each entry runs in its default mode -- the same
# way scripts/git/pre-commit invokes it.
#
# The suite is grouped into helpers below rather than written as one 150-line
# body. The grouping is CONTIGUOUS and the execution order is unchanged: these
# checks are independent of one another, but the order decides which failure a
# developer sees first, and reordering them would churn that for no gain. Each
# helper is its own `( set -e; ... )` subshell, so the first failing check in a
# group still aborts the whole gate.

# Constructs that may not appear in first-party source at all: superseded
# standards, missing TrustZone world tags, MC/DC block markers, heap use after
# init (NASA P10 Rule 3), AI attribution, and the C NULL macro.
_pcc_banned_constructs() (
  set -e
  python3 scripts/checks/check_obsolete_standards.py
  python3 scripts/checks/check_world_tags.py --strict
  python3 scripts/checks/check_mcdc_block.py
  # --all asks it to enumerate src/ + libs/ rather than read staged files.
  python3 scripts/checks/check_no_dynamic_alloc.py --all
  python3 scripts/checks/check_no_ai_attribution.py
  # C23 nullptr-only in first-party code. Vendor macros UX_NULL / TX_NULL /
  # FX_NULL / NX_NULL are exempted.
  python3 scripts/checks/check_no_null.py --all
  # NASA P10 Rule 1 -- no goto/setjmp/longjmp in firmware. A parse-independent
  # textual backstop: goto/setjmp were enforced only indirectly via the MISRA
  # cppcheck ratchet, which runs at --std=c11 (cppcheck 2.20 cannot parse C23)
  # and skips C23-syntax lines, so a construct on a skipped line was never ruled
  # on. The textual scan does not depend on a parse and covers the whole tree.
  # (Recursion needs a call graph -- covered by annot_rules.py RA8_NO_RECURSION
  # and MISRA 17.2.) --selftest asserts the detector both fires on code and
  # stays silent on comment/string occurrences before the tree is trusted.
  python3 scripts/checks/check_no_goto_setjmp.py --selftest
  python3 scripts/checks/check_no_goto_setjmp.py --all
)

# The two size caps. NASA P10 Rule 4 -- every function fits in <=60 lines --
# plus the 1000-line file cap. Independent of the clang-tidy compile-db, so
# they cover cross-compiled TUs the host tidy build never sees
# (ThreadX/USBX/NetX/HAL register code).
#
# Both checkers were rewritten under #359: their scope is now derived from
# git ls-files plus per-file language detection rather than a hardcoded
# root/suffix list that had quietly stopped describing the tree, so they
# cover Python, shell, CMake, YAML, Make and linker scripts as well as C --
# and the extensionless git hooks, which no suffix-driven scope has ever
# seen. Both --selftests assert every parser in both directions.
#
# BOTH caps are now ENFORCING. Every offender the widened scope revealed was
# split by responsibility -- 8 files, then the 31 remaining oversized
# functions -- with no waiver list and no narrowed scope. The two rejected
# alternatives are worth naming, because both report green: a waiver list
# would grandfather the offenders permanently, and narrowing the scope back to
# C would restore the exact defect #359 exists to fix.
_pcc_size_caps() (
  set -e
  python3 scripts/checks/check_function_size.py --selftest
  python3 scripts/checks/check_file_size.py --selftest
  python3 scripts/checks/check_file_size.py
  python3 scripts/checks/check_function_size.py
)

# Where things are allowed to live: header placement, board-fact ownership,
# library layering, and the scope of .gitignore patterns.
_pcc_tree_structure() (
  set -e
  # A header under a src/ directory is module-private and must be named
  # *_internal.h. A non-internal src/ header is a misfiled public interface
  # (belongs in inc/) or an unmarked private one.
  python3 scripts/checks/check_header_file_placement.py
  # The EK-RA8D2 pinout is a board fact owned by libs/ra8_board_ek_ra8d2.
  # Forbid the (port << 8 | pin) idiom in examples so the USB-pin duplication
  # #251 fixed (identical pins copy-pasted across 29 apps) cannot come back.
  python3 scripts/checks/check_example_board_pins.py
  # ra8_core is the foundation lib: it must depend on nothing above itself.
  python3 scripts/checks/check_core_layering.py
  # No .gitignore directory pattern may match at arbitrary depth. `build/` did,
  # for the life of the tree: any directory named `build` was silently
  # unaddable, and nearly lost the files moved into scripts/build/  # PATHREF-OK: #359
  # and would have lost a seventh outright. The failure is invisible in both
  # directions -- git declines to add and says nothing -- so nothing but a gate
  # asking the question can catch it (#377).
  python3 scripts/checks/check_gitignore_scope.py --selftest
  python3 scripts/checks/check_gitignore_scope.py
)

# How source is written: trailing newline, named constants, C23 attribute
# spelling, no silently-discarded error codes, no session-bookkeeping tags.
_pcc_source_form() (
  set -e
  # Every first-party source file ends in a trailing newline. Complements
  # .clang-format InsertNewlineAtEOF (C/C++ only) by covering scripts and
  # config-as-code.
  python3 scripts/checks/check_final_newline.py
  # No magic numbers. clang-tidy's readability-magic-numbers only sees files
  # in the host compile-db (no example main.c, no ARM-only #ifdef paths),
  # which is how ra8_delay_ms(500U) slipped past CI.
  python3 scripts/checks/check_magic_numbers.py
  # C23 [[...]] attribute syntax tree-wide (GNU __attribute__((...)) is
  # rejected except for interrupt / cmse_nonsecure_entry / cmse_nonsecure_call,
  # which clang has no portable [[gnu::]] spelling for).
  python3 scripts/checks/check_no_gnu_attribute.py
  # The four C23 source patterns (_Static_assert -> static_assert, = {0} ->
  # = {}, no <stdbool.h>, paren-wrapped numeric #define values). These lived
  # ONLY as inline grep loops in scripts/git/pre-commit and were never run by
  # this gate, so a violation the hook rejects slipped through CI on any
  # machine whose hook was not installed. The hook and this gate now share one
  # implementation. The selftest asserts each rule in both directions before
  # the sweep so a rule that stopped matching cannot pass as a clean tree.
  python3 scripts/checks/check_c23_patterns.py --selftest
  python3 scripts/checks/check_c23_patterns.py --all
  # No silent ra8_err_t discards at TrustZone boot boundaries. A C23
  # (void)-cast silences [[nodiscard]] by ISO rule, so -Werror can never catch
  # a discarded ra8_cgc_init() right before a BLXNS (#191).
  python3 scripts/checks/check_tz_boundary_discard.py
  # Ban the numbered session-bookkeeping tags from comments and docs.
  python3 scripts/checks/check_no_wave_references.py
  # C23 typed enums (every enum names an explicit underlying type) and
  # pragma-once headers (no classic #ifndef include guards). Both were
  # CLAUDE.md mandates with no checker until #409; the --selftest asserts the
  # detector fires and stays silent for both rules before the tree is swept.
  python3 scripts/checks/check_c23_headers.py --selftest
  python3 scripts/checks/check_c23_headers.py --all
)

# Security invariants that a compiler cannot express: the NS->S entry surface,
# the placeholder-crypto guard, linker-only stubs, and driver asm guards.
_pcc_security_invariants() (
  set -e
  # Every RA8_NSC_VENEER declared in ra8_nsc.h must have a definition -- a
  # decl with no def advertises an NS->S trust-boundary entry point that does
  # not exist.
  python3 scripts/checks/check_nsc_veneer_defs.py
  # Every insecure placeholder-crypto body (deterministic TRNG, forgeable
  # key-import MAC, plain-SRAM key vault, non-cryptographic RSIP key-wrap)
  # must sit behind the RA8_INSECURE_STUB_CRYPTO / RA8_SIMULATOR_MODE guard
  # with a fail-closed #else, so a release image that forgot to swap in real
  # crypto fails closed instead of shipping the stub (#180).
  python3 scripts/checks/check_stub_crypto_guarded.py
  # No function may exist only to satisfy the linker. Two narrowly-calibrated
  # rules: SHADOW (a do-nothing second definition of a symbol implemented for
  # real elsewhere -- the tools/*/webp_stub.c case, which made both host tools
  # advertise WebP and fail at runtime) and CANNED (an unsupported-error return
  # that discards every argument). Legitimate no-ops -- platform alternatives,
  # vtable/ISR callbacks, the fail-closed crypto #else above, MMIO handlers
  # returning module state -- are outside both rules by construction. Hardware
  # that does not exist yet is waived only by TODO(<named missing part>).
  # --selftest runs first and asserts the detector both fires and stays silent
  # on the right inputs, so a detector that quietly stopped matching cannot
  # pass as clean.
  python3 scripts/checks/check_no_silent_stubs.py --selftest
  python3 scripts/checks/check_no_silent_stubs.py
  # A HAL peripheral driver must not guard bare CPU asm
  # (wfi/dsb/isb/nop/cpsie/cpsid/reset-spin) on RA8_SIMULATOR_MODE -- those
  # route through libs/ra8_hal/inc/ra8_hw_intrinsics.h +
  # tests/mocks/ra8_host_asm_stub.c so the driver stays branch-free and
  # coverage lands on the shipping path (#293).
  python3 scripts/checks/check_no_driver_asm_guard.py
)

# Documentation completeness, cross-reference integrity, and the test-side
# discipline rules (MC/DC arrival, HIL instrumentation, assert casts).
_pcc_docs_and_tests() (
  set -e
  # The in-tree line-number citation ban: reference a symbol, never a file
  # plus line number, since line numbers rot.
  python3 scripts/checks/check_line_citations.py
  # Per-app SystemInit boot init-order audit.
  python3 scripts/checks/audit_init_order.py
  # Every new compound boolean decision must arrive with MC/DC vectors.
  # #355: the checker used `git diff --cached`, so in any CI checkout (nothing
  # staged) it saw 0 files and exited 0, auditing nothing ever. It is
  # range-aware and fail-loud now (no mode / unresolvable range is exit 2, not
  # a silent clean scan). --selftest proves the detector in BOTH directions, so
  # one that stopped matching cannot pass as clean; the staged counterpart runs
  # blocking in scripts/git/pre-commit (--staged).
  python3 scripts/checks/check_new_compound_has_mcdc.py --selftest
  # The blocking --range scan is written and proven but off until the measured
  # backlog (998 decisions / 391 files, issue #426) is burned down. Then add:
  #   python3 scripts/checks/check_new_compound_has_mcdc.py \
  #     --range "$(ci_commit_range)" --repo "$(ci_history_repo)"
  # OSHWA inclusive-terminology gate over first-party sources.
  python3 scripts/checks/check_inclusive_terminology.py
  # MAXIMUM-documentation gate: every function -- including statics -- carries
  # the full Doxygen tag set.
  #
  # Regression-test the auditor before trusting either verdict. Both modes
  # below are enforcing and both are driven by one regex over source text, so
  # a construct the parser stops recognising takes its offenders with it and
  # the gate reports a documented tree. The selftest asserts both modes in
  # both directions: every defect class fires, and the legal-but-tricky forms
  # (a .c definition whose header owns the contract, a static forward
  # prototype, `else if`, inline asm) stay clean.
  python3 scripts/checks/doxy_audit.py --selftest
  python3 scripts/checks/doxy_audit.py --check
  # ... and for aggregate members: every enum value, struct/union member, and
  # macro across the first-party tree carries a doc comment.
  python3 scripts/checks/doxy_audit.py --members --check
  # Every hw_validated/hil app must be instrumented (a probed counter +
  # HIL_MODE=jlink_memprobe) or explicitly HIL_FAULT_EXPECTED -- a bare
  # HIL_MODE=alive proves nothing.
  python3 scripts/checks/check_hil_alive_policy.py
  # Every scripts/... path named anywhere in the tree resolves to a file that
  # exists. A git mv inside scripts/ silently breaks doc links, hook comments
  # and workflow steps -- no build error, no test failure, and ci-fast has
  # already missed exactly that. The selftest runs first: a path checker that
  # stopped matching would report a clean tree, which is worse than no gate.
  python3 scripts/checks/check_script_references.py --selftest
  python3 scripts/checks/check_script_references.py
  # Reject explicit integer casts inside TEST_ASSERT_EQ arguments. The macro
  # widens both args to int64_t, so an outer (int)/(uint32_t) cast is
  # redundant and latently buggy (a (int) cast on a uint32_t enum truncates
  # before the widening).
  python3 scripts/checks/check_assert_casts.py tests/*.c
)

gate_pre_commit_checks() (
  set -e
  _pcc_banned_constructs
  _pcc_size_caps
  _pcc_tree_structure
  _pcc_source_form
  _pcc_security_invariants
  _pcc_docs_and_tests
)

# --- annotations ----------------------------------------------------------
# check_annotations.py walks the AST via the libclang Python bindings and
# enforces the ra8_* annotation rules (docs/ANNOTATIONS.md).
#
# The import probe is load-bearing: check_annotations.py EXITS 0 when libclang
# is missing, so without the probe a strict gate reports nothing and passes.
# That is strictly worse than not running it at all.
gate_annotations() (
  set -e
  require_python_mod clang.cindex \
    "CI installs libclang==18.1.1; add it to .devcontainer/Dockerfile too."
  # Regression-test the checker itself before trusting its verdict.
  python3 scripts/checks/check_annotations.py --selftest
  python3 scripts/checks/check_annotations.py --check
)

# --- doc-attachment -------------------------------------------------------
# doxy_audit.py (run inside pre-commit-checks) asks only whether a block is
# PRESENT. A block attached to the wrong symbol SATISFIES that: paste one block
# twice and measured coverage rises while one symbol silently loses its
# documentation and another gains a duplicate. This gate asks the other
# question -- does the block describe the thing it sits on.
gate_doc_attachment() (
  set -e
  require_python_mod clang.cindex \
    "CI installs libclang==18.1.1; add it to .devcontainer/Dockerfile too."
  # Regression-test the checker itself, in BOTH directions, before trusting its
  # verdict: every defect class must fire, and the legal-but-tricky forms
  # (@copydoc, the CLAUDE.md definition-site one-liner, macro-generated
  # declarations, documented //#define options) must not.
  python3 scripts/checks/check_doc_attachment.py --selftest
  python3 scripts/checks/check_doc_attachment.py --check
)

# --- cite-check -----------------------------------------------------------
# cite-VALIDATION pass: every existing HUM cite must parse and point at a real
# chapter/page. The complementary cite-COVERAGE pass (--require-cites: does
# every MMIO access HAVE a cite?) surfaces a large libs/ra8_hal backlog and is
# not yet gate-clean, so it is deliberately not wired blocking.
gate_cite_check() {
  python3 scripts/checks/cite_check.py --strict
}

# --- hil-sil-parity -------------------------------------------------------
# SIM==HIL: re-derives each harness's app discovery from hil_all.sh /
# sil_all.sh and fails if a hil/ app has no hil.conf, sits outside
# sil_all.sh's run set, or declares a HIL_MODE board_sim cannot check.
# Hardware-free, so an added HIL app cannot escape SIM coverage.
gate_hil_sil_parity() {
  python3 scripts/checks/check_hil_sil_parity.py
}
