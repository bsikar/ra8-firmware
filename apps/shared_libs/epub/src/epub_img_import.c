/**
 * @file epub_img_import.c
 * @brief Import-time transcode wiring: manifest href -> JOF atlas -> binder
 *        (#231's open-path integration).
 *
 * @details
 * Implements `epub_tile_binder_import()`: resolve a manifest image href,
 * register it in place when the entry already is a stored JOF atlas, or
 * stream its encoded JPEG/PNG/WebP bytes through the bounded transcode producer
 * (`jof_produce`) into the caller's atlas store and register the
 * result. Every source codec normalizes to the one JOF container on import
 * (#290). Either way the binder afterwards serves the image's full-resolution
 * tiles decode-on-demand -- the #231 goal for pages larger than SDRAM.
 *
 * Guarded on `__has_include` exactly like the binder unit so epub-only apps
 * still link with this TU empty.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / EPUB]
 * {World: NS}
 */

#if __has_include("ra8_tile_cache.h") && __has_include("jof.h")

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "epub.h"
#include "epub_entry.h"
#include "epub_img_tiles.h"
#include "jof.h"
#include "jof_produce.h"
#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"

/**
 * @enum epub_import_const_t
 * @brief Import-path sniffing constants.
 */
typedef enum : uint8_t {
  k_epub_import_magic_len = 4U, /**< Atlas magic length sniffed. */
} epub_import_const_t;

/**
 * @struct epub_entry_pull_t
 * @brief ::jof_pull_fn adapter over a streaming entry cursor.
 *
 * @details Wraps `epub_entry_read` so the producer can pull the encoded
 *          source image straight off the archive (deflated or stored) in
 *          bounded chunks.
 *
 * @invariant `reader` stays open for the adapter's lifetime.
 */
typedef struct {
  epub_entry_reader_t reader; /**< Open entry cursor. */
} epub_entry_pull_t;

/**
 * @brief Pull adapter: stream the next entry bytes to the producer.
 * @details Delegates each chunk to the open cursor; end-of-entry surfaces as a 0-byte read.
 * @param[in]  ctx A ::epub_entry_pull_t.
 * @param[out] buf Destination buffer.
 * @param[in]  cap Capacity of @p buf.
 * @param[out] got Bytes delivered (0 at end of entry).
 * @return Result code from the entry cursor.
 * @retval k_ra8_ok Chunk delivered (or clean end of entry).
 * @pre @p ctx holds an open cursor.
 * @pre @p buf holds @p cap writable bytes.
 * @post `*got <= cap` entry bytes were delivered in order.
 * @post On any error the transcode aborts.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_entry_pull(void* ctx, uint8_t* buf, size_t cap, size_t* got)
{
  epub_entry_pull_t* pull = (epub_entry_pull_t*)ctx;
  return epub_entry_read(&pull->reader, buf, cap, got);
}

/**
 * @brief Classify an entry: stored JOF atlas (register in place) or not.
 * @details A 4-byte positioned read succeeds only for *stored* entries; a
 *          DEFLATE-compressed entry reports `k_ra8_err_not_supported`, which
 *          classifies as "not an in-place atlas" (the transcode path).
 * @param[in]  book     Open book.
 * @param[in]  href     Entry path.
 * @param[out] out_is_atlas Receives true when the entry starts with "JOF1".
 * @return Result code.
 * @retval k_ra8_ok            Classified (see @p out_is_atlas).
 * @retval k_ra8_err_not_found No entry at @p href.
 * @retval other               Propagated reader failure.
 * @pre @p book is open; @p href is NUL-terminated.
 * @pre @p out_is_atlas is writable.
 * @post `*out_is_atlas` is meaningful only on k_ra8_ok.
 * @post No entry cursor remains open.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_classify(epub_book_t* book, const char* href, bool* out_is_atlas)
{
  /** @brief JOF header magic (mirrors the atlas reader unit). */
  static const uint8_t s_atlas_magic[k_epub_import_magic_len] = {'J', 'O', 'F', '1'};
  *out_is_atlas                                               = false;
  uint8_t         magic[k_epub_import_magic_len]              = {};
  size_t          got                                         = 0U;
  const ra8_err_t err = epub_entry_pread(book, href, 0U, magic, sizeof(magic), &got);
  if (err == k_ra8_err_not_supported) {
    return k_ra8_ok; /* deflated entry: cannot be an in-place atlas */
  }
  if (err != k_ra8_ok) {
    return err;
  }
  if ((got == sizeof(magic)) && (memcmp(magic, s_atlas_magic, sizeof(magic)) == 0)) {
    *out_is_atlas = true;
  }
  return k_ra8_ok;
}

