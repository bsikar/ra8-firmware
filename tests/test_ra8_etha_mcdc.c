/**
 * @file test_ra8_etha_mcdc.c
 * @brief MC/DC vector tests for ra8_etha.c compound decisions + the
 *        eth_frame_fixture.h header parser
 *
 * @details Split from test_ra8_etha.c along the test-group seam: this
 * binary owns the `@par MC/DC:` vector suites for the driver's compound
 * boolean guards and the shared Ethernet-frame fixture parser; the
 * sibling test_ra8_etha.c owns the public-API contract tests.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "eth_frame_fixture.h"
#include "ra8_err.h"
#include "ra8_etha.h"
#include "ra8_etha_regs.h"
#include "ra8_fake_mmap.h"
#include "ra8_mstp.h"
#include "unity_minimal.h"

/**
 * @enum etha_mcdc_test_lit_t
 * @brief Named constants for the register stamp patterns and literal
 *        test vectors previously inlined in this file's test bodies.
 */
typedef enum : uint32_t {
  k_etha_mcdc_lit_xaa = 0xAAU, /**< Etha MCDC literal 0xAA. */
  k_etha_mcdc_lit_xbb = 0xBBU, /**< Etha MCDC literal 0xBB. */
  k_etha_mcdc_lit_xcc = 0xCCU, /**< Etha MCDC literal 0xCC. */
  k_etha_mcdc_lit_xdd = 0xDDU, /**< Etha MCDC literal 0xDD. */
  k_etha_mcdc_lit_xee = 0xEEU, /**< Etha MCDC literal 0xEE. */
  k_etha_mcdc_lit_xff = 0xFFU, /**< Etha MCDC literal 0xFF. */
  k_etha_mcdc_lit_x11 = 0x11U, /**< Etha MCDC literal 0x11. */
  k_etha_mcdc_lit_x22 = 0x22U, /**< Etha MCDC literal 0x22. */
  k_etha_mcdc_lit_x33 = 0x33U, /**< Etha MCDC literal 0x33. */
  k_etha_mcdc_lit_x44 = 0x44U, /**< Etha MCDC literal 0x44. */
  k_etha_mcdc_lit_x55 = 0x55U, /**< Etha MCDC literal 0x55. */
  k_etha_mcdc_lit_x66 = 0x66U, /**< Etha MCDC literal 0x66. */
} etha_mcdc_test_lit_t;

/** @brief Per-test fixture reset: fresh peripheral RAM + MSTP model. */
static void prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
}

/**
 * @test test_clear_status_mcdc_port_block
 *
 * @par MC/DC:
 * Decision: `if (!internal_port_ok(port) || !internal_irq_block_ok(block))`
 * (2 conditions, libs/ra8_hal/src/ra8_etha.c line 238)
 * Standard: DO-178C Table A-7 obj 5; ISO 26262 Part 6 Table 12.
 * - Vector 1: port=invalid, block=valid -> C1=T (short-circuits) -> Decision T
 * - Vector 2: port=valid,   block=valid -> C1=F, C2=F -> Decision F (ok)
 * - Vector 3: port=valid,   block=invalid -> C1=F, C2=T -> Decision T
 * Vectors 1+2 vary C1 (decision flips); vectors 2+3 vary C2 with C1=F
 * (decision flips). N+1 = 3 vectors for N=2 conditions: minimal MC/DC.
 * The same compound decision shape repeats in ra8_etha_enable_irq (line
 * 272) and ra8_etha_disable_irq (line 299); a single MC/DC vector set
 * on clear_status discharges the obligation for the textually
 * identical guards (rationale per DO-178C Section 6.4.4.3 source-text
 * equivalence).
 */
