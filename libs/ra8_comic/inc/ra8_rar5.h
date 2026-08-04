/**
 * @file ra8_rar5.h
 * @brief Clean-room RAR 5.0 ("method 50") decompressor -- LZ + Huffman + filters.
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * The compressed half of the `.cbr` comic path. ::ra8_rar_next enumerates a RAR5
 * member and, when it is packed with a RAR compressor rather than stored verbatim,
 * ::ra8_rar_extract routes the member through ::ra8_rar5_decompress instead of the
 * STORE memcpy. This unit reconstructs the original bytes from the RAR 5.0 packed
 * bitstream: a Huffman-coded LZ77 token stream (literals, length/distance matches,
 * repeated-distance matches) followed by the optional RAR5 data filters
 * (delta / x86-E8 / x86-E8E9 / ARM branch).
 *
 * @par Non-solid, single-member scope (deliberate)
 * The decoder treats each member independently: LZ back-references resolve inside
 * the member's own already-emitted output (which doubles as the sliding window),
 * so no cross-member dictionary is carried. This is the shape of a *non-solid*
 * archive -- the default WinRAR produces and the only shape comics ship in. A
 * back-reference that would reach before the member start (a *solid* archive)
 * is rejected as malformed rather than mis-decoded; solid CBR is out of scope.
 *
 * @par Zero heap (NASA P10 Rule 3)
 * All decompressor scratch -- the five Huffman decode tables, the pending-filter
 * list, the delta reorder buffer, and the streaming bit-reader refill window --
 * lives in a caller-owned ::ra8_rar5_state_t (mirroring the miniz static pool the
 * CBZ path routes through). The unpacked output goes straight into the caller's
 * page buffer, which is also the LZ window: no separate dictionary allocation.
 *
 * @par Bounded input (streaming)
 * The bit reader pulls packed bytes on demand through the archive's
 * ::ra8_rar_read_fn seam into a small fixed refill window, so a multi-megabyte
 * compressed page is decoded without ever making the packed member resident.
 *
 * @par Licensing (clean-room, deliberate)
 * Written from the public RAR 5.0 format description; it vendors **no** code from
 * RARLAB's `unrar` (whose license is non-free and forbids building a
 * RAR-compatible archiver) and is first-party MIT, carrying no SBOM entry -- the
 * same footing as the ::ra8_rar_open walker it plugs into.
 *
 * @note Not thread-safe; the single-threaded reader loop serialises access.
 *
 * @see ra8_rar.h   The block walker that locates the packed member.
 * @see ra8_comic.h The comic facade that turns a decoded page into pixels.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since Version 0.1.0
 *
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"
#include "ra8_rar.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum ra8_rar5_dims_t
 * @brief Alphabet sizes and fixed scratch dimensions of the RAR5 decoder.
 * @details The four LZ alphabets (main / distance / low-distance / repeat-length)
 *          plus the bit-length pre-table are the canonical RAR 5.0 code counts; the
 *          remaining values bound the caller-owned scratch so no allocation is ever
 *          needed. ::k_ra8_rar5_nc is the largest alphabet and therefore the width
 *          of every decode table's symbol array.
 * @since Version 0.1.0
 */
typedef enum : uint16_t {
  k_ra8_rar5_nc            = 306U, /**< Main alphabet size (literals+len+rep+filter). */
  k_ra8_rar5_dc            = 64U,  /**< Distance-slot alphabet size.                  */
  k_ra8_rar5_ldc           = 16U,  /**< Low-distance alphabet size.                   */
  k_ra8_rar5_rc            = 44U,  /**< Repeat-length alphabet size.                  */
  k_ra8_rar5_bc            = 20U,  /**< Bit-length pre-table alphabet size.           */
  k_ra8_rar5_huff_total    = 430U, /**< NC+DC+LDC+RC combined length-table size.      */
  k_ra8_rar5_max_filters   = 16U,  /**< Pending data-filter records per member.       */
  k_ra8_rar5_refill_bytes  = 512U, /**< Streaming bit-reader packed-byte window.      */
  k_ra8_rar5_delta_scratch = 512U, /**< Delta-filter reorder buffer (in-place bound). */
  k_ra8_rar5_old_dist      = 4U,   /**< Remembered recent match distances.            */
} ra8_rar5_dims_t;

