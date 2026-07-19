/**
 * @file test_ra8_dma_cov.c
 * @brief Coverage-gap tests for libs/ra8_hal/src/ra8_dma.c.
 *
 * @details
 * This file is a standalone test executable that exercises the branches
 * in ra8_dma.c that are NOT reached by test_ra8_dma.c.  It is auto-
 * discovered by the GLOB in tests/CMakeLists.txt and linked against
 * the same ra8_core_hal OBJECT library.  gcovr merges the resulting
 * .gcda files so the coverage figures are additive.
 *
 * Uncovered lines targeted (as reported by gcovr):
 *   182           -- internal_validate_request: width out of range
 *   221, 222      -- ra8_dma_init: ra8_mstp_enable failure path
 *   263-267       -- ra8_dma_deinit: body of `if (in_use)` loop
 *   272, 273      -- ra8_dma_deinit: ra8_mstp_disable failure path
 *   330, 331      -- ra8_dma_request: ra8_dmac_start failure path
 *   381, 382      -- ra8_dma_release: ra8_dmac_stop failure path
 *   417, 420      -- ra8_dma_sim_peek_request: out-of-range / not-in-use
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_dma.h"
#include "ra8_dmac.h"
#include "ra8_err.h"
#include "ra8_mstp.h"
#include "ra8_mstp_regs.h"
#include "ra8_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum dma_cov_fixture_t
 * @brief Poison values written into out-parameters before a call, so one that fails without assigning is detectable.
 */
typedef enum : uint8_t {
  k_dma_ch_unset =
    0xFFU, /**< Poison channel id written before an allocate call, so a failed allocate that left the out-parameter alone is detectable. */
  k_dma_alloc_attempts_after_one =
    254U, /**< The same, one fewer, for the case that holds a channel back before exhausting the rest. */
  k_dma_alloc_attempts =
    255U, /**< Allocate attempts: more than the driver has channels, so the loop runs it out and exercises the exhausted arm. */
} dma_cov_fixture_t;

/* -------------------------------------------------------------------------
 * Shared helpers
 * -------------------------------------------------------------------------
 */

/** @brief Reset all sim MMIO, MSTP refcounts, and DMA channel table. */
static void cov_reset(void)
{
  ra8_sim_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dma_init());
}

/** @brief Minimal valid 1-byte request descriptor for scaffolding tests. */
/* The pointer parameters below cannot be const: this mock implements a
 * function-pointer interface (the DI seam under test), so its signature is
 * fixed by the typedef it is assigned to -- adding const changes the
 * function type and the assignment stops compiling. */
// NOLINTNEXTLINE(readability-non-const-parameter)
static ra8_dma_request_t cov_make_req(uint8_t* src, uint8_t* dst)
{
  ra8_dma_request_t r = {};
  r.src_addr          = (uintptr_t)src;
  r.dst_addr          = (uintptr_t)dst;
  r.count             = 1U;
  r.width             = k_ra8_dmac_width_byte;
  r.src_inc           = false;
  r.dst_inc           = false;
  r.on_complete       = nullptr;
  r.ctx               = nullptr;
  return r;
}

/* -------------------------------------------------------------------------
 * Test functions
 * -------------------------------------------------------------------------
 */

/**
 * @brief ra8_dma_request rejects a width value that exceeds the maximum.
 *
 * @details
 * Exercises line 182 of ra8_dma.c: the second guard inside
 * internal_validate_request() returns k_ra8_err_invalid_arg when
 * (uint8_t)req->width > k_ra8_dmac_width_word.
 *
 * @par MC/DC:
 * Decision: `if ((uint8_t)req->width > k_ra8_dmac_width_word)` (1 condition)
 * - Vector 1: width=2 (k_ra8_dmac_width_word) -> false (control; already
 *   covered by test_ra8_dma.c happy-path tests)
 * - Vector 2: width=3 -> true (hits line 182)
 * Single-condition decisions need N+1=2 vectors; both are now provided.
 *
 * @pre ra8_dma_init() has been called.
 * @post No channel is allocated.
 *
 * @note Thread safety: single-threaded test context.
 * @since 0.1.0
 */
static void test_request_invalid_width(void)
{
  TEST_BEGIN("ra8_dma_request: width out of range -> k_ra8_err_invalid_arg");
  cov_reset();

  uint8_t           buf[4] = {};
  ra8_dma_request_t req    = cov_make_req(buf, buf);
  req.width                = (ra8_dmac_width_t)3U; /* one past k_ra8_dmac_width_word */
  req.count                = 1U;
  uint8_t ch               = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_dma_request(&req, &ch));

  TEST_END("ra8_dma_request: width out of range -> k_ra8_err_invalid_arg");
}