static void test_clear_status_mcdc_port_block(void)
{
  TEST_BEGIN("etha clear_status MC/DC: !port_ok || !block_ok");
  prep();
  const ra8_etha_config_t cfg = {.initial_mode = k_ra8_etha_opc_operation,
                                 .eaeie0_mask  = 0U,
                                 .eaeie1_mask  = 0U,
                                 .eaeie2_mask  = 0U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_init(k_ra8_etha_port_0, &cfg));

  /* Vector 1: port out of range, block in range. C1=T short-circuits. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_etha_clear_status((ra8_etha_port_t)(uint8_t)k_ra8_etha_port_count,
                                       k_ra8_etha_irq_class_0,
                                       0U));

  /* Vector 2: port valid, block valid. C1=F, C2=F -> Decision F. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_clear_status(k_ra8_etha_port_0, k_ra8_etha_irq_class_0, 0U));

  /* Vector 3: port valid, block out of range. C1=F, C2=T. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_etha_clear_status(k_ra8_etha_port_0,
                                       (ra8_etha_irq_class_t)(uint8_t)k_ra8_etha_irq_class_count,
                                       0U));
  TEST_END("etha clear_status MC/DC: !port_ok || !block_ok");
}

/**
 * @test test_set_mode_mcdc_port_mode
 *
 * @par MC/DC:
 * Decision: `if (!internal_port_ok(port) ||
 *               (uint32_t)mode > k_ra8_etha_mask_opc)`
 * (2 conditions, libs/ra8_hal/src/ra8_etha.c line 408)
 * Standard: DO-178C Table A-7 obj 5; IEC 61508-3 SIL 3.
 * - Vector 1: port=invalid, mode=valid -> C1=T (short-circuits) -> Decision T
 * - Vector 2: port=valid,   mode<=mask -> C1=F, C2=F -> Decision F (ok)
 * - Vector 3: port=valid,   mode>mask  -> C1=F, C2=T -> Decision T
 */
static void test_set_mode_mcdc_port_mode(void)
{
  TEST_BEGIN("etha set_mode MC/DC: !port_ok || mode > mask");
  prep();
  const ra8_etha_config_t cfg = {.initial_mode = k_ra8_etha_opc_config,
                                 .eaeie0_mask  = 0U,
                                 .eaeie1_mask  = 0U,
                                 .eaeie2_mask  = 0U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_init(k_ra8_etha_port_0, &cfg));

  /* Vector 1: port out of range, valid mode. C1=T short-circuits. */
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_etha_set_mode((ra8_etha_port_t)(uint8_t)k_ra8_etha_port_count, k_ra8_etha_opc_config));

  /* Vector 2: port valid, mode within mask. C1=F, C2=F -> Decision F.
   * Pre-stage EAMS.OPS at CONFIG so the driver's real bounded EAMS poll
   * (no longer short-circuited under RA8_OFF_TARGET, T1-01) is
   * satisfied on poll 0 and the decision-F leg returns k_ra8_ok. */
  ra8_etha(k_ra8_etha_port_0)->EAMS = (uint32_t)k_ra8_etha_ops_config;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_set_mode(k_ra8_etha_port_0, k_ra8_etha_opc_config));

  /* Vector 3: port valid, mode > k_ra8_etha_mask_opc. C1=F, C2=T. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_etha_set_mode(k_ra8_etha_port_0, (ra8_etha_opc_t)(k_ra8_etha_mask_opc + 1U)));
  TEST_END("etha set_mode MC/DC: !port_ok || mode > mask");
}

/**
 * @test test_mcdc_enable_irq_port_block
 *
 * @par MC/DC:
 * Decision: ``if (!internal_port_ok(port) || !internal_irq_block_ok(block))``
 * (2 conditions, libs/ra8_hal/src/ra8_etha.c ra8_etha_enable_irq). N+1 = 3.
 * - V1: port=0 (ok), block=0 (ok)              -> C1=F, C2=F -> dec F (proceeds, ok)
 * - V2: port=99 (oor)                          -> C1=T short-circuits -> dec T -> invalid_arg
 * - V3: port=0 (ok), block=count (oor)         -> C1=F, C2=T -> dec T -> invalid_arg
 * (V1,V2) flips C1 with C2 masked F; (V1,V3) flips C2 with C1 fixed F.
 */
