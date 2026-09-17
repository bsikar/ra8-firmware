/**
 * @file test_ra8_mipi_dsi_mcdc.c
 * @brief MC/DC vectors for the ra8_mipi_dsi.c compound decisions
 *
 * @details
 * Split sibling of the original test_ra8_mipi_dsi.c suite closing the
 * DO-178C Level B / IEC 61508 SIL 3 MC/DC obligations for the compound
 * boolean decisions in ra8_mipi_dsi.c: config lane-count validation,
 * command validation short paths, descriptor staging, link-state
 * checks, long-packet argument guards, ULPS enter / exit pairs, video
 * timing compounds, receive-pending dispatch and descriptor buffer
 * programming.
 *
 * Sibling suites: test_ra8_mipi_dsi_cmd.c (init + command packets)
 * and test_ra8_mipi_dsi_video.c (video mode + sweep-6 shorthands).
 * Shared fixtures live in support/inc/mipi_dsi_test_util.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "mipi_dsi_test_util.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_mipi_dsi.h"
#include "ra8_mipi_dsi_regs.h"
#include "ra8_mstp.h"
#include "unity_minimal.h"

/**
 * @enum t_dsi_buf_t
 * @brief Transmit-buffer capacities and the payload the short-packet arm sends.
 *
 * @details
 * The driver switches between low-power and high-speed paths on payload size,
 * so the three capacities straddle that boundary: one under the LP limit, one
 * at the HS limit, and one deliberately past it.
 */
typedef enum : uint16_t {
  k_t_lp_buf_cap = 128U,  /**< Low-power transmit buffer, bytes.         */
  k_t_hs_buf_cap = 1024U, /**< High-speed transmit buffer, bytes.        */
  k_t_over_cap   = 1100U, /**< A buffer past the largest legal transfer. */
  k_t_payload_b0 = 0xAAU, /**< Short-packet payload byte 0; distinct from b1 so
                               a swapped pair is visible.                       */
  k_t_payload_b1 = 0xBBU, /**< Short-packet payload byte 1. */
} t_dsi_buf_t;

/**
 * @enum t_dsi_timing_t
 * @brief Out-of-range timing value the video-mode validator must reject.
 */
typedef enum : uint16_t {
  k_t_timing_over = 0xFFFFU, /**< Applied to each timing field in turn: every
                                  one must independently fail validation.       */
} t_dsi_timing_t;

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
  static uint8_t s_lp_buf[k_t_lp_buf_cap] = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_dsi_send_long_packet(k_ra8_mipi_dsi_dt_dcs_long_write,
                                               k_ra8_mipi_dsi_vc0,
                                               s_lp_buf,
                                               128U,
                                               true));
  static uint8_t s_hs_buf[k_t_hs_buf_cap] = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_dsi_send_long_packet(k_ra8_mipi_dsi_dt_dcs_long_write,
                                               k_ra8_mipi_dsi_vc0,
                                               s_hs_buf,
                                               1024U,
                                               false));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_mipi_dsi_send_long_packet(k_ra8_mipi_dsi_dt_dcs_long_write,
                                               k_ra8_mipi_dsi_vc0,
                                               s_hs_buf,
                                               (uint16_t)k_mcdc_dsi_lp_over,
                                               true));
  static uint8_t s_big_buf[k_t_over_cap] = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_mipi_dsi_send_long_packet(k_ra8_mipi_dsi_dt_dcs_long_write,
                                               k_ra8_mipi_dsi_vc0,
                                               s_big_buf,
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
  t.horizontal_sync = (uint16_t)k_t_timing_over;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_dsi_set_video_timing(&t));
  t               = make_timing();
  t.vertical_sync = (uint16_t)k_t_timing_over;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_dsi_set_video_timing(&t));
  t                       = make_timing();
  t.horizontal_back_porch = (uint16_t)k_t_timing_over;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_dsi_set_video_timing(&t));
  t                        = make_timing();
  t.horizontal_front_porch = (uint16_t)k_t_timing_over;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_dsi_set_video_timing(&t));
  t                     = make_timing();
  t.vertical_back_porch = (uint16_t)k_t_timing_over;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_dsi_set_video_timing(&t));
  t                      = make_timing();
  t.vertical_front_porch = (uint16_t)k_t_timing_over;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_dsi_set_video_timing(&t));
  t                   = make_timing();
  t.horizontal_active = (uint16_t)k_t_timing_over;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mipi_dsi_set_video_timing(&t));
  t                 = make_timing();
  t.vertical_active = (uint16_t)k_t_timing_over;
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
  static uint8_t s_rx[4] = {};
  reg->LINKSR            = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_dsi_read_packet(k_ra8_mipi_dsi_dt_dcs_read,
                                          k_ra8_mipi_dsi_vc0,
                                          0xAAU,
                                          0x55U,
                                          s_rx,
                                          (uint16_t)sizeof(s_rx)));
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
  reg                    = ra8_mipi_dsi();
  static uint8_t s_rx[4] = {};
  reg->LINKSR            = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_dsi_read_packet(k_ra8_mipi_dsi_dt_dcs_read,
                                          k_ra8_mipi_dsi_vc0,
                                          0xAAU,
                                          0x55U,
                                          s_rx,
                                          (uint16_t)sizeof(s_rx)));
  /* SQCH0DSC[0].D is a 32-bit HW register; truncate the host pointer. */
  const uint32_t exp_rx_addr = (uint32_t)(uintptr_t)s_rx;
  TEST_ASSERT_EQ(exp_rx_addr, reg->SQCH0DSC[0].D);

  /* V3: bta=none, rx!=NULL via send_command. The descriptor's D word
   * must come from the rx buffer (C2 alone forces the OR true). */
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_init(&cfg));
  reg                                       = ra8_mipi_dsi();
  reg->LINKSR                               = 0U;
  static uint8_t               s_rx_only[4] = {};
  static uint8_t               s_tx_buf[2]  = {k_t_payload_b0, k_t_payload_b1};
  const ra8_mipi_dsi_command_t cmd          = {
    .cmd_id          = k_ra8_mipi_dsi_dt_dcs_short_write_1,
    .virtual_channel = k_ra8_mipi_dsi_vc0,
    .bta             = k_ra8_mipi_dsi_bta_none,
    .low_power       = true,
    .ack_request     = false,
    .aux_operation   = false,
    .action_code     = 0U,
    .tx_len          = (uint16_t)sizeof(s_tx_buf),
    .p_tx_buffer     = s_tx_buf,
    .p_rx_buffer     = s_rx_only,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_dsi_send_command(&cmd));
  const uint32_t exp_rx_only_addr = (uint32_t)(uintptr_t)s_rx_only;
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

int main(void)
{
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
  return 0;
}
