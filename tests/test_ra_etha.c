/**
 * @file test_ra_etha.c
 * @brief Unit tests for ra_etha.c (per-port Ethernet Agent driver)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8d2_etha_regs.h"
#include "ra8d2_rmac_regs.h"
#include "ra_err.h"
#include "ra_etha.h"
#include "ra_mstp.h"
#include "ra_rmac.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

static uint32_t       s_etha_cb_count;
static uint32_t       s_etha_cb_eaeis0;
static uint32_t       s_etha_cb_eaeis1;
static uint32_t       s_etha_cb_eaeis2;
static ra_etha_port_t s_etha_cb_last_port;

static void stub_etha_cb(void* ctx, ra_etha_port_t port, uint32_t s0, uint32_t s1, uint32_t s2)
{
  (void)ctx;
  ++s_etha_cb_count;
  s_etha_cb_eaeis0    = s0;
  s_etha_cb_eaeis1    = s1;
  s_etha_cb_eaeis2    = s2;
  s_etha_cb_last_port = port;
}

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  s_etha_cb_count     = 0U;
  s_etha_cb_eaeis0    = 0U;
  s_etha_cb_eaeis1    = 0U;
  s_etha_cb_eaeis2    = 0U;
  s_etha_cb_last_port = k_ra_etha_port_0;
}

/* --- Lifecycle / init --- */

static void test_init_happy(void)
{
  TEST_BEGIN("etha init happy");
  prep();
  const ra_etha_config_t cfg = {.initial_mode = k_ra_etha_opc_config,
                                .eaeie0_mask  = 0xDEADBEEFU,
                                .eaeie1_mask  = 0xCAFEBABEU,
                                .eaeie2_mask  = 0x12345678U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_etha_init(k_ra_etha_port_0, &cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_etha_opc_config,
                 (int32_t)(ra_etha(k_ra_etha_port_0)->EAMC & (uint32_t)k_ra_etha_mask_opc));
  TEST_ASSERT_EQ((int32_t)0xDEADBEEFU, (int32_t)ra_etha(k_ra_etha_port_0)->EAEIE0);
  TEST_ASSERT_EQ((int32_t)0xCAFEBABEU, (int32_t)ra_etha(k_ra_etha_port_0)->EAEIE1);
  TEST_ASSERT_EQ((int32_t)0x12345678U, (int32_t)ra_etha(k_ra_etha_port_0)->EAEIE2);
  TEST_END("etha init happy");
}

static void test_init_null(void)
{
  TEST_BEGIN("etha init null cfg");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_etha_init(k_ra_etha_port_0, nullptr));
  TEST_END("etha init null cfg");
}

static void test_init_bad_port(void)
{
  TEST_BEGIN("etha init bad port");
  prep();
  const ra_etha_config_t cfg = {.initial_mode = k_ra_etha_opc_reset,
                                .eaeie0_mask  = 0U,
                                .eaeie1_mask  = 0U,
                                .eaeie2_mask  = 0U};
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_etha_init((ra_etha_port_t)(uint8_t)k_ra_etha_port_count, &cfg));
  TEST_END("etha init bad port");
}

static void test_status_read_and_clear(void)
{
  TEST_BEGIN("etha status read + clear");
  prep();
  const ra_etha_config_t cfg = {.initial_mode = k_ra_etha_opc_operation,
                                .eaeie0_mask  = 0U,
                                .eaeie1_mask  = 0U,
                                .eaeie2_mask  = 0U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_etha_init(k_ra_etha_port_1, &cfg));
  ra_etha(k_ra_etha_port_1)->EAMS   = (uint32_t)k_ra_etha_ops_operation;
  ra_etha(k_ra_etha_port_1)->EAEIS0 = 0xCAFEBABEU;
  ra_etha(k_ra_etha_port_1)->EAEIS1 = 0xAABBCCDDU;
  ra_etha(k_ra_etha_port_1)->EAEIS2 = 0x11223344U;

  ra_etha_status_t snap = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_etha_get_status(k_ra_etha_port_1, &snap));
  TEST_ASSERT_EQ((int32_t)k_ra_etha_ops_operation, (int32_t)snap.ops);
  TEST_ASSERT_EQ((int32_t)0xCAFEBABEU, (int32_t)snap.eaeis0);
  TEST_ASSERT_EQ((int32_t)0xAABBCCDDU, (int32_t)snap.eaeis1);
  TEST_ASSERT_EQ((int32_t)0x11223344U, (int32_t)snap.eaeis2);

  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_etha_clear_status(k_ra_etha_port_1, k_ra_etha_irq_class_0, 0x0F0F0F0FU));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_etha_get_status(k_ra_etha_port_1, nullptr));
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_arg,
    (int32_t)ra_etha_clear_status(k_ra_etha_port_1,
                                  (ra_etha_irq_class_t)(uint8_t)k_ra_etha_irq_class_count,
                                  0U));
  TEST_END("etha status read + clear");
}

