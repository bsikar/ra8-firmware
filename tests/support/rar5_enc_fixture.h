/**
 * @file rar5_enc_fixture.h
 * @brief Shared test fixture: a spec-conformant RAR5 "method 50" writer.
 *
 * @details
 * There is no free tool that *writes* a RAR-compressed stream, so the RAR5
 * test suites share this small writer: it emits one compressed block with
 * uniform-length canonical Huffman tables (so a symbol's code is its own
 * index) and a literal/match/repeat/filter token program, then wraps it in
 * the block header the decoder validates. Also provides the flat-buffer
 * ra8_rar_read_fn backing, the round-trip decode oracle, and the shared
 * decoder scratch pool. The writer is first-party test scaffolding; it
 * vendors no `unrar` code. Every helper is `static inline`, so each
 * including test binary gets an independent copy and unused helpers
 * compile away.
 *
 * Tests are magic-number exempt, so byte offsets and bit widths appear as
 * literals.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_rar.h"
#include "ra8_rar5.h"
#include "unity_minimal.h"

/**
 * @enum rar5_enc_fixture_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_rar5_enc_fixture_bw_put_12   = 12U,
  k_rar5_enc_fixture_bw_put_15   = 15U,
  k_rar5_enc_fixture_bw_put_5    = 5U,
  k_rar5_enc_fixture_bw_put_9    = 9U,
  k_rar5_enc_fixture_bw_put_ff   = 0xFFU,
  k_rar5_enc_fixture_exp_copy_33 = 33U,
  k_rar5_enc_fixture_exp_copy_65 = 65U,
  k_rar5_enc_fixture_i_10        = 10U,
  k_rar5_enc_fixture_i_20        = 20U,
  k_rar5_enc_fixture_i_34        = 34U,
  k_rar5_enc_fixture_i_44        = 44U,
  k_rar5_enc_fixture_i_64        = 64U,
  k_rar5_enc_fixture_sentinel_5a = 0x5AU,
  k_rar5_enc_fixture_val_7       = 7U,
} rar5_enc_fixture_uint8_const_t;

/**
 * @enum rar5_enc_fixture_uint16_const_t
 * @brief Named uint16_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint16_t {
  k_rar5_enc_fixture_bw_put_256   = 256U,
  k_rar5_enc_fixture_bw_put_258   = 258U,
  k_rar5_enc_fixture_bw_put_262   = 262U,
  k_rar5_enc_fixture_bw_put_270   = 270U,
  k_rar5_enc_fixture_exp_copy_257 = 257U,
  k_rar5_enc_fixture_i_272        = 272U,
} rar5_enc_fixture_uint16_const_t;

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
static inline size_t buf_read(void* ctx, uint64_t off, void* dst, size_t len)
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
static inline void bw_put(bitw_t* w, uint32_t v, uint32_t n)
{
  for (uint32_t i = n; i > 0U; --i) {
    const uint32_t bit = (v >> (i - 1U)) & 1U;
    if (w->byte < w->cap) {
      if (bit != 0U) {
        w->buf[w->byte] |= (uint8_t)(1U << (k_rar5_enc_fixture_val_7 - w->bitpos));
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
static inline size_t bw_bytes(const bitw_t* w)
{
  return (w->bitpos == 0U) ? w->byte : (w->byte + 1U);
}

/** @brief Valid bits in the last written byte (1..8). */
static inline uint32_t bw_lastbits(const bitw_t* w)
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
static inline void enc_table_body(bitw_t* w)
{
  /* LD: 272 nines then 34 zeros. */
  for (uint32_t i = 0U; i < k_rar5_enc_fixture_i_272; ++i) {
    bw_put(w, k_rar5_enc_fixture_bw_put_9, k_rar5_enc_fixture_bw_put_5);
  }
  for (uint32_t i = 0U; i < k_rar5_enc_fixture_i_34; ++i) {
    bw_put(w, 0U, k_rar5_enc_fixture_bw_put_5);
  }
  for (uint32_t i = 0U; i < k_rar5_enc_fixture_i_64; ++i) { /* DD: 64 sixes */
    bw_put(w, 6U, k_rar5_enc_fixture_bw_put_5);
  }
  for (uint32_t i = 0U; i < 16U; ++i) { /* LDD: 16 fours */
    bw_put(w, 4U, k_rar5_enc_fixture_bw_put_5);
  }
  for (uint32_t i = 0U; i < k_rar5_enc_fixture_i_44; ++i) { /* RD: 44 sixes */
    bw_put(w, 6U, k_rar5_enc_fixture_bw_put_5);
  }
}

