/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/ereader_shelf/src/sh_paged.c
 * @brief Demand-paged .rabook backing: chunk reader + page cache + source bind.
 *
 * @details
 * The #204/#205 open path. Every `.rabook` book -- baked MRAM blob or SD file --
 * is opened through the chunked "RBKC" reader (ra8_book_chunked) and bound as an
 * ::ra8_book_src_paged source over an ::ra8_vmem page cache. There is deliberately
 * NO resident/paged size threshold: a small book simply never evicts anything
 * from the generously-sized frame pool (so it behaves like the old resident
 * fast path), while a book far larger than the pool evicts normally through the
 * same code. One code path, no threshold tuning.
 *
 * This module owns every static buffer the paged stack needs:
 *   - the SDRAM frame pool (the budget the old whole-book inflate scratch used),
 *   - the compressed-chunk staging buffer,
 *   - the resident chunk table, the cache metadata + hash buckets,
 *   - the single-slot ::ra8_vsource registry and the ::ra8_vmem cache.
 *
 * The container's `chunk_bytes` is validated at open to equal the cache's
 * `frame_bytes` (one chunk == one frame == one inflate; the contract
 * ra8_book_chunked_read relies on).
 *
 *
 * [Ring 6 / App] {World: NS}
 *
 * @since 0.1.0
 */
#include <string.h>

#include "ra8_book_chunked.h"
#include "ra8_check.h"
#include "ra8_vmem.h"
#include "ra8_vsource.h"
#include "sh_app.h"

/**
 * @enum sh_paged_dim_t
 * @brief Paged-stack buffer budgets.
 * @details `k_shp_frame_bytes` MUST equal the RBKC `chunk_bytes` the book
 *          compiler emits (tools/epub_compile `--chunk-bytes`, default 64 KiB);
 *          a container compiled with any other chunk size is rejected at open.
 *          The frame pool repurposes the 24 MiB SDRAM budget the retired
 *          whole-book inflate scratch (`s_scratch` in main.c) used to hold:
 *          384 frames x 64 KiB = 24 MiB. The chunk-table budget caps the
 *          largest openable book at 1024 chunks = 64 MiB inflated (1025
 *          entries; a bigger book fails ra8_book_chunked_open honestly).
 *          The staging buffer must cover the largest compressed chunk; 72 KiB
 *          exceeds both zlib's compressBound(64 KiB) (~65.6 KiB) and miniz's
 *          more conservative mz_compressBound(64 KiB) (~70.6 KiB).
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_shp_frame_bytes   = 64U * 1024U, /**< ra8_vmem frame size == container chunk_bytes. */
  k_shp_frame_count   = 384U,        /**< 384 x 64 KiB = 24 MiB SDRAM frame pool.       */
  k_shp_bucket_count  = 512U,        /**< Cache hash buckets (~1.3x frames, pow2).      */
  k_shp_table_entries = 1025U,       /**< chunk_count+1 budget: 64 MiB inflated cap.    */
  k_shp_staging_bytes = 72U * 1024U, /**< >= compressBound(chunk) for zlib and miniz.   */
} sh_paged_dim_t;

/** @brief SDRAM page-frame pool (the old inflate-scratch budget, repurposed). */
[[gnu::section(".sdram_data"),
  gnu::aligned(8)]] static uint8_t s_shp_frames[k_shp_frame_count * k_shp_frame_bytes];

/** @brief SDRAM staging buffer for one compressed chunk's zlib stream. */
[[gnu::section(".sdram_data"), gnu::aligned(8)]] static uint8_t s_shp_staging[k_shp_staging_bytes];

/** @brief Resident chunk table (`chunk_count + 1` stream offsets). */
static uint64_t s_shp_table[k_shp_table_entries];

/** @brief Per-frame cache metadata (parallel to the frame pool). */
static ra8_vmem_frame_t s_shp_meta[k_shp_frame_count];

/** @brief Per-frame cache key storage (parallel to the frame pool). */
static ra8_vmem_key_t s_shp_keys[k_shp_frame_count];

/** @brief Cache hash-bucket heads. */
static int32_t s_shp_buckets[k_shp_bucket_count];

/** @brief The open book's chunk reader (unbound when no rabook is open). */
static ra8_book_chunked_t s_shp_rd;

/** @brief Single-slot object registry: object 0 is the open book. */
static ra8_vsource_obj_t s_shp_obj;

