/**
 * @file test_ra8_mipi_dsi_cmd.c
 * @brief Unit tests for MIPI DSI-2 init, IRQ dispatch and command-mode packets
 *
 * @details
 * Split sibling of the original test_ra8_mipi_dsi.c suite covering
 * link bring-up and the command-mode datapath of ra8_mipi_dsi.c
 * against the ``ra8_fake_mmap``-backed register window:
 *
 * - init happy path, null-cfg and lane-count rejection
 * - status get / clear and per-class IRQ dispatch fan-out
 * - short / long packet transmit incl. VC, busy, LP-size and
 *   null-data guards, HS routing and the aux-op path
 * - BTA read-packet flow and ULPS enter / exit incl. the
 *   continuous-clock-lane rejection
 *
 * Sibling suites: test_ra8_mipi_dsi_video.c (video mode + sweep-6
 * command shorthands) and test_ra8_mipi_dsi_mcdc.c (MC/DC vectors).
 * Shared fixtures live in support/mipi_dsi_test_util.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_mipi_dsi.h"
#include "ra8_mipi_dsi_regs.h"
#include "ra8_mstp.h"
#include "support/mipi_dsi_test_util.h"
#include "unity_minimal.h"

/**
 * @enum t_cmd_t
 * @brief Lane count, status seeds and payload base for the command-mode arms.
 */
typedef enum : uint8_t {
  k_t_lane_count_over = 5U,    /**< Lanes past the four the D-PHY supports. */
  k_t_status_all_ones = 0xFFU, /**< Written to the sequence and video status
                                    registers so a driver that clears the wrong
                                    one leaves a visible residue.              */
  k_t_payload_base    = 0xA0U, /**< First byte of the ascending DCS payload;
                                    high enough not to collide with the DCS
                                    command bytes the arms also send.           */
} t_cmd_t;

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_happy(void)
{
  TEST_BEGIN("mipi_dsi init happy");
  prep();

  const ra8_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));

  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  /* TXSETR: 2 lanes (NUMLANE=1) + CLEN + DLEN. */
  const uint32_t expected_txsetr = (uint32_t)k_ra8_mipi_dsi_txset_lane2 |
                                   (uint32_t)k_ra8_mipi_dsi_txset_clen |
                                   (uint32_t)k_ra8_mipi_dsi_txset_dlen;
  TEST_ASSERT_EQ(expected_txsetr, reg->TXSETR);
  /* DSISETR: ECCEN + EOTPEN + MRPSZ + VC0 CRC. */
  TEST_ASSERT(((reg->DSISETR & 0xFFFFU) == (uint32_t)k_test_max_return_pkt));
  TEST_ASSERT(((reg->DSISETR >> 16) & 1U) == 1U);
  TEST_ASSERT(((reg->DSISETR >> 31) & 1U) == 1U);
  TEST_ASSERT(((reg->DSISETR >> 20) & 1U) == 1U);
  /* ULPSSETR.WKUP. */
  TEST_ASSERT_EQ(k_test_ulps_wkup, reg->ULPSSETR);
  /* Bus timeouts loaded. */
  TEST_ASSERT_EQ(0x1000, reg->HSTXTOSETR);
  TEST_ASSERT_EQ(0x2000, reg->LRXHTOSETR);
  TEST_ASSERT_EQ(0x3000, reg->TATOSETR);
  TEST_ASSERT_EQ(0x4000, reg->PRESPTOBTASETR);

  TEST_END("mipi_dsi init happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_null_cfg(void)
{
  TEST_BEGIN("mipi_dsi init null cfg");
  prep();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mipi_dsi_init(nullptr));
  TEST_END("mipi_dsi init null cfg");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_bad_lane_count(void)
{
  TEST_BEGIN("mipi_dsi init bad lane count");
  prep();

  ra8_mipi_dsi_config_t cfg = make_cfg();
  cfg.lane_count            = (ra8_mipi_dsi_lane_count_t)0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_dsi_init(&cfg));

  cfg.lane_count = (ra8_mipi_dsi_lane_count_t)k_t_lane_count_over;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_dsi_init(&cfg));
  TEST_END("mipi_dsi init bad lane count");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_status_get_clear(void)
{
  TEST_BEGIN("mipi_dsi status get + clear");
  prep();

  const ra8_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));

  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  reg->ISR                        = (uint32_t)k_test_isr_seed;
  reg->SQCH0SCR                   = k_t_status_all_ones;
  reg->VMSCR                      = k_t_status_all_ones;

  uint32_t mask = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_get_status(&mask));
  TEST_ASSERT_EQ(k_test_isr_seed, mask);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mipi_dsi_get_status(nullptr));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_clear_status(mask));
  TEST_END("mipi_dsi status get + clear");
}

