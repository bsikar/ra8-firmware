/**
 * @file ra8_rar5.c
 * @brief Implementation of the clean-room RAR 5.0 decompressor (ra8_rar5.h).
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * A from-scratch RAR 5.0 ("method 50") unpacker: a streaming MSB-first bit reader
 * feeds canonical-Huffman decode tables that drive an LZ77 token loop, writing the
 * reconstructed bytes straight into the caller's output buffer (which is also the
 * sliding window). After the token stream completes, any recorded data filters
 * (delta / x86 / ARM) are replayed over their output ranges.
 *
 * @par Structure
 * - Bit reader: ::s_fetch_byte / ::s_ensure / ::s_peek / ::s_drop / ::s_get /
 *   ::s_align pull packed bytes through the archive ::ra8_rar_read_fn on demand.
 * - Huffman: ::s_make_tables builds a canonical decode table from a bit-length
 *   vector; ::s_decode_num reads one symbol.
 * - Container: ::s_read_block_header + ::s_read_tables parse each compressed block.
 * - LZ: ::s_decode_token classifies one main symbol into literal / match /
 *   repeat-match / filter, copying matches with ::s_copy_match.
 * - Filters: ::s_apply_filters replays delta / x86 / ARM transforms post-decode.
 *
 * @par Conformance note (deliberate, verifiable scope)
 * The LZ + Huffman container grammar follows the published RAR 5.0 format so a
 * genuine WinRAR-produced member decodes. The delta / x86 / ARM data filters are
 * implemented as reversible transforms self-consistent with this codec's writer;
 * byte-exact equivalence to WinRAR's filter output cannot be cross-checked here (no
 * free RAR compressor exists) and comic JPEG/PNG pages never carry these filters,
 * so that last equivalence is left to an owner-supplied real archive.
 *
 * @since Version 0.1.0
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "ra8_rar5.h"

#include <string.h>

#include "ra8_check.h"

/** @brief Log tag for RAR5 decoder diagnostics. */
static const char* const s_tag_rar5 = "ra8_rar5";

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

/* ---- little-endian scalar reads/writes ---------------------------------- */

/**
 * @brief Read a little-endian uint32 from four unaligned bytes.
 * @details memcpy-based so the source may sit at any alignment on a little-endian
 *          host/target.
 * @param[in] p Pointer to four readable bytes (non-NULL).
 * @return The decoded value.
 * @retval 0 When all four bytes are zero.
 * @pre @p p addresses at least four readable bytes.
 * @pre The host is little-endian.
 * @post No state is modified (pure read).
 * @post The result is a pure function of the four bytes.
 * @note Thread-safe: pure read.
 * @since Version 0.1.0
 */
static uint32_t s_rd_le32(const uint8_t* p)
{
  uint32_t v = 0U;
  (void)memcpy(&v, p, sizeof(v));
  return v;
}

/**
 * @brief Write a uint32 as four little-endian bytes.
 * @details memcpy-based so the destination may sit at any alignment.
 * @param[out] p Pointer to four writable bytes (non-NULL).
 * @param[in]  v Value to store.
 * @return Nothing.
 * @pre @p p addresses at least four writable bytes.
 * @pre The host is little-endian.
 * @post `p[0..4)` holds @p v in little-endian order.
 * @post No other state is modified.
 * @note Thread-safe: writes only through @p p.
 * @since Version 0.1.0
 */
static void s_wr_le32(uint8_t* p, uint32_t v)
{
  (void)memcpy(p, &v, sizeof(v));
}

/* ---- streaming MSB-first bit reader ------------------------------------- */

/**
 * @brief Pull the next packed byte, refilling the window from the backing reader.
 * @details Serves bytes from the ::ra8_rar5_state_t::refill window, refetching a
 *          fresh window through the archive reader when it drains. Past the packed
 *          member end it returns 0 and latches ::ra8_rar5_state_t::overrun.
 * @param[in,out] st Decoder state (non-NULL).
 * @return The next packed byte, or 0 at/after the member end.
 * @retval 0 The packed member is exhausted (overrun latched).
 * @pre @p st was bound with a valid archive reader.
 * @pre @p st::base + @p st::packlen is within the archive.
 * @post On success @p st::refill_pos advanced by one.
 * @post At member end @p st::overrun is true.
 * @note Not thread-safe; drives the archive reader.
 * @since Version 0.1.0
 */
static uint8_t s_fetch_byte(ra8_rar5_state_t* st)
{
  if (st->refill_pos >= st->refill_len) {
    if (st->fetched >= st->packlen) {
      st->overrun = true;
      return 0U;
    }
    const uint64_t remain = st->packlen - st->fetched;
    const size_t   want   = (remain < (uint64_t)k_ra8_rar5_refill_bytes)
                              ? (size_t)remain
                              : (size_t)k_ra8_rar5_refill_bytes;
    const size_t   r      = st->rar->read(st->rar->ctx, st->base + st->fetched, st->refill, want);
    if (r == 0U) {
      st->overrun = true;
      return 0U;
    }
    st->refill_len = (uint32_t)r;
    st->refill_pos = 0U;
    st->fetched += (uint64_t)r;
  }
  const uint8_t b = st->refill[st->refill_pos];
  st->refill_pos += 1U;
  return b;
}

/**
 * @brief Ensure at least @p n bits are buffered in the accumulator.
 * @details Shifts whole bytes in from ::s_fetch_byte until the buffered bit count
 *          reaches @p n; past-end fetches contribute zero bits.
 * @param[in,out] st Decoder state (non-NULL).
 * @param[in]     n  Bits required (1..32).
 * @return Nothing.
 * @pre @p st is a bound decoder state.
 * @pre @p n <= 32.
 * @post `st->nbits >= n`.
 * @post @p st::acc holds the buffered bits, oldest at the top.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
static void s_ensure(ra8_rar5_state_t* st, uint32_t n)
{
  while (st->nbits < n) { /* bound: n<=32, +8 bits per pass -> <=4 passes */
    const uint8_t b = s_fetch_byte(st);
    st->acc         = (st->acc << k_r5_byte_bits) | (uint64_t)b;
    st->nbits += (uint32_t)k_r5_byte_bits;
  }
}