static void test_mcdc_enable_irq_port_block(void)
{
  TEST_BEGIN("etha enable_irq MC/DC: !port_ok || !block_ok");
  prep();
  const ra8_etha_config_t cfg = {.initial_mode = k_ra8_etha_opc_config};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_init(k_ra8_etha_port_0, &cfg));
  /* V1 */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_enable_irq(k_ra8_etha_port_0, k_ra8_etha_irq_class_0, 0xFFU));
  /* V2 */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_etha_enable_irq((ra8_etha_port_t)99U, k_ra8_etha_irq_class_0, 0xFFU));
  /* V3 */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_etha_enable_irq(k_ra8_etha_port_0,
                                     (ra8_etha_irq_class_t)k_ra8_etha_irq_class_count,
                                     0xFFU));
  TEST_END("etha enable_irq MC/DC: !port_ok || !block_ok");
}

/**
 * @test test_mcdc_disable_irq_port_block
 *
 * @par MC/DC:
 * Decision: ``if (!internal_port_ok(port) || !internal_irq_block_ok(block))``
 * (2 conditions, libs/ra8_hal/src/ra8_etha.c ra8_etha_disable_irq). N+1 = 3.
 * Same shape as enable_irq above; vectors mirror.
 */
static void test_mcdc_disable_irq_port_block(void)
{
  TEST_BEGIN("etha disable_irq MC/DC: !port_ok || !block_ok");
  prep();
  const ra8_etha_config_t cfg = {.initial_mode = k_ra8_etha_opc_config};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_init(k_ra8_etha_port_0, &cfg));
  /* V1 */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_disable_irq(k_ra8_etha_port_0, k_ra8_etha_irq_class_0, 0xFFU));
  /* V2 */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_etha_disable_irq((ra8_etha_port_t)99U, k_ra8_etha_irq_class_0, 0xFFU));
  /* V3 */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_etha_disable_irq(k_ra8_etha_port_0,
                                      (ra8_etha_irq_class_t)k_ra8_etha_irq_class_count,
                                      0xFFU));
  TEST_END("etha disable_irq MC/DC: !port_ok || !block_ok");
}

/**
 * @test test_mcdc_set_queue_arb_triple
 *
 * @par MC/DC:
 * Decision: ``if (!internal_port_ok(port) || !internal_tc_ok(tc) ||
 *               (uint32_t)arb > k_ra8_etha_mask_tdqa)``
 * (3 conditions, libs/ra8_hal/src/ra8_etha.c ra8_etha_set_queue_arb).
 *
 * @par DO-178C 6.4.4.3 omission rationale:
 * Full short-circuit MC/DC for N=3 OR requires N+1=4 vectors. We use the
 * canonical short-circuit set; each predicate flips with the others held
 * at their masking value (F).
 * - V1: port=ok, tc=ok, arb=ok  -> all F          -> dec F (proceeds, ok)
 * - V2: port=99 (oor)           -> C1=T short    -> dec T -> invalid_arg
 * - V3: port=ok, tc=99 (oor)    -> C1=F, C2=T short -> dec T -> invalid_arg
 * - V4: port=ok, tc=ok, arb=oor -> C1=F, C2=F, C3=T -> dec T -> invalid_arg
 */
static void test_mcdc_set_queue_arb_triple(void)
{
  TEST_BEGIN("etha set_queue_arb MC/DC: !port_ok || !tc_ok || arb>mask");
  prep();
  const ra8_etha_config_t cfg = {.initial_mode = k_ra8_etha_opc_config};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_init(k_ra8_etha_port_0, &cfg));
  /* V1: all good. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_set_queue_arb(k_ra8_etha_port_0, (ra8_etha_tc_t)0U, 1U));
  /* V2: bad port. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_etha_set_queue_arb((ra8_etha_port_t)99U, (ra8_etha_tc_t)0U, 1U));
  /* V3: bad tc. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_etha_set_queue_arb(k_ra8_etha_port_0, (ra8_etha_tc_t)k_ra8_etha_tc_count, 1U));
  /* V4: bad arb. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_etha_set_queue_arb(k_ra8_etha_port_0, (ra8_etha_tc_t)0U, 0xFFU));
  TEST_END("etha set_queue_arb MC/DC: !port_ok || !tc_ok || arb>mask");
}

/**
 * @test test_mcdc_set_queue_depth_triple
 *
 * @par MC/DC:
 * Decision: ``if (!internal_port_ok(port) || !internal_tc_ok(tc) ||
 *               (uint32_t)depth > k_ra8_etha_mask_dqd)``
 * (3 conditions, libs/ra8_hal/src/ra8_etha.c ra8_etha_set_queue_depth).
 * Short-circuit OR: N+1 = 4 vectors.
 * - V1: port=ok, tc=ok, depth=ok    -> all F        -> dec F (proceeds, ok)
 * - V2: port=oor                    -> C1=T short   -> dec T -> invalid_arg
 * - V3: port=ok, tc=oor             -> C1=F, C2=T   -> dec T -> invalid_arg
 * - V4: port=ok, tc=ok, depth=oor   -> C1=F, C2=F, C3=T -> dec T -> invalid_arg
 * V1+V2 prove C1 independence; V2->V3 with C1 held F proves C2; V3->V4 with
 * C1,C2 held F proves C3.
 */
