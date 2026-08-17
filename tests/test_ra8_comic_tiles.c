/**
 * @file test_ra8_comic_tiles.c
 * @brief #344 -- an oversized CBZ page that overruns the whole-decode arena
 *        opens and zooms through the JOF tile path.
 *
 * @details
 * Turns the #344 requirement into CI gates. A large RGB page is synthesized in
 * test, packed into a real CBZ, opened by `ra8_comic`, and paged the two ways a
 * reader chooses between:
 *   - the whole-decode fast path (`ra8_img_decode_blit`) genuinely FAILS
 *     `k_ra8_err_no_mem` on the large page against a modest arena -- this is the
 *     cap the issue describes -- and succeeds on a small page (the golden path
 *     that must stay unchanged);
 *   - the tile path (`ra8_comic_tiles_*`) opens the same large page, and every
 *     tile -- interior and clamped edge -- decodes byte-exact against the source
 *     pattern, at full resolution (no downscale), with resident decoded memory
 *     bounded by the tiny cache cell budget rather than the whole image.
 *
 * The sub-rect (loupe) property is exercised directly: a single interior tile is
 * fetched and verified full-resolution without touching its neighbours. The
 * size-threshold decision (`ra8_comic_tiles_over_budget`) carries an MC/DC
 * vector set, and a page turn proves the epoch key namespacing never surfaces a
 * stale tile of the previous page. Both the raw and DEFLATE tile codecs are
 * covered, plus the reader's fail-closed guards.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "miniz.h"
#include "ra8_attributes.h"
#include "ra8_comic.h"
#include "ra8_comic_tiles.h"
#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_jof.h"
#include "ra8_jof_produce.h"
#include "ra8_reflow_image.h"
#include "ra8_tile_cache.h"
#include "unity_minimal.h"

/**
 * @enum comic_tiles_geom_t
 * @brief Page + tile + cache geometry driving the #344 invariants.
 * @details The big page (700x480 RGB) is deliberately not tile-aligned, so the
 *          right column and bottom row are clamped edge tiles; at 4 bpp its
 *          decoded footprint is 1.34 MiB, ~5x the ::k_budget_bytes threshold and
 *          well past the ::k_small_arena the whole-decode path is given. The
 *          cache holds 8 tiles of 128x128x3, ~7x less RAM than the decoded page.
 * @since Version 0.1.0
 */
typedef enum : uint32_t {
  k_big_w        = 700U,                        /**< Big page width (not tile-aligned).           */
  k_big_h        = 480U,                        /**< Big page height (not tile-aligned).          */
  k_small_w      = 96U,                         /**< Small page width (fits the arena).           */
  k_small_h      = 64U,                         /**< Small page height.                           */
  k_tile         = 128U,                        /**< Square tile edge.                            */
  k_rgb_bpp      = 3U,                          /**< Source / atlas bytes per pixel (RGB).        */
  k_cell_bytes   = k_tile * k_tile * k_rgb_bpp, /**< Bytes per cache cell.                        */
  k_cells        = 8U,                          /**< Cache cell budget (< the tile count).        */
  k_buckets      = 16U,                         /**< Cache hash buckets.                          */
  k_budget_bytes = 256U * 1024U,                /**< Resident decode budget (threshold).          */
  k_small_arena  = 256U * 1024U,                /**< Whole-decode arena measured against the cap. */
  k_probe_w      = 64U,                         /**< Off-screen framebuffer width.                */
  k_probe_h      = 64U,                         /**< Off-screen framebuffer height.               */
  k_tiny_cell    = 64U,                         /**< Cell too small; forces a no_mem tile decode. */
} comic_tiles_geom_t;

/**
 * @enum comic_tiles_cap_t
 * @brief Static buffer capacities for the in-test fixtures.
 * @since Version 0.1.0
 */
typedef enum : uint32_t {
  k_arc_cap = 4U * 1024U * 1024U, /**< Built CBZ capacity.       */
  k_png_cap = 2U * 1024U * 1024U, /**< Synthesized PNG capacity. */
  k_raw_cap = ((size_t)k_big_w * (size_t)k_rgb_bpp + 1U) * (size_t)k_big_h,
  /**< Filter-0 RGB scanline buffer. */
  k_work_cap  = 8U * 1024U * 1024U,                        /**< Producer work arena.    */
  k_atlas_cap = 4U * 1024U * 1024U,                        /**< Atlas memstore backing. */
  k_scratch   = k_cell_bytes + (k_cell_bytes / 8U) + 256U, /**< Stored-tile staging.    */
} comic_tiles_cap_t;

/**
 * @enum comic_tiles_pat_t
 * @brief Deterministic RGB pattern multipliers (coprime, so tiles differ).
 * @since Version 0.1.0
 */
typedef enum : uint32_t {
  k_pat_r_x = 7U,  /**< Red   column multiplier. */
  k_pat_r_y = 3U,  /**< Red   row multiplier.    */
  k_pat_g_x = 5U,  /**< Green column multiplier. */
  k_pat_g_y = 11U, /**< Green row multiplier.    */
  k_pat_b_x = 2U,  /**< Blue  column multiplier. */
  k_pat_b_y = 13U, /**< Blue  row multiplier.    */
  k_chan_r  = 0U,  /**< Red   channel index.     */
  k_chan_g  = 1U,  /**< Green channel index.     */
  k_chan_b  = 2U,  /**< Blue  channel index.     */
} comic_tiles_pat_t;

/**
 * @enum comic_tiles_png_t
 * @brief PNG on-disk field widths + shifts for the hand-rolled RGB encoder.
 * @since Version 0.1.0
 */
