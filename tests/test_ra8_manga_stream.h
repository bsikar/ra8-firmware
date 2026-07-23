/**
 * @file test_ra8_manga_stream.h
 * @brief #232 manga-scale streaming harness: a synthetic volume-of-JOF-atlases
 *        driven through the real #231 tile-cache + #147 page-cache stack.
 *
 * @details
 * Shared test harness for the two #232 manga-scale streaming TUs
 * (`test_ra8_manga_stream.c` -- bounded residency + correctness;
 * `test_ra8_manga_stream_policy.c` -- scan resistance + eviction-split tuning).
 * It proves that a manga volume whose decoded size dwarfs the ~10 MB SDRAM
 * working set (the cited worst cases are a 612 MB and a 2.75 GB book) streams
 * correctly through a fixed RAM budget by driving the EXACT production two-tier
 * stack over a volume that is never materialised:
 *
 * ```
 *   ra8_tile_cache (LRU, decoded bands)          <- Layer 3b (#231)
 *     |  miss -> decode
 *     v
 *   ra8_jof_read_tile (real JOF reader)   <- the #231/#289 band format
 *     |  pread
 *     v
 *   ra8_vmem_stream_read (byte stream)           <- Layer 2 helper (#151)
 *     |
 *     v
 *   ra8_vmem (SLRU page cache, raw atlas bytes)  <- Layer 2 (#147)
 *     |  miss -> loader
 *     v
 *   ra8_vsource (paged object)                   <- Layer 1 (#147)
 *     |
 *     v
 *   synthetic volume-of-JOF-atlases (content-generated, never materialised)
 * ```
 *
 * The volume is a concatenation of byte-identical JOF atlases, each a legal
 * 768 x 32768 longstrip strip: `tile_w == width` (one tile column), so a tile row
 * IS a scroll band (#289) with O(1) seek to any scroll-y. Its bytes -- header,
 * every band's pixels, the tile index, the footer -- are produced on the fly by
 * a pure content function, so a 2.75 GB book is modelled in a few hundred KiB of
 * host RAM (no downscaling: full-resolution tiling is the memory strategy). The
 * band pixels are a 64-bit avalanche of (atlas, band, offset), so a mis-routed,
 * stale, or offset-truncated read is caught by the byte compare that doubles as
 * the reference oracle.
 *
 * The real >SDRAM run off the Pmod2 SD card (board_sim, then EK-RA8D2 HIL) is an
 * owner/bench step (#232); this host gate is the sim==HIL golden it is compared
 * against.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_jof.h"
#include "ra8_keycache.h"
#include "ra8_tile_cache.h"
#include "ra8_vmem.h"
#include "ra8_vmem_stream.h"
#include "ra8_vsource.h"
#include "unity_minimal.h"

/**
 * @enum t_mg_geom_t
 * @brief Synthetic JOF atlas geometry: one legal longstrip strip (#289 bands).
 *
 * @details A single tile column (`tile_w == width`) of `tile_h`-tall bands at
 *          the maximum legal 32768-px height, gray8, raw codec. The derived
 *          byte offsets are cross-checked by ::static_assert against the JOF
 *          layout so they cannot silently drift from the format.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_mg_img_w      = 768U,      /**< Longstrip strip width, pixels (== tile_w). */
  k_mg_img_h      = 32768U,    /**< Strip height, pixels (JOF max_dim).        */
  k_mg_tile_w     = 768U,      /**< Tile width == image width -> one column.   */
  k_mg_tile_h     = 64U,       /**< Band height, pixels.                       */
  k_mg_bpp        = 1U,        /**< Bytes per pixel (gray8).                   */
  k_mg_bands      = 512U,      /**< Bands per atlas (img_h / tile_h == tiles). */
  k_mg_payload    = 49152U,    /**< Band payload bytes (tile_w*tile_h*bpp).    */
  k_mg_index_off  = 25165856U, /**< Index offset (hdr + bands*payload).        */
  k_mg_index_len  = 4096U,     /**< Index bytes (bands * 8).                   */
  k_mg_atlas_size = 25169968U, /**< Whole atlas bytes (index_off+idx+footer).  */
} t_mg_geom_t;

/* Cross-check the derived offsets against the real JOF layout so a geometry
 * edit cannot desynchronise the synthetic bytes from the reader's expectations. */
