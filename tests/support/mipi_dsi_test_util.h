/**
 * @file mipi_dsi_test_util.h
 * @brief Shared fixture helpers for the test_ra8_mipi_dsi_* sibling suites
 *
 * @details
 * The original test_ra8_mipi_dsi.c suite was split along its test-group
 * seams into test_ra8_mipi_dsi_cmd.c, test_ra8_mipi_dsi_video.c and
 * test_ra8_mipi_dsi_mcdc.c. All three siblings share the same numeric
 * test-constant enum and the config / timing fixture builders below, so
 * they live here once (header-only, ``static inline``) instead of being
 * copied three times.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_mipi_dsi.h"
#include "ra8_mstp.h"
#include "ra8_sim_mmap.h"

/**
 * @enum ra8_mipi_dsi_test_const_t
 * @brief Numeric inputs used by the test cases (no magic numbers).
 */
typedef enum : uint32_t {
  k_test_max_return_pkt      = 64U,
  k_test_dcs_soft_reset      = 0x01U,
  k_test_param0              = 0xAAU,
  k_test_param1              = 0x55U,
  k_test_isr_seed            = (uint32_t)k_ra8_mipi_dsi_isr_sq0 | (uint32_t)k_ra8_mipi_dsi_isr_vm,
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
} ra8_mipi_dsi_test_const_t;

/**
 * @brief Reset the simulated peripheral memory + MSTP table.
 */
static inline void prep(void)
{
  ra8_sim_mmap_reset();
  (void)ra8_mstp_init();
}

/**
 * @brief Build a "happy path" config struct.
 */
static inline ra8_mipi_dsi_config_t make_cfg(void)
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
static inline ra8_mipi_dsi_config_t make_cfg_non_continuous(void)
{
  ra8_mipi_dsi_config_t cfg = make_cfg();
  cfg.clock_mode            = k_ra8_mipi_dsi_clock_non_continuous;
  return cfg;
}

/**
 * @brief Build the reference 1024x600 video timing set used across the suites.
 */
static inline ra8_mipi_dsi_video_timing_t make_timing(void)
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