/** @brief Emit the BD pre-table lengths (all 5) then the 430 combined lengths. */
static inline void enc_tables(bitw_t* w)
{
  for (uint32_t i = 0U; i < k_rar5_enc_fixture_i_20;
       ++i) { /* BD: 20 lengths of 5 bits, all value 5 */
    bw_put(w, k_rar5_enc_fixture_bw_put_5, 4U);
  }
  enc_table_body(w);
}

/**
 * @brief Emit tables whose BD length list zeroes symbols 10..14 via an escape run.
 * @details The BD symbols the table body actually uses (0, 4, 6, 9) all stay length
 *          5 and keep their index-as-code property, so decode is unchanged -- but the
 *          15-escape zero run exercises the decoder's zero-fill leg.
 */
static inline void enc_tables_bdzero(bitw_t* w)
{
  for (uint32_t i = 0U; i < k_rar5_enc_fixture_i_10; ++i) { /* BD symbols 0..9 length 5 */
    bw_put(w, k_rar5_enc_fixture_bw_put_5, 4U);
  }
  bw_put(w, k_rar5_enc_fixture_bw_put_15, 4U); /* escape                                 */
  bw_put(w, 3U, 4U);                           /* zero count 3 -> 3+2 = 5 zeros (10..14) */
  for (uint32_t i = 0U; i < k_rar5_enc_fixture_bw_put_5;
       ++i) { /* BD symbols 15..19 length 5 */
    bw_put(w, k_rar5_enc_fixture_bw_put_5, 4U);
  }
  enc_table_body(w);
}

/** @brief Emit one literal byte + record it in the expected output. */
static inline void enc_lit(bitw_t* w, uint8_t* exp, size_t* elen, uint8_t b)
{
  bw_put(w, b, k_rar5_enc_fixture_bw_put_9);
  exp[*elen] = b;
  *elen += 1U;
}

/** @brief Copy @p len bytes from `-dist` into the expected output. */
static inline void exp_copy(uint8_t* exp, size_t* elen, uint32_t len, uint32_t dist)
{
  for (uint32_t k = 0U; k < len; ++k) {
    exp[*elen] = exp[*elen - dist];
    *elen += 1U;
  }
}

/** @brief Emit a match with no extra bits (lenslot<8, distslot<4). */
static inline void
enc_match(bitw_t* w, uint8_t* exp, size_t* elen, uint32_t lenslot, uint32_t distslot)
{
  bw_put(w, k_rar5_enc_fixture_bw_put_262 + lenslot, k_rar5_enc_fixture_bw_put_9);
  bw_put(w, distslot, 6U);
  exp_copy(exp, elen, 2U + lenslot, 1U + distslot);
}

/** @brief Emit lenslot 8 (1 length-extra bit), distslot<4. */
static inline void
enc_match_lenx(bitw_t* w, uint8_t* exp, size_t* elen, uint32_t len_extra, uint32_t distslot)
{
  bw_put(w, k_rar5_enc_fixture_bw_put_270, k_rar5_enc_fixture_bw_put_9); /* length slot 8 */
  bw_put(w, len_extra, 1U);
  bw_put(w, distslot, 6U);
  exp_copy(exp, elen, k_rar5_enc_fixture_i_10 + len_extra, 1U + distslot);
}

/** @brief Emit distslot 4 (1 distance-extra bit), lenslot<8. */
static inline void
enc_match_distx(bitw_t* w, uint8_t* exp, size_t* elen, uint32_t lenslot, uint32_t dist_extra)
{
  bw_put(w, k_rar5_enc_fixture_bw_put_262 + lenslot, k_rar5_enc_fixture_bw_put_9);
  bw_put(w, 4U, 6U); /* distance slot 4 -> dbits 1 */
  bw_put(w, dist_extra, 1U);
  exp_copy(exp, elen, 2U + lenslot, k_rar5_enc_fixture_bw_put_5 + dist_extra);
}

