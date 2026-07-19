/**
 * @file test_ra8_rar5_mcdc.c
 * @brief MC/DC vector tests for the promoted ra8_rar5 decode helpers.
 *
 * @details
 * Split out of test_ra8_rar5.c to keep each test translation unit under the
 * repository file-size cap. This sibling owns the direct MC/DC drivers for
 * the promoted decode helpers (copy-match, delta filter, zero fill, table
 * apply-run, block chaining) plus the entry-point guard tests; the
 * round-trip / filter / malformed suite stays in test_ra8_rar5.c and the
 * CBR facade tests live in test_ra8_rar5_archive.c. The shared RAR5 writer
 * fixture is tests/support/rar5_enc_fixture.h. Tests are magic-number
 * exempt, so byte offsets and bit widths appear as literals.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_rar.h"
#include "ra8_rar5.h"
#include "ra8_rar5_internal.h"
#include "support/rar5_enc_fixture.h"
#include "unity_minimal.h"

/**
 * @enum rar5_mcdc_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_rar5_mcdc_bsz_ff      = 0xFFU,
  k_rar5_mcdc_bw_put_41   = 0x41U,
  k_rar5_mcdc_bw_put_42   = 0x42U,
  k_rar5_mcdc_bw_put_43   = 0x43U,
  k_rar5_mcdc_bw_put_44   = 0x44U,
  k_rar5_mcdc_bw_put_45   = 0x45U,
  k_rar5_mcdc_bw_put_46   = 0x46U,
  k_rar5_mcdc_bw_put_9    = 9U,
  k_rar5_mcdc_pos_5       = 5U,
  k_rar5_mcdc_s_tbl_7     = 7U,
  k_rar5_mcdc_sentinel_5a = 0x5AU,
  k_rar5_mcdc_val_20      = 20,
  k_rar5_mcdc_val_64      = 64,
} rar5_mcdc_uint8_const_t;

/**
 * @enum rar5_mcdc_uint16_const_t
 * @brief Named uint16_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint16_t {
  k_rar5_mcdc_ra8_rar5_filter_delta_600 = 600U,
  k_rar5_mcdc_val_427                   = 427,
  k_rar5_mcdc_val_700                   = 700,
} rar5_mcdc_uint16_const_t;

/* ---- direct MC/DC drivers for the promoted decode helpers --------------- */

/** @brief Emit one block header + body to @p pk; return bytes written. */
static size_t enc_block(const bitw_t* body, uint8_t* pk, bool has_tables, bool is_last)
{
  const size_t   bsz   = bw_bytes(body);
  const uint32_t bits  = bw_lastbits(body);
  const uint32_t bcnt  = (bsz < 256U) ? 1U : 2U;
  const uint8_t  flags = (uint8_t)((has_tables ? 0x80U : 0U) | (is_last ? 0x40U : 0U) |
                                   ((bcnt - 1U) << 3U) | (bits - 1U));
  size_t         p     = 0U;
  pk[p]                = flags;
  p += 1U;
  pk[p] = (uint8_t)(bsz & k_rar5_mcdc_bsz_ff);
  p += 1U;
  if (bcnt == 2U) {
    pk[p] = (uint8_t)((bsz >> 8U) & k_rar5_mcdc_bsz_ff);
    p += 1U;
  }
  uint8_t chk = (uint8_t)(k_rar5_mcdc_sentinel_5a ^ flags ^ (uint8_t)(bsz & k_rar5_mcdc_bsz_ff));
  if (bcnt == 2U) {
    chk ^= (uint8_t)((bsz >> 8U) & k_rar5_mcdc_bsz_ff);
  }
  pk[p] = chk;
  p += 1U;
  memcpy(&pk[p], body->buf, bsz);
  return p + bsz;
}

/** @brief Persistent backing so a bound ::s_state can pull bits after return. */
static uint8_t   s_bind_buf[16];
static buf_src_t s_bind_src; /**< Reader context over ::s_bind_buf.    */
static ra8_rar_t s_bind_rar; /**< Archive handle bound into ::s_state. */

