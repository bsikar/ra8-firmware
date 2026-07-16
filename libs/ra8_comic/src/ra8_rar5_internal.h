/**
 * @file ra8_rar5_internal.h
 * @brief Cross-TU seam between the RAR5 entropy front-end and the LZ decoder.
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * The clean-room RAR 5.0 decompressor is split across two translation units so
 * neither exceeds the file-size maintainability cap:
 * - `ra8_rar5_tables.c` -- the entropy front-end: the streaming MSB-first bit
 *   reader, the canonical-Huffman decode-table builder, and the per-block header
 *   plus length-table parser.
 * - `ra8_rar5.c` -- the LZ decoder driver: the length/distance token decode, the
 *   post-decode data filters, the block/stream loop, and the public entry
 *   ::ra8_rar5_decompress.
 *
 * The dependency is one-directional: the driver calls into the front-end for a
 * raw bit read (::ra8_rar5_get), one Huffman symbol (::ra8_rar5_decode_num), a
 * block header (::ra8_rar5_read_block_header), and a table block
 * (::ra8_rar5_read_tables). The front-end never calls back into the driver. This
 * header is the entire contract between them plus the shared bit-field / symbol /
 * filter enum vocabulary and the decoded block-header struct; it is not part of
 * the public ::ra8_rar5_decompress surface (`ra8_rar5.h`).
 *
 * @note Not thread-safe; the single-threaded reader loop serialises access.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_rar5.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum r5_mask_t
 * @brief Wide bit masks the decoder applies to accumulated fields.
 * @details The DecodeNumber 16-bit window (low bit cleared), the low-nibble mask
 *          for a transmitted bit length, and the single-byte mask.
 * @since Version 0.1.0
 */
typedef enum : uint32_t {
  k_r5_bf_mask     = 0xFFFEU, /**< DecodeNumber bit-field, low bit cleared. */
  k_r5_nibble_mask = 0x0FU,   /**< Low nibble of a bit-length byte.         */
  k_r5_byte_mask   = 0xFFU,   /**< Single-byte mask.                        */
} r5_mask_t;

/**
 * @enum r5_bits_t
 * @brief Fixed field widths and code-length limits of the RAR5 bitstream.
 * @since Version 0.1.0
 */
typedef enum : uint8_t {
  k_r5_byte_bits  = 8U,  /**< Bits per packed byte.                    */
  k_r5_bf_bits    = 16U, /**< DecodeNumber look-ahead window width.    */
  k_r5_maxbits    = 15U, /**< Longest Huffman code length.             */
  k_r5_len_escape = 15U, /**< 4-bit escape in the BD length list.      */
  k_r5_low3_mask  = 7U,  /**< Low three bits (byte-alignment residue). */
  k_r5_acc_bits   = 64U, /**< Bit-accumulator width (mask guard).      */
} r5_bits_t;

/**
 * @enum r5_blockflag_t
 * @brief RAR5 compressed-block header flag layout.
 * @details Bits 0-2 give the last-byte valid-bit count minus one; bits 3-4 the
 *          block-size byte count minus one; bit 6 marks the last block in the file;
 *          bit 7 marks a block that carries fresh Huffman tables.
 * @since Version 0.1.0
 */
typedef enum : uint8_t {
  k_r5_bf_bitsize_mask = 0x07U, /**< Last-byte valid-bit count minus one. */
  k_r5_bf_bcount_shift = 3U,    /**< Shift to the block-size byte count.  */
  k_r5_bf_bcount_mask  = 3U,    /**< Mask of the block-size byte count.   */
  k_r5_bf_last         = 0x40U, /**< Last block in the file.              */
  k_r5_bf_tables       = 0x80U, /**< Block carries new Huffman tables.    */
  k_r5_hdr_chk_seed    = 0x5AU, /**< Header-checksum seed constant.       */
} r5_blockflag_t;

/**
 * @enum r5_mainsym_t
 * @brief Reserved main-alphabet symbol boundaries above the 256 literals.
 * @details Symbol 256 reads a filter, 257 repeats the last match, 258-261 select a
 *          remembered distance, and 262+ are LZ length slots.
 * @since Version 0.1.0
 */
