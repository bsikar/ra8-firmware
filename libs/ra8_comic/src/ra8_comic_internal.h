/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_comic_internal.h
 * @brief Cross-TU seams between the comic facade and its CBZ / CBR backends.
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * `ra8_comic.c` owns the public API, the shared page-index storage, the entry-name
 * image filter, and the name-sort. Each container backend -- `ra8_comic_cbz.c`
 * (miniz ZIP) and `ra8_comic_cbr.c` (RAR via `ra8_rar.h`) -- enumerates its image
 * members by calling ::ra8_comic_page_add and extracts one page through its own
 * `*_extract`. This header is the internal contract between them; it is not part
 * of the public `ra8_comic.h` surface.
 *
 * @note Not thread-safe; the single-threaded reader loop serialises access.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_comic.h"
#include "ra8_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Whether an archive entry name is a decodable comic page image.
 * @details A page qualifies when its base name is not a dotfile (skips hidden
 *          entries and macOS `__MACOSX/._*` AppleDouble resource forks) and it
 *          ends in an image extension the on-device decoder (`stb_image`) handles:
 *          `.jpg`, `.jpeg`, `.png`, `.gif`, `.bmp` (case-insensitive). Both `/`
 *          and `\\` are treated as path separators for the base-name test.
 * @param[in] name Entry name bytes (not NUL-terminated; @p len is authoritative).
 * @param[in] len  Length of @p name in bytes.
 * @return Whether @p name is a page image to index.
 * @retval true  @p name is a decodable, non-hidden page image.
 * @retval false @p name is hidden, a resource fork, or not a decodable image.
 * @pre @p name addresses at least @p len readable bytes (or @p len is 0).
 * @pre @p len is the true entry-name length.
 * @post No state is modified (pure read).
 * @post The result depends only on @p name / @p len.
 * @note Thread-safe: pure read.
 * @since Version 0.1.0
 */
RA8_PRIV bool ra8_comic_is_page_name(const char* name, uint16_t len);

