#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# shellcheck shell=bash
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
# Gates in this file: pre-commit-checks, agnostic-registers, annotations,
# doc-attachment, tests-readme, disambig-readmes, init-order-freshness,
# cite-check, hil-eil-parity

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
  # The obsolete-safety-standard ban moved to _pcc_cross_references, next to
  # the other "does this reference still hold?" checks.
  # --selftest FIRST for each derived-scope checker (#358): it proves the rule
  # fires and that tools/ -- silently omitted by the old hardcoded scan lists --
  # is back in scope, before the tree is trusted. A re-narrowed scope turns the
  # selftest red instead of passing green over files it stopped scanning.
  python3 scripts/checks/check_world_tags.py --selftest
  python3 scripts/checks/check_world_tags.py --strict
  # Every unit test must declare its MC/DC vector pattern via @par MC/DC:.
  # #325: the checker used `git diff --cached`, so in any CI checkout (nothing
  # staged) it scanned 0 files and passed unconditionally -- a no-op that read
  # as green while `make ci` (which stages `git add -A`) saw the real backlog.
  # --selftest FIRST proves the detector fires in BOTH directions, so one that
  # stopped matching cannot pass as clean; --all then audits the whole tree
  # index-independently (the fix), so CI and local agree. The --staged
  # counterpart runs blocking in scripts/git/pre-commit.
  python3 scripts/checks/check_mcdc_block.py --selftest
  python3 scripts/checks/check_mcdc_block.py --all
  # --all asks it to enumerate src/ + libs/ rather than read staged files.
  python3 scripts/checks/check_no_dynamic_alloc.py --all
  python3 scripts/checks/check_no_ai_attribution.py --selftest
  python3 scripts/checks/check_no_ai_attribution.py
  # C23 nullptr-only in first-party code. Vendor macros UX_NULL / TX_NULL /
  # FX_NULL / NX_NULL are exempted.
  python3 scripts/checks/check_no_null.py --selftest
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
  # --selftest proves the detector fires AND that an in-source build under
  # examples/<app>/build/ is excluded from the scope (#549) before a clean run.
  python3 scripts/checks/check_example_board_pins.py --selftest
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
  # The C6 SPI pin map is stated twice: coprocessor/esp32c6/pins.env (the source
  # of truth, read by shell) and sdkconfig.defaults (the same numbers in the only
  # syntax esp-idf reads). Drift between them is invisible downstream -- the
  # build succeeds, the image flashes, and the link silently never comes up
  # because the two ends drive different pins. Pure text compare, no esp-idf
  # needed, so it runs here as well as on the bench (build.sh calls it too).
  python3 scripts/checks/check_c6_pin_config.py --selftest
  python3 scripts/checks/check_c6_pin_config.py
  # infra/fleet.yml is the single registry of the machines CI runs on and how
  # much of each one it may use. The declaration has to hold together on its
  # own terms (capacity that fits the declared budget, per-instance floors, a
  # parseable quiet-hours window, an instance count that is the sizing
  # formula's or carries a written reason), no host_vars file may re-declare a
  # tunable it owns, and every variable the mapping emits must be one some role
  # actually reads. --selftest FIRST, both directions, so a rule that stopped
  # firing cannot pass as clean.
  python3 scripts/checks/check_fleet_declaration.py --selftest
  python3 scripts/checks/check_fleet_declaration.py
  # ra8-ci:latest, the image `make ci` boots, is a pure function of
  # .devcontainer/ only while scripts/ci/devcontainer_image.sh is its SOLE
  # builder (#521): a second `docker build -t ra8-ci` with the old
  # reuse-forever logic silently defeats the context-digest staleness guard,
  # exactly as the deleted inner-local.sh did (#528). This is the image's
  # equivalent of ci-parity's ban on a second `run:` check body. --selftest
  # FIRST, both directions plus a non-vacuity floor, so a reconstruction that
  # stopped matching fails instead of reporting a clean, empty scan.
  python3 scripts/checks/check_ci_image_single_builder.py --selftest
  python3 scripts/checks/check_ci_image_single_builder.py
)

