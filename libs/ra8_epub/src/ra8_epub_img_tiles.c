/**
 * @file ra8_epub_img_tiles.c
 * @brief Tile-cache paging of a large in-EPUB image + a real reflow img loader (#231).
 *
 * @details
 * Implements ::ra8_epub_tile_binder (pages a display-native tile atlas through
 * ::ra8_tile_cache, decode-on-demand keyed by `image_id + tile`, bounded RAM) and
 * ::ra8_epub_reflow_img_load (resolves an `<img src>` href to encoded bytes in a
 * caller-owned bounded scratch). Both read off the streaming reader, so the whole
 * archive is never resident; the tile path additionally never holds the whole
 * image -- each tile is a handful of positioned reads into one cache cell.
 *
 * Guarded on `__has_include("ra8_tile_cache.h")`: the pure `ra8_epub` core (and
 * epub-only apps that never pull in `ra8_mem`) still link with this TU empty.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / EPUB]
 * {World: NS}
 */

/* Compile the binder only where the ra8_mem tile cache is on the include path
 * (ra8_reflow / ra8_mem consumers, and the host test build). Otherwise empty. */
#if __has_include("ra8_tile_cache.h")

#include "ra8_epub_img_tiles.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_check.h"
#include "ra8_epub.h"
#include "ra8_epub_entry.h"
#include "ra8_err.h"
#include "ra8_tile_cache.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_epub_img_tiles";

/** @brief Tile-atlas magic bytes ("RTI1"). */
static const uint8_t s_magic[k_ra8_epub_tileimg_magic_len] = {'R', 'T', 'I', '1'};

/**
 * @enum ra8_epub_tile_le_t
 * @brief Little-endian byte layout constants for the 16-bit header fields.
 */
typedef enum : uint8_t {
  k_ra8_tile_le_lo    = 0U, /**< Low byte offset.  */
  k_ra8_tile_le_hi    = 1U, /**< High byte offset. */
  k_ra8_tile_le_shift = 8U, /**< High-byte shift.  */
} ra8_epub_tile_le_t;