static void test_mcdc_set_queue_depth_triple(void)
{
  TEST_BEGIN("etha set_queue_depth MC/DC: !port_ok || !tc_ok || depth>mask");
  prep();
  const ra8_etha_config_t cfg = {.initial_mode = k_ra8_etha_opc_config};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_init(k_ra8_etha_port_0, &cfg));
  /* V1: all good. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_set_queue_depth(k_ra8_etha_port_0, (ra8_etha_tc_t)0U, 16U));
  /* V2: bad port. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_etha_set_queue_depth((ra8_etha_port_t)99U, (ra8_etha_tc_t)0U, 16U));
  /* V3: bad tc. */
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_etha_set_queue_depth(k_ra8_etha_port_0, (ra8_etha_tc_t)k_ra8_etha_tc_count, 16U));
  /* V4: bad depth (> 0x7FF mask). */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_etha_set_queue_depth(k_ra8_etha_port_0, (ra8_etha_tc_t)0U, 0x800U));
  TEST_END("etha set_queue_depth MC/DC: !port_ok || !tc_ok || depth>mask");
}

/**
 * @test test_mcdc_descriptor_ring_args
 *
 * @par MC/DC:
 * Decision in ``internal_ring_args_ok`` (libs/ra8_hal/src/ra8_etha.c):
 * ``return (num_tx >= MIN) && (num_tx <= MAX) && (num_rx >= MIN) &&
 *          (num_rx <= MAX) && (buffer_size >= BUF_MIN) && (buffer_size <= BUF_MAX)``
 * 6-condition AND chain. Short-circuit MC/DC requires N+1 = 7 vectors.
 * Reached via ra8_etha_descriptor_ring_init.
 * - V1: all in range                     -> all T -> ok (control)
 * - V2: num_tx = 0                       -> C1=F short -> err
 * - V3: num_tx = 4097                    -> C1=T, C2=F -> err
 * - V4: num_rx = 0                       -> C3=F -> err
 * - V5: num_rx = 4097                    -> C4=F -> err
 * - V6: buffer_size = 1                  -> C5=F -> err
 * - V7: buffer_size = 16384              -> C6=F -> err
 * Each vector pairs with V1 to prove independence of one condition.
 */
static void test_mcdc_descriptor_ring_args(void)
{
  TEST_BEGIN("etha descriptor_ring_init MC/DC: 6-cond AND args_ok");
  prep();
  const ra8_etha_config_t cfg = {.initial_mode = k_ra8_etha_opc_config};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_init(k_ra8_etha_port_0, &cfg));
  /* V1: control - all in range. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_descriptor_ring_init(k_ra8_etha_port_0, 4U, 4U, 1500U));
  /* V2: num_tx below min. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_etha_descriptor_ring_init(k_ra8_etha_port_0, 0U, 4U, 1500U));
  /* V3: num_tx above max. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_etha_descriptor_ring_init(k_ra8_etha_port_0, 4097U, 4U, 1500U));
  /* V4: num_rx below min. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_etha_descriptor_ring_init(k_ra8_etha_port_0, 4U, 0U, 1500U));
  /* V5: num_rx above max. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_etha_descriptor_ring_init(k_ra8_etha_port_0, 4U, 4097U, 1500U));
  /* V6: buffer_size below min (64). */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_etha_descriptor_ring_init(k_ra8_etha_port_0, 4U, 4U, 1U));
  /* V7: buffer_size above max (16383). */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_etha_descriptor_ring_init(k_ra8_etha_port_0, 4U, 4U, 16384U));
  TEST_END("etha descriptor_ring_init MC/DC: 6-cond AND args_ok");
}