# How source is written: trailing newline, named constants, C23 attribute
# spelling, no silently-discarded error codes, no session-bookkeeping tags.
_pcc_source_form() (
  set -e
  # Every first-party source file ends in a trailing newline. Complements
  # .clang-format InsertNewlineAtEOF (C/C++ only) by covering scripts and
  # config-as-code. --selftest proves the detector fires and that the derived
  # scope reaches the roots a hardcoded list had dropped (#549).
  python3 scripts/checks/check_final_newline.py --selftest
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
  # --selftest proves the detector fires and that the derived scope reaches the
  # roots a hardcoded list had dropped (#549).
  python3 scripts/checks/check_no_wave_references.py --selftest
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
  # must sit behind the RA8_INSECURE_STUB_CRYPTO / RA8_OFF_TARGET guard
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
  # (wfi/dsb/isb/nop/cpsie/cpsid/reset-spin) on RA8_OFF_TARGET -- those
  # route through libs/ra8_hal/inc/ra8_hw_intrinsics.h +
  # tests/mocks/ra8_host_asm_stub.c so the driver stays branch-free and
  # coverage lands on the shipping path (#293).
  python3 scripts/checks/check_no_driver_asm_guard.py
  # No first-party file may introduce a permanent anti-recovery brick ACTION
  # (setting the ce "Disable Initialize" security flag, or transitioning the DLM
  # to a terminal LCK_BOOT lock). Owner policy 2026-07-23: this project must
  # never permanently disable device recovery. --selftest runs first and asserts
  # the detector both fires on brick actions and stays silent on the recovery
  # scripts + defensive checks, so a detector that stopped matching cannot pass
  # as a clean tree.
  python3 scripts/checks/check_no_antirecovery.py --selftest
  python3 scripts/checks/check_no_antirecovery.py
  # The pre-flash IMAGE guard's detector (scripts/hil/lib/preflash_guard.sh runs
  # it before every flash): refuse a firmware image that programs a lockdown
  # value into the disable-initialize / permanent-block-protect / HUK-zeroize
  # option-setting region, while allowing benign OFS0/OFS1/SAS/BPS. No tree to
  # scan here, so only its --selftest runs -- it asserts a lockdown image is
  # refused and a benign one allowed, both directions.
  python3 scripts/checks/check_image_no_antirecovery.py --selftest
)

# The MC/DC discipline: DO-178C Level B makes MC/DC of every compound boolean
# decision the core evidence, so this is the half of the gate that has teeth
# about it.
_pcc_mcdc_discipline() (
  set -e
  # Every new compound boolean decision must arrive with MC/DC vectors.
  # #355: the checker used `git diff --cached`, so in any CI checkout (nothing
  # staged) it saw 0 files and exited 0, auditing nothing ever. It is
  # range-aware and fail-loud now (no mode / unresolvable range is exit 2, not
  # a silent clean scan). --selftest proves the detector in BOTH directions, so
  # one that stopped matching cannot pass as clean; the staged counterpart runs
  # blocking in scripts/git/pre-commit (--staged).
  python3 scripts/checks/check_new_compound_has_mcdc.py --selftest
  # ... and the CI teeth for that rule: the MC/DC RATCHET (#426). Until it
  # landed, enforcement here was decorative -- only the --selftest above ran, so
  # it proved the detector worked while auditing none of the tree, and a new
  # uncovered compound decision passed CI. The `--range` delta scan that was
  # meant to be the teeth had only ever existed here as a commented-out line.
  #
  # That delta scan is not what is enabled, because it keys on new source
  # LINES: with a backlog of pre-existing uncovered decisions it fails on a
  # mere reformat of one of them, which is a cliff, and a cliff gets bypassed.
  # The ratchet is the shape this tree already uses for a measured debt
  # (tidy_ratchet.py, misra_ratchet.py): per-file-per-function counts frozen in
  # .github/mcdc-compound-baseline.txt, any INCREASE fails, shrinkage passes
  # and can be re-baselined. The backlog is tolerated, cannot grow, and a
  # genuinely new uncovered decision fails the push. --selftest first, both
  # directions, so a ratchet that stopped detecting growth cannot pass clean.
  python3 scripts/checks/mcdc_compound_ratchet.py --selftest
  python3 scripts/checks/mcdc_compound_ratchet.py --check
)

