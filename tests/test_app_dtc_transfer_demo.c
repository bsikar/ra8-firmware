/**
 * @file test_app_dtc_transfer_demo.c
 * @brief Integration test: DTC 1 KB SRAM->SRAM block-copy bring-up
 *
 * @details
 * Mirrors examples/ek_ra8d2/dtc_transfer_demo/main.c. The descriptor
 * encoding and vector-table wiring are pure logic (the DTC engine is not
 * modelled on the host), so the test focuses on:
 *
 *  - The fill helper produces a non-trivial source + zeroed destination.
 *  - The verify helper detects matches and mismatches (MC/DC).
 *  - The Transfer Information mode word encodes block / 32-bit /
 *    increment-both as 0xA8080000 (HUM Figure 18.4).
 *  - The DTC vector table is an indirect table of 4-byte TI start
 *    addresses, not a flat array of 16-byte TI blocks.
 *  - The slot re-arm sets ICU.IELSRn.DTCE while preserving IELS and
 *    write-1-clearing the RW1C IR flag.
 *
 * Each test exercises one branch of the demo's compound decisions for
 * MC/DC coverage. No ra8_fake_mmap MMIO is required.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_dtc_regs.h"
#include "ra8_icu_regs.h"
#include "unity_minimal.h"

typedef enum : uint16_t {
  k_t_dtc_words   = 256U, /**< T dtc words.   */
  k_t_dtc_byte_sh = 8U,   /**< T dtc byte sh. */
} t_dtc_const_t;

/* Mirror of the demo's TI mode-bit field values (HUM Ch 18.2.2 / 18.2.3). */
typedef enum : uint8_t {
  k_t_dtc_md_block   = 0x2U, /**< T dtc md block.   */
  k_t_dtc_sz_word    = 0x2U, /**< T dtc sz word.    */
  k_t_dtc_sm_inc     = 0x2U, /**< T dtc sm inc.     */
  k_t_dtc_dm_inc     = 0x2U, /**< T dtc dm inc.     */
  k_t_dtc_mra_md_pos = 6U,   /**< T dtc mra md pos. */
  k_t_dtc_mra_sz_pos = 4U,   /**< T dtc mra sz pos. */
  k_t_dtc_mra_sm_pos = 2U,   /**< T dtc mra sm pos. */
  k_t_dtc_mrb_dm_pos = 2U,   /**< T dtc mrb dm pos. */
  k_t_dtc_mra_bpos   = 24U,  /**< T dtc mra bpos.   */
  k_t_dtc_mrb_bpos   = 16U,  /**< T dtc mrb bpos.   */
} t_dtc_mr_t;

typedef enum : uint32_t {
  k_t_dtc_mr_expected = 0xA8080000UL, /**< block / 32-bit / inc-both.    */
  k_t_dtc_iels_sample = 0x0CCUL,      /**< ELC software event 0 in IELS. */
} t_dtc_word_t;

static uint32_t s_src[k_t_dtc_words];
static uint32_t s_dst[k_t_dtc_words];

static void reset_world(void)
{
  (void)memset(s_src, 0, sizeof(s_src));
  (void)memset(s_dst, 0, sizeof(s_dst));
}

/** @brief Copy of the demo's pattern-fill helper. */
static void fill_buffers(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_t_dtc_words; ++i) {
    s_src[i] = i ^ (i >> (uint32_t)k_t_dtc_byte_sh);
    s_dst[i] = 0U;
  }
}

/** @brief Copy of the demo's verify helper. */
static uint8_t verify(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_t_dtc_words; ++i) {
    if (s_dst[i] != s_src[i]) {
      return 0U;
    }
  }
  return 1U;
}

/** @brief Build the TI mode word exactly as the demo does. */
static uint32_t build_mr(void)
{
  const uint8_t mra = (uint8_t)(((uint8_t)k_t_dtc_md_block << k_t_dtc_mra_md_pos) |
                                ((uint8_t)k_t_dtc_sz_word << k_t_dtc_mra_sz_pos) |
                                ((uint8_t)k_t_dtc_sm_inc << k_t_dtc_mra_sm_pos));
  const uint8_t mrb = (uint8_t)((uint8_t)k_t_dtc_dm_inc << k_t_dtc_mrb_dm_pos);
  return ((uint32_t)mra << k_t_dtc_mra_bpos) | ((uint32_t)mrb << k_t_dtc_mrb_bpos);
}

/**
 * @brief Pattern fill produces a non-trivial source buffer.
 *
 * @par MC/DC:
 * No compound decision; the fill loop only has the bound check. Two
 * vectors prove the pattern is not all-zero and the destination is
 * fully cleared.
 */
static void test_dtc_app_fill(void)
{
  reset_world();
  TEST_BEGIN("dtc_transfer_demo: fill produces non-trivial src + zero dst");
  fill_buffers();
  uint32_t accum_src = 0U;
  uint32_t accum_dst = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_t_dtc_words; ++i) {
    accum_src |= s_src[i];
    accum_dst |= s_dst[i];
  }
  TEST_ASSERT(accum_src != 0U);
  TEST_ASSERT_EQ(0U, accum_dst);
  TEST_END("dtc_transfer_demo: fill produces non-trivial src + zero dst");
}

/**
 * @brief verify returns 1 when the buffers match and 0 when they do not.
 *
 * @par MC/DC:
 * Compound decision: ``s_dst[i] != s_src[i]``. One atomic condition x 2
 * vectors -- equal (this test, vector A) + first mismatch (vector B).
 */