/**
 * @test test_mcdc_set_vlan_tag_six
 *
 * @par MC/DC:
 * Decision (libs/ra8_hal/src/ra8_etha.c): 6-condition OR over the six
 * range checks (c.vid, s.vid, c.pcp, s.pcp, c.dei, s.dei). Short-circuit MC/DC
 * requires N+1 = 7 vectors.
 * Masks: vid<=0xFFF, pcp<=0x7, dei<=0x1.
 * - V1: all in range                 -> all F -> dec F -> ok (control)
 * - V2: c.vid=0x1000                 -> C1=T short -> err
 * - V3: s.vid=0x1000                 -> C1=F, C2=T -> err
 * - V4: c.pcp=8                      -> C3=T -> err
 * - V5: s.pcp=8                      -> C4=T -> err
 * - V6: c.dei=2                      -> C5=T -> err
 * - V7: s.dei=2                      -> C6=T -> err
 * Each Vi (i>=2) flips condition Ci with all earlier conditions held F.
 */
static void test_mcdc_set_vlan_tag_six(void)
{
  TEST_BEGIN("etha set_vlan_tag MC/DC: 6-cond OR field range");
  prep();
  const ra8_etha_config_t cfg = {.initial_mode = k_ra8_etha_opc_config};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_init(k_ra8_etha_port_0, &cfg));
  const ra8_etha_vlan_tag_t ok_c = {.vid = 100U, .pcp = 5U, .dei = 1U};
  const ra8_etha_vlan_tag_t ok_s = {.vid = 200U, .pcp = 3U, .dei = 0U};
  /* V1 */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_set_vlan_tag(k_ra8_etha_port_0, &ok_c, &ok_s));
  /* V2: c.vid out. */
  const ra8_etha_vlan_tag_t v2 = {.vid = 0x1000U, .pcp = 0U, .dei = 0U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_etha_set_vlan_tag(k_ra8_etha_port_0, &v2, &ok_s));
  /* V3: s.vid out. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_etha_set_vlan_tag(k_ra8_etha_port_0, &ok_c, &v2));
  /* V4: c.pcp out. */
  const ra8_etha_vlan_tag_t v4 = {.vid = 0U, .pcp = 8U, .dei = 0U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_etha_set_vlan_tag(k_ra8_etha_port_0, &v4, &ok_s));
  /* V5: s.pcp out. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_etha_set_vlan_tag(k_ra8_etha_port_0, &ok_c, &v4));
  /* V6: c.dei out. */
  const ra8_etha_vlan_tag_t v6 = {.vid = 0U, .pcp = 0U, .dei = 2U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_etha_set_vlan_tag(k_ra8_etha_port_0, &v6, &ok_s));
  /* V7: s.dei out. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_etha_set_vlan_tag(k_ra8_etha_port_0, &ok_c, &v6));
  TEST_END("etha set_vlan_tag MC/DC: 6-cond OR field range");
}

/**
 * @test test_mcdc_get_queue_level_pair
 *
 * @par MC/DC:
 * Decision: ``if (!internal_port_ok(port) || !internal_tc_ok(tc))``
 * (2 conditions, libs/ra8_hal/src/ra8_etha.c ra8_etha_get_queue_level).
 * Short-circuit OR: N+1 = 3 vectors.
 * - V1: port=ok, tc=ok      -> C1=F, C2=F -> dec F (proceeds, ok).
 * - V2: port=oor            -> C1=T short -> dec T -> invalid_arg.
 *   (V1->V2 isolates C1.)
 * - V3: port=ok, tc=oor     -> C1=F, C2=T -> dec T -> invalid_arg.
 *   (V1->V3 isolates C2.)
 */