static_assert((uint32_t)k_mg_payload ==
                (uint32_t)k_mg_tile_w * (uint32_t)k_mg_tile_h * (uint32_t)k_mg_bpp,
              "payload must equal tile_w * tile_h * bpp");
static_assert((uint32_t)k_mg_bands == (uint32_t)k_mg_img_h / (uint32_t)k_mg_tile_h,
              "band count must equal img_h / tile_h");
static_assert((uint32_t)k_mg_index_off ==
                (uint32_t)k_ra8_jof_hdr_bytes + ((uint32_t)k_mg_bands * (uint32_t)k_mg_payload),
              "index_off must trail the header + all band streams");
static_assert((uint32_t)k_mg_index_len == (uint32_t)k_mg_bands * (uint32_t)k_ra8_jof_index_entry,
              "index_len must be bands * index-entry size");
static_assert((uint32_t)k_mg_atlas_size == (uint32_t)k_mg_index_off + (uint32_t)k_mg_index_len +
                                             (uint32_t)k_ra8_jof_footer_bytes,
              "atlas size must be index_off + index + footer");
static_assert((uint32_t)k_mg_img_h <= (uint32_t)k_ra8_jof_max_dim,
              "strip height must be within the JOF dimension cap");
static_assert((uint32_t)k_mg_bands <= (uint32_t)k_ra8_jof_max_tiles,
              "band count must be within the JOF tile cap");

/**
 * @enum t_mg_target_t
 * @brief The two manga-scale volume sizes #232 cites (bytes).
 *
 * @details The bounded-residency gate builds a whole number of atlases whose
 *          total is >= the target, so the modelled volume genuinely spans the
 *          cited size.
 *
 * @since 0.1.0
 */
typedef enum : uint64_t {
  k_mg_target_small = 612000000ULL,  /**< 612 MB manga volume.  */
  k_mg_target_big   = 2750000000ULL, /**< 2.75 GB manga volume. */
} t_mg_target_t;

/**
 * @enum t_mg_budget_t
 * @brief Fixed RAM budgets for the two caches (independent of volume size).
 *
 * @details The whole point of #147/#231: these budgets are what the resident
 *          set is bounded by. Decoded budget = `k_mg_cells * k_mg_cell_bytes`
 *          (~192 KiB); raw page budget = `k_mg_frames * k_mg_frame_bytes`
 *          (~256 KiB). A band's raw stream (payload) spans 12 page frames.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_mg_frame_bytes = 4096U,  /**< Page-cache frame size, bytes.               */
  k_mg_frames      = 64U,    /**< Page-cache frame budget (256 KiB).          */
  k_mg_pc_buckets  = 256U,   /**< Page-cache hash buckets.                    */
  k_mg_cell_bytes  = 49152U, /**< Tile-cache cell = one decoded band payload. */
  k_mg_cells       = 4U,     /**< Tile-cache cell budget (192 KiB, tiny).     */
  k_mg_tc_buckets  = 16U,    /**< Tile-cache hash buckets.                    */
} t_mg_budget_t;

/**
 * @enum t_mg_mix_t
 * @brief 64-bit avalanche multipliers for the band content generator.
 *
 * @details A wrong (atlas, band, offset) triple must produce different bytes so
 *          a mis-routed read is caught; three odd multipliers mix the three
 *          coordinates before a high-bit byte selection.
 *
 * @since 0.1.0
 */
typedef enum : uint64_t {
  k_mg_mul_atlas = 0x9E3779B97F4A7C15ULL, /**< Fibonacci hash (atlas index).         */
  k_mg_mul_band  = 0xC2B2AE3D27D4EB4FULL, /**< xxHash prime (band index).            */
  k_mg_mul_off   = 0x165667B19E3779F9ULL, /**< xxHash prime (byte offset).           */
  k_mg_sel_shift = 40U,                   /**< Byte-selection shift (>> then trunc). */
} t_mg_mix_t;

/**
 * @enum t_mg_le_t
 * @brief Little-endian byte-assembly constants for the synthetic header/index/footer.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_mg_bits_per_byte = 8U,    /**< Bits per byte (LE shift step). */
  k_mg_byte_mask     = 0xFFU, /**< Low-byte mask.                 */
} t_mg_le_t;

