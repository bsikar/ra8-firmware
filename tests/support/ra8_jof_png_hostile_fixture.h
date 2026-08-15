/**
 * @file ra8_jof_png_hostile_fixture.h
 * @brief Caller-owned buffers and format constants for hostile PNG stream tests.
 *
 * @details
 * Centralizes the bounded source, producer workspace, memstore, scanline
 * staging, and field offsets used only by test_ra8_jof_png_hostile.c. Every
 * object retains internal linkage; inclusion by any second translation unit
 * would duplicate the large fixture storage and is intentionally unsupported.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/** @brief Trailing-garbage probe appended after a complete zlib stream. */
typedef enum : uint16_t {
  k_png_hostile_fill_trailing = 0xEEU, /**< Must be refused, not decoded. */
  k_png_hostile_trailing_len  = 64U,   /**< Trailing garbage bytes.       */
} png_hostile_trailing_t;

/**
 * @enum t_png_layout_t
 * @brief PNG serialisation offsets and field widths the crafters poke.
 *
 * @details
 * `internal_begin_png()` takes an IHDR byte index in its `extra_field` argument, so the
 * `k_t_ihdr_off_*` names double as "which IHDR field is being corrupted".
 * Offsets ending `_b<N>` are the `N`-th byte of a big-endian 32-bit field.
 */
typedef enum : uint8_t {
  k_t_be32_hi_shift        = 24U,   /**< Shift for the top byte of a big-endian 32-bit field.     */
  k_t_byte_mask            = 0xFFU, /**< Low-byte mask used while serialising a big-endian field. */
  k_t_png_ihdr_len         = 13U,   /**< IHDR payload length, fixed by the PNG spec.              */
  k_t_ihdr_short_len       = 12U,   /**< One byte short of a legal IHDR; must be refused.         */
  k_t_ihdr_off_h_b1        = 5U,    /**< Height byte 1 within the IHDR payload.                   */
  k_t_ihdr_off_h_b3        = 7U,    /**< Height byte 3 within the IHDR payload.                   */
  k_t_ihdr_off_ct          = 9U,    /**< Colour-type byte within the IHDR payload.                */
  k_t_ihdr_off_compression = 10U,   /**< Compression-method byte; only 0 is legal.                */
  k_t_ihdr_off_filter      = 11U,   /**< Filter-method byte; only 0 is legal.                     */
  k_t_zlib_cmf             = 0x78U, /**< zlib CMF: deflate with a 32 KiB window.                  */
  k_t_filter_invalid       = 9U,    /**< Row filter outside the legal 0..4 range.                 */
} t_png_layout_t;

/**
 * @enum t_trunc_t
 * @brief Tail bytes removed to truncate a stream mid-payload.
 *
 * @details
 * Each value is chosen so the cut lands inside a specific chunk's payload
 * rather than on a chunk boundary, which is what exercises the partial-read
 * path instead of the clean end-of-stream path.
 */
typedef enum : uint8_t {
  k_t_trunc_plte_bytes = 7U, /**< Cut that lands mid-PLTE payload. */
  k_t_trunc_trns_bytes = 5U, /**< Cut that lands mid-tRNS payload. */
} t_trunc_t;

/**
 * @enum t_junk_t
 * @brief Filler bytes appended after a well-formed stream.
 *
 * @details
 * Arbitrary by design -- their only requirement is that no two are equal, so a
 * decoder that runs past the stream end reveals which byte it consumed.
 */
typedef enum : uint8_t {
  k_t_trailer_junk_b0 = 0xAAU, /**< First byte past the zlib stream.  */
  k_t_trailer_junk_b1 = 0xBBU, /**< Second byte past the zlib stream. */
  k_t_trailer_junk_b2 = 0xCCU, /**< Third byte past the zlib stream.  */
} t_junk_t;

/**
 * @enum t_corpus_t
 * @brief Geometry and buffer sizing for the crafted corpora.
 */
typedef enum : uint16_t {
  k_t_kib               = 1024U, /**< Bytes per KiB.                                              */
  k_t_arena_kib         = 512U,  /**< Direct-seam bump-arena size, in KiB.                        */
  k_t_raw_cap           = 4096U, /**< Unfiltered-scanline scratch capacity, bytes.                */
  k_t_zbuf_cap          = 4200U, /**< Stored-deflate output capacity: just over one 4 KiB window. */
  k_t_zbig_kib          = 64U,   /**< Oversized zlib staging buffer, in KiB.                      */
  k_t_frame_dim         = 300U,  /**< Edge of the square frame used for the large-image arms.     */
  k_t_frame_stride      = 301U,  /**< Its row stride: k_t_frame_dim pixels + 1 filter byte.       */
  k_t_window_frame_w    = 15U,   /**< Width of the frame whose zlib output is exactly one window. */
  k_t_window_frame_h    = 255U,  /**< Its height; 15x255 gray encodes to exactly 4096 bytes.      */
  k_t_plte_oversize_len = 771U,  /**< PLTE payload one entry past the 256-entry maximum.          */
  k_t_trns_oversize_len = 300U,  /**< tRNS payload past the 256-entry maximum.                    */
} t_corpus_t;

/** @brief Corpus geometry + buffer sizing. */
enum : uint32_t {
  k_t_w         = 8U,            /**< Tiny test image width.              */
  k_t_h         = 8U,            /**< Tiny test image height.             */
  k_t_tile      = 8U,            /**< Tile edge under test.               */
  k_t_src_cap   = 256U * 1024U,  /**< Crafted source capacity.            */
  k_t_store_cap = 128U * 1024U,  /**< Memstore capacity.                  */
  k_t_work_cap  = 1024U * 1024U, /**< Producer work arena.                */
  k_t_many      = 4100U,         /**< Chunk-budget flood count.           */
  k_t_zout_cap  = 512U,          /**< internal_make_zlib caller capacity. */
};

/** @brief Crafted source bytes. */
static uint8_t s_src[k_t_src_cap];
/** @brief Crafted source length. */
static size_t s_src_len;
/** @brief Producer work arena. */
static uint8_t s_work[k_t_work_cap];
/** @brief Memstore backing. */
static uint8_t s_store_buf[k_t_store_cap];
/** @brief Raw (filtered) scanline staging. */
static uint8_t s_raw[k_t_raw_cap];

/**
 * @struct t_pull_t
 * @brief Memory pull source with an optional hard-failure trigger.
 */
typedef struct {
  size_t pos;     /**< Read cursor.                                   */
  size_t fail_at; /**< Byte offset that returns an error (0 = never). */
} t_pull_t;
