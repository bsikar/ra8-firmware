/**
 * @file ra_epub_internal.h
 * @brief Test-access surface for ra_epub internal helpers (MC/DC).
 *
 * @details
 * Not part of the public API. Tests under tests/ MAY include this
 * header to drive compound boolean decisions that sit in TU-private
 * helpers behind the public ra_epub facade. See CLAUDE.md
 * "Test access to internal symbols (MC/DC scope)".
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

/**
 * @brief Concatenate ``dir`` and ``name`` into ``dst``, NUL-terminated.
 *
 * @details Promoted from TU-private static linkage so tests can drive
 *          its line-76, line-82, line-89 compound decisions on the
 *          production source under -fcoverage-mcdc. Production callers
 *          MUST keep using the public ra_epub facade.
 *
 * @param[in]  dir  Optional directory prefix (NUL-terminated, may be NULL).
 * @param[in]  name Optional name suffix (NUL-terminated, may be NULL).
 * @param[out] dst  Destination buffer (may be NULL when ``cap`` is 0).
 * @param[in]  cap  Capacity of ``dst`` in bytes.
 *
 * @pre cap == 0 implies dst may be NULL.
 * @pre cap > 0 implies dst is non-NULL and writeable for ``cap`` bytes.
 * @post dst is NUL-terminated when cap > 0.
 * @post No more than (cap - 1) bytes written from inputs.
 *
 * @note Test-access only.
 *
 * @par MC/DC:
 * Drives the line-76 ``(dst == NULL || cap == 0U)`` OR and the two
 * loop ANDs at lines 82 and 89.
 *
 * @since 0.1.0
 */
void ra_epub_internal_join_path(const char* dir, const char* name, char* dst, size_t cap);

#ifdef __cplusplus
}
#endif
