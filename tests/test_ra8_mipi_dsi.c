/**
 * @file test_ra8_mipi_dsi.c
 * @brief Host-side unit tests for the full ra8_mipi_dsi.c surface
 *
 * @details
 * Exercises every public entry point in `ra8_mipi_dsi.h`:
 * lifecycle, HS clock, sequence channels (LP + HS, short, long,
 * read-with-BTA), ULPS enter / exit (with continuous-clock guard),
 * video-mode configure / start / stop, every status / IRQ helper,
 * and per-class dispatch fanout. Backed by `ra8_sim_mmap` so the
 * driver writes against ordinary host RAM.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_mipi_dsi.h"
#include "ra8_mipi_dsi_regs.h"
#include "ra8_mstp.h"
#include "ra8_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum ra8_mipi_dsi_test_const_t
 * @brief Numeric inputs used by the test cases (no magic numbers).
 */
typedef enum : uint32_t {
  k_test_max_return_pkt = 64U,   /**< Test maximum return pkt. */
  k_test_dcs_soft_reset = 0x01U, /**< Test dcs soft reset.     */
  k_test_param0         = 0xAAU, /**< Test param0.             */
  k_test_param1         = 0x55U, /**< Test param1.             */
  k_test_isr_seed =
    (uint32_t)k_ra8_mipi_dsi_isr_sq0 | (uint32_t)k_ra8_mipi_dsi_isr_vm, /**< Test ISR seed.    */
  k_test_bad_vc              = 9U,                                      /**< Test bad vc.      */
  k_test_long_len            = 8U,                                      /**< Test long length. */
  k_test_huge_len            = 200U,                                    /**< > LP cap (128).   */
  k_test_long_payload_first  = 0xDEU,        /**< Test long payload first.  */
  k_test_long_payload_second = 0xADU,        /**< Test long payload second. */
  k_test_video_h_act         = 1024U,        /**< Test video h act.         */
  k_test_video_v_act         = 600U,         /**< Test video v act.         */
  k_test_video_hsa           = 12U,          /**< Test video hsa.           */
  k_test_video_hbp           = 64U,          /**< Test video hbp.           */
  k_test_video_hfp           = 32U,          /**< Test video hfp.           */
  k_test_video_vsa           = 4U,           /**< Test video vsa.           */
  k_test_video_vbp           = 8U,           /**< Test video vbp.           */
  k_test_video_vfp           = 6U,           /**< Test video vfp.           */
  k_test_ulps_wkup           = 0x40U,        /**< Test ulps wkup.           */
  k_test_action_code         = 0x24U,        /**< initial skew calibration. */
  k_test_rx_payload_w0       = 0xDEADBEEFUL, /**< Test RX payload w0.       */
  k_test_rx_payload_w1       = 0xCAFEBABEUL, /**< Test RX payload w1.       */
  k_test_rx_payload_w2       = 0x12345678UL, /**< Test RX payload w2.       */
  k_test_rx_payload_w3       = 0x90ABCDEFUL, /**< Test RX payload w3.       */
} ra8_mipi_dsi_test_const_t;

/**
 * @brief Reset the simulated peripheral memory + MSTP table.
 */
static void prep(void)
{
  ra8_sim_mmap_reset();
  (void)ra8_mstp_init();
}

/**
 * @brief Build a "happy path" config struct.
 */
