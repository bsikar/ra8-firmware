#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# shellcheck shell=bash
#
# scripts/ci/gates/checks_standalone.sh -- The independently registered first-party checker gates.
#
# SOURCED, NEVER EXECUTED. scripts/ci.sh sources every file in this directory
# and is the only entry point; RA8_GATE_REGISTRY -- the single list of what
# gates exist -- stays there too. These files hold gate BODIES only, so there
# is still exactly one home for a gate's definition and exactly one command
# for a workflow to call (`just quality::local::gate <name>`). Adding a second
# registry here would recreate the drift the single-definition rule exists to
# prevent.
#
# Split out of checks.sh, which had reached 1046 lines against the 1000-line
# cap that _pcc_size_caps enforces. The seam is the responsibility line that
# was already there: checks.sh owns the AGGREGATE pre-commit-checks gate and
# the _pcc_* suites it dispatches, and this file owns every gate that is
# registered and invoked on its own. Nothing about the architecture changes;
# gate bodies are looked up by name at call time, so the file a body lives in
# is invisible to ci.sh and to the workflow.
#
# The suites stayed in checks.sh deliberately rather than moving here: two
# checkers, python_lock_policy_uv_cache.py and hil_cache_repair_rules.py, read
# scripts/ci/gates/checks.sh by path and parse _pcc_python_authority and
# _pcc_repository_structure out of its text. Moving those would have broken
# both.
#
# Gates in this file: agnostic-registers, shebangs, tier-imports,
# entry-points, pinout-freshness, font-coverage, bench-lock, annotations,
# enum-underlying-casts, tests-readme, disambig-readmes,
# cite-check, hum-register-map, arch-caps, arch-compiles, measured-counts,
# hil-eil-parity

# --- agnostic-registers --------------------------------------------------
# The existing clock, display, GPIO and timer reach-ins are migration debt
# under RA8FW-299. Freeze it before that migration starts: a new concrete symbol
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
# Every first-party shell path has an explicit typed authority: portable Bash
# or POSIX sh uses an env shebang, while security boundaries use fixed
# `/bin/bash -p`. Protected entries also bind the combined four-line preamble
# (shebang, SPDX, copyright, exact security rationale), reviewed executable
# mode, and complete outer privileged-body wrapper. Scope includes untracked
# files, so a brand-new script is judged immediately. --selftest runs first in
# both directions, so a collapsed authority cannot pass as clean.
gate_shebangs() (
  set -e
  python3 scripts/checks/check_shebangs.py --selftest
  python3 scripts/checks/check_shebangs.py
)

# --- tier-imports ---------------------------------------------------------
# The three-tier dependency arrow (#718), enforced instead of described.
# libs/, port/, src/ and tools/ are the PLATFORM and must not reach into
# apps/; within the products tier, apps/shared_libs/ sits BELOW the form
# categories (apps/host/, apps/board/) and must not reach up
# into them. Both directions have to be checked in two languages, because
# there are two ways to couple: an #include, and a CMake source list or
# include directory naming another layer's files.
#
# The scanner is calibrated for zero false positives on the current tree --
# comments are stripped in both languages before matching, since the three
# `apps/` mentions in the platform listfiles today are all prose. A bare
# `#include "mdl_cache.h"` is judged against an EXCLUSIVE header-basename
# census, so it fires only when the name can resolve nowhere else.
#
# --selftest FIRST, both directions plus separate PLATFORM and PRODUCTS
# population floors: a scanner whose scope collapsed to zero files is also
# perfectly quiet, and a collapsed census would make the bare-name rule
# vacuous while still reporting a clean tree.
gate_tier_imports() (
  set -e
  require_cmd python3 "the tier-imports gate is a Python source scanner"
  python3 scripts/checks/check_tier_imports.py --selftest
  python3 scripts/checks/check_tier_imports.py --all
)

