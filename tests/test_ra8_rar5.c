/**
 * @file test_ra8_rar5.c
 * @brief Tests for the clean-room RAR5 decompressor (ra8_rar5) and the CBR
 *        compressed-member decode path.
 *
 * @details
 * There is no free tool that *writes* a RAR-compressed stream, so this suite ships a
 * small, spec-conformant RAR5 "method 50" writer: it emits one compressed block with
 * uniform-length canonical Huffman tables (so a symbol's code is its own index) and a
 * literal/match/repeat/filter token program, then wraps it in the block header the
 * decoder validates. The decoder is exercised two ways:
 *
 *   1. directly through `ra8_rar5_decompress` -- a round-trip oracle over literals,
 *      length/distance matches (every extra-bit leg), remembered-distance and
 *      last-match repeats, the four data filters, and the malformed-input guards, and
 *   2. through the `ra8_comic` facade -- a `.cbr` whose page is RAR5-compressed opens,
 *      decodes to the *same bytes* as the equivalent STORE page (the CBZ-parity
 *      acceptance for #235), while a RAR4-compressed page stays unsupported.
 *
 * The writer is first-party test scaffolding; it vendors no `unrar` code (that
 * license restriction is on RARLAB's decompressor, which this codec does not use).
 * Tests are magic-number exempt, so byte offsets and bit widths appear as literals.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "comic_fixture.h"
#include "ra8_comic.h"
#include "ra8_err.h"
#include "ra8_rar.h"
#include "ra8_rar5.h"
#include "ra8_rar5_internal.h"
#include "unity_minimal.h"

/** @brief Working buffers sized for the small crafted streams. */
enum : uint32_t {
  k_pk_cap  = 8192U,  /**< Packed-stream build buffer. */
  k_out_cap = 8192U,  /**< Decoded-output buffer.      */
  k_arc_cap = 65536U, /**< Whole-archive build buffer. */
};

/** @brief One RAR5 decoder scratch pool (kept off the stack: several KB). */
static ra8_rar5_state_t s_state;

/* ---- streaming read backing over a flat buffer -------------------------- */

/** @brief A flat byte buffer served through a ra8_rar_read_fn. */
typedef struct {
  const uint8_t* data; /**< Buffer bytes.  */
  size_t         len;  /**< Buffer length. */
} buf_src_t;

/** @brief Seek+read callback over a ::buf_src_t. */
static size_t buf_read(void* ctx, uint64_t off, void* dst, size_t len)
{
  const buf_src_t* s = (const buf_src_t*)ctx;
  if (off >= (uint64_t)s->len) {
    return 0U;
  }
  const uint64_t avail = (uint64_t)s->len - off;
  const size_t   n     = (len > (size_t)avail) ? (size_t)avail : len;
  memcpy(dst, &s->data[off], n);
  return n;
}

/* ---- MSB-first bit writer ----------------------------------------------- */

/** @brief Growing MSB-first bit writer into a fixed buffer. */
typedef struct {
  uint8_t* buf;    /**< Destination bytes.                      */
  size_t   cap;    /**< Capacity in bytes.                      */
  size_t   byte;   /**< Current byte index.                     */
  uint32_t bitpos; /**< Next bit in the current byte (0 = MSB). */
} bitw_t;

/** @brief Append @p n bits of @p v (MSB-first) to the writer. */
static void bw_put(bitw_t* w, uint32_t v, uint32_t n)
{
  for (uint32_t i = n; i > 0U; --i) {
    const uint32_t bit = (v >> (i - 1U)) & 1U;
    if (w->byte < w->cap) {
      if (bit != 0U) {
        w->buf[w->byte] |= (uint8_t)(1U << (7U - w->bitpos));
      }
    }
    w->bitpos++;
    if (w->bitpos == 8U) {
      w->bitpos = 0U;
      w->byte++;
    }
  }
}

/** @brief Total bytes written (rounding up a partial last byte). */
static size_t bw_bytes(const bitw_t* w)
{
  return (w->bitpos == 0U) ? w->byte : (w->byte + 1U);
}

/** @brief Valid bits in the last written byte (1..8). */
static uint32_t bw_lastbits(const bitw_t* w)
{
  return (w->bitpos == 0U) ? 8U : w->bitpos;
}

/* ---- RAR5 token encoder (matches the decoder's grammar) ----------------- */

/*
 * Table shape: main (LD) symbols 0..271 length 9; DD 0..63 length 6; LDD 0..15
 * length 4; RD 0..43 length 6. Uniform lengths make a symbol's canonical code its
 * own index, so `bw_put(sym, L)` emits the code for `sym`.
 */

/** @brief Emit the 430 combined lengths (each as a 5-bit BD symbol). */
static void enc_table_body(bitw_t* w)
{
  /* LD: 272 nines then 34 zeros. */
  for (uint32_t i = 0U; i < 272U; ++i) {
    bw_put(w, 9U, 5U);
  }
  for (uint32_t i = 0U; i < 34U; ++i) {
    bw_put(w, 0U, 5U);
  }
  for (uint32_t i = 0U; i < 64U; ++i) { /* DD: 64 sixes */
    bw_put(w, 6U, 5U);
  }
  for (uint32_t i = 0U; i < 16U; ++i) { /* LDD: 16 fours */
    bw_put(w, 4U, 5U);
  }
  for (uint32_t i = 0U; i < 44U; ++i) { /* RD: 44 sixes */
    bw_put(w, 6U, 5U);
  }
}

/** @brief Emit the BD pre-table lengths (all 5) then the 430 combined lengths. */
static void enc_tables(bitw_t* w)
{
  for (uint32_t i = 0U; i < 20U; ++i) { /* BD: 20 lengths of 5 bits, all value 5 */
    bw_put(w, 5U, 4U);
  }
  enc_table_body(w);
}

/**
 * @brief Emit tables whose BD length list zeroes symbols 10..14 via an escape run.
 * @details The BD symbols the table body actually uses (0, 4, 6, 9) all stay length
 *          5 and keep their index-as-code property, so decode is unchanged -- but the
 *          15-escape zero run exercises the decoder's zero-fill leg.
 */
static void enc_tables_bdzero(bitw_t* w)
{
  for (uint32_t i = 0U; i < 10U; ++i) { /* BD symbols 0..9 length 5 */
    bw_put(w, 5U, 4U);
  }
  bw_put(w, 15U, 4U);                  /* escape                                 */
  bw_put(w, 3U, 4U);                   /* zero count 3 -> 3+2 = 5 zeros (10..14) */
  for (uint32_t i = 0U; i < 5U; ++i) { /* BD symbols 15..19 length 5             */
    bw_put(w, 5U, 4U);
  }
  enc_table_body(w);
}

/** @brief Emit one literal byte + record it in the expected output. */
static void enc_lit(bitw_t* w, uint8_t* exp, size_t* elen, uint8_t b)
{
  bw_put(w, b, 9U);
  exp[*elen] = b;
  *elen += 1U;
}

/** @brief Copy @p len bytes from `-dist` into the expected output. */
static void exp_copy(uint8_t* exp, size_t* elen, uint32_t len, uint32_t dist)
{
  for (uint32_t k = 0U; k < len; ++k) {
    exp[*elen] = exp[*elen - dist];
    *elen += 1U;
  }
}

/** @brief Emit a match with no extra bits (lenslot<8, distslot<4). */
static void enc_match(bitw_t* w, uint8_t* exp, size_t* elen, uint32_t lenslot, uint32_t distslot)
{
  bw_put(w, 262U + lenslot, 9U);
  bw_put(w, distslot, 6U);
  exp_copy(exp, elen, 2U + lenslot, 1U + distslot);
}

/** @brief Emit lenslot 8 (1 length-extra bit), distslot<4. */
static void
enc_match_lenx(bitw_t* w, uint8_t* exp, size_t* elen, uint32_t len_extra, uint32_t distslot)
{
  bw_put(w, 270U, 9U); /* length slot 8 */
  bw_put(w, len_extra, 1U);
  bw_put(w, distslot, 6U);
  exp_copy(exp, elen, 10U + len_extra, 1U + distslot);
}