/** @brief Source registry fronting ::s_shp_obj. */
static ra8_vsource_t s_shp_vs;

/** @brief The page cache the bound ::ra8_book_src_t reads through. */
static ra8_vmem_t s_shp_vm;

/**
 * @struct sh_paged_mram_t
 * @brief Memory-mapped container backing (baked books).
 */
typedef struct {
  const uint8_t* data; /**< First container byte (MRAM, memory-mapped). */
  uint64_t       len;  /**< Container length in bytes.                  */
} sh_paged_mram_t;

/** @brief Backing context while an MRAM (baked) book is open. */
static sh_paged_mram_t s_shp_mram;

/** @brief Log tag for paged-open diagnostics. */
static const char* const s_shp_tag = "sh_paged";

/** @brief ra8_vsource_read_fn over a memory-mapped (MRAM) container blob. */
static ra8_err_t sh_paged_mram_read(void* ctx, uint64_t offset, uint8_t* buf, uint32_t len)
{
  RA8_CHECK_NULL_PTR(ctx, s_shp_tag, "mram: null ctx");
  RA8_CHECK_NULL_PTR(buf, s_shp_tag, "mram: null buf");
  const sh_paged_mram_t* m = (const sh_paged_mram_t*)ctx;
  if ((offset + (uint64_t)len) > m->len) {
    return k_ra8_err_out_of_range;
  }
  (void)memcpy(buf, &m->data[offset], (size_t)len);
  return k_ra8_ok;
}

/** @brief Does the header extent [off, off + count*elem) fit inside @p total? */
static bool sh_paged_extent_ok(uint32_t off, uint32_t count, uint32_t elem, uint32_t total)
{
  const uint64_t end = (uint64_t)off + ((uint64_t)count * (uint64_t)elem);
  if (off > total) {
    return false;
  }
  return end <= (uint64_t)total;
}

/**
 * @brief Validate the paged source's header copy (no whole-blob CRC).
 * @details Checks the magic, format version, that the header's `total_size`
 *          equals the container's inflated total, and that every table/pool
 *          extent lies inside the blob -- the same shape checks
 *          ra8_book_validate() runs, minus its body CRC-32. Verifying that CRC
 *          here would fault every chunk in (a full-book read), which is exactly
 *          what always-paging avoids, so it is honestly NOT checked in this
 *          mode: integrity is instead carried by the RBKC geometry checks at
 *          open, the zlib Adler-32 that sh_inflate/tinfl verifies on EVERY
 *          chunk as it faults in, and the exact inflated-span check in
 *          ra8_book_chunked_read.
 */
static bool sh_paged_hdr_ok(const ra8_book_header_t* hdr, uint64_t inflated_total)
{
  const char k_magic[8] = {'R', 'A', 'B', 'O', 'O', 'K', '1', '\0'};
  if (memcmp(hdr->magic, k_magic, sizeof k_magic) != 0) {
    return false;
  }
  if (hdr->format_version != (uint32_t)k_ra8_book_format_version) {
    return false;
  }
  if ((uint64_t)hdr->total_size != inflated_total) {
    return false;
  }
  if (hdr->total_size < (uint32_t)sizeof(ra8_book_header_t)) {
    return false;
  }
  const struct {
    uint32_t off;   /**< Extent start offset.        */
    uint32_t count; /**< Element count (or bytes).   */
    uint32_t elem;  /**< Element size (1 for pools). */
  } k_ext[] = {
    {hdr->chapter_off, hdr->chapter_count, (uint32_t)sizeof(ra8_book_chapter_t)},
    {hdr->node_off, hdr->node_count, (uint32_t)sizeof(ra8_book_node_t)},
    {hdr->attr_off, hdr->attr_count, (uint32_t)sizeof(ra8_book_attr_t)},
    {hdr->stylesheet_off, hdr->stylesheet_count, (uint32_t)sizeof(ra8_book_stylesheet_t)},
    {hdr->image_off, hdr->image_count, (uint32_t)sizeof(ra8_book_image_t)},
    {hdr->string_off, hdr->string_size, 1U},
    {hdr->image_pool_off, hdr->image_pool_size, 1U},
  };
  for (size_t i = 0U; i < (sizeof(k_ext) / sizeof(k_ext[0])); ++i) {
    if (!sh_paged_extent_ok(k_ext[i].off, k_ext[i].count, k_ext[i].elem, hdr->total_size)) {
      return false;
    }
  }
  return true;
}

