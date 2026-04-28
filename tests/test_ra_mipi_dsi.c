/**
 * @file test_ra_mipi_dsi.c
 * @brief Host-side unit tests for the full ra_mipi_dsi.c surface
 *
 * @details
 * Exercises every public entry point in `ra_mipi_dsi.h`:
 * lifecycle, HS clock, sequence channels (LP + HS, short, long,
 * read-with-BTA), ULPS enter / exit (with continuous-clock guard),
 * video-mode configure / start / stop, every status / IRQ helper,
 * and per-class dispatch fanout. Backed by `ra_sim_mmap` so the
 * driver writes against ordinary host RAM.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8d2_mipi_dsi_regs.h"
#include "ra_err.h"
#include "ra_mipi_dsi.h"
#include "ra_mstp.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum ra_mipi_dsi_test_const_t
 * @brief Numeric inputs used by the test cases (no magic numbers).
 */
typedef enum : uint32_t {
  k_test_max_return_pkt      = 64U,
  k_test_dcs_soft_reset      = 0x01U,
  k_test_param0              = 0xAAU,
  k_test_param1              = 0x55U,
  k_test_isr_seed            = (uint32_t)k_ra_mipi_dsi_isr_sq0 | (uint32_t)k_ra_mipi_dsi_isr_vm,
  k_test_bad_vc              = 9U,
  k_test_long_len            = 8U,
  k_test_huge_len            = 200U, /* > LP cap (128). */
  k_test_long_payload_first  = 0xDEU,
  k_test_long_payload_second = 0xADU,
  k_test_video_h_act         = 1024U,
  k_test_video_v_act         = 600U,
  k_test_video_hsa           = 12U,
  k_test_video_hbp           = 64U,
  k_test_video_hfp           = 32U,
  k_test_video_vsa           = 4U,
  k_test_video_vbp           = 8U,
  k_test_video_vfp           = 6U,
  k_test_ulps_wkup           = 0x40U,
  k_test_action_code         = 0x24U, /* initial skew calibration */
  k_test_rx_payload_w0       = 0xDEADBEEFUL,
  k_test_rx_payload_w1       = 0xCAFEBABEUL,
  k_test_rx_payload_w2       = 0x12345678UL,
  k_test_rx_payload_w3       = 0x90ABCDEFUL,
} ra_mipi_dsi_test_const_t;

/**
 * @brief Reset the simulated peripheral memory + MSTP table.
 */
static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
}

/**
 * @brief Build a "happy path" config struct.
 */
static ra_mipi_dsi_config_t make_cfg(void)
{
  const ra_mipi_dsi_config_t cfg = {
    .lane_count             = k_ra_mipi_dsi_lanes_2,
    .clock_mode             = k_ra_mipi_dsi_clock_continuous,
    .max_return_packet_size = (uint16_t)k_test_max_return_pkt,
    .ulps_wakeup_period     = (uint8_t)k_test_ulps_wkup,
    .ecc_check_enable       = true,
    .eotp_enable            = true,
    .scramble_enable        = false,
    .tearing_detect_enable  = false,
    .crc_check_vc_mask      = 0x1U,
    .timing =
      {
        .clock_stop_time       = 0x10U,
        .clock_beforehand_time = 0x08U,
        .clock_keep_time       = 0x10U,
        .go_lp_and_back        = 0x20U,
      },
    .timeouts =
      {
        .hs_tx_timeout      = 0x1000U,
        .lp_rx_host_timeout = 0x2000U,
        .turnaround_timeout = 0x3000U,
        .bta_timeout        = 0x4000U,
        .lp_rw_timeout      = 0x50005000U,
        .hs_rw_timeout      = 0x60006000U,
      },
  };
  return cfg;
}

/**
 * @brief Drop a non-continuous-clock variant (needed for ULPS clock-lane test).
 */
static ra_mipi_dsi_config_t make_cfg_non_continuous(void)
{
  ra_mipi_dsi_config_t cfg = make_cfg();
  cfg.clock_mode           = k_ra_mipi_dsi_clock_non_continuous;
  return cfg;
}