/**
 * @brief Peek the next @p n bits without consuming them.
 * @details Ensures the bits are buffered, then returns the top @p n bits of the
 *          accumulator as a right-aligned value.
 * @param[in,out] st Decoder state (non-NULL).
 * @param[in]     n  Bits to read (1..32).
 * @return The next @p n bits, oldest bit most significant.
 * @retval 0 When the next @p n bits are all zero (or all padding).
 * @pre @p st is a bound decoder state.
 * @pre 1 <= @p n <= 32.
 * @post @p st::consumed is unchanged (a pure peek).
 * @post At least @p n bits remain buffered.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
static uint32_t s_peek(ra8_rar5_state_t* st, uint32_t n)
{
  s_ensure(st, n);
  const uint64_t mask = ((uint64_t)1U << n) - (uint64_t)1U;
  return (uint32_t)((st->acc >> (st->nbits - n)) & mask);
}

/**
 * @brief Consume @p n previously-peeked bits.
 * @details Reduces the buffered count and the running consumed-bit total, keeping
 *          the accumulator masked to its remaining valid bits.
 * @param[in,out] st Decoder state (non-NULL).
 * @param[in]     n  Bits to consume (<= currently buffered).
 * @return Nothing.
 * @pre @p st is a bound decoder state.
 * @pre @p n <= @p st::nbits.
 * @post `st->nbits` decreased by @p n and `st->consumed` increased by @p n.
 * @post @p st::acc retains only its remaining valid bits.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
static void s_drop(ra8_rar5_state_t* st, uint32_t n)
{
  st->nbits -= n;
  st->consumed += (uint64_t)n;
  const uint64_t mask = (st->nbits >= (uint32_t)k_r5_acc_bits)
                          ? ~(uint64_t)0U
                          : (((uint64_t)1U << st->nbits) - (uint64_t)1U);
  st->acc &= mask;
}

/**
 * @brief Peek and consume @p n bits in one step.
 * @details Convenience wrapper: ::s_peek followed by ::s_drop of the same width.
 * @param[in,out] st Decoder state (non-NULL).
 * @param[in]     n  Bits to read (1..32).
 * @return The consumed @p n bits, oldest bit most significant.
 * @retval 0 When the bits are all zero.
 * @pre @p st is a bound decoder state.
 * @pre 1 <= @p n <= 32.
 * @post `st->consumed` increased by @p n.
 * @post `st->nbits` decreased by @p n.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
static uint32_t s_get(ra8_rar5_state_t* st, uint32_t n)
{
  const uint32_t v = s_peek(st, n);
  s_drop(st, n);
  return v;
}

/**
 * @brief Discard bits up to the next byte boundary.
 * @details Drops the sub-byte residue so the following read starts on a whole
 *          packed byte, as each RAR5 block header requires.
 * @param[in,out] st Decoder state (non-NULL).
 * @return Nothing.
 * @pre @p st is a bound decoder state.
 * @pre The residue bits are already buffered (always true after a read).
 * @post `st->consumed` is a multiple of 8.
 * @post No packed byte is skipped whole.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
static void s_align(ra8_rar5_state_t* st)
{
  s_drop(st, st->nbits & (uint32_t)k_r5_low3_mask);
}

/* ---- canonical Huffman decode tables ------------------------------------ */

/**
 * @brief Fill a decode table's per-length upper-limit and start-position arrays.
 * @details Turns a per-bit-length code count into the left-aligned upper-limit
 *          code (::ra8_rar5_dtab_t::len) and the first-symbol index
 *          (::ra8_rar5_dtab_t::pos) for each length, the canonical-Huffman prefix.
 * @param[out] d     Table to populate (non-NULL).
 * @param[in]  count Per-length code counts (index 1..15 used).
 * @return Nothing.
 * @pre @p count has at least 16 entries with `count[0] == 0`.
 * @pre @p d is writable.
 * @post `d->len[i]` is non-decreasing across the bit lengths.
 * @post `d->pos[i]` accumulates the preceding length counts.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
static void s_tab_limits(ra8_rar5_dtab_t* d, const uint32_t* count)
{
  d->len[0]      = 0U;
  d->pos[0]      = 0U;
  uint32_t upper = 0U;
  for (uint32_t i = 1U; i < (uint32_t)(k_r5_maxbits + 1U); ++i) { /* bound: 15 lengths */
    upper += count[i];
    d->len[i] = upper << ((uint32_t)k_r5_bf_bits - i);
    upper <<= 1U;
    d->pos[i] = d->pos[i - 1U] + count[i - 1U];
  }
}

/**
 * @brief Build a canonical Huffman decode table from a bit-length vector.
 * @details Counts codes per length, computes the prefix limits via ::s_tab_limits,
 *          then assigns each non-zero-length symbol to its canonical code slot in
 *          symbol order. Zero-length symbols are absent from the table.
 * @param[out] d       Table to populate (non-NULL).
 * @param[in]  lengths Per-symbol bit lengths (@p size entries).
 * @param[in]  size    Alphabet size (<= ::k_ra8_rar5_nc).
 * @return Nothing.
 * @pre @p lengths holds @p size readable bytes.
 * @pre @p size <= ::k_ra8_rar5_nc.
 * @post `d->max == size` and populated slots hold their symbols.
 * @post Every slot index read by ::s_decode_num is initialised.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
static void s_make_tables(ra8_rar5_dtab_t* d, const uint8_t* lengths, uint16_t size)
{
  uint32_t count[k_r5_maxbits + 1U] = {};
  for (uint16_t i = 0U; i < size; ++i) { /* bound: size <= k_ra8_rar5_nc */
    count[lengths[i] & (uint8_t)k_r5_nibble_mask] += 1U;
  }
  count[0] = 0U;
  s_tab_limits(d, count);
  (void)memset(d->num, 0, (size_t)size * sizeof(d->num[0]));
  uint32_t copypos[k_r5_maxbits + 1U];
  for (uint32_t i = 0U; i < (uint32_t)(k_r5_maxbits + 1U); ++i) { /* bound: 16 */
    copypos[i] = d->pos[i];
  }
  for (uint16_t i = 0U; i < size; ++i) { /* bound: size */
    const uint8_t bl = lengths[i] & (uint8_t)k_r5_nibble_mask;
    if (bl != 0U) {
      d->num[copypos[bl]] = i;
      copypos[bl] += 1U;
    }
  }
  d->max = size;
}

/**
 * @brief Decode one Huffman symbol from @p d, consuming its code bits.
 * @details Reads the 16-bit look-ahead window, finds the matching bit length by
 *          comparing against the upper limits, then indexes the symbol slot. An
 *          out-of-range slot (malformed stream) clamps to slot 0 rather than reading
 *          out of bounds.
 * @param[in,out] st Decoder state (non-NULL).
 * @param[in]     d  Decode table built by ::s_make_tables (non-NULL).
 * @return The decoded symbol.
 * @retval 0 On a clamped (out-of-range) or genuinely-zero code.
 * @pre @p st is a bound decoder state.
 * @pre @p d was built by ::s_make_tables.
 * @post `st->consumed` advanced by the code's bit length (>= 1).
 * @post The returned symbol is < `d->max`.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
static uint32_t s_decode_num(ra8_rar5_state_t* st, const ra8_rar5_dtab_t* d)
{
  const uint32_t bf   = s_peek(st, (uint32_t)k_r5_bf_bits) & (uint32_t)k_r5_bf_mask;
  uint32_t       bits = (uint32_t)k_r5_maxbits;
  for (uint32_t i = 1U; i < (uint32_t)k_r5_maxbits; ++i) { /* bound: 14 lengths */
    if (bf < d->len[i]) {
      bits = i;
      break;
    }
  }
  s_drop(st, bits);
  const uint32_t dist = (bf - d->len[bits - 1U]) >> ((uint32_t)k_r5_bf_bits - bits);
  uint32_t       pos  = d->pos[bits] + dist;
  if (pos >= (uint32_t)d->max) {
    pos = 0U;
  }
  return d->num[pos];
}