static void test_mcdc_get_queue_level_pair(void)
{
  TEST_BEGIN("etha get_queue_level MC/DC: !port_ok || !tc_ok");
  prep();
  const ra8_etha_config_t cfg = {.initial_mode = k_ra8_etha_opc_config};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_init(k_ra8_etha_port_0, &cfg));
  uint16_t cur = 0U;
  uint16_t pk  = 0U;
  /* V1: all good. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_etha_get_queue_level(k_ra8_etha_port_0, (ra8_etha_tc_t)0U, &cur, &pk));
  /* V2: bad port (C1=T, short-circuits). */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_etha_get_queue_level((ra8_etha_port_t)99U, (ra8_etha_tc_t)0U, &cur, &pk));
  /* V3: bad tc (C1=F, C2=T). */
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_etha_get_queue_level(k_ra8_etha_port_0, (ra8_etha_tc_t)k_ra8_etha_tc_count, &cur, &pk));
  TEST_END("etha get_queue_level MC/DC: !port_ok || !tc_ok");
}

/**
 * @test test_mcdc_set_max_frame_size_pair
 *
 * @par MC/DC:
 * Decision: ``if (!internal_port_ok(port) || !internal_tc_ok(tc))``
 * (2 conditions, libs/ra8_hal/src/ra8_etha.c ra8_etha_set_max_frame_size).
 * Short-circuit OR: N+1 = 3 vectors.
 * - V1: port=ok, tc=ok -> dec F.   (control case).
 * - V2: port=oor       -> C1=T short, dec T (V1 vs V2 isolates C1).
 * - V3: port=ok, tc=oor-> C1=F,C2=T, dec T (V1 vs V3 isolates C2).
 */
static void test_mcdc_set_max_frame_size_pair(void)
{
  TEST_BEGIN("etha set_max_frame_size MC/DC: !port_ok || !tc_ok");
  prep();
  const ra8_etha_config_t cfg = {.initial_mode = k_ra8_etha_opc_config};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_init(k_ra8_etha_port_0, &cfg));
  /* V1: all good. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_etha_set_max_frame_size(k_ra8_etha_port_0, (ra8_etha_tc_t)0U, 1500U));
  /* V2: bad port. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_etha_set_max_frame_size((ra8_etha_port_t)99U, (ra8_etha_tc_t)0U, 1500U));
  /* V3: bad tc. */
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_etha_set_max_frame_size(k_ra8_etha_port_0, (ra8_etha_tc_t)k_ra8_etha_tc_count, 1500U));
  TEST_END("etha set_max_frame_size MC/DC: !port_ok || !tc_ok");
}

/**
 * @test test_mcdc_configure_cut_through_pair
 *
 * @par MC/DC:
 * Decision: ``if (!internal_port_ok(port) || (uint32_t)dqd > k_ra8_etha_mask_ctdqd)``
 * (2 conditions, libs/ra8_hal/src/ra8_etha.c ra8_etha_configure_cut_through).
 * Short-circuit OR: N+1 = 3 vectors.
 * - V1: port=ok, dqd=ok -> dec F.
 * - V2: port=oor        -> C1=T short, dec T (V1 vs V2 isolates C1).
 * - V3: port=ok, dqd=oor-> C1=F,C2=T, dec T (V1 vs V3 isolates C2).
 *
 * @par MC/DC (paired):
 * Also exercises libs/ra8_hal/src/ra8_etha.c (configure_cbs port||tc),
 * 1035 (cbs param range), 1091 (get_cbs_state port||tc) by issuing
 * configure_cbs and get_cbs_state with the same fault vectors. The
 * pre-existing happy-path tests already supply the all-F vector for
 * each, so adding the C1=T and C1=F,C2=T failures completes MC/DC.
 */