/** @brief Emit distslot 4 (1 distance-extra bit), lenslot<8. */
static void
enc_match_distx(bitw_t* w, uint8_t* exp, size_t* elen, uint32_t lenslot, uint32_t dist_extra)
{
  bw_put(w, 262U + lenslot, 9U);
  bw_put(w, 4U, 6U); /* distance slot 4 -> dbits 1 */
  bw_put(w, dist_extra, 1U);
  exp_copy(exp, elen, 2U + lenslot, 5U + dist_extra);
}

/** @brief Emit distslot 10 (dbits==4, low-distance symbol), lenslot<4. */
static void
enc_match_lowdist(bitw_t* w, uint8_t* exp, size_t* elen, uint32_t lenslot, uint32_t lowdist)
{
  bw_put(w, 262U + lenslot, 9U);
  bw_put(w, 10U, 6U);     /* distance slot 10 -> dbits 4 */
  bw_put(w, lowdist, 4U); /* low-distance table symbol   */
  exp_copy(exp, elen, 2U + lenslot, 33U + lowdist);
}

/** @brief Emit distslot 12 (dbits>4: hi extra + low-distance symbol). */
static void enc_match_hilow(bitw_t*  w,
                            uint8_t* exp,
                            size_t*  elen,
                            uint32_t lenslot,
                            uint32_t hi,
                            uint32_t lowdist)
{
  bw_put(w, 262U + lenslot, 9U);
  bw_put(w, 12U, 6U); /* distance slot 12 -> dbits 5 */
  bw_put(w, hi, 1U);  /* dbits-4 == 1 extra bit      */
  bw_put(w, lowdist, 4U);
  exp_copy(exp, elen, 2U + lenslot, 65U + (hi << 4U) + lowdist);
}

/** @brief Emit distslot 16 (large distance > 0x100, exercises length bias). */
static void enc_match_big(bitw_t* w, uint8_t* exp, size_t* elen, uint32_t lenslot)
{
  bw_put(w, 262U + lenslot, 9U);
  bw_put(w, 16U, 6U); /* distance slot 16 -> dbits 7 */
  bw_put(w, 0U, 3U);  /* dbits-4 == 3 extra bits = 0 */
  bw_put(w, 0U, 4U);  /* low-distance symbol 0       */
  /* dist = 1 + (2<<7) = 257 (> 0x100) -> length += 1 */
  exp_copy(exp, elen, 2U + lenslot + 1U, 257U);
}

/** @brief Emit a remembered-distance match (idx 0) reusing @p dist. */
static void enc_repdist(bitw_t* w, uint8_t* exp, size_t* elen, uint32_t rd_lenslot, uint32_t dist)
{
  bw_put(w, 258U, 9U);       /* rep distance index 0 */
  bw_put(w, rd_lenslot, 6U); /* repeat-length slot   */
  exp_copy(exp, elen, 2U + rd_lenslot, dist);
}

/** @brief Emit a last-match repeat of @p len at @p dist. */
static void enc_replast(bitw_t* w, uint8_t* exp, size_t* elen, uint32_t len, uint32_t dist)
{
  bw_put(w, 257U, 9U);
  exp_copy(exp, elen, len, dist);
}

/** @brief Emit a filter descriptor (does not itself produce output). */
static void enc_filter(bitw_t* w, uint32_t type, uint32_t rel, uint32_t len, uint32_t chan)
{
  bw_put(w, 256U, 9U);
  bw_put(w, 0U, 2U); /* filter-data byte count 1 */
  bw_put(w, rel & 0xFFU, 8U);
  bw_put(w, 0U, 2U);
  bw_put(w, len & 0xFFU, 8U);
  bw_put(w, type, 3U);
  if (type == 0U) {
    bw_put(w, chan - 1U, 5U);
  }
}

/** @brief Wrap a completed body writer into a packed block; return packed length. */
static size_t enc_finish(const bitw_t* body, uint8_t* pk)
{
  const size_t   bsz  = bw_bytes(body);
  const uint32_t bits = bw_lastbits(body);
  const uint32_t bcnt = (bsz < 256U) ? 1U : 2U;
  const uint8_t  flags =
    (uint8_t)(0xC0U | ((bcnt - 1U) << 3U) | (bits - 1U)); /* tables|last|bitsize */
  size_t p = 0U;
  pk[p]    = flags;
  p += 1U;
  pk[p] = (uint8_t)(bsz & 0xFFU);
  p += 1U;
  if (bcnt == 2U) {
    pk[p] = (uint8_t)((bsz >> 8U) & 0xFFU);
    p += 1U;
  }
  uint8_t chk = (uint8_t)(0x5AU ^ flags ^ (uint8_t)(bsz & 0xFFU));
  if (bcnt == 2U) {
    chk ^= (uint8_t)((bsz >> 8U) & 0xFFU);
  }
  pk[p] = chk;
  p += 1U;
  memcpy(&pk[p], body->buf, bsz);
  return p + bsz;
}

/** @brief Encode @p src as an all-literal RAR5 block; return packed length. */
static size_t enc_all_literal(const uint8_t* src, size_t srclen, uint8_t* pk, size_t pkcap)
{
  static uint8_t bodybuf[k_pk_cap];
  memset(bodybuf, 0, sizeof(bodybuf));
  bitw_t  body  = {.buf = bodybuf, .cap = sizeof(bodybuf)};
  uint8_t dummy = 0U;
  size_t  dlen  = 0U;
  enc_tables(&body);
  for (size_t i = 0U; i < srclen; ++i) {
    bw_put(&body, src[i], 9U);
    (void)dummy;
    (void)dlen;
  }
  (void)pkcap;
  return enc_finish(&body, pk);
}

/** @brief Decode a packed member via a flat backing; assert byte-parity to @p exp. */
static void decode_and_check(const uint8_t* pk, size_t pklen, const uint8_t* exp, size_t explen)
{
  buf_src_t      src = {.data = pk, .len = pklen};
  ra8_rar_t      rar = {.read      = buf_read,
                        .ctx       = &src,
                        .size      = (uint64_t)pklen,
                        .first_off = 0U,
                        .version   = k_ra8_rar_ver_5};
  static uint8_t out[k_out_cap];
  memset(out, 0xAA, sizeof(out));
  size_t          got = 0U;
  const ra8_err_t e   = ra8_rar5_decompress(&rar,
                                            0U,
                                            (uint64_t)pklen,
                                            out,
                                            sizeof(out),
                                            (uint64_t)explen,
                                            &s_state,
                                            &got);
  TEST_ASSERT_EQ(k_ra8_ok, e);
  TEST_ASSERT_EQ(explen, got);
  TEST_ASSERT_EQ(0, memcmp(out, exp, explen));
}

/**
 * @test test_rar5_all_literal_roundtrip
 * @brief An all-literal RAR5 block round-trips arbitrary bytes exactly.
 *
 * @par MC/DC:
 * (no compound decisions under test -- a byte-exact round-trip oracle over the
 * literal decode path, independent of the match/distance formulas.)
 */
static void test_rar5_all_literal_roundtrip(void)
{
  TEST_BEGIN("rar5: all-literal round-trip");
  static uint8_t src[600];
  for (size_t i = 0U; i < sizeof(src); ++i) {
    src[i] = (uint8_t)((i * 7U) + (i >> 2U) + 3U);
  }
  static uint8_t pk[k_pk_cap];
  memset(pk, 0, sizeof(pk));
  const size_t pklen = enc_all_literal(src, sizeof(src), pk, sizeof(pk));
  decode_and_check(pk, pklen, src, sizeof(src));
  TEST_END("rar5: all-literal round-trip");
}

/**
 * @test test_rar5_match_legs
 * @brief Every LZ token leg (direct/extra-length/extra-distance/low-distance/
 *        hi+low/large-distance match, remembered-distance, last-match repeat)
 *        decodes to the byte pattern the writer built.
 *
 * @par MC/DC:
 * Drives the control legs of the compound decisions the match run touches:
 * - libs/ra8_comic/src/ra8_rar5.c@ra8_rar5_copy_match `dist == 0 || dist > pos`:
 *   every match here has 0 < dist <= pos -> (false, false) control (copy proceeds);
 *   the dist == 0 and dist > pos true legs are driven directly by
 *   test_mcdc_copy_match. `k < length && pos < unp`: each match copies its full
 *   length with pos < unp -> the loop exits on k == length (the (false, true) leg);
 *   the (true, false) clamp leg is driven by test_mcdc_copy_match.
 * - libs/ra8_comic/src/ra8_rar5.c@s_decode_stream `out_pos < unp && consumed <=
 *   cap_bits`: the token run keeps out_pos < unp with consumed <= cap_bits (true,
 *   true) until the last token, then exits on out_pos == unp -> (false, true),
 *   proving out_pos independently ends the loop. The (true, false) runaway leg is
 *   driven by test_rar5_malformed's truncation sweep; the single-condition block
 *   boundary / last-block breaks are driven by test_mcdc_decode_stream_blocks.
 */