/**
 * @brief ra8_dma_init returns k_ra8_err_hw_init_failed when ra8_mstp_enable
 *        fails because the DMAC/DTC0 refcount has been saturated.
 *
 * @details
 * Exercises lines 221-222 of ra8_dma.c: the error branch after
 * `ra8_mstp_enable(k_ra8_mstp_dmac0_dtc0)` inside ra8_dma_init().
 *
 * Setup: after ra8_mstp_init() the refcount is 0.  Calling
 * ra8_mstp_enable(k_ra8_mstp_dmac0_dtc0) 255 times drives the counter to
 * UINT8_MAX.  The next call (inside ra8_dma_init) sees the saturated
 * counter and returns k_ra8_err_invalid_state, so ra8_dma_init returns
 * k_ra8_err_hw_init_failed.
 *
 * @par MC/DC:
 * Decision: `if (mst_err != k_ra8_ok)` inside ra8_dma_init (1 condition)
 * - Vector 1: mst_err==k_ra8_ok -> false (happy path covered by
 *   test_ra8_dma.c::test_init_marks_channels_free)
 * - Vector 2: mst_err==k_ra8_err_invalid_state -> true (this test, line 221)
 * Both vectors are now provided.
 *
 * @pre ra8_sim_mmap_reset() and ra8_mstp_init() complete successfully.
 * @post ra8_dma_init() failed; no channel table was reset.
 *
 * @note Thread safety: single-threaded test context.
 * @since 0.1.0
 */
static void test_init_mstp_enable_fails(void)
{
  TEST_BEGIN("ra8_dma_init: ra8_mstp_enable saturated -> k_ra8_err_hw_init_failed");
  ra8_sim_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_init());

  /* Saturate the DMAC/DTC0 refcount to UINT8_MAX (255 enables). */
  for (uint16_t i = 0U; i < k_dma_alloc_attempts; ++i) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_enable(k_ra8_mstp_dmac0_dtc0));
  }

  /* This ra8_dma_init call drives ra8_mstp_enable into the saturated path. */
  TEST_ASSERT_EQ(k_ra8_err_hw_init_failed, ra8_dma_init());

  TEST_END("ra8_dma_init: ra8_mstp_enable saturated -> k_ra8_err_hw_init_failed");
}

/**
 * @brief ra8_dma_deinit stops and clears every in-use channel.
 *
 * @details
 * Exercises lines 263-267 of ra8_dma.c: the body of the
 * `if (s_channels[ch].in_use)` branch inside ra8_dma_deinit().
 *
 * The existing test_ra8_dma.c::test_dma_request_without_init_fails calls
 * ra8_dma_deinit() only when no channels are allocated, so the inner body
 * (ra8_dmac_stop + table clear) was never reached.  This test allocates a
 * channel first and then calls ra8_dma_deinit() without releasing it.
 *
 * @par MC/DC:
 * Decision: `if (s_channels[ch].in_use)` (1 condition)
 * - Vector 1: in_use==false -> false (skips body; covered by prior test)
 * - Vector 2: in_use==true  -> true  (this test, lines 263-267)
 *
 * @pre ra8_dma_init() has been called.
 * @post ra8_dma_deinit() succeeds; channel is freed and MSTP released.
 *
 * @note Thread safety: single-threaded test context.
 * @since 0.1.0
 */
static void test_deinit_with_channel_in_use(void)
{
  TEST_BEGIN("ra8_dma_deinit: in-use channel is stopped and cleared");
  cov_reset();

  uint8_t           buf[4] = {};
  ra8_dma_request_t req    = cov_make_req(buf, buf);
  uint8_t           ch     = k_dma_ch_unset;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dma_request(&req, &ch));
  TEST_ASSERT_EQ(0U, ch);

  /* Confirm the channel is reported busy before deinit. */
  bool busy = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dma_channel_is_busy(ch, &busy));
  TEST_ASSERT(busy);

  /* Deinit without releasing: must stop channel 0 internally. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dma_deinit());

  /* Channel must be reported not-busy after deinit cleared it. */
  busy = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dma_channel_is_busy(ch, &busy));
  TEST_ASSERT(!busy);

  TEST_END("ra8_dma_deinit: in-use channel is stopped and cleared");
}

