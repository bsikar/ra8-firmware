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

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_rar.h"
#include "ra8_rar5.h"
#include "ra8_rar5_internal.h"
#include "support/rar5_enc_fixture.h"
#include "unity_minimal.h"

/** @brief Fill byte for the all-ones fixture buffer. */
typedef enum : uint8_t {
  k_rar5_mcdc_fill_ones = 0xFFU, /**< All-ones: decodes as an invalid vint. */
} rar5_mcdc_fill_t;

/**
 * @enum t_r5_hdr_t
 * @brief RAR5 block-header fields the MC/DC vectors vary.
 */
typedef enum : uint8_t {
  k_t_byte_mask     = 0xFFU, /**< Low-byte mask while serialising the block size,
                              and the modulus of the ramp fill pattern.      */
  k_t_hdr_csum_seed = 0x5AU, /**< Seed the header checksum is XOR-folded against. */
} t_r5_hdr_t;

/**
 * @enum t_r5_stream_t
 * @brief Literal payloads and code widths of the crafted bitstreams.
 *
 * @details
 * Every literal carries a 9-bit code, so `k_t_lit_code_bits` is both the
 * table's assigned length and the width of one emitted symbol. The payload
 * bytes are ASCII 'A'..'F' -- arbitrary, but distinct and printable, so a
 * failing dump is readable.
 */
typedef enum : uint16_t {
  k_t_lit_code_bits = 9U,    /**< Bits per emitted literal symbol.   */
  k_t_blk1_b0       = 0x41U, /**< Block 1 payload byte 0, ASCII 'A'. */
  k_t_blk1_b1       = 0x42U, /**< Block 1 payload byte 1, ASCII 'B'. */
  k_t_blk1_b2       = 0x43U, /**< Block 1 payload byte 2, ASCII 'C'. */
  k_t_blk2_b0       = 0x44U, /**< Block 2 payload byte 0, ASCII 'D'. */
  k_t_blk2_b1       = 0x45U, /**< Block 2 payload byte 1, ASCII 'E'. */
  k_t_blk2_b2       = 0x46U, /**< Block 2 payload byte 2, ASCII 'F'. */
  k_t_tbl_len_short = 5U,    /**< A short code length written into the table;
                                 also the parse position the guards restart from. */
  k_t_tbl_len_long  = 7U,    /**< The longer code length at table slot 0. */
  k_t_tbl_slot_last = 427U,  /**< Final table slot, where a truncated table
                                 must still leave a defined length.       */
} t_r5_stream_t;

/**
 * @enum t_r5_buf_t
 * @brief Fixture buffer capacities and filter run lengths.
 */
typedef enum : uint16_t {
  k_t_out_cap   = 64U,  /**< Decoder output scratch, bytes. */
  k_t_small_cap = 20U,  /**< Deliberately small output buffer for the
                             out-of-room arm, bytes.                         */
  k_t_delta_cap = 700U, /**< Delta-filter working buffer, bytes.         */
  k_t_delta_len = 600U, /**< Bytes the delta filter is asked to process. */
} t_r5_buf_t;

/* ---- direct MC/DC drivers for the promoted decode helpers --------------- */

/**
 * @brief Emit one complete RAR5 block header and body.
 * @details Derives the encoded body length and final-byte bit count, selects
 * the one- or two-byte length form, folds the synthetic header checksum, and
 * appends the body bytes after the header.
 * @param[in] body Finalized bit-writer whose bytes form the compressed body.
 * @param[out] pk Destination receiving the header followed by the body.
 * @param[in] has_tables Whether the block introduces new Huffman tables.
 * @param[in] is_last Whether the block terminates the compressed stream.
 * @return Total number of bytes written to @p pk.
 * @retval 0 Never returned for a valid finalized fixture body.
 * @pre @p body and @p pk are non-NULL.
 * @pre @p pk has room for the body plus the maximum synthetic header.
 * @post The returned prefix of @p pk contains exactly one encoded block.
 * @post Neither @p body nor its backing bytes are modified.
 * @note This fixture checksum is intentionally the value expected by the test
 * decoder rather than a general-purpose archive checksum implementation.
 * @since 0.1.0
 */