static void test_rar5_match_legs(void)
{
  TEST_BEGIN("rar5: LZ match / repeat legs");
  static uint8_t bodybuf[k_pk_cap];
  static uint8_t exp[k_out_cap];
  memset(bodybuf, 0, sizeof(bodybuf));
  memset(exp, 0, sizeof(exp));
  bitw_t body = {.buf = bodybuf, .cap = sizeof(bodybuf)};
  size_t elen = 0U;
  enc_tables(&body);
  /* 300-byte literal prefix so far/large distances have a window. */
  for (uint32_t i = 0U; i < 300U; ++i) {
    enc_lit(&body, exp, &elen, (uint8_t)(i + 1U));
  }
  enc_match(&body, exp, &elen, 2U, 3U);           /* len 4, dist 4          */
  enc_match_lenx(&body, exp, &elen, 1U, 1U);      /* len 11, dist 2         */
  enc_match_distx(&body, exp, &elen, 0U, 1U);     /* len 2, dist 6          */
  enc_match_lowdist(&body, exp, &elen, 0U, 5U);   /* len 2, dist 38         */
  enc_match_hilow(&body, exp, &elen, 0U, 1U, 2U); /* len 2, dist 65+16+2=83 */
  enc_match_big(&body, exp, &elen, 0U);           /* len 3, dist 257        */
  enc_repdist(&body, exp, &elen, 1U, 257U);       /* len 3, reuse dist 257  */
  enc_replast(&body, exp, &elen, 3U, 257U);       /* repeat last len/dist   */
  static uint8_t pk[k_pk_cap];
  memset(pk, 0, sizeof(pk));
  const size_t pklen = enc_finish(&body, pk);
  decode_and_check(pk, pklen, exp, elen);
  TEST_END("rar5: LZ match / repeat legs");
}

/* ---- filter oracles (independent inverse reimplementations) -------------- */

/** @brief Independent delta-filter inverse over @p d (test oracle). */
static void oracle_delta(uint8_t* d, uint32_t len, uint32_t chan)
{
  static uint8_t tmp[k_out_cap];
  memcpy(tmp, d, len);
  uint32_t dpos = 0U;
  for (uint32_t ch = 0U; ch < chan; ++ch) {
    uint8_t prev = 0U;
    for (uint32_t i = ch; i < len; i += chan) {
      prev = (uint8_t)(prev - tmp[dpos]);
      dpos++;
      d[i] = prev;
    }
  }
}

/** @brief Independent x86 CALL/JMP inverse over @p d (test oracle). */
static void oracle_x86(uint8_t* d, uint32_t len, uint32_t filepos, bool e9)
{
  if (len < 5U) {
    return;
  }
  uint32_t i = 0U;
  while (i <= len - 5U) {
    const uint8_t op = d[i];
    if (op == 0xE8U || (e9 && op == 0xE9U)) {
      const uint32_t off = i + 1U;
      uint32_t       v   = (uint32_t)d[off] | ((uint32_t)d[off + 1U] << 8U) |
                           ((uint32_t)d[off + 2U] << 16U) | ((uint32_t)d[off + 3U] << 24U);
      v -= (filepos + off);
      d[off]      = (uint8_t)(v & 0xFFU);
      d[off + 1U] = (uint8_t)((v >> 8U) & 0xFFU);
      d[off + 2U] = (uint8_t)((v >> 16U) & 0xFFU);
      d[off + 3U] = (uint8_t)((v >> 24U) & 0xFFU);
      i += 5U;
    } else {
      i += 1U;
    }
  }
}

/** @brief Independent ARM BL inverse over @p d (test oracle). */
static void oracle_arm(uint8_t* d, uint32_t len, uint32_t filepos)
{
  if (len < 4U) {
    return;
  }
  uint32_t i = 0U;
  while (i <= len - 4U) {
    if (d[i + 3U] == 0xEBU) {
      uint32_t v = (uint32_t)d[i] | ((uint32_t)d[i + 1U] << 8U) | ((uint32_t)d[i + 2U] << 16U);
      v          = (v - ((filepos + i) >> 2U)) & 0xFFFFFFU;
      d[i]       = (uint8_t)(v & 0xFFU);
      d[i + 1U]  = (uint8_t)((v >> 8U) & 0xFFU);
      d[i + 2U]  = (uint8_t)((v >> 16U) & 0xFFU);
    }
    i += 4U;
  }
}

/** @brief Round-trip a filter over crafted literals; compare to the test oracle. */
static void run_filter_case(uint32_t type, uint32_t chan, const uint8_t* raw, uint32_t len)
{
  static uint8_t bodybuf[k_pk_cap];
  static uint8_t exp[k_out_cap];
  memset(bodybuf, 0, sizeof(bodybuf));
  bitw_t body = {.buf = bodybuf, .cap = sizeof(bodybuf)};
  size_t elen = 0U;
  enc_tables(&body);
  /* The filter token precedes the range it covers: read at output position 0, it
   * transforms [0, len) once decoding completes (RAR emits the filter, then data). */
  enc_filter(&body, type, 0U, len, chan);
  for (uint32_t i = 0U; i < len; ++i) {
    enc_lit(&body, exp, &elen, raw[i]);
  }
  /* Expected = the same literals with the decoder's inverse applied. */
  if (type == 0U) {
    oracle_delta(exp, len, chan);
  } else if (type == 1U) {
    oracle_x86(exp, len, 0U, false);
  } else if (type == 2U) {
    oracle_x86(exp, len, 0U, true);
  } else {
    oracle_arm(exp, len, 0U);
  }
  static uint8_t pk[k_pk_cap];
  memset(pk, 0, sizeof(pk));
  const size_t pklen = enc_finish(&body, pk);
  decode_and_check(pk, pklen, exp, elen);
}

/**
 * @test test_rar5_filters
 * @brief Each RAR5 data filter (delta / x86 E8 / x86 E8E9 / ARM) transforms the
 *        decoded range to match an independent inverse.
 *
 * @par MC/DC:
 * Decision libs/ra8_comic/src/ra8_rar5.c@s_x86_is_op:
 * `op == 0xE8 || (e9 && op == 0xE9)` (3 conditions)
 * - E8 stream, byte 0xE8 -> true  (op==call true: control)
 * - E8 stream, byte 0xE9 -> false (op!=call, e9 false)          -- varies op vs call
 * - E8E9 stream, byte 0xE9 -> true (e9 true, op==jmp true)      -- varies e9 and jmp
 * The E8-only vs E8E9 cases prove `e9` independently gates the 0xE9 branch, and
 * the 0xE8/0xE9 bytes prove each opcode compare independently affects the outcome.
 * Also drives libs/ra8_comic/src/ra8_rar5.c@ra8_rar5_filter_delta
 * `len > k_ra8_rar5_delta_scratch || channels == 0`: the 32-byte delta range with
 * channels == 3 makes both conditions false (control, transform runs); the
 * over-long and zero-channel true legs (a decoded comic page never carries an
 * over-long or zero-channel delta) are driven directly by test_mcdc_filter_delta.
 */
static void test_rar5_filters(void)
{
  TEST_BEGIN("rar5: delta / x86 / arm filters");
  static const uint8_t k_raw[32] = {0xE8U, 0x10U, 0x20U, 0x30U, 0x40U, 0xE9U, 0x01U, 0x02U,
                                    0x03U, 0x04U, 0x11U, 0x22U, 0x33U, 0xEBU, 0x55U, 0x66U,
                                    0x77U, 0x88U, 0x99U, 0xEBU, 0xA0U, 0xB0U, 0xC0U, 0xD0U,
                                    0xE0U, 0xF0U, 0x12U, 0x34U, 0x56U, 0x78U, 0x9AU, 0xBCU};
  run_filter_case(0U, 3U, k_raw, sizeof(k_raw)); /* delta, 3 channels */
  run_filter_case(1U, 1U, k_raw, sizeof(k_raw)); /* x86 E8            */
  run_filter_case(2U, 1U, k_raw, sizeof(k_raw)); /* x86 E8E9          */
  run_filter_case(3U, 1U, k_raw, sizeof(k_raw)); /* ARM               */
  TEST_END("rar5: delta / x86 / arm filters");
}