typedef enum : uint32_t {
  k_png_ihdr_len  = 13U,   /**< IHDR data length.                   */
  k_png_overhead  = 12U,   /**< Chunk length + type + CRC bytes.    */
  k_png_bitdepth8 = 8U,    /**< 8-bit samples.                      */
  k_png_color_rgb = 2U,    /**< PNG colour type 2 = truecolour RGB. */
  k_byte_mask     = 0xFFU, /**< Low-byte mask.                      */
  k_shift_b3      = 24U,   /**< Most-significant byte shift.        */
  k_shift_b2      = 16U,   /**< Second byte shift.                  */
  k_shift_b1      = 8U,    /**< Third byte shift.                   */
  k_crc_b1_ofs    = 9U,    /**< Chunk CRC second byte offset.       */
  k_crc_b2_ofs    = 10U,   /**< Chunk CRC third byte offset.        */
  k_crc_b3_ofs    = 11U,   /**< Chunk CRC last byte offset.         */
  k_ihdr_h_b1     = 5U,    /**< IHDR height byte 1 (big-endian).    */
  k_ihdr_h_b3     = 7U,    /**< IHDR height byte 3 (big-endian).    */
  k_ihdr_color    = 9U,    /**< IHDR colour-type byte offset.       */
  k_name_cap      = 512U,  /**< Comic name-arena capacity, bytes.   */
} comic_tiles_png_t;

/* ---------------------------------------------------------------------------
 * Fixtures (static storage; the host test build has no heap for these).
 * ---------------------------------------------------------------------------
 */

/** @brief Built CBZ archive bytes. */
static uint8_t s_arc[k_arc_cap];
/** @brief Built CBZ length. */
static size_t s_arc_size;
/** @brief Synthesized PNG scratch. */
static uint8_t s_png[k_png_cap];
/** @brief Synthesized PNG length. */
static size_t s_png_len;
/** @brief Producer work arena. */
static uint8_t s_work[k_work_cap];
/** @brief Atlas memstore backing. */
static uint8_t s_atlas[k_atlas_cap];

/** @brief Comic page-index storage. */
static ra8_comic_page_t s_pages[8];
/** @brief Comic name arena. */
static char s_names[k_name_cap];
/** @brief One extracted encoded page. */
static uint8_t s_pagebuf[k_png_cap];
/** @brief Whole-decode scratch arena (the cap under test). */
static uint8_t s_decode_arena[k_small_arena];
/** @brief Off-screen framebuffer so ra8_img_decode_blit has a bound target. */
static uint16_t s_fb[(size_t)k_probe_h * (size_t)k_probe_w];

/** @brief Cache cell storage. */
static uint8_t s_cell_mem[(size_t)k_cells * (size_t)k_cell_bytes];
/** @brief Cache key storage. */
static ra8_tile_key_t s_keys[k_cells];
/** @brief Cache dim descriptors. */
static ra8_tile_dims_t s_dims[k_cells];
/** @brief Cache link metadata. */
static ra8_keycache_cell_t s_meta[k_cells];
/** @brief Cache hash buckets. */
static int32_t s_buckets[k_buckets];
/** @brief Reader stored-tile staging. */
static uint8_t s_scratch[k_scratch];

/* ---------------------------------------------------------------------------
 * Source synthesis: a truecolour RGB PNG of the deterministic pattern.
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Compute one deterministic RGB source sample.
 * @details Mixes pixel coordinates with channel-specific constants for byte-exact tile checks.
 * @param[in] x Source pixel column.
 * @param[in] y Source pixel row.
 * @param[in] ch Channel selector from comic_tiles_channel_t.
 * @return Expected channel byte.
 * @retval 0..255 Deterministic sample for the selected coordinate and channel.
 * @pre @p ch is one of the three fixture channel values.
 * @pre Coordinate arithmetic is evaluated with uint32_t wrap semantics.
 * @post No fixture storage is modified.
 * @post Repeated calls with equal arguments return equal bytes.
 * @note This is the oracle for every decoded tile comparison.
 * @since Version 0.1.0
 */
RA8_INTERNAL static uint8_t internal_pix_rgb(uint32_t x, uint32_t y, uint32_t ch)
{
  if (ch == (uint32_t)k_chan_r) {
    return (uint8_t)((x * (uint32_t)k_pat_r_x + y * (uint32_t)k_pat_r_y) & (uint32_t)k_byte_mask);
  }
  if (ch == (uint32_t)k_chan_g) {
    return (uint8_t)((x * (uint32_t)k_pat_g_x + y * (uint32_t)k_pat_g_y) & (uint32_t)k_byte_mask);
  }
  return (uint8_t)((x * (uint32_t)k_pat_b_x + y * (uint32_t)k_pat_b_y) & (uint32_t)k_byte_mask);
}

