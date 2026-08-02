#!/usr/bin/env bash
# shellcheck shell=bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/ci/gates/lint.sh -- Language linters, plus the gate asserting every code file is claimed.
#
# SOURCED, NEVER EXECUTED. scripts/ci.sh sources every file in this directory
# and is the only entry point; RA8_GATE_REGISTRY -- the single list of what
# gates exist -- stays there too. These files hold gate BODIES only, so there
# is still exactly one home for a gate's definition and exactly one command
# for a workflow to call (bash scripts/ci.sh --gate <name>). Adding a second
# registry here would recreate the drift the single-definition rule exists to
# prevent.
#
# Gates in this file: lint-py-shell, lint-cmake, lint-yaml, lint-make,
# lint-ld, lint-asm, lint-devcontainer, lint-coverage

# --- lint-py-shell --------------------------------------------------------
# --require: fail (never skip) when a tool is missing. These gates fail on ANY
# finding -- there is no grandfathering.
gate_lint_py_shell() (
  set -e
  # Pinned-version guard (#333). --selftest proves the version comparator fires
  # in both directions, then require_tool_versions asserts the three tools this
  # gate resolves are the pinned builds -- not the shellcheck 0.9.0 / shfmt
  # 3.6.0 a non-login PATH used to reach, and not an absent ruff. use_pinned_
  # tool_path in run_one_gate has already made the resolution deterministic; this
  # makes a wrong version fail loudly rather than lint under the wrong tool.
  python3 scripts/checks/check_tool_versions.py --selftest
  require_tool_versions ruff shellcheck shfmt
  # --selftest runs FIRST and asserts the configured rule set really enforces
  # what it advertises: a deliberately non-conforming fixture must trip every
  # rule family named in EXPECTED_CODES, and a legal-but-tricky one must stay
  # silent. #360 is why -- `check_ruff.py` hardcoded its target list, so seven
  # first-party files (host tooling and the HIL fixture generators) were never
  # linted at all while the gate reported a clean tree. Scope is derived from
  # `git ls-files` now, and gutting the select list turns the selftest red
  # instead of turning the tree green.
  python3 scripts/checks/check_ruff.py --selftest
  python3 scripts/checks/check_ruff.py --require
  # --selftest FIRST, then the tree. It asserts the shell checker still fires on
  # every class it claims to enforce (a relaxed severity or a dropped opt-in
  # check turns one of those green) and still stays quiet on the tricky-but-
  # correct forms this tree uses. Without it, "0 findings" is indistinguishable
  # from "checked nothing".
  python3 scripts/checks/check_shell.py --selftest
  python3 scripts/checks/check_shell.py --require
  # The narrow, first-party form of ShellCheck's SC2310. That opt-in check is
  # unsatisfiable here -- measured 90 findings, and the only two source forms
  # it ACCEPTS are worse than the ones it rejects: `set +e; fn; rc=$?; set -e`
  # (which it passes, and which still lets a brace-bodied callee run past a
  # failure) and a bare subshell (which aborts the parent). Adopting it would
  # mean ~90 disables. This fires only where a first-party function whose body
  # has two or more failable commands is invoked with its status masked, which
  # is the shape that once reduced this whole gate suite to "did each gate's
  # LAST command succeed". --selftest first, both directions, as ever.
  python3 scripts/checks/check_errexit_masking.py --selftest
  python3 scripts/checks/check_errexit_masking.py
)

