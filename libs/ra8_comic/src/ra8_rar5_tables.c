/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_rar5_tables.c
 * @brief RAR 5.0 entropy front-end: bit reader, canonical Huffman, block tables.
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * The lower half of the clean-room RAR 5.0 decompressor (paired with the LZ
 * driver in `ra8_rar5.c`). Three layers, bottom-up:
 * - Bit reader: ::s_fetch_byte / ::s_ensure / ::s_peek / ::s_drop /
 *   ::ra8_rar5_get / ::s_align pull packed bytes through the archive
 *   ::ra8_rar_read_fn on demand, serving an MSB-first bit stream.
 * - Huffman: ::s_make_tables builds a canonical decode table from a bit-length
 *   vector; ::ra8_rar5_decode_num reads one symbol.
 * - Container: ::ra8_rar5_read_block_header validates one compressed-block header
 *   and ::ra8_rar5_read_tables parses the four LZ decode tables that drive it.
 *
 * The driver in `ra8_rar5.c` calls the four ::ra8_rar5_internal.h cross-TU entry
 * points here; nothing in this unit calls back into the driver.
 *
 * @since Version 0.1.0
 */
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_rar5.h"
#include "ra8_rar5_internal.h"

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
RA8_INTERNAL
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
RA8_INTERNAL
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
RA8_INTERNAL
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
RA8_INTERNAL
static void s_drop(ra8_rar5_state_t* st, uint32_t n)
{
  st->nbits -= n;
  st->consumed += (uint64_t)n;
  const uint64_t mask = (st->nbits >= (uint32_t)k_r5_acc_bits)
                          ? ~(uint64_t)0U
                          : (((uint64_t)1U << st->nbits) - (uint64_t)1U);
  st->acc &= mask;
}

/** @brief Implementation of `ra8_rar5_get()` -- ::s_peek then ::s_drop of @p n bits. */
uint32_t ra8_rar5_get(ra8_rar5_state_t* st, uint32_t n)
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
RA8_INTERNAL
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
RA8_INTERNAL
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
 * @post Every slot index read by ::ra8_rar5_decode_num is initialised.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
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

/** @brief Implementation of `ra8_rar5_decode_num()` -- limit-compare canonical decode. */
uint32_t ra8_rar5_decode_num(ra8_rar5_state_t* st, const ra8_rar5_dtab_t* d)
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
RA8_INTERNAL
static uint8_t s_checksum(uint32_t flags, uint64_t blocksize)
{
  uint32_t x = (uint32_t)k_r5_hdr_chk_seed ^ flags;
  x ^= (uint32_t)(blocksize & (uint64_t)k_r5_byte_mask);
  x ^= (uint32_t)((blocksize >> k_r5_byte_bits) & (uint64_t)k_r5_byte_mask);
  x ^= (uint32_t)((blocksize >> (k_r5_byte_bits * 2U)) & (uint64_t)k_r5_byte_mask);
  return (uint8_t)(x & (uint32_t)k_r5_byte_mask);
}

/** @brief Implementation of `ra8_rar5_read_block_header()` -- align, flags, size, checksum. */
ra8_err_t ra8_rar5_read_block_header(ra8_rar5_state_t* st, r5_block_t* b)
{
  s_align(st);
  const uint32_t flags = ra8_rar5_get(st, (uint32_t)k_r5_byte_bits);
  const uint32_t bytecount =
    ((flags >> (uint32_t)k_r5_bf_bcount_shift) & (uint32_t)k_r5_bf_bcount_mask) + 1U;
  uint64_t bsz = 0U;
  for (uint32_t i = 0U; i < bytecount; ++i) { /* bound: bytecount <= 4 */
    bsz |= (uint64_t)ra8_rar5_get(st, (uint32_t)k_r5_byte_bits) << (i * (uint32_t)k_r5_byte_bits);
  }
  const uint32_t saved = ra8_rar5_get(st, (uint32_t)k_r5_byte_bits);
  if ((uint32_t)s_checksum(flags, bsz) != saved) {
    return k_ra8_err_validation_failed;
  }
  b->size      = bsz;
  b->last_bits = (flags & (uint32_t)k_r5_bf_bitsize_mask) + 1U;
  b->tables    = (flags & (uint32_t)k_r5_bf_tables) != 0U;
  b->last      = (flags & (uint32_t)k_r5_bf_last) != 0U;
  return k_ra8_ok;
}