/**
 * @test test_rar5_table_runs
 * @brief The length-table continuation codes (copy-previous and zero runs, short
 *        and long) reconstruct a table that then decodes an all-literal payload.
 *
 * @par MC/DC:
 * Provides the vector set for the compound decisions in
 * libs/ra8_comic/src/ra8_rar5_tables.c@ra8_rar5_apply_run:
 * - `num == k_r5_tbl_copy_long || num == k_r5_tbl_zero_long` (is_long): code 17
 *   -> first true; code 18 (short) -> both false; code 19 -> second true.
 * - `num == k_r5_tbl_zero_short || num == k_r5_tbl_zero_long` (is_zero): code 17
 *   -> both false; code 18 -> first true; code 19 -> second true.
 * - `!is_zero && *idx == 0`: code 9 first sets tbl[0], so every copy run here has
 *   *idx > 0 -> false (control); the copy-with-no-previous true leg is driven by
 *   test_rar5_malformed. `c < count && i < k_ra8_rar5_huff_total`: each run fills
 *   its full count with i < total -> loop runs then exits on c == count.
 * The stream emits codes 17, 18 and 19, so both operands of each `||` and both
 * operands of the `&&`/loop guard are independently exercised.
 */
static void test_rar5_table_runs(void)
{
  TEST_BEGIN("rar5: length-table run codes");
  static uint8_t bodybuf[k_pk_cap];
  memset(bodybuf, 0, sizeof(bodybuf));
  bitw_t body = {.buf = bodybuf, .cap = sizeof(bodybuf)};
  /* BD: 20 lengths of 5. */
  for (uint32_t i = 0U; i < 20U; ++i) {
    bw_put(&body, 5U, 4U);
  }
  /* LD 0..271 = 9 via a literal 9 then copy-previous runs (codes 16/17). */
  bw_put(&body, 9U, 5U);   /* tbl[0] = 9                          */
  bw_put(&body, 17U, 5U);  /* copy prev, long run                 */
  bw_put(&body, 127U, 7U); /* run = 11 + 127 = 138 -> tbl[1..138] */
  bw_put(&body, 17U, 5U);
  bw_put(&body, 122U, 7U); /* run = 133 -> tbl[139..271] */
  /* LD 272..305 = 0 via a zero run (code 18 short + 19 long). */
  bw_put(&body, 18U, 5U);
  bw_put(&body, 7U, 3U); /* run = 3 + 7 = 10 -> tbl[272..281] */
  bw_put(&body, 19U, 5U);
  bw_put(&body, 13U, 7U); /* run = 11 + 13 = 24 -> tbl[282..305] */
  /* DD/LDD/RD via direct lengths. */
  for (uint32_t i = 0U; i < 64U; ++i) {
    bw_put(&body, 6U, 5U);
  }
  for (uint32_t i = 0U; i < 16U; ++i) {
    bw_put(&body, 4U, 5U);
  }
  for (uint32_t i = 0U; i < 44U; ++i) {
    bw_put(&body, 6U, 5U);
  }
  /* Payload: a handful of literals. */
  static const uint8_t k_pay[5] = {0x41U, 0x42U, 0x43U, 0x44U, 0x45U};
  for (uint32_t i = 0U; i < sizeof(k_pay); ++i) {
    bw_put(&body, k_pay[i], 9U);
  }
  static uint8_t pk[k_pk_cap];
  memset(pk, 0, sizeof(pk));
  const size_t pklen = enc_finish(&body, pk);
  decode_and_check(pk, pklen, k_pay, sizeof(k_pay));
  TEST_END("rar5: length-table run codes");
}

/**
 * @test test_rar5_bd_zero_run
 * @brief A BD length list that zeroes unused symbols 10..14 via the 15-escape
 *        zero run still decodes an all-literal payload byte-exactly.
 *
 * @par MC/DC:
 * Drives libs/ra8_comic/src/ra8_rar5_tables.c@ra8_rar5_fill_zeros
 * `c < count && i < max`: the 5-entry zero run fills every entry with i < max
 * -> the loop iterates then exits on c == count (both conditions independently end
 * the fill); the i == max short-circuit true leg is a defensive bound the fuzz
 * harness drives. The BD symbols the table actually uses (0/4/6/9) keep length 5,
 * so a byte-exact decode proves the zero-fill left the used codes intact.
 */
static void test_rar5_bd_zero_run(void)
{
  TEST_BEGIN("rar5: BD length-list zero run");
  static uint8_t bodybuf[k_pk_cap];
  memset(bodybuf, 0, sizeof(bodybuf));
  bitw_t body = {.buf = bodybuf, .cap = sizeof(bodybuf)};
  enc_tables_bdzero(&body);
  static const uint8_t k_pay[6] = {0x30U, 0x31U, 0x32U, 0x33U, 0x34U, 0x35U};
  for (uint32_t i = 0U; i < sizeof(k_pay); ++i) {
    bw_put(&body, k_pay[i], 9U);
  }
  static uint8_t pk[k_pk_cap];
  memset(pk, 0, sizeof(pk));
  const size_t pklen = enc_finish(&body, pk);
  decode_and_check(pk, pklen, k_pay, sizeof(k_pay));
  TEST_END("rar5: BD length-list zero run");
}

/* ---- malformed / guard coverage ----------------------------------------- */

/** @brief Decode a packed buffer and return the status (no assertions). */
static ra8_err_t decode_status(const uint8_t* pk, size_t pklen, uint64_t unp)
{
  buf_src_t      src = {.data = pk, .len = pklen};
  ra8_rar_t      rar = {.read      = buf_read,
                        .ctx       = &src,
                        .size      = (uint64_t)pklen,
                        .first_off = 0U,
                        .version   = k_ra8_rar_ver_5};
  static uint8_t out[k_out_cap];
  size_t         got = 0U;
  return ra8_rar5_decompress(&rar, 0U, (uint64_t)pklen, out, sizeof(out), unp, &s_state, &got);
}

/**
 * @test test_rar5_malformed
 * @brief Corrupt headers, truncation, and out-of-range references are rejected
 *        (validation_failed) without a crash.
 *
 * @par MC/DC:
 * (no compound decisions under test -- each malformed input drives one guard leg;
 * the bad-checksum, no-tables, truncation and short-output cases are independent.)
 */
static void test_rar5_malformed(void)
{
  TEST_BEGIN("rar5: malformed / truncated rejects");
  /* A valid all-literal block, then corrupt it. */
  static const uint8_t k_src[8] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
  static uint8_t       pk[k_pk_cap];
  memset(pk, 0, sizeof(pk));
  const size_t pklen = enc_all_literal(k_src, sizeof(k_src), pk, sizeof(pk));

  /* Bad header checksum. */
  static uint8_t bad[k_pk_cap];
  memcpy(bad, pk, pklen);
  bad[2] ^= 0xFFU; /* wrong checksum byte (bytecount 1) */
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, decode_status(bad, pklen, sizeof(k_src)));

  /* Truncated stream (header only) with a large expected size -> short output. */
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, decode_status(pk, 3U, sizeof(k_src)));

  /* Every truncation length is either a clean parse or a rejection, never a crash. */
  for (size_t sz = 1U; sz <= pklen; ++sz) {
    const ra8_err_t r = decode_status(pk, sz, sizeof(k_src));
    TEST_ASSERT(r == k_ra8_ok || r == k_ra8_err_validation_failed);
  }

  /* A first block that declares no tables must be rejected. */
  static uint8_t notab[16] = {};
  notab[0]                 = 0x40U;                            /* last, no tables, bytecount 1 */
  notab[1]                 = 0x02U;                            /* block size 2                 */
  notab[2]                 = (uint8_t)(0x5AU ^ 0x40U ^ 0x02U); /* checksum                     */
  notab[3]                 = 0x00U;
  notab[4]                 = 0x00U;
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, decode_status(notab, 5U, sizeof(k_src)));

  /* A match whose distance reaches before the member start (solid reference). */
  static uint8_t bodybuf[k_pk_cap];
  memset(bodybuf, 0, sizeof(bodybuf));
  bitw_t body = {.buf = bodybuf, .cap = sizeof(bodybuf)};
  enc_tables(&body);
  bw_put(&body, 262U, 9U); /* length slot 0 -> length 2                      */
  bw_put(&body, 3U, 6U);   /* distance slot 3 -> dist 4, but no prior output */
  static uint8_t pk2[k_pk_cap];
  memset(pk2, 0, sizeof(pk2));
  const size_t pklen2 = enc_finish(&body, pk2);
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, decode_status(pk2, pklen2, 2U));
  TEST_END("rar5: malformed / truncated rejects");
}

