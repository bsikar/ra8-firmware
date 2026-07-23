/**
 * @file examples/ek_ra8d2/hw_pending/ereader_comic/cm_tiled_check.c
 * @brief Oversized-page tile self-check for ereader_comic (#344).
 *
 * @par Tag
 * [Ring 7 / App] {World: NS}
 *
 * @details
 * Opens the baked oversized single-page CBZ (::k_comic_large_cbz), confirms its
 * decoded footprint overruns the whole-decode budget, transcodes the page to a
 * JOF atlas through ::ra8_comic_tiles_import, and decodes every tile of the
 * atlas in bounded RAM, FNV-hashing the decoded pixels. The result feeds a
 * deterministic boot banner field that pins the fix on device (board_sim /
 * silicon). Every buffer is file-static SDRAM -- no heap (NASA P10 Rule 3) -- and
 * the panel framebuffer is never touched, so the reader's page-1 render is
 * undisturbed.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include "cm_tiled_check.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "comic_large_fixture.h"
#include "ra8_comic.h"
#include "ra8_comic_tiles.h"
#include "ra8_err.h"
#include "ra8_jof.h"
#include "ra8_jof_produce.h"
#include "ra8_tile_cache.h"

/**
 * @enum cm_tiled_cfg_t
 * @brief Tile geometry, cache budget, and hashing constants for the self-check.
 * @details Grayscale 256x256 tiles (1 bpp -> 64 KiB cells); an 8-cell cache is
 *          far smaller than the multi-MiB decoded page, so decode-on-demand
 *          paging is genuinely exercised. The budget matches the reader's
 *          whole-decode arena so the footprint decision mirrors the live path.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ct_tile       = 256U,                                  /**< Square tile edge, pixels.        */
  k_ct_bpp_gray   = 1U,                                    /**< Grayscale atlas bytes per pixel. */
  k_ct_cell_bytes = k_ct_tile * k_ct_tile * k_ct_bpp_gray, /**< Bytes per cache cell.            */
  k_ct_cells      = 8U,                                    /**< Tile-cache cell budget.          */
  k_ct_buckets    = 16U,                                   /**< Tile-cache hash buckets.         */
  k_ct_budget     = 2U * 1024U * 1024U,                    /**< Whole-decode budget (threshold). */
  k_ct_pagebuf    = 64U * 1024U,                           /**< Encoded page scratch, bytes.     */
  k_ct_work       = 2U * 1024U * 1024U,                    /**< Producer work arena, bytes.      */
  k_ct_atlas      = 2U * 1024U * 1024U,                    /**< Atlas memstore backing, bytes.   */
  k_ct_scratch    = 96U * 1024U,                           /**< Stored-tile staging, bytes.      */
  k_ct_page_cap   = 4U,                                    /**< Comic page-index capacity.       */
  k_ct_name_cap   = 64U,                                   /**< Comic name-arena bytes.          */
  k_ct_fnv_offset = 2166136261U,                           /**< FNV-1a-32 offset basis.          */
  k_ct_fnv_prime  = 16777619U,                             /**< FNV-1a-32 prime.                 */
} cm_tiled_cfg_t;

/**
 * @struct cm_ct_blob_t
 * @brief Resident-buffer backing for the comic reader's seek+read seam.
 * @details Points at the baked ::k_comic_large_cbz bytes (a bounds-checked
 *          memcpy); no archive is ever resident beyond this fixture.
 * @since 0.1.0
 */
typedef struct {
  const uint8_t* d; /**< Archive bytes.  */
  size_t         n; /**< Archive length. */
} cm_ct_blob_t;

/** @brief SDRAM: encoded page scratch (the tile producer's input). */
[[gnu::section(".sdram_data")]] static uint8_t s_ct_pagebuf[k_ct_pagebuf];
/** @brief SDRAM: producer streaming work arena. */
[[gnu::section(".sdram_data")]] static uint8_t s_ct_work[k_ct_work];
/** @brief SDRAM: produced JOF atlas backing store. */
[[gnu::section(".sdram_data")]] static uint8_t s_ct_atlas[k_ct_atlas];
/** @brief SDRAM: tile-cache cell storage. */
[[gnu::section(
  ".sdram_data")]] static uint8_t s_ct_cells[(size_t)k_ct_cells * (size_t)k_ct_cell_bytes];