/* ---- block header + Huffman tables -------------------------------------- */

/**
 * @brief Compute the RAR5 block-header checksum byte.
 * @details XOR of the seed, the flags, and the three low bytes of the block size --
 *          the value the header stores for validation.
 * @param[in] flags     Block-flags byte.
 * @param[in] blocksize Decoded block size.
 * @return The expected checksum byte.
 * @retval 0 When the XOR of all inputs is zero.
 * @pre @p flags is the raw block-flags byte.
 * @pre @p blocksize is the decoded block size.
 * @post No state is modified (pure function).
 * @post The result fits one byte.
 * @note Thread-safe: pure.
 * @since Version 0.1.0
 */
static uint8_t s_checksum(uint32_t flags, uint64_t blocksize)
{
  uint32_t x = (uint32_t)k_r5_hdr_chk_seed ^ flags;
  x ^= (uint32_t)(blocksize & (uint64_t)k_r5_byte_mask);
  x ^= (uint32_t)((blocksize >> k_r5_byte_bits) & (uint64_t)k_r5_byte_mask);
  x ^= (uint32_t)((blocksize >> (k_r5_byte_bits * 2U)) & (uint64_t)k_r5_byte_mask);
  return (uint8_t)(x & (uint32_t)k_r5_byte_mask);
}

/**
 * @struct r5_block_t
 * @brief Decoded fields of one RAR5 compressed-block header.
 * @details Filled by ::s_read_block_header and consumed by ::s_open_block.
 * @since Version 0.1.0
 */
typedef struct {
  uint64_t size;      /**< Block size in bytes from BlockStart. */
  uint32_t last_bits; /**< Valid bits in the block's last byte. */
  bool     tables;    /**< Block carries fresh Huffman tables.  */
  bool     last;      /**< Block is the last in the file.       */
} r5_block_t;

/**
 * @brief Read and validate one RAR5 block header at the current bit position.
 * @details Byte-aligns, reads the flags, the variable-width block size, and the
 *          checksum byte, rejecting a header whose checksum does not match.
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
 * @since Version 0.1.0
 */
static ra8_err_t s_read_block_header(ra8_rar5_state_t* st, r5_block_t* b)
{
  s_align(st);
  const uint32_t flags = s_get(st, (uint32_t)k_r5_byte_bits);
  const uint32_t bytecount =
    ((flags >> (uint32_t)k_r5_bf_bcount_shift) & (uint32_t)k_r5_bf_bcount_mask) + 1U;
  uint64_t bsz = 0U;
  for (uint32_t i = 0U; i < bytecount; ++i) { /* bound: bytecount <= 4 */
    bsz |= (uint64_t)s_get(st, (uint32_t)k_r5_byte_bits) << (i * (uint32_t)k_r5_byte_bits);
  }
  const uint32_t saved = s_get(st, (uint32_t)k_r5_byte_bits);
  if ((uint32_t)s_checksum(flags, bsz) != saved) {
    return k_ra8_err_validation_failed;
  }
  b->size      = bsz;
  b->last_bits = (flags & (uint32_t)k_r5_bf_bitsize_mask) + 1U;
  b->tables    = (flags & (uint32_t)k_r5_bf_tables) != 0U;
  b->last      = (flags & (uint32_t)k_r5_bf_last) != 0U;
  return k_ra8_ok;
}

/**
 * @brief Extend @p out with @p count zero bit-lengths, bounded by @p max.
 * @details Appends up to @p count zero entries from @p start, stopping at @p max so a
 *          hostile run length can never overflow the array.
 * @param[in,out] out   Bit-length array being filled.
 * @param[in]     start First index to write.
 * @param[in]     count Zero entries to append.
 * @param[in]     max   Array capacity (no write at/after it).
 * @return The next write index after the appended zeros.
 * @retval start When @p start is already at @p max.
 * @pre @p out holds @p max writable bytes.
 * @pre @p start <= @p max.
 * @post `out[start .. min(start+count, max))` are zero.
 * @post The return value is <= @p max.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
static uint32_t s_fill_zeros(uint8_t* out, uint32_t start, uint32_t count, uint32_t max)
{
  uint32_t i = start;
  for (uint32_t c = 0U; c < count && i < max; ++c) { /* bound: count, i<max */
    out[i] = 0U;
    i += 1U;
  }
  return i;
}

