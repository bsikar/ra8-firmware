/**
 * @file test_ra8_dmac.c
 * @brief Unit tests for ra8_dmac.c (Direct Memory Access Controller)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_dmac.h"
#include "ra8_dmac_internal.h"
#include "ra8_dmac_regs.h"
#include "ra8_err.h"
#include "ra8_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum dmac_token_narrow_t
 * @brief A narrow callback token, checked on the way back.
 */
typedef enum : uint8_t {
  k_dmac_ctx_token_small =
    7, /**< Small token handed to the callback and checked on the way back. */
} dmac_token_narrow_t;

/**
 * @enum dmac_token_wide_t
 * @brief A wider one, proving the context is not truncated to a byte.
 */
typedef enum : uint16_t {
  k_dmac_ctx_token_wide =
    0xABCD, /**< A wider token, proving the context is not truncated to a byte. */
} dmac_token_wide_t;

typedef enum : uint32_t {
  k_ra8_dmac_test_src    = 0x22000100UL, /**< RA8 DMAC test src.    */
  k_ra8_dmac_test_dst    = 0x22000200UL, /**< RA8 DMAC test dst.    */
  k_ra8_dmac_test_count  = 0x0040U,      /**< RA8 DMAC test count.  */
  k_ra8_dmac_test_enable = 0x01U,        /**< RA8 DMAC test enable. */
} ra8_dmac_test_const_t;