/** @brief Emit distslot 10 (dbits==4, low-distance symbol), lenslot<4. */
static inline void
enc_match_lowdist(bitw_t* w, uint8_t* exp, size_t* elen, uint32_t lenslot, uint32_t lowdist)
{
  bw_put(w, k_rar5_enc_fixture_bw_put_262 + lenslot, k_rar5_enc_fixture_bw_put_9);
  bw_put(w, k_rar5_enc_fixture_i_10, 6U); /* distance slot 10 -> dbits 4 */
  bw_put(w, lowdist, 4U);                 /* low-distance table symbol   */
  exp_copy(exp, elen, 2U + lenslot, k_rar5_enc_fixture_exp_copy_33 + lowdist);
}

/** @brief Emit distslot 12 (dbits>4: hi extra + low-distance symbol). */
static inline void enc_match_hilow(bitw_t*  w,
                                   uint8_t* exp,
                                   size_t*  elen,
                                   uint32_t lenslot,
                                   uint32_t hi,
                                   uint32_t lowdist)
{
  bw_put(w, k_rar5_enc_fixture_bw_put_262 + lenslot, k_rar5_enc_fixture_bw_put_9);
  bw_put(w, k_rar5_enc_fixture_bw_put_12, 6U); /* distance slot 12 -> dbits 5 */
  bw_put(w, hi, 1U);                           /* dbits-4 == 1 extra bit      */
  bw_put(w, lowdist, 4U);
  exp_copy(exp, elen, 2U + lenslot, k_rar5_enc_fixture_exp_copy_65 + (hi << 4U) + lowdist);
}

/** @brief Emit distslot 16 (large distance > 0x100, exercises length bias). */
static inline void enc_match_big(bitw_t* w, uint8_t* exp, size_t* elen, uint32_t lenslot)
{
  bw_put(w, k_rar5_enc_fixture_bw_put_262 + lenslot, k_rar5_enc_fixture_bw_put_9);
  bw_put(w, 16U, 6U); /* distance slot 16 -> dbits 7 */
  bw_put(w, 0U, 3U);  /* dbits-4 == 3 extra bits = 0 */
  bw_put(w, 0U, 4U);  /* low-distance symbol 0       */
  /* dist = 1 + (2<<7) = 257 (> 0x100) -> length += 1 */
  exp_copy(exp, elen, 2U + lenslot + 1U, k_rar5_enc_fixture_exp_copy_257);
}

/** @brief Emit a remembered-distance match (idx 0) reusing @p dist. */
static inline void
enc_repdist(bitw_t* w, uint8_t* exp, size_t* elen, uint32_t rd_lenslot, uint32_t dist)
{
  bw_put(w, k_rar5_enc_fixture_bw_put_258, k_rar5_enc_fixture_bw_put_9); /* rep distance index 0 */
  bw_put(w, rd_lenslot, 6U);                                             /* repeat-length slot   */
  exp_copy(exp, elen, 2U + rd_lenslot, dist);
}

/** @brief Emit a last-match repeat of @p len at @p dist. */
static inline void enc_replast(bitw_t* w, uint8_t* exp, size_t* elen, uint32_t len, uint32_t dist)
{
  bw_put(w, k_rar5_enc_fixture_exp_copy_257, k_rar5_enc_fixture_bw_put_9);
  exp_copy(exp, elen, len, dist);
}

/** @brief Emit a filter descriptor (does not itself produce output). */
static inline void enc_filter(bitw_t* w, uint32_t type, uint32_t rel, uint32_t len, uint32_t chan)
{
  bw_put(w, k_rar5_enc_fixture_bw_put_256, k_rar5_enc_fixture_bw_put_9);
  bw_put(w, 0U, 2U); /* filter-data byte count 1 */
  bw_put(w, rel & k_rar5_enc_fixture_bw_put_ff, 8U);
  bw_put(w, 0U, 2U);
  bw_put(w, len & k_rar5_enc_fixture_bw_put_ff, 8U);
  bw_put(w, type, 3U);
  if (type == 0U) {
    bw_put(w, chan - 1U, k_rar5_enc_fixture_bw_put_5);
  }
}

