/**
 * @file test_ra_dma.c
 * @brief Unit tests for the generic DMA transfer substrate (libs/ra_hal/src/ra_dma.c).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra_dma.h"
#include "ra_dmac.h"
#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_sim_dma.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

static int32_t s_complete_count    = 0;
static int32_t s_complete_last_ctx = 0;

static void reset_state(void)
{
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mstp_init());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dma_init());
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

static void test_init_marks_channels_free(void)
{
  TEST_BEGIN("ra_dma_init: all channels free");
  reset_state();

  for (uint8_t ch = 0U; ch < (uint8_t)k_ra_dma_channel_count; ++ch) {
    bool busy = true;
    TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dma_channel_is_busy(ch, &busy));
    TEST_ASSERT(!busy);
  }
  TEST_END("ra_dma_init: all channels free");
}

static void test_request_allocates_channel_zero(void)
{
  TEST_BEGIN("ra_dma_request: first allocation gets channel 0");
  reset_state();

  uint8_t src[4] = {0x11, 0x22, 0x33, 0x44};
  uint8_t dst[4] = {};

  const ra_dma_request_t req = {
    .src_addr    = (uintptr_t)src,
    .dst_addr    = (uintptr_t)dst,
    .count       = 4U,
    .width       = k_ra_dmac_width_byte,
    .src_inc     = true,
    .dst_inc     = true,
    .trigger     = k_ra_elc_event_none,
    .on_complete = nullptr,
    .ctx         = nullptr,
  };

  uint8_t ch = 0xFFU;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dma_request(&req, &ch));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)ch);

  bool busy = false;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dma_channel_is_busy(ch, &busy));
  TEST_ASSERT(busy);
  TEST_END("ra_dma_request: first allocation gets channel 0");
}

static void test_request_rejects_zero_count(void)
{
  TEST_BEGIN("ra_dma_request: zero count rejected");
  reset_state();

  uint8_t                buf[4] = {};
  const ra_dma_request_t req    = {
    .src_addr = (uintptr_t)buf,
    .dst_addr = (uintptr_t)buf,
    .count    = 0U,
    .width    = k_ra_dmac_width_byte,
  };
  uint8_t ch = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_dma_request(&req, &ch));
  TEST_END("ra_dma_request: zero count rejected");
}

static void test_request_null_ptrs(void)
{
  TEST_BEGIN("ra_dma_request: null pointers rejected");
  reset_state();

  uint8_t ch = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_dma_request(nullptr, &ch));

  uint8_t                buf[4] = {};
  const ra_dma_request_t req    = {
    .src_addr = (uintptr_t)buf,
    .dst_addr = (uintptr_t)buf,
    .count    = 1U,
    .width    = k_ra_dmac_width_byte,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_dma_request(&req, nullptr));
  TEST_END("ra_dma_request: null pointers rejected");
}

static void test_release_round_trip(void)
{
  TEST_BEGIN("ra_dma_release: channel returns to free pool");
  reset_state();

  uint8_t                buf[8] = {};
  const ra_dma_request_t req    = {
    .src_addr = (uintptr_t)buf,
    .dst_addr = (uintptr_t)buf,
    .count    = 4U,
    .width    = k_ra_dmac_width_byte,
  };
  uint8_t ch = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dma_request(&req, &ch));

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dma_release(ch));

  bool busy = true;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dma_channel_is_busy(ch, &busy));
  TEST_ASSERT(!busy);

  /* Second release is an error. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_dma_release(ch));
  TEST_END("ra_dma_release: channel returns to free pool");
}

static void test_release_bad_channel(void)
{
  TEST_BEGIN("ra_dma_release: out-of-range rejected");
  reset_state();
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_dma_release(99U));
  TEST_END("ra_dma_release: out-of-range rejected");
}

static void test_channel_exhaustion(void)
{
  TEST_BEGIN("ra_dma_request: no-mem when all channels busy");
  reset_state();

  uint8_t                buf[4] = {};
  const ra_dma_request_t req    = {
    .src_addr = (uintptr_t)buf,
    .dst_addr = (uintptr_t)buf,
    .count    = 1U,
    .width    = k_ra_dmac_width_byte,
  };

  for (uint8_t i = 0U; i < (uint8_t)k_ra_dma_channel_count; ++i) {
    uint8_t ch = 0U;
    TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dma_request(&req, &ch));
  }
  uint8_t ch = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_no_mem, (int32_t)ra_dma_request(&req, &ch));

  TEST_END("ra_dma_request: no-mem when all channels busy");
}

static void test_sim_dma_memcpy_byte(void)
{
  TEST_BEGIN("ra_sim_dma_memcpy: byte transfer");
  reset_state();

  uint8_t                src[6] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
  uint8_t                dst[6] = {};
  const ra_dma_request_t req    = {
    .src_addr = (uintptr_t)src,
    .dst_addr = (uintptr_t)dst,
    .count    = 6U,
    .width    = k_ra_dmac_width_byte,
    .src_inc  = true,
    .dst_inc  = true,
  };
  uint8_t ch = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dma_request(&req, &ch));

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sim_dma_memcpy(ch));

  TEST_ASSERT_EQ((int32_t)0x10, (int32_t)dst[0]);
  TEST_ASSERT_EQ((int32_t)0x60, (int32_t)dst[5]);
  TEST_END("ra_sim_dma_memcpy: byte transfer");
}

static void test_sim_dma_memcpy_word(void)
{
  TEST_BEGIN("ra_sim_dma_memcpy: word transfer");
  reset_state();

  uint32_t               src[3] = {0xDEADBEEFU, 0xCAFEBABEU, 0x12345678U};
  uint32_t               dst[3] = {};
  const ra_dma_request_t req    = {
    .src_addr = (uintptr_t)src,
    .dst_addr = (uintptr_t)dst,
    .count    = 3U,
    .width    = k_ra_dmac_width_word,
    .src_inc  = true,
    .dst_inc  = true,
  };
  uint8_t ch = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dma_request(&req, &ch));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sim_dma_memcpy(ch));

  TEST_ASSERT_EQ((int64_t)0xDEADBEEFU, (int64_t)dst[0]);
  TEST_ASSERT_EQ((int64_t)0xCAFEBABEU, (int64_t)dst[1]);
  TEST_ASSERT_EQ((int64_t)0x12345678U, (int64_t)dst[2]);
  TEST_END("ra_sim_dma_memcpy: word transfer");
}

static void test_sim_dma_complete_fires_callback(void)
{
  TEST_BEGIN("ra_sim_dma_complete: callback invoked with ctx");
  reset_state();

  int32_t                ctx_val = 0xABCD;
  uint8_t                buf[4]  = {};
  const ra_dma_request_t req     = {
    .src_addr    = (uintptr_t)buf,
    .dst_addr    = (uintptr_t)buf,
    .count       = 1U,
    .width       = k_ra_dmac_width_byte,
    .on_complete = stub_complete,
    .ctx         = &ctx_val,
  };
  uint8_t ch = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dma_request(&req, &ch));

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sim_dma_complete(ch));
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_complete_count);
  TEST_ASSERT_EQ((int32_t)0xABCD, (int32_t)s_complete_last_ctx);
  TEST_END("ra_sim_dma_complete: callback invoked with ctx");
}

static void test_dma_request_without_init_fails(void)
{
  TEST_BEGIN("ra_dma_request: not-initialised rejected");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mstp_init());
  /* Deinit explicitly so s_initialized is false. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dma_init());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dma_deinit());

  uint8_t                buf[4] = {};
  const ra_dma_request_t req    = {
    .src_addr = (uintptr_t)buf,
    .dst_addr = (uintptr_t)buf,
    .count    = 1U,
    .width    = k_ra_dmac_width_byte,
  };
  uint8_t ch = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized, (int32_t)ra_dma_request(&req, &ch));
  TEST_END("ra_dma_request: not-initialised rejected");
}

static void test_channel_is_busy_bad_inputs(void)
{
  TEST_BEGIN("ra_dma_channel_is_busy: bad inputs");
  reset_state();

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_dma_channel_is_busy(0U, nullptr));
  bool busy = false;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_dma_channel_is_busy(99U, &busy));
  TEST_END("ra_dma_channel_is_busy: bad inputs");
}

static void test_dispatch_out_of_range(void)
{
  TEST_BEGIN("ra_dma_dispatch_complete: out-of-range is no-op");
  reset_state();
  /* No assertion -- the call just must not crash. */
  ra_dma_dispatch_complete(200U);
  TEST_END("ra_dma_dispatch_complete: out-of-range is no-op");
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
  (void)fprintf(stderr, "[OK  ] test_ra_dma.c\n");
  return 0;
}
