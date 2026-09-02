/**
 * @file test_ra8_ipc_irq.c
 * @brief Unit tests for the ra8_ipc dispatch, per-event IRQ decode, NMI,
 *        and ISR registration paths
 *
 * @details Split from test_ra8_ipc.c along the test-group seam. Shared
 * fixture state (stub callbacks, prep(), make_cfg()) lives in
 * support/inc/ipc_test_util.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ipc_test_util.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_fake_mmio.h"
#include "ra8_ipc.h"
#include "ra8_ipc_regs.h"
#include "ra8_isr.h"
#include "unity_minimal.h"

/* ---------- Dispatch tests ---------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_and_dispatch_message(void)
{
  TEST_BEGIN("ipc attach + dispatch message");
  prep();
  const ra8_ipc_config_t cfg = make_cfg((uint8_t)k_ra8_ipc_test_ch_first);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ipc_attach_handler(stub_ipc_cb, (void*)(uintptr_t)k_ra8_ipc_test_attr_ctx));
  volatile r_ipc_channel_regs_t* reg = ra8_ipc_channel((uint8_t)k_ra8_ipc_test_ch_first);
  TEST_ASSERT_NOT_NULL((void*)reg);
  reg->CLR = 0U;
  reg->STA = (uint32_t)k_ra8_ipc_sta_mask_rdy | 0x01U;
  reg->RXD = (uint32_t)k_ra8_ipc_test_msg_b;
  ra8_ipc_dispatch((uint8_t)k_ra8_ipc_test_ch_first);
  TEST_ASSERT_EQ(1, s_ipc_cb_count);
  TEST_ASSERT_EQ(k_ra8_ipc_test_ch_first, s_ipc_cb_last_channel);
  TEST_ASSERT_EQ(k_ra8_ipc_test_msg_b, s_ipc_cb_last_message);
  TEST_ASSERT((s_ipc_cb_last_event_mask & (uint32_t)k_ra8_ipc_event_msg_ready) != 0U);
  TEST_ASSERT((s_ipc_cb_last_event_mask & (uint32_t)k_ra8_ipc_event_irq0) != 0U);
  TEST_ASSERT((uintptr_t)s_ipc_cb_last_ctx == (uintptr_t)k_ra8_ipc_test_attr_ctx);
  const uint32_t expected_clr = (uint32_t)1U | (uint32_t)k_ra8_ipc_clr_mask_rst;
  TEST_ASSERT_EQ(expected_clr, reg->CLR);
  TEST_END("ipc attach + dispatch message");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_dispatch_no_callback_is_safe(void)
{
  TEST_BEGIN("ipc dispatch without callback is safe");
  prep();
  ra8_ipc_dispatch((uint8_t)k_ra8_ipc_test_ch_bad);
  TEST_ASSERT_EQ(0, s_ipc_cb_count);
  const ra8_ipc_config_t cfg = make_cfg((uint8_t)k_ra8_ipc_test_ch_mid);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_init(&cfg));
  volatile r_ipc_channel_regs_t* reg = ra8_ipc_channel((uint8_t)k_ra8_ipc_test_ch_mid);
  TEST_ASSERT_NOT_NULL((void*)reg);
  reg->STA = 0x01U;
  ra8_ipc_dispatch((uint8_t)k_ra8_ipc_test_ch_mid);
  TEST_ASSERT_EQ(0, s_ipc_cb_count);
  TEST_END("ipc dispatch without callback is safe");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_per_event_handler_decodes_each_line(void)
{
  TEST_BEGIN("ipc per-event handler fires per line");
  prep();
  ra8_ipc_config_t cfg = make_cfg((uint8_t)k_ra8_ipc_test_ch_first);
  cfg.event_mask       = (uint32_t)k_ra8_ipc_event_irq_all;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_init(&cfg));

  /* Wire each of the eight IRQ lines to the same stub. */
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_ipc_irq_event_count; ++i) {
    TEST_ASSERT_EQ(k_ra8_ok,
                   ra8_ipc_attach_event_handler((uint8_t)k_ra8_ipc_test_ch_first,
                                                (ra8_ipc_irq_event_id_t)i,
                                                stub_ipc_irq_cb,
                                                (void*)(uintptr_t)k_ra8_ipc_test_irq_ctx));
  }

  volatile r_ipc_channel_regs_t* reg = ra8_ipc_channel((uint8_t)k_ra8_ipc_test_ch_first);
  TEST_ASSERT_NOT_NULL((void*)reg);
  reg->STA = (uint32_t)k_ra8_ipc_sta_mask_irq_all;
  ra8_ipc_dispatch((uint8_t)k_ra8_ipc_test_ch_first);
  /* All eight per-line callbacks fired. */
  TEST_ASSERT_EQ(k_ra8_ipc_irq_event_count, s_ipc_irq_cb_count);
  TEST_ASSERT((uintptr_t)s_ipc_irq_cb_last_ctx == (uintptr_t)k_ra8_ipc_test_irq_ctx);
  TEST_ASSERT_EQ((k_ra8_ipc_irq_event_count - 1U), s_ipc_irq_cb_last_event);

  /* Bad-arg paths */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ipc_attach_event_handler((uint8_t)k_ra8_ipc_test_ch_bad,
                                              k_ra8_ipc_irq_event_0,
                                              stub_ipc_irq_cb,
                                              nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ipc_attach_event_handler((uint8_t)k_ra8_ipc_test_ch_first,
                                              (ra8_ipc_irq_event_id_t)k_ra8_ipc_irq_event_count,
                                              stub_ipc_irq_cb,
                                              nullptr));
  TEST_END("ipc per-event handler fires per line");
}

