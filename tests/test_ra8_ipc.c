/**
 * @file test_ra8_ipc.c
 * @brief Unit tests for ra8_ipc.c (Inter-Processor Communication driver)
 *
 * @details Core mailbox contract tests: lifecycle, event masks, message
 * send/recv (including retry and burst paths), status, and attribution
 * decode. Siblings: test_ra8_ipc_irq.c (dispatch + NMI + ISR install),
 * test_ra8_ipc_sem.c (hardware semaphores), test_ra8_ipc_ring.c (ring
 * buffer + channel-helper sweeps + MC/DC). Shared fixture state lives
 * in support/ipc_test_util.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_fake_mmio.h"
#include "ra8_ipc.h"
#include "ra8_ipc_regs.h"
#include "ra8_isr.h"
#include "support/ipc_test_util.h"
#include "unity_minimal.h"

/**
 * @enum ipc_test_lit_t
 * @brief Named constants for the register stamp patterns and literal
 *        test vectors previously inlined in this file's test bodies.
 */
typedef enum : uint32_t {
  k_ipc_test_sentinel = 0xFFU,        /**< Ipc test sentinel.  */
  k_ipc_lit_xcafe     = 0xCAFEU,      /**< Ipc literal 0xCAFE. */
  k_ipc_lit_xff       = 0xFFU,        /**< Ipc literal 0xFF.   */
  k_ipc_stamp_clr     = 0xA5A5A5A5UL, /**< Ipc stamp clr.      */
} ipc_test_lit_t;

/* ---------- Lifecycle tests ---------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_happy(void)
{
  TEST_BEGIN("ipc init happy");
  prep();
  volatile r_ipc_channel_regs_t* reg = ra8_ipc_channel((uint8_t)k_ra8_ipc_test_ch_first);
  TEST_ASSERT_NOT_NULL((void*)reg);
  const ra8_ipc_config_t cfg = make_cfg((uint8_t)k_ra8_ipc_test_ch_first);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_init(&cfg));
  TEST_END("ipc init happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_null_cfg(void)
{
  TEST_BEGIN("ipc init null cfg");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ipc_init(nullptr));
  TEST_END("ipc init null cfg");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_bad_channel(void)
{
  TEST_BEGIN("ipc init bad channel");
  prep();
  ra8_ipc_config_t cfg = make_cfg((uint8_t)k_ra8_ipc_test_ch_bad);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ipc_init(&cfg));
  cfg.channel = (uint8_t)k_ra8_ipc_test_ch_way;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ipc_init(&cfg));
  TEST_END("ipc init bad channel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_deinit_clears_state(void)
{
  TEST_BEGIN("ipc deinit clears state");
  prep();
  const ra8_ipc_config_t cfg = make_cfg((uint8_t)k_ra8_ipc_test_ch_mid);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_deinit((uint8_t)k_ra8_ipc_test_ch_mid));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ipc_deinit((uint8_t)k_ra8_ipc_test_ch_bad));
  TEST_END("ipc deinit clears state");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_reset_fifo(void)
{
  TEST_BEGIN("ipc reset_fifo");
  prep();
  const ra8_ipc_config_t cfg = make_cfg((uint8_t)k_ra8_ipc_test_ch_first);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_init(&cfg));
  volatile r_ipc_channel_regs_t* reg = ra8_ipc_channel((uint8_t)k_ra8_ipc_test_ch_first);
  TEST_ASSERT_NOT_NULL((void*)reg);
  reg->CLR = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_reset_fifo((uint8_t)k_ra8_ipc_test_ch_first));
  TEST_ASSERT_EQ(k_ra8_ipc_clr_mask_rst, reg->CLR);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ipc_reset_fifo((uint8_t)k_ra8_ipc_test_ch_bad));
  TEST_END("ipc reset_fifo");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_event_mask(void)
{
  TEST_BEGIN("ipc set_event_mask");
  prep();
  const ra8_ipc_config_t cfg = make_cfg((uint8_t)k_ra8_ipc_test_ch_first);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_init(&cfg));

  /* New mask -> only RDY in scope; staging an IRQ0 should NOT fire
   * the per-event handler we attach. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_ipc_set_event_mask((uint8_t)k_ra8_ipc_test_ch_first, (uint32_t)k_ra8_ipc_event_msg_ready));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ipc_set_event_mask((uint8_t)k_ra8_ipc_test_ch_bad, 0U));

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ipc_attach_event_handler((uint8_t)k_ra8_ipc_test_ch_first,
                                              k_ra8_ipc_irq_event_0,
                                              stub_ipc_irq_cb,
                                              (void*)(uintptr_t)k_ra8_ipc_test_irq_ctx));

  volatile r_ipc_channel_regs_t* reg = ra8_ipc_channel((uint8_t)k_ra8_ipc_test_ch_first);
  TEST_ASSERT_NOT_NULL((void*)reg);
  reg->STA = (uint32_t)k_ra8_ipc_sta_mask_irq0;
  ra8_ipc_dispatch((uint8_t)k_ra8_ipc_test_ch_first);
  /* Filter blocked the IRQ0; per-event callback NOT fired. */
  TEST_ASSERT_EQ(0, s_ipc_irq_cb_count);
  TEST_END("ipc set_event_mask");
}