/**
 * @enum t_mg_probe_t
 * @brief Verification-probe constants shared by both TUs (no magic numbers).
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_mg_pix_stride  = 1021U, /**< Prime pixel-sample stride within a band.    */
  k_mg_min_oversub = 100U,  /**< Min distinct-band-to-cell oversubscription. */
} t_mg_probe_t;

/** @brief JOF header magic ("JOF1"). */
static const uint8_t s_mg_magic_hdr[k_ra8_jof_magic_len] = {'J', 'O', 'F', '1'};
/** @brief JOF footer magic ("JOFE"). */
static const uint8_t s_mg_magic_ftr[k_ra8_jof_magic_len] = {'J', 'O', 'F', 'E'};

/* --- Fixed-budget cache storage (the asserted RAM high-water). ------------- */

/** @brief Page-cache frame storage. */
static uint8_t s_pc_frames[(size_t)k_mg_frames * (size_t)k_mg_frame_bytes];
/** @brief Page-cache frame metadata. */
static ra8_vmem_frame_t s_pc_meta[(size_t)k_mg_frames];
/** @brief Page-cache frame key storage. */
static ra8_vmem_key_t s_pc_keys[(size_t)k_mg_frames];
/** @brief Page-cache hash buckets. */
static int32_t s_pc_buckets[(size_t)k_mg_pc_buckets];

/** @brief Tile-cache cell storage (decoded bands). */
static uint8_t s_tc_cells[(size_t)k_mg_cells * (size_t)k_mg_cell_bytes];
/** @brief Tile-cache key storage. */
static ra8_tile_key_t s_tc_keys[(size_t)k_mg_cells];
/** @brief Tile-cache per-cell dimension descriptors. */
static ra8_tile_dims_t s_tc_dims[(size_t)k_mg_cells];
/** @brief Tile-cache cell link metadata. */
static ra8_keycache_cell_t s_tc_meta[(size_t)k_mg_cells];
/** @brief Tile-cache hash buckets. */
static int32_t s_tc_buckets[(size_t)k_mg_tc_buckets];

/**
 * @struct t_mg_atlas_pread_t
 * @brief Per-atlas positioned-read context: a base offset over the volume stream.
 *
 * @details Binds one atlas's byte range to the JOF reader's atlas-local pread
 *          contract by translating atlas-local offsets to absolute volume
 *          offsets and serving them through the shared page-cache byte stream.
 *
 * @since 0.1.0
 */
typedef struct {
  ra8_vmem_stream_t* st;   /**< Volume byte stream (page cache underneath).  */
  uint64_t           base; /**< Absolute offset of this atlas in the volume. */
} t_mg_atlas_pread_t;

/**
 * @struct t_mg_decode_ctx_t
 * @brief Tile-cache decode context: the stream + the shared parsed geometry.
 *
 * @details All atlases share one geometry, so the tile decoder reuses a single
 *          parsed ::ra8_jof_info_t and derives each atlas's base offset
 *          from the tile key's `image_id` (the page/atlas index).
 *
 * @since 0.1.0
 */
typedef struct {
  ra8_vmem_stream_t*    st;   /**< Volume byte stream.           */
  const ra8_jof_info_t* info; /**< Shared parsed atlas geometry. */
} t_mg_decode_ctx_t;

/**
 * @struct t_mg_hw_t
 * @brief Residency high-water marks for the two caches (max valid entries seen).
 *
 * @since 0.1.0
 */
typedef struct {
  uint32_t pc_frames; /**< Max resident page frames observed. */
  uint32_t tc_cells;  /**< Max resident tile cells observed.  */
} t_mg_hw_t;

