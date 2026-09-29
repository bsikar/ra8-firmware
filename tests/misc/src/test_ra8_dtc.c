/**
 * @file test_ra8_dtc.c
 * @brief Unit tests for ra8_dtc.c (Data Transfer Controller)
 * @details Covers DTC descriptor setup, vector-table activation, transfer validation, and lifecycle state using bounded fixture storage.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_dtc.h"
#include "ra8_dtc_regs.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "unity_minimal.h"

/**
 * @enum dtc_fixture_t
 * @brief Buffer capacities and payload sizes.
 */
typedef enum : uint8_t {
  k_dtc_regs_bytes =
    0x30U, /**< Required DTC register-block size; static_assert catches hardware-layout drift. */
} dtc_fixture_t;

/**
 * @enum dtc_fixture2_t
 * @brief Values planted in registers to prove a read or write reaches them.
 */
typedef enum : uint16_t {
  k_dtc_probe_sts_c =
    0xBEADU, /**< A third value, one nibble off the second, so a partial-width read is visible. */
  k_dtc_probe_sts_b =
    0xBEEFU, /**< A second, different value, so the read cannot be a cached first result. */
  /** Planted in DTCSTS to prove the read reaches the register. */
  k_dtc_probe_sts_a = 0xCAFEU,
} dtc_fixture2_t;

typedef enum : uintptr_t {
  k_ra8_dtc_test_vector_addr  = 0x22000400UL, /**< Arbitrary SRAM-region pointer. */
  k_ra8_dtc_test_vector_addr2 = 0x22000800UL, /**< Secondary reconfig target.     */
} ra8_dtc_test_addr_t;

/* Compile-time check: the regs block matches FSP R_DTC_Type
 * (size = 0x30 / 48 bytes per RA8D2 CMSIS R_DTC_Type). */
static_assert(sizeof(r_dtc_regs_t) == k_dtc_regs_bytes,
              "r_dtc_regs_t must be 48 bytes (FSP R_DTC_Type)");
static_assert(sizeof(r_dtc_xfer_info_t) == 16U, "r_dtc_xfer_info_t must be 16 bytes (HUM 18.2)");

static uint32_t s_dtc_cb_count;
static uint16_t s_dtc_cb_last_mask;

static void stub_dtc_cb(void* ctx, uint16_t mask)
{
  (void)ctx;
  ++s_dtc_cb_count;
  s_dtc_cb_last_mask = mask;
}

/**
 * @enum dtc_desc_fixture_t
 * @brief Counts the descriptor facade encodes, and the slots bind_activation is handed.
 */
typedef enum : uint16_t {
  k_dtc_bad_block_units = 257U, /**< One past the 256-unit block ceiling (HUM 18.2.7). */
  k_dtc_slot_past_end   = 96U,  /**< First slot number outside the vector table.       */
  k_dtc_max_block_units = 255U, /**< Largest block size that encodes as itself.        */
  k_dtc_block_units     = 4U,   /**< A block size with a distinct high and low byte.   */
  k_dtc_normal_units    = 7U,   /**< A normal-mode transfer count.                     */
  k_dtc_block_count     = 2U,   /**< A block count, distinct from the block size.      */
  k_dtc_block_count_b   = 3U,   /**< A second block count, for the 256-unit case.      */
  k_dtc_unwritten_cra   = 0xA5A5U, /**< Planted in out_ti to prove a rejection leaves it. */
  k_dtc_free_slot       = 5U,   /**< In range, but never handed to ra8_isr_register.   */
} dtc_desc_fixture_t;

/**
 * @enum dtc_mr_expect_t
 * @brief Expected MR words: MRA in MR[31:24], MRB in MR[23:16], MRC left zero.
 */
typedef enum : uint32_t {
  /** block + word + src inc + dst fixed: MRA = 0xA8, MRB = 0x00. */
  k_dtc_mr_block_word_inc = 0xA8000000UL,
  /** normal + byte + src fixed + dst inc: MRA = 0x00, MRB = 0x08. */
  k_dtc_mr_normal_byte_dst_inc = 0x00080000UL,
  /** normal + half + both inc: MRA = 0x18, MRB = 0x08. */
  k_dtc_mr_normal_half_both_inc = 0x18080000UL,
} dtc_mr_expect_t;

/** Payload the descriptor cases point SAR and DAR at; never transferred here. */
static uint32_t s_dtc_src_buf[4];
static uint32_t s_dtc_dst_buf[4];

/** Correctly-aligned vector table, so bind_activation has somewhere real to write. */
static ra8_dtc_vector_table_t s_dtc_vectors;

/**
 * @brief A cfg the describe cases mutate one field at a time.
 */
