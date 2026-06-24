/**
 * @file ra_io_fsfmt.h
 * @brief ra_io pluggable filesystem-format registry (probe + capabilities).
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * A small registry that lets the fabric recognise the on-disk filesystem on a
 * block device and report what that format can do, without the upper layers
 * hard-coding a `switch` over FAT vs exFAT. Each format provides a `probe`
 * (does this volume look like me?) and a capability descriptor (read-only?
 * sub-directories? streaming writes? name length?). FAT and exFAT are
 * registered as built-ins; a foreign format (a future APFS / NTFS / btrfs / ZFS
 * reader, or a test stub) registers through ::ra_io_fsfmt_register with no
 * change to the existing code -- that is the pluggability seam.
 *
 * The capability flags are how the fabric degrades gracefully instead of
 * failing: a caller can check `caps.supports_mkdir` before attempting one, and
 * an operation a format lacks returns ::k_ra_err_not_supported rather than
 * corrupting the volume.
 *
 * @code
 * (void)ra_io_fsfmt_init();
 * const ra_io_fsfmt_t* fmt = nullptr;
 * if (ra_io_fsfmt_probe(&backend, &fmt) == k_ra_ok) {
 *   // fmt->name e.g. "fat"; fmt->caps.supports_streaming_write etc.
 * }
 * @endcode
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra_err.h"
#include "ra_fs.h"

/**
 * @enum ra_io_fsfmt_limits_t
 * @brief Registry sizing.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra_io_fsfmt_max = 8, /**< Max registered formats (built-ins + foreign). */
} ra_io_fsfmt_limits_t;

/**
 * @struct ra_io_fsfmt_caps_t
 * @brief What a filesystem format supports.
 *
 * @details Lets a caller branch on a format's abilities (and lets the VFS
 *          return ::k_ra_err_not_supported for an unsupported op) instead of
 *          assuming FAT semantics everywhere.
 *
 * @since 0.1.0
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint16_t max_name_len;             /**< Longest file name (incl NUL).        */
  bool     read_only;                /**< true => no writes/creates/erases.    */
  bool     supports_mkdir;           /**< true => sub-directory creation.      */
  bool     supports_streaming_write; /**< true => open-for-write streaming.    */
  bool     case_sensitive;           /**< true => names compare case-sensitive.*/
} ra_io_fsfmt_caps_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @typedef ra_io_fsfmt_probe_fn_t
 * @brief Returns true if `backend`'s volume looks like this format.
 *
 * @param[in] backend Block-device backend to inspect (reads block 0 etc.).
 * @return true when the on-disk signature matches this format.
 * @since 0.1.0
 */
typedef bool (*ra_io_fsfmt_probe_fn_t)(const ra_fs_backend_t* backend);

/**
 * @struct ra_io_fsfmt_t
 * @brief One registered filesystem format.
 *
 * @details A format is a `const` instance the caller (or a built-in) exports;
 *          the registry holds pointers to them, so they must out-live it.
 *
 * @invariant `name` and `probe` are non-NULL.
 *
 * @since 0.1.0
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  const char*            name;  /**< Short format name, e.g. "fat" / "exfat". */
  ra_io_fsfmt_caps_t     caps;  /**< What the format supports.                */
  ra_io_fsfmt_probe_fn_t probe; /**< On-disk signature test.                  */
} ra_io_fsfmt_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @brief Reset the registry and register the built-in FAT + exFAT formats.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok Registry holds the two built-ins.
 *
 * @pre None.
 * @pre No probe is in flight.
 * @post Only the built-in formats are registered.
 * @post Foreign formats must be re-registered after this call.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_io_fsfmt_init(void);

/**
 * @brief Register a filesystem format (the foreign-format seam).
 *
 * @param[in] fmt Format descriptor (a const instance that out-lives the registry).
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok              Format registered.
 * @retval k_ra_err_null_ptr    `fmt`, `fmt->name`, or `fmt->probe` was NULL.
 * @retval k_ra_err_no_mem      The registry is full.
 *
 * @pre `fmt` and its members out-live the registry.
 * @pre The registry has a free slot.
 * @post `fmt` participates in subsequent probes (lowest priority -- last).
 * @post On any non-ok return the registry is unchanged.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_io_fsfmt_register(const ra_io_fsfmt_t* fmt);

/**
 * @brief Detect the format of a volume by probing registered formats in order.
 *
 * @param[in]  backend Block-device backend to inspect.
 * @param[out] out     Set to the first matching format.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok            A format matched; `*out` is set.
 * @retval k_ra_err_null_ptr  `backend` or `out` was NULL.
 * @retval k_ra_err_not_found No registered format claimed the volume.
 *
 * @pre At least the built-ins are registered (call ::ra_io_fsfmt_init first).
 * @pre `out` is writable.
 * @post On success `*out` points at a registered format.
 * @post On any non-ok return `*out` is untouched.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_io_fsfmt_probe(const ra_fs_backend_t* backend, const ra_io_fsfmt_t** out);

#ifdef __cplusplus
}
#endif