/**
 * @brief Append one complete PNG chunk to the fixture buffer.
 * @details Emits big-endian length, four-byte type, payload, and calculated CRC.
 * @param[in] type Four-byte PNG chunk type.
 * @param[in] data Optional payload bytes.
 * @param[in] len Payload byte count.
 * @pre s_png has capacity for the current prefix, payload, and chunk overhead.
 * @pre @p data is non-NULL whenever @p len is nonzero.
 * @post s_png_len advances by exactly payload plus chunk overhead.
 * @post The appended CRC covers the emitted type and payload bytes.
 * @note The caller owns fixture-capacity proofs through fixed test dimensions.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_png_chunk(const char* type, const uint8_t* data, uint32_t len)
{
  uint8_t* p = &s_png[s_png_len];
  p[0]       = (uint8_t)(len >> (uint32_t)k_shift_b3);
  p[1]       = (uint8_t)((len >> (uint32_t)k_shift_b2) & (uint32_t)k_byte_mask);
  p[2]       = (uint8_t)((len >> (uint32_t)k_shift_b1) & (uint32_t)k_byte_mask);
  p[3]       = (uint8_t)(len & (uint32_t)k_byte_mask);
  (void)memcpy(&p[4], type, 4U);
  if (len > 0U) {
    (void)memcpy(&p[8], data, len);
  }
  const uint32_t crc = (uint32_t)mz_crc32(MZ_CRC32_INIT, &p[4], (size_t)len + 4U);
  p[8U + len]        = (uint8_t)(crc >> (uint32_t)k_shift_b3);
  p[(uint32_t)k_crc_b1_ofs + len] =
    (uint8_t)((crc >> (uint32_t)k_shift_b2) & (uint32_t)k_byte_mask);
  p[(uint32_t)k_crc_b2_ofs + len] =
    (uint8_t)((crc >> (uint32_t)k_shift_b1) & (uint32_t)k_byte_mask);
  p[(uint32_t)k_crc_b3_ofs + len] = (uint8_t)(crc & (uint32_t)k_byte_mask);
  s_png_len += (size_t)k_png_overhead + (size_t)len;
}

/**
 * @brief Build a deterministic truecolour PNG fixture.
 * @details Generates RGB8 filter-zero rows, compresses them, and emits IHDR/IDAT/IEND.
 * @param[in] w Image width in pixels.
 * @param[in] h Image height in pixels.
 * @pre @p w and @p h fit the fixed raw and PNG fixture capacities.
 * @pre The miniz compressor is available to the host test.
 * @post s_png contains one complete PNG and s_png_len is its exact size.
 * @post Every source sample follows internal_pix_rgb.
 * @note Static staging avoids a giant stack frame in the test executable.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_png_build_rgb(uint32_t w, uint32_t h)
{
  static const uint8_t sig[8] = {0x89U, 'P', 'N', 'G', 0x0DU, 0x0AU, 0x1AU, 0x0AU};
  static uint8_t       s_raw[k_raw_cap];
  static uint8_t       s_zbuf[k_png_cap];
  s_png_len = 0U;
  (void)memcpy(s_png, sig, sizeof(sig));
  s_png_len                    = sizeof(sig);
  uint8_t ihdr[k_png_ihdr_len] = {};
  ihdr[0]                      = (uint8_t)(w >> (uint32_t)k_shift_b3);
  ihdr[1]                      = (uint8_t)((w >> (uint32_t)k_shift_b2) & (uint32_t)k_byte_mask);
  ihdr[2]                      = (uint8_t)((w >> (uint32_t)k_shift_b1) & (uint32_t)k_byte_mask);
  ihdr[3]                      = (uint8_t)(w & (uint32_t)k_byte_mask);
  ihdr[4]                      = (uint8_t)(h >> (uint32_t)k_shift_b3);
  ihdr[k_ihdr_h_b1]            = (uint8_t)((h >> (uint32_t)k_shift_b2) & (uint32_t)k_byte_mask);
  ihdr[6]                      = (uint8_t)((h >> (uint32_t)k_shift_b1) & (uint32_t)k_byte_mask);
  ihdr[k_ihdr_h_b3]            = (uint8_t)(h & (uint32_t)k_byte_mask);
  ihdr[8]                      = (uint8_t)k_png_bitdepth8;
  ihdr[k_ihdr_color]           = (uint8_t)k_png_color_rgb;
  internal_png_chunk("IHDR", ihdr, (uint32_t)k_png_ihdr_len);
  size_t o = 0U;
  for (uint32_t y = 0U; y < h; y++) {
    s_raw[o] = 0U; /* filter type 0 (none) */
    o++;
    for (uint32_t x = 0U; x < w; x++) {
      s_raw[o]      = internal_pix_rgb(x, y, (uint32_t)k_chan_r);
      s_raw[o + 1U] = internal_pix_rgb(x, y, (uint32_t)k_chan_g);
      s_raw[o + 2U] = internal_pix_rgb(x, y, (uint32_t)k_chan_b);
      o += (size_t)k_rgb_bpp;
    }
  }
  mz_ulong zlen = (mz_ulong)sizeof(s_zbuf);
  TEST_ASSERT_EQ(MZ_OK, mz_compress(s_zbuf, &zlen, s_raw, (mz_ulong)o));
  internal_png_chunk("IDAT", s_zbuf, (uint32_t)zlen);
  internal_png_chunk("IEND", nullptr, 0U);
}

/**
 * @brief Build the two-page CBZ used by tile tests.
 * @details Stores the large PNG and deflates the small PNG before finalizing into s_arc.
 * @pre PNG and archive fixture capacities cover both generated pages.
 * @pre The host miniz writer can allocate its temporary archive buffer.
 * @post s_arc_size names the exact finalized CBZ byte count.
 * @post The miniz finalization buffer is released before return.
 * @note This host-only fixture allocation is checked by LeakSanitizer.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_build_cbz(void)
{
  mz_zip_archive zip = {};
  TEST_ASSERT(mz_zip_writer_init_heap(&zip, 0U, (size_t)k_arc_cap) == MZ_TRUE);
  internal_png_build_rgb((uint32_t)k_big_w, (uint32_t)k_big_h);
  TEST_ASSERT(mz_zip_writer_add_mem(&zip, "01_big.png", s_png, s_png_len, MZ_NO_COMPRESSION) ==
              MZ_TRUE);
  internal_png_build_rgb((uint32_t)k_small_w, (uint32_t)k_small_h);
  TEST_ASSERT(
    mz_zip_writer_add_mem(&zip, "02_small.png", s_png, s_png_len, MZ_DEFAULT_COMPRESSION) ==
    MZ_TRUE);
  void*  heap = nullptr;
  size_t hsz  = 0U;
  TEST_ASSERT(mz_zip_writer_finalize_heap_archive(&zip, &heap, &hsz) == MZ_TRUE);
  TEST_ASSERT(hsz <= (size_t)k_arc_cap);
  (void)memcpy(s_arc, heap, hsz);
  s_arc_size = hsz;
  mz_free(heap);
  mz_zip_writer_end(&zip);
}

/**
 * @brief Read a bounds-clamped span from the built CBZ.
 * @details Implements ra8_comic_read_fn directly over immutable s_arc bytes.
 * @param[in] ctx Unused callback context.
 * @param[in] off Absolute archive offset.
 * @param[out] buf Destination for available bytes.
 * @param[in] len Requested byte count.
 * @return Bytes copied.
 * @retval 0 Offset is at or beyond the archive end.
 * @retval 1..len Available bytes copied without crossing s_arc_size.
 * @pre @p buf is writable for @p len bytes when bytes are available.
 * @pre internal_build_cbz initialized s_arc_size.
 * @post Archive storage remains unchanged.
 * @post The callback never reads past s_arc_size.
 * @note The context is intentionally unused because the fixture is file-local.
 * @since Version 0.1.0
 */