/**
 * @brief Read the 20-entry bit-length pre-table (BD) length list.
 * @details Each entry is a 4-bit length; the value 15 escapes to a following 4-bit
 *          field that is either a literal length-15 code (0) or a zero run (n+2).
 * @param[in,out] st  Decoder state (non-NULL).
 * @param[out]    out Receives ::k_ra8_rar5_bc bit lengths (non-NULL).
 * @return ra8_err_t status.
 * @retval k_ra8_ok The 20 lengths were read.
 * @pre @p st is a bound decoder state.
 * @pre @p out holds ::k_ra8_rar5_bc writable bytes.
 * @post `out[0 .. k_ra8_rar5_bc)` are populated.
 * @post `st->consumed` advanced past the length list.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
static ra8_err_t s_read_bd_lengths(ra8_rar5_state_t* st, uint8_t* out)
{
  uint32_t i = 0U;
  while (i < (uint32_t)k_ra8_rar5_bc) { /* bound: i advances toward k_ra8_rar5_bc */
    const uint32_t len = s_get(st, 4U);
    if (len != (uint32_t)k_r5_len_escape) {
      out[i] = (uint8_t)len;
      i += 1U;
      continue;
    }
    const uint32_t zc = s_get(st, 4U);
    if (zc == 0U) {
      out[i] = (uint8_t)k_r5_len_escape;
      i += 1U;
    } else {
      i = s_fill_zeros(out, i, zc + (uint32_t)k_r5_zeros_extra, (uint32_t)k_ra8_rar5_bc);
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Append one run (copy-previous or zero) to the length table.
 * @details Reads the run length for the continuation code @p num and repeats either
 *          the previous bit length (codes 16/17) or zero (codes 18/19).
 * @param[in,out] st  Decoder state (non-NULL).
 * @param[in,out] tbl Length table being filled (non-NULL).
 * @param[in,out] idx Current fill index; advanced past the run (non-NULL).
 * @param[in]     num Continuation code (16..19).
 * @return ra8_err_t status.
 * @retval k_ra8_ok                    The run was appended.
 * @retval k_ra8_err_validation_failed A copy-previous run with no previous entry.
 * @pre @p tbl holds ::k_ra8_rar5_huff_total writable bytes.
 * @pre `*idx <= k_ra8_rar5_huff_total`.
 * @post `*idx` advanced by the (clamped) run length.
 * @post A copy run repeats `tbl[*idx-1]`; a zero run writes zeros.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
static ra8_err_t s_apply_run(ra8_rar5_state_t* st, uint8_t* tbl, uint32_t* idx, uint32_t num)
{
  const bool is_long =
    (num == (uint32_t)k_r5_tbl_copy_long) || (num == (uint32_t)k_r5_tbl_zero_long);
  const bool is_zero =
    (num == (uint32_t)k_r5_tbl_zero_short) || (num == (uint32_t)k_r5_tbl_zero_long);
  const uint32_t count = is_long
                           ? (s_get(st, (uint32_t)k_r5_run_long_bits) + (uint32_t)k_r5_run_long_add)
                           : (s_get(st, 3U) + 3U);
  if (!is_zero && *idx == 0U) {
    return k_ra8_err_validation_failed;
  }
  const uint8_t prev = (*idx > 0U) ? tbl[*idx - 1U] : 0U;
  const uint8_t fill = is_zero ? 0U : prev;
  uint32_t      i    = *idx;
  for (uint32_t c = 0U; c < count && i < (uint32_t)k_ra8_rar5_huff_total; ++c) { /* bound: count */
    tbl[i] = fill;
    i += 1U;
  }
  *idx = i;
  return k_ra8_ok;
}

/**
 * @brief Decode the combined length table for the four LZ alphabets.
 * @details Reads ::k_ra8_rar5_huff_total bit lengths through the BD table: codes
 *          0-15 are literal lengths, 16-19 are copy-previous / zero runs.
 * @param[in,out] st  Decoder state (non-NULL, ::ra8_rar5_state_t::bd built).
 * @param[out]    tbl Receives the combined length table (non-NULL).
 * @return ra8_err_t status.
 * @retval k_ra8_ok                    The full table was decoded.
 * @retval k_ra8_err_validation_failed A run code without a previous entry.
 * @pre @p st::bd was built by ::s_make_tables.
 * @pre @p tbl holds ::k_ra8_rar5_huff_total writable bytes.
 * @post `tbl[0 .. k_ra8_rar5_huff_total)` are populated.
 * @post `st->consumed` advanced past the table.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
static ra8_err_t s_read_full_table(ra8_rar5_state_t* st, uint8_t* tbl)
{
  uint32_t i = 0U;
  while (i < (uint32_t)k_ra8_rar5_huff_total) { /* bound: i advances each pass */
    const uint32_t num = s_decode_num(st, &st->bd);
    if (num < 16U) {
      tbl[i] = (uint8_t)num;
      i += 1U;
      continue;
    }
    const ra8_err_t e = s_apply_run(st, tbl, &i, num);
    if (e != k_ra8_ok) {
      return e;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Parse a table block: build the BD pre-table then the four LZ tables.
 * @details Reads the BD lengths, builds the BD decode table, decodes the combined
 *          length table, and splits it into the main / distance / low-distance /
 *          repeat-length decode tables.
 * @param[in,out] st Decoder state (non-NULL).
 * @return ra8_err_t status.
 * @retval k_ra8_ok                    All five tables were built.
 * @retval k_ra8_err_validation_failed A malformed length table.
 * @pre @p st is a bound decoder state at a table block.
 * @pre @p st scratch tables are writable.
 * @post On k_ra8_ok, `st->tables_ready` is true.
 * @post The four LZ decode tables are usable by ::s_decode_num.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
static ra8_err_t s_read_tables(ra8_rar5_state_t* st)
{
  uint8_t   bdlen[k_ra8_rar5_bc] = {};
  ra8_err_t e                    = s_read_bd_lengths(st, bdlen);
  if (e != k_ra8_ok) {
    return e;
  }
  s_make_tables(&st->bd, bdlen, (uint16_t)k_ra8_rar5_bc);
  uint8_t tbl[k_ra8_rar5_huff_total] = {};
  e                                  = s_read_full_table(st, tbl);
  if (e != k_ra8_ok) {
    return e;
  }
  s_make_tables(&st->ld, &tbl[0], (uint16_t)k_ra8_rar5_nc);
  s_make_tables(&st->dd, &tbl[k_ra8_rar5_nc], (uint16_t)k_ra8_rar5_dc);
  s_make_tables(&st->ldd, &tbl[k_ra8_rar5_nc + k_ra8_rar5_dc], (uint16_t)k_ra8_rar5_ldc);
  s_make_tables(&st->rd,
                &tbl[k_ra8_rar5_nc + k_ra8_rar5_dc + k_ra8_rar5_ldc],
                (uint16_t)k_ra8_rar5_rc);
  st->tables_ready = true;
  return k_ra8_ok;
}

/* ---- LZ token decode ---------------------------------------------------- */

/**
 * @brief Convert an LZ length slot into a match length, reading any extra bits.
 * @details Slots 0-7 map to lengths 2-9 directly; higher slots add
 *          `(4 | slot&3) << (slot/4 - 1)` plus that many extra bits.
 * @param[in,out] st   Decoder state (non-NULL).
 * @param[in]     slot Length slot value.
 * @return The decoded match length.
 * @retval 2 For length slot 0.
 * @pre @p st is a bound decoder state.
 * @pre @p slot is a valid length slot.
 * @post `st->consumed` advanced by the extra-bit count.
 * @post The result is >= 2.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
static uint32_t s_slot_to_length(ra8_rar5_state_t* st, uint32_t slot)
{
  uint32_t length = 2U;
  if (slot < 8U) {
    return length + slot;
  }
  const uint32_t lbits = (slot / 4U) - 1U;
  length += (4U | (slot & 3U)) << lbits;
  length += s_get(st, lbits);
  return length;
}

/**
 * @brief Decode an LZ match distance from the distance / low-distance tables.
 * @details Slots 0-3 give a direct distance; higher slots reconstruct the top bits
 *          from the slot and the remaining bits from either extra bits or the
 *          low-distance table.
 * @param[in,out] st Decoder state (non-NULL).
 * @return The decoded 1-based match distance.
 * @retval 1 For distance slot 0.
 * @pre @p st is a bound decoder state with the DD/LDD tables built.
 * @pre @p st scratch tables are usable.
 * @post `st->consumed` advanced past the distance code and extras.
 * @post The result is >= 1.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
static uint64_t s_decode_distance(ra8_rar5_state_t* st)
{
  const uint32_t slot = s_decode_num(st, &st->dd);
  if (slot < 4U) {
    return (uint64_t)1U + slot;
  }
  const uint32_t dbits = (slot / 2U) - 1U;
  uint64_t       dist  = (uint64_t)1U + ((uint64_t)(2U | (slot & 1U)) << dbits);
  if (dbits < 4U) {
    return dist + s_get(st, dbits);
  }
  if (dbits > 4U) {
    dist += (uint64_t)s_get(st, dbits - 4U) << 4U;
  }
  dist += s_decode_num(st, &st->ldd);
  return dist;
}

/**
 * @brief Add the RAR5 distance-dependent bias to a match length.
 * @details Distances above 0x100 / 0x2000 / 0x40000 add 1 / 2 / 3 to the length.
 * @param[in] length Base match length.
 * @param[in] dist   Match distance.
 * @return The biased match length.
 * @retval length When @p dist <= 0x100.
 * @pre @p length is a decoded base length.
 * @pre @p dist is a decoded distance.
 * @post No state is modified (pure function).
 * @post The result is >= @p length.
 * @note Thread-safe: pure.
 * @since Version 0.1.0
 */
static uint32_t s_adjust_length(uint32_t length, uint64_t dist)
{
  if (dist > (uint64_t)k_r5_dist_th1) {
    length += 1U;
    if (dist > (uint64_t)k_r5_dist_th2) {
      length += 1U;
      if (dist > (uint64_t)k_r5_dist_th3) {
        length += 1U;
      }
    }
  }
  return length;
}

/**
 * @brief Push a fresh match distance onto the recent-distance ring.
 * @details Shifts the four remembered distances down by one and stores @p dist at the
 *          front, so a later remembered-distance symbol can reuse it.
 * @param[in,out] st   Decoder state (non-NULL).
 * @param[in]     dist Distance to remember.
 * @return Nothing.
 * @pre @p st is a bound decoder state.
 * @pre @p st::old_dist has ::k_ra8_rar5_old_dist entries.
 * @post `st->old_dist[0] == dist` and the older entries shift down.
 * @post The oldest remembered distance is discarded.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
static void s_push_dist(ra8_rar5_state_t* st, uint64_t dist)
{
  for (uint32_t i = (uint32_t)k_ra8_rar5_old_dist - 1U; i > 0U; --i) { /* bound: ring size */
    st->old_dist[i] = st->old_dist[i - 1U];
  }
  st->old_dist[0] = dist;
}