static ra8_mipi_dsi_config_t make_cfg(void)
{
  const ra8_mipi_dsi_config_t cfg = {
    .lane_count             = k_ra8_mipi_dsi_lanes_2,
    .clock_mode             = k_ra8_mipi_dsi_clock_continuous,
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
static ra8_mipi_dsi_config_t make_cfg_non_continuous(void)
{
  ra8_mipi_dsi_config_t cfg = make_cfg();
  cfg.clock_mode            = k_ra8_mipi_dsi_clock_non_continuous;
  return cfg;
}

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

  cfg.lane_count = (ra8_mipi_dsi_lane_count_t)5U;
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
  reg->SQCH0SCR                   = 0xFFU;
  reg->VMSCR                      = 0xFFU;

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
    payload[i] = (uint8_t)(0xA0U + i);
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

  static uint8_t big[k_test_huge_len];
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_mipi_dsi_send_long_packet(k_ra8_mipi_dsi_dt_gen_long_write,
                                               k_ra8_mipi_dsi_vc0,
                                               big,
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

static ra8_mipi_dsi_video_cfg_t make_video_cfg(void)
{
  const ra8_mipi_dsi_video_cfg_t v = {
    .pixel_format             = k_ra8_mipi_dsi_dt_pixel_rgb888,
    .virtual_channel          = k_ra8_mipi_dsi_vc0,
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

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_video_configure(void)
{
  TEST_BEGIN("mipi_dsi video configure");
  prep();

  const ra8_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));

  const ra8_mipi_dsi_video_cfg_t v = make_video_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_video_configure(&v));

  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  /* VMPPSETR encodes pixel format = 0x3E (RGB888) at bits 21:16. */
  TEST_ASSERT_EQ(k_ra8_mipi_dsi_dt_pixel_rgb888, ((reg->VMPPSETR >> 16) & 0x3FU));
  TEST_ASSERT(((reg->VMPPSETR & (uint32_t)k_ra8_mipi_dsi_vmpp_txesync) != 0U));
  /* VSA / VACT splits. */
  TEST_ASSERT_EQ(k_test_video_vsa, (reg->VMVSSETR & 0xFFFU));
  TEST_ASSERT_EQ(k_test_video_v_act, ((reg->VMVSSETR >> 16) & 0x7FFFU));

  /* Null & bad-vc rejection. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mipi_dsi_video_configure(nullptr));
  ra8_mipi_dsi_video_cfg_t bad = v;
  bad.virtual_channel          = (ra8_mipi_dsi_vc_t)k_test_bad_vc;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_dsi_video_configure(&bad));
  TEST_END("mipi_dsi video configure");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_video_pixel_formats(void)
{
  TEST_BEGIN("mipi_dsi video all pixel formats");
  prep();

  const ra8_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));

  ra8_mipi_dsi_video_cfg_t    v   = make_video_cfg();
  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();

  v.pixel_format = k_ra8_mipi_dsi_dt_pixel_rgb565;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_video_configure(&v));
  TEST_ASSERT_EQ(k_ra8_mipi_dsi_dt_pixel_rgb565, ((reg->VMPPSETR >> 16) & 0x3FU));

  v.pixel_format = k_ra8_mipi_dsi_dt_pixel_rgb666;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_video_configure(&v));
  TEST_ASSERT_EQ(k_ra8_mipi_dsi_dt_pixel_rgb666, ((reg->VMPPSETR >> 16) & 0x3FU));

  v.pixel_format = k_ra8_mipi_dsi_dt_pixel_rgb666_loose;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_video_configure(&v));
  TEST_ASSERT_EQ(k_ra8_mipi_dsi_dt_pixel_rgb666_loose, ((reg->VMPPSETR >> 16) & 0x3FU));

  v.pixel_format = k_ra8_mipi_dsi_dt_pixel_rgb888;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_video_configure(&v));
  TEST_ASSERT_EQ(k_ra8_mipi_dsi_dt_pixel_rgb888, ((reg->VMPPSETR >> 16) & 0x3FU));
  TEST_END("mipi_dsi video all pixel formats");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_video_start_stop(void)
{
  TEST_BEGIN("mipi_dsi video start + stop");
  prep();

  const ra8_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));

  const ra8_mipi_dsi_video_cfg_t v = make_video_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_video_configure(&v));

  /* Pre-seed VMSR.VIRDY so the bounded poll inside _video_start
   * succeeds immediately. */
  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  reg->VMSR                       = (uint32_t)k_ra8_mipi_dsi_vmsr_virdy;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_video_start(&v));
  TEST_ASSERT(((reg->VMSET0R & (uint32_t)k_ra8_mipi_dsi_vmset0_vstart) != 0U));
  TEST_ASSERT(((reg->VMSET0R & (uint32_t)k_ra8_mipi_dsi_vmset0_hsanolp) != 0U));

  /* Pre-seed VMSR.STOP so _video_stop's poll returns success. */
  reg->VMSR = (uint32_t)k_ra8_mipi_dsi_vmsr_stop;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_video_stop());
  TEST_END("mipi_dsi video start + stop");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_hs_clock_start_stop(void)
{
  TEST_BEGIN("mipi_dsi HS clock start + stop");
  prep();

  /* Use non-continuous mode so the start poll requires PLSR.CLLP2HS. */
  const ra8_mipi_dsi_config_t cfg = make_cfg_non_continuous();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));

  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  reg->PLSR                       = (uint32_t)k_ra8_mipi_dsi_plsr_cllp2hs;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_hs_clock_start());
  TEST_ASSERT(((reg->HSCLKSETR & (uint32_t)k_ra8_mipi_dsi_hsclk_start) != 0U));

  reg->PLSR = (uint32_t)k_ra8_mipi_dsi_plsr_clhs2lp;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_hs_clock_stop());
  TEST_ASSERT(reg->HSCLKSETR == 0U);
  TEST_END("mipi_dsi HS clock start + stop");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_link_status_get(void)
{
  TEST_BEGIN("mipi_dsi link status decode");
  prep();

  const ra8_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));

  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  reg->LINKSR = (uint32_t)k_ra8_mipi_dsi_link_sq0run | (uint32_t)k_ra8_mipi_dsi_link_vrun |
                (uint32_t)k_ra8_mipi_dsi_link_hsbusy;

  ra8_mipi_dsi_link_status_t s = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_link_status_get(&s));
  TEST_ASSERT(s.sequence_ch0_running);
  TEST_ASSERT(!s.sequence_ch1_running);
  TEST_ASSERT(s.video_running);
  TEST_ASSERT(s.hs_busy);
  TEST_ASSERT(!s.lp_busy);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mipi_dsi_link_status_get(nullptr));
  TEST_END("mipi_dsi link status decode");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_ack_error(void)
{
  TEST_BEGIN("mipi_dsi ack/error get");
  prep();

  const ra8_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));

  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  reg->AKEPACMSR                  = 0x0001A55AUL; /* VC=1 in bits 19:16, errors in 15:0 */

  ra8_mipi_dsi_ack_error_t e = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_ack_error_get(&e));
  TEST_ASSERT_EQ(0xA55A, e.error_report);
  TEST_ASSERT_EQ(k_ra8_mipi_dsi_vc1, e.virtual_channel);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mipi_dsi_ack_error_get(nullptr));
  TEST_END("mipi_dsi ack/error get");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_rx_result_get(void)
{
  TEST_BEGIN("mipi_dsi rx result decode");
  prep();

  const ra8_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));

  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  /* Build a fake slot-0 capture: data0=0x12, data1=0x34, dt=0x06,
   * vc=0, fmt=0, rxsuc=1. */
  reg->RXRSSR  = (uint32_t)k_ra8_mipi_dsi_rxrssr_slt0vld;
  reg->RXRSS0R = 0x12U | (0x34U << 8) | (0x06U << 16) | (uint32_t)k_ra8_mipi_dsi_rxrss_rxsuc;

  ra8_mipi_dsi_rx_result_t r = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_rx_result_get(0U, &r));
  TEST_ASSERT_EQ(0x12, r.data[0]);
  TEST_ASSERT_EQ(0x34, r.data[1]);
  TEST_ASSERT_EQ(0x06, r.cmd_id);
  TEST_ASSERT(r.rx_success);

  /* Slot not valid -> no_data. */
  reg->RXRSSR = 0U;
  TEST_ASSERT_EQ(k_ra8_err_no_data, ra8_mipi_dsi_rx_result_get(0U, &r));

  /* Bad slot index. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_dsi_rx_result_get(99U, &r));
  /* NULL out arg. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mipi_dsi_rx_result_get(0U, nullptr));
  TEST_END("mipi_dsi rx result decode");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_rx_payload_read(void)
{
  TEST_BEGIN("mipi_dsi rx payload read");
  prep();

  const ra8_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));

  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  reg->RXPPD0R                    = (uint32_t)k_test_rx_payload_w0;
  reg->RXPPD1R                    = (uint32_t)k_test_rx_payload_w1;
  reg->RXPPD2R                    = (uint32_t)k_test_rx_payload_w2;
  reg->RXPPD3R                    = (uint32_t)k_test_rx_payload_w3;

  uint8_t  dst[k_ra8_mipi_dsi_payload_max] = {};
  uint16_t got                             = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_dsi_rx_payload_read(dst, (uint16_t)k_ra8_mipi_dsi_payload_max, &got));
  TEST_ASSERT_EQ(k_ra8_mipi_dsi_payload_max, got);
  TEST_ASSERT_EQ(0xEF, dst[0]);
  TEST_ASSERT_EQ(0xBE, dst[1]);

  /* Null-arg paths. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mipi_dsi_rx_payload_read(nullptr, 4U, &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mipi_dsi_rx_payload_read(dst, 4U, nullptr));
  TEST_END("mipi_dsi rx payload read");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_te_event(void)
{
  TEST_BEGIN("mipi_dsi tearing-effect event");
  prep();

  const ra8_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));

  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  reg->RXSR                       = (uint32_t)k_ra8_mipi_dsi_rxsr_rxte;

  bool pending = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_te_event_pending(&pending));
  TEST_ASSERT(pending);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_te_event_clear());
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mipi_dsi_te_event_pending(nullptr));
  TEST_END("mipi_dsi tearing-effect event");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_irq_enable(void)
{
  TEST_BEGIN("mipi_dsi irq enable per class");
  prep();

  const ra8_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));

  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_dsi_irq_enable(k_ra8_mipi_dsi_event_seq0,
                                         (uint32_t)k_ra8_mipi_dsi_sqch_aactfin,
                                         true));
  TEST_ASSERT(((reg->SQCH0IER & (uint32_t)k_ra8_mipi_dsi_sqch_aactfin) != 0U));

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_dsi_irq_enable(k_ra8_mipi_dsi_event_seq0,
                                         (uint32_t)k_ra8_mipi_dsi_sqch_aactfin,
                                         false));
  TEST_ASSERT(((reg->SQCH0IER & (uint32_t)k_ra8_mipi_dsi_sqch_aactfin) == 0U));

  /* All other classes too. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_irq_enable(k_ra8_mipi_dsi_event_seq1, 1U, true));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_irq_enable(k_ra8_mipi_dsi_event_video, 1U, true));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_irq_enable(k_ra8_mipi_dsi_event_receive, 1U, true));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_irq_enable(k_ra8_mipi_dsi_event_fatal, 1U, true));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_irq_enable(k_ra8_mipi_dsi_event_phy, 1U, true));

  /* Bad event class. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_mipi_dsi_irq_enable((ra8_mipi_dsi_event_t)99U, 1U, true));
  TEST_END("mipi_dsi irq enable per class");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_soft_reset(void)
{
  TEST_BEGIN("mipi_dsi soft reset");
  prep();

  const ra8_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_soft_reset());
  TEST_END("mipi_dsi soft reset");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_power_transition(void)
{
  TEST_BEGIN("mipi_dsi power transition");
  prep();

  const ra8_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_enter_stop());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_exit_stop());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_deinit());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_deinit());
  TEST_END("mipi_dsi power transition");
}

/* ===========================================================================
 * Sweep 6: video timing, command-mode short/long, ULPS shorthand,
 * link-status snapshot. NULL-arg coverage included.
 * ===========================================================================
 */