/**
 * @brief Deterministic content byte of band @p band, offset @p i in atlas @p k.
 *
 * @details A 64-bit avalanche of (atlas, band, offset): distinct triples give
 *          distinct bytes, so a mis-routed or stale read is caught by the
 *          byte compare. This is both the generator (the synthetic backing) and
 *          the oracle (the reference the cached tile is checked against).
 *
 * @param[in] k    Atlas (page) index within the volume.
 * @param[in] band Band (tile row) index within the atlas.
 * @param[in] i    Byte offset within the band payload.
 *
 * @return The content byte at (@p k, @p band, @p i).
 * @retval 0-255 The selected avalanche byte.
 *
 * @pre `band < k_mg_bands` and `i < k_mg_payload`.
 * @pre The same formula backs both the generator and the reference.
 * @post No state is modified.
 * @post The result depends only on the three coordinates.
 *
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
static uint8_t t_mg_payload_byte(uint64_t k, uint32_t band, uint32_t i)
{
  const uint64_t mix = (k * (uint64_t)k_mg_mul_atlas) ^ ((uint64_t)band * (uint64_t)k_mg_mul_band) ^
                       ((uint64_t)i * (uint64_t)k_mg_mul_off);
  return (uint8_t)(mix >> (uint8_t)k_mg_sel_shift);
}

/**
 * @brief Emit little-endian byte @p j of the 32-bit value @p v.
 *
 * @param[in] v Value to serialise.
 * @param[in] j Byte index in `[0, 4)`.
 *
 * @return Byte @p j of @p v (little-endian).
 * @retval 0-255 The selected byte.
 *
 * @pre `j < 4`.
 * @pre @p v is the field value to serialise.
 * @post No state is modified.
 * @post The result is `(v >> (8*j)) & 0xFF`.
 *
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
static uint8_t t_mg_le_byte(uint32_t v, uint32_t j)
{
  return (uint8_t)((v >> (j * (uint32_t)k_mg_bits_per_byte)) & (uint32_t)k_mg_byte_mask);
}

/**
 * @brief Synthetic JOF header byte at atlas-local offset @p off (0..31).
 *
 * @details The header is atlas-independent (uniform geometry): magic, width,
 *          height, tile_w, tile_h, bpp, codec, zeroed reserved runs, and the
 *          tile count. Fields not covered by a case are reserved bytes -> 0.
 *
 * @param[in] off Atlas-local byte offset within the 32-byte header.
 *
 * @return The header byte at @p off.
 * @retval 0-255 The assembled header byte (0 for reserved positions).
 *
 * @pre `off < k_ra8_jof_hdr_bytes`.
 * @pre The geometry enums match the parsed atlas.
 * @post No state is modified.
 * @post The bytes reproduce a valid JOF header for this geometry.
 *
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
static uint8_t t_mg_hdr_byte(uint32_t off)
{
  if (off < (uint32_t)k_ra8_jof_magic_len) {
    return s_mg_magic_hdr[off];
  }
  if (off < (uint32_t)k_ra8_jof_ofs_height) {
    return t_mg_le_byte((uint32_t)k_mg_img_w, off - (uint32_t)k_ra8_jof_ofs_width);
  }
  if (off < (uint32_t)k_ra8_jof_ofs_tile_w) {
    return t_mg_le_byte((uint32_t)k_mg_img_h, off - (uint32_t)k_ra8_jof_ofs_height);
  }
  if (off < (uint32_t)k_ra8_jof_ofs_tile_h) {
    return t_mg_le_byte((uint32_t)k_mg_tile_w, off - (uint32_t)k_ra8_jof_ofs_tile_w);
  }
  if (off < (uint32_t)k_ra8_jof_ofs_bpp) {
    return t_mg_le_byte((uint32_t)k_mg_tile_h, off - (uint32_t)k_ra8_jof_ofs_tile_h);
  }
  if (off == (uint32_t)k_ra8_jof_ofs_bpp) {
    return (uint8_t)k_mg_bpp;
  }
  if (off == (uint32_t)k_ra8_jof_ofs_codec) {
    return (uint8_t)k_ra8_jof_codec_raw;
  }
  if ((off >= (uint32_t)k_ra8_jof_ofs_tile_count) &&
      (off < ((uint32_t)k_ra8_jof_ofs_tile_count + (uint32_t)sizeof(uint32_t)))) {
    return t_mg_le_byte((uint32_t)k_mg_bands, off - (uint32_t)k_ra8_jof_ofs_tile_count);
  }
  return 0U; /* reserved runs at ofs_reserved and ofs_reserved2 */
}