/**
 * @brief Append one image member to the resident page index.
 * @details Copies @p name into the comic's name arena and fills the next
 *          ::ra8_comic_page_t slot with the backend bookkeeping. Rejects an index
 *          or arena overflow so no page is silently dropped.
 * @param[in,out] c           Comic accumulating its page index.
 * @param[in]     name        Entry name bytes.
 * @param[in]     name_len    Length of @p name in bytes.
 * @param[in]     raw_size    Encoded image length in bytes.
 * @param[in]     extractable 1 if this reader can decode the page, else 0.
 * @param[in]     method      CBR ::ra8_rar_method_t (0 = store); 0 for CBZ.
 * @param[in]     zip_index   CBZ miniz central-directory index (0 for CBR).
 * @param[in]     data_off    CBR member data-area offset (0 for CBZ).
 * @param[in]     pack_size   CBR packed size (0 for CBZ).
 * @return ra8_err_t status.
 * @retval k_ra8_ok               Page appended.
 * @retval k_ra8_err_invalid_size Page index or name arena is full.
 * @pre @p c has its `pages` / `names` buffers bound.
 * @pre @p name addresses @p name_len readable bytes.
 * @post On k_ra8_ok, `c->page_count` and `c->names_len` advanced by one entry.
 * @post On any error the index and arena are unchanged.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_PRIV ra8_err_t ra8_comic_page_add(ra8_comic_t* c,
                                      const char*  name,
                                      uint16_t     name_len,
                                      uint64_t     raw_size,
                                      uint8_t      extractable,
                                      uint8_t      method,
                                      uint32_t     zip_index,
                                      uint64_t     data_off,
                                      uint64_t     pack_size);

/**
 * @brief Open the CBZ (ZIP) backend: init the streaming miniz reader + page index.
 * @details Binds miniz's user-read ZIP reader to the comic's seek+read backing,
 *          walks the central directory, and appends each image entry via
 *          ::ra8_comic_page_add. On firmware, miniz allocations route through the
 *          `ra8_epub_miniz_alloc` static pool (no heap).
 * @param[in,out] c Comic with backing + buffers bound, `kind == cbz`.
 * @return ra8_err_t status.
 * @retval k_ra8_ok                Central directory walked; index populated.
 * @retval k_ra8_err_validation_failed miniz could not open the ZIP.
 * @retval k_ra8_err_invalid_size  The page index / name arena was too small.
 * @pre `c->read` / `c->size` / `c->pages` / `c->names` are bound.
 * @pre `c->stream` is available for the miniz I/O opaque pointer.
 * @post On k_ra8_ok, `c->zip_active == 1` and the index holds every image entry.
 * @post On any error the miniz reader is torn down.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_PRIV ra8_err_t ra8_comic_cbz_open(ra8_comic_t* c);

/**
 * @brief Extract one CBZ page's encoded image through the streaming ZIP reader.
 * @details Inflates the ZIP member named by `p->zip_index` (STORE or DEFLATE) on
 *          demand through miniz, into the caller buffer -- one page of I/O.
 * @param[in]  c    Comic with an active CBZ reader.
 * @param[in]  p    Page-index entry (its `zip_index` selects the ZIP member).
 * @param[out] buf  Destination for the encoded image bytes.
 * @param[in]  cap  Capacity of @p buf; must be >= `p->raw_size`.
 * @param[out] got  Receives the bytes written.
 * @return ra8_err_t status.
 * @retval k_ra8_ok               Page inflated into @p buf.
 * @retval k_ra8_err_no_mem       @p cap is smaller than `p->raw_size`.
 * @retval k_ra8_err_validation_failed miniz could not extract the member.
 * @pre `c->zip_active == 1`; @p p came from this comic's index.
 * @pre @p buf holds @p cap writable bytes.
 * @post On k_ra8_ok, `*got == p->raw_size`.
 * @post On any error `*got == 0`.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_PRIV ra8_err_t ra8_comic_cbz_extract(ra8_comic_t*            c,
                                         const ra8_comic_page_t* p,
                                         uint8_t*                buf,
                                         size_t                  cap,
                                         size_t*                 got);

/**
 * @brief Tear down the CBZ backend's miniz reader.
 * @details Ends the miniz reader (freeing its central directory to the static
 *          pool) when one is active, and clears the active flag; a no-op otherwise.
 * @param[in,out] c Comic whose CBZ reader is to be released.
 * @return ra8_err_t status.
 * @retval k_ra8_ok Always; a non-active reader is a no-op.
 * @pre @p c is a valid comic (possibly already inactive).
 * @pre @p c out-lives the call.
 * @post `c->zip_active == 0`.
 * @post The miniz central directory is freed to the static pool.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_PRIV ra8_err_t ra8_comic_cbz_close(ra8_comic_t* c);

/**
 * @brief Open the CBR (RAR) backend: walk the archive + build the page index.
 * @details Iterates the RAR block chain (`ra8_rar_next`) from `c->rar.first_off`,
 *          appending each image file member via ::ra8_comic_page_add. STORE and (on
 *          a RAR5 archive) compressed members are indexed `extractable == 1`; only a
 *          RAR4-compressed member (legacy codec, unsupported) gets `extractable == 0`.
 * @param[in,out] c Comic with `c->rar` bound by `ra8_rar_open`, `kind == cbr`.
 * @return ra8_err_t status.
 * @retval k_ra8_ok                Archive walked; index populated.
 * @retval k_ra8_err_invalid_size  The page index / name arena was too small.
 * @retval k_ra8_err_*             A RAR-walk error.
 * @pre `c->rar` was bound by `ra8_rar_open`; buffers are bound.
 * @pre `c->size` is the archive length.
 * @post On k_ra8_ok, the index holds every image member in archive order.
 * @post On any error the index is left partially filled (caller discards @p c).
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_PRIV ra8_err_t ra8_comic_cbr_open(ra8_comic_t* c);

/**
 * @brief Extract one CBR page's encoded image (STORE copy or RAR5 decode).
 * @details Routes the page through ::ra8_rar_extract: a STORE member's literal bytes
 *          are streamed, a RAR5-compressed member is inflated by the decompressor
 *          into @p buf. A RAR4-compressed page (indexed non-extractable) is rejected.
 * @param[in]  c    Comic with a bound CBR walker.
 * @param[in]  p    Page-index entry (its `data_off` / `raw_size` / `rar_method`).
 * @param[out] buf  Destination for the encoded image bytes.
 * @param[in]  cap  Capacity of @p buf; must be >= `p->raw_size`.
 * @param[out] got  Receives the bytes written.
 * @return ra8_err_t status.
 * @retval k_ra8_ok                Member copied / decoded into @p buf.
 * @retval k_ra8_err_not_supported The page is a RAR4-compressed member.
 * @retval k_ra8_err_no_mem        @p cap is smaller than `p->raw_size`.
 * @retval k_ra8_err_invalid_size  A STORE member overruns the archive / short read.
 * @retval k_ra8_err_validation_failed A malformed / truncated compressed stream.
 * @pre @p p came from this comic's CBR index.
 * @pre @p buf holds @p cap writable bytes.
 * @post On k_ra8_ok, `*got == p->raw_size`.
 * @post On any error `*got == 0`.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_PRIV ra8_err_t ra8_comic_cbr_extract(ra8_comic_t*            c,
                                         const ra8_comic_page_t* p,
                                         uint8_t*                buf,
                                         size_t                  cap,
                                         size_t*                 got);

/**
 * @brief Open the CBT (tar) backend: walk the archive + build the page index.
 * @details Iterates the tar member chain (`ra8_unarch_tar_next`) from offset 0,
 *          appending each image file member via ::ra8_comic_page_add. tar stores
 *          members verbatim, so every indexed page is `extractable == 1` with
 *          `pack_size == raw_size`. The walk (and each member's declared size)
 *          is charged against the walker's embedded decompression budget.
 * @param[in,out] c Comic with `c->tar` bound by `ra8_unarch_tar_open`,
 *                  `kind == cbt`.
 * @return ra8_err_t status.
 * @retval k_ra8_ok                Archive walked; index populated.
 * @retval k_ra8_err_invalid_size  The page index / name arena was too small.
 * @retval k_ra8_err_decomp_*      The walk breached the decompression policy.
 * @retval k_ra8_err_*             A tar-walk error (malformed header, ...).
 * @pre `c->tar` was bound by `ra8_unarch_tar_open`; buffers are bound.
 * @pre `c->size` is the archive length.
 * @post On k_ra8_ok, the index holds every image member in archive order.
 * @post On any error the index is left partially filled (caller discards @p c).
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_PRIV ra8_err_t ra8_comic_cbt_open(ra8_comic_t* c);

/**
 * @brief Extract one CBT page's encoded image (verbatim tar member copy).
 * @details Streams the member's literal bytes through ::ra8_unarch_tar_read,
 *          which re-validates the (potentially forged) entry bounds against
 *          the archive before any copy.
 * @param[in]  c    Comic with a bound CBT walker.
 * @param[in]  p    Page-index entry (its `data_off` / `raw_size`).
 * @param[out] buf  Destination for the encoded image bytes.
 * @param[in]  cap  Capacity of @p buf; must be >= `p->raw_size`.
 * @param[out] got  Receives the bytes written.
 * @return ra8_err_t status.
 * @retval k_ra8_ok               Member copied into @p buf.
 * @retval k_ra8_err_no_mem       @p cap is smaller than `p->raw_size`.
 * @retval k_ra8_err_invalid_size The member overruns the archive / short read.
 * @pre @p p came from this comic's CBT index.
 * @pre @p buf holds @p cap writable bytes.
 * @post On k_ra8_ok, `*got == p->raw_size`.
 * @post On any error `*got == 0`.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_PRIV ra8_err_t ra8_comic_cbt_extract(ra8_comic_t*            c,
                                         const ra8_comic_page_t* p,
                                         uint8_t*                buf,
                                         size_t                  cap,
                                         size_t*                 got);

#ifdef __cplusplus
}
#endif
