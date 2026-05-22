# ra8d2-firmware

This file provides guidance to Claude Code when working in this repository. <!-- AI-OK: self-reference to CLAUDE.md/Claude Code -->

## Quick Reference Commands

- **Build default app**: `make` (blink)
- **Build specific app**: `make <app>` (e.g., `make blink_hal`)
- **List discovered apps**: `make apps`
- **Run host unit tests**: `make test`
- **Check MC/DC coverage**: `make mcdc`
- **Code formatter**: `make format` (apply) or `make check` (dry run)
- **Run linter (clang-tidy)**: `make tidy`
- **Generate Doxygen docs**: `make docs`
- **Pre-commit validation**: `make ascii` (encoding check), `make version` (check @since tags)

## Core Policies & Imports

- **Style Guide**: See @docs/STYLE_GUIDE.md (naming, enums, Doxygen, NASA Power of 10).
- **AI Attribution**: See @docs/AI_ATTRIBUTION_POLICY.md (STRICTLY NO AI attribution in commits/PRs/codebase. Use `AI-OK: <reason>` opt-out on lines quoting policies). <!-- AI-OK: quoting policy details -->
- **External Citations**: See @docs/CITATION_POLICY.md (Mandatory HUM citations: `/* HUM Ch X.Y "section name" p NNNN */` immediately above all HAL register accesses). No in-tree source citations (`file.c:line`).
- **Safety / MC/DC**: See @docs/MCDC.md and @docs/CERTIFICATION_SCOPE.md (DO-178C Level B requires MC/DC). State MC/DC vector pattern in test Doxygen blocks.
- **Architecture**: See @docs/ARCHITECTURE.md and @docs/RING_AND_WORLD.md.
- **Backward Compatibility**: None. Breaking changes are encouraged. Refactor immediately, delete old APIs, update all call sites.
- **Summary Documents**: DO NOT create summary documents, integration summaries, or completion reports unless explicitly requested by the user.

## Critical Gotchas & Coding Rules

- **C23 Syntax**: Use `bool` directly without `#include <stdbool.h>`. Use `static_assert` directly. Initialize structs/arrays with `= {}` (not `= {0}`).
- **Typed Enums (C23)**: Every enum MUST specify an explicit underlying type (`typedef enum : <type> { ... } name_t;`). Use `uintptr_t` for register base addresses. NO macros for integer constants.
- **Header Guards**: Use `#pragma once` at the top of headers. DO NOT use traditional include guards.
- **Function Validation**: Minimum 2 validation checks per function (NASA Rule 5). Use `RA_CHECK_NULL_PTR` from `ra_check.h` for null guards.
