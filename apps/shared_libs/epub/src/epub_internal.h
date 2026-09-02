/**
 * @file epub_internal.h
 * @brief Test-access surface for epub internal helpers (MC/DC).
 * @ingroup grp_ereader
 *
 * @details
 * Not part of the public API. Tests under tests/ MAY include this
 * header to drive compound boolean decisions that sit in TU-private
 * helpers behind the public epub facade. See CLAUDE.md
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

#include "epub.h"
#include "miniz.h"
#include "ra8_attributes.h"
#include "ra8_err.h"

/**
 * @brief Read one bounded span from resident EPUB media.
 * @details Test-access form of the callback used by the resident ZIP and
 * decompression preflight paths; production callers keep using the public EPUB
 * facade.
 * @param[in] ctx Bound ::epub_mem_media_t descriptor.
 * @param[in] offset Absolute archive offset.
 * @param[out] buf Destination for exactly @p len bytes.
 * @param[in] len Requested byte count.
 * @return Number of bytes copied, or zero when any guard rejects the request.
 * @retval 0 One guard rejected the request.
 * @retval len Exactly the requested bytes were copied.
 * @pre Non-null arguments address their documented extents.
 * @pre The resident media outlives the call.
 * @post Success copies exactly @p len bytes.
 * @post Rejection does not modify the destination.
 * @note Test-access only; performs no allocation.
 * @par MC/DC:
 * The four-condition OR rejects null media, null output, an offset past the
 * archive, and a length beyond the remaining suffix. Its N+1 vectors are in
 * test_epub_chapter.c.
 * @since 0.1.0
 */
RA8_PRIV size_t priv_epub_mem_read(void* ctx, uint64_t offset, void* buf, size_t len);

/**
 * @brief Copy the directory prefix of an EPUB package path.
 * @details Test-access form of the helper used while binding the OPF base
 * directory. Production callers keep using the public EPUB facade.
 * @param[in] path NUL-terminated package path.
 * @param[out] dst Destination for the directory prefix.
 * @param[in] cap Capacity of @p dst.
 * @pre When non-null, @p path points to a NUL-terminated string.
 * @pre A nonzero @p cap means @p dst is writable for @p cap bytes.
 * @post A destination with nonzero capacity is NUL-terminated.
 * @post A null destination or zero capacity causes no write.
 * @note Test-access only and not thread-safe; performs no allocation.
 * @par MC/DC:
 * The destination-null / zero-capacity OR has one-invalid-at-a-time and
 * all-valid vectors in test_epub_open.c.
 * @since 0.1.0
 */
RA8_PRIV void priv_epub_dirname(const char* path, char* dst, size_t cap);

/**
 * @brief Forward one bounded miniz read to streamed EPUB media.
 * @details Test-access form of the callback installed in a streamed ZIP
 * reader. Production callers use ::epub_open_streamed.
 * @param[in] opaque Bound ::epub_stream_media_t descriptor.
 * @param[in] file_ofs Absolute archive offset.
 * @param[out] buf Destination for up to @p n bytes.
 * @param[in] n Requested byte count.
 * @return Number of bytes supplied by the media callback, or zero on rejection.
 * @retval 0 The descriptor or callback was null, or the offset reached EOF.
 * @retval n The backing callback supplied the full bounded request.
 * @retval <n The backing callback supplied a short read.
 * @pre When non-null, @p opaque points to a live ::epub_stream_media_t.
 * @pre When non-null, @p buf is writable for @p n bytes.
 * @post At most @p n bytes are written to @p buf.
 * @post A null descriptor or callback does not invoke backing media.
 * @note Test-access only and not thread-safe; performs no allocation.
 * @par MC/DC:
 * The descriptor-null / callback-null OR has one-null-at-a-time and all-valid
 * vectors in test_epub_open.c.
 * @since 0.1.0
 */
RA8_PRIV size_t priv_epub_stream_read(void* opaque, mz_uint64 file_ofs, void* buf, size_t n);

/**
 * @brief Finish parsing one initialized ZIP reader into an EPUB book.
 * @details Test-access form of the common resident/streamed open tail.
 * Production callers keep using ::epub_open or ::epub_open_streamed.
 * @param[in,out] zip Initialized miniz reader.
 * @param[in,out] out_book Zeroed destination book.
 * @return Parse result or argument-validation error.
 * @retval k_ra8_ok The archive parsed and the book became live.
 * @retval k_ra8_err_null_ptr One required object was null.
 * @retval k_ra8_err_decomp_entries The archive exceeded the entry-count policy.
 * @retval k_ra8_err_decomp_output_cap An entry exceeded the output-size policy.
 * @retval k_ra8_err_decomp_ratio An entry exceeded the compression-ratio policy.
 * @retval k_ra8_err_not_found A required EPUB package entry was absent.
 * @retval k_ra8_err_no_mem A required entry or spine exceeded fixed storage.
 * @retval k_ra8_err_invalid_size A required XML document was empty.
 * @retval k_ra8_err_validation_failed The archive or package metadata was invalid.
 * @pre When non-null, @p zip is an initialized miniz reader.
 * @pre When non-null, @p out_book points to zeroed writable storage.
 * @post Success marks @p out_book live.
 * @post A parse failure destroys the ZIP reader without marking the book live.
 * @note Test-access only; not thread-safe because parsing uses shared scratch.
 * @par MC/DC:
 * The ZIP-null / book-null OR has one-null-at-a-time vectors in
 * test_epub_open.c and all-valid vectors in test_epub_open_cov.c.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_epub_finish_open(mz_zip_archive* zip, epub_book_t* out_book);

/**
 * @brief Bind one ZIP reader to a book's caller-owned miniz arena.
 * @details Test-access form of the shared resident/streamed open helper. It
 * initializes the embedded arena and installs all three allocation callbacks
 * plus their opaque context.
 * @param[in,out] zip Zeroed archive descriptor to configure.
 * @param[in,out] book Book owning the arena and workspace.
 * @return Arena initialization or argument-validation status.
 * @retval k_ra8_ok The arena and callbacks were installed.
 * @retval k_ra8_err_null_ptr One required object was null.
 * @pre Non-null objects are writable and distinct.
 * @pre @p zip has not entered a miniz reader mode.
 * @post Success binds every allocator callback to @p book.
 * @post Null rejection mutates neither candidate object.
 * @note Test-access only; the public EPUB ABI is unchanged.
 * @par MC/DC:
 * The two-condition null OR has one-null-at-a-time and all-valid vectors in
 * test_epub_chapter.c.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_epub_set_miniz_alloc(mz_zip_archive* zip, epub_book_t* book);

/**
 * @brief Concatenate ``dir`` and ``name`` into ``dst``, NUL-terminated.
 *
 * @details Promoted from TU-private static linkage so tests can drive
 *          its line-76, line-82, line-89 compound decisions on the
 *          production source under -fcoverage-mcdc. Production callers
 *          MUST keep using the public epub facade.
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
 *          apps/shared_libs/epub/src/epub_chapter.c inside
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
 *          apps/shared_libs/epub/src/epub_chapter.c lines 300 and 369
 *          (inside @c epub_load_chapter and
 *          @c epub_get_cover_image / @c epub_get_metadata).
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
 *          open paths funnel through `priv_epub_finish_open`).
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