# Cross-reference integrity: every in-tree reference points at something that
# still exists, and none of them is a rot-prone file:line anchor.
_pcc_cross_references() (
  set -e
  # The in-tree line-number citation ban: reference a symbol, never a file
  # plus line number, since line numbers rot. --selftest FIRST (#358): it
  # proves the ban fires in source AND docs and that tools/ -- omitted by the
  # old SCAN_ROOTS tuple -- is back in scope.
  python3 scripts/checks/check_line_citations.py --selftest
  python3 scripts/checks/check_line_citations.py
  # Every scripts/... path named anywhere in the tree resolves to a file that
  # exists. A git mv inside scripts/ silently breaks doc links, hook comments
  # and workflow steps -- no build error, no test failure, and ci-fast has
  # already missed exactly that. The selftest runs first: a path checker that
  # stopped matching would report a clean tree, which is worse than no gate.
  python3 scripts/checks/check_script_references.py --selftest
  python3 scripts/checks/check_script_references.py
  # Ban citations of safety standards that have been superseded (the checker
  # names them; this comment deliberately does not, since the ban applies to
  # this file too). --all, not the bare invocation (#190): it read
  # `git diff --cached` unconditionally, so in any CI checkout -- where nothing
  # is staged -- it enumerated 0 files, printed "0 findings" and passed, having
  # audited nothing for its whole life in this gate. Same defect class as
  # #325 / #355; a bare invocation is an error now rather than the vacuous mode.
  python3 scripts/checks/check_obsolete_standards.py --selftest
  python3 scripts/checks/check_obsolete_standards.py --all
)

# Documentation completeness, cross-reference integrity, and the test-side
# discipline rules (HIL instrumentation, assert casts).
_pcc_docs_and_tests() (
  set -e
  _pcc_cross_references
  # Per-app SystemInit boot init-order audit. --selftest FIRST (#190): the
  # discovery glob was capped at three directory levels while the tree is up to
  # five deep, so this saw 11 of 217 apps and reported the other 206 clean. The
  # selftest asserts the detector fires on an inverted sequence AND that live
  # discovery clears the app floor, so a re-collapsed glob fails instead of
  # reporting the cleanest tree it has ever seen.
  python3 scripts/checks/audit_init_order.py --selftest
  python3 scripts/checks/audit_init_order.py
  # OSHWA inclusive-terminology gate over first-party sources. --selftest
  # proves the detector fires on a legacy symbol, spares vendored/HW names, and
  # that the derived scope reaches the roots a hardcoded list had dropped (#549).
  python3 scripts/checks/check_inclusive_terminology.py --selftest
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
  # ... and the two docs/STYLE_GUIDE.md tag rules that attach to no symbol, so
  # neither gate above ever saw them (#532): the file-header block (@file
  # present and naming THIS file, @brief, @details) and the @param direction
  # bracket. The style guide asserted both as facts -- one of them naming
  # cite_check / check_world_tags as the enforcer, neither of which has ever
  # read a @file tag. @details is ratcheted against .github/doxy-details-
  # baseline.txt; everything else is hard, with zero debt.
  python3 scripts/checks/doxy_audit.py --style
  # Every hw_validated/hil app must be instrumented (a probed counter +
  # HIL_MODE=jlink_memprobe) or explicitly HIL_FAULT_EXPECTED -- a bare
  # HIL_MODE=alive proves nothing.
  python3 scripts/checks/check_hil_alive_policy.py
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
  _pcc_mcdc_discipline
  _pcc_docs_and_tests
)