/* ---------- NMI tests ---------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_nmi_accessor(void)
{
  TEST_BEGIN("ipc nmi accessor");
  prep();
  volatile r_ipc_nmi_regs_t* nmi0 = ra8_ipc_nmi(0U);
  volatile r_ipc_nmi_regs_t* nmi1 = ra8_ipc_nmi(1U);
  TEST_ASSERT_NOT_NULL((void*)nmi0);
  TEST_ASSERT_NOT_NULL((void*)nmi1);
  TEST_ASSERT((uintptr_t)nmi1 - (uintptr_t)nmi0 == (uintptr_t)k_ra8_ipc_nmi_stride);
  TEST_ASSERT_NULL((void*)ra8_ipc_nmi((uint8_t)k_ra8_ipc_nmi_unit_count));
  TEST_END("ipc nmi accessor");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_nmi_send_clear_status(void)
{
  TEST_BEGIN("ipc nmi send/clear/status");
  prep();
  volatile r_ipc_nmi_regs_t* nmi = ra8_ipc_nmi(0U);
  TEST_ASSERT_NOT_NULL((void*)nmi);
  nmi->NMISET = 0U;
  nmi->NMISTA = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_nmi_send(0U));
  TEST_ASSERT_EQ(k_ra8_ipc_nmi_mask_bit, nmi->NMISET);

  nmi->NMISTA  = (uint32_t)k_ra8_ipc_nmi_mask_bit;
  bool pending = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_nmi_get_status(0U, &pending));
  TEST_ASSERT(pending == true);
  nmi->NMISTA = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_nmi_get_status(0U, &pending));
  TEST_ASSERT(pending == false);

  nmi->NMICLR = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_nmi_clear(0U));
  TEST_ASSERT_EQ(k_ra8_ipc_nmi_mask_bit, nmi->NMICLR);

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ipc_nmi_send((uint8_t)k_ra8_ipc_test_unit_bad));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ipc_nmi_clear((uint8_t)k_ra8_ipc_test_unit_bad));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ipc_nmi_get_status(0U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ipc_nmi_get_status((uint8_t)k_ra8_ipc_test_unit_bad, &pending));
  TEST_END("ipc nmi send/clear/status");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_nmi_dispatch_invokes_callback(void)
{
  TEST_BEGIN("ipc nmi dispatch invokes callback");
  prep();
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_ipc_attach_nmi_handler(stub_ipc_nmi_cb, (void*)(uintptr_t)k_ra8_ipc_test_nmi_ctx));
  volatile r_ipc_nmi_regs_t* nmi = ra8_ipc_nmi(1U);
  TEST_ASSERT_NOT_NULL((void*)nmi);
  /* Pending NMI -> dispatch fires + acks. */
  nmi->NMISTA = (uint32_t)k_ra8_ipc_nmi_mask_bit;
  nmi->NMICLR = 0U;
  ra8_ipc_dispatch_nmi(1U);
  TEST_ASSERT_EQ(1, s_ipc_nmi_cb_count);
  TEST_ASSERT_EQ(1, s_ipc_nmi_cb_last_unit);
  TEST_ASSERT((uintptr_t)s_ipc_nmi_cb_last_ctx == (uintptr_t)k_ra8_ipc_test_nmi_ctx);
  TEST_ASSERT_EQ(k_ra8_ipc_nmi_mask_bit, nmi->NMICLR);

  /* No NMI pending -> dispatch is a silent no-op. */
  nmi->NMISTA = 0U;
  ra8_ipc_dispatch_nmi(1U);
  TEST_ASSERT_EQ(1, s_ipc_nmi_cb_count);

  /* Bad unit id -> no-op. */
  ra8_ipc_dispatch_nmi((uint8_t)k_ra8_ipc_test_unit_bad);
  TEST_ASSERT_EQ(1, s_ipc_nmi_cb_count);
  TEST_END("ipc nmi dispatch invokes callback");
}