static ra8_mipi_dsi_video_timing_t make_timing(void)
{
  const ra8_mipi_dsi_video_timing_t t = {
    .horizontal_sync        = (uint16_t)k_test_video_hsa,
    .horizontal_back_porch  = (uint16_t)k_test_video_hbp,
    .horizontal_active      = (uint16_t)k_test_video_h_act,
    .horizontal_front_porch = (uint16_t)k_test_video_hfp,
    .vertical_sync          = (uint16_t)k_test_video_vsa,
    .vertical_back_porch    = (uint16_t)k_test_video_vbp,
    .vertical_active        = (uint16_t)k_test_video_v_act,
    .vertical_front_porch   = (uint16_t)k_test_video_vfp,
  };
  return t;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_video_timing(void)
{
  TEST_BEGIN("mipi_dsi set_video_timing programmes VM* timing block");
  prep();

  const ra8_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));

  const ra8_mipi_dsi_video_timing_t t = make_timing();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_set_video_timing(&t));

  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  TEST_ASSERT_EQ(k_test_video_vsa, (reg->VMVSSETR & 0xFFFU));
  TEST_ASSERT_EQ(k_test_video_v_act, ((reg->VMVSSETR >> 16) & 0x7FFFU));
  TEST_ASSERT_EQ(k_test_video_vbp, (reg->VMVPSETR & 0x1FFFU));
  TEST_ASSERT_EQ(k_test_video_vfp, ((reg->VMVPSETR >> 16) & 0x1FFFU));
  TEST_ASSERT_EQ(k_test_video_hsa, (reg->VMHSSETR & 0xFFFU));
  TEST_ASSERT_EQ(k_test_video_h_act, ((reg->VMHSSETR >> 16) & 0x7FFFU));
  TEST_ASSERT_EQ(k_test_video_hbp, (reg->VMHPSETR & 0x1FFFU));
  TEST_ASSERT_EQ(k_test_video_hfp, ((reg->VMHPSETR >> 16) & 0x1FFFU));

  /* Default-fill: pixel format is RGB888 on VC0. */
  TEST_ASSERT_EQ(k_ra8_mipi_dsi_dt_pixel_rgb888, ((reg->VMPPSETR >> 16) & 0x3FU));

  TEST_END("mipi_dsi set_video_timing programmes VM* timing block");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_video_timing_null(void)
{
  TEST_BEGIN("mipi_dsi set_video_timing rejects nullptr");
  prep();
  const ra8_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mipi_dsi_set_video_timing(nullptr));

  /* Field overflow rejected. */
  ra8_mipi_dsi_video_timing_t bad = make_timing();
  bad.horizontal_active           = (uint16_t)0xFFFFU; /* > 15 bits. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_dsi_set_video_timing(&bad));
  TEST_END("mipi_dsi set_video_timing rejects nullptr");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_send_command_short(void)
{
  TEST_BEGIN("mipi_dsi send_command_short stages on VC0/LP");
  prep();
  const ra8_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));

  const uint8_t params[2] = {(uint8_t)k_test_param0, (uint8_t)k_test_param1};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_dsi_send_command_short(k_ra8_mipi_dsi_dt_dcs_short_write_1, params));

  /* Sequence channel 0 (LP) is exercised -- SQCH0SET0R START asserted. */
  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  TEST_ASSERT(((reg->SQCH0SET0R & (uint32_t)k_ra8_mipi_dsi_sqch_start) != 0U));

  /* NULL-arg rejection. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_mipi_dsi_send_command_short(k_ra8_mipi_dsi_dt_dcs_short_write_1, nullptr));
  TEST_END("mipi_dsi send_command_short stages on VC0/LP");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_send_command_long(void)
{
  TEST_BEGIN("mipi_dsi send_command_long stages on VC0/HS");
  prep();
  const ra8_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));

  const uint8_t payload[4] = {0x01U, 0x02U, 0x03U, 0x04U};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_dsi_send_command_long(k_ra8_mipi_dsi_dt_dcs_long_write,
                                                payload,
                                                (uint16_t)sizeof(payload)));

  /* Sequence channel 1 (HS) carries the long packet. */
  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  TEST_ASSERT(((reg->SQCH1SET0R & (uint32_t)k_ra8_mipi_dsi_sqch_start) != 0U));

  /* len > 0 with NULL payload rejected. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_mipi_dsi_send_command_long(k_ra8_mipi_dsi_dt_dcs_long_write, nullptr, 1U));

  /* len == 0 with NULL payload is OK (zero-length). */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_dsi_send_command_long(k_ra8_mipi_dsi_dt_null_packet, nullptr, 0U));
  TEST_END("mipi_dsi send_command_long stages on VC0/HS");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_enter_exit_ulps(void)
{
  TEST_BEGIN("mipi_dsi enter_ulps / exit_ulps cover both lanes");
  prep();
  /* Non-continuous so clock-lane ULPS is permitted. */
  const ra8_mipi_dsi_config_t cfg = make_cfg_non_continuous();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_enter_ulps());
  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  TEST_ASSERT(((reg->ULPSCR & (uint32_t)k_ra8_mipi_dsi_ulpscr_dlent) != 0U));
  TEST_ASSERT(((reg->ULPSCR & (uint32_t)k_ra8_mipi_dsi_ulpscr_clent) != 0U));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_exit_ulps());
  TEST_ASSERT(((reg->ULPSCR & (uint32_t)k_ra8_mipi_dsi_ulpscr_dlexit) != 0U));
  TEST_ASSERT(((reg->ULPSCR & (uint32_t)k_ra8_mipi_dsi_ulpscr_clexit) != 0U));
  TEST_END("mipi_dsi enter_ulps / exit_ulps cover both lanes");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_enter_ulps_continuous_rejected(void)
{
  TEST_BEGIN("mipi_dsi enter_ulps blocked under continuous-clock mode");
  prep();
  const ra8_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));
  /* lane_all includes the clock lane -> rejected in continuous mode. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_dsi_enter_ulps());
  TEST_END("mipi_dsi enter_ulps blocked under continuous-clock mode");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_link_status(void)
{
  TEST_BEGIN("mipi_dsi get_link_status decodes LINKSR");
  prep();
  const ra8_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));

  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  reg->LINKSR = (uint32_t)k_ra8_mipi_dsi_link_vrun | (uint32_t)k_ra8_mipi_dsi_link_hsbusy;

  ra8_mipi_dsi_link_status_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_get_link_status(&st));
  TEST_ASSERT(st.video_running);
  TEST_ASSERT(st.hs_busy);
  TEST_ASSERT(!st.lp_busy);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mipi_dsi_get_link_status(nullptr));
  TEST_END("mipi_dsi get_link_status decodes LINKSR");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_video_timing_overflow_each_field(void)
{
  TEST_BEGIN("mipi_dsi set_video_timing rejects every field overflow");
  prep();
  const ra8_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));

  ra8_mipi_dsi_video_timing_t t = make_timing();
  t.vertical_sync               = (uint16_t)0xFFFFU;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_dsi_set_video_timing(&t));

  t                       = make_timing();
  t.horizontal_back_porch = (uint16_t)0xFFFFU;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_dsi_set_video_timing(&t));

  t                 = make_timing();
  t.vertical_active = (uint16_t)0xFFFFU;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_dsi_set_video_timing(&t));
  TEST_END("mipi_dsi set_video_timing rejects every field overflow");
}

/* ---------------------------------------------------------------------------
 * Sweep 15 / Phase 2: command-mode payload + LP-00 ULPS verification.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 * --------------------------------------------------------------------------- */

