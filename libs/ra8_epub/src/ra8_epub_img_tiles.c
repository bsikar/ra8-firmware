/**
 * @file ra8_epub_img_tiles.c
 * @brief Tile-cache paging of JOF atlases + a real reflow img loader (#231).
 *
 * @details
 * Implements ::ra8_epub_tile_binder (pages JOF tile atlases through
 * ::ra8_tile_cache, decode-on-demand keyed by `image_id + tile`, bounded RAM)
 * and ::ra8_epub_reflow_img_load (resolves an `<img src>` href to encoded
 * bytes in a caller-owned bounded scratch). Atlas sources are either stored
 * archive entries (windowed with `ra8_epub_entry_pread`) or external pread
 * seams (SDRAM/SD stores filled by the import path in
 * `ra8_epub_img_import.c`). Tile decode itself is `ra8_tileatlas_read_tile()`
 * -- one bounded read plus one bounded inflate per miss.
 *
 * Guarded on `__has_include`: the pure `ra8_epub` core (and epub-only apps
 * that never pull in `ra8_mem`/`ra8_tileatlas`) still link with this TU empty.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / EPUB]
 * {World: NS}
 */

/* Compile the binder only where the ra8_mem tile cache and the atlas reader
 * are on the include path (ra8_reflow / ra8_mem consumers and the host test
 * build). Otherwise empty. */
#if __has_include("ra8_tile_cache.h") && __has_include("ra8_tileatlas.h")

#include "ra8_epub_img_tiles.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_epub.h"
#include "ra8_epub_entry.h"
#include "ra8_err.h"
#include "ra8_tile_cache.h"
#include "ra8_tileatlas.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_epub_img_tiles";

/* ---------------------------------------------------------------------------
 * Small helpers.
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Find the registered source index for @p image_id, or -1.
 * @details Linear scan over the registered sources (bounded by the source cap).
 * @param[in] binder   Initialised binder.
 * @param[in] image_id Image id to find.
 * @return Source index in `[0, source_count)`, or -1 if unregistered.
 * @retval 0-source_count-1 Index of the source whose image_id matches.
 * @retval -1 No registered source matches @p image_id.
 * @pre @p binder is non-NULL and initialised.
 * @pre @p binder->source_count <= ::k_ra8_epub_tile_max_sources.
 * @post No state mutated.
 * @post A non-negative return indexes a live source.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static int32_t priv_find(const ra8_epub_tile_binder_t* binder, uint32_t image_id)
{
  for (uint8_t i = 0U; i < binder->source_count; ++i) {
    if (binder->sources[i].image_id == image_id) {
      return (int32_t)i;
    }
  }
  return -1;
}

/**
 * @brief ::ra8_tileatlas_pread_fn adapter over a stored archive entry.
 * @details @p ctx is the owning ::ra8_epub_tile_source_t; the read windows
 *          the entry's uncompressed bytes via `ra8_epub_entry_pread`.
 * @param[in]  ctx    The book-backed source.
 * @param[in]  offset Byte offset into the atlas entry.
 * @param[out] buf    Destination buffer.
 * @param[in]  len    Bytes requested.
 * @param[out] got    Bytes actually read.
 * @return Result code from the entry reader.
 * @retval k_ra8_ok Window read (short reads included).
 * @pre @p ctx has a non-NULL `book` and a valid `path`.
 * @pre @p buf holds @p len writable bytes.
 * @post `*got <= len` bytes were written to @p buf.
 * @post On any error `*got == 0`.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_entry_pread(void* ctx, uint64_t offset, uint8_t* buf, size_t len, size_t* got)
{
  const ra8_epub_tile_source_t* src = (const ra8_epub_tile_source_t*)ctx;
  return ra8_epub_entry_pread(src->book, src->path, offset, buf, len, got);
}

/**
 * @brief Measure an archive entry's uncompressed size.
 * @details Opens (and immediately closes) an entry cursor purely for the
 *          size report -- no entry bytes are inflated.
 * @param[in]  book     Open book.
 * @param[in]  path     Entry path.
 * @param[out] out_size Receives the entry's uncompressed byte length.
 * @return Result code.
 * @retval k_ra8_ok Entry found and measured.
 * @retval other    Propagated from the entry cursor.
 * @pre @p book is open; @p path is NUL-terminated.
 * @pre @p out_size is writable.
 * @post On success `*out_size` holds the entry length.
 * @post No cursor remains open.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_measure_entry(ra8_epub_book_t* book, const char* path, uint64_t* out_size)
{
  ra8_epub_entry_reader_t reader = {};
  ra8_err_t               err    = ra8_epub_entry_open(book, path, &reader, out_size);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_epub_entry_close(&reader);
  return err;
}

/* ---------------------------------------------------------------------------
 * Tile decode-on-miss.
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Tile-cache decode-on-miss trampoline (::ra8_tile_decode_fn).
 * @details Resolves the key's image and decodes the tile off its backing via
 *          `ra8_tileatlas_read_tile()` (entry pread for book-backed sources,
 *          the external seam otherwise). A tile or its stored stream that
 *          exceeds the cell/scratch budget maps to `k_ra8_err_no_mem`,
 *          preserving the binder's documented contract.
 * @param[in]  ctx        The owning ::ra8_epub_tile_binder_t.
 * @param[in]  key        Tile to decode.
 * @param[out] cell       Destination cell.
 * @param[in]  cell_bytes Cell capacity, bytes.
 * @param[out] out_w      Decoded tile width.
 * @param[out] out_h      Decoded tile height.
 * @return Result code from the atlas read (or a mapping thereof).
 * @retval k_ra8_ok The tile was decoded into @p cell.
 * @pre @p ctx is the owning binder; @p key indexes a registered image.
 * @pre @p cell holds @p cell_bytes bytes.
 * @post On success `*out_w`/`*out_h` hold the (possibly clamped) tile size.
 * @post On failure the descriptor is not written.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_tile_decode(void*                 ctx,
                                  const ra8_tile_key_t* key,
                                  uint8_t*              cell,
                                  uint32_t              cell_bytes,
                                  uint16_t*             out_w,
                                  uint16_t*             out_h)
{
  ra8_epub_tile_binder_t* binder = (ra8_epub_tile_binder_t*)ctx;
  const int32_t           si     = priv_find(binder, key->image_id);
  if (si < 0) {
    return k_ra8_err_not_found; /* GCOVR_EXCL_LINE -- get() rejects unregistered ids before the cache */
  }
  ra8_epub_tile_source_t*      src   = &binder->sources[si];
  const ra8_tileatlas_pread_fn pread = (src->book != nullptr) ? priv_entry_pread : src->pread;
  void*                        pctx  = (src->book != nullptr) ? (void*)src : src->pread_ctx;
  const ra8_err_t              err   = ra8_tileatlas_read_tile(pread,
                                                               pctx,
                                                               &src->info,
                                                               key->tile_x,
                                                               key->tile_y,
                                                               binder->scratch,
                                                               binder->scratch_cap,
                                                               cell,
                                                               cell_bytes,
                                                               out_w,
                                                               out_h);
  if (err == k_ra8_err_invalid_size) {
    return k_ra8_err_no_mem; /* tile exceeds the cell / scratch budget */
  }
  return err;
}