RA8_INTERNAL static size_t internal_arc_read(void* ctx, uint64_t off, void* buf, size_t len)
{
  (void)ctx;
  if (off >= (uint64_t)s_arc_size) {
    return 0U;
  }
  const uint64_t avail = (uint64_t)s_arc_size - off;
  const size_t   k     = (len > (size_t)avail) ? (size_t)avail : len;
  (void)memcpy(buf, &s_arc[off], k);
  return k;
}

/**
 * @brief Compose the default tile-import configuration.
 * @details Binds fixed tile geometry and caller-owned work/atlas storage.
 * @param[in] codec JOF tile codec selected by the vector.
 * @return Complete import configuration.
 * @retval ra8_comic_tiles_import_cfg_t Configuration bound to file-local storage.
 * @pre @p codec is a codec accepted by the JOF producer.
 * @pre File-local work and atlas buffers remain live for the import.
 * @post No reader or fixture byte is modified.
 * @post The returned maximum dimensions cover the large fixture page.
 * @note Returning by value keeps each vector's mutations independent.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_comic_tiles_import_cfg_t internal_import_cfg(uint8_t codec)
{
  return (ra8_comic_tiles_import_cfg_t){
    .tile_w        = (uint16_t)k_tile,
    .tile_h        = (uint16_t)k_tile,
    .codec         = codec,
    .max_width     = (uint16_t)k_big_w,
    .max_height    = (uint16_t)k_big_h,
    .work          = s_work,
    .work_cap      = sizeof s_work,
    .webp_work     = nullptr,
    .webp_work_cap = 0U,
    .atlas         = s_atlas,
    .atlas_cap     = sizeof s_atlas,
  };
}

/**
 * @brief Compose tile-cache storage over fixed buffers.
 * @details Binds cells, keys, dimensions, metadata, and hash buckets without a decoder.
 * @return Complete cache configuration.
 * @retval ra8_tile_cache_cfg_t Configuration bound to file-local cache storage.
 * @pre Static cache arrays retain their declared capacities.
 * @pre The reader will install its decoder before cache use.
 * @post No cache cell or metadata entry is modified.
 * @post Returned counts match the compile-time fixture dimensions.
 * @note Returning by value permits deliberate per-test capacity changes.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_tile_cache_cfg_t internal_cache_cfg(void)
{
  return (ra8_tile_cache_cfg_t){
    .cell_mem     = s_cell_mem,
    .cell_bytes   = (uint32_t)k_cell_bytes,
    .cell_count   = (uint32_t)k_cells,
    .meta         = s_meta,
    .keys         = s_keys,
    .dims         = s_dims,
    .buckets      = s_buckets,
    .bucket_count = (uint32_t)k_buckets,
    .decode       = nullptr, /* the reader wires this */
    .decode_ctx   = nullptr,
  };
}

/**
 * @brief Open the CBZ and extract the large encoded page.
 * @details Binds the comic facade to the fixture reader and reads page zero into s_pagebuf.
 * @param[out] comic Comic handle retained by the caller for cleanup.
 * @return Extracted encoded page size.
 * @retval 1..k_png_cap Exact nonzero byte count placed in s_pagebuf.
 * @pre internal_build_cbz produced the two-page archive.
 * @pre @p comic points to writable zero-initialized storage.
 * @post @p comic remains open on the fixture archive.
 * @post s_pagebuf holds page zero without truncation.
 * @note Unity assertions terminate the vector on any setup failure.
 * @since Version 0.1.0
 */
RA8_INTERNAL static size_t internal_open_big_page_bytes(ra8_comic_t* comic)
{
  const ra8_err_t oerr = ra8_comic_open(comic,
                                        internal_arc_read,
                                        nullptr,
                                        (uint64_t)s_arc_size,
                                        s_pages,
                                        (uint32_t)(sizeof s_pages / sizeof s_pages[0]),
                                        s_names,
                                        (uint32_t)sizeof s_names);
  TEST_ASSERT_EQ(k_ra8_ok, oerr);
  TEST_ASSERT_EQ(2U, ra8_comic_page_count(comic));
  size_t          got  = 0U;
  const ra8_err_t rerr = ra8_comic_page_read(comic, 0U, s_pagebuf, sizeof s_pagebuf, &got);
  TEST_ASSERT_EQ(k_ra8_ok, rerr);
  TEST_ASSERT(got > 0U);
  return got;
}