/**
 * @brief Copy an LZ match of @p length bytes at back-distance @p dist into @p out.
 * @details Byte-by-byte self-overlapping copy (so a short distance repeats), clamped
 *          to @p unp. A distance reaching before the member start (a solid archive)
 *          or of zero is rejected.
 * @param[in,out] out      Output/window buffer (non-NULL).
 * @param[in,out] out_pos  Current output length; advanced by the copy (non-NULL).
 * @param[in]     unp      Target unpacked length (copy clamp).
 * @param[in]     length   Match length in bytes.
 * @param[in]     dist     Back-distance in bytes.
 * @return Whether the match was valid and copied.
 * @retval true  The match copied (possibly clamped at @p unp).
 * @retval false @p dist is zero or reaches before the member start.
 * @pre @p out holds at least @p unp writable bytes.
 * @pre `*out_pos <= unp`.
 * @post On true, `*out_pos` advanced by up to @p length.
 * @post On false no byte is written.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
static bool s_copy_match(uint8_t* out, size_t* out_pos, size_t unp, uint32_t length, uint64_t dist)
{
  size_t pos = *out_pos;
  if (dist == 0U || dist > (uint64_t)pos) {
    return false;
  }
  const size_t back = (size_t)dist;
  for (uint32_t k = 0U; k < length && pos < unp; ++k) { /* bound: length, pos<unp */
    out[pos] = out[pos - back];
    pos += 1U;
  }
  *out_pos = pos;
  return true;
}

/* ---- in-stream data filters --------------------------------------------- */

/**
 * @brief Read one length/offset field of a RAR5 filter descriptor.
 * @details A 2-bit byte count selects 1..4 little-endian bytes of the value.
 * @param[in,out] st Decoder state (non-NULL).
 * @return The decoded field value.
 * @retval 0 When the encoded bytes are all zero.
 * @pre @p st is a bound decoder state at a filter descriptor.
 * @pre @p st has bits remaining or pads with zero.
 * @post `st->consumed` advanced past the field.
 * @post The result fits 32 bits.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
static uint32_t s_read_filter_data(ra8_rar5_state_t* st)
{
  const uint32_t bc   = s_get(st, 2U) + 1U;
  uint32_t       data = 0U;
  for (uint32_t i = 0U; i < bc; ++i) { /* bound: bc <= 4 */
    data |= s_get(st, (uint32_t)k_r5_byte_bits) << (i * (uint32_t)k_r5_byte_bits);
  }
  return data;
}

/**
 * @brief Read one filter descriptor and queue it for post-decode replay.
 * @details Records the transform's absolute output start (relative offset plus the
 *          current output position), its length, kind, and delta channel count.
 * @param[in,out] st      Decoder state (non-NULL).
 * @param[in]     out_pos Current output position (filter start base).
 * @return ra8_err_t status.
 * @retval k_ra8_ok                    The filter was queued.
 * @retval k_ra8_err_validation_failed The filter list is full or the kind is invalid.
 * @pre @p st is a bound decoder state.
 * @pre @p out_pos is the current output length.
 * @post On k_ra8_ok, `st->filter_count` incremented by one.
 * @post `st->consumed` advanced past the descriptor.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
static ra8_err_t s_read_filter(ra8_rar5_state_t* st, size_t out_pos)
{
  if (st->filter_count >= (uint16_t)k_ra8_rar5_max_filters) {
    return k_ra8_err_validation_failed;
  }
  const uint32_t rel  = s_read_filter_data(st);
  const uint32_t len  = s_read_filter_data(st);
  const uint32_t type = s_get(st, (uint32_t)k_r5_ftype_bits);
  if (type > (uint32_t)k_ra8_rar5_filter_arm) {
    return k_ra8_err_validation_failed;
  }
  uint32_t channels = 1U;
  if (type == (uint32_t)k_ra8_rar5_filter_delta) {
    channels = s_get(st, (uint32_t)k_r5_fchan_bits) + 1U;
  }
  ra8_rar5_filter_t* f = &st->filters[st->filter_count];
  f->start             = (uint64_t)out_pos + (uint64_t)rel;
  f->len               = len;
  f->type              = (uint8_t)type;
  f->channels          = (uint8_t)channels;
  st->filter_count += 1U;
  return k_ra8_ok;
}

/**
 * @brief Apply the per-channel byte-delta filter over @p d.
 * @details De-interleaves @p channels streams and runs a running byte-sum per
 *          channel. Bounded by ::k_ra8_rar5_delta_scratch; a longer range is left
 *          untouched (comic pages never carry a delta filter).
 * @param[in,out] st       Decoder state (delta scratch, non-NULL).
 * @param[in,out] d        Output range to transform (non-NULL).
 * @param[in]     len      Range length in bytes.
 * @param[in]     channels Delta channel count (1..32).
 * @return Nothing.
 * @pre @p d holds @p len writable bytes.
 * @pre @p st::delta holds ::k_ra8_rar5_delta_scratch bytes.
 * @post On a supported length, `d` holds the delta-decoded range.
 * @post On an over-long range or zero channels, `d` is unchanged.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
static void s_filter_delta(ra8_rar5_state_t* st, uint8_t* d, uint32_t len, uint32_t channels)
{
  if (len > (uint32_t)k_ra8_rar5_delta_scratch || channels == 0U) {
    return;
  }
  (void)memcpy(st->delta, d, (size_t)len);
  uint32_t dpos = 0U;
  for (uint32_t ch = 0U; ch < channels; ++ch) { /* bound: channels <= 32 */
    uint8_t prev = 0U;
    for (uint32_t i = ch; i < len; i += channels) { /* bound: i < len */
      prev = (uint8_t)(prev - st->delta[dpos]);
      dpos += 1U;
      d[i] = prev;
    }
  }
}