static void test_mcdc_configure_cut_through_pair(void)
{
  TEST_BEGIN("etha configure_cut_through + cbs MC/DC");
  prep();
  const ra8_etha_config_t cfg = {.initial_mode = k_ra8_etha_opc_config};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_init(k_ra8_etha_port_0, &cfg));

  /* configure_cut_through (line 993) */
  /* V1: all ok. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_configure_cut_through(k_ra8_etha_port_0, 16U, 1U));
  /* V2: bad port. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_etha_configure_cut_through((ra8_etha_port_t)99U, 16U, 1U));
  /* V3: bad dqd (mask is 0x7F so 0x80 is out-of-range). */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_etha_configure_cut_through(k_ra8_etha_port_0, 16U, 0x80U));

  /* configure_cbs (line 1029, 1035) */
  const ra8_etha_cbs_param_t ok_cbs = {.increment = 100U, .upper_lim = 1000U};
  /* V1 (1029): all ok, enable=1. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_etha_configure_cbs(k_ra8_etha_port_0, (ra8_etha_tc_t)0U, 1U, &ok_cbs));
  /* V2 (1029): bad port. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_etha_configure_cbs((ra8_etha_port_t)99U, (ra8_etha_tc_t)0U, 1U, &ok_cbs));
  /* V3 (1029): bad tc. */
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_etha_configure_cbs(k_ra8_etha_port_0, (ra8_etha_tc_t)k_ra8_etha_tc_count, 1U, &ok_cbs));
  /* V_param_lo (1035): increment exceeds k_ra8_etha_mask_civ (0xFFFFF). */
  const ra8_etha_cbs_param_t bad_inc = {.increment = 0x100000U, .upper_lim = 1U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_etha_configure_cbs(k_ra8_etha_port_0, (ra8_etha_tc_t)0U, 1U, &bad_inc));
  /* V_param_hi (1035): upper_lim exceeds k_ra8_etha_mask_cul (0x7FFFFFFF). */
  const ra8_etha_cbs_param_t bad_ul = {.increment = 1U, .upper_lim = 0x80000000U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_etha_configure_cbs(k_ra8_etha_port_0, (ra8_etha_tc_t)0U, 1U, &bad_ul));

  /* get_cbs_state (line 1091) */
  uint8_t              en   = 0U;
  uint8_t              gop  = 0U;
  ra8_etha_cbs_param_t oprm = {};
  /* V1: all ok. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_etha_get_cbs_state(k_ra8_etha_port_0, (ra8_etha_tc_t)0U, &en, &gop, &oprm));
  /* V2: bad port. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_etha_get_cbs_state((ra8_etha_port_t)99U, (ra8_etha_tc_t)0U, &en, &gop, &oprm));
  /* V3: bad tc. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,

                 ra8_etha_get_cbs_state(k_ra8_etha_port_0,
                                        (ra8_etha_tc_t)k_ra8_etha_tc_count,
                                        &en,
                                        &gop,
                                        &oprm));

  TEST_END("etha configure_cut_through + cbs MC/DC");
}

/**
 * @test test_mcdc_eth_frame_parse_quad
 *
 * @par MC/DC:
 * Decision (tests/eth_frame_fixture.h@eff_parse_eth_header eff_parse_eth_header):
 * 4-condition OR over the four NULL guards
 * ``frame == nullptr || out_dst == nullptr || out_src == nullptr ||
 *   out_etype == nullptr``. Short-circuit MC/DC requires N+1 = 5
 * vectors:
 *   - V1: all four pointers non-NULL                  -> dec F -> ok.
 *   - V2: frame=NULL, others valid                    -> C1=T short.
 *   - V3: frame valid, out_dst=NULL, others valid     -> C1=F, C2=T.
 *   - V4: frame valid, out_dst valid, out_src=NULL    -> C1..C2=F, C3=T.
 *   - V5: frame valid, out_dst/src valid, out_etype=NULL -> C1..C3=F, C4=T.
 * Each Vi (i>=2) flips condition Ci with all earlier conditions held F.
 */