RA8_INTERNAL static size_t
internal_enc_block(const bitw_t* body, uint8_t* pk, bool has_tables, bool is_last)
{
  const size_t   bsz   = bw_bytes(body);
  const uint32_t bits  = bw_lastbits(body);
  const uint32_t bcnt  = (bsz < 256U) ? 1U : 2U;
  const uint8_t  flags = (uint8_t)((has_tables ? 0x80U : 0U) | (is_last ? 0x40U : 0U) |
                                   ((bcnt - 1U) << 3U) | (bits - 1U));
  size_t         p     = 0U;
  pk[p]                = flags;
  p += 1U;
  pk[p] = (uint8_t)(bsz & k_t_byte_mask);
  p += 1U;
  if (bcnt == 2U) {
    pk[p] = (uint8_t)((bsz >> 8U) & k_t_byte_mask);
    p += 1U;
  }
  uint8_t chk = (uint8_t)(k_t_hdr_csum_seed ^ flags ^ (uint8_t)(bsz & k_t_byte_mask));
  if (bcnt == 2U) {
    chk ^= (uint8_t)((bsz >> 8U) & k_t_byte_mask);
  }
  pk[p] = chk;
  p += 1U;
  memcpy(&pk[p], body->buf, bsz);
  return p + bsz;
}

/** @brief Persistent backing so a bound ::s_state can pull bits after return.
 */
static uint8_t   s_bind_buf[16];
static buf_src_t s_bind_src; /**< Reader context over ::s_bind_buf.    */
static ra8_rar_t s_bind_rar; /**< Archive handle bound into ::s_state. */

/**
 * @brief Reset the shared decoder state over a copied bit-source fixture.
 * @details Copies the caller bytes into persistent storage, binds the buffered
 * read callback and archive descriptor, then initializes ::s_state for direct
 * promoted-helper calls that continue reading after this function returns.
 * @param[in] bytes Encoded bit source to bind.
 * @param[in] len Number of bytes to copy from @p bytes.
 * @pre @p bytes is readable for @p len bytes.
 * @pre @p len does not exceed the capacity of ::s_bind_buf.
 * @post ::s_state references the persistent ::s_bind_rar archive at offset
 * zero.
 * @post The copied source length and archive size both equal @p len.
 * @note Mutates shared fixture state and is therefore not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_bind_bits(const uint8_t* bytes, size_t len)
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
 * @test internal_test_mcdc_copy_match
 * @brief Both guard/clamp compound decisions of the LZ match copier.
 *
 * @par MC/DC:
 * libs/ra8_comic/src/ra8_rar5.c@priv_rar5_copy_match:
 * - `dist == 0 || dist > (uint64_t)pos` (2 conditions):
 *   - dist=2,  pos=5 -> (false, false) control: the copy proceeds.
 *   - dist=0,  pos=5 -> (true, -)  varies dist == 0 only -> reject.
 *   - dist=100,pos=5 -> (false, true) varies dist > pos only -> reject.
 *   Vectors 1+2 prove `dist == 0` independently rejects; 1+3 prove `dist > pos`
 *   does. N+1 = 3 vectors for N=2.
 * - `k < length && pos < unp` (2 conditions):
 *   - length=3, unp=100 from pos=5 -> the loop runs (true, true) x3 then exits
 * on k == length -> (false, true), proving `k < length` ends the copy.
 *   - length=10, unp=6 from pos=5 -> the loop runs (true, true) then exits on
 *     pos == unp -> (true, false), proving `pos < unp` clamps the copy.
 * @details Seeds a deterministic output history, drives every guard pair, and
 * verifies both the successful copy extent and the unchanged rejection offset.
 * @pre The promoted match-copy helper is available to the test translation
 * unit.
 * @pre The static output fixture is large enough for every requested copy.
 * @post Valid copies advance the position to the expected bounded endpoint.
 * @post Invalid distances leave the supplied position unchanged.
 * @note The vectors intentionally inspect the private helper directly.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_copy_match(void)
{
  TEST_BEGIN("rar5: priv_rar5_copy_match MC/DC");
  static uint8_t output[k_t_out_cap];
  for (uint32_t i = 0U; i < sizeof(output); ++i) {
    output[i] = (uint8_t)(i + 1U);
  }
  /* dist == 0 || dist > pos. */
  size_t pos = k_t_tbl_len_short;
  TEST_ASSERT(priv_rar5_copy_match(output, &pos, 100U, 3U, 2U)); /* (F,F) control */
  TEST_ASSERT_EQ(8U, pos);
  pos = k_t_tbl_len_short;
  TEST_ASSERT(!priv_rar5_copy_match(output, &pos, 100U, 3U, 0U)); /* dist==0 (T,-) */
  TEST_ASSERT_EQ(5U, pos);
  pos = k_t_tbl_len_short;
  TEST_ASSERT(!priv_rar5_copy_match(output, &pos, 100U, 3U, 100U)); /* dist>pos (F,T) */
  TEST_ASSERT_EQ(5U, pos);
  /* k < length && pos < unp. */
  pos = k_t_tbl_len_short;
  TEST_ASSERT(priv_rar5_copy_match(output, &pos, 100U, 3U, 2U)); /* exit on k==length */
  TEST_ASSERT_EQ(8U, pos);
  pos = k_t_tbl_len_short;
  TEST_ASSERT(priv_rar5_copy_match(output, &pos, 6U, 10U, 2U)); /* clamp on pos==unp */
  TEST_ASSERT_EQ(6U, pos);
  TEST_END("rar5: priv_rar5_copy_match MC/DC");
}