static void test_init_happy(void)
{
  TEST_BEGIN("mipi_dsi init happy");
  prep();

  const ra_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_init(&cfg));

  volatile r_mipi_dsi_regs_t* reg = ra_mipi_dsi();
  /* TXSETR: 2 lanes (NUMLANE=1) + CLEN + DLEN. */
  const uint32_t expected_txsetr = (uint32_t)k_ra_mipi_dsi_txset_lane2 |
                                   (uint32_t)k_ra_mipi_dsi_txset_clen |
                                   (uint32_t)k_ra_mipi_dsi_txset_dlen;
  TEST_ASSERT_EQ((int)expected_txsetr, (int)reg->TXSETR);
  /* DSISETR: ECCEN + EOTPEN + MRPSZ + VC0 CRC. */
  TEST_ASSERT(((reg->DSISETR & 0xFFFFU) == (uint32_t)k_test_max_return_pkt));
  TEST_ASSERT(((reg->DSISETR >> 16) & 1U) == 1U);
  TEST_ASSERT(((reg->DSISETR >> 31) & 1U) == 1U);
  TEST_ASSERT(((reg->DSISETR >> 20) & 1U) == 1U);
  /* ULPSSETR.WKUP. */
  TEST_ASSERT_EQ((int)k_test_ulps_wkup, (int)reg->ULPSSETR);
  /* Bus timeouts loaded. */
  TEST_ASSERT_EQ((int)0x1000, (int)reg->HSTXTOSETR);
  TEST_ASSERT_EQ((int)0x2000, (int)reg->LRXHTOSETR);
  TEST_ASSERT_EQ((int)0x3000, (int)reg->TATOSETR);
  TEST_ASSERT_EQ((int)0x4000, (int)reg->PRESPTOBTASETR);

  TEST_END("mipi_dsi init happy");
}

static void test_init_null_cfg(void)
{
  TEST_BEGIN("mipi_dsi init null cfg");
  prep();

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_mipi_dsi_init(nullptr));
  TEST_END("mipi_dsi init null cfg");
}

static void test_init_bad_lane_count(void)
{
  TEST_BEGIN("mipi_dsi init bad lane count");
  prep();

  ra_mipi_dsi_config_t cfg = make_cfg();
  cfg.lane_count           = (ra_mipi_dsi_lane_count_t)0U;
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_mipi_dsi_init(&cfg));

  cfg.lane_count = (ra_mipi_dsi_lane_count_t)5U;
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_mipi_dsi_init(&cfg));
  TEST_END("mipi_dsi init bad lane count");
}

static void test_status_get_clear(void)
{
  TEST_BEGIN("mipi_dsi status get + clear");
  prep();

  const ra_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_init(&cfg));

  volatile r_mipi_dsi_regs_t* reg = ra_mipi_dsi();
  reg->ISR                        = (uint32_t)k_test_isr_seed;
  reg->SQCH0SCR                   = 0xFFU;
  reg->VMSCR                      = 0xFFU;

  uint32_t mask = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_get_status(&mask));
  TEST_ASSERT_EQ((int)k_test_isr_seed, (int)mask);

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_mipi_dsi_get_status(nullptr));

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_clear_status(mask));
  TEST_END("mipi_dsi status get + clear");
}

static uint32_t            s_cb_count;
static uint32_t            s_cb_last_mask;
static void*               s_cb_last_ctx;
static ra_mipi_dsi_event_t s_cb_last_event;

static void stub_dsi_cb(void* ctx, ra_mipi_dsi_event_t event, uint32_t status_mask)
{
  ++s_cb_count;
  s_cb_last_event = event;
  s_cb_last_mask  = status_mask;
  s_cb_last_ctx   = ctx;
}

static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("mipi_dsi attach + dispatch");
  prep();

  const ra_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_init(&cfg));

  s_cb_count            = 0U;
  s_cb_last_mask        = 0U;
  s_cb_last_ctx         = nullptr;
  s_cb_last_event       = k_ra_mipi_dsi_event_phy;
  void* const ctx_token = (void*)(uintptr_t)0xDEADBEEFUL;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_attach_handler(stub_dsi_cb, ctx_token));

  volatile r_mipi_dsi_regs_t* reg = ra_mipi_dsi();
  reg->ISR                        = (uint32_t)k_ra_mipi_dsi_isr_sq0;
  reg->SQCH0SR                    = (uint32_t)k_ra_mipi_dsi_sqch_aactfin;
  ra_mipi_dsi_dispatch();
  TEST_ASSERT_EQ((int)1, (int)s_cb_count);
  TEST_ASSERT_EQ((int)k_ra_mipi_dsi_event_seq0, (int)s_cb_last_event);
  TEST_ASSERT(s_cb_last_ctx == ctx_token);

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_attach_handler(nullptr, nullptr));
  TEST_END("mipi_dsi attach + dispatch");
}

