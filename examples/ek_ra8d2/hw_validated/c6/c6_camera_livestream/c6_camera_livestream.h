/**
 * @file examples/ek_ra8d2/hw_validated/c6/c6_camera_livestream/c6_camera_livestream.h
 * @brief Shared contracts for the OV5640-over-ESP32-C6 browser livestream.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_c6link.h"
#include "ra8_err.h"

/** @brief Application sizing, pacing and fixed network values. */
typedef enum : uint32_t {
  k_c6_cam_uart_baud       = 115200U,
  k_c6_cam_sck_hz          = 1000000U,
  k_c6_cam_edge_poll_ms    = 2U,
  k_c6_cam_boot_wait_ms    = 200U,
  k_c6_cam_assoc_polls     = 200U,
  k_c6_cam_assoc_gap_ms    = 50U,
  k_c6_cam_dhcp_wait_ms    = 25000U,
  k_c6_cam_worker_stack    = 12288U,
  k_c6_cam_worker_prio     = 8U,
  k_c6_cam_arena_bytes     = 4096U,
  k_c6_cam_http_port       = 80U,
  k_c6_cam_http_chunk      = 1400U,
  k_c6_cam_request_max     = 512U,
  k_c6_cam_source_width    = 640U,
  k_c6_cam_source_height   = 480U,
  k_c6_cam_source_stride   = 1280U,
  k_c6_cam_stream_width    = 320U,
  k_c6_cam_stream_height   = 240U,
  k_c6_cam_rgb_bytes       = 230400U,
  k_c6_cam_jpeg_bytes      = 230400U,
  k_c6_cam_jpeg_quality    = 65U,
  k_c6_cam_xclk_hz         = 24000000U,
  k_c6_cam_gpt_period_max  = 0xFFFFU,
  k_c6_cam_mode_settle_ms  = 100U,
  k_c6_cam_net_pkt_payload = 1568U,
  k_c6_cam_net_pool_bytes  = 49152U,
  k_c6_cam_net_ip_stack    = 2048U,
  k_c6_cam_net_arp_bytes   = 1040U,
  k_c6_cam_net_ip_prio     = 3U,
} c6_cam_cfg_t;

/** @brief DHCP lease returned by the raw, bench-validated C6/NetX path. */
typedef struct {
  uint32_t ip;
  uint32_t mask;
  uint32_t gateway;
  uint32_t dhcp_server;
  bool     bound;
} c6_cam_lease_t;

/** @brief Camera image and board-switch initialization, then one CEU capture. */
[[nodiscard]] ra8_err_t c6_cam_camera_init(void);
[[nodiscard]] ra8_err_t c6_cam_camera_capture_jpeg(const uint8_t** out_jpeg, uint32_t* out_bytes);

/** @brief Bind NetX to an associated raw C6 link and obtain a DHCP lease. */
[[nodiscard]] ra8_err_t
c6_cam_net_up(ra8_c6link_t* link, const ra8_c6link_mac_t* mac, c6_cam_lease_t* out);

/** @brief Serve the browser UI and fresh JPEG frames forever. */
[[noreturn]] void c6_cam_http_serve(void);

/** @brief Bounded SCI8 console helpers. */
void c6_cam_puts(const char* text);
void c6_cam_put_u32(uint32_t value);
void c6_cam_put_ip(uint32_t ip);