/**
 * @test internal_test_mcdc_filter_delta
 * @brief The over-long / zero-channel guard of the delta filter.
 *
 * @par MC/DC:
 * libs/ra8_comic/src/ra8_rar5.c@priv_rar5_filter_delta
 * `len > k_ra8_rar5_delta_scratch || channels == 0` (2 conditions):
 * - len=32,  channels=2 -> (false, false) control: the transform runs.
 * - len=600, channels=2 -> (true, -)  varies len only -> range left unchanged.
 * - len=32,  channels=0 -> (false, true) varies channels only -> unchanged.
 * The descriptor parser only ever yields channels >= 1, so the zero-channel leg
 * is reachable only by this direct call. N+1 = 3 vectors for N=2.
 * @details Initializes a byte ramp, proves the supported transform changes it,
 * then verifies both rejected inputs preserve independently sampled bytes.
 * @pre The promoted delta-filter helper is available to this test.
 * @pre The fixture capacity exceeds ::k_t_delta_len.
 * @post The supported short range differs from its source ramp.
 * @post Overlong and zero-channel vectors preserve the inspected bytes.
 * @note Resets shared decoder state before exercising the filter workspace.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_filter_delta(void)
{
  TEST_BEGIN("rar5: priv_rar5_filter_delta MC/DC");
  s_state = (ra8_rar5_state_t){};
  static uint8_t delta[k_t_delta_cap];
  for (uint32_t i = 0U; i < sizeof(delta); ++i) {
    delta[i] = (uint8_t)(i & k_t_byte_mask);
  }
  /* (F,F) control: a supported range transforms (differs from the raw input).
   */
  priv_rar5_filter_delta(&s_state, delta, 32U, 2U);
  bool changed = false;
  for (uint32_t i = 0U; i < 32U; ++i) {
    if (delta[i] != (uint8_t)(i & k_t_byte_mask)) {
      changed = true;
    }
  }
  TEST_ASSERT(changed);
  /* (T,-) over-long range: left unchanged. */
  for (uint32_t i = 0U; i < sizeof(delta); ++i) {
    delta[i] = (uint8_t)(i & k_t_byte_mask);
  }
  priv_rar5_filter_delta(&s_state, delta, k_t_delta_len, 2U);
  TEST_ASSERT_EQ(0x57U, delta[599]); /* 599 & 0xFF, unchanged */
  TEST_ASSERT_EQ(0x2CU, delta[300]); /* 300 & 0xFF, unchanged */
  /* (F,T) zero channels: left unchanged. */
  priv_rar5_filter_delta(&s_state, delta, 32U, 0U);
  TEST_ASSERT_EQ(0x0AU, delta[10]); /* 10 & 0xFF, unchanged */
  TEST_END("rar5: priv_rar5_filter_delta MC/DC");
}