static void test_dispatch_per_class(void)
{
  TEST_BEGIN("mipi_dsi per-class dispatch");
  prep();

  const ra_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_init(&cfg));

  s_cb_count = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_attach_handler(stub_dsi_cb, nullptr));

  volatile r_mipi_dsi_regs_t* reg = ra_mipi_dsi();

  reg->SQCH1SR = (uint32_t)k_ra_mipi_dsi_sqch_aactfin;
  ra_mipi_dsi_dispatch_seq1();
  TEST_ASSERT_EQ((int)k_ra_mipi_dsi_event_seq1, (int)s_cb_last_event);

  reg->VMSR = (uint32_t)k_ra_mipi_dsi_vmsr_virdy;
  ra_mipi_dsi_dispatch_video();
  TEST_ASSERT_EQ((int)k_ra_mipi_dsi_event_video, (int)s_cb_last_event);

  reg->RXSR = (uint32_t)k_ra_mipi_dsi_rxsr_btarend;
  ra_mipi_dsi_dispatch_receive();
  TEST_ASSERT_EQ((int)k_ra_mipi_dsi_event_receive, (int)s_cb_last_event);

  reg->FERRSR = (uint32_t)k_ra_mipi_dsi_ferrsr_htxto;
  ra_mipi_dsi_dispatch_fatal();
  TEST_ASSERT_EQ((int)k_ra_mipi_dsi_event_fatal, (int)s_cb_last_event);

  reg->PLSR = (uint32_t)k_ra_mipi_dsi_plsr_clulpent;
  ra_mipi_dsi_dispatch_phy();
  TEST_ASSERT_EQ((int)k_ra_mipi_dsi_event_phy, (int)s_cb_last_event);
  TEST_END("mipi_dsi per-class dispatch");
}

static void test_send_short_packet_happy(void)
{
  TEST_BEGIN("mipi_dsi send short packet happy");
  prep();

  const ra_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_init(&cfg));

  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_mipi_dsi_send_short_packet(k_ra_mipi_dsi_dt_dcs_short_write_1,
                                                    k_ra_mipi_dsi_vc0,
                                                    (uint8_t)k_test_dcs_soft_reset,
                                                    (uint8_t)k_test_param1));

  volatile r_mipi_dsi_regs_t* reg   = ra_mipi_dsi();
  const uint32_t              dsc_a = reg->SQCH0DSC[0].A;
  TEST_ASSERT_EQ((int)k_test_dcs_soft_reset, (int)(dsc_a & 0xFFU));
  TEST_ASSERT_EQ((int)k_test_param1, (int)((dsc_a >> 8) & 0xFFU));
  TEST_ASSERT_EQ((int)k_ra_mipi_dsi_dt_dcs_short_write_1, (int)((dsc_a >> 16) & 0x3FU));
  TEST_ASSERT(((dsc_a >> 25) & 1U) == 1U); /* SPD = LP escape */
  /* SQCH0SET0R contains CHSEL + START since channel = 0. */
  TEST_ASSERT_EQ((int)((uint32_t)k_ra_mipi_dsi_sqch_chsel | (uint32_t)k_ra_mipi_dsi_sqch_start),
                 (int)reg->SQCH0SET0R);
  /* SQCH1SET0R receives just CHSEL (no START since channel != 1). */
  TEST_ASSERT_EQ((int)((uint32_t)k_ra_mipi_dsi_sqch_chsel), (int)reg->SQCH1SET0R);

  TEST_END("mipi_dsi send short packet happy");
}

static void test_send_short_packet_bad_vc(void)
{
  TEST_BEGIN("mipi_dsi send short packet bad vc");
  prep();

  const ra_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_init(&cfg));

  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg,
                 (int)ra_mipi_dsi_send_short_packet(k_ra_mipi_dsi_dt_gen_short_write_0,
                                                    (ra_mipi_dsi_vc_t)k_test_bad_vc,
                                                    (uint8_t)k_test_param0,
                                                    (uint8_t)k_test_param1));
  TEST_END("mipi_dsi send short packet bad vc");
}

static void test_send_short_packet_busy(void)
{
  TEST_BEGIN("mipi_dsi send short packet busy");
  prep();

  const ra_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_init(&cfg));

  volatile r_mipi_dsi_regs_t* reg = ra_mipi_dsi();
  reg->LINKSR                     = (uint32_t)k_ra_mipi_dsi_link_sq0run;

  TEST_ASSERT_EQ((int)k_ra_err_busy,
                 (int)ra_mipi_dsi_send_short_packet(k_ra_mipi_dsi_dt_dcs_short_write_0,
                                                    k_ra_mipi_dsi_vc0,
                                                    0U,
                                                    0U));
  TEST_END("mipi_dsi send short packet busy");
}