/* ---------------------------------------------------------------------------
 * Small pure helpers.
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Read a little-endian uint16 from @p buf at @p off.
 * @details See implementation.
 * @param[in] buf Source bytes.
 * @param[in] off Field offset.
 * @return The 16-bit value.
 * @retval 0-65535 Little-endian uint16 assembled from the two bytes at @p off.
 * @pre @p buf holds at least `off + 2` bytes.
 * @pre @p off does not overflow the buffer.
 * @post No state mutated.
 * @post Return depends solely on the two bytes read.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
static uint16_t priv_rd_u16(const uint8_t* buf, uint8_t off)
{
  return (uint16_t)((uint16_t)buf[off + k_ra8_tile_le_lo] |
                    (uint16_t)((uint16_t)buf[off + k_ra8_tile_le_hi] << k_ra8_tile_le_shift));
}

/**
 * @brief Minimum of two 32-bit values.
 * @details See implementation.
 * @param[in] a First value.
 * @param[in] b Second value.
 * @return The smaller of @p a and @p b.
 * @retval a @p a when @p a is strictly less than @p b.
 * @retval b Otherwise (@p b when @p b <= @p a).
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return is <= both inputs.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
static uint32_t priv_min_u32(uint32_t a, uint32_t b)
{
  return (a < b) ? a : b;
}

/**
 * @brief Ceil-divide @p n by @p d (d > 0).
 * @details See implementation.
 * @param[in] n Numerator.
 * @param[in] d Denominator (> 0).
 * @return `ceil(n / d)`.
 * @retval 0-UINT32_MAX The ceiling of @p n divided by @p d.
 * @pre @p d is non-zero.
 * @pre The result fits uint32_t.
 * @post No state mutated.
 * @post Return * d >= n.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
static uint32_t priv_ceil_div(uint32_t n, uint32_t d)
{
  return (n + d - 1U) / d;
}

/**
 * @brief Find the registered source index for @p image_id, or -1.
 * @details See implementation.
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
static int32_t priv_find(const ra8_epub_tile_binder_t* binder, uint32_t image_id)
{
  for (uint8_t i = 0U; i < binder->source_count; ++i) {
    if (binder->sources[i].image_id == image_id) {
      return (int32_t)i;
    }
  }
  return -1;
}

/* ---------------------------------------------------------------------------
 * Tile decode-on-miss.
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Read a tile's `th` scanline bands off the atlas into a cache cell.
 * @details See implementation. One positioned read per tile row; the tile is
 *          packed tightly at `tw * bpp` bytes per row in the cell.
 * @param[in]  src  Registered source (book + path + geometry).
 * @param[in]  px   Tile left edge, pixels.
 * @param[in]  py   Tile top edge, pixels.
 * @param[in]  tw   Tile width, pixels (clamped for edge tiles).
 * @param[in]  th   Tile height, pixels (clamped for edge tiles).
 * @param[out] cell Destination cell.
 * @return Result code.
 * @retval k_ra8_ok                    All rows read.
 * @retval k_ra8_err_validation_failed A row came up short (corrupt entry).
 * @retval other                       Propagated from `ra8_epub_entry_pread`.
 * @pre @p src is a live source; @p cell holds `tw * th * bpp` bytes.
 * @pre `px + tw <= width` and `py + th <= height`.
 * @post On success @p cell holds the packed tile pixels.
 * @post On any error the cell contents are partial.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t priv_read_tile_rows(const ra8_epub_tile_source_t* src,
                                     uint32_t                      px,
                                     uint32_t                      py,
                                     uint32_t                      tw,
                                     uint32_t                      th,
                                     uint8_t*                      cell)
{
  const uint32_t bpp        = (uint32_t)src->info.bpp;
  const uint32_t row_bytes  = tw * bpp;
  const uint32_t src_stride = (uint32_t)src->info.width * bpp;
  for (uint32_t r = 0U; r < th; ++r) {
    const uint64_t  off = (uint64_t)k_ra8_epub_tileimg_hdr_bytes +
                          ((uint64_t)(py + r) * (uint64_t)src_stride) +
                          ((uint64_t)px * (uint64_t)bpp);
    size_t          got = 0U;
    const ra8_err_t err = ra8_epub_entry_pread(src->book,
                                               src->path,
                                               off,
                                               &cell[(size_t)r * (size_t)row_bytes],
                                               row_bytes,
                                               &got);
    if (err != k_ra8_ok) {
      return err;
    }
    if (got != (size_t)row_bytes) {
      return k_ra8_err_validation_failed; /* GCOVR_EXCL_LINE -- rows lie within a validated entry */
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Tile-cache decode-on-miss trampoline (::ra8_tile_decode_fn).
 * @details See implementation. Resolves the key's image, clamps edge tiles, and
 *          reads the tile rows into the cell.
 * @param[in]  ctx        The owning ::ra8_epub_tile_binder_t.
 * @param[in]  key        Tile to decode.
 * @param[out] cell       Destination cell.
 * @param[in]  cell_bytes Cell capacity, bytes.
 * @param[out] out_w      Decoded tile width.
 * @param[out] out_h      Decoded tile height.
 * @return Result code from the read (or a validation error).
 * @retval k_ra8_ok The tile was decoded into @p cell.
 * @pre @p ctx is the owning binder; @p key indexes a registered image.
 * @pre @p cell holds @p cell_bytes bytes.
 * @post On success `*out_w`/`*out_h` hold the (possibly clamped) tile size.
 * @post On failure the descriptor is not written.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t priv_tile_decode(void*                 ctx,
                                  const ra8_tile_key_t* key,
                                  uint8_t*              cell,
                                  uint32_t              cell_bytes,
                                  uint16_t*             out_w,
                                  uint16_t*             out_h)
{
  const ra8_epub_tile_binder_t* binder = (const ra8_epub_tile_binder_t*)ctx;
  const int32_t                 si     = priv_find(binder, key->image_id);
  if (si < 0) {
    return k_ra8_err_not_found; /* GCOVR_EXCL_LINE -- get() rejects unregistered ids before the cache */
  }
  const ra8_epub_tile_source_t* src = &binder->sources[si];
  const uint32_t                px  = (uint32_t)key->tile_x * (uint32_t)src->info.tile_w;
  const uint32_t                py  = (uint32_t)key->tile_y * (uint32_t)src->info.tile_h;
  if (px >= (uint32_t)src->info.width) {
    return k_ra8_err_out_of_range; /* GCOVR_EXCL_LINE -- get() bounds tile_x before the cache */
  }
  if (py >= (uint32_t)src->info.height) {
    return k_ra8_err_out_of_range; /* GCOVR_EXCL_LINE -- get() bounds tile_y before the cache */
  }
  const uint32_t tw = priv_min_u32((uint32_t)src->info.tile_w, (uint32_t)src->info.width - px);
  const uint32_t th = priv_min_u32((uint32_t)src->info.tile_h, (uint32_t)src->info.height - py);
  if ((tw * th * (uint32_t)src->info.bpp) > cell_bytes) {
    return k_ra8_err_no_mem;
  }
  const ra8_err_t err = priv_read_tile_rows(src, px, py, tw, th, cell);
  if (err != k_ra8_ok) {
    return err;
  }
  *out_w = (uint16_t)tw;
  *out_h = (uint16_t)th;
  return k_ra8_ok;
}