/** @brief Wrap a completed body writer into a packed block; return packed length. */
static inline size_t enc_finish(const bitw_t* body, uint8_t* pk)
{
  const size_t   bsz  = bw_bytes(body);
  const uint32_t bits = bw_lastbits(body);
  const uint32_t bcnt = (bsz < 256U) ? 1U : 2U;
  const uint8_t  flags =
    (uint8_t)(0xC0U | ((bcnt - 1U) << 3U) | (bits - 1U)); /* tables|last|bitsize */
  size_t p = 0U;
  pk[p]    = flags;
  p += 1U;
  pk[p] = (uint8_t)(bsz & k_rar5_enc_fixture_bw_put_ff);
  p += 1U;
  if (bcnt == 2U) {
    pk[p] = (uint8_t)((bsz >> 8U) & k_rar5_enc_fixture_bw_put_ff);
    p += 1U;
  }
  uint8_t chk = (uint8_t)(k_rar5_enc_fixture_sentinel_5a ^ flags ^
                          (uint8_t)(bsz & k_rar5_enc_fixture_bw_put_ff));
  if (bcnt == 2U) {
    chk ^= (uint8_t)((bsz >> 8U) & k_rar5_enc_fixture_bw_put_ff);
  }
  pk[p] = chk;
  p += 1U;
  memcpy(&pk[p], body->buf, bsz);
  return p + bsz;
}

/** @brief Encode @p src as an all-literal RAR5 block; return packed length. */
static inline size_t enc_all_literal(const uint8_t* src, size_t srclen, uint8_t* pk, size_t pkcap)
{
  static uint8_t s_bodybuf[k_pk_cap];
  memset(s_bodybuf, 0, sizeof(s_bodybuf));
  bitw_t  body  = {.buf = s_bodybuf, .cap = sizeof(s_bodybuf)};
  uint8_t dummy = 0U;
  size_t  dlen  = 0U;
  enc_tables(&body);
  for (size_t i = 0U; i < srclen; ++i) {
    bw_put(&body, src[i], k_rar5_enc_fixture_bw_put_9);
    (void)dummy;
    (void)dlen;
  }
  (void)pkcap;
  return enc_finish(&body, pk);
}

/** @brief Decode a packed member via a flat backing; assert byte-parity to @p exp. */
static inline void
decode_and_check(const uint8_t* pk, size_t pklen, const uint8_t* exp, size_t explen)
{
  buf_src_t      src = {.data = pk, .len = pklen};
  ra8_rar_t      rar = {.read      = buf_read,
                        .ctx       = &src,
                        .size      = (uint64_t)pklen,
                        .first_off = 0U,
                        .version   = k_ra8_rar_ver_5};
  static uint8_t s_out[k_out_cap];
  memset(s_out, 0xAA, sizeof(s_out));
  size_t          got = 0U;
  const ra8_err_t e   = ra8_rar5_decompress(&rar,
                                            0U,
                                            (uint64_t)pklen,
                                            s_out,
                                            sizeof(s_out),
                                            (uint64_t)explen,
                                            &s_state,
                                            &got);
  TEST_ASSERT_EQ(k_ra8_ok, e);
  TEST_ASSERT_EQ(explen, got);
  TEST_ASSERT_EQ(0, memcmp(s_out, exp, explen));
}

/**
 * @test test_rar5_all_literal_roundtrip
 * @brief An all-literal RAR5 block round-trips arbitrary bytes exactly.
 *
 * @par MC/DC:
 * (no compound decisions under test -- a byte-exact round-trip oracle over the
 * literal decode path, independent of the match/distance formulas.)
 */
static inline ra8_err_t decode_status(const uint8_t* pk, size_t pklen, uint64_t unp)
{
  buf_src_t      src = {.data = pk, .len = pklen};
  ra8_rar_t      rar = {.read      = buf_read,
                        .ctx       = &src,
                        .size      = (uint64_t)pklen,
                        .first_off = 0U,
                        .version   = k_ra8_rar_ver_5};
  static uint8_t s_out[k_out_cap];
  size_t         got = 0U;
  return ra8_rar5_decompress(&rar, 0U, (uint64_t)pklen, s_out, sizeof(s_out), unp, &s_state, &got);
}