typedef enum : uint8_t {
  k_ra8_dmac_test_channel_valid = 0U, /**< RA8 DMAC test channel valid. */
  k_ra8_dmac_test_channel_last  = 7U, /**< RA8 DMAC test channel last.  */
  k_ra8_dmac_test_channel_bad   = 8U, /**< RA8 DMAC test channel bad.   */
} ra8_dmac_test_channel_t;

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_start_null_cfg(void)
{
  TEST_BEGIN("dmac start null cfg");
  ra8_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_dmac_start((uint8_t)k_ra8_dmac_test_channel_valid, nullptr));
  TEST_END("dmac start null cfg");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_start_bad_channel(void)
{
  TEST_BEGIN("dmac start bad channel");
  ra8_sim_mmap_reset();

  const ra8_dmac_config_t cfg = {
    .src     = (uint32_t)k_ra8_dmac_test_src,
    .dst     = (uint32_t)k_ra8_dmac_test_dst,
    .count   = (uint16_t)k_ra8_dmac_test_count,
    .width   = k_ra8_dmac_width_word,
    .src_inc = true,
    .dst_inc = true,
  };
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 ra8_dmac_start((uint8_t)k_ra8_dmac_test_channel_bad, &cfg));
  TEST_END("dmac start bad channel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_start_happy_both_inc(void)
{
  TEST_BEGIN("dmac start happy both inc");
  ra8_sim_mmap_reset();

  const ra8_dmac_config_t cfg = {
    .src     = (uint32_t)k_ra8_dmac_test_src,
    .dst     = (uint32_t)k_ra8_dmac_test_dst,
    .count   = (uint16_t)k_ra8_dmac_test_count,
    .width   = k_ra8_dmac_width_word,
    .src_inc = true,
    .dst_inc = true,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dmac_start((uint8_t)k_ra8_dmac_test_channel_valid, &cfg));

  volatile r_dmac_channel_regs_t* reg = ra8_dmac((uint8_t)k_ra8_dmac_test_channel_valid);
  TEST_ASSERT_NOT_NULL((void*)reg);
  TEST_ASSERT_EQ(k_ra8_dmac_test_src, reg->DMSAR);
  TEST_ASSERT_EQ(k_ra8_dmac_test_dst, reg->DMDAR);
  TEST_ASSERT_EQ(k_ra8_dmac_test_count, reg->DMCRA);
  TEST_ASSERT_EQ(k_ra8_dmac_test_enable, reg->DMCNT);
  TEST_END("dmac start happy both inc");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_start_no_src_inc(void)
{
  TEST_BEGIN("dmac start no src inc");
  ra8_sim_mmap_reset();

  const ra8_dmac_config_t cfg = {
    .src     = (uint32_t)k_ra8_dmac_test_src,
    .dst     = (uint32_t)k_ra8_dmac_test_dst,
    .count   = (uint16_t)k_ra8_dmac_test_count,
    .width   = k_ra8_dmac_width_byte,
    .src_inc = false,
    .dst_inc = true,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dmac_start((uint8_t)k_ra8_dmac_test_channel_valid, &cfg));
  TEST_END("dmac start no src inc");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_start_no_dst_inc(void)
{
  TEST_BEGIN("dmac start no dst inc");
  ra8_sim_mmap_reset();

  const ra8_dmac_config_t cfg = {
    .src     = (uint32_t)k_ra8_dmac_test_src,
    .dst     = (uint32_t)k_ra8_dmac_test_dst,
    .count   = (uint16_t)k_ra8_dmac_test_count,
    .width   = k_ra8_dmac_width_half,
    .src_inc = true,
    .dst_inc = false,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dmac_start((uint8_t)k_ra8_dmac_test_channel_valid, &cfg));
  TEST_END("dmac start no dst inc");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_start_neither_inc(void)
{
  TEST_BEGIN("dmac start neither inc");
  ra8_sim_mmap_reset();

  const ra8_dmac_config_t cfg = {
    .src     = (uint32_t)k_ra8_dmac_test_src,
    .dst     = (uint32_t)k_ra8_dmac_test_dst,
    .count   = (uint16_t)k_ra8_dmac_test_count,
    .width   = k_ra8_dmac_width_byte,
    .src_inc = false,
    .dst_inc = false,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dmac_start((uint8_t)k_ra8_dmac_test_channel_last, &cfg));
  TEST_END("dmac start neither inc");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_stop_happy(void)
{
  TEST_BEGIN("dmac stop happy");
  ra8_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_dmac_stop((uint8_t)k_ra8_dmac_test_channel_valid));

  volatile r_dmac_channel_regs_t* reg = ra8_dmac((uint8_t)k_ra8_dmac_test_channel_valid);
  TEST_ASSERT_EQ(0, reg->DMCNT);
  TEST_END("dmac stop happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_stop_bad_channel(void)
{
  TEST_BEGIN("dmac stop bad channel");
  ra8_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_dmac_stop((uint8_t)k_ra8_dmac_test_channel_bad));
  TEST_END("dmac stop bad channel");
}

/* =============================================================================
 * Sweep 6 extensions: repeat / block / address-mode / callbacks
 * =============================================================================
 */

static uint32_t s_dmac_full_count;
static uint32_t s_dmac_half_count;
static void*    s_dmac_last_ctx;

static void stub_dmac_full_cb(void* ctx)
{
  ++s_dmac_full_count;
  s_dmac_last_ctx = ctx;
}

static void stub_dmac_half_cb(void* ctx)
{
  ++s_dmac_half_count;
  s_dmac_last_ctx = ctx;
}

static void prep_dmac_ext(void)
{
  ra8_sim_mmap_reset();
  s_dmac_full_count = 0U;
  s_dmac_half_count = 0U;
  s_dmac_last_ctx   = nullptr;
  /* Clear any stale callback slots from a prior test. */
  (void)ra8_dmac_attach_callback((uint8_t)k_ra8_dmac_test_channel_valid, nullptr, nullptr);
  (void)ra8_dmac_attach_half_complete_handler((uint8_t)k_ra8_dmac_test_channel_valid,
                                              nullptr,
                                              nullptr);
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_start_repeat_mode(void)
{
  TEST_BEGIN("dmac start_repeat sets MD=01b");
  prep_dmac_ext();

  const ra8_dmac_config_t cfg = {
    .src         = (uint32_t)k_ra8_dmac_test_src,
    .dst         = (uint32_t)k_ra8_dmac_test_dst,
    .count       = (uint16_t)k_ra8_dmac_test_count,
    .width       = k_ra8_dmac_width_word,
    .src_inc     = true,
    .dst_inc     = true,
    .repeat_area = k_ra8_dmac_repeat_area_dest,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dmac_start_repeat((uint8_t)k_ra8_dmac_test_channel_valid, &cfg));

  volatile r_dmac_channel_regs_t* reg = ra8_dmac((uint8_t)k_ra8_dmac_test_channel_valid);
  TEST_ASSERT_NOT_NULL((void*)reg);
  const uint16_t md = (uint16_t)((reg->DMTMD & k_ra8_dmtmd_md_mask) >> k_ra8_dmtmd_md_pos);
  TEST_ASSERT_EQ(k_ra8_dmtmd_md_repeat, md);
  TEST_END("dmac start_repeat sets MD=01b");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_start_repeat_null(void)
{
  TEST_BEGIN("dmac start_repeat null cfg");
  prep_dmac_ext();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_dmac_start_repeat((uint8_t)k_ra8_dmac_test_channel_valid, nullptr));
  TEST_END("dmac start_repeat null cfg");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_start_block_mode(void)
{
  TEST_BEGIN("dmac start_block sets MD=10b and DMCRB");
  prep_dmac_ext();

  const ra8_dmac_config_t cfg = {
    .src         = (uint32_t)k_ra8_dmac_test_src,
    .dst         = (uint32_t)k_ra8_dmac_test_dst,
    .count       = (uint16_t)k_ra8_dmac_test_count,
    .width       = k_ra8_dmac_width_word,
    .src_inc     = true,
    .dst_inc     = true,
    .block_count = 4U,
    .repeat_area = k_ra8_dmac_repeat_area_src,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dmac_start_block((uint8_t)k_ra8_dmac_test_channel_valid, &cfg));

  volatile r_dmac_channel_regs_t* reg = ra8_dmac((uint8_t)k_ra8_dmac_test_channel_valid);
  TEST_ASSERT_NOT_NULL((void*)reg);
  const uint16_t md = (uint16_t)((reg->DMTMD & k_ra8_dmtmd_md_mask) >> k_ra8_dmtmd_md_pos);
  TEST_ASSERT_EQ(k_ra8_dmtmd_md_block, md);
  /* HUM Ch 17.2.9 p 737 -- block mode mirrors block_count into both
   * DMCRBH and DMCRBL, so DMCRB = bc | (bc << 16). */
  TEST_ASSERT_EQ((4U | (4U << 16U)), reg->DMCRB);
  TEST_END("dmac start_block sets MD=10b and DMCRB");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_start_block_zero_count(void)
{
  TEST_BEGIN("dmac start_block rejects block_count=0");
  prep_dmac_ext();
  const ra8_dmac_config_t cfg = {
    .src   = (uint32_t)k_ra8_dmac_test_src,
    .dst   = (uint32_t)k_ra8_dmac_test_dst,
    .count = (uint16_t)k_ra8_dmac_test_count,
    .width = k_ra8_dmac_width_word,
  };
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_dmac_start_block((uint8_t)k_ra8_dmac_test_channel_valid, &cfg));
  TEST_END("dmac start_block rejects block_count=0");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_address_mode_happy(void)
{
  TEST_BEGIN("dmac set_address_mode writes SM/DM");
  prep_dmac_ext();

  const ra8_dmac_config_t cfg = {
    .src     = (uint32_t)k_ra8_dmac_test_src,
    .dst     = (uint32_t)k_ra8_dmac_test_dst,
    .count   = (uint16_t)k_ra8_dmac_test_count,
    .width   = k_ra8_dmac_width_word,
    .src_inc = true,
    .dst_inc = true,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dmac_start((uint8_t)k_ra8_dmac_test_channel_valid, &cfg));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_dmac_set_address_mode((uint8_t)k_ra8_dmac_test_channel_valid,
                                           k_ra8_dmac_addr_decrement,
                                           k_ra8_dmac_addr_offset));

  volatile r_dmac_channel_regs_t* reg = ra8_dmac((uint8_t)k_ra8_dmac_test_channel_valid);
  const uint16_t sm = (uint16_t)((reg->DMAMD & k_ra8_dmamd_sm_mask) >> k_ra8_dmamd_sm_pos);
  const uint16_t dm = (uint16_t)((reg->DMAMD & k_ra8_dmamd_dm_mask) >> k_ra8_dmamd_dm_pos);
  TEST_ASSERT_EQ(k_ra8_dmac_addr_decrement, sm);
  TEST_ASSERT_EQ(k_ra8_dmac_addr_offset, dm);
  TEST_END("dmac set_address_mode writes SM/DM");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_address_mode_invalid(void)
{
  TEST_BEGIN("dmac set_address_mode rejects invalid args");
  prep_dmac_ext();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_dmac_set_address_mode((uint8_t)k_ra8_dmac_test_channel_valid,
                                           (ra8_dmac_addr_mode_t)0x77U,
                                           k_ra8_dmac_addr_fixed));
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 ra8_dmac_set_address_mode((uint8_t)k_ra8_dmac_test_channel_bad,
                                           k_ra8_dmac_addr_fixed,
                                           k_ra8_dmac_addr_fixed));
  TEST_END("dmac set_address_mode rejects invalid args");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_half_complete_handler(void)
{
  TEST_BEGIN("dmac half-complete handler dispatches");
  prep_dmac_ext();
  int sentinel = k_dmac_ctx_token_small;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_dmac_attach_half_complete_handler((uint8_t)k_ra8_dmac_test_channel_valid,
                                                       stub_dmac_half_cb,
                                                       &sentinel));
  ra8_dmac_dispatch_half((uint8_t)k_ra8_dmac_test_channel_valid);
  TEST_ASSERT_EQ(1, s_dmac_half_count);
  TEST_ASSERT(s_dmac_last_ctx == &sentinel);

  /* Out-of-range channel must early-exit. */
  ra8_dmac_dispatch_half((uint8_t)k_ra8_dmac_test_channel_bad);
  TEST_ASSERT_EQ(1, s_dmac_half_count);

  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 ra8_dmac_attach_half_complete_handler((uint8_t)k_ra8_dmac_test_channel_bad,
                                                       stub_dmac_half_cb,
                                                       nullptr));
  TEST_END("dmac half-complete handler dispatches");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_per_channel_callback(void)
{
  TEST_BEGIN("dmac per-channel callback dispatches");
  prep_dmac_ext();
  int sentinel = k_dmac_ctx_token_wide;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_dmac_attach_callback((uint8_t)k_ra8_dmac_test_channel_valid, stub_dmac_full_cb, &sentinel));
  ra8_dmac_dispatch((uint8_t)k_ra8_dmac_test_channel_valid);
  TEST_ASSERT_EQ(1, s_dmac_full_count);
  TEST_ASSERT(s_dmac_last_ctx == &sentinel);

  ra8_dmac_dispatch((uint8_t)k_ra8_dmac_test_channel_bad);
  TEST_ASSERT_EQ(1, s_dmac_full_count);

  TEST_ASSERT_EQ(
    k_ra8_err_out_of_range,
    ra8_dmac_attach_callback((uint8_t)k_ra8_dmac_test_channel_bad, stub_dmac_full_cb, nullptr));

  /* Clearing the slot must silence further dispatches. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_dmac_attach_callback((uint8_t)k_ra8_dmac_test_channel_valid, nullptr, nullptr));
  ra8_dmac_dispatch((uint8_t)k_ra8_dmac_test_channel_valid);
  TEST_ASSERT_EQ(1, s_dmac_full_count);
  TEST_END("dmac per-channel callback dispatches");
}

/**
 * @test test_mcdc_set_address_mode_bounds
 *
 * @par MC/DC:
 * Decision: `if ((src_mode > k_ra8_dmac_addr_decrement) ||
 *               (dest_mode > k_ra8_dmac_addr_decrement))`
 * (2 conditions, libs/ra8_hal/src/ra8_dmac.c line 629 -- gap row 337 in CSV)
 * - Vector 1: src=fixed (0), dest=fixed (0) -> F,F decision F -> ok.
 * - Vector 2: src=99 (out of range), dest=fixed -> T,_ short-circuit
 *   decision T -> invalid_arg (varies C1 vs V1).
 * - Vector 3: src=fixed, dest=99 -> F,T decision T -> invalid_arg
 *   (varies C2 vs V1; C1 held F).
 * MC/DC pair for C1: V1(F,F)->F vs V2(T,_)->T (decision flips, C2
 * masked by short-circuit). MC/DC pair for C2: V1(F,F)->F vs V3(F,T)->T
 * (decision flips, C1 held F). N+1 = 3 vectors for N=2 conditions.
 */
static void test_mcdc_set_address_mode_bounds(void)
{
  TEST_BEGIN("dmac set_address_mode MC/DC: src>dec || dest>dec");
  prep_dmac_ext();

  /* Vector 1: both in range. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_dmac_set_address_mode((uint8_t)k_ra8_dmac_test_channel_valid,
                                           k_ra8_dmac_addr_fixed,
                                           k_ra8_dmac_addr_fixed));

  /* Vector 2: src out of range, dest in range -> C1=T short-circuits. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_dmac_set_address_mode((uint8_t)k_ra8_dmac_test_channel_valid,
                                           (ra8_dmac_addr_mode_t)99U,
                                           k_ra8_dmac_addr_fixed));

  /* Vector 3: src in range, dest out of range -> C1=F, C2=T. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_dmac_set_address_mode((uint8_t)k_ra8_dmac_test_channel_valid,
                                           k_ra8_dmac_addr_fixed,
                                           (ra8_dmac_addr_mode_t)99U));

  TEST_END("dmac set_address_mode MC/DC: src>dec || dest>dec");
}

/**
 * @test test_mcdc_dmac_internal_mode_disables_dts
 *
 * @par MC/DC:
 * Decision at libs/ra8_hal/src/ra8_dmac.c (call site) -> helper at
 * libs/ra8_hal/src/ra8_dmac.c:
 *   ``mode == k_ra8_dmac_mode_normal || mode == k_ra8_dmac_mode_repeat_block``
 *   (2 conditions, OR). Direct-call vectors:
 * - V1: mode=REPEAT(1)        -> false (both false-side)
 * - V2: mode=NORMAL(0)        -> true  (varies left)
 * - V3: mode=REPEAT_BLOCK(3)  -> true  (varies right)
 * V1+V2 isolate left; V1+V3 isolate right. N+1 = 3.
 */
static void test_mcdc_dmac_internal_mode_disables_dts(void)
{
  TEST_BEGIN("dmac MC/DC: mode_disables_dts OR");
  TEST_ASSERT(!ra8_dmac_internal_mode_disables_dts((uint32_t)k_ra8_dmac_mode_normal,
                                                   (uint32_t)k_ra8_dmac_mode_repeat_block,
                                                   (uint32_t)k_ra8_dmac_mode_repeat));
  TEST_ASSERT(ra8_dmac_internal_mode_disables_dts((uint32_t)k_ra8_dmac_mode_normal,
                                                  (uint32_t)k_ra8_dmac_mode_repeat_block,
                                                  (uint32_t)k_ra8_dmac_mode_normal));
  TEST_ASSERT(ra8_dmac_internal_mode_disables_dts((uint32_t)k_ra8_dmac_mode_normal,
                                                  (uint32_t)k_ra8_dmac_mode_repeat_block,
                                                  (uint32_t)k_ra8_dmac_mode_repeat_block));
  TEST_END("dmac MC/DC: mode_disables_dts OR");
}

/**
 * @test test_mcdc_dmac_internal_dmint_extra_irq
 *
 * @par MC/DC:
 * Decision at libs/ra8_hal/src/ra8_dmac.c (call site) -> helper at
 * libs/ra8_hal/src/ra8_dmac.c:
 *   ``cfg->irq_each && cfg->mode != k_ra8_dmac_mode_repeat_block``
 *   (2 conditions, AND). Direct-call vectors:
 * - V1: irq_each=false, mode=NORMAL        -> false (both false-side)
 * - V2: irq_each=true,  mode=NORMAL        -> true  (varies left)
 * - V3: irq_each=true,  mode=REPEAT_BLOCK  -> false (varies right)
 * V1+V2 isolate left; V2+V3 isolate right. N+1 = 3.
 */
static void test_mcdc_dmac_internal_dmint_extra_irq(void)
{
  TEST_BEGIN("dmac MC/DC: dmint_extra_irq AND");
  TEST_ASSERT(!ra8_dmac_internal_dmint_extra_irq(false,
                                                 (uint32_t)k_ra8_dmac_mode_repeat_block,
                                                 (uint32_t)k_ra8_dmac_mode_normal));
  TEST_ASSERT(ra8_dmac_internal_dmint_extra_irq(true,
                                                (uint32_t)k_ra8_dmac_mode_repeat_block,
                                                (uint32_t)k_ra8_dmac_mode_normal));
  TEST_ASSERT(!ra8_dmac_internal_dmint_extra_irq(true,
                                                 (uint32_t)k_ra8_dmac_mode_repeat_block,
                                                 (uint32_t)k_ra8_dmac_mode_repeat_block));
  TEST_END("dmac MC/DC: dmint_extra_irq AND");
}

int32_t main(void)
{
  test_start_null_cfg();
  test_start_bad_channel();
  test_start_happy_both_inc();
  test_start_no_src_inc();
  test_start_no_dst_inc();
  test_start_neither_inc();
  test_stop_happy();
  test_stop_bad_channel();
  test_start_repeat_mode();
  test_start_repeat_null();
  test_start_block_mode();
  test_start_block_zero_count();
  test_set_address_mode_happy();
  test_set_address_mode_invalid();
  test_attach_half_complete_handler();
  test_attach_per_channel_callback();
  test_mcdc_set_address_mode_bounds();
  test_mcdc_dmac_internal_mode_disables_dts();
  test_mcdc_dmac_internal_dmint_extra_irq();
  (void)fprintf(stderr, "[OK  ] test_ra8_dmac.c\n");
  return 0;
}