static ra8_dtc_xfer_cfg_t base_cfg(void)
{
  return (ra8_dtc_xfer_cfg_t){
    .src         = s_dtc_src_buf,
    .dst         = s_dtc_dst_buf,
    .src_mode    = k_ra8_dtc_addr_inc,
    .dst_mode    = k_ra8_dtc_addr_inc,
    .unit        = k_ra8_dtc_unit_word,
    .mode        = k_ra8_dtc_mode_normal,
    .unit_count  = k_dtc_normal_units,
    .block_count = 0U,
  };
}

static void prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
  s_dtc_cb_count     = 0U;
  s_dtc_cb_last_mask = 0U;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_null_vector(void)
{
  TEST_BEGIN("dtc init null vector");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_dtc_init(nullptr));
  TEST_END("dtc init null vector");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_happy(void)
{
  TEST_BEGIN("dtc init happy");
  prep();

  void*           vec = (void*)(uintptr_t)k_ra8_dtc_test_vector_addr;
  const ra8_err_t err = ra8_dtc_init(vec);
  TEST_ASSERT_EQ(k_ra8_ok, err);

  volatile r_dtc_regs_t* reg = ra8_dtc();
  TEST_ASSERT_EQ(0, reg->DTCCR);
  TEST_ASSERT_EQ(0, reg->DTCST);
  TEST_ASSERT_EQ(k_ra8_dtc_test_vector_addr, reg->DTCVBR);
  TEST_END("dtc init happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_enable_then_disable(void)
{
  TEST_BEGIN("dtc enable then disable");
  prep();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_dtc_enable());
  volatile r_dtc_regs_t* reg = ra8_dtc();
  TEST_ASSERT_EQ(k_ra8_dtcst_dtcst_msk, reg->DTCST);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_dtc_disable());
  TEST_ASSERT_EQ(0, reg->DTCST);
  TEST_END("dtc enable then disable");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_deinit(void)
{
  TEST_BEGIN("dtc deinit");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dtc_init((void*)(uintptr_t)k_ra8_dtc_test_vector_addr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dtc_deinit());
  volatile r_dtc_regs_t* reg = ra8_dtc();
  TEST_ASSERT_EQ(0, reg->DTCVBR);
  TEST_END("dtc deinit");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_reconfigure(void)
{
  TEST_BEGIN("dtc reconfigure");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dtc_init((void*)(uintptr_t)k_ra8_dtc_test_vector_addr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_dtc_reconfigure(nullptr));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_dtc_reconfigure((void*)(uintptr_t)k_ra8_dtc_test_vector_addr2));
  volatile r_dtc_regs_t* reg = ra8_dtc();
  TEST_ASSERT_EQ(k_ra8_dtc_test_vector_addr2, reg->DTCVBR);
  TEST_ASSERT_EQ(0, reg->DTCST);
  /* FSP-aligned: reconfigure leaves DTCCR with RRS enabled (0x18). */
  TEST_ASSERT_EQ(k_ra8_dtccr_rrs_enable, reg->DTCCR);
  TEST_END("dtc reconfigure");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_status_read_and_clear(void)
{
  TEST_BEGIN("dtc status read + clear");
  prep();

  ra8_dtc()->DTCSTS = k_dtc_probe_sts_c;
  uint16_t mask     = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dtc_get_status(&mask));
  TEST_ASSERT_EQ(0xBEADU, mask);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_dtc_clear_status(0x00FFU));
  TEST_ASSERT_EQ((0xBEADU & ~0x00FFU), ra8_dtc()->DTCSTS);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_dtc_get_status(nullptr));
  TEST_END("dtc status read + clear");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("dtc attach + dispatch");
  prep();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_dtc_attach_handler(stub_dtc_cb, (void*)(uintptr_t)0xD0U));
  ra8_dtc()->DTCSTS = k_dtc_probe_sts_a;
  ra8_dtc_dispatch();
  TEST_ASSERT_EQ(1, s_dtc_cb_count);
  TEST_ASSERT_EQ(0xCAFEU, s_dtc_cb_last_mask);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_dtc_attach_handler(nullptr, nullptr));
  ra8_dtc()->DTCSTS = k_dtc_probe_sts_b;
  ra8_dtc_dispatch();
  TEST_ASSERT_EQ(1, s_dtc_cb_count);
  TEST_END("dtc attach + dispatch");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_power_transition(void)
{
  TEST_BEGIN("dtc power transition");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dtc_init((void*)(uintptr_t)k_ra8_dtc_test_vector_addr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dtc_enter_stop());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dtc_exit_stop());
  TEST_END("dtc power transition");
}

/**
 * @par MC/DC:
 * `ra8_dtc_describe`'s validity decision is a four-term OR
 * (`!mode_valid || !unit_valid || !src_valid || !dst_valid`). Each
 * sub-case below drives exactly one term true with the other three
 * false, and the happy path in @ref test_describe_mr_encoding drives
 * all four false, so every term is shown to independently decide the
 * outcome.
 */
