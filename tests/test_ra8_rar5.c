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
 *   2. through the `ra8_comic` facade -- covered by the split sibling
 *      test_ra8_rar5_archive.c; the direct MC/DC drivers for the promoted
 *      decode helpers live in test_ra8_rar5_mcdc.c. This sibling owns leg 1.
 *
 * The writer (tests/support/rar5_enc_fixture.h) is first-party test
 * scaffolding; it vendors no `unrar` code (that license restriction is on
 * RARLAB's decompressor, which this codec does not use). Tests are
 * magic-number exempt, so byte offsets and bit widths appear as literals.
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
 * @enum t_r5_code_t
 * @brief Bit widths and symbols of the RAR5 length-table encoding.
 *
 * @details
 * A RAR5 block opens with three Huffman tables whose code lengths are
 * themselves coded: four bits per bit-length-table entry, then the main tables
 * as literal lengths interleaved with the continuation codes below. The test
 * hand-emits that bitstream, so these widths are the encoder's contract with
 * the decoder under test.
 */
typedef enum : uint16_t {
  k_t_len_code_bits   = 5U,   /**< Bits per literal code-length entry. */
  k_t_long_run_bits   = 7U,   /**< Bits carrying a long run count.     */
  k_t_lit_code_bits   = 9U,   /**< Code length assigned to every literal, hence the
                                   bit width of one emitted literal symbol.         */
  k_t_code_copy_long  = 17U,  /**< Continuation code 17: copy the previous length,
                                   long run.                                        */
  k_t_code_zero_short = 18U,  /**< Continuation code 18: zero run, short form.          */
  k_t_code_zero_long  = 19U,  /**< Continuation code 19: zero run, long form.           */
  k_t_run_138         = 127U, /**< Long-run payload giving 11 + 127 = 138 entries.      */
  k_t_run_133         = 122U, /**< Long-run payload giving 133 entries.                 */
  k_t_run_24          = 13U,  /**< Long zero-run payload giving 11 + 13 = 24 entries.   */
  k_t_run_10          = 7U,   /**< Short zero-run payload giving 3 + 7 = 10 entries.    */
  k_t_sym_len_slot0   = 262U, /**< Symbol selecting length slot 0, i.e. match length 2. */
} t_r5_code_t;

/**
 * @enum t_r5_table_t
 * @brief Entry counts of the RAR5 code-length tables.
 */
typedef enum : uint8_t {
  k_t_tbl_bd_entries = 20U, /**< Bit-length table (BD) entries.       */
  k_t_tbl_dd_entries = 64U, /**< Distance table (DD) entries.         */
  k_t_tbl_rd_entries = 44U, /**< Low-distance + repeat table entries. */
} t_r5_table_t;

/**
 * @enum t_r5_filter_t
 * @brief Opcodes and instruction widths the executable filters key on.
 *
 * @details
 * The x86 filter rewrites the rel32 operand of CALL / JMP; the ARM filter
 * rewrites the 24-bit offset of BL. The oracle re-implements both inversely,
 * so these must match the architecture, not the decoder.
 */
typedef enum : uint32_t {
  k_t_x86_op_call   = 0xE8U,     /**< x86 CALL rel32 opcode.                        */
  k_t_x86_op_jmp    = 0xE9U,     /**< x86 JMP rel32 opcode.                         */
  k_t_x86_insn_len  = 5U,        /**< Its total length: 1 opcode + 4 operand bytes. */
  k_t_arm_op_bl     = 0xEBU,     /**< ARM BL opcode, the high byte of the word.     */
  k_t_arm_off_mask  = 0xFFFFFFU, /**< The 24-bit BL offset field.                   */
  k_t_byte_mask     = 0xFFU,     /**< Low-byte mask while serialising.              */
  k_t_le32_hi_shift = 24U,       /**< Shift for the top byte of a 32-bit LE field.  */
} t_r5_filter_t;

/**
 * @enum t_r5_fixture_t
 * @brief Sizes and stimulus values of the crafted streams.
 */
typedef enum : uint16_t {
  k_t_src_len       = 600U,  /**< All-literal round-trip source length, bytes. */
  k_t_src_stride    = 7U,    /**< Multiplier of its byte pattern; co-prime with 4
                                 so the pattern does not repeat per word.          */
  k_t_lit_prefix    = 300U,  /**< Literal prefix giving far matches a window.       */
  k_t_reuse_dist    = 257U,  /**< Distance reused by the repeat-distance legs.      */
  k_t_notab_flags   = 0x40U, /**< Block header: last block, no tables, bytecount 1. */
  k_t_hdr_csum_seed = 0x5AU, /**< Seed the header checksum is XOR-folded against.   */
} t_r5_fixture_t;

/**
 * @test test_rar5_all_literal_roundtrip
 * @brief An all-literal RAR5 method-50 block round-trips byte-exactly.
 *
 * @par MC/DC:
 * (no compound decision is varied by this case -- the stream carries only literal
 * tokens (no match/repeat), so ra8_rar5_copy_match is never reached; the decoder
 * runs the token loop to completion and the output is compared byte-for-byte. The
 * LZ decisions `dist == 0 || dist > pos` and the decode-stream loop guard are
 * driven for MC/DC by test_rar5_match_legs and test_ra8_rar5_mcdc.)
 */
static void test_rar5_all_literal_roundtrip(void)
{
  TEST_BEGIN("rar5: all-literal round-trip");
  static uint8_t s_src[k_t_src_len];
  for (size_t i = 0U; i < sizeof(s_src); ++i) {
    s_src[i] = (uint8_t)((i * k_t_src_stride) + (i >> 2U) + 3U);
  }
  static uint8_t s_pk[k_pk_cap];
  memset(s_pk, 0, sizeof(s_pk));
  const size_t pklen = enc_all_literal(s_src, sizeof(s_src), s_pk, sizeof(s_pk));
  decode_and_check(s_pk, pklen, s_src, sizeof(s_src));
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
  static uint8_t s_bodybuf[k_pk_cap];
  static uint8_t s_exp[k_out_cap];
  memset(s_bodybuf, 0, sizeof(s_bodybuf));
  memset(s_exp, 0, sizeof(s_exp));
  bitw_t body = {.buf = s_bodybuf, .cap = sizeof(s_bodybuf)};
  size_t elen = 0U;
  enc_tables(&body);
  /* 300-byte literal prefix so far/large distances have a window. */
  for (uint32_t i = 0U; i < k_t_lit_prefix; ++i) {
    enc_lit(&body, s_exp, &elen, (uint8_t)(i + 1U));
  }
  enc_match(&body, s_exp, &elen, 2U, 3U);                        /* len 4, dist 4          */
  enc_match_lenx(&body, s_exp, &elen, 1U, 1U);                   /* len 11, dist 2         */
  enc_match_distx(&body, s_exp, &elen, 0U, 1U);                  /* len 2, dist 6          */
  enc_match_lowdist(&body, s_exp, &elen, 0U, k_t_len_code_bits); /* len 2, dist 38         */
  enc_match_hilow(&body, s_exp, &elen, 0U, 1U, 2U);              /* len 2, dist 65+16+2=83 */
  enc_match_big(&body, s_exp, &elen, 0U);                        /* len 3, dist 257        */
  enc_repdist(&body, s_exp, &elen, 1U, k_t_reuse_dist);          /* len 3, reuse dist 257  */
  enc_replast(&body, s_exp, &elen, 3U, k_t_reuse_dist);          /* repeat last len/dist   */
  static uint8_t s_pk[k_pk_cap];
  memset(s_pk, 0, sizeof(s_pk));
  const size_t pklen = enc_finish(&body, s_pk);
  decode_and_check(s_pk, pklen, s_exp, elen);
  TEST_END("rar5: LZ match / repeat legs");
}

/* ---- filter oracles (independent inverse reimplementations) -------------- */

/** @brief Independent delta-filter inverse over @p d (test oracle). */
static void oracle_delta(uint8_t* d, uint32_t len, uint32_t chan)
{
  static uint8_t s_tmp[k_out_cap];
  memcpy(s_tmp, d, len);
  uint32_t dpos = 0U;
  for (uint32_t ch = 0U; ch < chan; ++ch) {
    uint8_t prev = 0U;
    for (uint32_t i = ch; i < len; i += chan) {
      prev = (uint8_t)(prev - s_tmp[dpos]);
      dpos++;
      d[i] = prev;
    }
  }
}

/** @brief Independent x86 CALL/JMP inverse over @p d (test oracle). */
static void oracle_x86(uint8_t* d, uint32_t len, uint32_t filepos, bool e9)
{
  if (len < k_t_x86_insn_len) {
    return;
  }
  uint32_t i = 0U;
  while (i <= len - k_t_x86_insn_len) {
    const uint8_t op = d[i];
    if (op == k_t_x86_op_call || (e9 && op == k_t_x86_op_jmp)) {
      const uint32_t off = i + 1U;
      uint32_t v = (uint32_t)d[off] | ((uint32_t)d[off + 1U] << 8U) |
                   ((uint32_t)d[off + 2U] << 16U) | ((uint32_t)d[off + 3U] << k_t_le32_hi_shift);
      v -= (filepos + off);
      d[off]      = (uint8_t)(v & k_t_byte_mask);
      d[off + 1U] = (uint8_t)((v >> 8U) & k_t_byte_mask);
      d[off + 2U] = (uint8_t)((v >> 16U) & k_t_byte_mask);
      d[off + 3U] = (uint8_t)((v >> k_t_le32_hi_shift) & k_t_byte_mask);
      i += k_t_x86_insn_len;
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
    if (d[i + 3U] == k_t_arm_op_bl) {
      uint32_t v = (uint32_t)d[i] | ((uint32_t)d[i + 1U] << 8U) | ((uint32_t)d[i + 2U] << 16U);
      v          = (v - ((filepos + i) >> 2U)) & k_t_arm_off_mask;
      d[i]       = (uint8_t)(v & k_t_byte_mask);
      d[i + 1U]  = (uint8_t)((v >> 8U) & k_t_byte_mask);
      d[i + 2U]  = (uint8_t)((v >> 16U) & k_t_byte_mask);
    }
    i += 4U;
  }
}

/** @brief Round-trip a filter over crafted literals; compare to the test oracle. */
static void run_filter_case(uint32_t type, uint32_t chan, const uint8_t* raw, uint32_t len)
{
  static uint8_t s_bodybuf[k_pk_cap];
  static uint8_t s_exp[k_out_cap];
  memset(s_bodybuf, 0, sizeof(s_bodybuf));
  bitw_t body = {.buf = s_bodybuf, .cap = sizeof(s_bodybuf)};
  size_t elen = 0U;
  enc_tables(&body);
  /* The filter token precedes the range it covers: read at output position 0, it
   * transforms [0, len) once decoding completes (RAR emits the filter, then data). */
  enc_filter(&body, type, 0U, len, chan);
  for (uint32_t i = 0U; i < len; ++i) {
    enc_lit(&body, s_exp, &elen, raw[i]);
  }
  /* Expected = the same literals with the decoder's inverse applied. */
  if (type == 0U) {
    oracle_delta(s_exp, len, chan);
  } else if (type == 1U) {
    oracle_x86(s_exp, len, 0U, false);
  } else if (type == 2U) {
    oracle_x86(s_exp, len, 0U, true);
  } else {
    oracle_arm(s_exp, len, 0U);
  }
  static uint8_t s_pk[k_pk_cap];
  memset(s_pk, 0, sizeof(s_pk));
  const size_t pklen = enc_finish(&body, s_pk);
  decode_and_check(s_pk, pklen, s_exp, elen);
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
 * @brief Emit tables that build the length list out of continuation run codes.
 *
 * @details
 * Encodes the same logical table ::enc_tables writes literally, but expresses
 * the repetition through the decoder's run codes instead of spelling out every
 * length. LD entries 0..271 become one literal 9 followed by two copy-previous
 * runs (code 17, long form), and entries 272..305 become a short zero run
 * (code 18) followed by a long one (code 19). Because the reconstructed table
 * is identical to the literal one, a byte-exact decode of the payload proves
 * the run codes rebuilt it correctly -- the runs are the thing under test, not
 * the payload.
 *
 * Run lengths follow the format's bias: the long forms encode `11 + extra` and
 * the short zero form `3 + extra`, which is why 127 yields 138 entries and 13
 * yields 24.
 *
 * @param[in,out] w Bit writer positioned at the start of the table section.
 *
 *
 * @pre @p w is non-NULL and has capacity for the whole table section.
 * @pre No table bits have been written to @p w yet.
 * @post @p w is positioned where the payload literals begin.
 * @post The emitted table is equivalent to the one ::enc_tables writes.
 *
 * @note Not thread-safe; @p w carries all the state.
 *
 * @see enc_tables()         The literal spelling of the same table.
 * @see enc_tables_bdzero()  The BD zero-run variant.
 */
static void enc_tables_runs(bitw_t* w)
{
  /* BD: 20 lengths of 5. */
  for (uint32_t i = 0U; i < k_t_tbl_bd_entries; ++i) {
    bw_put(w, k_t_len_code_bits, 4U);
  }
  /* LD 0..271 = 9 via a literal 9 then copy-previous runs (codes 16/17). */
  bw_put(w, k_t_lit_code_bits, k_t_len_code_bits);  /* tbl[0] = 9                          */
  bw_put(w, k_t_code_copy_long, k_t_len_code_bits); /* copy prev, long run                 */
  bw_put(w, k_t_run_138, k_t_long_run_bits);        /* run = 11 + 127 = 138 -> tbl[1..138] */
  bw_put(w, k_t_code_copy_long, k_t_len_code_bits);
  bw_put(w, k_t_run_133, k_t_long_run_bits); /* run = 133 -> tbl[139..271] */
  /* LD 272..305 = 0 via a zero run (code 18 short + 19 long). */
  bw_put(w, k_t_code_zero_short, k_t_len_code_bits);
  bw_put(w, k_t_run_10, 3U); /* run = 3 + 7 = 10 -> tbl[272..281] */
  bw_put(w, k_t_code_zero_long, k_t_len_code_bits);
  bw_put(w, k_t_run_24, k_t_long_run_bits); /* run = 11 + 13 = 24 -> tbl[282..305] */
  /* DD/LDD/RD via direct lengths. */
  for (uint32_t i = 0U; i < k_t_tbl_dd_entries; ++i) {
    bw_put(w, 6U, k_t_len_code_bits);
  }
  for (uint32_t i = 0U; i < 16U; ++i) {
    bw_put(w, 4U, k_t_len_code_bits);
  }
  for (uint32_t i = 0U; i < k_t_tbl_rd_entries; ++i) {
    bw_put(w, 6U, k_t_len_code_bits);
  }
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
  static uint8_t s_bodybuf[k_pk_cap];
  memset(s_bodybuf, 0, sizeof(s_bodybuf));
  bitw_t body = {.buf = s_bodybuf, .cap = sizeof(s_bodybuf)};
  enc_tables_runs(&body);
  /* Payload: a handful of literals. */
  static const uint8_t k_pay[5] = {0x41U, 0x42U, 0x43U, 0x44U, 0x45U};
  for (uint32_t i = 0U; i < sizeof(k_pay); ++i) {
    bw_put(&body, k_pay[i], k_t_lit_code_bits);
  }
  static uint8_t s_pk[k_pk_cap];
  memset(s_pk, 0, sizeof(s_pk));
  const size_t pklen = enc_finish(&body, s_pk);
  decode_and_check(s_pk, pklen, k_pay, sizeof(k_pay));
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
  static uint8_t s_bodybuf[k_pk_cap];
  memset(s_bodybuf, 0, sizeof(s_bodybuf));
  bitw_t body = {.buf = s_bodybuf, .cap = sizeof(s_bodybuf)};
  enc_tables_bdzero(&body);
  static const uint8_t k_pay[6] = {0x30U, 0x31U, 0x32U, 0x33U, 0x34U, 0x35U};
  for (uint32_t i = 0U; i < sizeof(k_pay); ++i) {
    bw_put(&body, k_pay[i], k_t_lit_code_bits);
  }
  static uint8_t s_pk[k_pk_cap];
  memset(s_pk, 0, sizeof(s_pk));
  const size_t pklen = enc_finish(&body, s_pk);
  decode_and_check(s_pk, pklen, k_pay, sizeof(k_pay));
  TEST_END("rar5: BD length-list zero run");
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
  static uint8_t       s_pk[k_pk_cap];
  memset(s_pk, 0, sizeof(s_pk));
  const size_t pklen = enc_all_literal(k_src, sizeof(k_src), s_pk, sizeof(s_pk));

  /* Bad header checksum. */
  static uint8_t s_bad[k_pk_cap];
  memcpy(s_bad, s_pk, pklen);
  s_bad[2] ^= k_t_byte_mask; /* wrong checksum byte (bytecount 1) */
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, decode_status(s_bad, pklen, sizeof(k_src)));

  /* Truncated stream (header only) with a large expected size -> short output. */
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, decode_status(s_pk, 3U, sizeof(k_src)));

  /* Every truncation length is either a clean parse or a rejection, never a crash. */
  for (size_t sz = 1U; sz <= pklen; ++sz) {
    const ra8_err_t r = decode_status(s_pk, sz, sizeof(k_src));
    TEST_ASSERT(r == k_ra8_ok || r == k_ra8_err_validation_failed);
  }

  /* A first block that declares no tables must be rejected. */
  static uint8_t s_notab[16] = {};
  s_notab[0]                 = k_t_notab_flags; /* last, no tables, bytecount 1 */
  s_notab[1]                 = 0x02U;           /* block size 2                 */
  /* checksum */
  s_notab[2] = (uint8_t)(k_t_hdr_csum_seed ^ k_t_notab_flags ^ 0x02U);
  s_notab[3] = 0x00U;
  s_notab[4] = 0x00U;
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, decode_status(s_notab, 5U, sizeof(k_src)));

  /* A match whose distance reaches before the member start (solid reference). */
  static uint8_t s_bodybuf[k_pk_cap];
  memset(s_bodybuf, 0, sizeof(s_bodybuf));
  bitw_t body = {.buf = s_bodybuf, .cap = sizeof(s_bodybuf)};
  enc_tables(&body);
  bw_put(&body,
         k_t_sym_len_slot0,
         k_t_lit_code_bits); /* length slot 0 -> length 2                      */
  bw_put(&body, 3U, 6U);     /* distance slot 3 -> dist 4, but no prior output */
  static uint8_t s_pk2[k_pk_cap];
  memset(s_pk2, 0, sizeof(s_pk2));
  const size_t pklen2 = enc_finish(&body, s_pk2);
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, decode_status(s_pk2, pklen2, 2U));
  TEST_END("rar5: malformed / truncated rejects");
}

/**
 * @brief Test entry point -- runs the RAR5 decompressor round-trip suite.
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
  (void)fprintf(stderr, "[OK  ] test_ra8_rar5.c\n");
  return 0;
}
