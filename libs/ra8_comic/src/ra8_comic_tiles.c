/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_comic_tiles.c
 * @brief Tile an oversized comic page: encoded bytes -> JOF atlas -> tile cache
 *        (#344). Import-time transcode + decode-on-demand tile paging.
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * Implements ::ra8_comic_tile_reader_t. On import the page's encoded bytes are
 * streamed through `ra8_jof_produce()` into a caller memstore, the produced
 * atlas is validated with `ra8_jof_parse()`, and bound as the reader's single
 * source under a fresh epoch. ::ra8_comic_tiles_tile then pages the page's
 * full-resolution tiles through the owned ::ra8_tile_cache, decode-on-miss via
 * `ra8_jof_read_tile()` -- one bounded read plus one bounded inflate per tile.
 * The whole page is never resident; the resident cost is the cache cell budget.
 *
 * The size-threshold front end (::ra8_comic_tiles_footprint /
 * ::ra8_comic_tiles_over_budget) keeps small pages on the caller's existing
 * whole-decode fast path -- tiling a page that already fits saves nothing.
 *
 * Guarded on `__has_include`: a comic app that never pulls in `ra8_mem` /
 * `ra8_jof` still links this TU empty (a comic-only reader keeps the
 * whole-decode path and never references the tile binder).
 */

/* Compile the tile reader only where the ra8_mem tile cache and the JOF atlas
 * reader/producer are on the include path (tiling comic apps + the host test
 * build). Otherwise empty. */
#if __has_include("ra8_tile_cache.h") && __has_include("ra8_jof.h")

#include "ra8_comic_tiles.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_jof.h"
#include "ra8_jof_produce.h"
#include "ra8_tile_cache.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_comic_tiles";

/**
 * @struct ra8_comic_tiles_pull_t
 * @brief ::ra8_jof_pull_fn adapter over an in-memory encoded page.
 * @details The comic page's encoded bytes are already extracted into a caller
 *          buffer by `ra8_comic_page_read`; this feeds them to the producer's
 *          forward, single-pass pull without a second copy.
 * @invariant `off <= n` at all times.
 * @since Version 0.1.0
 */
typedef struct {
  const uint8_t* d;   /**< Encoded page bytes.      */
  size_t         n;   /**< Encoded byte count.      */
  size_t         off; /**< Bytes already delivered. */
} ra8_comic_tiles_pull_t;

/**
 * @brief Pull adapter: deliver the next encoded page bytes to the producer.
 * @details Copies at most @p cap bytes from the remaining source; a 0-byte
 *          delivery signals a clean end of stream (single forward pass).
 * @param[in]  ctx A ::ra8_comic_tiles_pull_t.
 * @param[out] buf Destination buffer.
 * @param[in]  cap Capacity of @p buf.
 * @param[out] got Bytes delivered (0 at end of source).
 * @return Always k_ra8_ok (a memory source cannot fail).
 * @retval k_ra8_ok Chunk delivered (or clean end of source).
 * @pre @p ctx holds a bound memory source; @p buf holds @p cap writable bytes.
 * @pre @p got is writable.
 * @post `*got <= cap` and the cursor advanced by `*got`.
 * @post At end of source `*got == 0`.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_mem_pull(void* ctx, uint8_t* buf, size_t cap, size_t* got)
{
  ra8_comic_tiles_pull_t* p      = (ra8_comic_tiles_pull_t*)ctx;
  const size_t            remain = p->n - p->off;
  const size_t            k      = (cap < remain) ? cap : remain;
  if (k > 0U) {
    (void)memcpy(buf, &p->d[p->off], k);
  }
  p->off += k;
  *got = k;
  return k_ra8_ok;
}

/**
 * @brief Tile-cache decode-on-miss trampoline (::ra8_tile_decode_fn).
 * @details Decodes the keyed tile off the reader's single bound atlas via
 *          `ra8_jof_read_tile()` (memstore pread). A tile or its stored stream
 *          exceeding the cell/scratch budget maps to `k_ra8_err_no_mem`,
 *          preserving the reader's documented contract.
 * @param[in]  ctx        The owning ::ra8_comic_tile_reader_t.
 * @param[in]  key        Tile to decode (`image_id` carries the reader epoch).
 * @param[out] cell       Destination cell.
 * @param[in]  cell_bytes Cell capacity, bytes.
 * @param[out] out_w      Decoded tile width.
 * @param[out] out_h      Decoded tile height.
 * @return Result code from the atlas read (or a mapping thereof).
 * @retval k_ra8_ok The tile was decoded into @p cell.
 * @pre @p ctx is the owning reader with a page bound.
 * @pre @p cell holds @p cell_bytes bytes.
 * @post On success `*out_w`/`*out_h` hold the (possibly clamped) tile size.
 * @post On failure the cell is not trusted.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_tile_decode(void*                 ctx,
                                  const ra8_tile_key_t* key,
                                  uint8_t*              cell,
                                  uint32_t              cell_bytes,
                                  uint16_t*             out_w,
                                  uint16_t*             out_h)
{
  ra8_comic_tile_reader_t* r   = (ra8_comic_tile_reader_t*)ctx;
  const ra8_err_t          err = ra8_jof_read_tile(ra8_jof_memstore_pread,
                                                   &r->store,
                                                   &r->info,
                                                   key->tile_x,
                                                   key->tile_y,
                                                   r->scratch,
                                                   r->scratch_cap,
                                                   cell,
                                                   cell_bytes,
                                                   out_w,
                                                   out_h);
  if (err == k_ra8_err_invalid_size) {
    return k_ra8_err_no_mem; /* tile exceeds the cell / scratch budget */
  }
  return err;
}