static void test_send_command_payload_short(void)
{
  TEST_BEGIN("mipi_dsi send_command_payload routes <=2 bytes via short path");
  prep();
  const ra8_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));

  const uint8_t two[2] = {0xAAU, 0xBBU};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_dsi_send_command_payload(k_ra8_mipi_dsi_dt_dcs_short_write_1,
                                                   two,
                                                   (uint16_t)sizeof(two)));
  /* Sequence channel 0 (LP) carries the short packet. */
  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  TEST_ASSERT(((reg->SQCH0SET0R & (uint32_t)k_ra8_mipi_dsi_sqch_start) != 0U));
  TEST_END("mipi_dsi send_command_payload routes <=2 bytes via short path");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_send_command_payload_long(void)
{
  TEST_BEGIN("mipi_dsi send_command_payload routes >2 bytes via long path");
  prep();
  const ra8_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));

  const uint8_t payload[6] = {0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_dsi_send_command_payload(k_ra8_mipi_dsi_dt_dcs_long_write,
                                                   payload,
                                                   (uint16_t)sizeof(payload)));
  /* LP routing -> sequence channel 0 START asserted. */
  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  TEST_ASSERT(((reg->SQCH0SET0R & (uint32_t)k_ra8_mipi_dsi_sqch_start) != 0U));
  /* TXPPD0R holds the first 4 staged payload bytes (LE). */
  TEST_ASSERT_EQ(0x44332211U, reg->TXPPD0R);
  TEST_END("mipi_dsi send_command_payload routes >2 bytes via long path");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_send_command_payload_validation(void)
{
  TEST_BEGIN("mipi_dsi send_command_payload null + zero-length checks");
  prep();
  const ra8_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));

  /* len > 0 with NULL payload rejected. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_mipi_dsi_send_command_payload(k_ra8_mipi_dsi_dt_dcs_long_write, nullptr, 4U));
  /* len == 0 with NULL payload accepted (zero-length short packet). */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_dsi_send_command_payload(k_ra8_mipi_dsi_dt_null_packet, nullptr, 0U));
  TEST_END("mipi_dsi send_command_payload null + zero-length checks");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_ulps_lp00_drive_sequence(void)
{
  TEST_BEGIN("mipi_dsi ULPS enter -> exit drives ULPSCR LP-00 pulses");
  prep();
  /* Non-continuous clock so clock-lane ULPS is permitted. */
  const ra8_mipi_dsi_config_t cfg = make_cfg_non_continuous();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));

  /* Enter ULPS for both lanes -> CLENT and DLENT pulsed (LP-00 drive). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_ulps_enter(k_ra8_mipi_dsi_lane_all));
  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  TEST_ASSERT((reg->ULPSCR & (uint32_t)k_ra8_mipi_dsi_ulpscr_clent) != 0U);
  TEST_ASSERT((reg->ULPSCR & (uint32_t)k_ra8_mipi_dsi_ulpscr_dlent) != 0U);

  /* Exit ULPS -> CLEXIT/DLEXIT (LP-11 drive completion of the wake). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_ulps_exit(k_ra8_mipi_dsi_lane_all));
  TEST_ASSERT((reg->ULPSCR & (uint32_t)k_ra8_mipi_dsi_ulpscr_clexit) != 0U);
  TEST_ASSERT((reg->ULPSCR & (uint32_t)k_ra8_mipi_dsi_ulpscr_dlexit) != 0U);
  TEST_END("mipi_dsi ULPS enter -> exit drives ULPSCR LP-00 pulses");
}

/* MC/DC vector tests for ra8_mipi_dsi.c compound decisions
 * (DO-178C Level B / IEC 61508 SIL 3 / ISO 26262 ASIL C). */

typedef enum : uint8_t {
  k_mcdc_dsi_lane_bad_5 = 5U, /**< Mcdc dsi lane bad 5. */
} mcdc_dsi_lane_t;

typedef enum : uint16_t {
  k_mcdc_dsi_lp_over  = 200U,  /**< Mcdc dsi lp over.     */
  k_mcdc_dsi_hs_over  = 1100U, /**< Mcdc dsi hs over.     */
  k_mcdc_dsi_long_len = 4U,    /**< Mcdc dsi long length. */
} mcdc_dsi_u16_t;

/**
 * @test test_mcdc_validate_cfg_lane_count
 * @par MC/DC:
 * Decision (libs/ra8_hal/src/ra8_mipi_dsi.c line 240, 2 conditions):
 * `(lane_count != lanes_1) && (lane_count != lanes_2)`. V1 lc=1 (C1=F ok),
 * V2 lc=2 (T,F ok), V3 lc=5 (T,T invalid_arg). N+1=3.
 */
static void test_mcdc_validate_cfg_lane_count(void)
{
  TEST_BEGIN("mipi_dsi MC/DC validate_cfg: lc!=1 && lc!=2");
  prep();
  ra8_mipi_dsi_config_t cfg = make_cfg();
  cfg.lane_count            = k_ra8_mipi_dsi_lanes_1;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));
  prep();
  cfg            = make_cfg();
  cfg.lane_count = k_ra8_mipi_dsi_lanes_2;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));
  prep();
  cfg            = make_cfg();
  cfg.lane_count = (ra8_mipi_dsi_lane_count_t)k_mcdc_dsi_lane_bad_5;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_dsi_init(&cfg));
  TEST_END("mipi_dsi MC/DC validate_cfg: lc!=1 && lc!=2");
}

/**
 * @test test_mcdc_validate_cmd_short_paths
 * @par MC/DC:
 * Three decisions in internal_ra8_mipi_dsi_validate_cmd
 * (libs/ra8_hal/src/ra8_mipi_dsi.c lines 483, 486, 489):
 *   D_null: `(tx_len > 0) && (p_tx_buffer == NULL)`
 *   D_lp:   `low_power && (tx_len > k_max_lp_bytes)`
 *   D_hs:   `(!low_power) && (tx_len > k_max_hs_bytes)`
 * Driven via send_long_packet. 3 vectors per decision (N+1=9 total).
 */