/**
 * @brief Synthetic JOF index-entry byte for tile @p n, entry offset @p j (0..7).
 *
 * @details Entry `n` holds the tile stream offset (32 + n*payload) then the
 *          length (payload), both little-endian, matching the raw-codec layout.
 *
 * @param[in] n Linear tile (band) number.
 * @param[in] j Byte offset within the 8-byte index entry.
 *
 * @return The index-entry byte.
 * @retval 0-255 The assembled offset/length byte.
 *
 * @pre `n < k_mg_bands` and `j < k_ra8_jof_index_entry`.
 * @pre The generator matches the tile-stream placement.
 * @post No state is modified.
 * @post The entry points at `[32 + n*payload, +payload)`.
 *
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
static uint8_t t_mg_idx_byte(uint32_t n, uint32_t j)
{
  if (j < (uint32_t)k_ra8_jof_idx_ofs_length) {
    const uint32_t toff = (uint32_t)k_ra8_jof_hdr_bytes + (n * (uint32_t)k_mg_payload);
    return t_mg_le_byte(toff, j - (uint32_t)k_ra8_jof_idx_ofs_offset);
  }
  return t_mg_le_byte((uint32_t)k_mg_payload, j - (uint32_t)k_ra8_jof_idx_ofs_length);
}

/**
 * @brief Synthetic JOF footer byte at footer offset @p off (0..15).
 *
 * @details index_off, tile count, total atlas size (all little-endian), then the
 *          "JOFE" magic -- atlas-independent for uniform geometry.
 *
 * @param[in] off Byte offset within the 16-byte footer.
 *
 * @return The footer byte.
 * @retval 0-255 The assembled footer byte.
 *
 * @pre `off < k_ra8_jof_footer_bytes`.
 * @pre The geometry enums match the parsed atlas.
 * @post No state is modified.
 * @post The footer closes a valid JOF atlas of this geometry.
 *
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
static uint8_t t_mg_ftr_byte(uint32_t off)
{
  if (off < (uint32_t)k_ra8_jof_ftr_tile_count) {
    return t_mg_le_byte((uint32_t)k_mg_index_off, off - (uint32_t)k_ra8_jof_ftr_index_off);
  }
  if (off < (uint32_t)k_ra8_jof_ftr_total_size) {
    return t_mg_le_byte((uint32_t)k_mg_bands, off - (uint32_t)k_ra8_jof_ftr_tile_count);
  }
  if (off < (uint32_t)k_ra8_jof_ftr_magic) {
    return t_mg_le_byte((uint32_t)k_mg_atlas_size, off - (uint32_t)k_ra8_jof_ftr_total_size);
  }
  return s_mg_magic_ftr[off - (uint32_t)k_ra8_jof_ftr_magic];
}

/**
 * @brief One synthetic volume byte at absolute offset @p abs (region-routed).
 *
 * @details Splits @p abs into (atlas k, atlas-local `local`), then routes to the
 *          header / band-stream / index / footer emitter. The band-stream region
 *          is the only atlas-dependent part (its pixels carry the atlas index).
 *
 * @param[in] abs Absolute byte offset within the volume.
 *
 * @return The volume byte at @p abs.
 * @retval 0-255 The generated byte.
 *
 * @pre `abs` is within the modelled volume `[0, atlas_count * atlas_size)`.
 * @pre The geometry enums are consistent (guarded by the static_asserts above).
 * @post No state is modified.
 * @post The byte reproduces a valid JOF atlas stream at @p abs.
 *
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
static uint8_t t_mg_vol_byte(uint64_t abs)
{
  const uint64_t k     = abs / (uint64_t)k_mg_atlas_size;
  const uint32_t local = (uint32_t)(abs - (k * (uint64_t)k_mg_atlas_size));
  if (local < (uint32_t)k_ra8_jof_hdr_bytes) {
    return t_mg_hdr_byte(local);
  }
  if (local < (uint32_t)k_mg_index_off) {
    const uint32_t rel  = local - (uint32_t)k_ra8_jof_hdr_bytes;
    const uint32_t band = rel / (uint32_t)k_mg_payload;
    const uint32_t i    = rel - (band * (uint32_t)k_mg_payload);
    return t_mg_payload_byte(k, band, i);
  }
  if (local < ((uint32_t)k_mg_index_off + (uint32_t)k_mg_index_len)) {
    const uint32_t rel = local - (uint32_t)k_mg_index_off;
    const uint32_t n   = rel / (uint32_t)k_ra8_jof_index_entry;
    const uint32_t j   = rel - (n * (uint32_t)k_ra8_jof_index_entry);
    return t_mg_idx_byte(n, j);
  }
  return t_mg_ftr_byte(local - ((uint32_t)k_mg_index_off + (uint32_t)k_mg_index_len));
}

/**
 * @brief ra8_vsource read callback: serve generated volume bytes (no backing).
 *
 * @param[in]  ctx    Unused backing context.
 * @param[in]  offset Absolute volume byte offset.
 * @param[out] buf    Destination buffer (`len` writable bytes).
 * @param[in]  len    Bytes to generate.
 *
 * @return ra8_err_t Always k_ra8_ok.
 * @retval k_ra8_ok Buffer filled with the generated content.
 *
 * @pre `buf` is writable for `len` bytes.
 * @pre `offset + len` lies within the modelled volume.
 * @post Each `buf[j]` equals the volume byte at `offset + j`.
 * @post No state outside `buf` is modified.
 *
 * @note Not thread-safe (irrelevant: single-threaded test).
 * @since 0.1.0
 */