static void test_clear_status_blocks(void)
{
  TEST_BEGIN("etha clear_status all blocks");
  prep();
  const ra_etha_config_t cfg = {.initial_mode = k_ra_etha_opc_operation,
                                .eaeie0_mask  = 0U,
                                .eaeie1_mask  = 0U,
                                .eaeie2_mask  = 0U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_etha_init(k_ra_etha_port_0, &cfg));
  ra_etha(k_ra_etha_port_0)->EAEIS0 = 0xFFFFFFFFU;
  ra_etha(k_ra_etha_port_0)->EAEIS1 = 0xFFFFFFFFU;
  ra_etha(k_ra_etha_port_0)->EAEIS2 = 0xFFFFFFFFU;
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_etha_clear_status(k_ra_etha_port_0, k_ra_etha_irq_class_0, 0x000000FFU));
  TEST_ASSERT_EQ((int32_t)0xFFFFFF00U, (int32_t)ra_etha(k_ra_etha_port_0)->EAEIS0);
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_etha_clear_status(k_ra_etha_port_0, k_ra_etha_irq_class_1, 0x0000FF00U));
  TEST_ASSERT_EQ((int32_t)0xFFFF00FFU, (int32_t)ra_etha(k_ra_etha_port_0)->EAEIS1);
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_etha_clear_status(k_ra_etha_port_0, k_ra_etha_irq_class_2, 0xFFFF0000U));
  TEST_ASSERT_EQ((int32_t)0x0000FFFFU, (int32_t)ra_etha(k_ra_etha_port_0)->EAEIS2);
  TEST_END("etha clear_status all blocks");
}

static void test_enable_disable_irq(void)
{
  TEST_BEGIN("etha enable/disable irq");
  prep();
  const ra_etha_config_t cfg = {.initial_mode = k_ra_etha_opc_operation,
                                .eaeie0_mask  = 0xF0F0F0F0U,
                                .eaeie1_mask  = 0U,
                                .eaeie2_mask  = 0U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_etha_init(k_ra_etha_port_0, &cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_etha_enable_irq(k_ra_etha_port_0, k_ra_etha_irq_class_0, 0x0F0F0F0FU));
  TEST_ASSERT_EQ((int32_t)0xFFFFFFFFU, (int32_t)ra_etha(k_ra_etha_port_0)->EAEIE0);
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_etha_disable_irq(k_ra_etha_port_0, k_ra_etha_irq_class_0, 0x0F0F0F0FU));
  TEST_ASSERT_EQ((int32_t)0xF0F0F0F0U, (int32_t)ra_etha(k_ra_etha_port_0)->EAEIE0);

  /* Per-block coverage. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_etha_enable_irq(k_ra_etha_port_0, k_ra_etha_irq_class_1, 0x000000FFU));
  TEST_ASSERT_EQ((int32_t)0x000000FFU, (int32_t)ra_etha(k_ra_etha_port_0)->EAEIE1);
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_etha_enable_irq(k_ra_etha_port_0, k_ra_etha_irq_class_2, 0x0000FF00U));
  TEST_ASSERT_EQ((int32_t)0x0000FF00U, (int32_t)ra_etha(k_ra_etha_port_0)->EAEIE2);
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_etha_disable_irq(k_ra_etha_port_0, k_ra_etha_irq_class_1, 0x000000FFU));
  TEST_ASSERT_EQ((int32_t)0U, (int32_t)ra_etha(k_ra_etha_port_0)->EAEIE1);

  /* Bad-arg: out-of-range port + block. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_etha_enable_irq((ra_etha_port_t)(uint8_t)k_ra_etha_port_count,
                                             k_ra_etha_irq_class_0,
                                             0U));
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_arg,
    (int32_t)ra_etha_disable_irq(k_ra_etha_port_0,
                                 (ra_etha_irq_class_t)(uint8_t)k_ra_etha_irq_class_count,
                                 0U));
  TEST_END("etha enable/disable irq");
}

static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("etha attach + dispatch");
  prep();
  const ra_etha_config_t cfg = {.initial_mode = k_ra_etha_opc_operation,
                                .eaeie0_mask  = 0U,
                                .eaeie1_mask  = 0U,
                                .eaeie2_mask  = 0U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_etha_init(k_ra_etha_port_0, &cfg));
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_etha_attach_handler(k_ra_etha_port_0, stub_etha_cb, (void*)(uintptr_t)0xE0U));

  ra_etha(k_ra_etha_port_0)->EAEIS0 = 0xABCDU;
  ra_etha(k_ra_etha_port_0)->EAEIS1 = 0x1234U;
  ra_etha(k_ra_etha_port_0)->EAEIS2 = 0x5678U;
  ra_etha_dispatch(k_ra_etha_port_0);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_etha_cb_count);
  TEST_ASSERT_EQ((int32_t)0xABCDU, (int32_t)s_etha_cb_eaeis0);
  TEST_ASSERT_EQ((int32_t)0x1234U, (int32_t)s_etha_cb_eaeis1);
  TEST_ASSERT_EQ((int32_t)0x5678U, (int32_t)s_etha_cb_eaeis2);
  TEST_ASSERT_EQ((int32_t)k_ra_etha_port_0, (int32_t)s_etha_cb_last_port);

  /* Detach -- next dispatch must NOT increment the counter. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_etha_attach_handler(k_ra_etha_port_0, nullptr, nullptr));
  ra_etha(k_ra_etha_port_0)->EAEIS0 = 0xDEADU;
  ra_etha_dispatch(k_ra_etha_port_0);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_etha_cb_count);

  /* Out-of-range dispatch is a no-op (no callback fired). */
  ra_etha_dispatch((ra_etha_port_t)(uint8_t)k_ra_etha_port_count);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_etha_cb_count);
  TEST_END("etha attach + dispatch");
}

