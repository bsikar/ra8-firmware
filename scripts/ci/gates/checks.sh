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
# for a workflow to call (`just quality::local::gate <name>`). Adding a second
# registry here would recreate the drift the single-definition rule exists to
# prevent.
#
# Gates in this file: pre-commit-checks.
#
# The gates that are registered and invoked on their own moved to
# checks_standalone.sh when this file reached the 1000-line cap. What stays
# here is the aggregate gate and the _pcc_* suites it dispatches, including
# _pcc_python_authority and _pcc_repository_structure, which two checkers read
# out of this file BY PATH and must keep finding here.

# --- pre-commit-checks ----------------------------------------------------
# The check_*.py gate suite. Each entry runs in its default mode -- the same
# way the removed pre-commit hook invoked it.
#
# The suite is grouped into helpers below rather than written as one 150-line
# body. The grouping is CONTIGUOUS and the execution order is unchanged: these
# checks are independent of one another, but their stable order keeps reports
# deterministic. Each helper is its own `( set -e; ... )` subshell, so the
# first failing check aborts that group; the aggregate still runs and reports
# every other independent group.

# Constructs that may not appear in first-party source at all: superseded
# standards, missing TrustZone world tags, heap use after
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
  # --all asks it to enumerate src/ + libs/ rather than read staged files.
  python3 scripts/checks/check_no_dynamic_alloc.py --selftest
  python3 scripts/checks/check_no_dynamic_alloc.py --all
  python3 scripts/checks/check_freestanding_runtime.py --selftest
  python3 scripts/checks/check_freestanding_runtime.py --check-scripts
  python3 scripts/checks/check_freestanding_runtime.py --check-asserts
  # Opaque C/POSIX FILE and DIR streams hide allocation and buffer ownership.
  # The production contract is fw_fs_file_t plus injected ra8_io/logging; host
  # adapters use raw descriptors with bounded caller-owned state. Selftest
  # proves every token, exact generated-source exclusion, and scope floor before
  # the zero-baseline full-tree sweep is trusted.
  python3 scripts/checks/check_no_stdio_streams.py --selftest
  python3 scripts/checks/check_no_stdio_streams.py --all
  python3 scripts/checks/check_no_ai_attribution.py --selftest
  python3 scripts/checks/check_no_ai_attribution.py
  # C23 nullptr-only in first-party code. Vendor macros UX_NULL / TX_NULL /
  # FX_NULL / NX_NULL are exempted.
  (cd tools/ra8ci && GOWORK=off go run . no-null)
  # NASA P10 Rule 1 -- no goto/setjmp/longjmp in firmware. A parse-independent
  # textual backstop: goto/setjmp were enforced only indirectly via the MISRA
  # cppcheck ratchet, which runs at --std=c11 (cppcheck 2.13 cannot parse C23)
  # and skips C23-syntax lines, so a construct on a skipped line was never ruled
  # on. The textual scan does not depend on a parse and covers the whole tree.
  # (Recursion needs a call graph -- covered by annot_rules.py RA8_NO_RECURSION
  # and MISRA 17.2.) --selftest asserts the detector both fires on code and
  # stays silent on comment/string occurrences before the tree is trusted.
  (cd tools/ra8ci && GOWORK=off go run . no-goto-setjmp)
)

# The two size caps. NASA P10 Rule 4 -- every function fits in <=60 lines --
# plus the 1000-line file cap. Independent of the clang-tidy compile-db, so
# they cover cross-compiled TUs the host tidy build never sees
# (ThreadX/USBX/NetX/HAL register code).
#
# Both checkers were rewritten under #359: their scope is now derived from
# git ls-files plus per-file language detection rather than a hardcoded
# root/suffix list that had quietly stopped describing the tree, so they
# cover Python, shell, CMake, YAML, Just and linker scripts as well as C --
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