/* ---------- Channel-pair convention tests ---------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_channel_pair_convention(void)
{
  TEST_BEGIN("ipc channel-pair convention");
  prep();
  uint8_t ch = k_ipc_test_sentinel;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_channel_for_send(k_ra8_ipc_core_cpu0, 0U, &ch));
  TEST_ASSERT_EQ(2, ch);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_channel_for_send(k_ra8_ipc_core_cpu0, 1U, &ch));
  TEST_ASSERT_EQ(3, ch);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_channel_for_send(k_ra8_ipc_core_cpu1, 0U, &ch));
  TEST_ASSERT_EQ(0, ch);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_channel_for_recv(k_ra8_ipc_core_cpu0, 1U, &ch));
  TEST_ASSERT_EQ(1, ch);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_channel_for_recv(k_ra8_ipc_core_cpu1, 1U, &ch));
  TEST_ASSERT_EQ(3, ch);

  /* Bad-arg paths */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ipc_channel_for_send(k_ra8_ipc_core_cpu0, 0U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ipc_channel_for_send((ra8_ipc_core_id_t)9U, 0U, &ch));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ipc_channel_for_send(k_ra8_ipc_core_cpu0, 5U, &ch));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ipc_channel_for_recv(k_ra8_ipc_core_cpu0, 0U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ipc_channel_for_recv((ra8_ipc_core_id_t)9U, 0U, &ch));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ipc_channel_for_recv(k_ra8_ipc_core_cpu0, 5U, &ch));
  TEST_END("ipc channel-pair convention");
}