ra8_err_t ra8_comic_tiles_footprint(const uint8_t* enc,
                                    size_t         len,
                                    uint16_t*      out_w,
                                    uint16_t*      out_h,
                                    uint64_t*      out_decoded_bytes)
{
  RA8_CHECK_NULL_PTR(enc, s_tag, "footprint: null enc");
  RA8_CHECK_NULL_PTR(out_w, s_tag, "footprint: null out_w");
  RA8_CHECK_NULL_PTR(out_h, s_tag, "footprint: null out_h");
  RA8_CHECK_NULL_PTR(out_decoded_bytes, s_tag, "footprint: null out_decoded_bytes");
  if (len == 0U) {
    return k_ra8_err_invalid_size;
  }
  uint16_t        w   = 0U;
  uint16_t        h   = 0U;
  const ra8_err_t err = ra8_jof_probe_dims(enc, len, &w, &h);
  if (err != k_ra8_ok) {
    return err;
  }
  *out_w             = w;
  *out_h             = h;
  *out_decoded_bytes = (uint64_t)w * (uint64_t)h * (uint64_t)k_ra8_comic_tiles_decoded_bpp;
  return k_ra8_ok;
}

bool ra8_comic_tiles_over_budget(uint64_t decoded_bytes, uint64_t budget_bytes)
{
  return (budget_bytes != 0U) && (decoded_bytes > budget_bytes);
}

ra8_err_t ra8_comic_tiles_init(ra8_comic_tile_reader_t*    r,
                               const ra8_tile_cache_cfg_t* storage,
                               uint8_t*                    scratch,
                               uint32_t                    scratch_cap)
{
  RA8_CHECK_NULL_PTR(r, s_tag, "init: null r");
  RA8_CHECK_NULL_PTR(storage, s_tag, "init: null storage");
  (void)memset(r, 0, sizeof(*r));
  r->scratch               = scratch;
  r->scratch_cap           = (scratch != nullptr) ? scratch_cap : 0U;
  ra8_tile_cache_cfg_t cfg = *storage;
  cfg.decode               = priv_tile_decode;
  cfg.decode_ctx           = r;
  return ra8_tile_cache_init(&r->cache, &cfg);
}

