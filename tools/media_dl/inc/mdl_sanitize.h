/**
 * @file mdl_sanitize.h
 * @brief Neutralise untrusted names before they reach a filesystem or XML sink.
 *
 * @details
 * Chapter and series names come from attacker-controlled HTML (the last path
 * segment of a scraped URL, an anchor's text). Left raw they flow into two
 * dangerous sinks: filesystem paths (a `..` segment escapes the output tree)
 * and generated XML (an unescaped `<`/`&`/`"` in a page filename breaks the
 * EPUB this tool feeds back to the on-device reader). This module holds the
 * three pure predicates that close those holes, kept out of the CLI's
 * translation unit so they are unit-testable both directions.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Rewrite an untrusted path segment into a safe single filename.
 *
 * @details
 * Produces a non-empty NUL-terminated name in `out` that is always a single
 * filesystem-safe segment: characters outside `[A-Za-z0-9._-]` (including `/`,
 * NUL, and control bytes) become `_`, the traversal names `.` and `..` and an
 * empty result fall back to a generated name, a Windows reserved device name
 * (`CON`, `NUL`, `COM1`..`LPT9`, ...) is prefixed with `_`, and any input
 * longer than `cap - 1` bytes is truncated. Because the result can contain no
 * `/` and is never `.`/`..`, joining it under a parent directory cannot escape
 * that directory.
 *
 * @param[in]  raw Untrusted candidate name (NUL-terminated), or NULL.
 * @param[out] out Destination buffer for the sanitised name.
 * @param[in]  cap Capacity of `out` in bytes (must be >= 2 for a useful name).
 *
 * @return Whether `raw` was already fully safe (copied verbatim).
 * @retval true  `out` equals `raw`: no substitution, fallback, or truncation.
 * @retval false `out` was rewritten -- the caller may log the change.
 *
 * @pre `out`, when the call is useful, has room for at least `cap` bytes.
 * @pre `cap >= 2` so at least one character plus a NUL fits.
 * @post `out` is NUL-terminated and contains no `/`, `.`-only, or `..` result.
 * @post `out` is non-empty whenever `cap >= 2`.
 *
 * @note Thread-safe: writes only caller-provided storage.
 * @since 0.1.0
 */
bool mdl_sanitize_segment(const char* raw, char* out, size_t cap);

/**
 * @brief True when `candidate` is lexically contained under `parent`.
 *
 * @details
 * A prefix test that treats a directory boundary as significant, so
 * `/a/b` contains `/a/b` and `/a/b/c` but not `/a/bb`. Both paths are expected
 * to be already resolved (e.g. via `realpath`) so the comparison is purely
 * lexical; trailing slashes on `parent` are ignored.
 *
 * @param[in] parent    Absolute, resolved parent directory, or NULL.
 * @param[in] candidate Absolute, resolved candidate path, or NULL.
 *
 * @return Whether `candidate` is `parent` itself or a descendant of it.
 * @retval true  `candidate` equals `parent` or lies beneath it.
 * @retval false Either argument is NULL/empty, or `candidate` is outside.
 *
 * @pre `parent` and `candidate`, when non-NULL, are NUL-terminated strings.
 * @pre The caller resolved both paths before calling (no `..` remains).
 * @post Neither argument is modified.
 *
 * @note Thread-safe: depends only on its arguments.
 * @since 0.1.0

 * @post Documented outputs and the return value describe the same outcome.
 */
bool mdl_path_contained(const char* parent, const char* candidate);

/**
 * @brief Join one safe child segment under a parent directory path.
 *
 * @details
 * The single join primitive every `series_dir`/`chapter_dir` join routes
 * through, so a traversal name can never reach `mkdir`, an archiver, or an
 * output path. It refuses -- rather than composing -- a `seg` that is not a
 * single filesystem-safe segment: an empty string, `.` or `..`, or any name
 * containing a `/` (which also rejects an absolute `seg` such as `/etc`). A
 * result that would not fit `out` is likewise refused rather than truncated,
 * since a silently-shortened path would name a different directory. `seg` is
 * expected to already be the output of ::mdl_sanitize_segment (or a leaf
 * composed from such a slug); this predicate is the defence-in-depth gate that
 * makes an escape structurally impossible even if that upstream step regressed.
 *
 * @param[in]  parent Parent directory path (NUL-terminated), or NULL.
 * @param[in]  seg    Candidate child segment (NUL-terminated), or NULL.
 * @param[out] out    Destination buffer receiving `parent/seg`.
 * @param[in]  cap    Capacity of `out` in bytes.
 *
 * @return Whether `out` holds the joined `parent/seg` path.
 * @retval true  `out` is `parent` + `'/'` + `seg`, NUL-terminated.
 * @retval false A NULL/zero argument, an unsafe `seg`, or a non-fitting result.
 *
 * @pre `out`, when non-NULL, has room for at least `cap` bytes.
 * @pre The caller treats `false` as a hard error (no partial path is used).
 * @post On `false` with `cap > 0`, `out[0]` is `'\0'` (no usable partial path).
 * @post On `true`, `out` contains exactly one `/` more than `parent` did.
 *
 * @note Thread-safe: writes only caller-provided storage.
 * @see mdl_sanitize_segment  Produces the safe `seg` this join expects.
 * @see mdl_path_contained    Runtime realpath check callers pair with this.
 * @since 0.1.0
 */
bool mdl_path_join(const char* parent, const char* seg, char* out, size_t cap);

/**
 * @brief XML-escape `src` into `out`, failing rather than truncating.
 *
 * @details
 * Replaces the five XML metacharacters (`&`, `<`, `>`, `"`, `'`) with their
 * predefined entities and copies everything else verbatim. Applied to every
 * untrusted filename interpolated into the OPF, nav document and per-page
 * XHTML so a page named `a"><script>.jpg` cannot break the container's
 * well-formedness. If the escaped result would not fit, the function fails
 * instead of emitting a truncated (and possibly malformed) document.
 *
 * @param[in]  src Source string (NUL-terminated), or NULL.
 * @param[out] out Destination buffer for the escaped, NUL-terminated result.
 * @param[in]  cap Capacity of `out` in bytes.
 *
 * @return Whether the fully escaped string fit in `out`.
 * @retval true  `out` holds the complete escaped form of `src`.
 * @retval false `src`/`out` was NULL, `cap` was 0, or the result did not fit.
 *
 * @pre `out`, when non-NULL, has room for at least `cap` bytes.
 * @pre The caller treats `false` as a hard error (no partial output is used).
 * @post On `false` with `cap > 0`, `out[0]` is `'\0'`.
 * @post `src` is not modified.
 *
 * @note Thread-safe: writes only caller-provided storage.
 * @since 0.1.0
 */
bool mdl_xml_escape(const char* src, char* out, size_t cap);