/* ---------- Send / receive tests ---------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_send_event_writes_iset(void)
{
  TEST_BEGIN("ipc send_event writes ISET");
  prep();
  const ra8_ipc_config_t cfg = make_cfg((uint8_t)k_ra8_ipc_test_ch_last);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ipc_send_event((uint8_t)k_ra8_ipc_test_ch_last, k_ra8_ipc_irq_event_3));
  volatile r_ipc_channel_regs_t* reg = ra8_ipc_channel((uint8_t)k_ra8_ipc_test_ch_last);
  TEST_ASSERT_NOT_NULL((void*)reg);
  TEST_ASSERT_EQ(((uint32_t)1U << 3U), reg->ISET);
  TEST_END("ipc send_event writes ISET");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_send_event_bad_args(void)
{
  TEST_BEGIN("ipc send_event bad args");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ipc_send_event((uint8_t)k_ra8_ipc_test_ch_bad, k_ra8_ipc_irq_event_0));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ipc_send_event((uint8_t)k_ra8_ipc_test_ch_first,
                                    (ra8_ipc_irq_event_id_t)k_ra8_ipc_irq_event_count));
  TEST_END("ipc send_event bad args");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_clear_event_writes_clr(void)
{
  TEST_BEGIN("ipc clear_event writes CLR");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ipc_clear_event((uint8_t)k_ra8_ipc_test_ch_first, k_ra8_ipc_irq_event_5));
  volatile r_ipc_channel_regs_t* reg = ra8_ipc_channel((uint8_t)k_ra8_ipc_test_ch_first);
  TEST_ASSERT_NOT_NULL((void*)reg);
  TEST_ASSERT_EQ(((uint32_t)1U << 5U), reg->CLR);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ipc_clear_event((uint8_t)k_ra8_ipc_test_ch_bad, k_ra8_ipc_irq_event_0));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ipc_clear_event((uint8_t)k_ra8_ipc_test_ch_first,
                                     (ra8_ipc_irq_event_id_t)k_ra8_ipc_irq_event_count));
  TEST_END("ipc clear_event writes CLR");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_send_message_happy(void)
{
  TEST_BEGIN("ipc send_message happy");
  prep();
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_ipc_send_message((uint8_t)k_ra8_ipc_test_ch_first, (uint32_t)k_ra8_ipc_test_msg_a));
  volatile r_ipc_channel_regs_t* reg = ra8_ipc_channel((uint8_t)k_ra8_ipc_test_ch_first);
  TEST_ASSERT_NOT_NULL((void*)reg);
  TEST_ASSERT_EQ(k_ra8_ipc_test_msg_a, reg->TXD);
  TEST_END("ipc send_message happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_send_message_full_returns_busy(void)
{
  TEST_BEGIN("ipc send_message full");
  prep();
  volatile r_ipc_channel_regs_t* reg = ra8_ipc_channel((uint8_t)k_ra8_ipc_test_ch_first);
  TEST_ASSERT_NOT_NULL((void*)reg);
  reg->STA = (uint32_t)k_ra8_ipc_sta_mask_full;
  TEST_ASSERT_EQ(
    k_ra8_err_busy,
    ra8_ipc_send_message((uint8_t)k_ra8_ipc_test_ch_first, (uint32_t)k_ra8_ipc_test_msg_b));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_ipc_send_message((uint8_t)k_ra8_ipc_test_ch_bad, (uint32_t)k_ra8_ipc_test_msg_b));
  TEST_END("ipc send_message full");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_send_message_retry_eventually_succeeds(void)
{
  TEST_BEGIN("ipc send_message_retry eventually succeeds");
  prep();
  volatile r_ipc_channel_regs_t* reg = ra8_ipc_channel((uint8_t)k_ra8_ipc_test_ch_first);
  TEST_ASSERT_NOT_NULL((void*)reg);
  /* Start with FIFO not full -> immediate success on the first try. */
  reg->STA = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ipc_send_message_retry((uint8_t)k_ra8_ipc_test_ch_first,
                                            (uint32_t)k_ra8_ipc_test_msg_c,
                                            8U));
  TEST_ASSERT_EQ(k_ra8_ipc_test_msg_c, reg->TXD);
  TEST_END("ipc send_message_retry eventually succeeds");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_send_message_retry_times_out(void)
{
  TEST_BEGIN("ipc send_message_retry times out");
  prep();
  volatile r_ipc_channel_regs_t* reg = ra8_ipc_channel((uint8_t)k_ra8_ipc_test_ch_first);
  TEST_ASSERT_NOT_NULL((void*)reg);
  reg->STA = (uint32_t)k_ra8_ipc_sta_mask_full; /* permanently full */
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_ipc_send_message_retry((uint8_t)k_ra8_ipc_test_ch_first,
                                            (uint32_t)k_ra8_ipc_test_msg_a,
                                            2U));
  /* Driver should have written CLR.FCLR each iteration. */
  TEST_ASSERT((reg->CLR & (uint32_t)k_ra8_ipc_clr_mask_fclr) != 0U);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ipc_send_message_retry((uint8_t)k_ra8_ipc_test_ch_bad, 0U, 1U));
  TEST_END("ipc send_message_retry times out");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_send_burst_partial_on_full(void)
{
  TEST_BEGIN("ipc send_burst partial on full");
  prep();
  volatile r_ipc_channel_regs_t* reg = ra8_ipc_channel((uint8_t)k_ra8_ipc_test_ch_first);
  TEST_ASSERT_NOT_NULL((void*)reg);
  reg->STA              = 0U;
  const uint32_t data[] = {
    0x11111111U,
    0x22222222U,
    0x33333333U,
    0x44444444U,
    0x55555555U,
    0x66666666U,
  };
  uint32_t written = k_ipc_test_sentinel;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ipc_send_burst((uint8_t)k_ra8_ipc_test_ch_first,
                                    data,
                                    (uint32_t)k_ra8_ipc_test_burst,
                                    &written));
  /* Fake mmap returns whatever we wrote; STA stays at 0 so all 6 land. */
  TEST_ASSERT_EQ(k_ra8_ipc_test_burst, written);
  TEST_ASSERT_EQ(0x66666666U, reg->TXD);

  /* Force STA.FULL high -> 0 written, still k_ra8_ok. */
  reg->STA = (uint32_t)k_ra8_ipc_sta_mask_full;
  written  = k_ipc_test_sentinel;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ipc_send_burst((uint8_t)k_ra8_ipc_test_ch_first, data, 3U, &written));
  TEST_ASSERT_EQ(0, written);

  /* Bad args */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_ipc_send_burst((uint8_t)k_ra8_ipc_test_ch_first, nullptr, 1U, &written));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_ipc_send_burst((uint8_t)k_ra8_ipc_test_ch_first, data, 1U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ipc_send_burst((uint8_t)k_ra8_ipc_test_ch_bad, data, 1U, &written));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ipc_send_burst((uint8_t)k_ra8_ipc_test_ch_first, data, 0U, &written));
  TEST_END("ipc send_burst partial on full");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_recv_message_no_data(void)
{
  TEST_BEGIN("ipc recv_message no data");
  prep();
  uint32_t msg = k_ipc_lit_xcafe;
  TEST_ASSERT_EQ(k_ra8_err_no_data, ra8_ipc_recv_message((uint8_t)k_ra8_ipc_test_ch_first, &msg));
  TEST_ASSERT_EQ(0xCAFEU, msg);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_ipc_recv_message((uint8_t)k_ra8_ipc_test_ch_first, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ipc_recv_message((uint8_t)k_ra8_ipc_test_ch_bad, &msg));
  TEST_END("ipc recv_message no data");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_recv_message_happy(void)
{
  TEST_BEGIN("ipc recv_message happy");
  prep();
  volatile r_ipc_channel_regs_t* reg = ra8_ipc_channel((uint8_t)k_ra8_ipc_test_ch_first);
  TEST_ASSERT_NOT_NULL((void*)reg);
  reg->STA     = (uint32_t)k_ra8_ipc_sta_mask_rdy;
  reg->RXD     = (uint32_t)k_ra8_ipc_test_msg_a;
  uint32_t msg = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_recv_message((uint8_t)k_ra8_ipc_test_ch_first, &msg));
  TEST_ASSERT_EQ(k_ra8_ipc_test_msg_a, msg);
  TEST_END("ipc recv_message happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_recv_message_retry_succeeds(void)
{
  TEST_BEGIN("ipc recv_message_retry succeeds");
  prep();
  volatile r_ipc_channel_regs_t* reg = ra8_ipc_channel((uint8_t)k_ra8_ipc_test_ch_first);
  TEST_ASSERT_NOT_NULL((void*)reg);
  reg->STA     = (uint32_t)k_ra8_ipc_sta_mask_rdy;
  reg->RXD     = (uint32_t)k_ra8_ipc_test_msg_b;
  uint32_t msg = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_recv_message_retry((uint8_t)k_ra8_ipc_test_ch_first, &msg, 4U));
  TEST_ASSERT_EQ(k_ra8_ipc_test_msg_b, msg);
  TEST_END("ipc recv_message_retry succeeds");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_recv_message_retry_times_out(void)
{
  TEST_BEGIN("ipc recv_message_retry times out");
  prep();
  uint32_t msg = 0U;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_ipc_recv_message_retry((uint8_t)k_ra8_ipc_test_ch_first, &msg, 2U));
  volatile r_ipc_channel_regs_t* reg = ra8_ipc_channel((uint8_t)k_ra8_ipc_test_ch_first);
  TEST_ASSERT_NOT_NULL((void*)reg);
  TEST_ASSERT((reg->CLR & (uint32_t)k_ra8_ipc_clr_mask_rclr) != 0U);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_ipc_recv_message_retry((uint8_t)k_ra8_ipc_test_ch_first, nullptr, 1U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ipc_recv_message_retry((uint8_t)k_ra8_ipc_test_ch_bad, &msg, 1U));
  TEST_END("ipc recv_message_retry times out");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_recv_burst(void)
{
  TEST_BEGIN("ipc recv_burst");
  prep();
  volatile r_ipc_channel_regs_t* reg = ra8_ipc_channel((uint8_t)k_ra8_ipc_test_ch_first);
  TEST_ASSERT_NOT_NULL((void*)reg);
  reg->STA        = (uint32_t)k_ra8_ipc_sta_mask_rdy;
  reg->RXD        = (uint32_t)k_ra8_ipc_test_msg_a;
  uint32_t buf[3] = {0U, 0U, 0U};
  uint32_t got    = k_ipc_lit_xff;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_recv_burst((uint8_t)k_ra8_ipc_test_ch_first, buf, 3U, &got));
  TEST_ASSERT(got == 3U);
  TEST_ASSERT_EQ(k_ra8_ipc_test_msg_a, buf[0]);

  /* RDY low -> 0 read */
  reg->STA = 0U;
  got      = k_ipc_lit_xff;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_recv_burst((uint8_t)k_ra8_ipc_test_ch_first, buf, 3U, &got));
  TEST_ASSERT_EQ(0, got);

  /* Bad args */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_ipc_recv_burst((uint8_t)k_ra8_ipc_test_ch_first, nullptr, 1U, &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_ipc_recv_burst((uint8_t)k_ra8_ipc_test_ch_first, buf, 1U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ipc_recv_burst((uint8_t)k_ra8_ipc_test_ch_bad, buf, 1U, &got));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ipc_recv_burst((uint8_t)k_ra8_ipc_test_ch_first, buf, 0U, &got));
  TEST_END("ipc recv_burst");
}