/* ---------------------------------------------------------------------------
 * Header parse.
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Read + validate the 16-byte atlas header of @p path into @p out_info.
 * @details See implementation.
 * @param[in]  book     Open book holding the entry.
 * @param[in]  path     Atlas entry path.
 * @param[out] out_info Parsed geometry on success.
 * @return Result code.
 * @retval k_ra8_ok                    Header valid; geometry filled.
 * @retval k_ra8_err_validation_failed Short read / bad magic / zero dim / bad bpp.
 * @retval other                       Propagated from `ra8_epub_entry_pread`.
 * @pre @p book is open; @p path names a stored entry.
 * @pre @p out_info is writable.
 * @post On success `*out_info` holds validated geometry.
 * @post On failure `*out_info` is unspecified.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t
priv_parse_header(ra8_epub_book_t* book, const char* path, ra8_epub_tileimg_info_t* out_info)
{
  uint8_t         hdr[k_ra8_epub_tileimg_hdr_bytes] = {};
  size_t          got                               = 0U;
  const ra8_err_t err = ra8_epub_entry_pread(book, path, 0U, hdr, sizeof(hdr), &got);
  if (err != k_ra8_ok) {
    return err;
  }
  if (got != (size_t)k_ra8_epub_tileimg_hdr_bytes) {
    return k_ra8_err_validation_failed;
  }
  if (memcmp(&hdr[k_ra8_epub_tileimg_ofs_magic], s_magic, sizeof(s_magic)) != 0) {
    return k_ra8_err_validation_failed;
  }
  out_info->width  = priv_rd_u16(hdr, (uint8_t)k_ra8_epub_tileimg_ofs_width);
  out_info->height = priv_rd_u16(hdr, (uint8_t)k_ra8_epub_tileimg_ofs_height);
  out_info->tile_w = priv_rd_u16(hdr, (uint8_t)k_ra8_epub_tileimg_ofs_tile_w);
  out_info->tile_h = priv_rd_u16(hdr, (uint8_t)k_ra8_epub_tileimg_ofs_tile_h);
  out_info->bpp    = hdr[k_ra8_epub_tileimg_ofs_bpp];
  /* Single-condition guards (no compound decisions -> no MC/DC obligation). */
  if (out_info->width == 0U) {
    return k_ra8_err_validation_failed;
  }
  if (out_info->height == 0U) {
    return k_ra8_err_validation_failed;
  }
  if (out_info->tile_w == 0U) {
    return k_ra8_err_validation_failed;
  }
  if (out_info->tile_h == 0U) {
    return k_ra8_err_validation_failed;
  }
  if (out_info->bpp == 0U) {
    return k_ra8_err_validation_failed;
  }
  if (out_info->bpp > (uint8_t)k_ra8_epub_tile_bpp_max) {
    return k_ra8_err_validation_failed;
  }
  out_info->tile_cols = (uint16_t)priv_ceil_div(out_info->width, out_info->tile_w);
  out_info->tile_rows = (uint16_t)priv_ceil_div(out_info->height, out_info->tile_h);
  return k_ra8_ok;
}