static void test_power_transition(void)
{
  TEST_BEGIN("etha power transition");
  prep();
  const ra_etha_config_t cfg = {.initial_mode = k_ra_etha_opc_operation,
                                .eaeie0_mask  = 0U,
                                .eaeie1_mask  = 0U,
                                .eaeie2_mask  = 0U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_etha_init(k_ra_etha_port_0, &cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_etha_enter_stop(k_ra_etha_port_0));
  TEST_ASSERT_EQ((int32_t)k_ra_etha_opc_disable,
                 (int32_t)(ra_etha(k_ra_etha_port_0)->EAMC & (uint32_t)k_ra_etha_mask_opc));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_etha_exit_stop(k_ra_etha_port_0));
  TEST_ASSERT_EQ((int32_t)k_ra_etha_opc_operation,
                 (int32_t)(ra_etha(k_ra_etha_port_0)->EAMC & (uint32_t)k_ra_etha_mask_opc));
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_etha_set_mode(k_ra_etha_port_0, k_ra_etha_opc_config));
  TEST_ASSERT_EQ((int32_t)k_ra_etha_opc_config,
                 (int32_t)(ra_etha(k_ra_etha_port_0)->EAMC & (uint32_t)k_ra_etha_mask_opc));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_etha_reset(k_ra_etha_port_0));
  /* After reset() the final EAMC write should be CONFIG. */
  TEST_ASSERT_EQ((int32_t)k_ra_etha_opc_config,
                 (int32_t)(ra_etha(k_ra_etha_port_0)->EAMC & (uint32_t)k_ra_etha_mask_opc));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_etha_deinit(k_ra_etha_port_0));
  TEST_ASSERT_EQ((int32_t)k_ra_etha_opc_reset,
                 (int32_t)(ra_etha(k_ra_etha_port_0)->EAMC & (uint32_t)k_ra_etha_mask_opc));
  TEST_END("etha power transition");
}

static void test_lifecycle_bad_args(void)
{
  TEST_BEGIN("etha lifecycle bad args");
  prep();
  const ra_etha_port_t bad = (ra_etha_port_t)(uint8_t)k_ra_etha_port_count;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_etha_deinit(bad));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_etha_enter_stop(bad));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_etha_exit_stop(bad));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_etha_reset(bad));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_etha_set_mode(bad, k_ra_etha_opc_reset));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_etha_set_mode(k_ra_etha_port_0, (ra_etha_opc_t)4U));
  TEST_END("etha lifecycle bad args");
}

/* --- Per-class queue + preemption --- */

static void test_queue_arb_and_depth(void)
{
  TEST_BEGIN("etha queue arb + depth");
  prep();
  const ra_etha_config_t cfg = {.initial_mode = k_ra_etha_opc_config,
                                .eaeie0_mask  = 0U,
                                .eaeie1_mask  = 0U,
                                .eaeie2_mask  = 0U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_etha_init(k_ra_etha_port_0, &cfg));
  /* Full sweep over 8 traffic classes. */
  for (uint8_t i = 0U; i < (uint8_t)k_ra_etha_tc_count; ++i) {
    TEST_ASSERT_EQ((int32_t)k_ra_ok,
                   (int32_t)ra_etha_set_queue_arb(k_ra_etha_port_0, (ra_etha_tc_t)i, (uint8_t)i));
    TEST_ASSERT_EQ((int32_t)k_ra_ok,
                   (int32_t)ra_etha_set_queue_depth(k_ra_etha_port_0,
                                                    (ra_etha_tc_t)i,
                                                    (uint16_t)((i + 1U) * 8U)));
  }
  /* Verify EATDQAC packs the classes in the lower nibbles. */
  TEST_ASSERT_EQ((int32_t)0x76543210U, (int32_t)ra_etha(k_ra_etha_port_0)->EATDQAC);
  /* Verify per-class EATDQDC entries. */
  TEST_ASSERT_EQ((int32_t)8U, (int32_t)ra_etha(k_ra_etha_port_0)->EATDQDC[0]);
  TEST_ASSERT_EQ((int32_t)64U, (int32_t)ra_etha(k_ra_etha_port_0)->EATDQDC[7]);

  /* Bad args. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_etha_set_queue_arb(k_ra_etha_port_0, k_ra_etha_tc_0, 16U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_etha_set_queue_arb(k_ra_etha_port_0,
                                                (ra_etha_tc_t)(uint8_t)k_ra_etha_tc_count,
                                                0U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_etha_set_queue_depth(k_ra_etha_port_0, k_ra_etha_tc_0, 0x800U));
  TEST_END("etha queue arb + depth");
}

static void test_get_queue_level(void)
{
  TEST_BEGIN("etha get queue level");
  prep();
  const ra_etha_config_t cfg = {.initial_mode = k_ra_etha_opc_operation,
                                .eaeie0_mask  = 0U,
                                .eaeie1_mask  = 0U,
                                .eaeie2_mask  = 0U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_etha_init(k_ra_etha_port_0, &cfg));
  ra_etha(k_ra_etha_port_0)->EATDQM[3]   = 0x0123U;
  ra_etha(k_ra_etha_port_0)->EATDQMLM[3] = 0x0456U;
  uint16_t cur                           = 0U;
  uint16_t pk                            = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_etha_get_queue_level(k_ra_etha_port_0, k_ra_etha_tc_3, &cur, &pk));
  TEST_ASSERT_EQ((int32_t)0x0123U, (int32_t)cur);
  TEST_ASSERT_EQ((int32_t)0x0456U, (int32_t)pk);
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_etha_get_queue_level(k_ra_etha_port_0, k_ra_etha_tc_3, nullptr, &pk));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_etha_get_queue_level(k_ra_etha_port_0, k_ra_etha_tc_3, &cur, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_etha_get_queue_level(k_ra_etha_port_0,
                                                  (ra_etha_tc_t)(uint8_t)k_ra_etha_tc_count,
                                                  &cur,
                                                  &pk));
  TEST_END("etha get queue level");
}

static void test_set_preemption(void)
{
  TEST_BEGIN("etha set preemption");
  prep();
  const ra_etha_config_t cfg = {.initial_mode = k_ra_etha_opc_config,
                                .eaeie0_mask  = 0U,
                                .eaeie1_mask  = 0U,
                                .eaeie2_mask  = 0U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_etha_init(k_ra_etha_port_0, &cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_etha_set_preemption(k_ra_etha_port_0, 0xAAU, 1U, k_ra_etha_afs_192));
  /* Expected: TTQ7..0 = 0xAA, TTQ8 = 1, AFS = 2 << 16 */
  TEST_ASSERT_EQ((int32_t)((0xAAU) | (1U << 8) | (2U << 16)),
                 (int32_t)ra_etha(k_ra_etha_port_0)->EATPEC);
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_etha_set_preemption((ra_etha_port_t)(uint8_t)k_ra_etha_port_count,
                                                 0U,
                                                 0U,
                                                 k_ra_etha_afs_64));
  TEST_END("etha set preemption");
}