/* ---------- Status tests ---------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_status_passthrough(void)
{
  TEST_BEGIN("ipc get_status passthrough");
  prep();
  volatile r_ipc_channel_regs_t* reg = ra8_ipc_channel((uint8_t)k_ra8_ipc_test_ch_first);
  TEST_ASSERT_NOT_NULL((void*)reg);
  reg->STA     = (uint32_t)k_ra8_ipc_sta_mask_rdy | (uint32_t)k_ra8_ipc_sta_mask_ferr;
  uint32_t sta = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_get_status((uint8_t)k_ra8_ipc_test_ch_first, &sta));
  TEST_ASSERT_EQ(((uint32_t)k_ra8_ipc_sta_mask_rdy | (uint32_t)k_ra8_ipc_sta_mask_ferr), sta);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ipc_get_status((uint8_t)k_ra8_ipc_test_ch_first, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ipc_get_status((uint8_t)k_ra8_ipc_test_ch_bad, &sta));
  TEST_END("ipc get_status passthrough");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_clear_status_translates_bits(void)
{
  TEST_BEGIN("ipc clear_status translates bits");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ipc_clear_status(
                   (uint8_t)k_ra8_ipc_test_ch_first,
                   (uint32_t)k_ra8_ipc_event_irq2 | (uint32_t)k_ra8_ipc_event_msg_ready |
                     (uint32_t)k_ra8_ipc_event_err_empty | (uint32_t)k_ra8_ipc_event_err_full));
  volatile r_ipc_channel_regs_t* reg = ra8_ipc_channel((uint8_t)k_ra8_ipc_test_ch_first);
  TEST_ASSERT_NOT_NULL((void*)reg);
  const uint32_t expected = ((uint32_t)1U << 2U) | (uint32_t)k_ra8_ipc_clr_mask_rst |
                            (uint32_t)k_ra8_ipc_clr_mask_rclr | (uint32_t)k_ra8_ipc_clr_mask_fclr;
  TEST_ASSERT_EQ(expected, reg->CLR);
  reg->CLR = k_ipc_stamp_clr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_clear_status((uint8_t)k_ra8_ipc_test_ch_first, 0U));
  TEST_ASSERT_EQ(0xA5A5A5A5UL, reg->CLR);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ipc_clear_status((uint8_t)k_ra8_ipc_test_ch_bad, 0U));
  TEST_END("ipc clear_status translates bits");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_clear_errors(void)
{
  TEST_BEGIN("ipc clear_errors");
  prep();
  volatile r_ipc_channel_regs_t* reg = ra8_ipc_channel((uint8_t)k_ra8_ipc_test_ch_first);
  TEST_ASSERT_NOT_NULL((void*)reg);
  reg->CLR = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_clear_errors((uint8_t)k_ra8_ipc_test_ch_first));
  const uint32_t expected = (uint32_t)k_ra8_ipc_clr_mask_rclr | (uint32_t)k_ra8_ipc_clr_mask_fclr;
  TEST_ASSERT_EQ(expected, reg->CLR);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ipc_clear_errors((uint8_t)k_ra8_ipc_test_ch_bad));
  TEST_END("ipc clear_errors");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_can_send_and_has_data(void)
{
  TEST_BEGIN("ipc can_send + has_data");
  prep();
  volatile r_ipc_channel_regs_t* reg = ra8_ipc_channel((uint8_t)k_ra8_ipc_test_ch_first);
  TEST_ASSERT_NOT_NULL((void*)reg);
  reg->STA = 0U;
  bool can = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_can_send((uint8_t)k_ra8_ipc_test_ch_first, &can));
  TEST_ASSERT(can == true);
  reg->STA = (uint32_t)k_ra8_ipc_sta_mask_full;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_can_send((uint8_t)k_ra8_ipc_test_ch_first, &can));
  TEST_ASSERT(can == false);

  bool has = true;
  reg->STA = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_has_data((uint8_t)k_ra8_ipc_test_ch_first, &has));
  TEST_ASSERT(has == false);
  reg->STA = (uint32_t)k_ra8_ipc_sta_mask_rdy;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_has_data((uint8_t)k_ra8_ipc_test_ch_first, &has));
  TEST_ASSERT(has == true);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ipc_can_send((uint8_t)k_ra8_ipc_test_ch_first, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ipc_has_data((uint8_t)k_ra8_ipc_test_ch_first, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ipc_can_send((uint8_t)k_ra8_ipc_test_ch_bad, &can));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ipc_has_data((uint8_t)k_ra8_ipc_test_ch_bad, &has));
  TEST_END("ipc can_send + has_data");
}

/* ---------- Attribution tests ---------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_attribution_decodes_ipcsar(void)
{
  TEST_BEGIN("ipc get_attribution decodes IPCSAR/IPCPAR");
  prep();
  volatile uint32_t* sar = ra8_ipc_ipcsar();
  volatile uint32_t* par = ra8_ipc_ipcpar();
  TEST_ASSERT_NOT_NULL((void*)sar);
  TEST_ASSERT_NOT_NULL((void*)par);
  *sar                = (uint32_t)k_ra8_ipcsar_mask_saipcir1;
  *par                = 0U;
  ra8_ipc_attr_t attr = {.secure = false, .privileged = true};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_get_attribution(1U, &attr));
  TEST_ASSERT(attr.secure == true);
  TEST_ASSERT(attr.privileged == false);
  attr.secure     = true;
  attr.privileged = true;
  *sar            = 0U;
  *par            = (uint32_t)k_ra8_ipcpar_mask_paipcir0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_get_attribution(0U, &attr));
  TEST_ASSERT(attr.secure == false);
  TEST_ASSERT(attr.privileged == true);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ipc_get_attribution(0U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ipc_get_attribution((uint8_t)k_ra8_ipc_test_ch_bad, &attr));
  TEST_END("ipc get_attribution decodes IPCSAR/IPCPAR");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_nmi_attribution(void)
{
  TEST_BEGIN("ipc get_nmi_attribution");
  prep();
  volatile uint32_t* sar = ra8_ipc_ipcsar();
  volatile uint32_t* par = ra8_ipc_ipcpar();
  *sar                   = (uint32_t)k_ra8_ipcsar_mask_saipcnmi1;
  *par                   = (uint32_t)k_ra8_ipcpar_mask_paipcnmi0;

  ra8_ipc_attr_t attr = {.secure = false, .privileged = false};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_get_nmi_attribution((uint8_t)k_ra8_ipc_unit_ipc1, &attr));
  TEST_ASSERT(attr.secure == true);
  TEST_ASSERT(attr.privileged == false);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_get_nmi_attribution((uint8_t)k_ra8_ipc_unit_ipc0, &attr));
  TEST_ASSERT(attr.secure == false);
  TEST_ASSERT(attr.privileged == true);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ipc_get_nmi_attribution(0U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ipc_get_nmi_attribution((uint8_t)k_ra8_ipc_test_unit_bad, &attr));
  TEST_END("ipc get_nmi_attribution");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_sem_attribution(void)
{
  TEST_BEGIN("ipc get_sem_attribution");
  prep();
  volatile uint32_t* sar = ra8_ipc_ipcsar();
  volatile uint32_t* par = ra8_ipc_ipcpar();
  *sar                   = (uint32_t)k_ra8_ipcsar_mask_saipcsem1;
  *par                   = (uint32_t)k_ra8_ipcpar_mask_paipcsem0;

  ra8_ipc_attr_t attr = {.secure = false, .privileged = false};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_get_sem_attribution(k_ra8_ipc_sem_group_high, &attr));
  TEST_ASSERT(attr.secure == true);
  TEST_ASSERT(attr.privileged == false);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_get_sem_attribution(k_ra8_ipc_sem_group_low, &attr));
  TEST_ASSERT(attr.secure == false);
  TEST_ASSERT(attr.privileged == true);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ipc_get_sem_attribution(k_ra8_ipc_sem_group_low, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ipc_get_sem_attribution((ra8_ipc_sem_attr_group_t)9U, &attr));
  TEST_END("ipc get_sem_attribution");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_can_access(void)
{
  TEST_BEGIN("ipc can_access predicate");
  prep();
  volatile uint32_t* sar = ra8_ipc_ipcsar();
  volatile uint32_t* par = ra8_ipc_ipcpar();
  /* Channel 2 -> non-secure + unprivileged. */
  *sar                = (uint32_t)k_ra8_ipcsar_mask_saipcir2;
  *par                = (uint32_t)k_ra8_ipcpar_mask_paipcir2;
  ra8_ipc_attr_t want = {.secure = true, .privileged = true};
  bool           ok   = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_can_access((uint8_t)k_ra8_ipc_test_ch_mid, &want, &ok));
  TEST_ASSERT(ok == true);
  want.secure = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_can_access((uint8_t)k_ra8_ipc_test_ch_mid, &want, &ok));
  TEST_ASSERT(ok == false);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_ipc_can_access((uint8_t)k_ra8_ipc_test_ch_mid, nullptr, &ok));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_ipc_can_access((uint8_t)k_ra8_ipc_test_ch_mid, &want, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ipc_can_access((uint8_t)k_ra8_ipc_test_ch_bad, &want, &ok));
  TEST_END("ipc can_access predicate");
}