/* ---- direct MC/DC drivers for the promoted decode helpers --------------- */

/** @brief Emit one block header + body to @p pk; return bytes written. */
static size_t enc_block(const bitw_t* body, uint8_t* pk, bool has_tables, bool is_last)
{
  const size_t   bsz  = bw_bytes(body);
  const uint32_t bits = bw_lastbits(body);
  const uint32_t bcnt = (bsz < 256U) ? 1U : 2U;
  const uint8_t  flags =
    (uint8_t)((has_tables ? 0x80U : 0U) | (is_last ? 0x40U : 0U) | ((bcnt - 1U) << 3U) | (bits - 1U));
  size_t p = 0U;
  pk[p]    = flags;
  p += 1U;
  pk[p] = (uint8_t)(bsz & 0xFFU);
  p += 1U;
  if (bcnt == 2U) {
    pk[p] = (uint8_t)((bsz >> 8U) & 0xFFU);
    p += 1U;
  }
  uint8_t chk = (uint8_t)(0x5AU ^ flags ^ (uint8_t)(bsz & 0xFFU));
  if (bcnt == 2U) {
    chk ^= (uint8_t)((bsz >> 8U) & 0xFFU);
  }
  pk[p] = chk;
  p += 1U;
  memcpy(&pk[p], body->buf, bsz);
  return p + bsz;
}

/** @brief Persistent backing so a bound ::s_state can pull bits after return. */
static uint8_t   s_bind_buf[16];
static buf_src_t s_bind_src; /**< Reader context over ::s_bind_buf. */
static ra8_rar_t s_bind_rar; /**< Archive handle bound into ::s_state. */

/** @brief Reset ::s_state into a bit reader over @p bytes (for direct helper calls). */
static void bind_bits(const uint8_t* bytes, size_t len)
{
  memcpy(s_bind_buf, bytes, len);
  s_bind_src.data    = s_bind_buf;
  s_bind_src.len     = len;
  s_bind_rar.read    = buf_read;
  s_bind_rar.ctx     = &s_bind_src;
  s_bind_rar.size    = (uint64_t)len;
  s_bind_rar.first_off = 0U;
  s_bind_rar.version = k_ra8_rar_ver_5;
  s_state            = (ra8_rar5_state_t){};
  s_state.rar        = &s_bind_rar;
  s_state.base       = 0U;
  s_state.packlen    = (uint64_t)len;
}

/**
 * @test test_mcdc_copy_match
 * @brief Both guard/clamp compound decisions of the LZ match copier.
 *
 * @par MC/DC:
 * libs/ra8_comic/src/ra8_rar5.c@ra8_rar5_copy_match:
 * - `dist == 0 || dist > (uint64_t)pos` (2 conditions):
 *   - dist=2,  pos=5 -> (false, false) control: the copy proceeds.
 *   - dist=0,  pos=5 -> (true, -)  varies dist == 0 only -> reject.
 *   - dist=100,pos=5 -> (false, true) varies dist > pos only -> reject.
 *   Vectors 1+2 prove `dist == 0` independently rejects; 1+3 prove `dist > pos`
 *   does. N+1 = 3 vectors for N=2.
 * - `k < length && pos < unp` (2 conditions):
 *   - length=3, unp=100 from pos=5 -> the loop runs (true, true) x3 then exits on
 *     k == length -> (false, true), proving `k < length` ends the copy.
 *   - length=10, unp=6 from pos=5 -> the loop runs (true, true) then exits on
 *     pos == unp -> (true, false), proving `pos < unp` clamps the copy.
 */
static void test_mcdc_copy_match(void)
{
  TEST_BEGIN("rar5: ra8_rar5_copy_match MC/DC");
  static uint8_t out[64];
  for (uint32_t i = 0U; i < sizeof(out); ++i) {
    out[i] = (uint8_t)(i + 1U);
  }
  /* dist == 0 || dist > pos. */
  size_t pos = 5U;
  TEST_ASSERT(ra8_rar5_copy_match(out, &pos, 100U, 3U, 2U)); /* (F,F) control  */
  TEST_ASSERT_EQ(8U, pos);
  pos = 5U;
  TEST_ASSERT(!ra8_rar5_copy_match(out, &pos, 100U, 3U, 0U)); /* dist==0 (T,-) */
  TEST_ASSERT_EQ(5U, pos);
  pos = 5U;
  TEST_ASSERT(!ra8_rar5_copy_match(out, &pos, 100U, 3U, 100U)); /* dist>pos (F,T) */
  TEST_ASSERT_EQ(5U, pos);
  /* k < length && pos < unp. */
  pos = 5U;
  TEST_ASSERT(ra8_rar5_copy_match(out, &pos, 100U, 3U, 2U)); /* exit on k==length */
  TEST_ASSERT_EQ(8U, pos);
  pos = 5U;
  TEST_ASSERT(ra8_rar5_copy_match(out, &pos, 6U, 10U, 2U)); /* clamp on pos==unp  */
  TEST_ASSERT_EQ(6U, pos);
  TEST_END("rar5: ra8_rar5_copy_match MC/DC");
}

/**
 * @test test_mcdc_filter_delta
 * @brief The over-long / zero-channel guard of the delta filter.
 *
 * @par MC/DC:
 * libs/ra8_comic/src/ra8_rar5.c@ra8_rar5_filter_delta
 * `len > k_ra8_rar5_delta_scratch || channels == 0` (2 conditions):
 * - len=32,  channels=2 -> (false, false) control: the transform runs.
 * - len=600, channels=2 -> (true, -)  varies len only -> range left unchanged.
 * - len=32,  channels=0 -> (false, true) varies channels only -> unchanged.
 * The descriptor parser only ever yields channels >= 1, so the zero-channel leg
 * is reachable only by this direct call. N+1 = 3 vectors for N=2.
 */
static void test_mcdc_filter_delta(void)
{
  TEST_BEGIN("rar5: ra8_rar5_filter_delta MC/DC");
  s_state = (ra8_rar5_state_t){};
  static uint8_t d[700];
  for (uint32_t i = 0U; i < sizeof(d); ++i) {
    d[i] = (uint8_t)(i & 0xFFU);
  }
  /* (F,F) control: a supported range transforms (differs from the raw input). */
  ra8_rar5_filter_delta(&s_state, d, 32U, 2U);
  bool changed = false;
  for (uint32_t i = 0U; i < 32U; ++i) {
    if (d[i] != (uint8_t)(i & 0xFFU)) {
      changed = true;
    }
  }
  TEST_ASSERT(changed);
  /* (T,-) over-long range: left unchanged. */
  for (uint32_t i = 0U; i < sizeof(d); ++i) {
    d[i] = (uint8_t)(i & 0xFFU);
  }
  ra8_rar5_filter_delta(&s_state, d, 600U, 2U);
  TEST_ASSERT_EQ(0x57U, d[599]); /* 599 & 0xFF, unchanged */
  TEST_ASSERT_EQ(0x2CU, d[300]); /* 300 & 0xFF, unchanged */
  /* (F,T) zero channels: left unchanged. */
  ra8_rar5_filter_delta(&s_state, d, 32U, 0U);
  TEST_ASSERT_EQ(0x0AU, d[10]); /* 10 & 0xFF, unchanged */
  TEST_END("rar5: ra8_rar5_filter_delta MC/DC");
}