static ra8_err_t t_mg_vol_read(void* ctx, uint64_t offset, uint8_t* buf, uint32_t len)
{
  (void)ctx;
  for (uint32_t j = 0U; j < len; ++j) {
    buf[j] = t_mg_vol_byte(offset + (uint64_t)j);
  }
  return k_ra8_ok;
}

/**
 * @brief JOF pread seam over one atlas: base-translate onto the volume stream.
 *
 * @param[in]  ctx    A ::t_mg_atlas_pread_t (stream + atlas base).
 * @param[in]  offset Atlas-local byte offset.
 * @param[out] buf    Destination buffer (`len` writable bytes).
 * @param[in]  len    Bytes requested.
 * @param[out] got    Bytes actually copied through the page cache.
 *
 * @return ra8_err_t Always k_ra8_ok (a short read reports `*got < len`).
 * @retval k_ra8_ok The stream served `*got` bytes.
 *
 * @pre `ctx` is a bound ::t_mg_atlas_pread_t; `buf` is writable for `len`.
 * @pre The atlas lies fully within the volume stream's object.
 * @post `*got <= len` bytes were copied into `buf`.
 * @post At most one page frame is pinned during the copy.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t t_mg_atlas_pread(void* ctx, uint64_t offset, uint8_t* buf, size_t len, size_t* got)
{
  const t_mg_atlas_pread_t* ap = (const t_mg_atlas_pread_t*)ctx;
  *got                         = ra8_vmem_stream_read(ap->st, ap->base + offset, buf, len);
  return k_ra8_ok;
}

/**
 * @brief Tile-cache decode-on-miss: decode one band through the real JOF reader.
 *
 * @details Derives the atlas base from the tile key's `image_id` (page index)
 *          and reads the band (tile row) with ::ra8_jof_read_tile over the
 *          page-cache byte stream. Raw codec, so no deflate scratch is needed.
 *
 * @param[in]  ctx        A ::t_mg_decode_ctx_t (stream + shared geometry).
 * @param[in]  key        The band to decode (`image_id` = atlas, `tile_y` = band).
 * @param[out] cell       Destination band pixels (`cell_bytes` writable).
 * @param[in]  cell_bytes Cell capacity in bytes.
 * @param[out] out_w      Decoded band width, pixels.
 * @param[out] out_h      Decoded band height, pixels.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok    Band decoded into @p cell.
 * @retval k_ra8_err_* Propagated from the JOF reader / stream.
 *
 * @pre `ctx` is a bound ::t_mg_decode_ctx_t; `cell` holds `cell_bytes`.
 * @pre `key->tile_y < k_mg_bands`.
 * @post On success @p cell holds the band's packed gray8 pixels.
 * @post On any error @p cell content is unspecified.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t t_mg_tile_decode(void*                 ctx,
                                  const ra8_tile_key_t* key,
                                  uint8_t*              cell,
                                  uint32_t              cell_bytes,
                                  uint16_t*             out_w,
                                  uint16_t*             out_h)
{
  const t_mg_decode_ctx_t* dc = (const t_mg_decode_ctx_t*)ctx;
  t_mg_atlas_pread_t       ap = {.st   = dc->st,
                                 .base = (uint64_t)key->image_id * (uint64_t)k_mg_atlas_size};
  return ra8_jof_read_tile(t_mg_atlas_pread,
                           &ap,
                           dc->info,
                           key->tile_x,
                           key->tile_y,
                           nullptr,
                           0U,
                           cell,
                           cell_bytes,
                           out_w,
                           out_h);
}

/** @brief Count resident (valid) page-cache frames -- the raw residency probe. */
static uint32_t t_mg_pc_valid(void)
{
  uint32_t live = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_mg_frames; ++i) {
    if (s_pc_meta[i].valid != 0U) {
      ++live;
    }
  }
  return live;
}