static void test_set_max_frame_size(void)
{
  TEST_BEGIN("etha set max frame size (jumbo)");
  prep();
  const ra_etha_config_t cfg = {.initial_mode = k_ra_etha_opc_config,
                                .eaeie0_mask  = 0U,
                                .eaeie1_mask  = 0U,
                                .eaeie2_mask  = 0U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_etha_init(k_ra_etha_port_0, &cfg));
  /* Standard 1500 + jumbo 9000 byte payloads. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_etha_set_max_frame_size(k_ra_etha_port_0, k_ra_etha_tc_0, 1518U));
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_etha_set_max_frame_size(k_ra_etha_port_0, k_ra_etha_tc_7, 9018U));
  TEST_ASSERT_EQ((int32_t)1518U, (int32_t)ra_etha(k_ra_etha_port_0)->EATMFSC[0]);
  TEST_ASSERT_EQ((int32_t)9018U, (int32_t)ra_etha(k_ra_etha_port_0)->EATMFSC[7]);
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_etha_set_max_frame_size(k_ra_etha_port_0,
                                                     (ra_etha_tc_t)(uint8_t)k_ra_etha_tc_count,
                                                     1500U));
  TEST_END("etha set max frame size (jumbo)");
}

/* --- IPV remap + VLAN --- */

static void test_set_ipv_remap(void)
{
  TEST_BEGIN("etha set ipv remap");
  prep();
  const ra_etha_config_t cfg = {.initial_mode = k_ra_etha_opc_config,
                                .eaeie0_mask  = 0U,
                                .eaeie1_mask  = 0U,
                                .eaeie2_mask  = 0U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_etha_init(k_ra_etha_port_0, &cfg));
  const uint8_t ok_map[8] = {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_etha_set_ipv_remap(k_ra_etha_port_0, ok_map));
  /* Expected packed: 0x76543210 (IPVR7..0 each 3 bits in stride-4 nibbles). */
  TEST_ASSERT_EQ((int32_t)0x76543210U, (int32_t)ra_etha(k_ra_etha_port_0)->EAIRC);

  /* Bad arg: entry > 7 -> reject without writing. */
  ra_etha(k_ra_etha_port_0)->EAIRC = 0xDEADBEEFU;
  const uint8_t bad_map[8]         = {0U, 1U, 8U, 3U, 4U, 5U, 6U, 7U};
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_etha_set_ipv_remap(k_ra_etha_port_0, bad_map));
  TEST_ASSERT_EQ((int32_t)0xDEADBEEFU, (int32_t)ra_etha(k_ra_etha_port_0)->EAIRC);

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_etha_set_ipv_remap(k_ra_etha_port_0, nullptr));
  TEST_END("etha set ipv remap");
}

static void test_set_vlan_mode_and_tag(void)
{
  TEST_BEGIN("etha vlan mode + tag");
  prep();
  const ra_etha_config_t cfg = {.initial_mode = k_ra_etha_opc_config,
                                .eaeie0_mask  = 0U,
                                .eaeie1_mask  = 0U,
                                .eaeie2_mask  = 0U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_etha_init(k_ra_etha_port_0, &cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_etha_set_vlan_mode(k_ra_etha_port_0,
                                                k_ra_etha_vim_enabled,
                                                k_ra_etha_vem_strip_outer));
  /* VIM=1, VEM=1<<16. */
  TEST_ASSERT_EQ((int32_t)((1U) | (1U << 16)), (int32_t)ra_etha(k_ra_etha_port_0)->EAVCC);

  const ra_etha_vlan_tag_t c = {.vid = 100U, .pcp = 5U, .dei = 1U};
  const ra_etha_vlan_tag_t s = {.vid = 200U, .pcp = 3U, .dei = 0U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_etha_set_vlan_tag(k_ra_etha_port_0, &c, &s));
  /* CTV=100 in [11:0]; CTP=5 in [14:12]; CTD=1 in [15];
   * STV=200<<16; STP=3<<28; STD=0. */
  const uint32_t expect = (100U) | (5U << 12) | (1U << 15) | (200U << 16) | (3U << 28);
  TEST_ASSERT_EQ((int32_t)expect, (int32_t)ra_etha(k_ra_etha_port_0)->EAVTC);

  /* Bad-arg: vid > 4095. */
  const ra_etha_vlan_tag_t bad = {.vid = 0x1000U, .pcp = 0U, .dei = 0U};
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_etha_set_vlan_tag(k_ra_etha_port_0, &bad, &s));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_etha_set_vlan_tag(k_ra_etha_port_0, nullptr, &s));
  TEST_END("etha vlan mode + tag");
}