static void test_mcdc_validate_cmd_short_paths(void)
{
  TEST_BEGIN("mipi_dsi MC/DC validate_cmd: null/LP/HS guards");
  prep();
  const ra8_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_dsi_send_long_packet(k_ra8_mipi_dsi_dt_null_packet,
                                               k_ra8_mipi_dsi_vc0,
                                               nullptr,
                                               0U,
                                               false));
  static const uint8_t four[(uint16_t)k_mcdc_dsi_long_len] = {0x01U, 0x02U, 0x03U, 0x04U};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_dsi_send_long_packet(k_ra8_mipi_dsi_dt_dcs_long_write,
                                               k_ra8_mipi_dsi_vc0,
                                               four,
                                               (uint16_t)k_mcdc_dsi_long_len,
                                               false));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_mipi_dsi_send_long_packet(k_ra8_mipi_dsi_dt_dcs_long_write,
                                               k_ra8_mipi_dsi_vc0,
                                               nullptr,
                                               (uint16_t)k_mcdc_dsi_long_len,
                                               false));
  static uint8_t lp_buf[128] = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_dsi_send_long_packet(k_ra8_mipi_dsi_dt_dcs_long_write,
                                               k_ra8_mipi_dsi_vc0,
                                               lp_buf,
                                               128U,
                                               true));
  static uint8_t hs_buf[1024] = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_dsi_send_long_packet(k_ra8_mipi_dsi_dt_dcs_long_write,
                                               k_ra8_mipi_dsi_vc0,
                                               hs_buf,
                                               1024U,
                                               false));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_mipi_dsi_send_long_packet(k_ra8_mipi_dsi_dt_dcs_long_write,
                                               k_ra8_mipi_dsi_vc0,
                                               hs_buf,
                                               (uint16_t)k_mcdc_dsi_lp_over,
                                               true));
  static uint8_t big_buf[1100] = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_mipi_dsi_send_long_packet(k_ra8_mipi_dsi_dt_dcs_long_write,
                                               k_ra8_mipi_dsi_vc0,
                                               big_buf,
                                               (uint16_t)k_mcdc_dsi_hs_over,
                                               false));
  TEST_END("mipi_dsi MC/DC validate_cmd: null/LP/HS guards");
}

/**
 * @test test_mcdc_make_dsc_a_short_payload
 * @par MC/DC:
 * Two decisions in internal_ra8_mipi_dsi_make_dsc_a (libs/ra8_hal/src/
 * ra8_mipi_dsi.c lines 334, 337):
 *   D0: `(p_tx_buffer != NULL) && (tx_len > 0)`
 *   D1: `(p_tx_buffer != NULL) && (tx_len > 1)`
 * V1 buf!=NULL tx=2 (D0 T,T D1 T,T), V2 buf!=NULL tx=1 (D0 T,T D1 T,F),
 * V3 buf!=NULL tx=0 (D0 T,F D1 T,F), V4 buf=NULL tx=0 (D0 F D1 F).
 * N+1=4 over both decisions.
 */
static void test_mcdc_make_dsc_a_short_payload(void)
{
  TEST_BEGIN("mipi_dsi MC/DC make_dsc_a: short payload byte selection");
  prep();
  const ra8_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));
  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_dsi_send_short_packet(k_ra8_mipi_dsi_dt_dcs_short_write_1,
                                                k_ra8_mipi_dsi_vc0,
                                                0xA1U,
                                                0xB2U));
  TEST_ASSERT_EQ(0xA1, (reg->SQCH0DSC[0].A & 0xFFU));
  TEST_ASSERT_EQ(0xB2, ((reg->SQCH0DSC[0].A >> 8U) & 0xFFU));
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));
  static const uint8_t one_byte[1] = {0xC3U};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_dsi_send_long_packet(k_ra8_mipi_dsi_dt_dcs_short_write_0,
                                               k_ra8_mipi_dsi_vc0,
                                               one_byte,
                                               1U,
                                               false));
  TEST_ASSERT_EQ(0xC3, (reg->SQCH1DSC[0].A & 0xFFU));
  TEST_ASSERT_EQ(0x00, ((reg->SQCH1DSC[0].A >> 8U) & 0xFFU));
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));
  static const uint8_t any_byte[1] = {0xDDU};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_dsi_send_long_packet(k_ra8_mipi_dsi_dt_dcs_short_write_0,
                                               k_ra8_mipi_dsi_vc0,
                                               any_byte,
                                               0U,
                                               false));
  TEST_ASSERT_EQ(0x00, (reg->SQCH1DSC[0].A & 0xFFU));
  TEST_ASSERT_EQ(0x00, ((reg->SQCH1DSC[0].A >> 8U) & 0xFFU));
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_dsi_send_long_packet(k_ra8_mipi_dsi_dt_dcs_short_write_0,
                                               k_ra8_mipi_dsi_vc0,
                                               nullptr,
                                               0U,
                                               false));
  TEST_END("mipi_dsi MC/DC make_dsc_a: short payload byte selection");
}

/**
 * @test test_mcdc_check_link_state
 * @par MC/DC:
 * Decisions (libs/ra8_hal/src/ra8_mipi_dsi.c lines 721, 729, 2 cond each):
 *   D_lp:  `cmd->low_power && ((link & VRUN) != 0)`
 *   D_aux: `cmd->aux_operation && ((link & VRUN) != 0)`
 * V1 lp+VRUN=0 (F ok), V2 lp+VRUN=1 (T invalid_state), V3 hs+VRUN=1
 * (D_lp F via C1=F, proceeds). N+1=3 per decision.
 */
static void test_mcdc_check_link_state(void)
{
  TEST_BEGIN("mipi_dsi MC/DC check_link_state: LP+VRUN guard");
  prep();
  const ra8_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));
  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  reg->LINKSR                     = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_dsi_send_short_packet(k_ra8_mipi_dsi_dt_dcs_short_write_1,
                                                k_ra8_mipi_dsi_vc0,
                                                0x01U,
                                                0x02U));
  reg->LINKSR = (uint32_t)k_ra8_mipi_dsi_link_vrun;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_mipi_dsi_send_short_packet(k_ra8_mipi_dsi_dt_dcs_short_write_1,
                                                k_ra8_mipi_dsi_vc0,
                                                0x01U,
                                                0x02U));
  reg->LINKSR                       = (uint32_t)k_ra8_mipi_dsi_link_vrun;
  static const uint8_t hs_payload[] = {0x01U, 0x02U, 0x03U, 0x04U};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_dsi_send_long_packet(k_ra8_mipi_dsi_dt_dcs_long_write,
                                               k_ra8_mipi_dsi_vc0,
                                               hs_payload,
                                               (uint16_t)sizeof(hs_payload),
                                               false));
  TEST_END("mipi_dsi MC/DC check_link_state: LP+VRUN guard");
}

/**
 * @test test_mcdc_send_stage_long
 * @par MC/DC:
 * Decision (libs/ra8_hal/src/ra8_mipi_dsi.c line 790, 2 conditions):
 * `is_long && (cmd->tx_len > 0)`. V1 long+tx=4 (T,T staged), V2 long+tx=0
 * (T,F skip), V3 short+tx=2 (F,- skip). N+1=3.
 */
static void test_mcdc_send_stage_long(void)
{
  TEST_BEGIN("mipi_dsi MC/DC send_stage_and_pulse: is_long && tx_len>0");
  prep();
  const ra8_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));
  volatile r_mipi_dsi_regs_t* reg     = ra8_mipi_dsi();
  static const uint8_t        four[4] = {0x11U, 0x22U, 0x33U, 0x44U};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_dsi_send_long_packet(k_ra8_mipi_dsi_dt_dcs_long_write,
                                               k_ra8_mipi_dsi_vc0,
                                               four,
                                               4U,
                                               false));
  TEST_ASSERT_EQ(0x44332211, reg->TXPPD0R);
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_dsi_send_long_packet(k_ra8_mipi_dsi_dt_dcs_long_write,
                                               k_ra8_mipi_dsi_vc0,
                                               nullptr,
                                               0U,
                                               false));
  TEST_ASSERT_EQ(0, reg->TXPPD0R);
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_dsi_send_short_packet(k_ra8_mipi_dsi_dt_dcs_short_write_1,
                                                k_ra8_mipi_dsi_vc0,
                                                0x01U,
                                                0x02U));
  TEST_END("mipi_dsi MC/DC send_stage_and_pulse: is_long && tx_len>0");
}