static uint32_t             s_cb_count;
static uint32_t             s_cb_last_mask;
static void*                s_cb_last_ctx;
static ra8_mipi_dsi_event_t s_cb_last_event;

static void stub_dsi_cb(void* ctx, ra8_mipi_dsi_event_t event, uint32_t status_mask)
{
  ++s_cb_count;
  s_cb_last_event = event;
  s_cb_last_mask  = status_mask;
  s_cb_last_ctx   = ctx;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("mipi_dsi attach + dispatch");
  prep();

  const ra8_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));

  s_cb_count            = 0U;
  s_cb_last_mask        = 0U;
  s_cb_last_ctx         = nullptr;
  s_cb_last_event       = k_ra8_mipi_dsi_event_phy;
  void* const ctx_token = (void*)(uintptr_t)0xDEADBEEFUL;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_attach_handler(stub_dsi_cb, ctx_token));

  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  reg->ISR                        = (uint32_t)k_ra8_mipi_dsi_isr_sq0;
  reg->SQCH0SR                    = (uint32_t)k_ra8_mipi_dsi_sqch_aactfin;
  ra8_mipi_dsi_dispatch();
  TEST_ASSERT_EQ(1, s_cb_count);
  TEST_ASSERT_EQ(k_ra8_mipi_dsi_event_seq0, s_cb_last_event);
  TEST_ASSERT(s_cb_last_ctx == ctx_token);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_attach_handler(nullptr, nullptr));
  TEST_END("mipi_dsi attach + dispatch");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_dispatch_per_class(void)
{
  TEST_BEGIN("mipi_dsi per-class dispatch");
  prep();

  const ra8_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));

  s_cb_count = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_attach_handler(stub_dsi_cb, nullptr));

  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();

  reg->SQCH1SR = (uint32_t)k_ra8_mipi_dsi_sqch_aactfin;
  ra8_mipi_dsi_dispatch_seq1();
  TEST_ASSERT_EQ(k_ra8_mipi_dsi_event_seq1, s_cb_last_event);

  reg->VMSR = (uint32_t)k_ra8_mipi_dsi_vmsr_virdy;
  ra8_mipi_dsi_dispatch_video();
  TEST_ASSERT_EQ(k_ra8_mipi_dsi_event_video, s_cb_last_event);

  reg->RXSR = (uint32_t)k_ra8_mipi_dsi_rxsr_btarend;
  ra8_mipi_dsi_dispatch_receive();
  TEST_ASSERT_EQ(k_ra8_mipi_dsi_event_receive, s_cb_last_event);

  reg->FERRSR = (uint32_t)k_ra8_mipi_dsi_ferrsr_htxto;
  ra8_mipi_dsi_dispatch_fatal();
  TEST_ASSERT_EQ(k_ra8_mipi_dsi_event_fatal, s_cb_last_event);

  reg->PLSR = (uint32_t)k_ra8_mipi_dsi_plsr_clulpent;
  ra8_mipi_dsi_dispatch_phy();
  TEST_ASSERT_EQ(k_ra8_mipi_dsi_event_phy, s_cb_last_event);
  TEST_END("mipi_dsi per-class dispatch");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_send_short_packet_happy(void)
{
  TEST_BEGIN("mipi_dsi send short packet happy");
  prep();

  const ra8_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_dsi_send_short_packet(k_ra8_mipi_dsi_dt_dcs_short_write_1,
                                                k_ra8_mipi_dsi_vc0,
                                                (uint8_t)k_test_dcs_soft_reset,
                                                (uint8_t)k_test_param1));

  volatile r_mipi_dsi_regs_t* reg   = ra8_mipi_dsi();
  const uint32_t              dsc_a = reg->SQCH0DSC[0].A;
  TEST_ASSERT_EQ(k_test_dcs_soft_reset, (dsc_a & 0xFFU));
  TEST_ASSERT_EQ(k_test_param1, ((dsc_a >> 8) & 0xFFU));
  TEST_ASSERT_EQ(k_ra8_mipi_dsi_dt_dcs_short_write_1, ((dsc_a >> 16) & 0x3FU));
  TEST_ASSERT(((dsc_a >> 25) & 1U) == 1U); /* SPD = LP escape */
  /* SQCH0SET0R contains CHSEL + START since channel = 0. */
  TEST_ASSERT_EQ(((uint32_t)k_ra8_mipi_dsi_sqch_chsel | (uint32_t)k_ra8_mipi_dsi_sqch_start),
                 reg->SQCH0SET0R);
  /* SQCH1SET0R receives just CHSEL (no START since channel != 1). */
  TEST_ASSERT_EQ(((uint32_t)k_ra8_mipi_dsi_sqch_chsel), reg->SQCH1SET0R);

  TEST_END("mipi_dsi send short packet happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_send_short_packet_bad_vc(void)
{
  TEST_BEGIN("mipi_dsi send short packet bad vc");
  prep();

  const ra8_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_mipi_dsi_send_short_packet(k_ra8_mipi_dsi_dt_gen_short_write_0,
                                                (ra8_mipi_dsi_vc_t)k_test_bad_vc,
                                                (uint8_t)k_test_param0,
                                                (uint8_t)k_test_param1));
  TEST_END("mipi_dsi send short packet bad vc");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_send_short_packet_busy(void)
{
  TEST_BEGIN("mipi_dsi send short packet busy");
  prep();

  const ra8_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));

  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  reg->LINKSR                     = (uint32_t)k_ra8_mipi_dsi_link_sq0run;

  TEST_ASSERT_EQ(k_ra8_err_busy,
                 ra8_mipi_dsi_send_short_packet(k_ra8_mipi_dsi_dt_dcs_short_write_0,
                                                k_ra8_mipi_dsi_vc0,
                                                0U,
                                                0U));
  TEST_END("mipi_dsi send short packet busy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_send_long_packet(void)
{
  TEST_BEGIN("mipi_dsi send long packet (LP)");
  prep();

  const ra8_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));

  uint8_t payload[k_test_long_len];
  for (uint32_t i = 0U; i < k_test_long_len; ++i) {
    payload[i] = (uint8_t)(k_t_payload_base + i);
  }
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_dsi_send_long_packet(k_ra8_mipi_dsi_dt_dcs_long_write,
                                               k_ra8_mipi_dsi_vc0,
                                               payload,
                                               (uint16_t)k_test_long_len,
                                               true));
  volatile r_mipi_dsi_regs_t* reg   = ra8_mipi_dsi();
  const uint32_t              dsc_a = reg->SQCH0DSC[0].A;
  /* DATA0 / DATA1 must encode the word count. */
  TEST_ASSERT_EQ(k_test_long_len, (dsc_a & 0xFFU));
  TEST_ASSERT_EQ(0, ((dsc_a >> 8) & 0xFFU));
  /* FMT bit set. */
  TEST_ASSERT(((dsc_a >> 24) & 1U) == 1U);
  /* TXPPD0R has the first 4 payload bytes. */
  TEST_ASSERT_EQ(0xA3A2A1A0UL, reg->TXPPD0R);
  TEST_END("mipi_dsi send long packet (LP)");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_send_long_packet_lp_too_big(void)
{
  TEST_BEGIN("mipi_dsi send long packet too big for LP");
  prep();

  const ra8_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));

  static uint8_t s_big[k_test_huge_len];
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_mipi_dsi_send_long_packet(k_ra8_mipi_dsi_dt_gen_long_write,
                                               k_ra8_mipi_dsi_vc0,
                                               s_big,
                                               (uint16_t)k_test_huge_len,
                                               true));
  TEST_END("mipi_dsi send long packet too big for LP");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_send_long_packet_null_data(void)
{
  TEST_BEGIN("mipi_dsi send long packet with null data + nonzero len");
  prep();

  const ra8_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_mipi_dsi_send_long_packet(k_ra8_mipi_dsi_dt_gen_long_write,
                                               k_ra8_mipi_dsi_vc0,
                                               nullptr,
                                               (uint16_t)k_test_long_len,
                                               true));
  TEST_END("mipi_dsi send long packet with null data + nonzero len");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_send_long_packet_hs(void)
{
  TEST_BEGIN("mipi_dsi send long packet HS uses ch1");
  prep();

  const ra8_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));

  uint8_t payload[k_test_long_len];
  for (uint32_t i = 0U; i < k_test_long_len; ++i) {
    payload[i] = (uint8_t)(0x10U + i);
  }
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_dsi_send_long_packet(k_ra8_mipi_dsi_dt_gen_long_write,
                                               k_ra8_mipi_dsi_vc0,
                                               payload,
                                               (uint16_t)k_test_long_len,
                                               false));
  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  /* SPD bit (LP) must be 0 for HS. */
  TEST_ASSERT(((reg->SQCH1DSC[0].A >> 25) & 1U) == 0U);
  /* SQCH1SET0R contains CHSEL + START since channel = 1. */
  TEST_ASSERT_EQ(((uint32_t)k_ra8_mipi_dsi_sqch_chsel | (uint32_t)k_ra8_mipi_dsi_sqch_start),
                 reg->SQCH1SET0R);
  TEST_END("mipi_dsi send long packet HS uses ch1");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_send_command_aux_op(void)
{
  TEST_BEGIN("mipi_dsi send command aux op (skew cal)");
  prep();

  const ra8_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));

  const ra8_mipi_dsi_command_t cmd = {
    .cmd_id          = k_ra8_mipi_dsi_dt_gen_short_write_0,
    .virtual_channel = k_ra8_mipi_dsi_vc0,
    .bta             = k_ra8_mipi_dsi_bta_none,
    .low_power       = false,
    .ack_request     = false,
    .aux_operation   = true,
    .action_code     = (uint8_t)k_test_action_code,
    .tx_len          = 0U,
    .p_tx_buffer     = nullptr,
    .p_rx_buffer     = nullptr,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_send_command(&cmd));
  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  /* Word C should carry FINACT + AUXOP + ACTCODE. */
  TEST_ASSERT(((reg->SQCH1DSC[0].C & (uint32_t)k_ra8_mipi_dsi_dsc0c_auxop) != 0U));
  TEST_ASSERT_EQ(k_test_action_code,
                 ((reg->SQCH1DSC[0].C >> (uint32_t)k_ra8_mipi_dsi_dsc0c_actcode_shift) & 0xFFU));
  TEST_END("mipi_dsi send command aux op (skew cal)");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_send_command_video_running_blocks_lp(void)
{
  TEST_BEGIN("mipi_dsi LP send rejected during video mode");
  prep();

  const ra8_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));

  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  reg->LINKSR                     = (uint32_t)k_ra8_mipi_dsi_link_vrun;

  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_mipi_dsi_send_short_packet(k_ra8_mipi_dsi_dt_dcs_short_write_0,
                                                k_ra8_mipi_dsi_vc0,
                                                0U,
                                                0U));
  TEST_END("mipi_dsi LP send rejected during video mode");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_read_packet(void)
{
  TEST_BEGIN("mipi_dsi read packet (BTA-then-read)");
  prep();

  const ra8_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));

  uint8_t rx[k_ra8_mipi_dsi_payload_max];
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_dsi_read_packet(k_ra8_mipi_dsi_dt_dcs_read,
                                          k_ra8_mipi_dsi_vc0,
                                          0xDAU,
                                          0U,
                                          rx,
                                          (uint16_t)k_ra8_mipi_dsi_payload_max));
  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  /* BTA = bta_read should be encoded into bits 27:26. */
  TEST_ASSERT_EQ(k_ra8_mipi_dsi_bta_read,
                 ((reg->SQCH0DSC[0].A >> (uint32_t)k_ra8_mipi_dsi_dsc0a_shift_bta) & 0x3U));
  /* Word D should point at the rx buffer. */
  TEST_ASSERT_EQ(((uintptr_t)rx & 0xFFFFFFFFUL), reg->SQCH0DSC[0].D);

  /* Bad arg paths. */
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,

    ra8_mipi_dsi_read_packet(k_ra8_mipi_dsi_dt_dcs_read, k_ra8_mipi_dsi_vc0, 0U, 0U, nullptr, 4U));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_mipi_dsi_read_packet(k_ra8_mipi_dsi_dt_dcs_read, k_ra8_mipi_dsi_vc0, 0U, 0U, rx, 0U));
  TEST_END("mipi_dsi read packet (BTA-then-read)");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_ulps_enter_exit(void)
{
  TEST_BEGIN("mipi_dsi ULPS enter + exit");
  prep();

  /* Use non-continuous mode so clock-lane ULPS is allowed. */
  const ra8_mipi_dsi_config_t cfg = make_cfg_non_continuous();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_ulps_enter(k_ra8_mipi_dsi_lane_all));
  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  TEST_ASSERT(((reg->ULPSCR & (uint32_t)k_ra8_mipi_dsi_ulpscr_dlent) != 0U));
  TEST_ASSERT(((reg->ULPSCR & (uint32_t)k_ra8_mipi_dsi_ulpscr_clent) != 0U));

  /* Re-entering shouldn't pulse again -- driver tracks state. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_ulps_enter(k_ra8_mipi_dsi_lane_all));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_ulps_exit(k_ra8_mipi_dsi_lane_all));
  TEST_ASSERT(((reg->ULPSCR & (uint32_t)k_ra8_mipi_dsi_ulpscr_dlexit) != 0U));
  TEST_ASSERT(((reg->ULPSCR & (uint32_t)k_ra8_mipi_dsi_ulpscr_clexit) != 0U));

  /* No-lane variants are rejected. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_dsi_ulps_enter((uint8_t)k_ra8_mipi_dsi_lane_none));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_dsi_ulps_exit((uint8_t)k_ra8_mipi_dsi_lane_none));
  TEST_END("mipi_dsi ULPS enter + exit");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_ulps_clock_lane_continuous_rejected(void)
{
  TEST_BEGIN("mipi_dsi ULPS clock lane rejected in continuous mode");
  prep();

  const ra8_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_mipi_dsi_ulps_enter((uint8_t)k_ra8_mipi_dsi_lane_clock));

  /* Data-lane only is fine. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_ulps_enter((uint8_t)k_ra8_mipi_dsi_lane_data));
  TEST_END("mipi_dsi ULPS clock lane rejected in continuous mode");
}

int main(void)
{
  test_init_happy();
  test_init_null_cfg();
  test_init_bad_lane_count();
  test_status_get_clear();
  test_attach_and_dispatch();
  test_dispatch_per_class();
  test_send_short_packet_happy();
  test_send_short_packet_bad_vc();
  test_send_short_packet_busy();
  test_send_long_packet();
  test_send_long_packet_lp_too_big();
  test_send_long_packet_null_data();
  test_send_long_packet_hs();
  test_send_command_aux_op();
  test_send_command_video_running_blocks_lp();
  test_read_packet();
  test_ulps_enter_exit();
  test_ulps_clock_lane_continuous_rejected();
  return 0;
}