static void test_rx_tag_filter(void)
{
  TEST_BEGIN("etha rx tag filter");
  prep();
  const ra_etha_config_t cfg = {.initial_mode = k_ra_etha_opc_config,
                                .eaeie0_mask  = 0U,
                                .eaeie1_mask  = 0U,
                                .eaeie2_mask  = 0U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_etha_init(k_ra_etha_port_0, &cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_etha_set_rx_tag_filter(k_ra_etha_port_0, 0x000001AAU));
  TEST_ASSERT_EQ((int32_t)0x000001AAU, (int32_t)ra_etha(k_ra_etha_port_0)->EARTFC);
  /* Mask is constrained to 9 bits. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_etha_set_rx_tag_filter(k_ra_etha_port_0, 0xFFFFFFFFU));
  TEST_ASSERT_EQ((int32_t)0x000001FFU, (int32_t)ra_etha(k_ra_etha_port_0)->EARTFC);
  TEST_END("etha rx tag filter");
}

/* --- Cut-through queue --- */

static void test_cut_through_queue(void)
{
  TEST_BEGIN("etha cut through queue");
  prep();
  const ra_etha_config_t cfg = {.initial_mode = k_ra_etha_opc_config,
                                .eaeie0_mask  = 0U,
                                .eaeie1_mask  = 0U,
                                .eaeie2_mask  = 0U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_etha_init(k_ra_etha_port_0, &cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_etha_configure_cut_through(k_ra_etha_port_0, 0x1234U, 0x5U));
  TEST_ASSERT_EQ((int32_t)0x1234U, (int32_t)ra_etha(k_ra_etha_port_0)->EACTQC);
  TEST_ASSERT_EQ((int32_t)0x5U, (int32_t)ra_etha(k_ra_etha_port_0)->EACTDQDC);
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_etha_configure_cut_through(k_ra_etha_port_0, 0U, 0x10U));
  TEST_END("etha cut through queue");
}

/* --- CBS --- */

static void test_cbs_configure_and_state(void)
{
  TEST_BEGIN("etha cbs configure + state");
  prep();
  const ra_etha_config_t cfg = {.initial_mode = k_ra_etha_opc_config,
                                .eaeie0_mask  = 0U,
                                .eaeie1_mask  = 0U,
                                .eaeie2_mask  = 0U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_etha_init(k_ra_etha_port_0, &cfg));
  const ra_etha_cbs_param_t p = {.increment = 0x12345U, .upper_lim = 0x4000U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_etha_configure_cbs(k_ra_etha_port_0, k_ra_etha_tc_2, 1U, &p));
  TEST_ASSERT_EQ((int32_t)0x12345U, (int32_t)ra_etha(k_ra_etha_port_0)->EACAIVC[2]);
  TEST_ASSERT_EQ((int32_t)0x4000U, (int32_t)ra_etha(k_ra_etha_port_0)->EACAULC[2]);
  TEST_ASSERT_EQ((int32_t)(1U << 2), (int32_t)ra_etha(k_ra_etha_port_0)->EACAEC);
  TEST_ASSERT_EQ((int32_t)(1U << 2), (int32_t)ra_etha(k_ra_etha_port_0)->EACC);

  /* Disable path: clears the per-class bit but does not zero the param regs. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_etha_configure_cbs(k_ra_etha_port_0, k_ra_etha_tc_2, 0U, nullptr));
  TEST_ASSERT_EQ((int32_t)0U, (int32_t)ra_etha(k_ra_etha_port_0)->EACAEC);

  /* Read oper-side mirrors. */
  ra_etha(k_ra_etha_port_0)->EACOEM     = (1U << 2);
  ra_etha(k_ra_etha_port_0)->EACGSM     = (1U << 2);
  ra_etha(k_ra_etha_port_0)->EACOIVM[2] = 0x9999U;
  ra_etha(k_ra_etha_port_0)->EACOULM[2] = 0x77777777U;
  uint8_t             en                = 0U;
  uint8_t             gs                = 0U;
  ra_etha_cbs_param_t op                = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_etha_get_cbs_state(k_ra_etha_port_0, k_ra_etha_tc_2, &en, &gs, &op));
  TEST_ASSERT_EQ((int32_t)1, (int32_t)en);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)gs);
  TEST_ASSERT_EQ((int32_t)0x9999U, (int32_t)op.increment);
  TEST_ASSERT_EQ((int32_t)0x77777777U, (int32_t)op.upper_lim);

  /* Bad-arg + null. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_etha_configure_cbs(k_ra_etha_port_0, k_ra_etha_tc_0, 1U, nullptr));
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_null_ptr,
    (int32_t)ra_etha_get_cbs_state(k_ra_etha_port_0, k_ra_etha_tc_0, nullptr, &gs, &op));
  TEST_END("etha cbs configure + state");
}

/* --- TAS --- */