# Migration contracts that span the executable hook/checker surfaces.
_pcc_migration_contracts() (
  set -e
  # Shell recursion must preserve the Just executable that entered the recipe;
  # a noninteractive SSH PATH need not contain that binary's directory.
  /bin/bash -p scripts/dev/run_just.sh --selftest
  python3 scripts/checks/check_shell_just_invocations.py --selftest
  python3 scripts/checks/check_shell_just_invocations.py
  # CI/checker user surfaces may depend on GNU Make as a build tool, but may
  # not resurrect it as the repository task runner.
  (cd tools/ra8ci && GOWORK=off go run . legacy-make)
  # Python-managed tools belong in venvs. Reject the system-pip override in
  # active automation and in copy-pasteable developer guidance.
  (cd tools/ra8ci && GOWORK=off go run . no-unsafe-python-install)
  # Release bootstrap paths must pin both the upstream version and per-arch
  # bytes. Prove the container, native dev box, and macOS paths all download to
  # disk and verify before executing, parsing, or installing anything.
  python3 scripts/checks/check_download_installers.py --selftest
  python3 scripts/checks/check_download_installers.py
  # Ansible stages dirty candidate bytes, but ignored caches and worktree-
  # deleted paths must never enter its image/provisioning context. Exercise
  # both directions of the exact Git census and archive verifier.
  python3 scripts/dev/stage_worktree_context.py --selftest
  # Native Just recipes must prove a C23 compiler pair, reset incompatible
  # C/CXX caches, and discover every compiled tools/* CMake project. This
  # prevents a Debian `cc` (gcc-12) configure and a newly added tool silently
  # falling outside `just tools::build`/clean. ARM toolchain configures remain
  # explicit and outside the host wrapper.
  bash scripts/builders/host_cmake.sh --selftest
  bash scripts/builders/check_host_build_entrypoints.sh --selftest
  bash scripts/builders/check_host_build_entrypoints.sh
  # The Cortex-M33 side of the dual-core product: ra8_add_cpu1_image() attaches
  # the first-party warning + stack-usage profile PER-SOURCE to its own
  # SOURCES, so app-added first-party M33 translation units compile with no
  # -Wall/-Wextra/-Werror and emit no .su data (#843, TODO(T1-09)). That escape
  # was prose only. Enumerate it instead: --selftest proves the classifier
  # fires in both directions, then the tree check holds the escape to the
  # shrink-only .github/cpu1-warning-profile-baseline.txt, so a NEW first-party
  # M33 source outside the profile is red rather than invisible.
  python3 scripts/checks/check_cpu1_warning_profile.py --selftest
  python3 scripts/checks/check_cpu1_warning_profile.py

  # The same escape one image over: a dual-image app builds its Non-Secure
  # half as a raw add_executable(), which inherits no warning flags, so the
  # profile call is written by hand in every such app and nothing checked
  # that it was. --selftest proves the detector fires in both directions
  # before the tree check runs (#759).
  python3 scripts/checks/check_ns_image_warning_profile.py --selftest
  python3 scripts/checks/check_ns_image_warning_profile.py

  # The same shape one layer down, in C rather than CMake: a store to a
  # security-attribution register with PRCR PRC4 locked is discarded silently,
  # so the code reports success and the attribution keeps its reset value.
  # That defect has been found and hand-fixed three times (#131, #759c, #759d)
  # with nothing stopping a fourth. --selftest proves the detector fires in
  # both directions before the tree check runs (#759 item a).
  python3 scripts/checks/check_attribution_gates.py --selftest
  python3 scripts/checks/check_attribution_gates.py
)

# The Python project, bootstrap, exports, and managed environment boundaries.
# Every detector runs its synthetic both-direction proof before the real tree
# check so a collapsed scope or parser cannot produce a vacuous green result.
_pcc_python_authority() (
  set -e
  python3 scripts/dev/bootstrap_uv.py --selftest
  /bin/bash -p scripts/dev/provision_dev_box_toolchain.sh --selftest-uv-cache-contract
  /bin/bash -p scripts/dev/setup_ansible.sh --selftest
  python3 scripts/checks/check_ansible_collections.py --selftest
  python3 scripts/dev/verify_locked_environment.py --selftest
  /bin/bash -p scripts/hil/lib/python_env.sh --selftest
  python3 scripts/checks/check_python_lock_policy.py --selftest
  python3 scripts/checks/check_python_lock_policy.py
  # This subcheck also runs inside the toolchain container, which intentionally
  # carries no nested runtime. Exercise every offline policy/lifecycle attack
  # here; image-owning hosts retain `--selftest` for the real label round-trip.
  /bin/bash -p scripts/ci/devcontainer_image.sh --selftest-offline
)