/**
 * @test internal_test_mcdc_fill_zeros
 * @brief Both loop-guard legs of the bounded zero-length appender.
 *
 * @par MC/DC:
 * libs/ra8_comic/src/ra8_rar5_tables.c@priv_rar5_fill_zeros
 * `c < count && i < max` (2 conditions):
 * - start=0, count=3, max=20 -> the loop runs (true, true) x3 then exits on
 *   c == count -> (false, true), proving `c < count` ends the fill.
 * - start=18, count=5, max=20 -> the loop runs (true, true) x2 then exits on
 *   i == max -> (true, false), proving `i < max` clamps the fill.
 * @details Fills a sentinel array, exercises count-limited and capacity-limited
 * runs, and checks the exact written and untouched boundary bytes.
 * @pre The promoted zero-fill helper is linked into the test target.
 * @pre ::k_t_small_cap matches the maximum index supplied by the vectors.
 * @post The count-limited case writes exactly three zero entries.
 * @post The capacity-limited case stops at the final valid array element.
 * @note Sentinel bytes make an off-by-one write observable.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_fill_zeros(void)
{
  TEST_BEGIN("rar5: priv_rar5_fill_zeros MC/DC");
  static uint8_t buffer[k_t_small_cap];
  memset(buffer, k_rar5_mcdc_fill_ones, sizeof(buffer));
  /* c < count exit. */
  TEST_ASSERT_EQ(3U, priv_rar5_fill_zeros(buffer, 0U, 3U, 20U));
  TEST_ASSERT_EQ(0U, buffer[0]);
  TEST_ASSERT_EQ(0U, buffer[2]);
  TEST_ASSERT_EQ(0xFFU, buffer[3]);
  /* i < max clamp exit. */
  TEST_ASSERT_EQ(20U, priv_rar5_fill_zeros(buffer, 18U, 5U, 20U));
  TEST_ASSERT_EQ(0U, buffer[18]);
  TEST_ASSERT_EQ(0U, buffer[19]);
  TEST_END("rar5: priv_rar5_fill_zeros MC/DC");
}

/**
 * @test internal_test_mcdc_apply_run
 * @brief Every compound decision of the length-table run appender.
 *
 * @par MC/DC:
 * libs/ra8_comic/src/ra8_rar5_tables.c@priv_rar5_apply_run, four decisions:
 * - `num == 17 || num == 19` (is_long): num=16/18 -> (F,F); num=17 -> (T,-);
 *   num=19 -> (F,T).
 * - `num == 18 || num == 19` (is_zero): num=16/17 -> (F,F); num=18 -> (T,-);
 *   num=19 -> (F,T).
 * - `!is_zero && *idx == 0`: num=16,idx=0 -> (T,T) reject; num=16,idx=1 ->
 * (T,F) append; num=18,idx=0 -> (F,-) via is_zero true.
 * - `c < count && i < k_ra8_rar5_huff_total`: num=18,idx=0 -> runs then exits
 * on c == count (F,T); num=16,idx=428 -> runs then exits on i == huff_total
 * (T,F). All run-count reads pull from a bound bit source; N+1 vectors per
 * decision.
 * @details Rebinds a zero bit source for each vector and verifies the exact
 * table length, copied value, zero run, and capacity-clamped endpoint.
 * @pre The promoted table-run helper is available to the test target.
 * @pre ::internal_bind_bits has persistent storage for the one-byte source.
 * @post Every successful run advances the table index to its expected value.
 * @post The invalid copy-at-index-zero case returns validation failure.
 * @note Uses shared decoder and table fixtures, so the test runs serially.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_apply_run(void)
{
  TEST_BEGIN("rar5: priv_rar5_apply_run MC/DC");
  static uint8_t table[k_ra8_rar5_huff_total];
  static uint8_t idle_bits[1] = {0x00U};
  uint32_t       idx          = 0U;

  memset(table, 0, sizeof(table));
  /* num=16, idx=0 -> !is_zero && *idx==0 (T,T) -> reject. */
  internal_bind_bits(idle_bits, sizeof(idle_bits));
  idx = 0U;
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, priv_rar5_apply_run(&s_state, table, &idx, 16U));
  /* num=16, idx=1 (prev set) -> !is_zero && *idx==0 (T,F) -> copy run appends.
   */
  internal_bind_bits(idle_bits, sizeof(idle_bits));
  table[0] = k_t_tbl_len_long;
  idx      = 1U;
  TEST_ASSERT_EQ(k_ra8_ok, priv_rar5_apply_run(&s_state, table, &idx, 16U));
  TEST_ASSERT_EQ(4U, idx);
  TEST_ASSERT_EQ(7U, table[3]);
  /* num=18, idx=0 -> is_zero (T,-), c < count exit (F,T). */
  internal_bind_bits(idle_bits, sizeof(idle_bits));
  idx = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, priv_rar5_apply_run(&s_state, table, &idx, 18U));
  TEST_ASSERT_EQ(3U, idx);
  TEST_ASSERT_EQ(0U, table[0]);
  /* num=17 (copy long), idx=1 -> is_long (T,-). */
  internal_bind_bits(idle_bits, sizeof(idle_bits));
  table[0] = 6U;
  idx      = 1U;
  TEST_ASSERT_EQ(k_ra8_ok, priv_rar5_apply_run(&s_state, table, &idx, 17U));
  TEST_ASSERT_EQ(12U, idx); /* 1 + (11 + 0) */
  /* num=19 (zero long), idx=0 -> is_long (F,T) / is_zero (F,T). */
  internal_bind_bits(idle_bits, sizeof(idle_bits));
  idx = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, priv_rar5_apply_run(&s_state, table, &idx, 19U));
  TEST_ASSERT_EQ(11U, idx);
  /* num=16, idx=428 -> i < huff_total clamp (T,F). */
  internal_bind_bits(idle_bits, sizeof(idle_bits));
  table[k_t_tbl_slot_last] = k_t_tbl_len_short;
  idx                      = (uint32_t)k_ra8_rar5_huff_total - 2U;
  TEST_ASSERT_EQ(k_ra8_ok, priv_rar5_apply_run(&s_state, table, &idx, 16U));
  TEST_ASSERT_EQ(k_ra8_rar5_huff_total, idx);
  TEST_ASSERT_EQ(5U, table[(uint32_t)k_ra8_rar5_huff_total - 1U]);
  TEST_END("rar5: priv_rar5_apply_run MC/DC");
}