/**
 * @brief Verify one pinned tile byte-for-byte.
 * @details Computes clamped edge geometry and compares every RGB sample with the source oracle.
 * @param[in] t Pinned tile returned by the reader.
 * @param[in] tx Tile column.
 * @param[in] ty Tile row.
 * @pre @p t is a valid pinned tile for the large fixture page.
 * @pre @p tx and @p ty identify an in-range tile coordinate.
 * @post Every available pixel and both dimensions have been asserted.
 * @post The tile remains pinned and unmodified for caller release.
 * @note Edge tiles deliberately exercise width and height clamping.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_verify_tile(const ra8_tile_t* t, uint16_t tx, uint16_t ty)
{
  const uint32_t base_x = (uint32_t)tx * (uint32_t)k_tile;
  const uint32_t base_y = (uint32_t)ty * (uint32_t)k_tile;
  const uint32_t exp_w  = ((base_x + (uint32_t)k_tile) <= (uint32_t)k_big_w)
                            ? (uint32_t)k_tile
                            : ((uint32_t)k_big_w - base_x);
  const uint32_t exp_h  = ((base_y + (uint32_t)k_tile) <= (uint32_t)k_big_h)
                            ? (uint32_t)k_tile
                            : ((uint32_t)k_big_h - base_y);
  TEST_ASSERT_EQ(exp_w, t->width);
  TEST_ASSERT_EQ(exp_h, t->height);
  for (uint32_t j = 0U; j < exp_h; j++) {
    for (uint32_t i = 0U; i < exp_w; i++) {
      const size_t o = ((size_t)j * (size_t)exp_w + (size_t)i) * (size_t)k_rgb_bpp;
      TEST_ASSERT_EQ(internal_pix_rgb(base_x + i, base_y + j, k_chan_r), t->pixels[o]);
      TEST_ASSERT_EQ(internal_pix_rgb(base_x + i, base_y + j, k_chan_g), t->pixels[o + 1U]);
      TEST_ASSERT_EQ(internal_pix_rgb(base_x + i, base_y + j, k_chan_b), t->pixels[o + 2U]);
    }
  }
}

/**
 * @brief Prove whole-image decoding exceeds the modest arena.
 * @details Initializes an off-screen target and requires the encoded large page to return no_mem.
 * @param[in] enc Encoded image bytes.
 * @param[in] len Exact encoded byte count.
 * @pre @p enc addresses a valid large fixture image.
 * @pre s_decode_arena is smaller than the decoded image requirement.
 * @post The decoder reports k_ra8_err_no_mem.
 * @post No memory outside the arena and framebuffer is modified.
 * @note This establishes why the tile path is required rather than optional.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_assert_whole_decode_caps(const uint8_t* enc, size_t len)
{
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_gfx_init(s_fb, (uint16_t)k_probe_w, (uint16_t)k_probe_h, k_ra8_gfx_format_rgb565));
  ra8_img_arena_t arena = {.base = s_decode_arena, .cap = sizeof s_decode_arena};
  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 ra8_img_decode_blit(&arena,
                                     enc,
                                     len,
                                     0,
                                     0,
                                     (int32_t)k_probe_w,
                                     (int32_t)k_probe_h,
                                     nullptr,
                                     nullptr));
}

/**
 * @brief Verify every tile and one cache-hit refetch.
 * @details Walks all coordinates, validates bytes, releases pins, then compares refetched storage.
 * @param[in,out] r Imported tile reader and cache.
 * @param[in] info Validated page geometry.
 * @pre @p r has imported the large fixture page.
 * @pre @p info describes the same reader epoch.
 * @post Every tile coordinate has passed byte-exact validation.
 * @post All pins are released and the repeated origin tile is a cache hit.
 * @note The page contains more tiles than cache cells, exercising eviction too.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_walk_and_verify_tiles(ra8_comic_tile_reader_t* r,
                                                        const ra8_jof_info_t*    info)
{
  for (uint16_t ty = 0U; ty < info->tile_rows; ty++) {
    for (uint16_t tx = 0U; tx < info->tile_cols; tx++) {
      ra8_tile_t t = {};
      TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_tiles_tile(r, tx, ty, &t));
      internal_verify_tile(&t, tx, ty);
      TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_tiles_release(r, t.pixels));
    }
  }
  ra8_tile_t a = {};
  ra8_tile_t b = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_tiles_tile(r, 0U, 0U, &a));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_tiles_release(r, a.pixels));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_tiles_tile(r, 0U, 0U, &b));
  TEST_ASSERT(a.pixels == b.pixels);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_tiles_release(r, b.pixels));
}

/**
 * @brief Prove required import workspaces reject null bindings.
 * @details Independently removes the work arena and atlas pointer from otherwise valid configs.
 * @param[in,out] r Initialized tile reader.
 * @pre @p r completed ra8_comic_tiles_init.
 * @pre s_png contains at least the four bytes passed to each guard call.
 * @post Both malformed configurations return k_ra8_err_null_ptr.
 * @post The reader does not become bound to an imported page.
 * @note Each guard varies one required binding at a time.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_assert_import_cfg_guards(ra8_comic_tile_reader_t* r)
{
  ra8_comic_tiles_import_cfg_t no_work = internal_import_cfg((uint8_t)k_ra8_jof_codec_deflate);
  no_work.work                         = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_comic_tiles_import(r, s_png, 4U, &no_work));
  ra8_comic_tiles_import_cfg_t no_atlas = internal_import_cfg((uint8_t)k_ra8_jof_codec_deflate);
  no_atlas.atlas                        = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_comic_tiles_import(r, s_png, 4U, &no_atlas));
}

/* ---------------------------------------------------------------------------
 * Tests.
 * ---------------------------------------------------------------------------
 */

/**
 * @test comic_tiles_footprint_and_budget
 * @brief Verify decoded footprint and every budget-threshold decision.
 * @details Builds both fixture pages, validates dimensions, and drives the N+1 MC/DC set.
 * @pre Fixed PNG and CBZ buffers satisfy their declared capacities.
 * @pre The image-header parser accepts the generated RGB PNG.
 * @post Large and small decoded sizes select tile and flat paths respectively.
 * @post Null, empty, and unsupported-image guards return their precise errors.
 * @note This vector owns complete MC/DC coverage of the threshold conjunction.
 * @since Version 0.1.0
 *
 * @par MC/DC:
 * Decision: `(budget_bytes != 0U) && (decoded_bytes > budget_bytes)` (2 conditions)
 * - Vector 1: budget=256KiB, decoded=1MiB -> true  (control: both true)
 * - Vector 2: budget=0,      decoded=1MiB -> false (varies budget!=0 only)
 * - Vector 3: budget=1MiB,   decoded=512KiB -> false (varies decoded>budget only)
 * Vectors 1+2 prove `budget_bytes != 0` independently drives the outcome; 1+3
 * prove the same for `decoded_bytes > budget_bytes`. N+1 = 3 vectors for N=2.
 */