/* ---------- ISR registration tests ---------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_install_uninstall_isr(void)
{
  TEST_BEGIN("ipc install/uninstall ISR");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_install_isr(0U, 8U));
  /* Re-install -> exists. */
  TEST_ASSERT_EQ(k_ra8_err_exists, ra8_ipc_install_isr(0U, 8U));
  /* Bad unit. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ipc_install_isr((uint8_t)k_ra8_ipc_test_unit_bad, 8U));

  /* Uninstall -> ok, then not_found. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_uninstall_isr(0U));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_ipc_uninstall_isr(0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ipc_uninstall_isr((uint8_t)k_ra8_ipc_test_unit_bad));
  TEST_END("ipc install/uninstall ISR");
}

/**
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` after ra8_isr_unregister in
 * ra8_ipc_uninstall_isr (1 condition)
 * - Vector 1: the ICU registration is still live -> false (uninstall returns ok)
 * - Vector 2: the ICU table was cleared underneath it -> true (not_found surfaces)
 * N+1 = 2 vectors for N=1: minimal MC/DC. Vector 1 is already driven by
 * test_install_uninstall_isr; this case adds vector 2, the only way the
 * unregister failure is propagated rather than swallowed.
 */
static void test_uninstall_isr_propagates_unregister_failure(void)
{
  TEST_BEGIN("ipc uninstall propagates an ISR unregister failure");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ipc_install_isr(0U, 8U));

  /* Clear the ICU allocator behind the IPC layer's back: ra8_ipc still records
   * the unit as installed, so ra8_isr_unregister must fail and that failure
   * must reach the caller instead of the module marking itself uninstalled. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_isr_init());
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_ipc_uninstall_isr(0U));
  TEST_END("ipc uninstall propagates an ISR unregister failure");
}

int main(void)
{
  test_attach_and_dispatch_message();
  test_dispatch_no_callback_is_safe();
  test_per_event_handler_decodes_each_line();
  test_nmi_accessor();
  test_nmi_send_clear_status();
  test_nmi_dispatch_invokes_callback();
  test_install_uninstall_isr();
  test_uninstall_isr_propagates_unregister_failure();
  return 0;
}