/**
 * @test internal_test_mcdc_decode_stream_blocks
 * @brief The multi-block continuation and truncated-last-block branches.
 *
 * @par MC/DC:
 * (no compound decisions under test -- exercises the single-condition
 * `st->consumed >= block_end` and `if (last)` branches of
 * libs/ra8_comic/src/ra8_rar5.c@internal_decode_stream: a two-block stream
 * crosses a non-last block boundary (consumed >= block_end true, last false ->
 * opens the next block), and a single last block decoded with an over-large
 * unpacked size reaches the last-block boundary with output incomplete (last
 * true -> break -> short-output rejection). The loop's compound guard is
 * covered by internal_test_rar5_match_legs and internal_test_rar5_malformed.)
 * @details Builds two independently encoded blocks to prove table reuse and
 * continuation, then presents a valid short stream with an inflated output
 * size.
 * @pre The fixture encoders fit within their statically bounded buffers.
 * @pre The decoder workspace is initialized by the shared test support.
 * @post The two-block stream decodes byte-exactly to ASCII `ABCDEF`.
 * @post The truncated-last case returns validation failure.
 * @note The packed fixtures remain local to this test invocation.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_decode_stream_blocks(void)
{
  TEST_BEGIN("rar5: multi-block + truncated-last stream");
  /* Block 1: tables + literals "ABC", not the last block. */
  static uint8_t block1_buffer[k_pk_cap];
  memset(block1_buffer, 0, sizeof(block1_buffer));
  bitw_t b1 = {.buf = block1_buffer, .cap = sizeof(block1_buffer)};
  enc_tables(&b1);
  bw_put(&b1, k_t_blk1_b0, k_t_lit_code_bits);
  bw_put(&b1, k_t_blk1_b1, k_t_lit_code_bits);
  bw_put(&b1, k_t_blk1_b2, k_t_lit_code_bits);
  /* Block 2: literals "DEF" reusing block 1's tables, the last block. */
  static uint8_t block2_buffer[k_pk_cap];
  memset(block2_buffer, 0, sizeof(block2_buffer));
  bitw_t b2 = {.buf = block2_buffer, .cap = sizeof(block2_buffer)};
  bw_put(&b2, k_t_blk2_b0, k_t_lit_code_bits);
  bw_put(&b2, k_t_blk2_b1, k_t_lit_code_bits);
  bw_put(&b2, k_t_blk2_b2, k_t_lit_code_bits);
  static uint8_t packed[k_pk_cap];
  memset(packed, 0, sizeof(packed));
  size_t p = internal_enc_block(&b1, packed, true, false);
  p += internal_enc_block(&b2, &packed[p], false, true);
  static const uint8_t exp[6] = {0x41U, 0x42U, 0x43U, 0x44U, 0x45U, 0x46U};
  decode_and_check(packed, p, exp, sizeof(exp));
  /* Truncated last block: a valid 4-literal stream decoded with a larger
   * unpacked size drains its (last) block before completing -> if (last) break
   * -> reject. */
  static const uint8_t k_src[4] = {1U, 2U, 3U, 4U};
  static uint8_t       short_packed[k_pk_cap];
  memset(short_packed, 0, sizeof(short_packed));
  const size_t spklen = enc_all_literal(k_src, sizeof(k_src), short_packed, sizeof(short_packed));
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 decode_status(short_packed, spklen, (uint64_t)sizeof(k_src) + 2U));
  TEST_END("rar5: multi-block + truncated-last stream");
}

