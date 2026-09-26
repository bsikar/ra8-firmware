/**
 * @file ra8_path.h
 * @brief Untrusted-name policy: sanitise a segment, join it, prove containment.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 2 / Interface] {World: Any}
 *
 * @details
 * Anything that writes a file whose name it did not choose needs one answer to
 * "is this untrusted name safe to place under this directory?". The archive
 * readers produce such names -- `ra8_unarch_tar_next()` hands back a pax
 * `path` override, a GNU longname or a prefix-joined ustar name, clamped to
 * capacity and otherwise uninspected -- and until now the only implementation
 * of the policy lived in a product application, unreachable from `libs/`.
 *
 * Three predicates, in the order a caller uses them:
 *
 * 1. ::ra8_path_sanitize_segment rewrites one untrusted candidate into a name
 *    that is always a single filesystem-safe segment.
 * 2. ::ra8_path_join_under composes that segment under a parent and refuses,
 *    rather than composes, anything that is not a single safe segment.
 * 3. ::ra8_path_contained is the standalone lexical predicate a caller pairs
 *    with its own path resolution when it has a resolved absolute candidate.
 *
 * The bound on every path here is ::k_fw_fs_path_cap, the portable path cap
 * this interface tier already publishes, so a caller sizing a buffer for the
 * filesystem facade has sized it for this policy too.
 *
 * @code
 * char leaf[k_fw_fs_path_cap];
 * bool verbatim = false;
 * (void)ra8_path_sanitize_segment(member_name, leaf, sizeof(leaf), &verbatim);
 *
 * char dest[k_fw_fs_path_cap];
 * if (ra8_path_join_under("/books/incoming", leaf, dest, sizeof(dest)) == k_ra8_ok) {
 *   // dest cannot name anything outside /books/incoming
 * }
 * @endcode
 *
 * @note These are pure predicates: they touch no filesystem and resolve no
 *       symlink. A caller that must defeat a symlink already planted in the
 *       destination tree resolves the path itself and then applies
 *       ::ra8_path_contained to the resolved result.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "fw_if_fs_types.h"
#include "ra8_err.h"

/**
 * @enum ra8_path_limits_t
 * @brief Hard bounds the name policy enforces before it writes anything.
 *
 * @details
 * The segment minimum is the smallest capacity in which a useful result can
 * exist at all: one character plus its NUL. Below it the policy refuses rather
 * than emitting a name that is only a terminator.
 *
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_ra8_path_segment_cap_min = 2U,                /**< Smallest useful segment capacity. */
  k_ra8_path_cap             = k_fw_fs_path_cap,  /**< Portable path cap, incl. the NUL. */
} ra8_path_limits_t;

/**
 * @brief Rewrite an untrusted candidate into one filesystem-safe segment.
 *
 * @details
 * Characters outside `[A-Za-z0-9._-]` -- including `/`, NUL and every control
 * byte -- become `_`. A result that is empty, `.` or `..` is replaced by the
 * generated fallback name. A Windows reserved device base (`CON`, `NUL`,
 * `AUX`, `PRN`, `COM1`..`COM9`, `LPT1`..`LPT9`) is prefixed with `_`. Input
 * longer than `cap - 1` bytes is truncated. Because the result can contain no
 * `/` and is never `.` or `..`, joining it under a parent cannot escape that
 * parent.
 *
 * `out_verbatim` reports whether the input survived untouched, so a caller can
 * log that it renamed an entry. It is optional: pass `nullptr` to ignore it.
 *
 * @param[in]  raw          Untrusted candidate name (NUL-terminated), or NULL.
 * @param[out] out          Destination buffer for the sanitised segment.
 * @param[in]  cap          Capacity of `out` in bytes.
 * @param[out] out_verbatim Optional: receives true when `out` equals `raw`.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               `out` holds a safe single segment.
 * @retval k_ra8_err_null_ptr     `out` was NULL.
 * @retval k_ra8_err_invalid_size `cap` was below ::k_ra8_path_segment_cap_min.
 *
 * @pre `out` addresses at least `cap` writable bytes.
 * @pre `out` does not alias `raw`.
 * @post On success `out` is NUL-terminated, non-empty, free of `/`, and is
 *       neither `.` nor `..`.
 * @post On any non-ok return no caller storage is written.
 *
 * @note Thread-safe: writes only caller-provided storage.
 * @see ra8_path_join_under Composes the segment this produces.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_path_sanitize_segment(const char* raw, char* out, size_t cap, bool* out_verbatim);

/**
 * @brief Join one safe child segment under a parent directory path.
 *
 * @details
 * The defence-in-depth gate: it refuses -- rather than composes -- a `seg`
 * that is not a single filesystem-safe segment. An empty string, `.`, `..`,
 * and any name containing a `/` (which also rejects an absolute `seg` such as
 * `/etc/passwd`) are all refused. A result that would not fit `out` is refused
 * too, since a silently shortened path names a different file.
 *
 * `seg` is expected to be the output of ::ra8_path_sanitize_segment or a leaf
 * composed from one; this call makes an escape structurally impossible even if
 * that upstream step regressed.
 *
 * @param[in]  parent Parent directory path (NUL-terminated), or NULL.
 * @param[in]  seg    Candidate child segment (NUL-terminated), or NULL.
 * @param[out] out    Destination buffer receiving `parent` + `/` + `seg`.
 * @param[in]  cap    Capacity of `out` in bytes.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               `out` holds the joined path.
 * @retval k_ra8_err_null_ptr     `parent`, `seg` or `out` was NULL.
 * @retval k_ra8_err_invalid_size `cap` was zero.
 * @retval k_ra8_err_invalid_arg  `seg` was empty, `.`, `..`, or `/`-bearing.
 * @retval k_ra8_err_no_mem       The joined path does not fit `cap`.
 *
 * @pre `out` addresses at least `cap` writable bytes.
 * @pre `out` does not alias `parent` or `seg`.
 * @post On success `out` contains exactly one `/` more than `parent` did.
 * @post On any non-ok return with `cap > 0`, `out[0]` is `'\0'`: there is no
 *       usable partial path for a caller that ignored the return value.
 *
 * @note Thread-safe: writes only caller-provided storage.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_path_join_under(const char* parent, const char* seg, char* out, size_t cap);

/**
 * @brief Report whether `candidate` is lexically contained under `parent`.
 *
 * @details
 * A prefix test that treats a directory boundary as significant, so `/a/b`
 * contains `/a/b` and `/a/b/c` but not `/a/bb`. Trailing slashes on `parent`
 * are ignored. The comparison is purely lexical: both paths are expected to be
 * already resolved, with no `..` component left in either, which is the
 * caller's job because resolution needs a filesystem and this tier has none.
 *
 * @param[in]  parent        Resolved parent directory path, or NULL.
 * @param[in]  candidate     Resolved candidate path, or NULL.
 * @param[out] out_contained Receives the verdict.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Verdict reported in `*out_contained`.
 * @retval k_ra8_err_null_ptr    Any argument was NULL.
 * @retval k_ra8_err_invalid_arg `parent` was empty or only slashes.
 *
 * @pre `parent` and `candidate` are NUL-terminated.
 * @pre The caller resolved both paths, so no `..` component remains.
 * @post Neither input string is modified.
 * @post On any non-ok return `*out_contained` is not written.
 *
 * @note Thread-safe: depends only on its arguments.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_path_contained(const char* parent, const char* candidate, bool* out_contained);

#ifdef __cplusplus
}
#endif