typedef enum : uint16_t {
  k_r5_sym_filter   = 256U, /**< Read one data filter.      */
  k_r5_sym_replast  = 257U, /**< Repeat the last match.     */
  k_r5_sym_repdist0 = 258U, /**< First remembered distance. */
  k_r5_sym_lenbase  = 262U, /**< First LZ length slot.      */
} r5_mainsym_t;

/**
 * @enum r5_tblcode_t
 * @brief Continuation codes and run bases of the length-table encoding.
 * @details Values 0-15 are literal bit lengths; 16/17 copy the previous length for
 *          a short/long run; 18/19 emit a short/long run of zero lengths.
 * @since Version 0.1.0
 */
typedef enum : uint8_t {
  k_r5_tbl_copy_long  = 17U, /**< Copy previous length, 7-bit run. */
  k_r5_tbl_zero_short = 18U, /**< Zero length, 3-bit run.          */
  k_r5_tbl_zero_long  = 19U, /**< Zero length, 7-bit run.          */
  k_r5_run_long_bits  = 7U,  /**< Long-run extra-bit width.        */
  k_r5_run_long_add   = 11U, /**< Long-run length bias.            */
  k_r5_zeros_extra    = 2U,  /**< BD zero-run length bias.         */
} r5_tblcode_t;

/**
 * @enum r5_distth_t
 * @brief Distance thresholds that add 1..3 to a decoded match length.
 * @since Version 0.1.0
 */
typedef enum : uint32_t {
  k_r5_dist_th1 = 0x100U,   /**< +1 length above this distance. */
  k_r5_dist_th2 = 0x2000U,  /**< +1 more above this distance.   */
  k_r5_dist_th3 = 0x40000U, /**< +1 more above this distance.   */
} r5_distth_t;

/**
 * @enum r5_filter_t
 * @brief Field widths of the RAR5 in-stream filter descriptor.
 * @since Version 0.1.0
 */
typedef enum : uint8_t {
  k_r5_ftype_bits = 3U, /**< Filter-type field width.         */
  k_r5_fchan_bits = 5U, /**< Delta channel-count field width. */
} r5_filter_t;

/**
 * @enum r5_filterop_t
 * @brief Opcode bytes and instruction widths of the executable filters.
 * @since Version 0.1.0
 */
typedef enum : uint8_t {
  k_r5_x86_call = 0xE8U, /**< x86 near CALL opcode.       */
  k_r5_x86_jmp  = 0xE9U, /**< x86 near JMP opcode.        */
  k_r5_x86_ilen = 5U,    /**< CALL/JMP instruction width. */
  k_r5_arm_bl   = 0xEBU, /**< ARM BL opcode byte.         */
} r5_filterop_t;

/**
 * @enum r5_armmask_t
 * @brief The 24-bit branch-offset mask of the ARM filter.
 * @since Version 0.1.0
 */
typedef enum : uint32_t {
  k_r5_arm_off_mask = 0xFFFFFFU, /**< ARM BL 24-bit word offset. */
} r5_armmask_t;

/**
 * @enum r5_pad_t
 * @brief Slack, past the packed length, tolerated while draining the bit stream.
 * @details The token loop is bounded by @c consumed reaching the packed size plus
 *          this many padding bits, so a hostile stream cannot spin forever on
 *          zero-output symbols.
 * @since Version 0.1.0
 */
typedef enum : uint16_t {
  k_r5_max_pad_bits = 64U, /**< Bit slack past the packed member end. */
} r5_pad_t;

/**
 * @struct r5_block_t
 * @brief Decoded fields of one RAR5 compressed-block header.
 * @details Filled by ::ra8_rar5_read_block_header and consumed by the driver's
 *          block loop.
 * @since Version 0.1.0
 */
typedef struct {
  uint64_t size;      /**< Block size in bytes from BlockStart. */
  uint32_t last_bits; /**< Valid bits in the block's last byte. */
  bool     tables;    /**< Block carries fresh Huffman tables.  */
  bool     last;      /**< Block is the last in the file.       */
} r5_block_t;