/**
 * @test test_mcdc_send_long_packet_arg_guard
 * @par MC/DC:
 * Decision `(tx_len > 0) && (data == nullptr)` reused at three sites in
 * libs/ra8_hal/src/ra8_mipi_dsi.c (lines 846, 1433, 1443). Per site: V1
 * tx=0 data=NULL (C1=F ok), V2 tx=4 data!=NULL (T,F ok), V3 tx=4
 * data=NULL (T,T null_ptr). N+1=3 per site, 9 total.
 */
static void test_mcdc_send_long_packet_arg_guard(void)
{
  TEST_BEGIN("mipi_dsi MC/DC send_long/cmd_long/cmd_payload: tx>0 && data==NULL");
  prep();
  const ra8_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));
  static const uint8_t four[4] = {0x01U, 0x02U, 0x03U, 0x04U};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_dsi_send_long_packet(k_ra8_mipi_dsi_dt_null_packet,
                                               k_ra8_mipi_dsi_vc0,
                                               nullptr,
                                               0U,
                                               false));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_dsi_send_long_packet(k_ra8_mipi_dsi_dt_dcs_long_write,
                                               k_ra8_mipi_dsi_vc0,
                                               four,
                                               4U,
                                               false));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_mipi_dsi_send_long_packet(k_ra8_mipi_dsi_dt_dcs_long_write,
                                               k_ra8_mipi_dsi_vc0,
                                               nullptr,
                                               4U,
                                               false));
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_dsi_send_command_long(k_ra8_mipi_dsi_dt_null_packet, nullptr, 0U));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_dsi_send_command_long(k_ra8_mipi_dsi_dt_dcs_long_write, four, 4U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_mipi_dsi_send_command_long(k_ra8_mipi_dsi_dt_dcs_long_write, nullptr, 4U));
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_dsi_send_command_payload(k_ra8_mipi_dsi_dt_null_packet, nullptr, 0U));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_dsi_send_command_payload(k_ra8_mipi_dsi_dt_dcs_long_write, four, 4U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_mipi_dsi_send_command_payload(k_ra8_mipi_dsi_dt_dcs_long_write, nullptr, 4U));
  TEST_END("mipi_dsi MC/DC send_long/cmd_long/cmd_payload: tx>0 && data==NULL");
}

/**
 * @test test_mcdc_ulps_enter_exit
 * @par MC/DC:
 * Five 2-condition AND decisions in ulps_enter / _exit (libs/ra8_hal/src/
 * ra8_mipi_dsi.c lines 905, 910, 914, 929, 933). Combined into a state
 * walk that traverses 8 calls, isolating each condition by holding the
 * others constant via mode (continuous vs non-continuous) and prior
 * call state. The vectors collectively form the N+1 set for all five.
 */
static void test_mcdc_ulps_enter_exit(void)
{
  TEST_BEGIN("mipi_dsi MC/DC ulps_enter/exit: lane + state guards");
  prep();
  const ra8_mipi_dsi_config_t cfg_cont = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg_cont));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_mipi_dsi_ulps_enter((uint8_t)k_ra8_mipi_dsi_lane_clock));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_ulps_enter((uint8_t)k_ra8_mipi_dsi_lane_data));
  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  TEST_ASSERT(((reg->ULPSCR & (uint32_t)k_ra8_mipi_dsi_ulpscr_dlent) != 0U));
  reg->ULPSCR = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_ulps_enter((uint8_t)k_ra8_mipi_dsi_lane_data));
  TEST_ASSERT_EQ(0, (reg->ULPSCR & (uint32_t)k_ra8_mipi_dsi_ulpscr_dlent));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_ulps_exit((uint8_t)k_ra8_mipi_dsi_lane_data));
  TEST_ASSERT(((reg->ULPSCR & (uint32_t)k_ra8_mipi_dsi_ulpscr_dlexit) != 0U));
  reg->ULPSCR = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_ulps_exit((uint8_t)k_ra8_mipi_dsi_lane_data));
  TEST_ASSERT_EQ(0, (reg->ULPSCR & (uint32_t)k_ra8_mipi_dsi_ulpscr_dlexit));
  prep();
  const ra8_mipi_dsi_config_t cfg_nc = make_cfg_non_continuous();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg_nc));
  reg = ra8_mipi_dsi();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_ulps_enter((uint8_t)k_ra8_mipi_dsi_lane_clock));
  TEST_ASSERT(((reg->ULPSCR & (uint32_t)k_ra8_mipi_dsi_ulpscr_clent) != 0U));
  reg->ULPSCR = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_ulps_enter((uint8_t)k_ra8_mipi_dsi_lane_clock));
  TEST_ASSERT_EQ(0, (reg->ULPSCR & (uint32_t)k_ra8_mipi_dsi_ulpscr_clent));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_ulps_exit((uint8_t)k_ra8_mipi_dsi_lane_clock));
  TEST_ASSERT(((reg->ULPSCR & (uint32_t)k_ra8_mipi_dsi_ulpscr_clexit) != 0U));
  TEST_END("mipi_dsi MC/DC ulps_enter/exit: lane + state guards");
}

/**
 * @test test_mcdc_set_video_timing_compound
 * @par MC/DC:
 * Three decisions in ra8_mipi_dsi_set_video_timing (libs/ra8_hal/src/
 * ra8_mipi_dsi.c lines 1384/1388/1394): D_sync (2 cond), D_porch (4 cond),
 * D_active (2 cond). Each condition isolated by setting only that field
 * over-max while holding the rest in-range. N+1=9 vectors total.
 */
static void test_mcdc_set_video_timing_compound(void)
{
  TEST_BEGIN("mipi_dsi MC/DC set_video_timing: sync+porch+active overflow");
  prep();
  const ra8_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));
  ra8_mipi_dsi_video_timing_t t = make_timing();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_set_video_timing(&t));
  t                 = make_timing();
  t.horizontal_sync = (uint16_t)0xFFFFU;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_dsi_set_video_timing(&t));
  t               = make_timing();
  t.vertical_sync = (uint16_t)0xFFFFU;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_dsi_set_video_timing(&t));
  t                       = make_timing();
  t.horizontal_back_porch = (uint16_t)0xFFFFU;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_dsi_set_video_timing(&t));
  t                        = make_timing();
  t.horizontal_front_porch = (uint16_t)0xFFFFU;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_dsi_set_video_timing(&t));
  t                     = make_timing();
  t.vertical_back_porch = (uint16_t)0xFFFFU;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_dsi_set_video_timing(&t));
  t                      = make_timing();
  t.vertical_front_porch = (uint16_t)0xFFFFU;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_dsi_set_video_timing(&t));
  t                   = make_timing();
  t.horizontal_active = (uint16_t)0xFFFFU;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_dsi_set_video_timing(&t));
  t                 = make_timing();
  t.vertical_active = (uint16_t)0xFFFFU;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_dsi_set_video_timing(&t));
  TEST_END("mipi_dsi MC/DC set_video_timing: sync+porch+active overflow");
}