static void test_send_long_packet(void)
{
  TEST_BEGIN("mipi_dsi send long packet (LP)");
  prep();

  const ra_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_init(&cfg));

  uint8_t payload[k_test_long_len];
  for (uint32_t i = 0U; i < k_test_long_len; ++i) {
    payload[i] = (uint8_t)(0xA0U + i);
  }
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_mipi_dsi_send_long_packet(k_ra_mipi_dsi_dt_dcs_long_write,
                                                   k_ra_mipi_dsi_vc0,
                                                   payload,
                                                   (uint16_t)k_test_long_len,
                                                   true));
  volatile r_mipi_dsi_regs_t* reg   = ra_mipi_dsi();
  const uint32_t              dsc_a = reg->SQCH0DSC[0].A;
  /* DATA0 / DATA1 must encode the word count. */
  TEST_ASSERT_EQ((int)k_test_long_len, (int)(dsc_a & 0xFFU));
  TEST_ASSERT_EQ((int)0, (int)((dsc_a >> 8) & 0xFFU));
  /* FMT bit set. */
  TEST_ASSERT(((dsc_a >> 24) & 1U) == 1U);
  /* TXPPD0R has the first 4 payload bytes. */
  TEST_ASSERT_EQ((int)0xA3A2A1A0UL, (int)reg->TXPPD0R);
  TEST_END("mipi_dsi send long packet (LP)");
}

static void test_send_long_packet_lp_too_big(void)
{
  TEST_BEGIN("mipi_dsi send long packet too big for LP");
  prep();

  const ra_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_init(&cfg));

  static uint8_t big[k_test_huge_len];
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg,
                 (int)ra_mipi_dsi_send_long_packet(k_ra_mipi_dsi_dt_gen_long_write,
                                                   k_ra_mipi_dsi_vc0,
                                                   big,
                                                   (uint16_t)k_test_huge_len,
                                                   true));
  TEST_END("mipi_dsi send long packet too big for LP");
}

static void test_send_long_packet_null_data(void)
{
  TEST_BEGIN("mipi_dsi send long packet with null data + nonzero len");
  prep();

  const ra_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_init(&cfg));

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_mipi_dsi_send_long_packet(k_ra_mipi_dsi_dt_gen_long_write,
                                                   k_ra_mipi_dsi_vc0,
                                                   nullptr,
                                                   (uint16_t)k_test_long_len,
                                                   true));
  TEST_END("mipi_dsi send long packet with null data + nonzero len");
}

static void test_send_long_packet_hs(void)
{
  TEST_BEGIN("mipi_dsi send long packet HS uses ch1");
  prep();

  const ra_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_init(&cfg));

  uint8_t payload[k_test_long_len];
  for (uint32_t i = 0U; i < k_test_long_len; ++i) {
    payload[i] = (uint8_t)(0x10U + i);
  }
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_mipi_dsi_send_long_packet(k_ra_mipi_dsi_dt_gen_long_write,
                                                   k_ra_mipi_dsi_vc0,
                                                   payload,
                                                   (uint16_t)k_test_long_len,
                                                   false));
  volatile r_mipi_dsi_regs_t* reg = ra_mipi_dsi();
  /* SPD bit (LP) must be 0 for HS. */
  TEST_ASSERT(((reg->SQCH1DSC[0].A >> 25) & 1U) == 0U);
  /* SQCH1SET0R contains CHSEL + START since channel = 1. */
  TEST_ASSERT_EQ((int)((uint32_t)k_ra_mipi_dsi_sqch_chsel | (uint32_t)k_ra_mipi_dsi_sqch_start),
                 (int)reg->SQCH1SET0R);
  TEST_END("mipi_dsi send long packet HS uses ch1");
}

static void test_send_command_aux_op(void)
{
  TEST_BEGIN("mipi_dsi send command aux op (skew cal)");
  prep();

  const ra_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_init(&cfg));

  const ra_mipi_dsi_command_t cmd = {
    .cmd_id          = k_ra_mipi_dsi_dt_gen_short_write_0,
    .virtual_channel = k_ra_mipi_dsi_vc0,
    .bta             = k_ra_mipi_dsi_bta_none,
    .low_power       = false,
    .ack_request     = false,
    .aux_operation   = true,
    .action_code     = (uint8_t)k_test_action_code,
    .tx_len          = 0U,
    .p_tx_buffer     = nullptr,
    .p_rx_buffer     = nullptr,
  };
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_send_command(&cmd));
  volatile r_mipi_dsi_regs_t* reg = ra_mipi_dsi();
  /* Word C should carry FINACT + AUXOP + ACTCODE. */
  TEST_ASSERT(((reg->SQCH1DSC[0].C & (uint32_t)k_ra_mipi_dsi_dsc0c_auxop) != 0U));
  TEST_ASSERT_EQ(
    (int)k_test_action_code,
    (int)((reg->SQCH1DSC[0].C >> (uint32_t)k_ra_mipi_dsi_dsc0c_actcode_shift) & 0xFFU));
  TEST_END("mipi_dsi send command aux op (skew cal)");
}

