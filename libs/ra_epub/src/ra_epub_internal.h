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
#include <stdint.h>

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

/**
 * @brief Pure predicate: width OR height is negative.
 *
 * @details Promoted from the inline OR at
 *          libs/ra_epub/src/ra_epub_chapter.c inside
 *          @c priv_font_init.
 *
 * @param[in] w Glyph bbox width.
 * @param[in] h Glyph bbox height.
 *
 * @return Boolean reject predicate.
 * @retval true  Caller must return @c k_ra_err_validation_failed.
 * @retval false Both dimensions are non-negative.
 *
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on the two inputs.
 *
 * @note Test-access only. Pure function.
 *
 * @par MC/DC:
 * 2-condition OR; N+1 = 3 vectors:
 *  - w>=0, h>=0 -> false
 *  - w<0,  h>=0 -> true (varies left)
 *  - w>=0, h<0  -> true (varies right)
 *
 * @since 0.1.0
 */
bool ra_epub_internal_glyph_dim_invalid(int w, int h);

/**
 * @brief Pure predicate: book unused OR zip archive inactive.
 *
 * @details Promoted from the inline OR at
 *          libs/ra_epub/src/ra_epub_chapter.c lines 300 and 369
 *          (inside @c ra_epub_load_chapter and
 *          @c ra_epub_get_cover_image / @c ra_epub_get_metadata).
 *
 * @param[in] in_use              ``book->in_use`` byte (0 == unused).
 * @param[in] zip_archive_active  ``book->zip_archive_active`` byte (0 == inactive).
 *
 * @return Boolean reject predicate.
 * @retval true  Caller must return @c k_ra_err_not_initialized.
 * @retval false Book is ready.
 *
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on the two inputs.
 *
 * @note Test-access only. Pure function.
 *
 * @par MC/DC:
 * 2-condition OR; N+1 = 3 vectors:
 *  - in_use=1, zip=1 -> false
 *  - in_use=0, zip=1 -> true (varies left)
 *  - in_use=1, zip=0 -> true (varies right)
 *
 * @since 0.1.0
 */
bool ra_epub_internal_book_not_ready(uint8_t in_use, uint8_t zip_archive_active);

#ifdef __cplusplus
}
#endif