/**
 * @brief Test whether @p op is an x86 branch opcode this filter transforms.
 * @details CALL (0xE8) always; JMP (0xE9) only when @p e9 is set (E8E9 variant).
 * @param[in] op Instruction opcode byte.
 * @param[in] e9 Whether 0xE9 is also transformed.
 * @return Whether @p op is a transformed branch.
 * @retval true  @p op is CALL, or JMP with @p e9 set.
 * @retval false Any other opcode.
 * @pre @p op is the candidate opcode byte.
 * @pre @p e9 selects the E8E9 variant.
 * @post No state is modified (pure function).
 * @post Only 0xE8 / (0xE9 with @p e9) return true.
 * @note Thread-safe: pure.
 * @since Version 0.1.0
 */
static bool s_x86_is_op(uint8_t op, bool e9)
{
  if (op == (uint8_t)k_r5_x86_call) {
    return true;
  }
  return e9 && (op == (uint8_t)k_r5_x86_jmp);
}

/**
 * @brief Apply the x86 CALL/JMP relative-address filter over @p d.
 * @details For each transformed branch, subtracts the operand's stream position from
 *          the stored 32-bit operand -- the inverse of the writer's add.
 * @param[in,out] d       Output range to transform (non-NULL).
 * @param[in]     len     Range length in bytes.
 * @param[in]     filepos Absolute output offset of `d[0]`.
 * @param[in]     e9      Whether 0xE9 JMP is transformed too.
 * @return Nothing.
 * @pre @p d holds @p len writable bytes.
 * @pre @p len fits an x86 instruction (>= 5) to transform anything.
 * @post Each transformed operand is relative-decoded in place.
 * @post Non-branch bytes are unchanged.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
static void s_filter_x86(uint8_t* d, uint32_t len, uint64_t filepos, bool e9)
{
  if (len < (uint32_t)k_r5_x86_ilen) {
    return;
  }
  const uint32_t limit = len - (uint32_t)k_r5_x86_ilen;
  uint32_t       i     = 0U;
  while (i <= limit) { /* bound: i <= limit < len */
    if (s_x86_is_op(d[i], e9)) {
      const uint32_t off = i + 1U;
      const uint32_t pos = (uint32_t)(filepos + (uint64_t)off);
      s_wr_le32(&d[off], s_rd_le32(&d[off]) - pos);
      i += (uint32_t)k_r5_x86_ilen;
    } else {
      i += 1U;
    }
  }
}

/**
 * @brief Apply the ARM BL branch-offset filter over @p d.
 * @details For each 4-byte word whose top byte is the BL opcode, subtracts the word
 *          position from the 24-bit branch offset -- the inverse of the writer's add.
 * @param[in,out] d       Output range to transform (non-NULL).
 * @param[in]     len     Range length in bytes.
 * @param[in]     filepos Absolute output offset of `d[0]`.
 * @return Nothing.
 * @pre @p d holds @p len writable bytes.
 * @pre @p len >= 4 to transform anything.
 * @post Each BL offset is relative-decoded in place.
 * @post Non-BL words are unchanged.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
static void s_filter_arm(uint8_t* d, uint32_t len, uint64_t filepos)
{
  if (len < 4U) {
    return;
  }
  const uint32_t limit = len - 4U;
  uint32_t       i     = 0U;
  while (i <= limit) { /* bound: i steps by 4 to limit */
    if (d[i + 3U] == (uint8_t)k_r5_arm_bl) {
      uint32_t       v = (uint32_t)d[i] | ((uint32_t)d[i + 1U] << k_r5_byte_bits) |
                         ((uint32_t)d[i + 2U] << (k_r5_byte_bits * 2U));
      const uint32_t p = (uint32_t)((filepos + (uint64_t)i) >> 2U);
      v                = (v - p) & (uint32_t)k_r5_arm_off_mask;
      d[i]             = (uint8_t)(v & (uint32_t)k_r5_byte_mask);
      d[i + 1U]        = (uint8_t)((v >> k_r5_byte_bits) & (uint32_t)k_r5_byte_mask);
      d[i + 2U]        = (uint8_t)((v >> (k_r5_byte_bits * 2U)) & (uint32_t)k_r5_byte_mask);
    }
    i += 4U;
  }
}

/**
 * @brief Replay one queued filter over its (clamped) output range.
 * @details Clamps the filter's range to the decoded output, then dispatches to the
 *          delta / x86 / x86-e8e9 / ARM transform; a range starting past the output
 *          end is skipped.
 * @param[in,out] st  Decoder state (non-NULL).
 * @param[in,out] out Decoded output buffer (non-NULL).
 * @param[in]     unp Total unpacked length (range clamp).
 * @param[in]     f   Filter to apply (non-NULL).
 * @return Nothing.
 * @pre @p out holds @p unp writable bytes.
 * @pre @p f came from ::s_read_filter.
 * @post The output range is transformed in place, or skipped if out of range.
 * @post No byte outside `[f->start, unp)` is touched.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
static void
s_apply_one_filter(ra8_rar5_state_t* st, uint8_t* out, size_t unp, const ra8_rar5_filter_t* f)
{
  if (f->start >= (uint64_t)unp) {
    return;
  }
  const uint64_t avail = (uint64_t)unp - f->start;
  const uint32_t len   = ((uint64_t)f->len > avail) ? (uint32_t)avail : f->len;
  if (len == 0U) {
    return;
  }
  uint8_t* d = &out[(size_t)f->start];
  if (f->type == (uint8_t)k_ra8_rar5_filter_delta) {
    s_filter_delta(st, d, len, f->channels);
  } else if (f->type == (uint8_t)k_ra8_rar5_filter_e8) {
    s_filter_x86(d, len, f->start, false);
  } else if (f->type == (uint8_t)k_ra8_rar5_filter_e8e9) {
    s_filter_x86(d, len, f->start, true);
  } else {
    s_filter_arm(d, len, f->start);
  }
}

/**
 * @brief Replay every queued filter in stream order.
 * @details Iterates the recorded filter list front to back, applying each via
 *          ::s_apply_one_filter; a no-op when no filter was recorded (the comic case).
 * @param[in,out] st  Decoder state (non-NULL).
 * @param[in,out] out Decoded output buffer (non-NULL).
 * @param[in]     unp Total unpacked length.
 * @return Nothing.
 * @pre @p out holds @p unp writable bytes.
 * @pre @p st::filter_count <= ::k_ra8_rar5_max_filters.
 * @post Each queued filter has been applied.
 * @post No byte past @p unp is written.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
static void s_apply_filters(ra8_rar5_state_t* st, uint8_t* out, size_t unp)
{
  for (uint16_t f = 0U; f < st->filter_count; ++f) { /* bound: filter_count */
    s_apply_one_filter(st, out, unp, &st->filters[f]);
  }
}