static void test_send_command_video_running_blocks_lp(void)
{
  TEST_BEGIN("mipi_dsi LP send rejected during video mode");
  prep();

  const ra_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_init(&cfg));

  volatile r_mipi_dsi_regs_t* reg = ra_mipi_dsi();
  reg->LINKSR                     = (uint32_t)k_ra_mipi_dsi_link_vrun;

  TEST_ASSERT_EQ((int)k_ra_err_invalid_state,
                 (int)ra_mipi_dsi_send_short_packet(k_ra_mipi_dsi_dt_dcs_short_write_0,
                                                    k_ra_mipi_dsi_vc0,
                                                    0U,
                                                    0U));
  TEST_END("mipi_dsi LP send rejected during video mode");
}

static void test_read_packet(void)
{
  TEST_BEGIN("mipi_dsi read packet (BTA-then-read)");
  prep();

  const ra_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_init(&cfg));

  uint8_t rx[k_ra_mipi_dsi_payload_max];
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_mipi_dsi_read_packet(k_ra_mipi_dsi_dt_dcs_read,
                                              k_ra_mipi_dsi_vc0,
                                              0xDAU,
                                              0U,
                                              rx,
                                              (uint16_t)k_ra_mipi_dsi_payload_max));
  volatile r_mipi_dsi_regs_t* reg = ra_mipi_dsi();
  /* BTA = bta_read should be encoded into bits 27:26. */
  TEST_ASSERT_EQ((int)k_ra_mipi_dsi_bta_read,
                 (int)((reg->SQCH0DSC[0].A >> (uint32_t)k_ra_mipi_dsi_dsc0a_shift_bta) & 0x3U));
  /* Word D should point at the rx buffer. */
  TEST_ASSERT_EQ((int)((uintptr_t)rx & 0xFFFFFFFFUL), (int)reg->SQCH0DSC[0].D);

  /* Bad arg paths. */
  TEST_ASSERT_EQ(
    (int)k_ra_err_null_ptr,
    (int)
      ra_mipi_dsi_read_packet(k_ra_mipi_dsi_dt_dcs_read, k_ra_mipi_dsi_vc0, 0U, 0U, nullptr, 4U));
  TEST_ASSERT_EQ(
    (int)k_ra_err_invalid_arg,
    (int)ra_mipi_dsi_read_packet(k_ra_mipi_dsi_dt_dcs_read, k_ra_mipi_dsi_vc0, 0U, 0U, rx, 0U));
  TEST_END("mipi_dsi read packet (BTA-then-read)");
}

static void test_ulps_enter_exit(void)
{
  TEST_BEGIN("mipi_dsi ULPS enter + exit");
  prep();

  /* Use non-continuous mode so clock-lane ULPS is allowed. */
  const ra_mipi_dsi_config_t cfg = make_cfg_non_continuous();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_init(&cfg));

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_ulps_enter(k_ra_mipi_dsi_lane_all));
  volatile r_mipi_dsi_regs_t* reg = ra_mipi_dsi();
  TEST_ASSERT(((reg->ULPSCR & (uint32_t)k_ra_mipi_dsi_ulpscr_dlent) != 0U));
  TEST_ASSERT(((reg->ULPSCR & (uint32_t)k_ra_mipi_dsi_ulpscr_clent) != 0U));

  /* Re-entering shouldn't pulse again -- driver tracks state. */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_ulps_enter(k_ra_mipi_dsi_lane_all));

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_ulps_exit(k_ra_mipi_dsi_lane_all));
  TEST_ASSERT(((reg->ULPSCR & (uint32_t)k_ra_mipi_dsi_ulpscr_dlexit) != 0U));
  TEST_ASSERT(((reg->ULPSCR & (uint32_t)k_ra_mipi_dsi_ulpscr_clexit) != 0U));

  /* No-lane variants are rejected. */
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg,
                 (int)ra_mipi_dsi_ulps_enter((uint8_t)k_ra_mipi_dsi_lane_none));
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg,
                 (int)ra_mipi_dsi_ulps_exit((uint8_t)k_ra_mipi_dsi_lane_none));
  TEST_END("mipi_dsi ULPS enter + exit");
}

static void test_ulps_clock_lane_continuous_rejected(void)
{
  TEST_BEGIN("mipi_dsi ULPS clock lane rejected in continuous mode");
  prep();

  const ra_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_init(&cfg));

  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg,
                 (int)ra_mipi_dsi_ulps_enter((uint8_t)k_ra_mipi_dsi_lane_clock));

  /* Data-lane only is fine. */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_ulps_enter((uint8_t)k_ra_mipi_dsi_lane_data));
  TEST_END("mipi_dsi ULPS clock lane rejected in continuous mode");
}