/** @brief Register the bound chunk reader, init the cache, bind g_sh.book_src. */
static bool sh_paged_bind(void)
{
  if (ra8_vsource_init(&s_shp_vs, &s_shp_obj, 1U) != k_ra8_ok) {
    return false;
  }
  uint32_t oid = 0U;
  if (ra8_vsource_add_paged(&s_shp_vs,
                            ra8_book_chunked_read,
                            &s_shp_rd,
                            0U,
                            s_shp_rd.inflated_total,
                            &oid) != k_ra8_ok) {
    return false;
  }
  const ra8_vmem_cfg_t cfg = {
    .frame_mem    = s_shp_frames,
    .frame_bytes  = (uint32_t)k_shp_frame_bytes,
    .frame_count  = (uint32_t)k_shp_frame_count,
    .meta         = s_shp_meta,
    .keys         = s_shp_keys,
    .buckets      = s_shp_buckets,
    .bucket_count = (uint32_t)k_shp_bucket_count,
    .loader       = ra8_vsource_loader,
    .loader_ctx   = &s_shp_vs,
  };
  s_shp_vm = (ra8_vmem_t){};
  if (ra8_vmem_init(&s_shp_vm, &cfg) != k_ra8_ok) {
    return false;
  }
  /* Binding reads the 100-byte header through the cache (faults chunk 0 in). */
  if (ra8_book_src_paged(&g_sh.book_src,
                         &s_shp_vm,
                         oid,
                         (uint32_t)k_shp_frame_bytes,
                         (uint32_t)s_shp_rd.inflated_total) != k_ra8_ok) {
    return false;
  }
  return sh_paged_hdr_ok(&g_sh.book_src.hdr, s_shp_rd.inflated_total);
}

bool sh_paged_open(ra8_vsource_read_fn file_read, void* file_ctx, uint64_t file_len)
{
  /* Unbind any previous source, but do NOT sh_paged_close() here: the caller
   * has already torn the old book down (sh_book_open), and a full close would
   * wipe the MRAM context / close the SD file that @p file_read is about to
   * read from. The failure paths below do close, releasing the SD backing. */
  g_sh.book_src = (ra8_book_src_t){};
  s_shp_rd      = (ra8_book_chunked_t){};
  if ((file_read == nullptr) || (file_len == 0U)) {
    sh_paged_close();
    return false;
  }
  if (ra8_book_chunked_open(&s_shp_rd,
                            file_read,
                            file_ctx,
                            file_len,
                            sh_inflate,
                            s_shp_table,
                            (uint32_t)k_shp_table_entries,
                            s_shp_staging,
                            (uint32_t)k_shp_staging_bytes) != k_ra8_ok) {
    sh_paged_close();
    return false;
  }
  /* One chunk == one cache frame: the read callback's contract. A container
   * compiled with a different --chunk-bytes cannot be paged through this
   * cache, so reject it here instead of failing obscurely per read. */
  if (s_shp_rd.chunk_bytes != (uint32_t)k_shp_frame_bytes) {
    sh_paged_close();
    return false;
  }
  /* ra8_book_src_t carries a uint32 size; the table budget already caps books
   * at 64 MiB, so this guard is belt-and-braces against a lying header. */
  if (s_shp_rd.inflated_total > (uint64_t)UINT32_MAX) {
    sh_paged_close();
    return false;
  }
  if (!sh_paged_bind()) {
    sh_paged_close();
    return false;
  }
  return true;
}

bool sh_paged_open_mram(const uint8_t* blob, uint32_t blob_len)
{
  if (blob == nullptr) {
    return false;
  }
  if (blob_len == 0U) {
    return false;
  }
  s_shp_mram = (sh_paged_mram_t){.data = blob, .len = (uint64_t)blob_len};
  return sh_paged_open(sh_paged_mram_read, &s_shp_mram, (uint64_t)blob_len);
}

void sh_paged_close(void)
{
  g_sh.book_src = (ra8_book_src_t){};
  s_shp_rd      = (ra8_book_chunked_t){};
  s_shp_mram    = (sh_paged_mram_t){};
  sh_sd_book_close(); /* release the held SD file handle, if any */
}