/** @brief SDRAM: stored-tile staging for deflate tiles. */
[[gnu::section(".sdram_data")]] static uint8_t s_ct_scratch[k_ct_scratch];
/** @brief SDRAM: comic page-index storage. */
[[gnu::section(".sdram_data")]] static ra8_comic_page_t s_ct_pages[k_ct_page_cap];
/** @brief SDRAM: comic name arena. */
[[gnu::section(".sdram_data")]] static char s_ct_names[k_ct_name_cap];
/** @brief SDRAM: the open comic reader. */
[[gnu::section(".sdram_data")]] static ra8_comic_t s_ct_comic;
/** @brief SDRAM: the comic tile reader. */
[[gnu::section(".sdram_data")]] static ra8_comic_tile_reader_t s_ct_reader;
/** @brief Tile-cache key storage. */
static ra8_tile_key_t s_ct_keys[k_ct_cells];
/** @brief Tile-cache dimension descriptors. */
static ra8_tile_dims_t s_ct_dims[k_ct_cells];
/** @brief Tile-cache link metadata. */
static ra8_keycache_cell_t s_ct_meta[k_ct_cells];
/** @brief Tile-cache hash buckets. */
static int32_t s_ct_buckets[k_ct_buckets];
/** @brief Blob backing bound into the read seam. */
static cm_ct_blob_t s_ct_blob;

/** @brief ::ra8_comic_read_fn over the resident CBZ blob (bounds-clamped). */
// cppcheck-suppress constParameterCallback -- ctx is pinned by the ra8_comic_read_fn vtable signature
static size_t cm_ct_read(void* ctx, uint64_t off, void* buf, size_t len)
{
  const cm_ct_blob_t* s = (const cm_ct_blob_t*)ctx;
  if ((s == nullptr) || (buf == nullptr) || (off >= (uint64_t)s->n)) {
    return 0U;
  }
  const uint64_t avail = (uint64_t)s->n - off;
  const size_t   k     = (len > (size_t)avail) ? (size_t)avail : len;
  (void)memcpy(buf, &s->d[off], k);
  return k;
}

/** @brief FNV-1a-32 folding @p n bytes of @p p into the running hash @p h. */
static uint32_t cm_ct_fnv(uint32_t h, const uint8_t* p, size_t n)
{
  for (size_t i = 0U; i < n; ++i) {
    h = (h ^ (uint32_t)p[i]) * (uint32_t)k_ct_fnv_prime;
  }
  return h;
}

/**
 * @brief Open the baked oversized CBZ and read page 0's encoded bytes.
 * @param[out] out_got Receives the encoded page length.
 * @return Whether the archive opened and page 0 was extracted.
 * @retval true  ::s_ct_comic is open and ::s_ct_pagebuf holds the encoded page.
 * @retval false The archive failed to open or extract.
 * @pre ::k_comic_large_cbz holds ::k_comic_large_cbz_len bytes.
 * @pre @p out_got is writable.
 * @post On true `*out_got > 0` and the comic stays open (caller closes it).
 * @post On false ::s_ct_comic is left unopened.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static bool cm_ct_open(size_t* out_got)
{
  s_ct_blob.d = k_comic_large_cbz;
  s_ct_blob.n = (size_t)k_comic_large_cbz_len;
  if (ra8_comic_open(&s_ct_comic,
                     cm_ct_read,
                     &s_ct_blob,
                     (uint64_t)k_comic_large_cbz_len,
                     s_ct_pages,
                     (uint32_t)k_ct_page_cap,
                     s_ct_names,
                     (uint32_t)k_ct_name_cap) != k_ra8_ok) {
    return false;
  }
  if (ra8_comic_page_count(&s_ct_comic) == 0U) {
    return false;
  }
  *out_got = 0U;
  return ra8_comic_page_read(&s_ct_comic, 0U, s_ct_pagebuf, sizeof s_ct_pagebuf, out_got) ==
         k_ra8_ok;
}

/**
 * @brief Decode every tile of the bound atlas, FNV-hashing the pixels.
 * @param[in]  info Parsed atlas geometry of the imported page.
 * @param[out] out_crc Receives the digest over all tile payloads.
 * @return Whether every tile decoded cleanly.
 * @retval true  All tiles decoded; `*out_crc` holds the digest.
 * @retval false A tile fetch failed (digest not trustworthy).
 * @pre ::s_ct_reader has the page bound; @p info is its geometry.
 * @pre @p out_crc is writable.
 * @post On true `*out_crc` is a deterministic digest over the decoded pixels.
 * @post Every fetched tile is released (no pin leaks).
 * @note Not thread-safe.
 * @since 0.1.0
 */
