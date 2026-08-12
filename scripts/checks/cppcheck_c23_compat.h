/**
 * @file cppcheck_c23_compat.h
 * @brief cppcheck-only C23 compatibility shim, force-included by the gate.
 *
 * @details
 * The pinned analyser (cppcheck 2.13, enforced by `require_tool_versions`)
 * does not recognise the C23 `nullptr` keyword as a null pointer constant.
 * A guard such as `if (fp == nullptr) { return err; }` therefore fails to
 * convince its value-flow engine that `fp` is null on that path, and it
 * reports a false-positive `resourceLeak` / `memleak` on every `fopen` or
 * `malloc` guarded that way.
 *
 * Firmware never trips this: NASA Power-of-10 Rule 3 forbids the heap and
 * there is no `FILE*` on bare metal, so `src/` and `libs/` carry no such
 * pattern. The host tools under `tools/` do, which is why the gap only
 * surfaces once `tools/` comes under the cppcheck gate.
 *
 * This header maps the keyword to the classic null pointer constant for the
 * analyser only. It is passed with `--include=` (a cppcheck-exclusive flag),
 * so no real compiler ever sees it and the shipped code keeps the C23
 * keyword. The `#define` is unconditional on purpose: guarding it behind
 * `#ifdef __CPPCHECK__` would make cppcheck explore the guard both ways and
 * re-surface the false positives in the undefined configuration, and passing
 * `-Dnullptr=NULL` on the command line would disable cppcheck's automatic
 * configuration exploration (which `src/`, `libs/` and the examples rely on
 * for their `#ifdef`-selected build variants). A force-included header is the
 * only mechanism that reaches every configuration without collapsing them.
 *
 * @note Never `#include` this from first-party code; it exists solely as the
 *       gate's `--include=` argument.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#pragma once

#define nullptr ((void*)0)