static ra_mipi_dsi_video_cfg_t make_video_cfg(void)
{
  const ra_mipi_dsi_video_cfg_t v = {
    .pixel_format             = k_ra_mipi_dsi_dt_pixel_rgb888,
    .virtual_channel          = k_ra_mipi_dsi_vc0,
    .sync_pulse               = true,
    .hsa_no_lp                = true,
    .hbp_no_lp                = false,
    .hfp_no_lp                = false,
    .vsync_active_high        = true,
    .hsync_active_high        = true,
    .vertical_sync_lines      = (uint16_t)k_test_video_vsa,
    .vertical_active_lines    = (uint16_t)k_test_video_v_act,
    .vertical_back_porch      = (uint16_t)k_test_video_vbp,
    .vertical_front_porch     = (uint16_t)k_test_video_vfp,
    .horizontal_sync_lines    = (uint16_t)k_test_video_hsa,
    .horizontal_active_pixels = (uint16_t)k_test_video_h_act,
    .horizontal_back_porch    = (uint16_t)k_test_video_hbp,
    .horizontal_front_porch   = (uint16_t)k_test_video_hfp,
    .video_mode_delay         = 0x40U,
  };
  return v;
}

static void test_video_configure(void)
{
  TEST_BEGIN("mipi_dsi video configure");
  prep();

  const ra_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_init(&cfg));

  const ra_mipi_dsi_video_cfg_t v = make_video_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_video_configure(&v));

  volatile r_mipi_dsi_regs_t* reg = ra_mipi_dsi();
  /* VMPPSETR encodes pixel format = 0x3E (RGB888) at bits 21:16. */
  TEST_ASSERT_EQ((int)k_ra_mipi_dsi_dt_pixel_rgb888, (int)((reg->VMPPSETR >> 16) & 0x3FU));
  TEST_ASSERT(((reg->VMPPSETR & (uint32_t)k_ra_mipi_dsi_vmpp_txesync) != 0U));
  /* VSA / VACT splits. */
  TEST_ASSERT_EQ((int)k_test_video_vsa, (int)(reg->VMVSSETR & 0xFFFU));
  TEST_ASSERT_EQ((int)k_test_video_v_act, (int)((reg->VMVSSETR >> 16) & 0x7FFFU));

  /* Null & bad-vc rejection. */
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_mipi_dsi_video_configure(nullptr));
  ra_mipi_dsi_video_cfg_t bad = v;
  bad.virtual_channel         = (ra_mipi_dsi_vc_t)k_test_bad_vc;
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_mipi_dsi_video_configure(&bad));
  TEST_END("mipi_dsi video configure");
}

static void test_video_pixel_formats(void)
{
  TEST_BEGIN("mipi_dsi video all pixel formats");
  prep();

  const ra_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_init(&cfg));

  ra_mipi_dsi_video_cfg_t     v   = make_video_cfg();
  volatile r_mipi_dsi_regs_t* reg = ra_mipi_dsi();

  v.pixel_format = k_ra_mipi_dsi_dt_pixel_rgb565;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_video_configure(&v));
  TEST_ASSERT_EQ((int)k_ra_mipi_dsi_dt_pixel_rgb565, (int)((reg->VMPPSETR >> 16) & 0x3FU));

  v.pixel_format = k_ra_mipi_dsi_dt_pixel_rgb666;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_video_configure(&v));
  TEST_ASSERT_EQ((int)k_ra_mipi_dsi_dt_pixel_rgb666, (int)((reg->VMPPSETR >> 16) & 0x3FU));

  v.pixel_format = k_ra_mipi_dsi_dt_pixel_rgb666_loose;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_video_configure(&v));
  TEST_ASSERT_EQ((int)k_ra_mipi_dsi_dt_pixel_rgb666_loose, (int)((reg->VMPPSETR >> 16) & 0x3FU));

  v.pixel_format = k_ra_mipi_dsi_dt_pixel_rgb888;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_video_configure(&v));
  TEST_ASSERT_EQ((int)k_ra_mipi_dsi_dt_pixel_rgb888, (int)((reg->VMPPSETR >> 16) & 0x3FU));
  TEST_END("mipi_dsi video all pixel formats");
}