/**
 * @brief Reject any NULL ::ra8_comic_tiles_import argument (guards, split out).
 * @details Split so the public entry stays under the NASA-Rule-4 statement
 *          budget; validates the reader, source, config and its mandatory
 *          arena/store pointers.
 * @param[in] r   Reader pointer to validate.
 * @param[in] enc Encoded-source pointer to validate.
 * @param[in] cfg Import configuration to validate (incl. work + atlas).
 * @return Result code.
 * @retval k_ra8_ok           Every required pointer is non-NULL.
 * @retval k_ra8_err_null_ptr Some pointer is NULL.
 * @pre The caller forwards its own arguments.
 * @pre Only @p cfg is dereferenced (after its own check).
 * @post No state mutated.
 * @post Return depends solely on the inputs.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_import_args_ok(const ra8_comic_tile_reader_t*      r,
                                     const uint8_t*                      enc,
                                     const ra8_comic_tiles_import_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(r, s_tag, "import: null r");
  RA8_CHECK_NULL_PTR(enc, s_tag, "import: null enc");
  RA8_CHECK_NULL_PTR(cfg, s_tag, "import: null cfg");
  RA8_CHECK_NULL_PTR(cfg->work, s_tag, "import: null work");
  RA8_CHECK_NULL_PTR(cfg->atlas, s_tag, "import: null atlas");
  return k_ra8_ok;
}

/**
 * @brief Stream the encoded page through the producer into the atlas store.
 * @details Builds the producer configuration from the import knobs, resets the
 *          reader's memstore over @p cfg->atlas, produces the JOF atlas, and
 *          re-validates it with `ra8_jof_parse()` into `r->info`.
 * @param[in,out] r       Reader whose store + info this fills.
 * @param[in]     enc     Encoded page bytes.
 * @param[in]     enc_len Length of @p enc (> 0).
 * @param[in]     cfg     Import configuration (knobs + arena + store).
 * @return Result code.
 * @retval k_ra8_ok Atlas produced + validated into `r->store` / `r->info`.
 * @retval other    Propagated from the producer / parse.
 * @pre @p cfg->work is sized per `ra8_jof_work_bytes()`.
 * @pre @p enc holds @p enc_len readable bytes.
 * @post On success `r->store` holds one complete atlas and `r->info` is parsed.
 * @post On any error `r->store` holds a partial atlas (must not be trusted).
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_transcode(ra8_comic_tile_reader_t*            r,
                                const uint8_t*                      enc,
                                size_t                              enc_len,
                                const ra8_comic_tiles_import_cfg_t* cfg)
{
  r->store = (ra8_jof_memstore_t){.buf = cfg->atlas, .cap = cfg->atlas_cap, .len = 0U};
  ra8_comic_tiles_pull_t      pull = {.d = enc, .n = enc_len, .off = 0U};
  const ra8_jof_produce_cfg_t pcfg = {
    .pull          = priv_mem_pull,
    .pull_ctx      = &pull,
    .sink          = ra8_jof_memstore_sink,
    .sink_ctx      = &r->store,
    .tile_w        = cfg->tile_w,
    .tile_h        = cfg->tile_h,
    .codec         = cfg->codec,
    .max_width     = cfg->max_width,
    .max_height    = cfg->max_height,
    .work          = cfg->work,
    .work_cap      = cfg->work_cap,
    .webp_work     = cfg->webp_work,
    .webp_work_cap = cfg->webp_work_cap,
  };
  ra8_jof_info_t  produced = {};
  const ra8_err_t perr     = ra8_jof_produce(&pcfg, &produced);
  if (perr != k_ra8_ok) {
    return perr;
  }
  return ra8_jof_parse(ra8_jof_memstore_pread, &r->store, (uint64_t)r->store.len, &r->info);
}

ra8_err_t ra8_comic_tiles_import(ra8_comic_tile_reader_t*            r,
                                 const uint8_t*                      enc,
                                 size_t                              enc_len,
                                 const ra8_comic_tiles_import_cfg_t* cfg)
{
  const ra8_err_t nz = priv_import_args_ok(r, enc, cfg);
  if (nz != k_ra8_ok) {
    return nz;
  }
  if (enc_len == 0U) {
    return k_ra8_err_invalid_size;
  }
  r->bound            = false; /* invalidate first: any error leaves no bound page */
  const ra8_err_t err = priv_transcode(r, enc, enc_len, cfg);
  if (err != k_ra8_ok) {
    return err;
  }
  r->epoch += 1U;
  r->bound = true;
  return k_ra8_ok;
}

ra8_err_t ra8_comic_tiles_info(const ra8_comic_tile_reader_t* r, ra8_jof_info_t* out_info)
{
  RA8_CHECK_NULL_PTR(r, s_tag, "info: null r");
  RA8_CHECK_NULL_PTR(out_info, s_tag, "info: null out_info");
  if (!r->bound) {
    return k_ra8_err_invalid_state;
  }
  *out_info = r->info;
  return k_ra8_ok;
}

ra8_err_t ra8_comic_tiles_tile(ra8_comic_tile_reader_t* r,
                               uint16_t                 tile_x,
                               uint16_t                 tile_y,
                               ra8_tile_t*              out_tile)
{
  RA8_CHECK_NULL_PTR(r, s_tag, "tile: null r");
  RA8_CHECK_NULL_PTR(out_tile, s_tag, "tile: null out_tile");
  if (!r->bound) {
    return k_ra8_err_invalid_state;
  }
  if (tile_x >= r->info.tile_cols) {
    return k_ra8_err_out_of_range;
  }
  if (tile_y >= r->info.tile_rows) {
    return k_ra8_err_out_of_range;
  }
  const ra8_tile_key_t key = {.image_id = r->epoch,
                              .tile_x   = tile_x,
                              .tile_y   = tile_y,
                              .zoom     = 0U,
                              .reserved = 0U};
  return ra8_tile_cache_get(&r->cache, &key, out_tile);
}

ra8_err_t ra8_comic_tiles_release(ra8_comic_tile_reader_t* r, const uint8_t* pixels)
{
  RA8_CHECK_NULL_PTR(r, s_tag, "release: null r");
  RA8_CHECK_NULL_PTR(pixels, s_tag, "release: null pixels");
  return ra8_tile_cache_put(&r->cache, pixels);
}

#else
/* ra8_mem tile cache / JOF atlas reader not on the include path for this app:
 * the comic tile reader is unused here (the whole-decode path stands alone). A
 * single typedef keeps the translation unit non-empty. */
typedef int ra8_comic_tiles_unused_translation_unit_t;
#endif /* __has_include("ra8_tile_cache.h") && __has_include("ra8_jof.h") */