# --- entry-points ---------------------------------------------------------
# Two build domains, two entry-point contracts, one boundary (#707). Hosted
# code (tests/, tools/) runs under an OS and uses ISO `int main(...)`.
# Firmware (examples/, src/, port/) is reached from Reset_Handler with no
# process and no exit status, so it uses `void main(void)`, declared once in
# libs/ra8_core/inc/ra8_boot_entry.h and legal only because the firmware lane
# compiles -ffreestanding.
#
# The compiler cannot police this on its own, which is why the gate exists.
# `int32_t` is `long int` on arm-none-eabi and `int` on the host, so the old
# `int32_t main(void)` meant a different function TYPE per domain -- and the
# declaration lived in sixteen copy-pasted `extern int32_t main(void);` lines
# in vector tables, a different translation unit from every definition. About
# thirty apps had drifted into declaring one type and defining another with
# nothing able to notice, and 208 files carried a local
# `#pragma GCC diagnostic ignored "-Wmain"` to keep the contradiction quiet.
#
# Requiring the shared header at each definition is what lets the compiler
# check; this gate enforces the include, the spelling, the absence of a
# value-returning `return` in a void main, and that no suppression comes back.
# It carries per-domain non-vacuity floors plus a MUST_DISCOVER set, so a scan
# that stops reaching a root fails instead of reporting a clean tree.
# --selftest FIRST, both directions.
gate_entry_points() (
  set -e
  require_cmd python3 "the entry-points gate scans first-party C for main()"
  python3 scripts/checks/check_entry_points.py --selftest
  python3 scripts/checks/check_entry_points.py
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

# --- font coverage --------------------------------------------------------
# Which characters the reader can draw with no SD card present is decided by
# the cmap of the subset checked in under libs/ra8_fonts/, and until #687 no
# file in the tree declared that set: the only record was the pyftsubset recipe
# in the docstring of scripts/gen/font_to_c.py, which nothing read the font
# back against and which is 33 codepoints wider than the committed bytes.
# .github/font-coverage-declaration.txt is now that record, held against the
# .ttf in BOTH directions, so narrowing the baked coverage cannot land as a
# replaced blob with a green build.
#
# --selftest FIRST, both directions: every rule is driven against synthetic
# coverage that must fire it, and the committed declaration must be quiet, so
# "0 findings" cannot mean a checker that stopped reading the font.
gate_font_coverage() (
  set -e
  require_cmd python3 "the font-coverage gate parses the committed .ttf cmaps"
  python3 scripts/checks/check_font_coverage.py --selftest
  python3 scripts/checks/check_font_coverage.py
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
    "Run 'just setup_python' locally; CI/container use the same uv lock."
  # Regression-test the checker itself before trusting its verdict.
  python3 scripts/checks/check_annotations.py --selftest
  python3 scripts/checks/check_annotations.py --check
)

# --- enum-underlying-casts -----------------------------------------------
# Preserve the C23 fixed-enum representability constraint. A cast enclosing a
# complete initializer can narrow before the compiler checks the enumerator;
# operand casts used to select intermediate arithmetic width remain legal.
gate_enum_underlying_casts() (
  set -e
  require_python_mod clang.cindex \
    "Run 'just setup_python' locally; CI/container use the same uv lock."
  python3 scripts/checks/check_enum_underlying_casts.py --selftest
  python3 scripts/checks/check_enum_underlying_casts.py --all
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
  (cd tools/ra8ci && GOWORK=off go run . tests-readme)
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

# --- arch-caps ------------------------------------------------------------
# The port-completeness gate (RA8FW-298 invariant #4, RA8FW-300). arch/arch.h gates whole
# declaration blocks on the capability flags a core answers in
# arch/core/<core>/caps.h, and it told the reader outright that the gate fails
# a flag set with no backend translation unit behind it and a cleared flag with
# no documented decline. Nothing enforced it, and its first real run found two
# flags -- ARCH_HAS_SIMD and ARCH_HAS_MMU -- that both caps.h files answered
# and the contract never declared at all.
#
# Everything checked is DERIVED from the contract rather than listed here: the
# gated flags come from its own #if lines, the functions a set flag owes come
# from the block that flag gates, and the companion constants come from the
# ::ARCH_* references inside that block. So a fifth capability added to arch.h
# is checked the moment it is written, with no second place to update.
#
# There is no arch backend in this tree yet, so a set flag is satisfied by a
# MIGRATION note naming the tracked path that implements it today. Those notes
# retire themselves: once a backend defines the symbols, the note is reported
# as stale, so the pre-migration map cannot outlive the move.
#
# --selftest FIRST, and it carries a population floor for the same reason the
# other derived gates do: a read that found no cores and no gated flags is also
# perfectly quiet.
gate_arch_caps() (
  set -e
  require_cmd python3 "the port-completeness gate is a Python source scanner"
  python3 scripts/checks/check_arch_caps.py --selftest
  python3 scripts/checks/check_arch_caps.py
)

# --- arch-compiles --------------------------------------------------------
# The compiler-side twin of arch-caps (RA8FW-300). arch/arch.h is the contract every
# arch/<isa>/ backend implements, and nothing in this tree compiled it: no
# library, no test, no tool. A header nobody compiles rots the way an unmeasured
# number rots, and it had ::K_ARCH_FAULT_RAW_MAX referenced in the docs of
# arch_fault_info_t and defined nowhere, with raw[8] written as a bare literal.
# A text checker cannot see that; a compiler sees it immediately.
#
# What this gate compiles, all at the -std=c23 the tree pins with -Wall -Wextra
# -Wpedantic -Wundef -Wconversion -Werror:
#   - the contract against each real core's caps.h, which also evaluates the
#     static_asserts the contract now carries against that core's capability
#     VALUES (a priority-bit width outside 1..8, say);
#   - the contract against two SYNTHETIC cores, every optional capability off
#     and every one on. The two real cores between them never exercise
#     ARCH_HAS_MEM_PROTECT (0), ARCH_HAS_RTOS_CONTEXT (0) or
#     ARCH_HAS_TRUSTZONE_M (0), so a syntax error inside one of those blocks
#     would wait undiscovered for the first backend that declines it.
# -Wundef earns its place here: a capability is consumed with #if ARCH_HAS_X,
# and a misspelled flag evaluates to 0, which reads as a clean decline.
#
# It also holds the contract FREESTANDING. A bare-metal target ships no
# <assert.h>, so the include list is checked against the C23 freestanding set
# textually rather than by cross-compiling, and the gate needs no target
# toolchain to enforce it.
#
# Derived, not listed: the gated capability names come from the contract's own
# #if lines and the companion constants the synthetic cores need come from
# whichever real core defines them, so a fifth optional capability is exercised
# in both states the moment it is written. --selftest first, and a core-count
# floor, because a discovery that found nothing is also perfectly quiet.
gate_arch_compiles() (
  set -e
  require_cmd python3 "the arch-contract compile gate is driven from Python"
  python3 scripts/checks/check_arch_compiles.py --selftest
  python3 scripts/checks/check_arch_compiles.py
)

# --- measured-counts ------------------------------------------------------
# Two planning pages argue a decision from counts of this tree, and both had
# rotted. docs/PORTS.md (RA8FW-299) argues the build-first port order from coupling
# counts: clock read 240 against a tree of 227, GPIO 52 against 49, timebase
# 247 against 237, population 470 against 452. arch/README.md (RA8FW-300) argues the
# migration order from how many first-party files include each misfiled
# Armv8-M header: boot_entry read 278 against 266, exception 30 against 12, scb
# 9 against 8, systick 8 against 6, all four inside a week of being written.
#
# So the mechanism belongs to the page, not to one script. A page opts in by
# carrying a fenced MEASURED BLOCK naming, per figure, the table row it backs
# and the command that produces it; the gate finds every such page, re-runs
# each command and fails when the manifest count or the cell it names has
# drifted, in either direction. A count that grew silently is a migration going
# backwards; one that shrank silently is progress nobody credited. A table row
# carrying a count with no entry behind it fails too, because an unreproducible
# number is the thing this gate exists to stop.
#
# --selftest FIRST, with a page floor, a measurement floor and a scanned-file
# floor, for the same reason the other derived gates carry them: a page whose
# grammar stopped matching reads as perfectly clean, and so does a discovery
# that stopped finding pages.
gate_measured_counts() (
  set -e
  require_cmd python3 "the measured-counts gate is a Python source scanner"
  python3 scripts/checks/check_measured_counts.py --selftest
  python3 scripts/checks/check_measured_counts.py --check
)

# --- hil-eil-parity -------------------------------------------------------
# EIL==HIL: re-derives each harness's app discovery from hil_all.sh /
# eil_all.sh and fails if a hil/ app has no hil.conf, sits outside
# eil_all.sh's run set, or declares a HIL_MODE ra8_emulator cannot check.
# Hardware-free, so an added HIL app cannot escape EIL coverage.
gate_hil_eil_parity() (
  set -e
  python3 scripts/checks/check_hil_eil_parity.py --selftest
  python3 scripts/checks/check_hil_eil_parity.py
)