# Source and credential placement contracts shared by every first-party build
# unit. Kept separate from the board/repository checks so each helper stays
# below the enforcing NASA Rule 4 function-size cap.
_pcc_layout_and_credentials() (
  set -e
  # Every first-party C-family implementation lives under src/; interfaces
  # live under inc/ while module-private headers may remain beside their
  # implementation under src/. This keeps apps, examples, libraries, ports,
  # tests, and tools structurally predictable instead of allowing each build
  # unit to invent a flat layout.
  python3 scripts/checks/check_source_layout.py --selftest
  python3 scripts/checks/check_source_layout.py
  # Bench network credentials enter only after a freshly flashed image asks
  # for runtime provisioning. CMake, firmware source, and ordinary builders
  # must never ingest them or resurrect the removed compile-time path.
  python3 scripts/secrets/wifi_provision.py --selftest
  python3 scripts/hil/uart_write.py --selftest
  /bin/bash -p scripts/hil/lib/hil_conf.sh --selftest
  python3 scripts/checks/check_no_build_credentials.py --selftest
  python3 scripts/checks/check_no_build_credentials.py
  python3 scripts/checks/hil_privileged_helper_selftest.py --selftest
  python3 scripts/checks/check_hil_privilege_boundary.py --selftest
  python3 scripts/checks/check_hil_privilege_boundary.py
  python3 infra/ansible/roles/dev_box/files/ra8-hil-runner-idle-stop.py --selftest ignored.service
  /bin/bash -p scripts/hil/lib/rig_contract.sh --selftest
  python3 scripts/hil/rig_env_parse.py --selftest
  /bin/bash -p scripts/hil/run_direct.sh --selftest
  python3 scripts/checks/check_hil_rig_contract.py --selftest
  python3 scripts/checks/check_hil_rig_contract.py
  # A header under a src/ directory is module-private and must be named
  # *_internal.h. A non-internal src/ header is a misfiled public interface
  # (belongs in inc/) or an unmarked private one.
  bash scripts/builders/check_header_file_placement.sh --selftest
  bash scripts/builders/check_header_file_placement.sh
  # ra8_regs.h claims to be the one include that reaches every RA8D2 register
  # block. No TU includes an umbrella, so nothing preprocesses it and the claim
  # rots unobserved: by #1389 it re-exported 30 of 60 headers. Keep it complete.
  python3 scripts/checks/check_umbrella_regs.py --selftest
  python3 scripts/checks/check_umbrella_regs.py
)

# Board-fact ownership and library layering.
_pcc_board_and_layering() (
  set -e
  # The EK-RA8D2 pinout is a board fact owned by libs/ra8_board_ek_ra8d2.
  # Forbid the (port << 8 | pin) idiom in examples so the USB-pin duplication
  # #251 fixed (identical pins copy-pasted across 29 apps) cannot come back.
  # --selftest proves the detector fires AND that an in-source build under
  # examples/<app>/build/ is excluded from the scope (#549) before a clean run.
  bash scripts/builders/check_example_board_pins.sh --selftest
  bash scripts/builders/check_example_board_pins.sh
  # Library architecture: ra8_core stays foundational, module-private headers
  # do not leak across libraries, and hosted APIs stay behind port adapters.
  python3 scripts/checks/check_core_layering.py --selftest
  python3 scripts/checks/check_core_layering.py
  # No first-party library may name an RTOS or middleware API symbol: the
  # scheduler, the USB device stack and the FTL are reached through a seam
  # bound under port/ (#695 workstream (c)). The tree does not satisfy that
  # yet and #695 is design-only, so the ledger in the checker freezes the
  # leak sites that exist today: a new symbol fails, and a symbol that has
  # been burned down also fails until its ledger entry goes with it.
  # --selftest proves the detector fires, stays quiet on lookalike
  # identifiers (tx_len, tx_pool), and judges the ledger in both directions.
  python3 scripts/checks/check_rtos_symbol_isolation.py --selftest
  python3 scripts/checks/check_rtos_symbol_isolation.py
  # A board's memory map is a board fact too, and until #758 it was readable
  # only by the linker: three host-side consumers retyped it under three sets
  # of names. libs/ra8_board_<board>/inc/ra8_board_memmap.h publishes it, and
  # this pins the published copy to the MEMORY{} block next door so it can be
  # a second spelling of the map without becoming a second version of it.
  # --selftest proves each of the five rules fires against fixtures first.
  python3 scripts/checks/check_board_memory_map.py --selftest
  python3 scripts/checks/check_board_memory_map.py
)