static void test_tas_schedule_and_enable(void)
{
  TEST_BEGIN("etha tas schedule + enable");
  prep();
  const ra_etha_config_t cfg = {.initial_mode = k_ra_etha_opc_config,
                                .eaeie0_mask  = 0U,
                                .eaeie1_mask  = 0U,
                                .eaeie2_mask  = 0U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_etha_init(k_ra_etha_port_0, &cfg));
  const ra_etha_tas_gate_t list[2] = {
    {.gate_state = 0xFFU, .time_units = 1000U, .cut_through = 0U},
    {.gate_state = 0x0FU, .time_units = 2000U, .cut_through = 1U},
  };
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_etha_set_tas_schedule(k_ra_etha_port_0, list, 2U, 100000U, 0xCAFEBABEDEADBEEFULL));
  TEST_ASSERT_EQ((int32_t)0xDEADBEEFU, (int32_t)ra_etha(k_ra_etha_port_0)->EATASCSTC0);
  TEST_ASSERT_EQ((int32_t)0xCAFEBABEU, (int32_t)ra_etha(k_ra_etha_port_0)->EATASCSTC1);
  TEST_ASSERT_EQ((int32_t)100000U, (int32_t)ra_etha(k_ra_etha_port_0)->EATASCTC);
  TEST_ASSERT_EQ((int32_t)0xFFU, (int32_t)ra_etha(k_ra_etha_port_0)->EATASIGSC);
  /* Last EATASGL writes are observable. */
  TEST_ASSERT_EQ((int32_t)0x0FU, (int32_t)ra_etha(k_ra_etha_port_0)->EATASGL0);
  TEST_ASSERT_EQ((int32_t)((2000U) | (1U << 28)), (int32_t)ra_etha(k_ra_etha_port_0)->EATASGL1);

  /* TASCC commit bit set. */
  TEST_ASSERT(((ra_etha(k_ra_etha_port_0)->EATASC >> 1) & 1U) == 1U);

  /* Enable / disable. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_etha_enable_tas(k_ra_etha_port_0, 1U));
  TEST_ASSERT(((ra_etha(k_ra_etha_port_0)->EATASC >> 0) & 1U) == 1U);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_etha_enable_tas(k_ra_etha_port_0, 0U));
  TEST_ASSERT(((ra_etha(k_ra_etha_port_0)->EATASC >> 0) & 1U) == 0U);

  /* Bad args. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_etha_set_tas_schedule(k_ra_etha_port_0, list, 257U, 0U, 0ULL));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_etha_set_tas_schedule(k_ra_etha_port_0, nullptr, 1U, 0U, 0ULL));
  /* Empty list is allowed (just programs cycle / start time). */
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_etha_set_tas_schedule(k_ra_etha_port_0, nullptr, 0U, 999U, 0ULL));
  TEST_ASSERT_EQ((int32_t)999U, (int32_t)ra_etha(k_ra_etha_port_0)->EATASCTC);
  TEST_END("etha tas schedule + enable");
}

/* --- Stats --- */

static void test_read_clear_stats(void)
{
  TEST_BEGIN("etha read + clear stats");
  prep();
  const ra_etha_config_t cfg = {.initial_mode = k_ra_etha_opc_operation,
                                .eaeie0_mask  = 0U,
                                .eaeie1_mask  = 0U,
                                .eaeie2_mask  = 0U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_etha_init(k_ra_etha_port_0, &cfg));
  ra_etha(k_ra_etha_port_0)->EAUSMFSECN = 0x0010U;
  ra_etha(k_ra_etha_port_0)->EATFECN    = 0x0020U;
  ra_etha(k_ra_etha_port_0)->EAFSECN    = 0x0030U;
  ra_etha(k_ra_etha_port_0)->EADQOECN   = 0x0040U;
  ra_etha(k_ra_etha_port_0)->EADQSECN   = 0x0050U;

  ra_etha_stats_t st = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_etha_read_stats(k_ra_etha_port_0, &st));
  TEST_ASSERT_EQ((int32_t)0x10U, (int32_t)st.switch_min_frame_err);
  TEST_ASSERT_EQ((int32_t)0x20U, (int32_t)st.tag_filter_err);
  TEST_ASSERT_EQ((int32_t)0x30U, (int32_t)st.frame_size_err);
  TEST_ASSERT_EQ((int32_t)0x40U, (int32_t)st.queue_overflow_err);
  TEST_ASSERT_EQ((int32_t)0x50U, (int32_t)st.queue_security_err);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_etha_clear_stats(k_ra_etha_port_0));
  TEST_ASSERT_EQ((int32_t)0U, (int32_t)ra_etha(k_ra_etha_port_0)->EAUSMFSECN);
  TEST_ASSERT_EQ((int32_t)0U, (int32_t)ra_etha(k_ra_etha_port_0)->EATFECN);
  TEST_ASSERT_EQ((int32_t)0U, (int32_t)ra_etha(k_ra_etha_port_0)->EAFSECN);
  TEST_ASSERT_EQ((int32_t)0U, (int32_t)ra_etha(k_ra_etha_port_0)->EADQOECN);
  TEST_ASSERT_EQ((int32_t)0U, (int32_t)ra_etha(k_ra_etha_port_0)->EADQSECN);

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_etha_read_stats(k_ra_etha_port_0, nullptr));
  TEST_END("etha read + clear stats");
}