/**
 * @test test_mcdc_fill_zeros
 * @brief Both loop-guard legs of the bounded zero-length appender.
 *
 * @par MC/DC:
 * libs/ra8_comic/src/ra8_rar5_tables.c@ra8_rar5_fill_zeros
 * `c < count && i < max` (2 conditions):
 * - start=0, count=3, max=20 -> the loop runs (true, true) x3 then exits on
 *   c == count -> (false, true), proving `c < count` ends the fill.
 * - start=18, count=5, max=20 -> the loop runs (true, true) x2 then exits on
 *   i == max -> (true, false), proving `i < max` clamps the fill.
 */
static void test_mcdc_fill_zeros(void)
{
  TEST_BEGIN("rar5: ra8_rar5_fill_zeros MC/DC");
  static uint8_t buf[20];
  memset(buf, 0xFFU, sizeof(buf));
  /* c < count exit. */
  TEST_ASSERT_EQ(3U, ra8_rar5_fill_zeros(buf, 0U, 3U, 20U));
  TEST_ASSERT_EQ(0U, buf[0]);
  TEST_ASSERT_EQ(0U, buf[2]);
  TEST_ASSERT_EQ(0xFFU, buf[3]);
  /* i < max clamp exit. */
  TEST_ASSERT_EQ(20U, ra8_rar5_fill_zeros(buf, 18U, 5U, 20U));
  TEST_ASSERT_EQ(0U, buf[18]);
  TEST_ASSERT_EQ(0U, buf[19]);
  TEST_END("rar5: ra8_rar5_fill_zeros MC/DC");
}

/**
 * @test test_mcdc_apply_run
 * @brief Every compound decision of the length-table run appender.
 *
 * @par MC/DC:
 * libs/ra8_comic/src/ra8_rar5_tables.c@ra8_rar5_apply_run, four decisions:
 * - `num == 17 || num == 19` (is_long): num=16/18 -> (F,F); num=17 -> (T,-);
 *   num=19 -> (F,T).
 * - `num == 18 || num == 19` (is_zero): num=16/17 -> (F,F); num=18 -> (T,-);
 *   num=19 -> (F,T).
 * - `!is_zero && *idx == 0`: num=16,idx=0 -> (T,T) reject; num=16,idx=1 -> (T,F)
 *   append; num=18,idx=0 -> (F,-) via is_zero true.
 * - `c < count && i < k_ra8_rar5_huff_total`: num=18,idx=0 -> runs then exits on
 *   c == count (F,T); num=16,idx=428 -> runs then exits on i == huff_total (T,F).
 * All run-count reads pull from a bound bit source; N+1 vectors per decision.
 */
static void test_mcdc_apply_run(void)
{
  TEST_BEGIN("rar5: ra8_rar5_apply_run MC/DC");
  static uint8_t   tbl[k_ra8_rar5_huff_total];
  static uint8_t   idlebits[1] = {0x00U};
  uint32_t         idx         = 0U;

  memset(tbl, 0, sizeof(tbl));
  /* num=16, idx=0 -> !is_zero && *idx==0 (T,T) -> reject. */
  bind_bits(idlebits, sizeof(idlebits));
  idx = 0U;
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, ra8_rar5_apply_run(&s_state, tbl, &idx, 16U));
  /* num=16, idx=1 (prev set) -> !is_zero && *idx==0 (T,F) -> copy run appends. */
  bind_bits(idlebits, sizeof(idlebits));
  tbl[0] = 7U;
  idx    = 1U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rar5_apply_run(&s_state, tbl, &idx, 16U));
  TEST_ASSERT_EQ(4U, idx);
  TEST_ASSERT_EQ(7U, tbl[3]);
  /* num=18, idx=0 -> is_zero (T,-), c < count exit (F,T). */
  bind_bits(idlebits, sizeof(idlebits));
  idx = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rar5_apply_run(&s_state, tbl, &idx, 18U));
  TEST_ASSERT_EQ(3U, idx);
  TEST_ASSERT_EQ(0U, tbl[0]);
  /* num=17 (copy long), idx=1 -> is_long (T,-). */
  bind_bits(idlebits, sizeof(idlebits));
  tbl[0] = 6U;
  idx    = 1U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rar5_apply_run(&s_state, tbl, &idx, 17U));
  TEST_ASSERT_EQ(12U, idx); /* 1 + (11 + 0) */
  /* num=19 (zero long), idx=0 -> is_long (F,T) / is_zero (F,T). */
  bind_bits(idlebits, sizeof(idlebits));
  idx = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rar5_apply_run(&s_state, tbl, &idx, 19U));
  TEST_ASSERT_EQ(11U, idx);
  /* num=16, idx=428 -> i < huff_total clamp (T,F). */
  bind_bits(idlebits, sizeof(idlebits));
  tbl[427] = 5U;
  idx      = (uint32_t)k_ra8_rar5_huff_total - 2U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rar5_apply_run(&s_state, tbl, &idx, 16U));
  TEST_ASSERT_EQ((uint32_t)k_ra8_rar5_huff_total, idx);
  TEST_ASSERT_EQ(5U, tbl[(uint32_t)k_ra8_rar5_huff_total - 1U]);
  TEST_END("rar5: ra8_rar5_apply_run MC/DC");
}

/**
 * @test test_mcdc_decode_stream_blocks
 * @brief The multi-block continuation and truncated-last-block branches.
 *
 * @par MC/DC:
 * (no compound decisions under test -- exercises the single-condition
 * `st->consumed >= block_end` and `if (last)` branches of
 * libs/ra8_comic/src/ra8_rar5.c@s_decode_stream: a two-block stream crosses a
 * non-last block boundary (consumed >= block_end true, last false -> opens the
 * next block), and a single last block decoded with an over-large unpacked size
 * reaches the last-block boundary with output incomplete (last true -> break ->
 * short-output rejection). The loop's compound guard is covered by
 * test_rar5_match_legs and test_rar5_malformed.)
 */
static void test_mcdc_decode_stream_blocks(void)
{
  TEST_BEGIN("rar5: multi-block + truncated-last stream");
  /* Block 1: tables + literals "ABC", not the last block. */
  static uint8_t b1buf[k_pk_cap];
  memset(b1buf, 0, sizeof(b1buf));
  bitw_t b1 = {.buf = b1buf, .cap = sizeof(b1buf)};
  enc_tables(&b1);
  bw_put(&b1, 0x41U, 9U);
  bw_put(&b1, 0x42U, 9U);
  bw_put(&b1, 0x43U, 9U);
  /* Block 2: literals "DEF" reusing block 1's tables, the last block. */
  static uint8_t b2buf[k_pk_cap];
  memset(b2buf, 0, sizeof(b2buf));
  bitw_t b2 = {.buf = b2buf, .cap = sizeof(b2buf)};
  bw_put(&b2, 0x44U, 9U);
  bw_put(&b2, 0x45U, 9U);
  bw_put(&b2, 0x46U, 9U);
  static uint8_t pk[k_pk_cap];
  memset(pk, 0, sizeof(pk));
  size_t p = enc_block(&b1, pk, true, false);
  p += enc_block(&b2, &pk[p], false, true);
  static const uint8_t exp[6] = {0x41U, 0x42U, 0x43U, 0x44U, 0x45U, 0x46U};
  decode_and_check(pk, p, exp, sizeof(exp));
  /* Truncated last block: a valid 4-literal stream decoded with a larger unpacked
   * size drains its (last) block before completing -> if (last) break -> reject. */
  static const uint8_t k_src[4] = {1U, 2U, 3U, 4U};
  static uint8_t       spk[k_pk_cap];
  memset(spk, 0, sizeof(spk));
  const size_t spklen = enc_all_literal(k_src, sizeof(k_src), spk, sizeof(spk));
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 decode_status(spk, spklen, (uint64_t)sizeof(k_src) + 2U));
  TEST_END("rar5: multi-block + truncated-last stream");
}

/**
 * @test test_rar5_guards
 * @brief `ra8_rar5_decompress` rejects NULL args, a non-RAR5 archive, an
 *        undersized buffer, a zero packed size, and decodes an empty member.
 *
 * @par MC/DC:
 * (no compound decisions under test -- each guard is an independent single-condition
 * early return driven one at a time.)
 */