/**
 * @brief Run the producer over an open entry cursor into the atlas store.
 * @details Builds the producer configuration from the import knobs, streams
 *          the entry, and always closes the cursor (the first error wins).
 * @param[in]  book     Open book.
 * @param[in]  href     Source image entry path.
 * @param[in]  cfg      Import configuration (knobs + arena + store).
 * @param[out] out_info Receives the produced atlas geometry.
 * @return Result code.
 * @retval k_ra8_ok Atlas produced into the store.
 * @retval other    Propagated from the cursor / producer / store.
 * @pre @p cfg->work is sized per `jof_work_bytes()`.
 * @pre @p href resolves to an archive entry.
 * @post On success the store holds one complete atlas.
 * @post No entry cursor remains open.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_transcode(epub_book_t*                   book,
                                    const char*                    href,
                                    const epub_atlas_import_cfg_t* cfg,
                                    jof_info_t*                    out_info)
{
  epub_entry_pull_t pull = {};
  uint64_t          size = 0U;
  ra8_err_t         err  = epub_entry_open(book, href, &pull.reader, &size);
  if (err != k_ra8_ok) {
    return err;
  }
  const jof_produce_cfg_t pcfg = {
    .pull          = internal_entry_pull,
    .pull_ctx      = &pull,
    .sink          = cfg->store.sink,
    .sink_ctx      = cfg->store.sink_ctx,
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
  err                       = jof_produce(&pcfg, out_info);
  const ra8_err_t close_err = epub_entry_close(&pull.reader);
  return (err != k_ra8_ok) ? err : close_err;
}

/**
 * @brief Reject any NULL import argument (pointer guards, split out).
 * @details Split out so the public entry stays under the statement budget.
 * @param[in] binder Binder pointer to validate.
 * @param[in] book   Book pointer to validate.
 * @param[in] href   Href pointer to validate.
 * @param[in] cfg    Import configuration to validate (incl. the store seams).
 * @return Result code.
 * @retval k_ra8_ok           Every required pointer is non-NULL.
 * @retval k_ra8_err_null_ptr Some pointer is NULL.
 * @pre The caller forwards its own arguments.
 * @pre Only @p cfg is dereferenced (after its own check).
 * @post No state mutated.
 * @post Return depends solely on the inputs.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_import_args_ok(const epub_tile_binder_t*      binder,
                                         const epub_book_t*             book,
                                         const char*                    href,
                                         const epub_atlas_import_cfg_t* cfg)
{
  static const char* const s_tag = "epub_img_import";
  RA8_CHECK_NULL_PTR(binder, s_tag, "binder must not be nullptr");
  RA8_CHECK_NULL_PTR(book, s_tag, "book must not be nullptr");
  RA8_CHECK_NULL_PTR(href, s_tag, "href must not be nullptr");
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  RA8_CHECK_NULL_PTR(cfg->store.sink, s_tag, "store.sink must not be nullptr");
  RA8_CHECK_NULL_PTR(cfg->store.pread, s_tag, "store.pread must not be nullptr");
  return k_ra8_ok;
}

ra8_err_t epub_tile_binder_import(epub_tile_binder_t*            binder,
                                  epub_book_t*                   book,
                                  const char*                    href,
                                  uint32_t                       image_id,
                                  const epub_atlas_import_cfg_t* cfg)
{
  const ra8_err_t nz = internal_import_args_ok(binder, book, href, cfg);
  if (nz != k_ra8_ok) {
    return nz;
  }
  const size_t hlen = strlen(href);
  if ((hlen == 0U) || (hlen >= (size_t)k_epub_max_path_len)) {
    return k_ra8_err_invalid_arg;
  }
  bool      is_atlas = false;
  ra8_err_t err      = internal_classify(book, href, &is_atlas);
  if (err != k_ra8_ok) {
    return err;
  }
  if (is_atlas) {
    return epub_tile_binder_add(binder, book, href, image_id); /* in-place */
  }
  jof_info_t info = {};
  err             = internal_transcode(book, href, cfg, &info);
  if (err != k_ra8_ok) {
    return err;
  }
  return epub_tile_binder_add_ext(binder,
                                  cfg->store.pread,
                                  cfg->store.pread_ctx,
                                  (uint64_t)info.total_size,
                                  image_id);
}

#else
/* ra8_mem tile cache / atlas producer not on the include path for this app:
 * the import wiring is unused here. A single typedef keeps the translation
 * unit non-empty. */
typedef int epub_img_import_unused_translation_unit_t;
#endif /* __has_include("ra8_tile_cache.h") && __has_include("jof.h") */