/**
 * @test internal_test_rar5_guards
 * @brief `ra8_rar5_decompress` rejects NULL args, a non-RAR5 archive, an
 *        undersized buffer, a zero packed size, and decodes an empty member.
 *
 * @par MC/DC:
 * (no compound decisions under test -- each guard is an independent
 * single-condition early return driven one at a time.)
 * @details Builds one valid literal member as a control, varies each invalid
 * argument or archive property independently, and concludes with the empty
 * case.
 * @pre The fixture encoder produces a nonempty valid RAR5 packed stream.
 * @pre The output and decoder workspaces satisfy the successful-call contract.
 * @post Each invalid vector reports the expected precise error code.
 * @post The empty-member control succeeds and reports zero decoded bytes.
 * @note The archive callback reads from the local immutable packed fixture.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_rar5_guards(void)
{
  TEST_BEGIN("rar5: entry-point guards");
  static const uint8_t k_src[4] = {9U, 8U, 7U, 6U};
  static uint8_t       packed[k_pk_cap];
  memset(packed, 0, sizeof(packed));
  const size_t pklen = enc_all_literal(k_src, sizeof(k_src), packed, sizeof(packed));

  buf_src_t      src = {.data = packed, .len = pklen};
  ra8_rar_t      rar = {.read    = buf_read,
                        .ctx     = &src,
                        .size    = (uint64_t)pklen,
                        .version = k_ra8_rar_ver_5};
  static uint8_t output[k_out_cap];
  size_t         got = 0U;

  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_rar5_decompress(nullptr, 0U, pklen, output, sizeof(output), 4U, &s_state, &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rar5_decompress(&rar, 0U, pklen, nullptr, sizeof(output), 4U, &s_state, &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rar5_decompress(&rar, 0U, pklen, output, sizeof(output), 4U, nullptr, &got));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_rar5_decompress(&rar, 0U, pklen, output, sizeof(output), 4U, &s_state, nullptr));

  /* Non-RAR5 archive. */
  ra8_rar_t r4 = rar;
  r4.version   = k_ra8_rar_ver_4;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_rar5_decompress(&r4, 0U, pklen, output, sizeof(output), 4U, &s_state, &got));

  /* Output buffer too small. */
  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 ra8_rar5_decompress(&rar, 0U, pklen, output, 2U, 4U, &s_state, &got));

  /* Zero packed size. */
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 ra8_rar5_decompress(&rar, 0U, 0U, output, sizeof(output), 4U, &s_state, &got));

  /* Empty member (unp_size 0) decodes to nothing. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rar5_decompress(&rar, 0U, pklen, output, sizeof(output), 0U, &s_state, &got));
  TEST_ASSERT_EQ(0U, got);
  TEST_END("rar5: entry-point guards");
}

/* ---- CBR facade: compressed-page parity + RAR4 unsupported --------------- */

int main(void)
{
  internal_test_mcdc_copy_match();
  internal_test_mcdc_filter_delta();
  internal_test_mcdc_fill_zeros();
  internal_test_mcdc_apply_run();
  internal_test_mcdc_decode_stream_blocks();
  internal_test_rar5_guards();
  return 0;
}