/* ---- token dispatch + block/stream loops -------------------------------- */

/**
 * @brief Decode a length-slot LZ match (main symbol >= 262) into @p out.
 * @details Reads the length from the slot, the distance from the DD/LDD tables,
 *          applies the distance-length bias, remembers the distance, and copies the
 *          match into the output window.
 * @param[in,out] st      Decoder state (non-NULL).
 * @param[in]     slot    Main symbol (>= ::k_r5_sym_lenbase).
 * @param[in,out] out     Output/window buffer (non-NULL).
 * @param[in,out] out_pos Current output length; advanced (non-NULL).
 * @param[in]     unp     Target unpacked length.
 * @return ra8_err_t status.
 * @retval k_ra8_ok                    The match was copied.
 * @retval k_ra8_err_validation_failed An invalid back-distance.
 * @pre @p st has the LZ tables built.
 * @pre `*out_pos <= unp`.
 * @post On k_ra8_ok, `*out_pos` advanced and the recent-distance ring updated.
 * @post On error no output byte is written.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
static ra8_err_t
s_do_match(ra8_rar5_state_t* st, uint32_t slot, uint8_t* out, size_t* out_pos, size_t unp)
{
  uint32_t       length = s_slot_to_length(st, slot - (uint32_t)k_r5_sym_lenbase);
  const uint64_t dist   = s_decode_distance(st);
  length                = s_adjust_length(length, dist);
  s_push_dist(st, dist);
  st->last_length = length;
  if (!s_copy_match(out, out_pos, unp, length, dist)) {
    return k_ra8_err_validation_failed;
  }
  return k_ra8_ok;
}

/**
 * @brief Decode a remembered-distance LZ match (main symbols 258..261) into @p out.
 * @details Selects one of the four remembered distances, promotes it to most-recent,
 *          reads a repeat length from the RD table, and copies the match.
 * @param[in,out] st      Decoder state (non-NULL).
 * @param[in]     slot    Main symbol in 258..261.
 * @param[in,out] out     Output/window buffer (non-NULL).
 * @param[in,out] out_pos Current output length; advanced (non-NULL).
 * @param[in]     unp     Target unpacked length.
 * @return ra8_err_t status.
 * @retval k_ra8_ok                    The match was copied.
 * @retval k_ra8_err_validation_failed An invalid back-distance.
 * @pre @p st has the LZ tables built.
 * @pre `*out_pos <= unp`.
 * @post On k_ra8_ok, the chosen distance is promoted to most-recent.
 * @post On error no output byte is written.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
static ra8_err_t
s_do_repdist(ra8_rar5_state_t* st, uint32_t slot, uint8_t* out, size_t* out_pos, size_t unp)
{
  const uint32_t idx  = slot - (uint32_t)k_r5_sym_repdist0;
  const uint64_t dist = st->old_dist[idx];
  for (uint32_t i = idx; i > 0U; --i) { /* bound: idx <= 3 */
    st->old_dist[i] = st->old_dist[i - 1U];
  }
  st->old_dist[0]        = dist;
  const uint32_t lenslot = s_decode_num(st, &st->rd);
  const uint32_t length  = s_slot_to_length(st, lenslot);
  st->last_length        = length;
  if (!s_copy_match(out, out_pos, unp, length, dist)) {
    return k_ra8_err_validation_failed;
  }
  return k_ra8_ok;
}