/* ---------------------------------------------------------------------------
 * Public API -- tile binder.
 * ---------------------------------------------------------------------------
 */

ra8_err_t ra8_epub_tile_binder_init(ra8_epub_tile_binder_t*     binder,
                                    const ra8_tile_cache_cfg_t* storage)
{
  RA8_CHECK_NULL_PTR(binder, s_tag, "binder must not be nullptr");
  RA8_CHECK_NULL_PTR(storage, s_tag, "storage must not be nullptr");
  (void)memset(binder, 0, sizeof(*binder));
  ra8_tile_cache_cfg_t cfg = *storage;
  cfg.decode               = priv_tile_decode;
  cfg.decode_ctx           = binder;
  return ra8_tile_cache_init(&binder->cache, &cfg);
}

/**
 * @brief Append a validated source into the binder's next free slot.
 * @details See implementation. Split out so `ra8_epub_tile_binder_add` stays under
 *          the NASA-Rule-4 statement budget.
 * @param[in,out] binder   Binder with a free slot.
 * @param[in]     book     Owning book.
 * @param[in]     path     Entry path (`plen` bytes + NUL).
 * @param[in]     plen     `strlen(path)`.
 * @param[in]     image_id Unique image id.
 * @param[in]     info     Parsed geometry.
 * @pre `binder->source_count < k_ra8_epub_tile_max_sources`.
 * @pre `plen < k_ra8_epub_max_path_len` and @p info is valid.
 * @post One source is appended and `source_count` incremented.
 * @post The path is copied NUL-terminated into the slot.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void priv_register_source(ra8_epub_tile_binder_t*        binder,
                                 ra8_epub_book_t*               book,
                                 const char*                    path,
                                 size_t                         plen,
                                 uint32_t                       image_id,
                                 const ra8_epub_tileimg_info_t* info)
{
  ra8_epub_tile_source_t* src = &binder->sources[binder->source_count];
  src->book                   = book;
  src->image_id               = image_id;
  src->info                   = *info;
  (void)memcpy(src->path, path, plen + 1U);
  binder->source_count++;
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
  if (plen == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (plen >= (size_t)k_ra8_epub_max_path_len) {
    return k_ra8_err_invalid_arg;
  }
  if (binder->source_count >= (uint8_t)k_ra8_epub_tile_max_sources) {
    return k_ra8_err_no_mem;
  }
  if (priv_find(binder, image_id) >= 0) {
    return k_ra8_err_invalid_arg;
  }
  ra8_epub_tileimg_info_t info = {};
  const ra8_err_t         err  = priv_parse_header(book, path, &info);
  if (err != k_ra8_ok) {
    return err;
  }
  priv_register_source(binder, book, path, plen, image_id, &info);
  return k_ra8_ok;
}

ra8_err_t ra8_epub_tile_binder_info(const ra8_epub_tile_binder_t* binder,
                                    uint32_t                      image_id,
                                    ra8_epub_tileimg_info_t*      out_info)
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
  const ra8_epub_tileimg_info_t* info = &binder->sources[si].info;
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
 * @details See implementation. Split out so `ra8_epub_reflow_img_load` stays under
 *          the NASA-Rule-4 statement budget.
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
 * @details See implementation. Split out so `ra8_epub_reflow_img_load` stays under
 *          the NASA-Rule-4 statement budget.
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
/* ra8_mem tile cache not on the include path for this app: the EPUB image-tile
 * binder is unused here. A single typedef keeps the translation unit non-empty. */
typedef int ra8_epub_img_tiles_unused_translation_unit_t;
#endif /* __has_include("ra8_tile_cache.h") */