static void test_mcdc_eth_frame_parse_quad(void)
{
  TEST_BEGIN("eth frame fixture MC/DC: 4-cond OR null-check");
  enum : uint16_t {
    k_test_etype_ipv4 = 0x0800U, /**< IPv4 EtherType encoded in the fixture frame. */
  };
  uint8_t  frame[16] = {k_etha_mcdc_lit_xaa,
                        k_etha_mcdc_lit_xbb,
                        k_etha_mcdc_lit_xcc,
                        k_etha_mcdc_lit_xdd,
                        k_etha_mcdc_lit_xee,
                        k_etha_mcdc_lit_xff, /* dst */
                        k_etha_mcdc_lit_x11,
                        k_etha_mcdc_lit_x22,
                        k_etha_mcdc_lit_x33,
                        k_etha_mcdc_lit_x44,
                        k_etha_mcdc_lit_x55,
                        k_etha_mcdc_lit_x66, /* src */
                        0x08U,
                        0x00U, /* etype = IPv4 */
                        0x00U,
                        0x00U};
  uint8_t  out_dst[k_eff_mac_bytes];
  uint8_t  out_src[k_eff_mac_bytes];
  uint16_t out_etype = 0U;
  /* V1: all valid -- and the decoded fields match the header bytes. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    eff_parse_eth_header(frame, (uint32_t)sizeof(frame), out_dst, out_src, &out_etype));
  TEST_ASSERT_EQ(0, memcmp(out_dst, &frame[0], (size_t)k_eff_mac_bytes));
  TEST_ASSERT_EQ(0, memcmp(out_src, &frame[k_eff_mac_bytes], (size_t)k_eff_mac_bytes));
  TEST_ASSERT_EQ(k_test_etype_ipv4, out_etype);
  /* V2: frame NULL (C1=T short). */
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    eff_parse_eth_header(nullptr, (uint32_t)sizeof(frame), out_dst, out_src, &out_etype));
  /* V3: out_dst NULL (C1=F, C2=T). */
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    eff_parse_eth_header(frame, (uint32_t)sizeof(frame), nullptr, out_src, &out_etype));
  /* V4: out_src NULL (C1..C2=F, C3=T). */
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    eff_parse_eth_header(frame, (uint32_t)sizeof(frame), out_dst, nullptr, &out_etype));
  /* V5: out_etype NULL (C1..C3=F, C4=T). */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 eff_parse_eth_header(frame, (uint32_t)sizeof(frame), out_dst, out_src, nullptr));
  TEST_END("eth frame fixture MC/DC: 4-cond OR null-check");
}

/**
 * @test test_eth_frame_parse_short_frame
 *
 * @brief Cover the frame-too-short early return in ::eff_parse_eth_header.
 *
 * @details ``eff_parse_eth_header`` requires at least ::k_eff_hdr_bytes
 * bytes (a complete Ethernet II header: destination MAC, source MAC,
 * EtherType). Supplying one byte fewer exercises the length guard and
 * returns ``k_ra8_err_invalid_arg`` without writing any output.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- the ``frame_len`` guard is a
 * single-condition comparison; its true branch is covered here, the
 * false branch by ::test_mcdc_eth_frame_parse_quad vector V1)
 */
static void test_eth_frame_parse_short_frame(void)
{
  TEST_BEGIN("eth frame fixture: short frame -> k_ra8_err_invalid_arg");
  const uint8_t frame[k_eff_hdr_bytes] = {};
  uint8_t       out_dst[k_eff_mac_bytes];
  uint8_t       out_src[k_eff_mac_bytes];
  uint16_t      out_etype = 0U;
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    eff_parse_eth_header(frame, (uint32_t)k_eff_hdr_bytes - 1U, out_dst, out_src, &out_etype));
  TEST_ASSERT_EQ(0U, out_etype);
  TEST_END("eth frame fixture: short frame -> k_ra8_err_invalid_arg");
}

int main(void)
{
  test_clear_status_mcdc_port_block();
  test_set_mode_mcdc_port_mode();
  test_mcdc_enable_irq_port_block();
  test_mcdc_disable_irq_port_block();
  test_mcdc_set_queue_arb_triple();
  test_mcdc_set_queue_depth_triple();
  test_mcdc_descriptor_ring_args();
  test_mcdc_set_vlan_tag_six();
  test_mcdc_get_queue_level_pair();
  test_mcdc_set_max_frame_size_pair();
  test_mcdc_configure_cut_through_pair();
  test_mcdc_eth_frame_parse_quad();
  test_eth_frame_parse_short_frame();
  return 0;
}