/**
 * @brief Replay the last match (main symbol 257) into @p out.
 * @details Copies the most-recent match's length at the most-recent distance; a no-op
 *          when no match has been seen yet (last length is zero).
 * @param[in,out] st      Decoder state (non-NULL).
 * @param[in,out] out     Output/window buffer (non-NULL).
 * @param[in,out] out_pos Current output length; advanced (non-NULL).
 * @param[in]     unp     Target unpacked length.
 * @return ra8_err_t status.
 * @retval k_ra8_ok                    The repeat was copied (or was a no-op).
 * @retval k_ra8_err_validation_failed An invalid back-distance.
 * @pre @p st is a bound decoder state.
 * @pre `*out_pos <= unp`.
 * @post On k_ra8_ok, `*out_pos` advanced by the last length (0 if none yet).
 * @post On error no output byte is written.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
static ra8_err_t s_do_replast(ra8_rar5_state_t* st, uint8_t* out, size_t* out_pos, size_t unp)
{
  if (st->last_length == 0U) {
    return k_ra8_ok;
  }
  if (!s_copy_match(out, out_pos, unp, st->last_length, st->old_dist[0])) {
    return k_ra8_err_validation_failed;
  }
  return k_ra8_ok;
}

/**
 * @brief Decode one LZ token: literal, match, repeat-match, or filter.
 * @details Classifies the main symbol: below 256 is a literal byte, 256 a filter,
 *          257 the last-match repeat, 258-261 a remembered-distance match, 262+ a
 *          length-slot match.
 * @param[in,out] st      Decoder state (non-NULL).
 * @param[in,out] out     Output/window buffer (non-NULL).
 * @param[in,out] out_pos Current output length; advanced (non-NULL).
 * @param[in]     unp     Target unpacked length.
 * @return ra8_err_t status.
 * @retval k_ra8_ok                    One token was decoded.
 * @retval k_ra8_err_validation_failed A malformed match or filter.
 * @pre @p st has the LZ tables built.
 * @pre `*out_pos < unp`.
 * @post On k_ra8_ok, output/filters advanced by exactly one token.
 * @post On error no partial token corrupts prior output.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
static ra8_err_t s_decode_token(ra8_rar5_state_t* st, uint8_t* out, size_t* out_pos, size_t unp)
{
  const uint32_t slot = s_decode_num(st, &st->ld);
  if (slot < (uint32_t)k_r5_sym_filter) {
    out[*out_pos] = (uint8_t)slot;
    *out_pos += 1U;
    return k_ra8_ok;
  }
  if (slot == (uint32_t)k_r5_sym_filter) {
    return s_read_filter(st, *out_pos);
  }
  if (slot == (uint32_t)k_r5_sym_replast) {
    return s_do_replast(st, out, out_pos, unp);
  }
  if (slot < (uint32_t)k_r5_sym_lenbase) {
    return s_do_repdist(st, slot, out, out_pos, unp);
  }
  return s_do_match(st, slot, out, out_pos, unp);
}

/**
 * @brief Open the next compressed block: header, tables, and its bit-end.
 * @details Reads and validates the block header, rejects a zero-size block, computes
 *          the block's end bit position, and (re)builds the Huffman tables when the
 *          block carries them -- else requires that a prior block already did.
 * @param[in,out] st      Decoder state (non-NULL).
 * @param[out]    end_bit Receives the block's absolute end bit position (non-NULL).
 * @param[out]    last    Receives whether this is the last block (non-NULL).
 * @return ra8_err_t status.
 * @retval k_ra8_ok                    Block opened and tables ready.
 * @retval k_ra8_err_validation_failed A bad header, zero-size block, or no tables.
 * @pre @p st is a bound decoder state.
 * @pre @p end_bit and @p last are writable.
 * @post On k_ra8_ok, `*end_bit` is the block's end and tables are usable.
 * @post On error the decode is abandoned.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
static ra8_err_t s_open_block(ra8_rar5_state_t* st, uint64_t* end_bit, bool* last)
{
  r5_block_t      blk = {};
  const ra8_err_t e   = s_read_block_header(st, &blk);
  if (e != k_ra8_ok) {
    return e;
  }
  if (blk.size == 0U) {
    return k_ra8_err_validation_failed;
  }
  const uint64_t start = st->consumed;
  *end_bit = start + ((blk.size - 1U) * (uint64_t)k_r5_byte_bits) + (uint64_t)blk.last_bits;
  *last    = blk.last;
  if (blk.tables) {
    return s_read_tables(st);
  }
  if (!st->tables_ready) {
    return k_ra8_err_validation_failed;
  }
  return k_ra8_ok;
}

/**
 * @brief Drive the block + token loops until @p unp bytes are produced.
 * @details Reads a new block header whenever the previous block's bits are exhausted,
 *          decoding tokens into @p out until the output is complete, the stream ends,
 *          or the packed member (plus a small pad) is fully consumed.
 * @param[in,out] st      Decoder state (non-NULL).
 * @param[in,out] out     Output/window buffer (non-NULL).
 * @param[in]     out_cap Capacity of @p out in bytes.
 * @param[in]     unp     Target unpacked length (<= @p out_cap).
 * @return ra8_err_t status.
 * @retval k_ra8_ok                    Exactly @p unp bytes were produced.
 * @retval k_ra8_err_validation_failed Truncated / malformed stream, short output.
 * @pre @p out holds @p out_cap writable bytes, @p unp <= @p out_cap.
 * @pre @p st is a freshly reset decoder state.
 * @post On k_ra8_ok, `out[0..unp)` holds the decoded bytes.
 * @post On error the output is incomplete.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
static ra8_err_t s_decode_stream(ra8_rar5_state_t* st, uint8_t* out, size_t out_cap, size_t unp)
{
  (void)out_cap;
  size_t         out_pos    = 0U;
  bool           have_block = false;
  bool           last       = false;
  uint64_t       block_end  = 0U;
  const uint64_t cap_bits = (st->packlen * (uint64_t)k_r5_byte_bits) + (uint64_t)k_r5_max_pad_bits;
  while (out_pos < unp && st->consumed <= cap_bits) { /* bound: consumed strictly rises */
    if (!have_block || st->consumed >= block_end) {
      if (have_block && last) {
        break;
      }
      const ra8_err_t e = s_open_block(st, &block_end, &last);
      if (e != k_ra8_ok) {
        return e;
      }
      have_block = true;
      continue;
    }
    const ra8_err_t e = s_decode_token(st, out, &out_pos, unp);
    if (e != k_ra8_ok) {
      return e;
    }
  }
  if (out_pos != unp) {
    return k_ra8_err_validation_failed;
  }
  return k_ra8_ok;
}

/**
 * @brief Validate the non-output arguments of ::ra8_rar5_decompress.
 * @details Null-checks the archive, output, and scratch pointers, then rejects a
 *          non-RAR5 archive, an output buffer smaller than the unpacked size, and a
 *          zero packed size. Split out so the entry point stays within the
 *          function-size budget.
 * @param[in] rar       Archive to decode from.
 * @param[in] out       Output buffer.
 * @param[in] st        Decoder scratch.
 * @param[in] out_cap   Output-buffer capacity in bytes.
 * @param[in] unp_size  Expected unpacked length.
 * @param[in] pack_size Packed member length.
 * @return ra8_err_t status.
 * @retval k_ra8_ok                Arguments are usable.
 * @retval k_ra8_err_null_ptr      @p rar, @p out, or @p st was NULL.
 * @retval k_ra8_err_invalid_state @p rar is not a RAR5 archive.
 * @retval k_ra8_err_no_mem        @p out_cap is smaller than @p unp_size.
 * @retval k_ra8_err_validation_failed @p pack_size is zero.
 * @pre @p got was already validated and cleared by the caller.
 * @pre The caller forwards its arguments unchanged.
 * @post On k_ra8_ok every pointer is safe to dereference.
 * @post No state is modified.
 * @note Not thread-safe (reads @p rar fields).
 * @since Version 0.1.0
 */
static ra8_err_t s_decompress_check(const ra8_rar_t*        rar,
                                    const uint8_t*          out,
                                    const ra8_rar5_state_t* st,
                                    size_t                  out_cap,
                                    uint64_t                unp_size,
                                    uint64_t                pack_size)
{
  RA8_CHECK_NULL_PTR(rar, s_tag_rar5, "decompress: null rar");
  RA8_CHECK_NULL_PTR(out, s_tag_rar5, "decompress: null out");
  RA8_CHECK_NULL_PTR(st, s_tag_rar5, "decompress: null st");
  if (rar->version != k_ra8_rar_ver_5) {
    return k_ra8_err_invalid_state;
  }
  if (unp_size > (uint64_t)out_cap) {
    return k_ra8_err_no_mem;
  }
  if (pack_size == 0U) {
    return k_ra8_err_validation_failed;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_rar5_decompress(const ra8_rar_t*  rar,
                              uint64_t          data_off,
                              uint64_t          pack_size,
                              uint8_t*          out,
                              size_t            out_cap,
                              uint64_t          unp_size,
                              ra8_rar5_state_t* st,
                              size_t*           got)
{
  RA8_CHECK_NULL_PTR(got, s_tag_rar5, "decompress: null got");
  *got                   = 0U;
  const ra8_err_t vcheck = s_decompress_check(rar, out, st, out_cap, unp_size, pack_size);
  if (vcheck != k_ra8_ok) {
    return vcheck;
  }
  *st         = (ra8_rar5_state_t){};
  st->rar     = rar;
  st->base    = data_off;
  st->packlen = pack_size;
  if (unp_size == 0U) {
    return k_ra8_ok;
  }
  const ra8_err_t e = s_decode_stream(st, out, out_cap, (size_t)unp_size);
  if (e != k_ra8_ok) {
    return e;
  }
  s_apply_filters(st, out, (size_t)unp_size);
  *got = (size_t)unp_size;
  return k_ra8_ok;
}