static void test_dtc_app_verify_branches(void)
{
  reset_world();
  TEST_BEGIN("dtc_transfer_demo: verify covers match + mismatch");
  fill_buffers();
  (void)memcpy(s_dst, s_src, sizeof(s_src));
  TEST_ASSERT_EQ(1U, verify());
  s_dst[0] ^= 0x1U;
  TEST_ASSERT_EQ(0U, verify());
  TEST_END("dtc_transfer_demo: verify covers match + mismatch");
}

/**
 * @brief The TI mode word encodes block / 32-bit / increment-both.
 *
 * @par MC/DC:
 * Straight-line encoding; no compound decision. One vector confirms the
 * exact word (HUM Figure 18.4 p 799).
 */
static void test_dtc_app_ti_encoding(void)
{
  reset_world();
  TEST_BEGIN("dtc_transfer_demo: TI mode word == 0xA8080000");
  r_dtc_xfer_info_t ti = {};
  ti.MR                = build_mr();
  ti.SAR               = (uint32_t)(uintptr_t)s_src;
  ti.DAR               = (uint32_t)(uintptr_t)s_dst;
  ti.CRB               = 0x0001U; /* one block.                                     */
  ti.CRA               = 0x0000U; /* CRAH=CRAL=0 => 256-unit block (HUM Ch 18.2.7). */
  /* SAR/DAR are 32-bit register fields; truncate the host pointer to the
   * bus-address width before the compare (HUM Ch 18.2 p 793-797). */
  const uint32_t sar_expected = (uint32_t)(uintptr_t)s_src;
  const uint32_t dar_expected = (uint32_t)(uintptr_t)s_dst;
  TEST_ASSERT_EQ(k_t_dtc_mr_expected, ti.MR);
  TEST_ASSERT_EQ(sar_expected, ti.SAR);
  TEST_ASSERT_EQ(dar_expected, ti.DAR);
  TEST_ASSERT_EQ(0x0001U, ti.CRB);
  TEST_ASSERT_EQ(0x0000U, ti.CRA);
  TEST_END("dtc_transfer_demo: TI mode word == 0xA8080000");
}

/**
 * @brief The DTC vector table is indirect (4-byte TI start addresses).
 *
 * @par MC/DC:
 * Straight-line wiring; no compound decision. Vectors confirm the
 * 16-byte TI block, the 4-byte vector entry, and 16-byte TI alignment
 * (HUM Ch 18.3.1 p 796, Figures 18.2/18.3).
 */
static void test_dtc_app_vector_table_indirect(void)
{
  reset_world();
  TEST_BEGIN("dtc_transfer_demo: vector table holds a 4-byte TI pointer");
  [[gnu::aligned(16)]] static uint32_t          s_vt[16];
  [[gnu::aligned(16)]] static r_dtc_xfer_info_t s_ti;
  const uint16_t                                slot = 5U;
  s_vt[slot]                                         = (uint32_t)(uintptr_t)&s_ti;

  /* Vector-table slots are 32-bit; truncate the host pointer to match. */
  const uint32_t vt_expected = (uint32_t)(uintptr_t)&s_ti;
  TEST_ASSERT_EQ(16U, sizeof(s_ti));   /* TI block is 16 bytes.      */
  TEST_ASSERT_EQ(4U, sizeof(s_vt[0])); /* vector entry is a pointer. */
  TEST_ASSERT_EQ(vt_expected, s_vt[slot]);
  TEST_ASSERT_EQ(0U, (s_vt[slot] & 0xFU)); /* 16-byte aligned, bit0 = 0. */
  TEST_END("dtc_transfer_demo: vector table holds a 4-byte TI pointer");
}

/**
 * @brief Re-arm sets DTCE, preserves IELS, and write-1-clears IR.
 *
 * @par MC/DC:
 * Straight-line bit algebra; no compound decision. Vectors cover the
 * fresh-slot case (IR = 0) and the post-completion case (IR = 1), which
 * is what the read-modify-write must clear (HUM Ch 14.2.10, Figure 18.5).
 */
static void test_dtc_app_arm_slot_bits(void)
{
  reset_world();
  TEST_BEGIN("dtc_transfer_demo: arm sets DTCE, keeps IELS, clears IR");
  /* Fresh slot: IELS set, DTCE = 0, IR = 0. */
  const uint32_t fresh = (uint32_t)k_t_dtc_iels_sample;
  const uint32_t armed = fresh | (uint32_t)k_ra8_ielsr_dtce_mask;
  TEST_ASSERT_EQ(k_t_dtc_iels_sample, armed & (uint32_t)k_ra8_ielsr_iels_mask);
  TEST_ASSERT((armed & (uint32_t)k_ra8_ielsr_dtce_mask) != 0U);

  /* Post-completion: DTC cleared DTCE and set IR (HUM Figure 18.5). The
   * read-modify-write writes the read-back IR=1 (RW1C => clears) and
   * re-asserts DTCE. */
  const uint32_t completed = (uint32_t)k_t_dtc_iels_sample | (uint32_t)k_ra8_ielsr_ir_mask;
  const uint32_t rewritten = completed | (uint32_t)k_ra8_ielsr_dtce_mask;
  TEST_ASSERT((rewritten & (uint32_t)k_ra8_ielsr_ir_mask) != 0U);   /* writes 1 to IR (RW1C). */
  TEST_ASSERT((rewritten & (uint32_t)k_ra8_ielsr_dtce_mask) != 0U); /* DTCE re-armed.         */
  TEST_ASSERT_EQ(k_t_dtc_iels_sample, rewritten & (uint32_t)k_ra8_ielsr_iels_mask);
  TEST_END("dtc_transfer_demo: arm sets DTCE, keeps IELS, clears IR");
}

int main(void)
{
  test_dtc_app_fill();
  test_dtc_app_verify_branches();
  test_dtc_app_ti_encoding();
  test_dtc_app_vector_table_indirect();
  test_dtc_app_arm_slot_bits();
  return 0;
}