/**
 * @struct ra8_rar5_dtab_t
 * @brief One canonical Huffman decode table (shared by all five RAR5 alphabets).
 *
 * @details Built by the decoder from a per-symbol bit-length vector. @p len holds
 *          the left-aligned upper-limit code for each bit length 1..15; @p pos the
 *          start index in @p num for each length; @p num the symbol assigned to each
 *          canonical code slot, in the RAR canonical order. @p max is the alphabet
 *          size and the clamp used when a (possibly hostile) code lands past the
 *          populated slots.
 *
 * @invariant `max <= k_ra8_rar5_nc`.
 * @invariant `len[i] <= len[i + 1]` for every bit length (non-decreasing).
 * @see ra8_rar5_state_t
 * @since Version 0.1.0
 */
typedef struct {
  uint32_t len[16];            /**< Left-aligned upper-limit code per bit length. */
  uint32_t pos[16];            /**< First @p num index per bit length.            */
  uint16_t num[k_ra8_rar5_nc]; /**< Symbol per canonical code slot.               */
  uint16_t max;                /**< Alphabet size (populated slot count).         */
} ra8_rar5_dtab_t;

/**
 * @struct ra8_rar5_filter_t
 * @brief One pending RAR5 data filter recorded during decode, applied after.
 * @details The RAR5 token stream can insert a filter that post-processes an output
 *          range (executables/audio compress better delta/branch-transformed). For
 *          comic JPEG/PNG pages filters are effectively never emitted, but a
 *          conformant decoder records and replays them in order once decoding ends.
 * @invariant `type <= 3` (delta / e8 / e8e9 / arm).
 * @see ra8_rar5_state_t
 * @since Version 0.1.0
 */
typedef struct {
  uint64_t start;    /**< Absolute output offset the filter transforms from. */
  uint32_t len;      /**< Length of the transformed range in bytes.          */
  uint8_t  type;     /**< Filter kind (::ra8_rar5_filter_kind_t).            */
  uint8_t  channels; /**< Delta channel count (1..32); unused otherwise.     */
} ra8_rar5_filter_t;

/**
 * @enum ra8_rar5_filter_kind_t
 * @brief The RAR5 data-filter kinds ::ra8_rar5_filter_t::type can take.
 * @details Numbered exactly as the 3-bit filter-type field of the packed stream so
 *          the decode reads straight into the enum.
 * @since Version 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_rar5_filter_delta = 0U, /**< Per-channel byte-delta de-interleave.  */
  k_ra8_rar5_filter_e8    = 1U, /**< x86 E8 call-target absolute->relative. */
  k_ra8_rar5_filter_e8e9  = 2U, /**< x86 E8+E9 call/jmp-target transform.   */
  k_ra8_rar5_filter_arm   = 3U, /**< 32-bit ARM BL branch-target transform. */
} ra8_rar5_filter_kind_t;

/**
 * @struct ra8_rar5_state_t
 * @brief Caller-owned zero-heap scratch for one ::ra8_rar5_decompress call.
 *
 * @details Every field is decoder-private working storage, reset at the start of
 *          each call; a caller only supplies the memory (typically embedded in an
 *          ::ra8_comic_t). Holds the five Huffman decode tables, the streaming
 *          bit-reader window and cursor, the recent-distance ring, and the pending
 *          filter list plus the delta reorder buffer.
 *
 * @invariant `filter_count <= k_ra8_rar5_max_filters`.
 * @invariant Contents are meaningful only between the start and end of one
 *            ::ra8_rar5_decompress call.
 * @see ra8_rar5_decompress()
 * @since Version 0.1.0
 */