/**
 * @var s_test_roster
 * @brief Fixed-order roster of every test case in this translation unit.
 *
 * @details
 * main() walks this table instead of naming each case, so its size does not
 * grow with the number of tests and adding a case is a one-line edit.
 *
 * @note Order is significant: cases run top to bottom, exactly as before.
 */
static void (*const s_test_roster[])(void) = {
  test_init_happy,
  test_init_null_cfg,
  test_init_bad_channel,
  test_deinit_clears_state,
  test_reset_fifo,
  test_set_event_mask,
  test_channel_pair_convention,
  test_send_event_writes_iset,
  test_send_event_bad_args,
  test_clear_event_writes_clr,
  test_send_message_happy,
  test_send_message_full_returns_busy,
  test_send_message_retry_eventually_succeeds,
  test_send_message_retry_times_out,
  test_send_burst_partial_on_full,
  test_recv_message_no_data,
  test_recv_message_happy,
  test_recv_message_retry_succeeds,
  test_recv_message_retry_times_out,
  test_recv_burst,
  test_get_status_passthrough,
  test_clear_status_translates_bits,
  test_clear_errors,
  test_can_send_and_has_data,
  test_get_attribution_decodes_ipcsar,
  test_get_nmi_attribution,
  test_get_sem_attribution,
  test_can_access,
};

int32_t main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  (void)fprintf(stderr, "[OK  ] test_ra8_ipc.c\n");
  return 0;
}