static void test_describe_guards(void)
{
  TEST_BEGIN("dtc describe guards");
  prep();

  ra8_dtc_ti_t             ti  = {0};
  const ra8_dtc_xfer_cfg_t cfg = base_cfg();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_dtc_describe(nullptr, &ti));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_dtc_describe(&cfg, nullptr));

  ra8_dtc_xfer_cfg_t no_src = base_cfg();
  no_src.src                = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_dtc_describe(&no_src, &ti));

  ra8_dtc_xfer_cfg_t no_dst = base_cfg();
  no_dst.dst                = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_dtc_describe(&no_dst, &ti));

  /* One unsupported enum value per term. Repeat mode (MD = 01b), SZ = 11b and
   * the decrement / DTCDISP address modes are the encodings the facade
   * deliberately does not expose (see ra8_dtc.h). */
  ra8_dtc_xfer_cfg_t bad_mode = base_cfg();
  bad_mode.mode              = (ra8_dtc_mode_t)0x1U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_dtc_describe(&bad_mode, &ti));

  ra8_dtc_xfer_cfg_t bad_unit = base_cfg();
  bad_unit.unit              = (ra8_dtc_unit_t)0x3U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_dtc_describe(&bad_unit, &ti));

  ra8_dtc_xfer_cfg_t bad_src = base_cfg();
  bad_src.src_mode          = (ra8_dtc_addr_mode_t)0x1U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_dtc_describe(&bad_src, &ti));

  ra8_dtc_xfer_cfg_t bad_dst = base_cfg();
  bad_dst.dst_mode          = (ra8_dtc_addr_mode_t)0x3U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_dtc_describe(&bad_dst, &ti));

  TEST_END("dtc describe guards");
}

/**
 * @par MC/DC:
 * The count decisions are two ORs. Block mode:
 * `units == 0 || units > 256 || blocks == 0`; normal mode:
 * `units == 0 || blocks != 0`. Each rejection below makes one term true
 * with the rest false, and the accepted cases make every term false.
 */
static void test_describe_counts(void)
{
  TEST_BEGIN("dtc describe counts");
  prep();

  ra8_dtc_ti_t ti = {0};

  /* Normal mode: CRA is the transfer count, CRB stays 0. */
  ra8_dtc_xfer_cfg_t normal = base_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dtc_describe(&normal, &ti));
  TEST_ASSERT_EQ(k_dtc_normal_units, ti.ti.CRA);
  TEST_ASSERT_EQ(0U, ti.ti.CRB);

  /* Block mode: CRAH and CRAL both carry the block size, CRB the block count. */
  ra8_dtc_xfer_cfg_t block = base_cfg();
  block.mode               = k_ra8_dtc_mode_block;
  block.unit_count         = k_dtc_block_units;
  block.block_count        = k_dtc_block_count;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dtc_describe(&block, &ti));
  TEST_ASSERT_EQ(0x0404U, ti.ti.CRA);
  TEST_ASSERT_EQ(k_dtc_block_count, ti.ti.CRB);

  block.unit_count = k_dtc_max_block_units;
  block.block_count = 1U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dtc_describe(&block, &ti));
  TEST_ASSERT_EQ(0xFFFFU, ti.ti.CRA);

  /* HUM Ch 18.2.7 p 790: a 256-unit block is the one size that encodes as 0. */
  block.unit_count  = (uint16_t)k_ra8_dtc_block_units_max;
  block.block_count = k_dtc_block_count_b;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dtc_describe(&block, &ti));
  TEST_ASSERT_EQ(0x0000U, ti.ti.CRA);
  TEST_ASSERT_EQ(k_dtc_block_count_b, ti.ti.CRB);

  /* Rejections, and the @post that a rejected call leaves out_ti untouched. */
  ti.ti.CRA = k_dtc_unwritten_cra;

  ra8_dtc_xfer_cfg_t zero_units = base_cfg();
  zero_units.unit_count         = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_dtc_describe(&zero_units, &ti));

  ra8_dtc_xfer_cfg_t normal_with_blocks = base_cfg();
  normal_with_blocks.block_count        = 1U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_dtc_describe(&normal_with_blocks, &ti));

  ra8_dtc_xfer_cfg_t block_zero_size = base_cfg();
  block_zero_size.mode               = k_ra8_dtc_mode_block;
  block_zero_size.unit_count         = 0U;
  block_zero_size.block_count        = 1U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_dtc_describe(&block_zero_size, &ti));

  ra8_dtc_xfer_cfg_t block_too_big = base_cfg();
  block_too_big.mode               = k_ra8_dtc_mode_block;
  block_too_big.unit_count         = k_dtc_bad_block_units;
  block_too_big.block_count        = 1U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_dtc_describe(&block_too_big, &ti));

  ra8_dtc_xfer_cfg_t block_zero_count = base_cfg();
  block_zero_count.mode               = k_ra8_dtc_mode_block;
  block_zero_count.unit_count         = k_dtc_block_units;
  block_zero_count.block_count        = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_dtc_describe(&block_zero_count, &ti));

  TEST_ASSERT_EQ(k_dtc_unwritten_cra, ti.ti.CRA);
  TEST_END("dtc describe counts");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- it reads back the field
 * placement of an accepted description; the validity and count
 * decisions are covered by the two cases above)
 */
