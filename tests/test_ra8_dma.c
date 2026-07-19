/**
 * @file test_ra8_dma.c
 * @brief Unit tests for the generic DMA transfer substrate (libs/ra8_hal/src/ra8_dma.c).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_dma.h"
#include "ra8_dmac.h"
#include "ra8_err.h"
#include "ra8_mstp.h"
#include "ra8_sim_dma.h"
#include "ra8_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum dma_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_dma_ch_ff                         = 0xFFU,
  k_dma_ra8_dma_dispatch_complete_200 = 200U,
  k_dma_val_11                        = 0x11,
  k_dma_val_22                        = 0x22,
  k_dma_val_30                        = 0x30,
  k_dma_val_33                        = 0x33,
  k_dma_val_40                        = 0x40,
  k_dma_val_44                        = 0x44,
  k_dma_val_50                        = 0x50,
  k_dma_val_60                        = 0x60,
} dma_uint8_const_t;

/**
 * @enum dma_uint16_const_t
 * @brief Named uint16_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint16_t {
  k_dma_ctx_val_abcd = 0xABCD,
} dma_uint16_const_t;

/**
 * @enum dma_uint32_const_t
 * @brief Named uint32_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint32_t {
  k_dma_sentinel_12345678 = 0x12345678U,
  k_dma_sentinel_cafebabe = 0xCAFEBABEU,
  k_dma_sentinel_deadbeef = 0xDEADBEEFU,
} dma_uint32_const_t;

static int32_t s_complete_count    = 0;
static int32_t s_complete_last_ctx = 0;

static void reset_state(void)
{
  ra8_sim_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dma_init());
  s_complete_count    = 0;
  s_complete_last_ctx = 0;
}

static void stub_complete(void* ctx)
{
  ++s_complete_count;
  if (ctx != nullptr) {
    s_complete_last_ctx = *(int32_t*)ctx;
  }
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_marks_channels_free(void)
{
  TEST_BEGIN("ra8_dma_init: all channels free");
  reset_state();

  for (uint8_t ch = 0U; ch < (uint8_t)k_ra8_dma_channel_count; ++ch) {
    bool busy = true;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_dma_channel_is_busy(ch, &busy));
    TEST_ASSERT(!busy);
  }
  TEST_END("ra8_dma_init: all channels free");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_request_allocates_channel_zero(void)
{
  TEST_BEGIN("ra8_dma_request: first allocation gets channel 0");
  reset_state();

  uint8_t src[4] = {k_dma_val_11, k_dma_val_22, k_dma_val_33, k_dma_val_44};
  uint8_t dst[4] = {};

  const ra8_dma_request_t req = {
    .src_addr    = (uintptr_t)src,
    .dst_addr    = (uintptr_t)dst,
    .count       = 4U,
    .width       = k_ra8_dmac_width_byte,
    .src_inc     = true,
    .dst_inc     = true,
    .trigger     = k_ra8_elc_event_none,
    .on_complete = nullptr,
    .ctx         = nullptr,
  };

  uint8_t ch = k_dma_ch_ff;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dma_request(&req, &ch));
  TEST_ASSERT_EQ(0, ch);

  bool busy = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dma_channel_is_busy(ch, &busy));
  TEST_ASSERT(busy);
  TEST_END("ra8_dma_request: first allocation gets channel 0");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_request_rejects_zero_count(void)
{
  TEST_BEGIN("ra8_dma_request: zero count rejected");
  reset_state();

  uint8_t                 buf[4] = {};
  const ra8_dma_request_t req    = {
    .src_addr = (uintptr_t)buf,
    .dst_addr = (uintptr_t)buf,
    .count    = 0U,
    .width    = k_ra8_dmac_width_byte,
  };
  uint8_t ch = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_dma_request(&req, &ch));
  TEST_END("ra8_dma_request: zero count rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_request_null_ptrs(void)
{
  TEST_BEGIN("ra8_dma_request: null pointers rejected");
  reset_state();

  uint8_t ch = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_dma_request(nullptr, &ch));

  uint8_t                 buf[4] = {};
  const ra8_dma_request_t req    = {
    .src_addr = (uintptr_t)buf,
    .dst_addr = (uintptr_t)buf,
    .count    = 1U,
    .width    = k_ra8_dmac_width_byte,
  };
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_dma_request(&req, nullptr));
  TEST_END("ra8_dma_request: null pointers rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_release_round_trip(void)
{
  TEST_BEGIN("ra8_dma_release: channel returns to free pool");
  reset_state();

  uint8_t                 buf[8] = {};
  const ra8_dma_request_t req    = {
    .src_addr = (uintptr_t)buf,
    .dst_addr = (uintptr_t)buf,
    .count    = 4U,
    .width    = k_ra8_dmac_width_byte,
  };
  uint8_t ch = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dma_request(&req, &ch));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_dma_release(ch));

  bool busy = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dma_channel_is_busy(ch, &busy));
  TEST_ASSERT(!busy);

  /* Second release is an error. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_dma_release(ch));
  TEST_END("ra8_dma_release: channel returns to free pool");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_release_bad_channel(void)
{
  TEST_BEGIN("ra8_dma_release: out-of-range rejected");
  reset_state();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_dma_release(99U));
  TEST_END("ra8_dma_release: out-of-range rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_channel_exhaustion(void)
{
  TEST_BEGIN("ra8_dma_request: no-mem when all channels busy");
  reset_state();

  uint8_t                 buf[4] = {};
  const ra8_dma_request_t req    = {
    .src_addr = (uintptr_t)buf,
    .dst_addr = (uintptr_t)buf,
    .count    = 1U,
    .width    = k_ra8_dmac_width_byte,
  };

  for (uint8_t i = 0U; i < (uint8_t)k_ra8_dma_channel_count; ++i) {
    uint8_t ch = 0U;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_dma_request(&req, &ch));
  }
  uint8_t ch = 0U;
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_dma_request(&req, &ch));

  TEST_END("ra8_dma_request: no-mem when all channels busy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_sim_dma_memcpy_byte(void)
{
  TEST_BEGIN("ra8_sim_dma_memcpy: byte transfer");
  reset_state();

  uint8_t src[6] = {0x10, 0x20, k_dma_val_30, k_dma_val_40, k_dma_val_50, k_dma_val_60};
  uint8_t dst[6] = {};
  const ra8_dma_request_t req = {
    .src_addr = (uintptr_t)src,
    .dst_addr = (uintptr_t)dst,
    .count    = 6U,
    .width    = k_ra8_dmac_width_byte,
    .src_inc  = true,
    .dst_inc  = true,
  };
  uint8_t ch = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dma_request(&req, &ch));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_sim_dma_memcpy(ch));

  TEST_ASSERT_EQ(0x10, dst[0]);
  TEST_ASSERT_EQ(0x60, dst[5]);
  TEST_END("ra8_sim_dma_memcpy: byte transfer");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_sim_dma_memcpy_word(void)
{
  TEST_BEGIN("ra8_sim_dma_memcpy: word transfer");
  reset_state();

  uint32_t src[3] = {k_dma_sentinel_deadbeef, k_dma_sentinel_cafebabe, k_dma_sentinel_12345678};
  uint32_t dst[3] = {};
  const ra8_dma_request_t req = {
    .src_addr = (uintptr_t)src,
    .dst_addr = (uintptr_t)dst,
    .count    = 3U,
    .width    = k_ra8_dmac_width_word,
    .src_inc  = true,
    .dst_inc  = true,
  };
  uint8_t ch = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dma_request(&req, &ch));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sim_dma_memcpy(ch));

  TEST_ASSERT_EQ(0xDEADBEEFU, dst[0]);
  TEST_ASSERT_EQ(0xCAFEBABEU, dst[1]);
  TEST_ASSERT_EQ(0x12345678U, dst[2]);
  TEST_END("ra8_sim_dma_memcpy: word transfer");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_sim_dma_complete_fires_callback(void)
{
  TEST_BEGIN("ra8_sim_dma_complete: callback invoked with ctx");
  reset_state();

  int32_t                 ctx_val = k_dma_ctx_val_abcd;
  uint8_t                 buf[4]  = {};
  const ra8_dma_request_t req     = {
    .src_addr    = (uintptr_t)buf,
    .dst_addr    = (uintptr_t)buf,
    .count       = 1U,
    .width       = k_ra8_dmac_width_byte,
    .on_complete = stub_complete,
    .ctx         = &ctx_val,
  };
  uint8_t ch = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dma_request(&req, &ch));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_sim_dma_complete(ch));
  TEST_ASSERT_EQ(1, s_complete_count);
  TEST_ASSERT_EQ(0xABCD, s_complete_last_ctx);
  TEST_END("ra8_sim_dma_complete: callback invoked with ctx");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_dma_request_without_init_fails(void)
{
  TEST_BEGIN("ra8_dma_request: not-initialized rejected");
  ra8_sim_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_init());
  /* Deinit explicitly so s_initialized is false. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dma_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dma_deinit());

  uint8_t                 buf[4] = {};
  const ra8_dma_request_t req    = {
    .src_addr = (uintptr_t)buf,
    .dst_addr = (uintptr_t)buf,
    .count    = 1U,
    .width    = k_ra8_dmac_width_byte,
  };
  uint8_t ch = 0U;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_dma_request(&req, &ch));
  TEST_END("ra8_dma_request: not-initialized rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_channel_is_busy_bad_inputs(void)
{
  TEST_BEGIN("ra8_dma_channel_is_busy: bad inputs");
  reset_state();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_dma_channel_is_busy(0U, nullptr));
  bool busy = false;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_dma_channel_is_busy(99U, &busy));
  TEST_END("ra8_dma_channel_is_busy: bad inputs");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_dispatch_out_of_range(void)
{
  TEST_BEGIN("ra8_dma_dispatch_complete: out-of-range is no-op");
  reset_state();
  /* No assertion -- the call just must not crash. */
  ra8_dma_dispatch_complete(k_dma_ra8_dma_dispatch_complete_200);
  TEST_END("ra8_dma_dispatch_complete: out-of-range is no-op");
}

int32_t main(void)
{
  test_init_marks_channels_free();
  test_request_allocates_channel_zero();
  test_request_rejects_zero_count();
  test_request_null_ptrs();
  test_release_round_trip();
  test_release_bad_channel();
  test_channel_exhaustion();
  test_sim_dma_memcpy_byte();
  test_sim_dma_memcpy_word();
  test_sim_dma_complete_fires_callback();
  test_dma_request_without_init_fails();
  test_channel_is_busy_bad_inputs();
  test_dispatch_out_of_range();
  (void)fprintf(stderr, "[OK  ] test_ra8_dma.c\n");
  return 0;
}