/* --- Per-port descriptor ring + traffic stats --- */

static void test_descriptor_ring_init(void)
{
  TEST_BEGIN("etha descriptor_ring_init");
  prep();
  const ra_etha_config_t cfg = {.initial_mode = k_ra_etha_opc_config,
                                .eaeie0_mask  = 0U,
                                .eaeie1_mask  = 0U,
                                .eaeie2_mask  = 0U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_etha_init(k_ra_etha_port_0, &cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_etha_descriptor_ring_init(k_ra_etha_port_0, 32U, 64U, 1518U));
  for (uint8_t i = 0U; i < (uint8_t)k_ra_etha_tc_count; ++i) {
    TEST_ASSERT_EQ((int32_t)32U, (int32_t)ra_etha(k_ra_etha_port_0)->EATDQDC[i]);
  }
  ra_etha_port_stats_t st = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_etha_get_stats(k_ra_etha_port_0, &st));
  TEST_ASSERT_EQ((int32_t)32U, (int32_t)st.ring_tx);
  TEST_ASSERT_EQ((int32_t)64U, (int32_t)st.ring_rx);
  TEST_ASSERT_EQ((int32_t)1518U, (int32_t)st.ring_buf);
  TEST_ASSERT_EQ((int32_t)0U, (int32_t)st.tx_ok);

  /* Bad-arg + clamping cases. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_etha_descriptor_ring_init(k_ra_etha_port_0, 0U, 64U, 1518U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_etha_descriptor_ring_init(k_ra_etha_port_0, 32U, 8000U, 1518U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_etha_descriptor_ring_init(k_ra_etha_port_0, 32U, 64U, 16U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_etha_descriptor_ring_init(k_ra_etha_port_0, 32U, 64U, 17000U));
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_arg,
    (int32_t)
      ra_etha_descriptor_ring_init((ra_etha_port_t)(uint8_t)k_ra_etha_port_count, 32U, 64U, 1518U));
  /* Deep ring (>2047) clamped at the EATDQDC 11-bit ceiling. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_etha_descriptor_ring_init(k_ra_etha_port_0, 4096U, 4096U, 1518U));
  TEST_ASSERT_EQ((int32_t)2047U, (int32_t)ra_etha(k_ra_etha_port_0)->EATDQDC[0]);
  TEST_END("etha descriptor_ring_init");
}

static void test_get_stats_and_account(void)
{
  TEST_BEGIN("etha get_stats + account_traffic");
  prep();
  const ra_etha_config_t cfg = {.initial_mode = k_ra_etha_opc_operation,
                                .eaeie0_mask  = 0U,
                                .eaeie1_mask  = 0U,
                                .eaeie2_mask  = 0U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_etha_init(k_ra_etha_port_0, &cfg));
  ra_etha_port_stats_t st = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_etha_get_stats(k_ra_etha_port_0, &st));
  TEST_ASSERT_EQ((int32_t)0U, (int32_t)st.tx_ok);

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_etha_account_traffic(k_ra_etha_port_0, 100U, 2U, 75U, 3U, 1U));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_etha_get_stats(k_ra_etha_port_0, &st));
  TEST_ASSERT_EQ((int32_t)100U, (int32_t)st.tx_ok);
  TEST_ASSERT_EQ((int32_t)2U, (int32_t)st.tx_err);
  TEST_ASSERT_EQ((int32_t)75U, (int32_t)st.rx_ok);
  TEST_ASSERT_EQ((int32_t)3U, (int32_t)st.rx_err);
  TEST_ASSERT_EQ((int32_t)1U, (int32_t)st.rx_drop);

  /* Saturation: push tx_ok near UINT32_MAX and add 1000 more -> sat. */
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_etha_account_traffic(k_ra_etha_port_0, UINT32_MAX - 50U, 0U, 0U, 0U, 0U));
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_etha_account_traffic(k_ra_etha_port_0, 1000U, 0U, 0U, 0U, 0U));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_etha_get_stats(k_ra_etha_port_0, &st));
  TEST_ASSERT(st.tx_ok == UINT32_MAX);

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_etha_get_stats(k_ra_etha_port_0, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_etha_get_stats((ra_etha_port_t)(uint8_t)k_ra_etha_port_count, &st));
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_arg,
    (int32_t)
      ra_etha_account_traffic((ra_etha_port_t)(uint8_t)k_ra_etha_port_count, 0U, 0U, 0U, 0U, 0U));
  TEST_END("etha get_stats + account_traffic");
}

static void test_get_stats_after_deinit(void)
{
  TEST_BEGIN("etha get_stats after deinit (clean slate)");
  prep();
  const ra_etha_config_t cfg = {.initial_mode = k_ra_etha_opc_operation,
                                .eaeie0_mask  = 0U,
                                .eaeie1_mask  = 0U,
                                .eaeie2_mask  = 0U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_etha_init(k_ra_etha_port_0, &cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_etha_account_traffic(k_ra_etha_port_0, 5U, 5U, 5U, 5U, 5U));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_etha_deinit(k_ra_etha_port_0));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_etha_init(k_ra_etha_port_0, &cfg));
  ra_etha_port_stats_t st = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_etha_get_stats(k_ra_etha_port_0, &st));
  TEST_ASSERT_EQ((int32_t)0U, (int32_t)st.tx_ok);
  TEST_ASSERT_EQ((int32_t)0U, (int32_t)st.ring_tx);
  TEST_END("etha get_stats after deinit (clean slate)");
}