# Repository-wide structural contracts that are independent of C source
# placement: ignore scope, fleet declarations, and CI-image ownership.
_pcc_repository_structure() (
  set -e
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
  # The FortiGate replay declaration is exact desired state, not an example.
  # Run the checker directly first, so the Just recipe cannot disable the test
  # that audits its own body. Then exercise that same public recipe using this
  # gate's managed Python rather than requiring a checkout-local venv on a
  # disposable runner. Neither path loads serial, secrets, or the network.
  python3 -B -I infra/network/fg_bringup.py --selftest config
  /bin/bash -p scripts/dev/run_just.sh \
    infra::fortigate_config_selftest "$(command -v python3)"
  # The fleet driver carries credentials across SSH boundaries.
  # Its offline selftest proves strict typed schemas, private snapshots,
  # shell-argument integrity, redacted summaries, and cleanup after failure.
  python3 scripts/dev/fleet.py selftest
  # ra8-ci:latest, the image `just ci` boots, is a pure function of its exact
  # root-context allowlist while scripts/ci/devcontainer_image.sh is its SOLE
  # builder (#521): a second `docker build -t ra8-ci` with the old
  # reuse-forever logic silently defeats the context-digest staleness guard,
  # exactly as the deleted inner-local.sh did (#528). This is the image's
  # equivalent of ci-parity's ban on a second `run:` check body. --selftest
  # FIRST, both directions plus a non-vacuity floor, so a reconstruction that
  # stopped matching fails instead of reporting a clean, empty scan.
  python3 scripts/checks/check_ci_image_single_builder.py --selftest
  python3 scripts/checks/check_ci_image_single_builder.py
)

# Where things are allowed to live: header placement, board-fact ownership,
# library layering, and repository structure. Calls remain in their original
# order so the first surfaced failure is unchanged.
_pcc_tree_structure() (
  set -e
  _pcc_layout_and_credentials
  _pcc_board_and_layering
  _pcc_repository_structure
)