/**
 * @brief Peek and consume @p n bits from the streaming reader in one step.
 * @details The front-end's raw bit read: ensures the bits are buffered, returns
 *          them oldest-bit-first, and advances the consumed-bit total. The LZ
 *          driver uses it to read length/distance extra bits and filter fields.
 * @param[in,out] st Decoder state (non-NULL).
 * @param[in]     n  Bits to read (1..32).
 * @return The consumed @p n bits, oldest bit most significant.
 * @retval 0 When the bits are all zero (or all past-end padding).
 * @pre @p st is a bound decoder state.
 * @pre 1 <= @p n <= 32.
 * @post `st->consumed` increased by @p n.
 * @post `st->nbits` decreased by @p n.
 * @note Not thread-safe.
 * @see ra8_rar5_decode_num()
 * @since Version 0.1.0
 */
uint32_t ra8_rar5_get(ra8_rar5_state_t* st, uint32_t n);

/**
 * @brief Decode one Huffman symbol from @p d, consuming its code bits.
 * @details Reads the 16-bit look-ahead window, finds the matching bit length by
 *          comparing against the upper limits, then indexes the symbol slot. An
 *          out-of-range slot (malformed stream) clamps to slot 0 rather than reading
 *          out of bounds. The LZ driver uses it for the main / distance / low-distance
 *          / repeat-length alphabets.
 * @param[in,out] st Decoder state (non-NULL).
 * @param[in]     d  Decode table built by the front-end (non-NULL).
 * @return The decoded symbol.
 * @retval 0 On a clamped (out-of-range) or genuinely-zero code.
 * @pre @p st is a bound decoder state.
 * @pre @p d was built by the front-end table builder.
 * @post `st->consumed` advanced by the code's bit length (>= 1).
 * @post The returned symbol is < `d->max`.
 * @note Not thread-safe.
 * @see ra8_rar5_get()
 * @since Version 0.1.0
 */
uint32_t ra8_rar5_decode_num(ra8_rar5_state_t* st, const ra8_rar5_dtab_t* d);

/**
 * @brief Read and validate one RAR5 block header at the current bit position.
 * @details Byte-aligns, reads the flags, the variable-width block size, and the
 *          checksum byte, rejecting a header whose checksum does not match. The
 *          driver's block loop calls it once per compressed block.
 * @param[in,out] st Decoder state (non-NULL).
 * @param[out]    b  Receives the decoded block fields (non-NULL).
 * @return ra8_err_t status.
 * @retval k_ra8_ok                    Header decoded and checksum valid.
 * @retval k_ra8_err_validation_failed The checksum byte did not match.
 * @pre @p st is a bound decoder state.
 * @pre @p b is writable.
 * @post On k_ra8_ok, @p b holds the block size, table flag, and last flag.
 * @post `st->consumed` advanced past the whole header.
 * @note Not thread-safe.
 * @see ra8_rar5_read_tables()
 * @since Version 0.1.0
 */
ra8_err_t ra8_rar5_read_block_header(ra8_rar5_state_t* st, r5_block_t* b);

/**
 * @brief Parse a table block: build the BD pre-table then the four LZ tables.
 * @details Reads the BD lengths, builds the BD decode table, decodes the combined
 *          length table, and splits it into the main / distance / low-distance /
 *          repeat-length decode tables. The driver calls it whenever a block header
 *          flags fresh tables.
 * @param[in,out] st Decoder state (non-NULL).
 * @return ra8_err_t status.
 * @retval k_ra8_ok                    All five tables were built.
 * @retval k_ra8_err_validation_failed A malformed length table.
 * @pre @p st is a bound decoder state at a table block.
 * @pre @p st scratch tables are writable.
 * @post On k_ra8_ok, `st->tables_ready` is true.
 * @post The four LZ decode tables are usable by ::ra8_rar5_decode_num.
 * @note Not thread-safe.
 * @see ra8_rar5_read_block_header()
 * @since Version 0.1.0
 */
ra8_err_t ra8_rar5_read_tables(ra8_rar5_state_t* st);

#ifdef __cplusplus
}
#endif