/** @brief Reset ::s_state into a bit reader over @p bytes (for direct helper calls). */
static void bind_bits(const uint8_t* bytes, size_t len)
{
  memcpy(s_bind_buf, bytes, len);
  s_bind_src.data      = s_bind_buf;
  s_bind_src.len       = len;
  s_bind_rar.read      = buf_read;
  s_bind_rar.ctx       = &s_bind_src;
  s_bind_rar.size      = (uint64_t)len;
  s_bind_rar.first_off = 0U;
  s_bind_rar.version   = k_ra8_rar_ver_5;
  s_state              = (ra8_rar5_state_t){};
  s_state.rar          = &s_bind_rar;
  s_state.base         = 0U;
  s_state.packlen      = (uint64_t)len;
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
  static uint8_t s_out[k_rar5_mcdc_val_64];
  for (uint32_t i = 0U; i < sizeof(s_out); ++i) {
    s_out[i] = (uint8_t)(i + 1U);
  }
  /* dist == 0 || dist > pos. */
  size_t pos = k_rar5_mcdc_pos_5;
  TEST_ASSERT(ra8_rar5_copy_match(s_out, &pos, 100U, 3U, 2U)); /* (F,F) control */
  TEST_ASSERT_EQ(8U, pos);
  pos = k_rar5_mcdc_pos_5;
  TEST_ASSERT(!ra8_rar5_copy_match(s_out, &pos, 100U, 3U, 0U)); /* dist==0 (T,-) */
  TEST_ASSERT_EQ(5U, pos);
  pos = k_rar5_mcdc_pos_5;
  TEST_ASSERT(!ra8_rar5_copy_match(s_out, &pos, 100U, 3U, 100U)); /* dist>pos (F,T) */
  TEST_ASSERT_EQ(5U, pos);
  /* k < length && pos < unp. */
  pos = k_rar5_mcdc_pos_5;
  TEST_ASSERT(ra8_rar5_copy_match(s_out, &pos, 100U, 3U, 2U)); /* exit on k==length */
  TEST_ASSERT_EQ(8U, pos);
  pos = k_rar5_mcdc_pos_5;
  TEST_ASSERT(ra8_rar5_copy_match(s_out, &pos, 6U, 10U, 2U)); /* clamp on pos==unp */
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
  static uint8_t s_d[k_rar5_mcdc_val_700];
  for (uint32_t i = 0U; i < sizeof(s_d); ++i) {
    s_d[i] = (uint8_t)(i & k_rar5_mcdc_bsz_ff);
  }
  /* (F,F) control: a supported range transforms (differs from the raw input). */
  ra8_rar5_filter_delta(&s_state, s_d, 32U, 2U);
  bool changed = false;
  for (uint32_t i = 0U; i < 32U; ++i) {
    if (s_d[i] != (uint8_t)(i & k_rar5_mcdc_bsz_ff)) {
      changed = true;
    }
  }
  TEST_ASSERT(changed);
  /* (T,-) over-long range: left unchanged. */
  for (uint32_t i = 0U; i < sizeof(s_d); ++i) {
    s_d[i] = (uint8_t)(i & k_rar5_mcdc_bsz_ff);
  }
  ra8_rar5_filter_delta(&s_state, s_d, k_rar5_mcdc_ra8_rar5_filter_delta_600, 2U);
  TEST_ASSERT_EQ(0x57U, s_d[599]); /* 599 & 0xFF, unchanged */
  TEST_ASSERT_EQ(0x2CU, s_d[300]); /* 300 & 0xFF, unchanged */
  /* (F,T) zero channels: left unchanged. */
  ra8_rar5_filter_delta(&s_state, s_d, 32U, 0U);
  TEST_ASSERT_EQ(0x0AU, s_d[10]); /* 10 & 0xFF, unchanged */
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
  static uint8_t s_buf[k_rar5_mcdc_val_20];
  memset(s_buf, 0xFFU, sizeof(s_buf));
  /* c < count exit. */
  TEST_ASSERT_EQ(3U, ra8_rar5_fill_zeros(s_buf, 0U, 3U, 20U));
  TEST_ASSERT_EQ(0U, s_buf[0]);
  TEST_ASSERT_EQ(0U, s_buf[2]);
  TEST_ASSERT_EQ(0xFFU, s_buf[3]);
  /* i < max clamp exit. */
  TEST_ASSERT_EQ(20U, ra8_rar5_fill_zeros(s_buf, 18U, 5U, 20U));
  TEST_ASSERT_EQ(0U, s_buf[18]);
  TEST_ASSERT_EQ(0U, s_buf[19]);
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
  static uint8_t s_tbl[k_ra8_rar5_huff_total];
  static uint8_t s_idlebits[1] = {0x00U};
  uint32_t       idx           = 0U;

  memset(s_tbl, 0, sizeof(s_tbl));
  /* num=16, idx=0 -> !is_zero && *idx==0 (T,T) -> reject. */
  bind_bits(s_idlebits, sizeof(s_idlebits));
  idx = 0U;
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, ra8_rar5_apply_run(&s_state, s_tbl, &idx, 16U));
  /* num=16, idx=1 (prev set) -> !is_zero && *idx==0 (T,F) -> copy run appends. */
  bind_bits(s_idlebits, sizeof(s_idlebits));
  s_tbl[0] = k_rar5_mcdc_s_tbl_7;
  idx      = 1U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rar5_apply_run(&s_state, s_tbl, &idx, 16U));
  TEST_ASSERT_EQ(4U, idx);
  TEST_ASSERT_EQ(7U, s_tbl[3]);
  /* num=18, idx=0 -> is_zero (T,-), c < count exit (F,T). */
  bind_bits(s_idlebits, sizeof(s_idlebits));
  idx = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rar5_apply_run(&s_state, s_tbl, &idx, 18U));
  TEST_ASSERT_EQ(3U, idx);
  TEST_ASSERT_EQ(0U, s_tbl[0]);
  /* num=17 (copy long), idx=1 -> is_long (T,-). */
  bind_bits(s_idlebits, sizeof(s_idlebits));
  s_tbl[0] = 6U;
  idx      = 1U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rar5_apply_run(&s_state, s_tbl, &idx, 17U));
  TEST_ASSERT_EQ(12U, idx); /* 1 + (11 + 0) */
  /* num=19 (zero long), idx=0 -> is_long (F,T) / is_zero (F,T). */
  bind_bits(s_idlebits, sizeof(s_idlebits));
  idx = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rar5_apply_run(&s_state, s_tbl, &idx, 19U));
  TEST_ASSERT_EQ(11U, idx);
  /* num=16, idx=428 -> i < huff_total clamp (T,F). */
  bind_bits(s_idlebits, sizeof(s_idlebits));
  s_tbl[k_rar5_mcdc_val_427] = k_rar5_mcdc_pos_5;
  idx                        = (uint32_t)k_ra8_rar5_huff_total - 2U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rar5_apply_run(&s_state, s_tbl, &idx, 16U));
  TEST_ASSERT_EQ(k_ra8_rar5_huff_total, idx);
  TEST_ASSERT_EQ(5U, s_tbl[(uint32_t)k_ra8_rar5_huff_total - 1U]);
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
  static uint8_t s_b1buf[k_pk_cap];
  memset(s_b1buf, 0, sizeof(s_b1buf));
  bitw_t b1 = {.buf = s_b1buf, .cap = sizeof(s_b1buf)};
  enc_tables(&b1);
  bw_put(&b1, k_rar5_mcdc_bw_put_41, k_rar5_mcdc_bw_put_9);
  bw_put(&b1, k_rar5_mcdc_bw_put_42, k_rar5_mcdc_bw_put_9);
  bw_put(&b1, k_rar5_mcdc_bw_put_43, k_rar5_mcdc_bw_put_9);
  /* Block 2: literals "DEF" reusing block 1's tables, the last block. */
  static uint8_t s_b2buf[k_pk_cap];
  memset(s_b2buf, 0, sizeof(s_b2buf));
  bitw_t b2 = {.buf = s_b2buf, .cap = sizeof(s_b2buf)};
  bw_put(&b2, k_rar5_mcdc_bw_put_44, k_rar5_mcdc_bw_put_9);
  bw_put(&b2, k_rar5_mcdc_bw_put_45, k_rar5_mcdc_bw_put_9);
  bw_put(&b2, k_rar5_mcdc_bw_put_46, k_rar5_mcdc_bw_put_9);
  static uint8_t s_pk[k_pk_cap];
  memset(s_pk, 0, sizeof(s_pk));
  size_t p = enc_block(&b1, s_pk, true, false);
  p += enc_block(&b2, &s_pk[p], false, true);
  static const uint8_t exp[6] = {0x41U, 0x42U, 0x43U, 0x44U, 0x45U, 0x46U};
  decode_and_check(s_pk, p, exp, sizeof(exp));
  /* Truncated last block: a valid 4-literal stream decoded with a larger unpacked
   * size drains its (last) block before completing -> if (last) break -> reject. */
  static const uint8_t k_src[4] = {1U, 2U, 3U, 4U};
  static uint8_t       s_spk[k_pk_cap];
  memset(s_spk, 0, sizeof(s_spk));
  const size_t spklen = enc_all_literal(k_src, sizeof(k_src), s_spk, sizeof(s_spk));
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 decode_status(s_spk, spklen, (uint64_t)sizeof(k_src) + 2U));
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
  static uint8_t       s_pk[k_pk_cap];
  memset(s_pk, 0, sizeof(s_pk));
  const size_t pklen = enc_all_literal(k_src, sizeof(k_src), s_pk, sizeof(s_pk));

  buf_src_t      src = {.data = s_pk, .len = pklen};
  ra8_rar_t      rar = {.read    = buf_read,
                        .ctx     = &src,
                        .size    = (uint64_t)pklen,
                        .version = k_ra8_rar_ver_5};
  static uint8_t s_out[k_out_cap];
  size_t         got = 0U;

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rar5_decompress(nullptr, 0U, pklen, s_out, sizeof(s_out), 4U, &s_state, &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rar5_decompress(&rar, 0U, pklen, nullptr, sizeof(s_out), 4U, &s_state, &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rar5_decompress(&rar, 0U, pklen, s_out, sizeof(s_out), 4U, nullptr, &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rar5_decompress(&rar, 0U, pklen, s_out, sizeof(s_out), 4U, &s_state, nullptr));

  /* Non-RAR5 archive. */
  ra8_rar_t r4 = rar;
  r4.version   = k_ra8_rar_ver_4;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_rar5_decompress(&r4, 0U, pklen, s_out, sizeof(s_out), 4U, &s_state, &got));

  /* Output buffer too small. */
  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 ra8_rar5_decompress(&rar, 0U, pklen, s_out, 2U, 4U, &s_state, &got));

  /* Zero packed size. */
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 ra8_rar5_decompress(&rar, 0U, 0U, s_out, sizeof(s_out), 4U, &s_state, &got));

  /* Empty member (unp_size 0) decodes to nothing. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rar5_decompress(&rar, 0U, pklen, s_out, sizeof(s_out), 0U, &s_state, &got));
  TEST_ASSERT_EQ(0U, got);
  TEST_END("rar5: entry-point guards");
}

/* ---- CBR facade: compressed-page parity + RAR4 unsupported --------------- */

int32_t main(void)
{
  test_mcdc_copy_match();
  test_mcdc_filter_delta();
  test_mcdc_fill_zeros();
  test_mcdc_apply_run();
  test_mcdc_decode_stream_blocks();
  test_rar5_guards();
  (void)fprintf(stderr, "[OK  ] test_ra8_rar5_mcdc.c\n");
  return 0;
}