static void test_rar5_guards(void)
{
  TEST_BEGIN("rar5: entry-point guards");
  static const uint8_t k_src[4] = {9U, 8U, 7U, 6U};
  static uint8_t       pk[k_pk_cap];
  memset(pk, 0, sizeof(pk));
  const size_t pklen = enc_all_literal(k_src, sizeof(k_src), pk, sizeof(pk));

  buf_src_t      src = {.data = pk, .len = pklen};
  ra8_rar_t      rar = {.read    = buf_read,
                        .ctx     = &src,
                        .size    = (uint64_t)pklen,
                        .version = k_ra8_rar_ver_5};
  static uint8_t out[k_out_cap];
  size_t         got = 0U;

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rar5_decompress(nullptr, 0U, pklen, out, sizeof(out), 4U, &s_state, &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rar5_decompress(&rar, 0U, pklen, nullptr, sizeof(out), 4U, &s_state, &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rar5_decompress(&rar, 0U, pklen, out, sizeof(out), 4U, nullptr, &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rar5_decompress(&rar, 0U, pklen, out, sizeof(out), 4U, &s_state, nullptr));

  /* Non-RAR5 archive. */
  ra8_rar_t r4 = rar;
  r4.version   = k_ra8_rar_ver_4;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_rar5_decompress(&r4, 0U, pklen, out, sizeof(out), 4U, &s_state, &got));

  /* Output buffer too small. */
  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 ra8_rar5_decompress(&rar, 0U, pklen, out, 2U, 4U, &s_state, &got));

  /* Zero packed size. */
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 ra8_rar5_decompress(&rar, 0U, 0U, out, sizeof(out), 4U, &s_state, &got));

  /* Empty member (unp_size 0) decodes to nothing. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rar5_decompress(&rar, 0U, pklen, out, sizeof(out), 0U, &s_state, &got));
  TEST_ASSERT_EQ(0U, got);
  TEST_END("rar5: entry-point guards");
}

/* ---- CBR facade: compressed-page parity + RAR4 unsupported --------------- */

/** @brief Encode one RAR5 vint; return its byte length. */
static size_t av_vint(uint8_t* out, uint64_t v)
{
  size_t i = 0U;
  for (;;) {
    uint8_t b = (uint8_t)(v & 0x7FU);
    v >>= 7U;
    if (v != 0U) {
      out[i] = (uint8_t)(b | 0x80U);
      i++;
    } else {
      out[i] = b;
      i++;
      break;
    }
  }
  return i;
}

/** @brief Encode a RAR5 file block (STORE if @p method 0, else compressed). */
static size_t av5_file(uint8_t*       out,
                       const char*    name,
                       const uint8_t* data,
                       size_t         dlen,
                       uint64_t       unp,
                       uint32_t       method)
{
  uint8_t        body[256] = {};
  size_t         b         = 0U;
  const bool     has_data  = (dlen > 0U);
  const uint64_t hflags    = has_data ? 0x02U : 0x00U;
  const uint64_t cinfo     = (uint64_t)method << 7U; /* method occupies bits 7..9 */
  b += av_vint(&body[b], 2U);                        /* header type = file        */
  b += av_vint(&body[b], hflags);
  if (has_data) {
    b += av_vint(&body[b], (uint64_t)dlen);
  }
  b += av_vint(&body[b], 0U);  /* file flags    */
  b += av_vint(&body[b], unp); /* unpacked size */
  b += av_vint(&body[b], 0U);  /* attributes    */
  b += av_vint(&body[b], cinfo);
  b += av_vint(&body[b], 0U); /* host os */
  const size_t nlen = strlen(name);
  b += av_vint(&body[b], (uint64_t)nlen);
  memcpy(&body[b], name, nlen);
  b += nlen;
  size_t p = 4U; /* header crc (unverified) */
  p += av_vint(&out[p], (uint64_t)b);
  memcpy(&out[p], body, b);
  p += b;
  if (has_data) {
    memcpy(&out[p], data, dlen);
    p += dlen;
  }
  return p;
}

/** @brief Encode a RAR5 main/end block (@p htype 1 or 5). */
static size_t av5_simple(uint8_t* out, uint64_t htype)
{
  uint8_t body[8] = {};
  size_t  b       = 0U;
  b += av_vint(&body[b], htype);
  b += av_vint(&body[b], 0U);
  b += av_vint(&body[b], 0U);
  size_t p = 4U;
  p += av_vint(&out[p], (uint64_t)b);
  memcpy(&out[p], body, b);
  p += b;
  return p;
}

/** @brief Comic page/name/archive buffers for the facade tests. */
static uint8_t          s_farc[k_arc_cap];
static size_t           s_farc_len;
static ra8_comic_page_t s_fpages[16];
static char             s_fnames[1024];

/** @brief Read callback over ::s_farc. */
static size_t farc_read(void* ctx, uint64_t off, void* dst, size_t len)
{
  (void)ctx;
  buf_src_t s = {.data = s_farc, .len = s_farc_len};
  return buf_read(&s, off, dst, len);
}

/**
 * @test test_cbr_compressed_parity
 * @brief A `.cbr` whose page is RAR5-compressed opens by magic and decodes to the
 *        exact bytes of the equivalent STORE page (the #235 CBZ-parity acceptance).
 *
 * @par MC/DC:
 * Decision libs/ra8_comic/src/ra8_comic_cbr.c@s_add_member:
 * `is_store || is_rar5` (2 conditions)
 * - STORE page on RAR5:      is_store=1              -> extractable (control)
 * - compressed page on RAR5: is_store=0, is_rar5=1   -> extractable (varies is_rar5)
 * The RAR4-compressed case (both false -> not extractable) is covered by
 * test_cbr_rar4_compressed_unsupported; together they give the N+1 vectors.
 */
static void test_cbr_compressed_parity(void)
{
  TEST_BEGIN("comic cbr: RAR5-compressed page == STORE page (parity)");
  /* One PNG page; the STORE and compressed archives must decode to it identically. */
  static uint8_t png[k_cf_png_max];
  const size_t   plen = cf_make_png(7U, 5U, 9U, png, sizeof(png));
  TEST_ASSERT(plen > 0U);
  static uint8_t packed[k_pk_cap];
  memset(packed, 0, sizeof(packed));
  const size_t pklen = enc_all_literal(png, plen, packed, sizeof(packed));

  static const uint8_t k_sig[8] = {0x52U, 0x61U, 0x72U, 0x21U, 0x1AU, 0x07U, 0x01U, 0x00U};
  size_t               p        = 0U;
  memcpy(&s_farc[p], k_sig, sizeof(k_sig));
  p += sizeof(k_sig);
  p += av5_simple(&s_farc[p], 1U);
  p += av5_file(&s_farc[p], "page.png", packed, pklen, (uint64_t)plen, 1U); /* compressed */
  p += av5_simple(&s_farc[p], 5U);
  s_farc_len = p;

  ra8_comic_t c = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_comic_open(&c,
                                farc_read,
                                nullptr,
                                (uint64_t)s_farc_len,
                                s_fpages,
                                (uint32_t)(sizeof(s_fpages) / sizeof(s_fpages[0])),
                                s_fnames,
                                (uint32_t)sizeof(s_fnames)));
  TEST_ASSERT_EQ(k_ra8_comic_kind_cbr, ra8_comic_kind(&c));
  TEST_ASSERT_EQ(1U, ra8_comic_page_count(&c));
  uint16_t nl  = 0U;
  uint64_t raw = 0U;
  uint8_t  ex  = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_page_info(&c, 0U, nullptr, 0U, &nl, &raw, &ex));
  TEST_ASSERT_EQ(1U, ex); /* compressed RAR5 page is extractable */
  TEST_ASSERT_EQ(plen, raw);

  static uint8_t obuf[k_cf_png_max];
  memset(obuf, 0, sizeof(obuf));
  size_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_page_read(&c, 0U, obuf, sizeof(obuf), &got));
  TEST_ASSERT_EQ(plen, got);
  TEST_ASSERT_EQ(0, memcmp(obuf, png, plen)); /* byte-parity with the STORE page */
  TEST_ASSERT(cf_decode_ok(obuf, got, 7, 5)); /* and decodes to real pixels      */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_close(&c));
  TEST_END("comic cbr: RAR5-compressed page == STORE page (parity)");
}

/* ---- RAR4 compressed member: unsupported (extractable == 0) -------------- */