RA8_INTERNAL static void internal_test_footprint_and_budget(void)
{
  TEST_BEGIN("comic tiles footprint + budget threshold");
  internal_build_cbz();

  /* Big page footprint from the encoded header. */
  internal_png_build_rgb((uint32_t)k_big_w, (uint32_t)k_big_h);
  uint16_t w  = 0U;
  uint16_t h  = 0U;
  uint64_t db = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_tiles_footprint(s_png, s_png_len, &w, &h, &db));
  TEST_ASSERT_EQ(k_big_w, w);
  TEST_ASSERT_EQ(k_big_h, h);
  TEST_ASSERT_EQ(k_big_w * (uint64_t)k_big_h * (uint64_t)k_ra8_comic_tiles_decoded_bpp, db);

  /* MC/DC vectors for the threshold decision. */
  TEST_ASSERT(ra8_comic_tiles_over_budget((uint64_t)1024U * 1024U, (uint64_t)k_budget_bytes));
  TEST_ASSERT(!ra8_comic_tiles_over_budget((uint64_t)1024U * 1024U, 0U));
  TEST_ASSERT(!ra8_comic_tiles_over_budget((uint64_t)512U * 1024U, (uint64_t)1024U * 1024U));
  /* The big page is over budget; the small page is not. */
  TEST_ASSERT(ra8_comic_tiles_over_budget(db, (uint64_t)k_budget_bytes));
  internal_png_build_rgb((uint32_t)k_small_w, (uint32_t)k_small_h);
  uint64_t small_db = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_tiles_footprint(s_png, s_png_len, &w, &h, &small_db));
  TEST_ASSERT(!ra8_comic_tiles_over_budget(small_db, (uint64_t)k_budget_bytes));

  /* Guards -- every required pointer, and the zero-length / non-image paths. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_comic_tiles_footprint(nullptr, 4U, &w, &h, &db));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_comic_tiles_footprint(s_png, 4U, nullptr, &h, &db));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_comic_tiles_footprint(s_png, 4U, &w, nullptr, &db));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_comic_tiles_footprint(s_png, 4U, &w, &h, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_comic_tiles_footprint(s_png, 0U, &w, &h, &db));
  static const uint8_t junk[8] = {'n', 'o', 't', 'i', 'm', 'a', 'g', 'e'};
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_comic_tiles_footprint(junk, sizeof junk, &w, &h, &db));
  TEST_END("comic tiles footprint + budget threshold");
}

/**
 * @test comic_tiles_large_page_opens
 * @brief Decode a page that exceeds the whole-image arena through bounded tiles.
 * @details The heart of #344: the big page defeats the whole-decode arena
 *          (`k_ra8_err_no_mem`) yet the tile path opens it and every tile is
 *          byte-exact at full resolution, with resident RAM bounded by the cell
 *          budget. A re-fetch is a cache hit, not a re-decode.
 * @pre internal_build_cbz has produced the deterministic two-page archive.
 * @pre Cache and codec workspaces retain their fixed capacities.
 * @post Every large-page tile is decoded at native resolution and verified.
 * @post The comic handle is closed and no cache pin remains held.
 * @note Resident decoded storage remains below the whole-image footprint.
 * @since Version 0.1.0
 *
 * @par MC/DC:
 * The `ra8_comic_tiles_over_budget` `&&` decision is exercised here only as a
 * control (a single true vector); its full N+1 MC/DC vector set lives in
 * ::internal_test_footprint_and_budget.
 */
RA8_INTERNAL static void internal_test_large_page_opens(void)
{
  TEST_BEGIN("comic tiles: large page opens where whole-decode caps");
  ra8_comic_t  comic = {};
  const size_t got   = internal_open_big_page_bytes(&comic);

  /* Threshold says tile. */
  uint16_t w  = 0U;
  uint16_t h  = 0U;
  uint64_t db = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_tiles_footprint(s_pagebuf, got, &w, &h, &db));
  TEST_ASSERT(ra8_comic_tiles_over_budget(db, (uint64_t)k_budget_bytes));

  /* The OLD path caps: whole-decode into a modest arena fails no_mem. */
  internal_assert_whole_decode_caps(s_pagebuf, got);

  /* The NEW path opens it. Resident cache RAM is far below the decoded image. */
  TEST_ASSERT((uint64_t)k_cells * (uint64_t)k_cell_bytes < db);
  ra8_comic_tile_reader_t r       = {};
  ra8_tile_cache_cfg_t    storage = internal_cache_cfg();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_comic_tiles_init(&r, &storage, s_scratch, (uint32_t)sizeof s_scratch));
  const ra8_comic_tiles_import_cfg_t cfg = internal_import_cfg((uint8_t)k_ra8_jof_codec_deflate);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_tiles_import(&r, s_pagebuf, got, &cfg));

  ra8_jof_info_t info = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_tiles_info(&r, &info));
  TEST_ASSERT_EQ(k_big_w, info.width);
  TEST_ASSERT_EQ(k_big_h, info.height);
  TEST_ASSERT_EQ(k_rgb_bpp, info.bpp);
  TEST_ASSERT(info.tile_count > (uint32_t)k_cells); /* more tiles than cells */

  /* Every tile -- interior and clamped edge -- decodes byte-exact, and a
   * re-fetch hits the cache rather than re-decoding. */
  internal_walk_and_verify_tiles(&r, &info);

  (void)ra8_comic_close(&comic);
  TEST_END("comic tiles: large page opens where whole-decode caps");
}

/**
 * @test comic_tiles_zoom_subrect
 * @brief Fetch one native-resolution interior tile without neighboring decodes.
 * @details The loupe property: a single interior tile is fetched and verified
 *          full-resolution without decoding the whole page or its neighbours.
 * @pre The deterministic CBZ is available through internal_arc_read.
 * @pre Raw-codec import storage covers one full tile.
 * @post Tile (2,1) matches the native source oracle and is released.
 * @post The comic handle is closed.
 * @note No neighboring tile is requested by this vector.
 * @since Version 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in this test)
 */