/**
 * @test test_mcdc_dispatch_receive_pending
 * @par MC/DC:
 * Decision (libs/ra8_hal/src/ra8_mipi_dsi.c line 1298, 2 conditions):
 * `(s_pending_rx_buffer != NULL) && (s_pending_rx_len > 0)`. V1 no
 * pending read, RXRESP set (C1=F no copy), V2 read_packet pending,
 * RXRESP set (T,T copies). The (buf!=NULL, len=0) row is unreachable
 * from the public API; documented as deactivated code per DO-178C
 * 6.4.4.3. N+1=2 for the reachable subset.
 */
static void test_mcdc_dispatch_receive_pending(void)
{
  TEST_BEGIN("mipi_dsi MC/DC dispatch_receive: pending buf && len>0");
  prep();
  const ra8_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));
  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  reg->RXSR                       = (uint32_t)k_ra8_mipi_dsi_rxsr_rxresp;
  ra8_mipi_dsi_dispatch_receive();
  static uint8_t rx[4] = {};
  reg->LINKSR          = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_dsi_read_packet(k_ra8_mipi_dsi_dt_dcs_read,
                                          k_ra8_mipi_dsi_vc0,
                                          0xAAU,
                                          0x55U,
                                          rx,
                                          (uint16_t)sizeof(rx)));
  reg->RXSR = (uint32_t)k_ra8_mipi_dsi_rxsr_rxresp;
  ra8_mipi_dsi_dispatch_receive();
  TEST_END("mipi_dsi MC/DC dispatch_receive: pending buf && len>0");
}

/**
 * @test test_mcdc_program_descriptor_buf_addr
 * @par MC/DC:
 * Decision (libs/ra8_hal/src/ra8_mipi_dsi.c line 859, 2 conditions):
 * `(cmd->bta == bta_read) || (cmd->p_rx_buffer != NULL)`.
 *  - V1: bta=none,  rx=NULL  -> C1=F, C2=F -> descriptor.D = tx_buffer.
 *  - V2: bta=read,  rx=buf   -> C1=T (shorts) -> descriptor.D = rx_buffer.
 *  - V3: bta=none,  rx=buf   -> C1=F, C2=T   -> descriptor.D = rx_buffer.
 * V1+V2 prove C1 independently flips outcome; V1+V3 prove C2.
 * V3 reaches the production decision via the public ra8_mipi_dsi_send_command
 * entry with a hand-built ra8_mipi_dsi_command_t carrying bta=none and a
 * non-NULL p_rx_buffer (write-with-rx is structurally legal at the API
 * boundary even though the higher-level read_packet wrapper always sets
 * bta=read). N+1 = 3 vectors satisfy MC/DC fully.
 */
static void test_mcdc_program_descriptor_buf_addr(void)
{
  TEST_BEGIN("mipi_dsi MC/DC program_descriptor: bta==read || p_rx!=NULL");
  prep();
  const ra8_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));
  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_dsi_send_short_packet(k_ra8_mipi_dsi_dt_dcs_short_write_1,
                                                k_ra8_mipi_dsi_vc0,
                                                0x01U,
                                                0x02U));
  (void)reg->SQCH0DSC[0].D;
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));
  reg                  = ra8_mipi_dsi();
  static uint8_t rx[4] = {};
  reg->LINKSR          = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_dsi_read_packet(k_ra8_mipi_dsi_dt_dcs_read,
                                          k_ra8_mipi_dsi_vc0,
                                          0xAAU,
                                          0x55U,
                                          rx,
                                          (uint16_t)sizeof(rx)));
  /* SQCH0DSC[0].D is a 32-bit HW register; truncate the host pointer. */
  const uint32_t exp_rx_addr = (uint32_t)(uintptr_t)rx;
  TEST_ASSERT_EQ(exp_rx_addr, reg->SQCH0DSC[0].D);

  /* V3: bta=none, rx!=NULL via send_command. The descriptor's D word
   * must come from the rx buffer (C2 alone forces the OR true). */
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));
  reg                                     = ra8_mipi_dsi();
  reg->LINKSR                             = 0U;
  static uint8_t               rx_only[4] = {0U};
  static uint8_t               tx_buf[2]  = {0xAAU, 0xBBU};
  const ra8_mipi_dsi_command_t cmd        = {
    .cmd_id          = k_ra8_mipi_dsi_dt_dcs_short_write_1,
    .virtual_channel = k_ra8_mipi_dsi_vc0,
    .bta             = k_ra8_mipi_dsi_bta_none,
    .low_power       = true,
    .ack_request     = false,
    .aux_operation   = false,
    .action_code     = 0U,
    .tx_len          = (uint16_t)sizeof(tx_buf),
    .p_tx_buffer     = tx_buf,
    .p_rx_buffer     = rx_only,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_send_command(&cmd));
  const uint32_t exp_rx_only_addr = (uint32_t)(uintptr_t)rx_only;
  TEST_ASSERT_EQ(exp_rx_only_addr, reg->SQCH0DSC[0].D);
  TEST_END("mipi_dsi MC/DC program_descriptor: bta==read || p_rx!=NULL");
}

/**
 * @test test_mcdc_validate_cmd_tx_null_pair
 *
 * @par MC/DC:
 * Decision: ``if ((cmd->tx_len > 0U) && (cmd->p_tx_buffer == nullptr))``
 * (libs/ra8_hal/src/ra8_mipi_dsi.c internal_ra8_mipi_dsi_validate_cmd).
 * Reachable only via the public ra8_mipi_dsi_send_command direct entry
 * (the ra8_mipi_dsi_send_long_packet wrapper has its own pre-guard at
 * line 953 that intercepts the (tx_len>0, buf=NULL) case before it can
 * land on validate_cmd).
 *
 * 2-condition AND, N+1 = 3 vectors:
 *  - V1: tx_len=0,  p_tx=NULL -> C1=F short -> dec F (proceed).
 *  - V2: tx_len=4,  p_tx=NULL -> C1=T,C2=T  -> dec T (null_ptr).
 *  - V3: tx_len=4,  p_tx=ok   -> C1=T,C2=F  -> dec F (proceed).
 * V1+V2 isolate C1; V2+V3 isolate C2.
 */
static void test_mcdc_validate_cmd_tx_null_pair(void)
{
  TEST_BEGIN("mipi_dsi MC/DC validate_cmd: tx_len>0 && p_tx==NULL via send_command");
  prep();
  const ra8_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));

  /* V1: tx_len=0, p_tx_buffer=NULL -> validate_cmd C1=F -> proceed. */
  const ra8_mipi_dsi_command_t v1_cmd = {
    .cmd_id          = k_ra8_mipi_dsi_dt_dcs_short_write_0,
    .virtual_channel = k_ra8_mipi_dsi_vc0,
    .bta             = k_ra8_mipi_dsi_bta_none,
    .low_power       = false,
    .ack_request     = false,
    .aux_operation   = false,
    .action_code     = 0U,
    .tx_len          = 0U,
    .p_tx_buffer     = nullptr,
    .p_rx_buffer     = nullptr,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_send_command(&v1_cmd));

  /* V2: tx_len=4, p_tx_buffer=NULL -> validate_cmd C1=T,C2=T -> null_ptr. */
  const ra8_mipi_dsi_command_t v2_cmd = {
    .cmd_id          = k_ra8_mipi_dsi_dt_dcs_long_write,
    .virtual_channel = k_ra8_mipi_dsi_vc0,
    .bta             = k_ra8_mipi_dsi_bta_none,
    .low_power       = false,
    .ack_request     = false,
    .aux_operation   = false,
    .action_code     = 0U,
    .tx_len          = 4U,
    .p_tx_buffer     = nullptr,
    .p_rx_buffer     = nullptr,
  };
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mipi_dsi_send_command(&v2_cmd));

  /* V3: tx_len=4, p_tx_buffer=valid -> validate_cmd C1=T,C2=F -> proceed. */
  static const uint8_t         v3_payload[] = {0x01U, 0x02U, 0x03U, 0x04U};
  const ra8_mipi_dsi_command_t v3_cmd       = {
    .cmd_id          = k_ra8_mipi_dsi_dt_dcs_long_write,
    .virtual_channel = k_ra8_mipi_dsi_vc0,
    .bta             = k_ra8_mipi_dsi_bta_none,
    .low_power       = false,
    .ack_request     = false,
    .aux_operation   = false,
    .action_code     = 0U,
    .tx_len          = (uint16_t)sizeof(v3_payload),
    .p_tx_buffer     = v3_payload,
    .p_rx_buffer     = nullptr,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_send_command(&v3_cmd));

  TEST_END("mipi_dsi MC/DC validate_cmd: tx_len>0 && p_tx==NULL via send_command");
}