/**
 * @brief ra8_dma_deinit returns k_ra8_err_hw_error when ra8_mstp_disable fails.
 *
 * @details
 * Exercises lines 272-273 of ra8_dma.c: the error branch after
 * `ra8_mstp_disable(k_ra8_mstp_dmac0_dtc0)` inside ra8_dma_deinit().
 *
 * Setup: after ra8_dma_init() the DMAC/DTC0 refcount is 1.  A direct call
 * to ra8_mstp_disable() drains it to 0.  When ra8_dma_deinit() then calls
 * ra8_mstp_disable() the refcount underflows and ra8_mstp_disable returns
 * k_ra8_err_invalid_state, causing ra8_dma_deinit to return k_ra8_err_hw_error.
 *
 * @par MC/DC:
 * Decision: `if (mst_err != k_ra8_ok)` inside ra8_dma_deinit (1 condition)
 * - Vector 1: mst_err==k_ra8_ok -> false (happy path covered elsewhere)
 * - Vector 2: mst_err==k_ra8_err_invalid_state -> true (this test, line 272)
 *
 * @pre ra8_dma_init() has been called.
 * @post ra8_dma_deinit() failed with k_ra8_err_hw_error.
 *
 * @note Thread safety: single-threaded test context.
 * @since 0.1.0
 */
static void test_deinit_mstp_disable_fails(void)
{
  TEST_BEGIN("ra8_dma_deinit: ra8_mstp_disable underflow -> k_ra8_err_hw_error");
  ra8_sim_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dma_init()); /* refcount becomes 1 */

  /* Drain the refcount to 0 so the next disable (from ra8_dma_deinit) underflows. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_disable(k_ra8_mstp_dmac0_dtc0));

  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_dma_deinit());

  TEST_END("ra8_dma_deinit: ra8_mstp_disable underflow -> k_ra8_err_hw_error");
}

/**
 * @brief ra8_dma_request returns k_ra8_err_hw_error when ra8_dmac_start fails.
 *
 * @details
 * Exercises lines 330-331 of ra8_dma.c: the error branch after
 * `ra8_dmac_start(ch, &cfg)` inside ra8_dma_request().
 *
 * ra8_dmac_start() calls ra8_mstp_enable(k_ra8_mstp_dmac0_dtc0).  When the
 * refcount is already at UINT8_MAX (255) that call fails with
 * k_ra8_err_invalid_state, which RA8_RETURN_ON_ERROR propagates out of
 * ra8_dmac_start().  ra8_dma_request() then logs the error and returns
 * k_ra8_err_hw_error.
 *
 * Setup: after cov_reset() (which calls ra8_dma_init()) the refcount is 1.
 * 254 additional ra8_mstp_enable() calls bring it to 255 = UINT8_MAX.
 *
 * @par MC/DC:
 * Decision: `if (derr != k_ra8_ok)` inside ra8_dma_request (1 condition)
 * - Vector 1: derr==k_ra8_ok -> false (happy path covered by test_ra8_dma.c)
 * - Vector 2: derr==k_ra8_err_invalid_state -> true (this test, line 330)
 *
 * @pre ra8_dma_init() has been called.
 * @post No channel is allocated; ra8_dma_request returns k_ra8_err_hw_error.
 *
 * @note Thread safety: single-threaded test context.
 * @since 0.1.0
 */
static void test_request_dmac_start_fails(void)
{
  TEST_BEGIN("ra8_dma_request: ra8_dmac_start failure -> k_ra8_err_hw_error");
  cov_reset(); /* refcount = 1 after ra8_dma_init() */

  /* 254 more enables bring the refcount to 255 = UINT8_MAX. */
  for (uint16_t i = 0U; i < k_dma_alloc_attempts_after_one; ++i) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_enable(k_ra8_mstp_dmac0_dtc0));
  }

  uint8_t           buf[4] = {};
  ra8_dma_request_t req    = cov_make_req(buf, buf);
  uint8_t           ch     = k_dma_ch_unset;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_dma_request(&req, &ch));

  TEST_END("ra8_dma_request: ra8_dmac_start failure -> k_ra8_err_hw_error");
}