/** @brief Implementation of `ra8_rar5_fill_zeros()` -- bounded zero-length append. */
uint32_t ra8_rar5_fill_zeros(uint8_t* out, uint32_t start, uint32_t count, uint32_t max)
{
  uint32_t i = start;
  for (uint32_t c = 0U; (c < count) && (i < max); ++c) { /* bound: count, i<max */
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
RA8_INTERNAL
static ra8_err_t s_read_bd_lengths(ra8_rar5_state_t* st, uint8_t* out)
{
  uint32_t i = 0U;
  while (i < (uint32_t)k_ra8_rar5_bc) { /* bound: i advances toward k_ra8_rar5_bc */
    const uint32_t len = ra8_rar5_get(st, 4U);
    if (len != (uint32_t)k_r5_len_escape) {
      out[i] = (uint8_t)len;
      i += 1U;
      continue;
    }
    const uint32_t zc = ra8_rar5_get(st, 4U);
    if (zc == 0U) {
      out[i] = (uint8_t)k_r5_len_escape;
      i += 1U;
    } else {
      i = ra8_rar5_fill_zeros(out, i, zc + (uint32_t)k_r5_zeros_extra, (uint32_t)k_ra8_rar5_bc);
    }
  }
  return k_ra8_ok;
}

/** @brief Implementation of `ra8_rar5_apply_run()` -- copy-previous / zero run append. */
ra8_err_t ra8_rar5_apply_run(ra8_rar5_state_t* st, uint8_t* tbl, uint32_t* idx, uint32_t num)
{
  const bool is_long =
    (num == (uint32_t)k_r5_tbl_copy_long) || (num == (uint32_t)k_r5_tbl_zero_long);
  const bool is_zero =
    (num == (uint32_t)k_r5_tbl_zero_short) || (num == (uint32_t)k_r5_tbl_zero_long);
  const uint32_t count =
    is_long ? (ra8_rar5_get(st, (uint32_t)k_r5_run_long_bits) + (uint32_t)k_r5_run_long_add)
            : (ra8_rar5_get(st, 3U) + 3U);
  if ((!is_zero) && (*idx == 0U)) {
    return k_ra8_err_validation_failed;
  }
  const uint8_t prev = (*idx > 0U) ? tbl[*idx - 1U] : 0U;
  const uint8_t fill = is_zero ? 0U : prev;
  uint32_t      i    = *idx;
  for (uint32_t c = 0U; (c < count) && (i < (uint32_t)k_ra8_rar5_huff_total);
       ++c) { /* bound: count */
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
RA8_INTERNAL
static ra8_err_t s_read_full_table(ra8_rar5_state_t* st, uint8_t* tbl)
{
  uint32_t i = 0U;
  while (i < (uint32_t)k_ra8_rar5_huff_total) { /* bound: i advances each pass */
    const uint32_t num = ra8_rar5_decode_num(st, &st->bd);
    if (num < 16U) {
      tbl[i] = (uint8_t)num;
      i += 1U;
      continue;
    }
    const ra8_err_t e = ra8_rar5_apply_run(st, tbl, &i, num);
    if (e != k_ra8_ok) {
      return e;
    }
  }
  return k_ra8_ok;
}

/** @brief Implementation of `ra8_rar5_read_tables()` -- BD pre-table then the four LZ tables. */
ra8_err_t ra8_rar5_read_tables(ra8_rar5_state_t* st)
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
