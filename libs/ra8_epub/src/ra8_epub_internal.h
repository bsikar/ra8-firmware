/**
 * @file ra8_epub_internal.h
 * @brief Test-access surface for ra8_epub internal helpers (MC/DC).
 * @ingroup grp_ereader
 *
 * @details
 * Not part of the public API. Tests under tests/ MAY include this
 * header to drive compound boolean decisions that sit in TU-private
 * helpers behind the public ra8_epub facade. See CLAUDE.md
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

#include "miniz.h"
#include "ra8_attributes.h"
#include "ra8_err.h"

/**
 * @brief Concatenate ``dir`` and ``name`` into ``dst``, NUL-terminated.
 *
 * @details Promoted from TU-private static linkage so tests can drive
 *          its line-76, line-82, line-89 compound decisions on the
 *          production source under -fcoverage-mcdc. Production callers
 *          MUST keep using the public ra8_epub facade.
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
RA8_PRIV void priv_epub_join_path(const char* dir, const char* name, char* dst, size_t cap);

/**
 * @brief Pure predicate: width OR height is negative.
 *
 * @details Promoted from the inline OR at
 *          libs/ra8_epub/src/ra8_epub_chapter.c inside
 *          @c internal_font_init.
 *
 * @param[in] w Glyph bbox width.
 * @param[in] h Glyph bbox height.
 *
 * @return Boolean reject predicate.
 * @retval true  Caller must return @c k_ra8_err_validation_failed.
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
RA8_PRIV bool priv_epub_glyph_dim_invalid(int w, int h);

/**
 * @brief Pure predicate: book unused OR zip archive inactive.
 *
 * @details Promoted from the inline OR at
 *          libs/ra8_epub/src/ra8_epub_chapter.c lines 300 and 369
 *          (inside @c ra8_epub_load_chapter and
 *          @c ra8_epub_get_cover_image / @c ra8_epub_get_metadata).
 *
 * @param[in] in_use              ``book->in_use`` byte (0 == unused).
 * @param[in] zip_archive_active  ``book->zip_archive_active`` byte (0 == inactive).
 *
 * @return Boolean reject predicate.
 * @retval true  Caller must return @c k_ra8_err_not_initialized.
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
RA8_PRIV bool priv_epub_book_not_ready(uint8_t in_use, uint8_t zip_archive_active);

/**
 * @brief Guard a just-opened ZIP archive against the decompression policy.
 *
 * @details The archive-level half of the unified decompression-limits
 *          retrofit (`ra8_decomp_limits.h`): rejects an archive whose
 *          central directory enumerates more entries than the default
 *          policy's `max_entries` -- the many-tiny-entries resource bomb
 *          -- before any entry is touched. Called once per
 *          `mz_zip_reader_init*` success (both the in-memory and streamed
 *          open paths funnel through `internal_finish_open`).
 *
 * @param[in] zip Initialised miniz reader (non-NULL).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                 The entry count is within policy.
 * @retval k_ra8_err_null_ptr       @p zip was NULL.
 * @retval k_ra8_err_decomp_entries The central directory exceeds the cap.
 *
 * @pre @p zip was initialised by an `mz_zip_reader_init*` call.
 * @pre The default decompression policy is in force (no per-book override).
 * @post No archive state is modified (pure count check).
 * @post On breach the caller must destroy the reader (fail-closed).
 *
 * @note Thread-safe: pure read of the reader's entry count.
 * @see priv_epub_zip_guard_entry()
 * @since 0.1.0
 */
RA8_PRIV [[nodiscard]] ra8_err_t priv_epub_zip_guard_archive(mz_zip_archive* zip);

/**
 * @brief Guard one ZIP entry's declared sizes against the policy.
 *
 * @details The entry-level half of the retrofit: rejects an entry whose
 *          central-directory record declares an uncompressed size over the
 *          default policy's per-unit output cap, or over the
 *          compression-ratio bound relative to its compressed size (the
 *          lying-header / decompression-bomb signatures) -- before any
 *          inflation starts. Called after every successful
 *          `mz_zip_reader_file_stat` that precedes an extraction.
 *
 * @param[in] st The entry's stat record (non-NULL).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                    Declared sizes are within policy.
 * @retval k_ra8_err_null_ptr          @p st was NULL.
 * @retval k_ra8_err_decomp_output_cap Declared output exceeds the cap.
 * @retval k_ra8_err_decomp_ratio      Declared output breaks the ratio.
 *
 * @pre @p st came from a successful `mz_zip_reader_file_stat`.
 * @pre The default decompression policy is in force.
 * @post No state is modified (pure check).
 * @post On breach the caller must not extract the entry (fail-closed).
 *
 * @note Thread-safe: pure read.
 * @see priv_epub_zip_guard_archive()
 * @since 0.1.0
 */
RA8_PRIV [[nodiscard]] ra8_err_t priv_epub_zip_guard_entry(const mz_zip_archive_file_stat* st);

#ifdef __cplusplus
}
#endif