/**
 * @brief ra8_dma_release returns k_ra8_err_hw_error when ra8_dmac_stop fails.
 *
 * @details
 * Exercises lines 381-382 of ra8_dma.c: the error branch after
 * `ra8_dmac_stop(channel)` inside ra8_dma_release().
 *
 * ra8_dmac_stop() calls ra8_mstp_disable(k_ra8_mstp_dmac0_dtc0).  After
 * ra8_dma_init() + ra8_dma_request() the refcount is 2.  Two direct calls
 * to ra8_mstp_disable() drain it to 0.  When ra8_dma_release() subsequently
 * calls ra8_dmac_stop() the internal ra8_mstp_disable() underflows and
 * returns k_ra8_err_invalid_state, causing ra8_dma_release to return
 * k_ra8_err_hw_error.
 *
 * @par MC/DC:
 * Decision: `if (stop_err != k_ra8_ok)` inside ra8_dma_release (1 condition)
 * - Vector 1: stop_err==k_ra8_ok -> false (happy path covered by test_ra8_dma.c)
 * - Vector 2: stop_err==k_ra8_err_invalid_state -> true (this test, line 381)
 *
 * @pre ra8_dma_init() and ra8_dma_request() both succeeded.
 * @post ra8_dma_release returns k_ra8_err_hw_error; channel remains in_use.
 *
 * @note Thread safety: single-threaded test context.
 * @since 0.1.0
 */
static void test_release_dmac_stop_fails(void)
{
  TEST_BEGIN("ra8_dma_release: ra8_dmac_stop failure -> k_ra8_err_hw_error");
  cov_reset(); /* refcount = 1 after ra8_dma_init() */

  uint8_t           buf[4] = {};
  ra8_dma_request_t req    = cov_make_req(buf, buf);
  uint8_t           ch     = k_dma_ch_unset;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dma_request(&req, &ch)); /* refcount = 2 */

  /* Drain the refcount to 0: two disables drop 2->1->0. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_disable(k_ra8_mstp_dmac0_dtc0));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_disable(k_ra8_mstp_dmac0_dtc0));

  /* ra8_dma_release -> ra8_dmac_stop -> ra8_mstp_disable underflows. */
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_dma_release(ch));

  TEST_END("ra8_dma_release: ra8_dmac_stop failure -> k_ra8_err_hw_error");
}

/**
 * @brief ra8_dma_sim_peek_request returns nullptr for out-of-range and
 *        not-in-use channels.
 *
 * @details
 * Exercises lines 417 and 420 of ra8_dma.c inside ra8_dma_sim_peek_request()
 * (RA8_SIMULATOR_MODE only):
 *   - Line 417: `return nullptr;` when channel >= k_ra8_dma_channel_count.
 *   - Line 420: `return nullptr;` when the slot is not in use.
 *
 * @par MC/DC:
 * Decision 1: `if (channel >= k_ra8_dma_channel_count)` (1 condition)
 * - Vector 1: channel==0 (in-range) -> false  (needed for line-420 case)
 * - Vector 2: channel==255 (out-of-range) -> true (line 417)
 *
 * Decision 2: `if (!s_channels[channel].in_use)` (1 condition)
 * - Vector 1: channel==0, not in use -> true (line 420)
 * - Vector 2: channel==0, in use -> false (covered by test_sim_dma_memcpy_*
 *   in test_ra8_dma.c which peek-request via ra8_sim_dma_memcpy internals)
 *
 * @pre ra8_dma_init() has been called.
 * @post No state mutated.
 *
 * @note Compiled only under RA8_SIMULATOR_MODE; guarded by the same ifdef.
 * @since 0.1.0
 */
static void test_sim_peek_request_edge_cases(void)
{
  TEST_BEGIN("ra8_dma_sim_peek_request: out-of-range and not-in-use -> nullptr");
  cov_reset();

  /* Line 417: channel >= k_ra8_dma_channel_count. */
  TEST_ASSERT_NULL(ra8_dma_sim_peek_request(255U));

  /* Line 420: channel in range but slot not in use. */
  TEST_ASSERT_NULL(ra8_dma_sim_peek_request(0U));

  TEST_END("ra8_dma_sim_peek_request: out-of-range and not-in-use -> nullptr");
}

/* -------------------------------------------------------------------------
 * Main
 * -------------------------------------------------------------------------
 */

int32_t main(void)
{
  test_request_invalid_width();
  test_init_mstp_enable_fails();
  test_deinit_with_channel_in_use();
  test_deinit_mstp_disable_fails();
  test_request_dmac_start_fails();
  test_release_dmac_stop_fails();
  test_sim_peek_request_edge_cases();
  (void)fprintf(stderr, "[OK  ] test_ra8_dma_cov.c\n");
  return 0;
}