# How source is written: trailing newline, named constants, C23 attribute
# spelling, no silently-discarded error codes, no session-bookkeeping tags.
_pcc_source_form() (
  set -e
  # Every first-party source file ends in a trailing newline. Complements
  # .clang-format InsertNewlineAtEOF (C/C++ only) by covering scripts and
  # config-as-code. --selftest proves the detector fires and that the derived
  # scope reaches the roots a hardcoded list had dropped (#549).
  (cd tools/ra8ci && GOWORK=off go run . final-newline)
  # No magic numbers. clang-tidy's readability-magic-numbers only sees files
  # in the host compile-db (no example main.c, no ARM-only #ifdef paths),
  # which is how ra8_delay_ms(500U) slipped past CI.
  python3 scripts/checks/check_magic_numbers.py --selftest
  python3 scripts/checks/check_magic_numbers.py
  # C23 [[...]] attribute syntax tree-wide (GNU __attribute__((...)) is
  # rejected except for interrupt / cmse_nonsecure_entry / cmse_nonsecure_call,
  # which clang has no portable [[gnu::]] spelling for).
  (cd tools/ra8ci && GOWORK=off go run . gnu-attribute)
  # The four C23 source patterns (_Static_assert -> static_assert, = {0} ->
  # = {}, no <stdbool.h>, paren-wrapped numeric #define values). These lived
  # ONLY as inline grep loops in the removed pre-commit hook and were never run by
  # this gate, so a violation the hook rejects slipped through CI on any
  # machine whose hook was not installed. The hook and this gate now share one
  # implementation. The selftest asserts each rule in both directions before
  # the sweep so a rule that stopped matching cannot pass as a clean tree.
  python3 scripts/checks/check_c23_patterns.py --selftest
  python3 scripts/checks/check_c23_patterns.py --all
  # No silent ra8_err_t discards at TrustZone boot boundaries. A C23
  # (void)-cast silences [[nodiscard]] by ISO rule, so -Werror can never catch
  # a discarded ra8_cgc_init() right before a BLXNS (#191).
  (cd tools/ra8ci && GOWORK=off go run . tz-boundary-discard)
  # Ban the numbered session-bookkeeping tags from comments and docs.
  # --selftest proves the detector fires and that the derived scope reaches the
  # roots a hardcoded list had dropped (#549).
  (cd tools/ra8ci && GOWORK=off go run . wave-references)
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
  (cd tools/ra8ci && GOWORK=off go run . nsc-veneer-defs)
  # Every insecure placeholder-crypto body (deterministic TRNG, forgeable
  # key-import MAC, plain-SRAM key vault, non-cryptographic RSIP key-wrap)
  # must sit behind the RA8_INSECURE_STUB_CRYPTO / RA8_OFF_TARGET guard
  # with a fail-closed #else, so a release image that forgot to swap in real
  # crypto fails closed instead of shipping the stub (#180).
  (cd tools/ra8ci && GOWORK=off go run . stub-crypto-guard)
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
  # tests/mocks/src/ra8_host_asm_stub.c so the driver stays branch-free and
  # coverage lands on the shipping path (#293).
  (cd tools/ra8ci && GOWORK=off go run . driver-asm-guard)
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
  # A documented thread-safety claim must still be backed by the unit's own
  # state (#893). The ra8_jpeg header advertised the decoder as re-entrant and
  # the encoder as thread-safe while both keep their working set in shared
  # statics; review caught it once, and nothing stopped it coming back. The
  # selftest runs first: a claim checker that stopped matching would report a
  # supported claim, which is the same defect class as the claim it polices.
  python3 scripts/checks/check_jpeg_concurrency_contract.py --selftest
  python3 scripts/checks/check_jpeg_concurrency_contract.py
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
  # Every hw_validated/hil app must be instrumented (a probed counter +
  # HIL_MODE=jlink_memprobe) or explicitly HIL_FAULT_EXPECTED -- a bare
  # HIL_MODE=alive proves nothing.
  python3 scripts/checks/check_hil_alive_policy.py --selftest
  python3 scripts/checks/check_hil_alive_policy.py
  # Reject explicit integer casts inside TEST_ASSERT_EQ arguments. The macro
  # widens both args to int64_t, so an outer (int)/(uint32_t) cast is
  # redundant and latently buggy (a (int) cast on a uint32_t enum truncates
  # before the widening).
  (cd tools/ra8ci && GOWORK=off go run . assert-casts)
)

_pcc_run_all() {
  ci_run_all "$@"
}

_pcc_git_environment_selftest() {
  python3 scripts/dev/git_environment.py --selftest
}

_pcc_ra8_apps_selftest() {
  # This registry drives developer and HIL wrappers indirectly, so exercise
  # its pruning and error contract before the selftest-coverage census.
  python3 scripts/dev/ra8_apps.py --selftest
}

_pcc_selftest_fail_then_pass() (
  set -e
  printf 'before-failure\n' >>"$log"
  false
  printf 'after-failure\n' >>"$log"
)

_pcc_selftest_green() {
  printf 'green\n' >>"$log"
  return 0
}

_pcc_aggregate_selftest() (
  local log output status
  log="$(mktemp)"
  trap 'rm -f "$log"' EXIT
  set +e
  output="$(_pcc_run_all masked _pcc_selftest_fail_then_pass later _pcc_selftest_green 2>&1)"
  status=$?
  set -e
  [[ "$status" -eq 1 ]]
  [[ "$(printf '%s\n' "$output" | grep -c 'masked (exit 1)')" -eq 1 ]]
  [[ "$(sed -n '1p' "$log")" == before-failure ]]
  [[ "$(sed -n '2p' "$log")" == green ]]
  if grep -q '^after-failure$' "$log"; then
    return 1
  fi
  : >"$log"
  _pcc_run_all green _pcc_selftest_green
  [[ "$(cat "$log")" == green ]]
)

gate_pre_commit_checks() (
  set -e
  _pcc_run_all \
    aggregate-selftest _pcc_aggregate_selftest \
    git-environment _pcc_git_environment_selftest \
    ra8-apps _pcc_ra8_apps_selftest \
    banned-constructs _pcc_banned_constructs \
    size-caps _pcc_size_caps \
    migration-contracts _pcc_migration_contracts \
    python-authority _pcc_python_authority \
    tree-structure _pcc_tree_structure \
    source-form _pcc_source_form \
    security-invariants _pcc_security_invariants \
    docs-and-tests _pcc_docs_and_tests
)