# --- agnostic-registers --------------------------------------------------
# The existing clock, display, GPIO and timer reach-ins are migration debt
# under #693. Freeze it before that migration starts: a new concrete symbol
# outside the HAL, a named backend TU, or a board composition library fails
# now, while a removed reference passes and can be ratcheted into the baseline. The
# selftest drives the same scanner first and asserts both directions plus the
# live-scope floor, so a broken matcher cannot report a clean tree.
gate_agnostic_registers() (
  set -e
  require_cmd python3 "the agnostic-registers gate is a Python source scanner"
  python3 scripts/checks/check_agnostic_registers.py --selftest
  python3 scripts/checks/check_agnostic_registers.py --check
)

# --- shebangs -------------------------------------------------------------
# Every first-party shell script starts with `#!/usr/bin/env <interp>`, and
# every shebang the tree carries uses that form. A hardcoded interpreter path
# is a portability claim this tree cannot keep (NixOS, a Homebrew bash 5 on a
# Mac whose /bin/bash is the 3.2 without mapfile, busybox images); a MISSING
# shebang on a script -- the gate bodies and libs this file sits among used to
# be exactly that -- leaves the interpreter to whoever execs it; and the
# near-miss `# !/bin/bash` is not a shebang at all, it reads as one and the
# kernel never sees it. Scope comes from git ls-files including untracked
# files, so a brand-new script is judged the moment it is written. --selftest
# FIRST, both directions, so a rule that stopped matching cannot pass as clean.
gate_shebangs() (
  set -e
  python3 scripts/checks/check_shebangs.py --selftest
  python3 scripts/checks/check_shebangs.py
)

# --- init-order-freshness -------------------------------------------------
# docs/INIT_ORDER_AUDIT.md is COMMITTED yet GENERATED (mk/docs.mk writes it via
# audit_init_order.py --report). Nothing regenerated it and byte-compared the
# committed copy, so it silently drifted -- it claimed 11 apps while the tree
# held 217, for as long as the discovery glob was depth-capped (#190/#537). It
# is cited from docs/qualification/, so a stale copy misrepresents the boot
# order to the qualification set. This gate regenerates it from the current
# tree and FAILS if the committed copy differs. The generator is hardware-free
# and reads a sorted glob (byte-stable across runs), so unlike the slow
# artefact-freshness gate this one needs no build output. --selftest FIRST,
# both directions, so a comparator that stopped detecting drift cannot pass as
# a clean tree.
gate_init_order_freshness() (
  set -e
  require_cmd python3 "the init-order-freshness gate regenerates docs/INIT_ORDER_AUDIT.md"
  python3 scripts/checks/check_init_order_freshness.py --selftest
  python3 scripts/checks/check_init_order_freshness.py
)

# --- roadmap-dashboard-freshness ------------------------------------------
# docs/ROADMAP_DASHBOARD.md is COMMITTED yet GENERATED (mk/docs.mk writes it via
# scripts/report/roadmap_dashboard.py), rendered purely from docs/ROADMAP.md.
# Nothing regenerated it and byte-compared the committed copy, so it could
# silently drift out of step with ROADMAP.md the same way INIT_ORDER_AUDIT.md
# did (#537). This gate regenerates it from the current tree and FAILS if the
# committed copy differs. The generator reads one committed markdown file and is
# hardware-free (byte-stable across runs), so unlike the slow artefact-freshness
# gate this one needs no build output and sits in the fast group beside
# init-order-freshness. --selftest FIRST, both directions, so a comparator that
# stopped detecting drift cannot pass as a clean tree.
gate_roadmap_dashboard_freshness() (
  set -e
  require_cmd python3 "the roadmap-dashboard-freshness gate regenerates docs/ROADMAP_DASHBOARD.md"
  python3 scripts/checks/check_roadmap_dashboard_freshness.py --selftest
  python3 scripts/checks/check_roadmap_dashboard_freshness.py
)