/** @brief Write a little-endian uint16. */
static void a4_le16(uint8_t* p, uint16_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

/** @brief Write a little-endian uint32. */
static void a4_le32(uint8_t* p, uint32_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

/** @brief Encode a RAR4 file block with an arbitrary METHOD byte. */
static size_t
a4_file(uint8_t* out, const char* name, const uint8_t* data, size_t dlen, uint8_t method)
{
  const size_t nlen = strlen(name);
  const size_t head = 32U + nlen;
  a4_le16(&out[0], 0U);
  out[2] = 0x74U;            /* type: file  */
  a4_le16(&out[3], 0x8000U); /* flags: LONG */
  a4_le16(&out[5], (uint16_t)head);
  a4_le32(&out[7], (uint32_t)dlen);
  a4_le32(&out[11], (uint32_t)dlen);
  out[15] = 0U;
  a4_le32(&out[16], 0U);
  a4_le32(&out[20], 0U);
  out[24] = 20U;
  out[25] = method; /* 0x30 = store; anything else = compressed */
  a4_le16(&out[26], (uint16_t)nlen);
  a4_le32(&out[28], 0U);
  memcpy(&out[32], name, nlen);
  memcpy(&out[head], data, dlen);
  return head + dlen;
}

/** @brief Encode the RAR4 main block. */
static size_t a4_main(uint8_t* out)
{
  a4_le16(&out[0], 0U);
  out[2] = 0x73U;
  a4_le16(&out[3], 0U);
  a4_le16(&out[5], 13U);
  memset(&out[7], 0, 6U);
  return 13U;
}

/**
 * @test test_cbr_rar4_compressed_unsupported
 * @brief A RAR4-compressed page is indexed but non-extractable; page_read reports
 *        it unsupported (the RAR4 codec is out of scope).
 *
 * @par MC/DC:
 * Completes the libs/ra8_comic/src/ra8_comic_cbr.c@s_add_member `is_store || is_rar5`
 * vector set from test_cbr_compressed_parity:
 * - RAR4 compressed page: is_store=0, is_rar5=0 -> not extractable (both false).
 */
static void test_cbr_rar4_compressed_unsupported(void)
{
  TEST_BEGIN("comic cbr: RAR4-compressed page unsupported");
  static const uint8_t k_sig[7]  = {0x52U, 0x61U, 0x72U, 0x21U, 0x1AU, 0x07U, 0x00U};
  static const uint8_t k_data[8] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
  size_t               p         = 0U;
  memcpy(&s_farc[p], k_sig, sizeof(k_sig));
  p += sizeof(k_sig);
  p += a4_main(&s_farc[p]);
  p +=
    a4_file(&s_farc[p], "page.png", k_data, sizeof(k_data), 0x33U); /* method 0x33 = compressed */
  s_farc_len = p;

  ra8_comic_t c = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_comic_open(&c,
                                farc_read,
                                nullptr,
                                (uint64_t)s_farc_len,
                                s_fpages,
                                (uint32_t)(sizeof(s_fpages) / sizeof(s_fpages[0])),
                                s_fnames,
                                (uint32_t)sizeof(s_fnames)));
  TEST_ASSERT_EQ(k_ra8_comic_kind_cbr, ra8_comic_kind(&c));
  TEST_ASSERT_EQ(1U, ra8_comic_page_count(&c));
  uint16_t nl  = 0U;
  uint64_t raw = 0U;
  uint8_t  ex  = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_page_info(&c, 0U, nullptr, 0U, &nl, &raw, &ex));
  TEST_ASSERT_EQ(0U, ex); /* RAR4-compressed: not extractable */
  static uint8_t obuf[64];
  size_t         got = 0U;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_comic_page_read(&c, 0U, obuf, sizeof(obuf), &got));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_close(&c));
  TEST_END("comic cbr: RAR4-compressed page unsupported");
}

/**
 * @test test_rar_extract_dispatch
 * @brief `ra8_rar_extract` routes STORE by copy, decodes RAR5-compressed, and
 *        rejects non-files, directories, RAR4-compressed, and a missing scratch.
 *
 * @par MC/DC:
 * Decision libs/ra8_comic/src/ra8_rar.c@ra8_rar_extract:
 * `ent->is_file == 0 || ent->is_dir != 0` (2 conditions)
 * - is_file=1, is_dir=0 -> false (control: a real file proceeds)
 * - is_file=0, is_dir=0 -> true  (varies is_file: non-file rejected)
 * - is_file=1, is_dir=1 -> true  (varies is_dir: directory rejected)
 * The first two prove is_file independently rejects; the first and third prove the
 * same for is_dir. N+1 = 3 vectors for N=2 conditions.
 */
static void test_rar_extract_dispatch(void)
{
  TEST_BEGIN("rar: extract dispatch (store / compressed / guards)");
  static const uint8_t k_bytes[16] =
    {10U, 11U, 12U, 13U, 14U, 15U, 16U, 17U, 18U, 19U, 20U, 21U, 22U, 23U, 24U, 25U};
  buf_src_t      src = {.data = k_bytes, .len = sizeof(k_bytes)};
  ra8_rar_t      rar = {.read      = buf_read,
                        .ctx       = &src,
                        .size      = (uint64_t)sizeof(k_bytes),
                        .first_off = 0U,
                        .version   = k_ra8_rar_ver_5};
  static uint8_t obuf[64];
  size_t         got = 0U;

  /* STORE member -> copy (control: is_file true, is_dir false). */
  ra8_rar_entry_t store = {.is_file   = 1U,
                           .is_dir    = 0U,
                           .method    = (uint8_t)k_ra8_rar_method_store,
                           .data_off  = 0U,
                           .pack_size = 8U,
                           .unp_size  = 8U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rar_extract(&rar, &store, obuf, sizeof(obuf), &s_state, &got));
  TEST_ASSERT_EQ(8U, got);
  TEST_ASSERT_EQ(0, memcmp(obuf, k_bytes, 8U));

  /* Non-file entry -> unsupported (varies is_file). */
  ra8_rar_entry_t nonfile = store;
  nonfile.is_file         = 0U;
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_rar_extract(&rar, &nonfile, obuf, sizeof(obuf), &s_state, &got));

  /* Directory entry -> unsupported (varies is_dir). */
  ra8_rar_entry_t dir = store;
  dir.is_dir          = 1U;
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_rar_extract(&rar, &dir, obuf, sizeof(obuf), &s_state, &got));

  /* Compressed member with NULL scratch -> null_ptr. */
  ra8_rar_entry_t comp = store;
  comp.method          = (uint8_t)k_ra8_rar_method_compressed;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rar_extract(&rar, &comp, obuf, sizeof(obuf), nullptr, &got));

  /* RAR4-compressed member -> unsupported (legacy codec out of scope). */
  ra8_rar_t r4 = rar;
  r4.version   = k_ra8_rar_ver_4;
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_rar_extract(&r4, &comp, obuf, sizeof(obuf), &s_state, &got));

  /* Unopened archive -> invalid_state. */
  ra8_rar_t none = rar;
  none.version   = k_ra8_rar_ver_none;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_rar_extract(&none, &store, obuf, sizeof(obuf), &s_state, &got));

  /* NULL argument guards. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rar_extract(nullptr, &store, obuf, sizeof(obuf), &s_state, &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rar_extract(&rar, nullptr, obuf, sizeof(obuf), &s_state, &got));
  TEST_END("rar: extract dispatch (store / compressed / guards)");
}

/**
 * @brief Test entry point -- runs the RAR5 decompressor suite in order.
 * @return 0 on success; unity_minimal.h exits non-zero on the first failure.
 */
int32_t main(void)
{
  test_rar5_all_literal_roundtrip();
  test_rar5_match_legs();
  test_rar5_filters();
  test_rar5_table_runs();
  test_rar5_bd_zero_run();
  test_rar5_malformed();
  test_mcdc_copy_match();
  test_mcdc_filter_delta();
  test_mcdc_fill_zeros();
  test_mcdc_apply_run();
  test_mcdc_decode_stream_blocks();
  test_rar5_guards();
  test_rar_extract_dispatch();
  test_cbr_compressed_parity();
  test_cbr_rar4_compressed_unsupported();
  (void)fprintf(stderr, "[OK  ] test_ra8_rar5.c\n");
  return 0;
}