/* ---------------------------------------------------------------------------
 * Public API -- tile binder.
 * ---------------------------------------------------------------------------
 */

ra8_err_t ra8_epub_tile_binder_init(ra8_epub_tile_binder_t*     binder,
                                    const ra8_tile_cache_cfg_t* storage,
                                    uint8_t*                    scratch,
                                    uint32_t                    scratch_cap)
{
  RA8_CHECK_NULL_PTR(binder, s_tag, "binder must not be nullptr");
  RA8_CHECK_NULL_PTR(storage, s_tag, "storage must not be nullptr");
  (void)memset(binder, 0, sizeof(*binder));
  binder->scratch          = scratch;
  binder->scratch_cap      = (scratch != nullptr) ? scratch_cap : 0U;
  ra8_tile_cache_cfg_t cfg = *storage;
  cfg.decode               = priv_tile_decode;
  cfg.decode_ctx           = binder;
  return ra8_tile_cache_init(&binder->cache, &cfg);
}

/**
 * @brief Common pre-registration guards shared by both add paths.
 * @details Split out so both add paths share the capacity + duplicate-id checks.
 * @param[in] binder   Initialised binder.
 * @param[in] image_id Proposed image id.
 * @return Result code.
 * @retval k_ra8_ok              A slot is free and the id is unused.
 * @retval k_ra8_err_no_mem      The source table is full.
 * @retval k_ra8_err_invalid_arg @p image_id is already registered.
 * @pre @p binder is non-NULL and initialised.
 * @pre The caller validated its own pointers.
 * @post No state mutated.
 * @post Return depends solely on the binder state and @p image_id.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_check_slot(const ra8_epub_tile_binder_t* binder, uint32_t image_id)
{
  if (binder->source_count >= (uint8_t)k_ra8_epub_tile_max_sources) {
    return k_ra8_err_no_mem;
  }
  if (priv_find(binder, image_id) >= 0) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/**
 * @brief Fill + validate a book-backed source slot (measure, then parse).
 * @details Runs against the final slot address so the entry-pread adapter's
 *          ctx (the slot itself) is stable; the caller commits the count
 *          only on success.
 * @param[in,out] src      The slot to fill (already zeroed).
 * @param[in]     book     Owning book.
 * @param[in]     path     Entry path (`plen` bytes + NUL).
 * @param[in]     plen     `strlen(path)`.
 * @param[in]     image_id Unique image id.
 * @return Result code.
 * @retval k_ra8_ok Slot holds a validated atlas source.
 * @retval other    Propagated from the measure / parse.
 * @pre `plen < k_ra8_epub_max_path_len` (caller-validated).
 * @pre @p src points at the slot's final storage.
 * @post On success the slot is fully bound.
 * @post On error the slot is re-zeroed.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_fill_book_source(ra8_epub_tile_source_t* src,
                                       ra8_epub_book_t*        book,
                                       const char*             path,
                                       size_t                  plen,
                                       uint32_t                image_id)
{
  src->book     = book;
  src->image_id = image_id;
  (void)memcpy(src->path, path, plen + 1U);
  ra8_err_t err = priv_measure_entry(book, path, &src->total_size);
  if (err == k_ra8_ok) {
    err = ra8_tileatlas_parse(priv_entry_pread, src, src->total_size, &src->info);
  }
  if (err != k_ra8_ok) {
    (void)memset(src, 0, sizeof(*src));
  }
  return err;
}

ra8_err_t ra8_epub_tile_binder_add(ra8_epub_tile_binder_t* binder,
                                   ra8_epub_book_t*        book,
                                   const char*             path,
                                   uint32_t                image_id)
{
  RA8_CHECK_NULL_PTR(binder, s_tag, "binder must not be nullptr");
  RA8_CHECK_NULL_PTR(book, s_tag, "book must not be nullptr");
  RA8_CHECK_NULL_PTR(path, s_tag, "path must not be nullptr");
  const size_t plen = strlen(path);
  if ((plen == 0U) || (plen >= (size_t)k_ra8_epub_max_path_len)) {
    return k_ra8_err_invalid_arg;
  }
  ra8_err_t err = priv_check_slot(binder, image_id);
  if (err != k_ra8_ok) {
    return err;
  }
  ra8_epub_tile_source_t* src = &binder->sources[binder->source_count];
  (void)memset(src, 0, sizeof(*src));
  err = priv_fill_book_source(src, book, path, plen, image_id);
  if (err != k_ra8_ok) {
    return err;
  }
  binder->source_count++;
  return k_ra8_ok;
}

ra8_err_t ra8_epub_tile_binder_add_ext(ra8_epub_tile_binder_t* binder,
                                       ra8_tileatlas_pread_fn  pread,
                                       void*                   pread_ctx,
                                       uint64_t                total_size,
                                       uint32_t                image_id)
{
  RA8_CHECK_NULL_PTR(binder, s_tag, "binder must not be nullptr");
  RA8_CHECK_NULL_PTR(pread, s_tag, "pread must not be nullptr");
  ra8_err_t err = priv_check_slot(binder, image_id);
  if (err != k_ra8_ok) {
    return err;
  }
  ra8_epub_tile_source_t* src = &binder->sources[binder->source_count];
  (void)memset(src, 0, sizeof(*src));
  src->pread      = pread;
  src->pread_ctx  = pread_ctx;
  src->total_size = total_size;
  src->image_id   = image_id;
  err             = ra8_tileatlas_parse(pread, pread_ctx, total_size, &src->info);
  if (err != k_ra8_ok) {
    (void)memset(src, 0, sizeof(*src));
    return err;
  }
  binder->source_count++;
  return k_ra8_ok;
}

ra8_err_t ra8_epub_tile_binder_info(const ra8_epub_tile_binder_t* binder,
                                    uint32_t                      image_id,
                                    ra8_tileatlas_info_t*         out_info)
{
  RA8_CHECK_NULL_PTR(binder, s_tag, "binder must not be nullptr");
  RA8_CHECK_NULL_PTR(out_info, s_tag, "out_info must not be nullptr");
  const int32_t si = priv_find(binder, image_id);
  if (si < 0) {
    return k_ra8_err_not_found;
  }
  *out_info = binder->sources[si].info;
  return k_ra8_ok;
}

ra8_err_t ra8_epub_tile_binder_get(ra8_epub_tile_binder_t* binder,
                                   uint32_t                image_id,
                                   uint16_t                tile_x,
                                   uint16_t                tile_y,
                                   ra8_tile_t*             out_tile)
{
  RA8_CHECK_NULL_PTR(binder, s_tag, "binder must not be nullptr");
  RA8_CHECK_NULL_PTR(out_tile, s_tag, "out_tile must not be nullptr");
  const int32_t si = priv_find(binder, image_id);
  if (si < 0) {
    return k_ra8_err_not_found;
  }
  const ra8_tileatlas_info_t* info = &binder->sources[si].info;
  if (tile_x >= info->tile_cols) {
    return k_ra8_err_out_of_range;
  }
  if (tile_y >= info->tile_rows) {
    return k_ra8_err_out_of_range;
  }
  const ra8_tile_key_t key = {.image_id = image_id, .tile_x = tile_x, .tile_y = tile_y};
  return ra8_tile_cache_get(&binder->cache, &key, out_tile);
}

ra8_err_t ra8_epub_tile_binder_put(ra8_epub_tile_binder_t* binder, const uint8_t* pixels)
{
  RA8_CHECK_NULL_PTR(binder, s_tag, "binder must not be nullptr");
  RA8_CHECK_NULL_PTR(pixels, s_tag, "pixels must not be nullptr");
  return ra8_tile_cache_put(&binder->cache, pixels);
}

/* ---------------------------------------------------------------------------
 * Public API -- reflow img loader.
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Validate a bound loader context + href length (single-condition guards).
 * @details Split out so `ra8_epub_reflow_img_load` stays under the
 *          NASA-Rule-4 statement budget.
 * @param[in] ld       Loader context.
 * @param[in] href_len `<img src>` href length.
 * @return Result code.
 * @retval k_ra8_ok              Context + length usable.
 * @retval k_ra8_err_invalid_arg A field is unset or the length is out of range.
 * @pre @p ld is non-NULL.
 * @pre @p href_len is the caller's href length.
 * @post No state mutated.
 * @post Return depends solely on the inputs.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_loader_args_ok(const ra8_epub_img_loader_t* ld, uint32_t href_len)
{
  if (ld->book == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if (ld->scratch == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if (ld->scratch_cap == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (href_len == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (href_len >= (uint32_t)k_ra8_epub_max_path_len) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/**
 * @brief Reject any NULL `ra8_epub_reflow_img_load` pointer argument.
 * @details Split out so `ra8_epub_reflow_img_load` stays under the
 *          NASA-Rule-4 statement budget.
 * @param[in] ctx       Loader context (opaque).
 * @param[in] href      Href string.
 * @param[in] out_bytes Output bytes pointer.
 * @param[in] out_len   Output length pointer.
 * @return Result code.
 * @retval k_ra8_ok           All four pointers are non-NULL.
 * @retval k_ra8_err_null_ptr Some pointer is NULL.
 * @pre The caller forwards its own arguments.
 * @pre No pointer is dereferenced here.
 * @post No state mutated.
 * @post Return depends solely on the inputs.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_loader_null_ok(const void*     ctx,
                                     const char*     href,
                                     const uint8_t** out_bytes,
                                     const size_t*   out_len)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  RA8_CHECK_NULL_PTR(href, s_tag, "href must not be nullptr");
  RA8_CHECK_NULL_PTR(out_bytes, s_tag, "out_bytes must not be nullptr");
  RA8_CHECK_NULL_PTR(out_len, s_tag, "out_len must not be nullptr");
  return k_ra8_ok;
}

ra8_err_t ra8_epub_reflow_img_load(void*           ctx,
                                   const char*     href,
                                   uint32_t        href_len,
                                   const uint8_t** out_bytes,
                                   size_t*         out_len)
{
  const ra8_err_t nz = priv_loader_null_ok(ctx, href, out_bytes, out_len);
  if (nz != k_ra8_ok) {
    return nz;
  }
  *out_len                  = 0U;
  ra8_epub_img_loader_t* ld = (ra8_epub_img_loader_t*)ctx;
  const ra8_err_t        ok = priv_loader_args_ok(ld, href_len);
  if (ok != k_ra8_ok) {
    return ok;
  }
  char path[k_ra8_epub_max_path_len];
  (void)memcpy(path, href, (size_t)href_len);
  path[href_len] = '\0';

  size_t          got = 0U;
  const ra8_err_t err = ra8_epub_get_resource(ld->book, path, ld->scratch, ld->scratch_cap, &got);
  if (err != k_ra8_ok) {
    return err;
  }
  *out_bytes = ld->scratch;
  *out_len   = got;
  return k_ra8_ok;
}

#else
/* ra8_mem tile cache / atlas reader not on the include path for this app: the
 * EPUB image-tile binder is unused here. A single typedef keeps the
 * translation unit non-empty. */
typedef int ra8_epub_img_tiles_unused_translation_unit_t;
#endif /* __has_include("ra8_tile_cache.h") && __has_include("ra8_tileatlas.h") */