/**
 * @test test_mcdc_check_link_aux_op_vrun
 *
 * @par MC/DC:
 * Decision: ``if (cmd->aux_operation && ((link & VRUN) != 0U))``
 * (libs/ra8_hal/src/ra8_mipi_dsi.c internal_check_link_state).
 * Reachable only via send_command direct entry (action-code helpers do
 * not set aux_operation for HS-mode commands).
 *
 * 2-condition AND, N+1 = 3 vectors:
 *  - V1: aux=false, vrun=0 -> C1=F short -> dec F (ok).
 *  - V2: aux=true,  vrun=1 -> C1=T,C2=T  -> dec T (invalid_state).
 *  - V3: aux=true,  vrun=0 -> C1=T,C2=F  -> dec F (ok).
 * V1+V2 isolate C1; V2+V3 isolate C2. (cmd is HS-mode so the line-820
 * lp+vrun guard does not preempt our line-828 check.)
 */
static void test_mcdc_check_link_aux_op_vrun(void)
{
  TEST_BEGIN("mipi_dsi MC/DC check_link_state: aux_op && VRUN");
  prep();
  const ra8_mipi_dsi_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));
  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();

  static const uint8_t         hs_payload[] = {0x10U, 0x20U};
  const ra8_mipi_dsi_command_t base         = {
    .cmd_id          = k_ra8_mipi_dsi_dt_dcs_long_write,
    .virtual_channel = k_ra8_mipi_dsi_vc0,
    .bta             = k_ra8_mipi_dsi_bta_none,
    .low_power       = false,
    .ack_request     = false,
    .aux_operation   = false,
    .action_code     = 0U,
    .tx_len          = (uint16_t)sizeof(hs_payload),
    .p_tx_buffer     = hs_payload,
    .p_rx_buffer     = nullptr,
  };

  /* V1: aux=false, vrun=0 -> ok. */
  reg->LINKSR = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_send_command(&base));

  /* V2: aux=true, vrun=1 -> invalid_state. */
  ra8_mipi_dsi_command_t v2 = base;
  v2.aux_operation          = true;
  reg->LINKSR               = (uint32_t)k_ra8_mipi_dsi_link_vrun;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_mipi_dsi_send_command(&v2));

  /* V3: aux=true, vrun=0 -> ok. */
  ra8_mipi_dsi_command_t v3 = base;
  v3.aux_operation          = true;
  reg->LINKSR               = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_send_command(&v3));

  TEST_END("mipi_dsi MC/DC check_link_state: aux_op && VRUN");
}

/**
 * @test test_mcdc_ulps_exit_clock_lane_state_pair
 *
 * @par MC/DC:
 * Decision: ``if (((lanes & CLOCK) != 0U) && s_clock_lanes_in_ulps)``
 * (libs/ra8_hal/src/ra8_mipi_dsi.c ra8_mipi_dsi_ulps_exit).
 *
 * 2-condition AND, N+1 = 3 vectors:
 *  - V1: lanes=DATA only,  clock_in_ulps=*    -> C1=F short -> no-op.
 *  - V2: lanes=CLOCK,      clock_in_ulps=true -> C1=T,C2=T  -> CLEXIT set.
 *  - V3: lanes=CLOCK,      clock_in_ulps=false-> C1=T,C2=F  -> no CLEXIT.
 * V1+V2 isolate C1; V2+V3 isolate C2. The non-continuous-clock config is
 * required because ulps_enter rejects clock-lane requests on continuous-
 * clock setups (line 1010 guard).
 */
static void test_mcdc_ulps_exit_clock_lane_state_pair(void)
{
  TEST_BEGIN("mipi_dsi MC/DC ulps_exit: (lanes & CLOCK) && s_clock_in_ulps");
  prep();
  const ra8_mipi_dsi_config_t cfg = make_cfg_non_continuous();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));
  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();

  /* V1: clock NOT in lanes mask -> C1=F short. Must already be in ulps
   * for data lane (set up via enter) so the data branch fires and we can
   * still observe a non-spurious dlexit while clock branch is skipped. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_ulps_enter((uint8_t)k_ra8_mipi_dsi_lane_data));
  reg->ULPSCR = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_ulps_exit((uint8_t)k_ra8_mipi_dsi_lane_data));
  TEST_ASSERT_EQ(0, (reg->ULPSCR & (uint32_t)k_ra8_mipi_dsi_ulpscr_clexit));

  /* V2: lanes=CLOCK, s_clock_in_ulps=true -> CLEXIT set. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_ulps_enter((uint8_t)k_ra8_mipi_dsi_lane_clock));
  reg->ULPSCR = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_ulps_exit((uint8_t)k_ra8_mipi_dsi_lane_clock));
  TEST_ASSERT(((reg->ULPSCR & (uint32_t)k_ra8_mipi_dsi_ulpscr_clexit) != 0U));

  /* V3: lanes=CLOCK, s_clock_in_ulps=false (just exited) -> no-op. */
  reg->ULPSCR = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_ulps_exit((uint8_t)k_ra8_mipi_dsi_lane_clock));
  TEST_ASSERT_EQ(0, (reg->ULPSCR & (uint32_t)k_ra8_mipi_dsi_ulpscr_clexit));

  TEST_END("mipi_dsi MC/DC ulps_exit: (lanes & CLOCK) && s_clock_in_ulps");
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
  test_set_video_timing();
  test_set_video_timing_null();
  test_set_video_timing_overflow_each_field();
  test_send_command_short();
  test_send_command_long();
  test_enter_exit_ulps();
  test_enter_ulps_continuous_rejected();
  test_get_link_status();
  test_send_command_payload_short();
  test_send_command_payload_long();
  test_send_command_payload_validation();
  test_ulps_lp00_drive_sequence();
  test_mcdc_validate_cfg_lane_count();
  test_mcdc_validate_cmd_short_paths();
  test_mcdc_make_dsc_a_short_payload();
  test_mcdc_check_link_state();
  test_mcdc_send_stage_long();
  test_mcdc_send_long_packet_arg_guard();
  test_mcdc_ulps_enter_exit();
  test_mcdc_set_video_timing_compound();
  test_mcdc_dispatch_receive_pending();
  test_mcdc_program_descriptor_buf_addr();
  test_mcdc_validate_cmd_tx_null_pair();
  test_mcdc_check_link_aux_op_vrun();
  test_mcdc_ulps_exit_clock_lane_state_pair();
  (void)fprintf(stderr, "[OK  ] test_ra8_mipi_dsi.c\n");
  return 0;
}