# --- pinout-freshness -----------------------------------------------------
# docs/pinouts/ is COMMITTED yet GENERATED: scripts/gen/gen_pinouts.py parses
# the ball maps for all 64 RA8D2/RA8P1 part numbers straight out of section 1.7
# "Pin Lists" of the two committed datasheets. The file it replaced was a
# hand-written quick reference that had drifted into stating a 2 MB SRAM and a
# guessed port-availability table for a part whose datasheet says otherwise --
# which is the whole reason the data is now parsed rather than typed. This gate
# re-parses and byte-compares, so a datasheet revision that moves a ball cannot
# land without the reference moving with it.
#
# The generator's own parse floors do the load-bearing work: it fails unless it
# recovers exactly 32 part numbers per group, exactly the ball count the package
# names, exactly the I/O-port count the datasheet's Function Comparison table
# prints for that variant, and a port set equal to the one drawn by the
# section 1.6 ball-grid FIGURE for that variant -- an independent rendering of
# the same fact, so the parse is cross-checked rather than merely
# self-consistent. A pdftotext or poppler change that mangles the column layout
# therefore reds the gate instead of silently emitting a thinner table.
# --selftest FIRST, both directions, so a parser that stopped detecting a
# ragged table cannot pass as a clean tree.
gate_pinout_freshness() (
  set -e
  require_cmd python3 "the pinout-freshness gate re-parses the RA8 datasheets"
  require_cmd pdftotext "poppler-utils provides pdftotext; the datasheets are PDFs"
  python3 scripts/gen/gen_pinouts.py --selftest
  python3 scripts/gen/gen_pinouts.py --check
)

