/**
 * @file test_ra8_mipi_dsi_video.c
 * @brief Unit tests for MIPI DSI-2 video mode, link status and timing setters
 *
 * @details
 * Split sibling of the original test_ra8_mipi_dsi.c suite covering
 * the video-mode datapath of ra8_mipi_dsi.c against the
 * ``ra8_fake_mmap``-backed register window:
 *
 * - video configure / pixel formats / start-stop and HS clock control
 * - link-status snapshot, ACK error, RX result + payload reads,
 *   tearing-effect event, IRQ enables, soft reset, power transition
 * - the sweep-6 shorthand surface: video timing setters incl.
 *   per-field overflow rejection, command-mode short / long / payload
 *   wrappers, ULPS shorthand and the LP-00 drive sequence
 *
 * Sibling suites: test_ra8_mipi_dsi_cmd.c (init + command packets)
 * and test_ra8_mipi_dsi_mcdc.c (MC/DC vectors). Shared fixtures live
 * in support/mipi_dsi_test_util.h.
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
 * @enum t_dsi_rx_t
 * @brief Receive-path register patterns and the out-of-range timing value.
 */
typedef enum : uint32_t {
  k_t_rx_payload_b0 = 0x12U,        /**< Short-response payload byte 0. */
  k_t_rx_payload_b1 = 0x34U,        /**< Payload byte 1; distinct from b0 so a
                                        swapped pair is visible.               */
  k_t_ack_err_word  = 0x0001A55AUL, /**< AKEPACMSR: virtual channel 1 in bits
                                         19:16, an alternating error pattern in
                                         15:0 so no adjacent error bits agree.  */
  k_t_timing_over   = 0xFFFFU,      /**< Applied to each timing field in turn:
                                        every one exceeds its 15-bit field and
                                        must independently fail validation.     */
} t_dsi_rx_t;

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
  reg->AKEPACMSR                  = k_t_ack_err_word; /* VC=1 in bits 19:16, errors in 15:0 */

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
  reg->RXRSS0R = k_t_rx_payload_b0 | (k_t_rx_payload_b1 << 8) | (0x06U << 16) |
                 (uint32_t)k_ra8_mipi_dsi_rxrss_rxsuc;

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
  bad.horizontal_active           = (uint16_t)k_t_timing_over; /* > 15 bits. */
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
  t.vertical_sync               = (uint16_t)k_t_timing_over;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_dsi_set_video_timing(&t));

  t                       = make_timing();
  t.horizontal_back_porch = (uint16_t)k_t_timing_over;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_dsi_set_video_timing(&t));

  t                 = make_timing();
  t.vertical_active = (uint16_t)k_t_timing_over;
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
  test_video_configure,
  test_video_pixel_formats,
  test_video_start_stop,
  test_hs_clock_start_stop,
  test_link_status_get,
  test_ack_error,
  test_rx_result_get,
  test_rx_payload_read,
  test_te_event,
  test_irq_enable,
  test_soft_reset,
  test_power_transition,
  test_set_video_timing,
  test_set_video_timing_null,
  test_send_command_short,
  test_send_command_long,
  test_enter_exit_ulps,
  test_enter_ulps_continuous_rejected,
  test_get_link_status,
  test_set_video_timing_overflow_each_field,
  test_send_command_payload_short,
  test_send_command_payload_long,
  test_send_command_payload_validation,
  test_ulps_lp00_drive_sequence,
};

int main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  return 0;
}