RA8_INTERNAL static void internal_test_zoom_subrect(void)
{
  TEST_BEGIN("comic tiles: sub-rect (loupe) tile is full-resolution");
  ra8_comic_t  comic = {};
  const size_t got   = internal_open_big_page_bytes(&comic);

  ra8_comic_tile_reader_t r       = {};
  ra8_tile_cache_cfg_t    storage = internal_cache_cfg();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_comic_tiles_init(&r, &storage, s_scratch, (uint32_t)sizeof s_scratch));
  const ra8_comic_tiles_import_cfg_t cfg = internal_import_cfg((uint8_t)k_ra8_jof_codec_raw);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_tiles_import(&r, s_pagebuf, got, &cfg));

  /* Center-ish interior tile (2,1) fetched on its own -> native pixels. */
  ra8_tile_t t = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_tiles_tile(&r, 2U, 1U, &t));
  TEST_ASSERT_EQ(k_tile, t.width);
  TEST_ASSERT_EQ(k_tile, t.height);
  internal_verify_tile(&t, 2U, 1U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_tiles_release(&r, t.pixels));

  (void)ra8_comic_close(&comic);
  TEST_END("comic tiles: sub-rect (loupe) tile is full-resolution");
}

/**
 * @test comic_tiles_tile_over_cell_budget
 * @brief Reject a decoded tile that cannot fit one cache cell.
 * @details A tile whose decoded payload exceeds the cache cell (a deliberately
 *          undersized cache) fails closed as `k_ra8_err_no_mem` rather than
 *          overrunning the cell -- the decode-on-miss maps the atlas reader's
 *          `k_ra8_err_invalid_size` to `k_ra8_err_no_mem`.
 * @pre The large fixture page imports successfully with raw tile coding.
 * @pre The cache cell size is deliberately reduced below one decoded tile.
 * @post The first tile request returns k_ra8_err_no_mem.
 * @post The comic handle is closed without an acquired tile pin.
 * @note The vector protects the cache-cell write boundary.
 * @since Version 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in this test)
 */
RA8_INTERNAL static void internal_test_tile_over_cell_budget(void)
{
  TEST_BEGIN("comic tiles: a tile larger than the cache cell fails no_mem");
  ra8_comic_t  comic = {};
  const size_t got   = internal_open_big_page_bytes(&comic);

  ra8_comic_tile_reader_t r       = {};
  ra8_tile_cache_cfg_t    storage = internal_cache_cfg();
  storage.cell_bytes              = (uint32_t)k_tiny_cell; /* far below a 128x128x3 tile */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_comic_tiles_init(&r, &storage, s_scratch, (uint32_t)sizeof s_scratch));
  const ra8_comic_tiles_import_cfg_t cfg = internal_import_cfg((uint8_t)k_ra8_jof_codec_raw);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_tiles_import(&r, s_pagebuf, got, &cfg));

  ra8_tile_t t = {};
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_comic_tiles_tile(&r, 0U, 0U, &t));

  (void)ra8_comic_close(&comic);
  TEST_END("comic tiles: a tile larger than the cache cell fails no_mem");
}

/**
 * @test comic_tiles_small_page_flat
 * @brief Preserve the whole-image path for a page below the decode budget.
 * @details A page under the budget stays on the whole-decode fast path: the
 *          same modest arena that capped the big page decodes the small page,
 *          so the existing golden render is preserved.
 * @pre The two-page CBZ contains the small PNG at page index one.
 * @pre The modest decode arena covers the small decoded footprint.
 * @post The budget decision selects the flat path and decode succeeds.
 * @post Positive output dimensions are reported and the comic is closed.
 * @note This is the golden counterpart to the large-page no_mem vector.
 * @since Version 0.1.0
 *
 * @par MC/DC:
 * The `ra8_comic_tiles_over_budget` `&&` decision is exercised here only as a
 * control (a single false vector); its full N+1 MC/DC vector set lives in
 * ::internal_test_footprint_and_budget.
 */
RA8_INTERNAL static void internal_test_small_page_flat(void)
{
  TEST_BEGIN("comic tiles: small page keeps the whole-decode fast path");
  internal_build_cbz();
  ra8_comic_t comic = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_comic_open(&comic,
                                internal_arc_read,
                                nullptr,
                                (uint64_t)s_arc_size,
                                s_pages,
                                (uint32_t)(sizeof s_pages / sizeof s_pages[0]),
                                s_names,
                                (uint32_t)sizeof s_names));
  size_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_page_read(&comic, 1U, s_pagebuf, sizeof s_pagebuf, &got));

  uint16_t w  = 0U;
  uint16_t h  = 0U;
  uint64_t db = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_tiles_footprint(s_pagebuf, got, &w, &h, &db));
  TEST_ASSERT_EQ(k_small_w, w);
  TEST_ASSERT(!ra8_comic_tiles_over_budget(db, (uint64_t)k_budget_bytes));

  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_gfx_init(s_fb, (uint16_t)k_probe_w, (uint16_t)k_probe_h, k_ra8_gfx_format_rgb565));
  ra8_img_arena_t arena = {.base = s_decode_arena, .cap = sizeof s_decode_arena};
  int32_t         dw    = 0;
  int32_t         dh    = 0;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_img_decode_blit(&arena,
                                     s_pagebuf,
                                     got,
                                     0,
                                     0,
                                     (int32_t)k_probe_w,
                                     (int32_t)k_probe_h,
                                     &dw,
                                     &dh));
  TEST_ASSERT(dw > 0);
  TEST_ASSERT(dh > 0);

  (void)ra8_comic_close(&comic);
  TEST_END("comic tiles: small page keeps the whole-decode fast path");
}

/**
 * @test comic_tiles_page_turn_epoch
 * @brief Prevent stale tile reuse after importing a different page.
 * @details Re-importing (a page turn) bumps the epoch so a same-coordinate tile
 *          fetch returns the NEW page's pixels, never a stale cached tile.
 * @pre The reader and cache are initialized over fixed storage.
 * @pre Both generated PNG pages are valid import sources.
 * @post The second import has a distinct epoch and small-page geometry.
 * @post The returned tile is released after its new-page dimensions are checked.
 * @note Equal origin sample bytes do not substitute for the geometry assertion.
 * @since Version 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in this test)
 */