static void test_video_start_stop(void)
{
  TEST_BEGIN("mipi_dsi video start + stop");
  prep();

  const ra_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_init(&cfg));

  const ra_mipi_dsi_video_cfg_t v = make_video_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_video_configure(&v));

  /* Pre-seed VMSR.VIRDY so the bounded poll inside _video_start
   * succeeds immediately. */
  volatile r_mipi_dsi_regs_t* reg = ra_mipi_dsi();
  reg->VMSR                       = (uint32_t)k_ra_mipi_dsi_vmsr_virdy;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_video_start(&v));
  TEST_ASSERT(((reg->VMSET0R & (uint32_t)k_ra_mipi_dsi_vmset0_vstart) != 0U));
  TEST_ASSERT(((reg->VMSET0R & (uint32_t)k_ra_mipi_dsi_vmset0_hsanolp) != 0U));

  /* Pre-seed VMSR.STOP so _video_stop's poll returns success. */
  reg->VMSR = (uint32_t)k_ra_mipi_dsi_vmsr_stop;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_video_stop());
  TEST_END("mipi_dsi video start + stop");
}

static void test_hs_clock_start_stop(void)
{
  TEST_BEGIN("mipi_dsi HS clock start + stop");
  prep();

  /* Use non-continuous mode so the start poll requires PLSR.CLLP2HS. */
  const ra_mipi_dsi_config_t cfg = make_cfg_non_continuous();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_init(&cfg));

  volatile r_mipi_dsi_regs_t* reg = ra_mipi_dsi();
  reg->PLSR                       = (uint32_t)k_ra_mipi_dsi_plsr_cllp2hs;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_hs_clock_start());
  TEST_ASSERT(((reg->HSCLKSETR & (uint32_t)k_ra_mipi_dsi_hsclk_start) != 0U));

  reg->PLSR = (uint32_t)k_ra_mipi_dsi_plsr_clhs2lp;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_hs_clock_stop());
  TEST_ASSERT(reg->HSCLKSETR == 0U);
  TEST_END("mipi_dsi HS clock start + stop");
}

static void test_link_status_get(void)
{
  TEST_BEGIN("mipi_dsi link status decode");
  prep();

  const ra_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_init(&cfg));

  volatile r_mipi_dsi_regs_t* reg = ra_mipi_dsi();
  reg->LINKSR = (uint32_t)k_ra_mipi_dsi_link_sq0run | (uint32_t)k_ra_mipi_dsi_link_vrun |
                (uint32_t)k_ra_mipi_dsi_link_hsbusy;

  ra_mipi_dsi_link_status_t s = {};
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_link_status_get(&s));
  TEST_ASSERT(s.sequence_ch0_running);
  TEST_ASSERT(!s.sequence_ch1_running);
  TEST_ASSERT(s.video_running);
  TEST_ASSERT(s.hs_busy);
  TEST_ASSERT(!s.lp_busy);

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_mipi_dsi_link_status_get(nullptr));
  TEST_END("mipi_dsi link status decode");
}

static void test_ack_error(void)
{
  TEST_BEGIN("mipi_dsi ack/error get");
  prep();

  const ra_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_init(&cfg));

  volatile r_mipi_dsi_regs_t* reg = ra_mipi_dsi();
  reg->AKEPACMSR                  = 0x0001A55AUL; /* VC=1 in bits 19:16, errors in 15:0 */

  ra_mipi_dsi_ack_error_t e = {};
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_ack_error_get(&e));
  TEST_ASSERT_EQ((int)0xA55A, (int)e.error_report);
  TEST_ASSERT_EQ((int)k_ra_mipi_dsi_vc1, (int)e.virtual_channel);

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_mipi_dsi_ack_error_get(nullptr));
  TEST_END("mipi_dsi ack/error get");
}

static void test_rx_result_get(void)
{
  TEST_BEGIN("mipi_dsi rx result decode");
  prep();

  const ra_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_init(&cfg));

  volatile r_mipi_dsi_regs_t* reg = ra_mipi_dsi();
  /* Build a fake slot-0 capture: data0=0x12, data1=0x34, dt=0x06,
   * vc=0, fmt=0, rxsuc=1. */
  reg->RXRSSR  = (uint32_t)k_ra_mipi_dsi_rxrssr_slt0vld;
  reg->RXRSS0R = 0x12U | (0x34U << 8) | (0x06U << 16) | (uint32_t)k_ra_mipi_dsi_rxrss_rxsuc;

  ra_mipi_dsi_rx_result_t r = {};
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_rx_result_get(0U, &r));
  TEST_ASSERT_EQ((int)0x12, (int)r.data[0]);
  TEST_ASSERT_EQ((int)0x34, (int)r.data[1]);
  TEST_ASSERT_EQ((int)0x06, (int)r.cmd_id);
  TEST_ASSERT(r.rx_success);

  /* Slot not valid -> no_data. */
  reg->RXRSSR = 0U;
  TEST_ASSERT_EQ((int)k_ra_err_no_data, (int)ra_mipi_dsi_rx_result_get(0U, &r));

  /* Bad slot index. */
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_mipi_dsi_rx_result_get(99U, &r));
  /* NULL out arg. */
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_mipi_dsi_rx_result_get(0U, nullptr));
  TEST_END("mipi_dsi rx result decode");
}