static void test_describe_mr_encoding(void)
{
  TEST_BEGIN("dtc describe MR encoding");
  prep();

  ra8_dtc_ti_t ti = {0};

  /* MD[7:6], SZ[5:4] and SM[3:2] land in MRA, DM[3:2] in MRB, MRC stays 0. */
  ra8_dtc_xfer_cfg_t block = base_cfg();
  block.mode               = k_ra8_dtc_mode_block;
  block.unit              = k_ra8_dtc_unit_word;
  block.src_mode           = k_ra8_dtc_addr_inc;
  block.dst_mode           = k_ra8_dtc_addr_fixed;
  block.unit_count         = k_dtc_block_units;
  block.block_count        = k_dtc_block_count;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dtc_describe(&block, &ti));
  TEST_ASSERT_EQ(k_dtc_mr_block_word_inc, ti.ti.MR);

  /* SAR and DAR are the cfg pointers, unmodified. */
  TEST_ASSERT_EQ((uint32_t)(uintptr_t)s_dtc_src_buf, ti.ti.SAR);
  TEST_ASSERT_EQ((uint32_t)(uintptr_t)s_dtc_dst_buf, ti.ti.DAR);

  ra8_dtc_xfer_cfg_t dst_inc = base_cfg();
  dst_inc.unit               = k_ra8_dtc_unit_byte;
  dst_inc.src_mode           = k_ra8_dtc_addr_fixed;
  dst_inc.dst_mode           = k_ra8_dtc_addr_inc;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dtc_describe(&dst_inc, &ti));
  TEST_ASSERT_EQ(k_dtc_mr_normal_byte_dst_inc, ti.ti.MR);

  ra8_dtc_xfer_cfg_t both_inc = base_cfg();
  both_inc.unit               = k_ra8_dtc_unit_half;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dtc_describe(&both_inc, &ti));
  TEST_ASSERT_EQ(k_dtc_mr_normal_half_both_inc, ti.ti.MR);

  TEST_END("dtc describe MR encoding");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- bind_activation's rejections
 * are a sequence of independent single-term guards, taken here in the
 * order the implementation checks them)
 */
static void test_bind_activation_guards(void)
{
  TEST_BEGIN("dtc bind_activation guards");
  prep();
  /* prep() resets the fake register file, not the driver's retained vector
   * base, and every case above leaves one installed. Drop it, so the
   * "ra8_dtc_init has not run" guard below is the thing being tested. */
  (void)ra8_dtc_deinit();

  ra8_dtc_ti_t             ti  = {0};
  const ra8_dtc_xfer_cfg_t cfg = base_cfg();

  /* Null checks come before the init check, so these hold with no vector base. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_dtc_bind_activation(0U, nullptr, &ti));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_dtc_bind_activation(0U, &cfg, nullptr));

  /* No ra8_dtc_init: there is no vector table to write into. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_dtc_bind_activation(0U, &cfg, &ti));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_dtc_init(s_dtc_vectors.entry));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_isr_init());

  /* One past the last vector-table entry. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_dtc_bind_activation(k_dtc_slot_past_end, &cfg, &ti));

  /* A cfg describe rejects is returned as-is, before anything is armed. */
  ra8_dtc_xfer_cfg_t bad = base_cfg();
  bad.unit_count         = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_dtc_bind_activation(k_dtc_free_slot, &bad, &ti));
  TEST_ASSERT_EQ(0U, s_dtc_vectors.entry[k_dtc_free_slot]);

  /* In range and describable, but nobody registered the slot: ra8_isr_set_dtc
   * rejects it. The vector-table write has already happened by then, which is
   * what the entry check below pins. */
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_dtc_bind_activation(k_dtc_free_slot, &cfg, &ti));
  TEST_ASSERT_EQ((uint32_t)(uintptr_t)&ti.ti, s_dtc_vectors.entry[k_dtc_free_slot]);

  TEST_END("dtc bind_activation guards");
}

int main(void)
{
  test_init_null_vector();
  test_init_happy();
  test_enable_then_disable();
  test_deinit();
  test_reconfigure();
  test_status_read_and_clear();
  test_attach_and_dispatch();
  test_power_transition();
  test_describe_guards();
  test_describe_counts();
  test_describe_mr_encoding();
  test_bind_activation_guards();
  return 0;
}