static bool cm_ct_hash_tiles(const ra8_jof_info_t* info, uint32_t* out_crc)
{
  uint32_t crc = (uint32_t)k_ct_fnv_offset;
  for (uint16_t ty = 0U; ty < info->tile_rows; ++ty) {
    for (uint16_t tx = 0U; tx < info->tile_cols; ++tx) {
      ra8_tile_t tile = {};
      if (ra8_comic_tiles_tile(&s_ct_reader, tx, ty, &tile) != k_ra8_ok) {
        return false;
      }
      const size_t n = (size_t)tile.width * (size_t)tile.height * (size_t)info->bpp;
      crc            = cm_ct_fnv(crc, tile.pixels, n);
      if (ra8_comic_tiles_release(&s_ct_reader, tile.pixels) != k_ra8_ok) {
        return false;
      }
    }
  }
  *out_crc = crc;
  return true;
}

/** @brief Tile-cache storage config over the file-static cache buffers. */
static ra8_tile_cache_cfg_t cm_ct_cache_cfg(void)
{
  return (ra8_tile_cache_cfg_t){
    .cell_mem     = s_ct_cells,
    .cell_bytes   = (uint32_t)k_ct_cell_bytes,
    .cell_count   = (uint32_t)k_ct_cells,
    .meta         = s_ct_meta,
    .keys         = s_ct_keys,
    .dims         = s_ct_dims,
    .buckets      = s_ct_buckets,
    .bucket_count = (uint32_t)k_ct_buckets,
    .decode       = nullptr,
    .decode_ctx   = nullptr,
  };
}

cm_tiled_result_t cm_comic_tiled_selfcheck(void)
{
  cm_tiled_result_t r   = {};
  size_t            got = 0U;
  if (!cm_ct_open(&got)) {
    return r;
  }
  uint16_t w   = 0U;
  uint16_t h   = 0U;
  uint64_t dec = 0U;
  if (ra8_comic_tiles_footprint(s_ct_pagebuf, got, &w, &h, &dec) != k_ra8_ok) {
    (void)ra8_comic_close(&s_ct_comic);
    return r;
  }
  if (!ra8_comic_tiles_over_budget(dec, (uint64_t)k_ct_budget)) {
    (void)ra8_comic_close(&s_ct_comic);
    return r; /* fixture must be oversized; a fit page is not the case under test */
  }
  ra8_tile_cache_cfg_t storage = cm_ct_cache_cfg();
  if (ra8_comic_tiles_init(&s_ct_reader, &storage, s_ct_scratch, (uint32_t)k_ct_scratch) !=
      k_ra8_ok) {
    (void)ra8_comic_close(&s_ct_comic);
    return r;
  }
  const ra8_comic_tiles_import_cfg_t cfg = {
    .tile_w        = (uint16_t)k_ct_tile,
    .tile_h        = (uint16_t)k_ct_tile,
    .codec         = (uint8_t)k_ra8_jof_codec_deflate,
    .max_width     = (uint16_t)k_comic_large_w,
    .max_height    = (uint16_t)k_comic_large_h,
    .work          = s_ct_work,
    .work_cap      = sizeof s_ct_work,
    .webp_work     = nullptr,
    .webp_work_cap = 0U,
    .atlas         = s_ct_atlas,
    .atlas_cap     = sizeof s_ct_atlas,
  };
  if (ra8_comic_tiles_import(&s_ct_reader, s_ct_pagebuf, got, &cfg) != k_ra8_ok) {
    (void)ra8_comic_close(&s_ct_comic);
    return r;
  }
  ra8_jof_info_t info = {};
  if (ra8_comic_tiles_info(&s_ct_reader, &info) != k_ra8_ok) {
    (void)ra8_comic_close(&s_ct_comic);
    return r;
  }
  uint32_t crc = 0U;
  r.ok         = cm_ct_hash_tiles(&info, &crc);
  r.w          = (uint32_t)info.width;
  r.h          = (uint32_t)info.height;
  r.tiles      = info.tile_count;
  r.crc        = r.ok ? crc : 0U;
  (void)ra8_comic_close(&s_ct_comic);
  return r;
}