static void test_rx_payload_read(void)
{
  TEST_BEGIN("mipi_dsi rx payload read");
  prep();

  const ra_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_init(&cfg));

  volatile r_mipi_dsi_regs_t* reg = ra_mipi_dsi();
  reg->RXPPD0R                    = (uint32_t)k_test_rx_payload_w0;
  reg->RXPPD1R                    = (uint32_t)k_test_rx_payload_w1;
  reg->RXPPD2R                    = (uint32_t)k_test_rx_payload_w2;
  reg->RXPPD3R                    = (uint32_t)k_test_rx_payload_w3;

  uint8_t  dst[k_ra_mipi_dsi_payload_max] = {};
  uint16_t got                            = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_mipi_dsi_rx_payload_read(dst, (uint16_t)k_ra_mipi_dsi_payload_max, &got));
  TEST_ASSERT_EQ((int)k_ra_mipi_dsi_payload_max, (int)got);
  TEST_ASSERT_EQ((int)0xEF, (int)dst[0]);
  TEST_ASSERT_EQ((int)0xBE, (int)dst[1]);

  /* Null-arg paths. */
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_mipi_dsi_rx_payload_read(nullptr, 4U, &got));
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_mipi_dsi_rx_payload_read(dst, 4U, nullptr));
  TEST_END("mipi_dsi rx payload read");
}

static void test_te_event(void)
{
  TEST_BEGIN("mipi_dsi tearing-effect event");
  prep();

  const ra_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_init(&cfg));

  volatile r_mipi_dsi_regs_t* reg = ra_mipi_dsi();
  reg->RXSR                       = (uint32_t)k_ra_mipi_dsi_rxsr_rxte;

  bool pending = false;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_te_event_pending(&pending));
  TEST_ASSERT(pending);

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_te_event_clear());
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_mipi_dsi_te_event_pending(nullptr));
  TEST_END("mipi_dsi tearing-effect event");
}

static void test_irq_enable(void)
{
  TEST_BEGIN("mipi_dsi irq enable per class");
  prep();

  const ra_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_init(&cfg));

  volatile r_mipi_dsi_regs_t* reg = ra_mipi_dsi();

  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_mipi_dsi_irq_enable(k_ra_mipi_dsi_event_seq0,
                                             (uint32_t)k_ra_mipi_dsi_sqch_aactfin,
                                             true));
  TEST_ASSERT(((reg->SQCH0IER & (uint32_t)k_ra_mipi_dsi_sqch_aactfin) != 0U));

  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_mipi_dsi_irq_enable(k_ra_mipi_dsi_event_seq0,
                                             (uint32_t)k_ra_mipi_dsi_sqch_aactfin,
                                             false));
  TEST_ASSERT(((reg->SQCH0IER & (uint32_t)k_ra_mipi_dsi_sqch_aactfin) == 0U));

  /* All other classes too. */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_irq_enable(k_ra_mipi_dsi_event_seq1, 1U, true));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_irq_enable(k_ra_mipi_dsi_event_video, 1U, true));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_irq_enable(k_ra_mipi_dsi_event_receive, 1U, true));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_irq_enable(k_ra_mipi_dsi_event_fatal, 1U, true));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_irq_enable(k_ra_mipi_dsi_event_phy, 1U, true));

  /* Bad event class. */
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg,
                 (int)ra_mipi_dsi_irq_enable((ra_mipi_dsi_event_t)99U, 1U, true));
  TEST_END("mipi_dsi irq enable per class");
}

static void test_soft_reset(void)
{
  TEST_BEGIN("mipi_dsi soft reset");
  prep();

  const ra_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_init(&cfg));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_soft_reset());
  TEST_END("mipi_dsi soft reset");
}

static void test_power_transition(void)
{
  TEST_BEGIN("mipi_dsi power transition");
  prep();

  const ra_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_init(&cfg));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_enter_stop());
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_exit_stop());
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_deinit());
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mipi_dsi_deinit());
  TEST_END("mipi_dsi power transition");
}

int32_t main(void)
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
  test_video_configure();
  test_video_pixel_formats();
  test_video_start_stop();
  test_hs_clock_start_stop();
  test_link_status_get();
  test_ack_error();
  test_rx_result_get();
  test_rx_payload_read();
  test_te_event();
  test_irq_enable();
  test_soft_reset();
  test_power_transition();
  (void)fprintf(stderr, "[OK  ] test_ra_mipi_dsi.c\n");
  return 0;
}