static void test_etha_open_bad_args(void)
{
  TEST_BEGIN("etha open bad args");
  prep();
  const ra_etha_config_t cfg = {.initial_mode = k_ra_etha_opc_config,
                                .eaeie0_mask  = 0U,
                                .eaeie1_mask  = 0U,
                                .eaeie2_mask  = 0U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_etha_init(k_ra_etha_port_0, &cfg));
  ra_rmac_phy_link_t       lk  = {};
  const ra_etha_phy_open_t phy = {.phy_addr   = 5U,
                                  .advertise  = (uint16_t)k_ra_rmac_phy_advert_100_fd,
                                  .timeout_ms = 1U};
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_etha_open(k_ra_etha_port_0, nullptr, &lk));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_etha_open(k_ra_etha_port_0, &phy, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_etha_open((ra_etha_port_t)(uint8_t)k_ra_etha_port_count, &phy, &lk));
  TEST_END("etha open bad args");
}

static void test_etha_open_eamc_transition(void)
{
  TEST_BEGIN("etha open EAMC transition");
  prep();
  const ra_etha_config_t ecfg = {.initial_mode = k_ra_etha_opc_config,
                                 .eaeie0_mask  = 0U,
                                 .eaeie1_mask  = 0U,
                                 .eaeie2_mask  = 0U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_etha_init(k_ra_etha_port_0, &ecfg));
  const ra_rmac_config_t rcfg = {.rx_filter       = k_ra_rmac_mrafc_unicast_match,
                                 .err_irq_enable  = 0U,
                                 .mon0_irq_enable = 0U,
                                 .mon1_irq_enable = 0U,
                                 .mon2_irq_enable = 0U,
                                 .phy_interface   = k_ra_rmac_pis_rmii,
                                 .link_speed      = k_ra_rmac_lsc_100mbit,
                                 .duplex          = k_ra_rmac_duplex_full};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_rmac_init(k_ra_rmac_port_0, &rcfg));
  /* Pre-arm MMIS1 for both PWACS and PRACS so MDIO writes complete; the
   * driver's reads return 0 (PRD bits zeroed by issue), so BMCR.RESET
   * appears already cleared and phy_reset succeeds. Auto-neg wait
   * never sees AN_COMPLETE and times out, but EAMC must already be
   * OPERATION at that point so callers can retry without re-entering
   * CONFIG. */
  ra_rmac(k_ra_rmac_port_0)->MMIS1 =
    (uint32_t)k_ra_rmac_mmis1_pwacs | (uint32_t)k_ra_rmac_mmis1_pracs;
  ra_rmac_phy_link_t       lk  = {};
  const ra_etha_phy_open_t phy = {.phy_addr   = 1U,
                                  .advertise  = (uint16_t)k_ra_rmac_phy_advert_100_fd,
                                  .timeout_ms = 1U};
  const ra_err_t           r   = ra_etha_open(k_ra_etha_port_0, &phy, &lk);
  TEST_ASSERT(r == k_ra_ok || r == k_ra_err_hw_timeout);
  TEST_ASSERT_EQ((int32_t)k_ra_etha_opc_operation,
                 (int32_t)(ra_etha(k_ra_etha_port_0)->EAMC & (uint32_t)k_ra_etha_mask_opc));
  TEST_END("etha open EAMC transition");
}

/* --- Security gate --- */

static void test_set_security(void)
{
  TEST_BEGIN("etha set security gate");
  prep();
  const ra_etha_config_t cfg = {.initial_mode = k_ra_etha_opc_config,
                                .eaeie0_mask  = 0U,
                                .eaeie1_mask  = 0U,
                                .eaeie2_mask  = 0U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_etha_init(k_ra_etha_port_0, &cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_etha_set_security(k_ra_etha_port_0, 0x00FF00FFU));
  TEST_ASSERT_EQ((int32_t)0x00FF00FFU, (int32_t)ra_etha(k_ra_etha_port_0)->EASCR);
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_etha_set_security((ra_etha_port_t)(uint8_t)k_ra_etha_port_count, 0U));
  TEST_END("etha set security gate");
}

int32_t main(void)
{
  test_init_happy();
  test_init_null();
  test_init_bad_port();
  test_status_read_and_clear();
  test_clear_status_blocks();
  test_enable_disable_irq();
  test_attach_and_dispatch();
  test_power_transition();
  test_lifecycle_bad_args();
  test_queue_arb_and_depth();
  test_get_queue_level();
  test_set_preemption();
  test_set_max_frame_size();
  test_set_ipv_remap();
  test_set_vlan_mode_and_tag();
  test_rx_tag_filter();
  test_cut_through_queue();
  test_cbs_configure_and_state();
  test_tas_schedule_and_enable();
  test_read_clear_stats();
  test_descriptor_ring_init();
  test_get_stats_and_account();
  test_get_stats_after_deinit();
  test_etha_open_bad_args();
  test_etha_open_eamc_transition();
  test_set_security();
  (void)fprintf(stderr, "[OK  ] test_ra_etha.c\n");
  return 0;
}