/** @brief Count resident (valid) tile-cache cells -- the decoded residency probe. */
static uint32_t t_mg_tc_valid(void)
{
  uint32_t live = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_mg_cells; ++i) {
    if (s_tc_meta[i].valid != 0U) {
      ++live;
    }
  }
  return live;
}

/**
 * @brief Sample the residency of both caches into the running high-water marks.
 *
 * @param[in,out] hw High-water marks to raise (never lowered).
 *
 * @return Nothing.
 *
 * @pre `hw` is a valid high-water accumulator.
 * @pre The caches are initialised.
 * @post `hw->pc_frames` / `hw->tc_cells` are >= the current residency.
 * @post No cache state is modified.
 *
 * @note Not thread-safe.
 *
 * @par MC/DC:
 * Decision: `pc > hw->pc_frames` and `tc > hw->tc_cells` (two independent
 * single-condition max-updates; no compound `&&`/`||`).
 *
 * @since 0.1.0
 */
static void t_mg_hw_sample(t_mg_hw_t* hw)
{
  const uint32_t pc = t_mg_pc_valid();
  const uint32_t tc = t_mg_tc_valid();
  if (pc > hw->pc_frames) {
    hw->pc_frames = pc;
  }
  if (tc > hw->tc_cells) {
    hw->tc_cells = tc;
  }
}

/**
 * @brief Get band (@p page, @p band) through the tile cache and verify it.
 *
 * @details Fetches the decoded band, checks its dimensions and a strided pixel
 *          sample against the independent reference ::t_mg_payload_byte, raises
 *          the residency high-water, and releases the pin. Any wrong-band /
 *          stale / streaming bug fails the pixel compare here.
 *
 * @param[in]     tc   Initialised tile cache.
 * @param[in]     page Atlas (page) index.
 * @param[in]     band Band (tile row) index.
 * @param[in,out] hw   Residency high-water accumulator.
 *
 * @return Nothing (asserts on failure).
 *
 * @pre `tc` is initialised; `band < k_mg_bands`.
 * @pre `page` indexes an atlas within the volume.
 * @post The band's pin count is unchanged on return (get then put).
 * @post The returned band matched the reference on every sampled pixel.
 *
 * @note Not thread-safe.
 *
 * @par MC/DC:
 * (no compound decisions -- a single-condition pixel-sample loop bound plus the
 * value equalities the cache output is checked against)
 *
 * @since 0.1.0
 */
static void t_mg_touch_band(ra8_tile_cache_t* tc, uint32_t page, uint32_t band, t_mg_hw_t* hw)
{
  ra8_tile_key_t key = {};
  key.image_id       = page;
  key.tile_x         = 0U;
  key.tile_y         = (uint16_t)band;
  ra8_tile_t tile    = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_get(tc, &key, &tile));
  TEST_ASSERT_EQ(k_mg_tile_w, tile.width);
  TEST_ASSERT_EQ(k_mg_tile_h, tile.height);
  for (uint32_t i = 0U; i < (uint32_t)k_mg_payload; i += (uint32_t)k_mg_pix_stride) {
    TEST_ASSERT_EQ(t_mg_payload_byte((uint64_t)page, band, i), tile.pixels[i]);
  }
  t_mg_hw_sample(hw);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_put(tc, tile.pixels));
}

/**
 * @brief Open (parse) atlas @p page through the page cache and check its geometry.
 *
 * @param[in]  st   Volume byte stream.
 * @param[in]  page Atlas (page) index.
 * @param[out] info Receives the parsed + validated geometry.
 *
 * @return Nothing (asserts on failure).
 *
 * @pre `st` is bound to the whole volume; `info` is writable.
 * @pre `page` indexes an atlas within the volume.
 * @post `*info` holds the uniform atlas geometry.
 * @post The parse read the atlas header + footer through the page cache.
 *
 * @note Not thread-safe.
 *
 * @par MC/DC:
 * (no compound decisions -- a fixed sequence of single-condition geometry
 * equalities over the parsed atlas)
 *
 * @since 0.1.0
 */