# --- lint-cmake -----------------------------------------------------------
# 283 first-party listfiles decide what is compiled with which flags -- #309
# found host tools silently building firmware sources with -w. cmake-format is
# the formatter (dry-run: any reformat is a failure) and cmake-lint the linter;
# both come from cmakelang, pinned so the gate and the runner cannot disagree
# about a rule. Config and the reasoning for every widened name pattern live
# in .cmake-format.yaml.
gate_lint_cmake() (
  set -e
  require_cmd cmake-format "pip install --user cmakelang==0.6.13"
  require_cmd cmake-lint "ships with cmakelang; check the cmakelang install"
  require_tool_versions cmake-format cmake-lint

  # Scope comes from lint_targets.py, which is also what check_lint_coverage.py
  # asks when it verifies that every CMake listfile is covered. One definition,
  # two readers -- an inline `git ls-files | grep` here would be a second copy
  # of the coverage map and would drift from the coverage gate on first edit.
  local files
  mapfile -t files < <(python3 scripts/checks/lint_targets.py cmake)
  if [[ ${#files[@]} -eq 0 ]]; then
    echo "ERROR: no CMake listfiles found; refusing to report success." >&2
    return 1
  fi
  echo "lint-cmake: ${#files[@]} listfiles"

  # Both directions of the formatter, asserted before the real run: a
  # deliberately misformatted listfile must be rejected, and the formatter's
  # own output must be accepted.
  bash scripts/checks/lint_selftest.sh cmake

  printf '%s\n' "${files[@]}" | xargs -r -P "$(ra8_max_jobs)" -n 20 cmake-format --check
  printf '%s\n' "${files[@]}" | xargs -r cmake-lint
)

# --- lint-yaml ------------------------------------------------------------
# The workflow YAML decides which gates run at all -- #357 found two that never
# ran on dev pushes. yamllint covers structure and style; actionlint covers the
# Actions schema specifically (invalid `on:` triggers, bad ${{ }} expressions,
# unknown runner labels), which is the class of defect behind #357.
gate_lint_yaml() (
  set -e
  require_cmd yamllint "pip install --user yamllint==1.37.1"
  require_cmd actionlint \
    "https://github.com/rhysd/actionlint/releases (pinned to 1.7.7)"
  require_tool_versions yamllint actionlint

  # Scope comes from lint_targets.py -- see the note in gate_lint_cmake.
  local files
  mapfile -t files < <(python3 scripts/checks/lint_targets.py yaml)
  if [[ ${#files[@]} -eq 0 ]]; then
    echo "ERROR: no YAML files found; refusing to report success." >&2
    return 1
  fi
  echo "lint-yaml: ${#files[@]} files"

  bash scripts/checks/lint_selftest.sh yaml

  yamllint --strict "${files[@]}"
  actionlint
)

# --- lint-make ------------------------------------------------------------
# checkmake was evaluated and rejected -- its .PHONY parser reads only the
# first physical line of a continued declaration, so its one applicable rule
# misreports this tree. check_makefiles.py implements the rules correctly; the
# full reasoning and the reduced test case are in its module docstring.
gate_lint_make() (
  set -e
  python3 scripts/checks/check_makefiles.py --selftest
  python3 scripts/checks/check_makefiles.py
)

# --- lint-ld --------------------------------------------------------------
# No linter exists for the GNU ld script language, so this enforces what is
# mechanically checkable without linking: licence header, ENTRY, MEMORY
# regions, region closure, formatting, and that every g_ra8_ls_* symbol C
# references is defined by some script. See the module docstring for what was
# deliberately left unenforceable and why.
gate_lint_ld() (
  set -e
  python3 scripts/checks/check_linker_scripts.py --selftest
  python3 scripts/checks/check_linker_scripts.py
)

# --- reserved-addrs -------------------------------------------------------
# Three drivers have now named an address describing hardware that is not
# there (ra8_rsip, ra8_ptp #498, ra8_wdt_regs #545) -- each compiled clean and
# failed only on silicon. check_hum_register_map.py (#540) checks register
# symbols and struct/window offsets against the manual's tables, but an
# absolute-address enumerator is neither, so that gate does not cover this.
# This one answers the cheaper question: is the address inside a hole?
gate_reserved_addrs() (
  set -e
  python3 scripts/checks/check_reserved_addresses.py --selftest
  python3 scripts/checks/check_reserved_addresses.py
)

# --- lint-asm -------------------------------------------------------------
# No linter exists for GNU `as` -- searched for, not assumed; see the module
# docstring for what was evaluated and rejected. Assembling would cover one of
# the two files and would have to SKIP the other (they are Armv8.1-M and
# rv32imac, and only arm-none-eabi is provisioned), and a gate that skips is
# the defect this suite exists to remove. So this enforces what is checkable
# about assembly as text: licence header, an explicit .section, `.type` /
# `.size` / a label behind every exported symbol, `.syntax unified` on ARM,
# and formatting. The header rule alone closed a real hole -- check-copyright.py
# has never covered .S, so hand-written assembly carried no SPDX at all.
gate_lint_asm() (
  set -e
  python3 scripts/checks/check_asm.py --selftest
  python3 scripts/checks/check_asm.py
)

# --- lint-devcontainer ----------------------------------------------------
# .devcontainer/Dockerfile pins every tool version CI resolves, so a defect in
# it changes what every other gate runs -- and it was linted by nothing until
# #371. hadolint found DL4006 on its first run: eight `curl | tar` pipelines
# under `/bin/sh -c`, where a failed download still reported success and built
# an image with the pinned tool absent. The zshrc is checked with `zsh -n`,
# which is the only real option: ShellCheck refuses zsh, and running it as
# --shell=bash reports errors that are not errors on the file's own
# `${(%):-%n}` and `plugins=(...)`.
gate_lint_devcontainer() (
  set -e
  require_cmd hadolint \
    "https://github.com/hadolint/hadolint/releases (pinned to 2.14.0)"
  require_cmd zsh "apt-get install zsh (already in the devcontainer image)"
  require_tool_versions hadolint
  python3 scripts/checks/check_devcontainer.py --selftest
  python3 scripts/checks/check_devcontainer.py
)

# --- lint-coverage --------------------------------------------------------
# The meta-gate: it does not lint anything itself, it proves that everything
# else does. Enumerates every file from `git ls-files`, classifies it by name /
# extension / shebang, then asks each checker above -- in its own --list-files
# mode -- which files it claims, and fails when a code file is claimed by no
# linter or no formatter, or when a file type has no classification rule at all.
#
# "Is everything linted?" was previously answerable only by hand-auditing each
# checker's scan list. That audit ran five times and was wrong five times
# (#296, #332, #358, #359, #360), every time because a hardcoded root list had
# stopped matching the tree while the checker still reported clean. This gate
# makes the answer mechanical, and makes a NEW language landing in the tree
# (.rs, .ts, .proto) go red until somebody decides how it is checked.
#
# --selftest FIRST, as everywhere else: it asserts the gate still fires on a
# planted unclassified type, on a code file in a directory no checker
# enumerates, and on a checker whose scan list was narrowed -- and that it
# stays quiet on legitimately exempt files and on new files of a covered type.
gate_lint_coverage() (
  set -e
  python3 scripts/checks/check_lint_coverage.py --selftest
  python3 scripts/checks/check_lint_coverage.py
)