# --- bench-lock -----------------------------------------------------------
# One EK-RA8D2, ~20 concurrent agents, a nightly CI job and two humans. Every
# script that drives it must take the bench lock first (#497); this proves the
# tree still does, and derives the set of bench-touching scripts MECHANICALLY
# so a new one cannot be forgotten.
#
# --selftest FIRST, and it asserts three things rather than one: that a bare
# JLinkExe call is caught, that a guarded one is not, and a DISCOVERY FLOOR --
# the live scan must still find at least N bench-touching files and every file
# in its named list. This repo's dominant tooling defect is a detector that
# quietly stopped matching and reported a clean tree; the floor turns that into
# a red gate instead of a green one.
gate_bench_lock() (
  set -e
  python3 scripts/checks/check_bench_lock.py --selftest
  python3 scripts/checks/check_bench_lock.py
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

# --- tests-readme ---------------------------------------------------------
# tests/README.md says what each subdirectory of tests/ is for. Prose like that
# rots the instant someone adds tests/newthing/ and does not describe it, or
# removes a subdirectory and leaves the paragraph behind -- and nothing notices.
# This gate makes both impossible: an undocumented subdirectory fails it, and so
# does a README row naming a subdirectory that no longer exists.
#
# --selftest FIRST, both directions plus the floor: it builds throwaway tests/
# trees and asserts an undocumented subdir fires, a stale entry fires, an
# in-sync tree stays quiet, and a collapsed scan is caught -- so a comparator
# that stopped detecting drift cannot pass as a clean tree.
gate_tests_readme() (
  set -e
  require_cmd python3 "the tests-readme gate reads tests/README.md against the tree"
  python3 scripts/checks/check_tests_readme.py --selftest
  python3 scripts/checks/check_tests_readme.py
)

# --- disambig-readmes -----------------------------------------------------
# Several pairs of things here can be picked wrongly -- two filesystems, two
# firmware-update mechanisms, a facade and the driver under it -- and each pair
# carries one small README saying which to use. That prose rots the same way the
# tests/ README did: the library gets renamed, the cited symbol disappears, the
# "two apps use this" count quietly becomes eleven, and nothing notices.
#
# So each of those READMEs states its load-bearing claims in a machine-readable
# block and this gate re-derives every one from the tree. Registration is the
# block itself -- a second list of anti-drift READMEs would be the very drift the
# gate exists to stop.
#
# --selftest FIRST, both directions plus the floor: a broken path, a vanished
# symbol, a stale count and a misfiled owner each fire, an in-sync tree stays
# quiet, and a scan that finds nothing is reported as vacuous rather than clean.
gate_disambig_readmes() (
  set -e
  require_cmd python3 "the disambig-readmes gate re-derives README claims from the tree"
  python3 scripts/checks/check_disambig_readmes.py --selftest
  python3 scripts/checks/check_disambig_readmes.py
)

# --- cite-check -----------------------------------------------------------
# BOTH halves of the HUM citation policy, which needs both to mean anything:
#
#   cite-VALIDATION (cite_check --strict) -- every cite that EXISTS parses and
#   points at a real chapter/page. Clean tree-wide, so it runs as a hard gate.
#
#   cite-COVERAGE (cite_ratchet --check) -- every MMIO access HAS a cite. This
#   ran in NO gate until #534, so the headline rule ("every register read/write
#   or access MUST have a citation") was enforced nowhere: an entirely uncited
#   new driver passed --strict cleanly, because there was nothing there to
#   validate. The measured backlog is 2884 uncited accesses across 254 files,
#   which cannot be citation-filled mechanically -- a guessed subsection would
#   pass validation while being factually false. So it is frozen in
#   .github/cite-baseline.txt and RATCHETED: the existing debt burns down, a
#   newly-added uncited access fails today.
gate_cite_check() (
  set -e
  # --selftest FIRST (#358): proves a malformed cite fires and that tools/
  # (ra8_emulator cites the RA8 HUM) and port/ are back in scope, before trusting
  # a clean run over the derived first-party-C set. The ratchet's selftest does
  # the same for the coverage pass -- it runs the REAL detector over a fixture
  # tree, so a coverage pass that stopped matching cannot read as a burn-down.
  #
  # A `( set -e )` subshell, not a `{ }` block: run_gate_capture disables
  # ERREXIT around the call, and that suppression is live inside a block -- so
  # this gate reported only `--strict`'s status and discarded the selftest's,
  # defeating the whole point of running it first. check_gate_bodies.py now
  # rejects the block form for every gate.
  python3 scripts/checks/cite_check.py --selftest
  python3 scripts/checks/cite_check.py --strict
  python3 scripts/checks/cite_ratchet.py --selftest
  python3 scripts/checks/cite_ratchet.py --check
)

# --- hum-register-map -----------------------------------------------------
# The complement of cite-check, and the reason it is a SEPARATE gate:
# cite_check.py asks whether a citation is well-formed and points inside the
# right chapter; this asks whether the register it names EXISTS, at the offset
# we declare, on the page we cite. Three landed defects were invisible to the
# first question and obvious to the second -- the ra8_rsip family, #498's
# reserved-aperture GPTP window, and #539's EASCR.
#
# The authority is the committed manual PDF, re-parsed here on every run, so
# pdftotext is a hard requirement: a gate that skipped when poppler was absent
# would report every register in the tree clean.
gate_hum_register_map() (
  set -e
  require_cmd pdftotext
  # --selftest FIRST: proves each of the four rules fires on a broken input
  # AND stays quiet on a real one, that both vacuity floors reject an empty
  # scan, and that the ratchet only permits shrinkage. A symbol-table
  # extractor that silently produced nothing would otherwise pass forever.
  python3 scripts/checks/check_hum_register_map.py --selftest
  python3 scripts/checks/check_hum_register_map.py
)

# --- hil-eil-parity -------------------------------------------------------
# EIL==HIL: re-derives each harness's app discovery from hil_all.sh /
# eil_all.sh and fails if a hil/ app has no hil.conf, sits outside
# eil_all.sh's run set, or declares a HIL_MODE ra8_emulator cannot check.
# Hardware-free, so an added HIL app cannot escape EIL coverage.
gate_hil_eil_parity() (
  set -e
  python3 scripts/checks/check_hil_eil_parity.py
)