static void t_mg_open_page(ra8_vmem_stream_t* st, uint32_t page, ra8_jof_info_t* info)
{
  t_mg_atlas_pread_t ap = {.st = st, .base = (uint64_t)page * (uint64_t)k_mg_atlas_size};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_jof_parse(t_mg_atlas_pread, &ap, (uint64_t)k_mg_atlas_size, info));
  TEST_ASSERT_EQ(k_mg_img_w, info->width);
  TEST_ASSERT_EQ(k_mg_img_h, info->height);
  TEST_ASSERT_EQ(k_mg_bands, info->tile_count);
  TEST_ASSERT_EQ(k_mg_bands, info->tile_rows);
  TEST_ASSERT_EQ(1U, info->tile_cols);
}

/**
 * @brief Wire the Layer 1/2/3 stack over the synthetic volume of @p atlas_count.
 *
 * @param[in]  atlas_count   Number of atlases in the modelled volume.
 * @param[in]  protected_pct Page-cache SLRU split (0 selects the 75% default).
 * @param[out] vs            Source registry to populate.
 * @param[out] objs          One object slot for the volume.
 * @param[out] vm            Page cache to populate.
 * @param[out] st            Volume byte stream to populate.
 * @param[out] tc            Tile cache to populate.
 * @param[out] dc            Decode context to populate.
 * @param[out] info          Shared parsed geometry (from atlas 0).
 *
 * @return The volume size in bytes (`atlas_count * atlas_size`).
 * @retval >0 Always (atlas_count >= 1).
 *
 * @pre Every out-parameter is a writable, caller-owned object.
 * @pre `atlas_count >= 1` and `protected_pct <= 100`.
 * @post The stack is ready and `*info` holds the atlas-0 geometry.
 * @post All caches are empty (cold).
 *
 * @note Not thread-safe.
 *
 * @par MC/DC:
 * (no compound decisions -- a straight-line wiring sequence of single-condition
 * init checks)
 *
 * @since 0.1.0
 */
static uint64_t t_mg_setup(uint32_t           atlas_count,
                           uint8_t            protected_pct,
                           ra8_vsource_t*     vs,
                           ra8_vsource_obj_t* objs,
                           ra8_vmem_t*        vm,
                           ra8_vmem_stream_t* st,
                           ra8_tile_cache_t*  tc,
                           t_mg_decode_ctx_t* dc,
                           ra8_jof_info_t*    info)
{
  const uint64_t vol = (uint64_t)atlas_count * (uint64_t)k_mg_atlas_size;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vsource_init(vs, objs, 1U));
  uint32_t obj = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vsource_add_paged(vs, t_mg_vol_read, nullptr, 0U, vol, &obj));

  ra8_vmem_cfg_t vcfg = {.frame_mem     = s_pc_frames,
                         .frame_bytes   = (uint32_t)k_mg_frame_bytes,
                         .frame_count   = (uint32_t)k_mg_frames,
                         .meta          = s_pc_meta,
                         .keys          = s_pc_keys,
                         .buckets       = s_pc_buckets,
                         .bucket_count  = (uint32_t)k_mg_pc_buckets,
                         .loader        = ra8_vsource_loader,
                         .loader_ctx    = vs,
                         .protected_pct = protected_pct};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_init(vm, &vcfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_stream_init(st, vm, obj, vol));

  /* Parse the shared geometry once (also the first read through the stack). */
  t_mg_open_page(st, 0U, info);

  *dc                             = (t_mg_decode_ctx_t){.st = st, .info = info};
  const ra8_tile_cache_cfg_t tcfg = {.cell_mem     = s_tc_cells,
                                     .cell_bytes   = (uint32_t)k_mg_cell_bytes,
                                     .cell_count   = (uint32_t)k_mg_cells,
                                     .meta         = s_tc_meta,
                                     .keys         = s_tc_keys,
                                     .dims         = s_tc_dims,
                                     .buckets      = s_tc_buckets,
                                     .bucket_count = (uint32_t)k_mg_tc_buckets,
                                     .decode       = t_mg_tile_decode,
                                     .decode_ctx   = dc};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_init(tc, &tcfg));
  return vol;
}