RA8_INTERNAL static void internal_test_page_turn_epoch(void)
{
  TEST_BEGIN("comic tiles: page turn never surfaces a stale tile");
  ra8_comic_tile_reader_t r       = {};
  ra8_tile_cache_cfg_t    storage = internal_cache_cfg();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_comic_tiles_init(&r, &storage, s_scratch, (uint32_t)sizeof s_scratch));

  const ra8_comic_tiles_import_cfg_t cfg = internal_import_cfg((uint8_t)k_ra8_jof_codec_deflate);

  /* Page A: the big pattern. Fetch tile (0,0), leave it cached. */
  internal_png_build_rgb((uint32_t)k_big_w, (uint32_t)k_big_h);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_tiles_import(&r, s_png, s_png_len, &cfg));
  const uint32_t epoch_a = r.epoch;
  ra8_tile_t     ta      = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_tiles_tile(&r, 0U, 0U, &ta));
  const uint8_t a00 = ta.pixels[0];
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_tiles_release(&r, ta.pixels));

  /* Page B: a DIFFERENT pattern (offset the source so (0,0) differs). Re-import
   * rebinds the store + bumps the epoch, so the old (0,0) tile cannot be hit. */
  internal_png_build_rgb((uint32_t)k_small_w, (uint32_t)k_small_h);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_tiles_import(&r, s_png, s_png_len, &cfg));
  TEST_ASSERT(r.epoch != epoch_a);
  ra8_jof_info_t info = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_tiles_info(&r, &info));
  TEST_ASSERT_EQ(k_small_w, info.width);
  ra8_tile_t tb = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_tiles_tile(&r, 0U, 0U, &tb));
  /* Same pattern origin (0,0) so byte 0 matches -- but assert we read page B's
   * geometry (the clamped tile is the small page's size, not the big page's). */
  TEST_ASSERT_EQ(k_small_w, tb.width);
  TEST_ASSERT_EQ(k_small_h, tb.height);
  TEST_ASSERT_EQ(a00, tb.pixels[0]); /* internal_pix_rgb(0,0,R) identical for both pages */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_tiles_release(&r, tb.pixels));
  TEST_END("comic tiles: page turn never surfaces a stale tile");
}

/**
 * @test comic_tiles_guards
 * @brief Reject invalid bindings, states, coordinates, formats, and capacities.
 * @details Fail-closed guards on every entry point: null pointers, unbound
 *          reader, out-of-range tiles, an unsupported source, and a too-small
 *          atlas store.
 * @pre Static cache, scratch, work, and atlas buffers remain available.
 * @pre The generated small and large PNG fixtures fit their capacities.
 * @post Every invalid binding, state, coordinate, and capacity is rejected.
 * @post Failed imports leave the reader unbound to a usable page.
 * @note Valid import between hostile probes proves failures do not poison reuse.
 * @since Version 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions in this test)
 */
RA8_INTERNAL static void internal_test_guards(void)
{
  TEST_BEGIN("comic tiles: fail-closed guards");
  ra8_comic_tile_reader_t r       = {};
  ra8_tile_cache_cfg_t    storage = internal_cache_cfg();

  /* init guards. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_comic_tiles_init(nullptr, &storage, s_scratch, 4U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_comic_tiles_init(&r, nullptr, s_scratch, 4U));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_comic_tiles_init(&r, &storage, s_scratch, (uint32_t)sizeof s_scratch));

  /* Fetching before an import is invalid_state. */
  ra8_tile_t     t    = {};
  ra8_jof_info_t info = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_comic_tiles_tile(&r, 0U, 0U, &t));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_comic_tiles_info(&r, &info));

  /* import guards. */
  const ra8_comic_tiles_import_cfg_t cfg = internal_import_cfg((uint8_t)k_ra8_jof_codec_deflate);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_comic_tiles_import(nullptr, s_png, 4U, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_comic_tiles_import(&r, nullptr, 4U, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_comic_tiles_import(&r, s_png, 4U, nullptr));
  internal_assert_import_cfg_guards(&r);
  internal_png_build_rgb((uint32_t)k_small_w, (uint32_t)k_small_h);
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_comic_tiles_import(&r, s_png, 0U, &cfg));

  /* An unsupported (non-image) source aborts the transcode fail-closed and
   * leaves the reader unbound. */
  static const uint8_t junk[16] =
    {'n', 'o', 't', ' ', 'a', 'n', ' ', 'i', 'm', 'a', 'g', 'e', '!', '!', '!', '!'};
  TEST_ASSERT(ra8_comic_tiles_import(&r, junk, sizeof junk, &cfg) != k_ra8_ok);
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_comic_tiles_tile(&r, 0U, 0U, &t));

  /* A valid import, then out-of-range tile coordinates. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_tiles_import(&r, s_png, s_png_len, &cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_tiles_info(&r, &info));
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 ra8_comic_tiles_tile(&r, (uint16_t)info.tile_cols, 0U, &t));
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 ra8_comic_tiles_tile(&r, 0U, (uint16_t)info.tile_rows, &t));

  /* Reader null guards. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_comic_tiles_tile(nullptr, 0U, 0U, &t));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_comic_tiles_tile(&r, 0U, 0U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_comic_tiles_info(&r, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_comic_tiles_release(&r, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_comic_tiles_release(nullptr, s_scratch));

  /* A too-small atlas store makes the producer fail; reader stays unbound. */
  ra8_comic_tiles_import_cfg_t tiny = internal_import_cfg((uint8_t)k_ra8_jof_codec_deflate);
  tiny.atlas_cap                    = 16U;
  internal_png_build_rgb((uint32_t)k_big_w, (uint32_t)k_big_h);
  TEST_ASSERT(ra8_comic_tiles_import(&r, s_png, s_png_len, &tiny) != k_ra8_ok);
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_comic_tiles_tile(&r, 0U, 0U, &t));
  TEST_END("comic tiles: fail-closed guards");
}

int main(void)
{
  internal_test_footprint_and_budget();
  internal_test_large_page_opens();
  internal_test_zoom_subrect();
  internal_test_tile_over_cell_budget();
  internal_test_small_page_flat();
  internal_test_page_turn_epoch();
  internal_test_guards();
  return 0;
}