typedef struct ra8_rar5_state {
  ra8_rar5_dtab_t bd;  /**< Bit-length pre-table.                 */
  ra8_rar5_dtab_t ld;  /**< Main literal/length/rep/filter table. */
  ra8_rar5_dtab_t dd;  /**< Distance-slot table.                  */
  ra8_rar5_dtab_t ldd; /**< Low-distance table.                   */
  ra8_rar5_dtab_t rd;  /**< Repeat-length table.                  */

  const ra8_rar_t* rar;                             /**< Archive backing for packed-byte refill. */
  uint64_t         base;                            /**< Absolute offset of the packed member.   */
  uint64_t         packlen;                         /**< Packed member length in bytes.          */
  uint64_t         fetched;                         /**< Packed bytes pulled from the backing.   */
  uint64_t         consumed;                        /**< Bits consumed from the packed stream.   */
  uint64_t         acc;                             /**< MSB-first bit accumulator.              */
  uint32_t         nbits;                           /**< Valid bits currently in @p acc.         */
  uint8_t          refill[k_ra8_rar5_refill_bytes]; /**< Refill window.                          */
  uint32_t         refill_len;                      /**< Valid bytes in @p refill.               */
  uint32_t         refill_pos;                      /**< Next byte to consume from @p refill.    */
  bool             overrun;                         /**< A read past the packed member ran dry.  */

  uint64_t old_dist[k_ra8_rar5_old_dist]; /**< Recent-distance ring.     */
  uint32_t last_length;                   /**< Length of the last match. */
  bool     tables_ready;                  /**< A table block was parsed. */

  ra8_rar5_filter_t filters[k_ra8_rar5_max_filters]; /**< Pending filters. */
  uint16_t          filter_count;                    /**< Filters pending. */
  uint8_t           delta[k_ra8_rar5_delta_scratch]; /**< Delta reorder.   */
} ra8_rar5_state_t;

/**
 * @brief Decompress one RAR5-packed member into the caller's output buffer.
 *
 * @details Streams the packed bytes at `[data_off, data_off + pack_size)` through
 *          the archive reader, decodes the RAR 5.0 Huffman-coded LZ token stream
 *          into @p out (which is also the LZ sliding window), then replays any
 *          filters the stream recorded. Produces exactly @p unp_size bytes on
 *          success. Malformed, truncated, or hostile input is rejected without
 *          reading or writing out of bounds -- never a crash.
 *
 * @param[in]  rar      Archive bound by ::ra8_rar_open (non-NULL).
 * @param[in]  data_off Absolute offset of the packed member data (`< rar->size`).
 * @param[in]  pack_size Packed member length in bytes (> 0).
 * @param[out] out      Destination and LZ window (non-NULL, @p out_cap writable).
 * @param[in]  out_cap  Capacity of @p out in bytes; must be >= @p unp_size.
 * @param[in]  unp_size Expected unpacked length in bytes.
 * @param[in,out] st    Caller-owned decoder scratch (non-NULL).
 * @param[out] got      Receives bytes written (non-NULL).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Member decoded; `*got == unp_size`.
 * @retval k_ra8_err_null_ptr      A required pointer argument was NULL.
 * @retval k_ra8_err_invalid_state @p rar was never bound by ::ra8_rar_open.
 * @retval k_ra8_err_no_mem        @p out_cap is smaller than @p unp_size.
 * @retval k_ra8_err_validation_failed Malformed / truncated / solid-reference stream.
 *
 * @pre @p rar was populated by ::ra8_rar_open (version 5).
 * @pre @p out holds at least @p out_cap writable bytes.
 * @post On k_ra8_ok, `out[0..*got)` holds the member's original bytes.
 * @post On any error `*got == 0` and @p out contents are unspecified.
 *
 * @note Not thread-safe; drives the archive reader and mutates @p st.
 * @see ra8_rar_extract()
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rar5_decompress(const ra8_rar_t*  rar,
                                            uint64_t          data_off,
                                            uint64_t          pack_size,
                                            uint8_t*          out,
                                            size_t            out_cap,
                                            uint64_t          unp_size,
                                            ra8_rar5_state_t* st,
                                            size_t*           got);

#ifdef __cplusplus
}
#endif
