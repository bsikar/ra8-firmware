/**
 * @file examples/ek_ra8d2/common/c6_camera_server/inc/c6_camera_server.h
 * @brief Shared contracts for ESP32-C6 browser camera servers.
 * @details Defines the heap-free camera, audio, console, and HTTP seams shared
 *          by the validated livestream and hardware-JPEG applications.
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
  k_c6_cam_uart_baud       = 115200U,   /**< Diagnostic console baud rate.          */
  k_c6_cam_sck_hz          = 10000000U, /**< Camera-stream ESP-hosted SPI rate.     */
  k_c6_cam_edge_poll_ms    = 2U,        /**< C6 interrupt-edge poll period.         */
  k_c6_cam_boot_wait_ms    = 200U,      /**< C6 boot stabilization delay.           */
  k_c6_cam_assoc_polls     = 200U,      /**< Maximum association status polls.      */
  k_c6_cam_assoc_gap_ms    = 50U,       /**< Delay between association polls.       */
  k_c6_cam_dhcp_wait_ms    = 25000U,    /**< DHCP lease timeout.                    */
  k_c6_cam_worker_stack    = 12288U,    /**< Worker thread stack bytes.             */
  k_c6_cam_worker_prio     = 8U,        /**< ThreadX worker priority.               */
  k_c6_cam_arena_bytes     = 4096U,     /**< C6-link caller-owned arena size.       */
  k_c6_cam_http_port       = 80U,       /**< Browser server TCP port.               */
  k_c6_cam_http_chunk      = 1400U,     /**< Maximum HTTP send chunk.               */
  k_c6_cam_request_max     = 512U,      /**< Maximum parsed request bytes.          */
  k_c6_cam_tcp_window      = 4096U,     /**< HTTP socket receive window.            */
  k_c6_cam_listen_backlog  = 1U,        /**< Single-client listen backlog.          */
  k_c6_cam_stream_frames   = 1024U,     /**< Frames served before client reconnect. */
  k_c6_cam_net_pkt_payload = 1568U,     /**< NetX packet payload bytes.             */
  k_c6_cam_net_pool_bytes  = 49152U,    /**< NetX packet pool bytes.                */
  k_c6_cam_net_ip_stack    = 2048U,     /**< NetX IP thread stack bytes.            */
  k_c6_cam_net_arp_bytes   = 1040U,     /**< NetX ARP cache bytes.                  */
  k_c6_cam_net_ip_prio     = 3U,        /**< NetX IP thread priority.               */
} c6_cam_cfg_t;

/** @brief DHCP lease returned by the raw, bench-validated C6/NetX path. */
typedef struct {
  uint32_t ip;          /**< Assigned IPv4 address.         */
  uint32_t mask;        /**< Assigned IPv4 network mask.    */
  uint32_t gateway;     /**< Assigned IPv4 default gateway. */
  uint32_t dhcp_server; /**< DHCP server IPv4 address.      */
  bool     bound;       /**< True after a lease is bound.   */
} c6_cam_lease_t;

/** @brief Per-frame pipeline latency exposed in HTTP Server-Timing headers. */
typedef struct {
  uint32_t timestamp_ms; /**< Capture-start monotonic timestamp. */
  uint32_t capture_ms;   /**< Camera capture latency.            */
  uint32_t convert_ms;   /**< Pixel conversion latency.          */
  uint32_t encode_ms;    /**< Codec latency.                     */
} c6_cam_frame_timing_t;

/** @brief Run the shared runtime-provisioned C6, DHCP, and HTTP application flow. */
[[nodiscard]] int32_t c6_cam_app_run(void);

/** @brief Camera image and board-switch initialization, then one CEU capture. */
[[nodiscard]] ra8_err_t c6_cam_camera_init(void);
[[nodiscard]] ra8_err_t c6_cam_camera_capture_jpeg(const uint8_t**        out_jpeg,
                                                   uint32_t*              out_bytes,
                                                   c6_cam_frame_timing_t* out_timing);
/** @brief Human-readable description of the selected camera backend. */
[[nodiscard]] const char* c6_cam_camera_description(void);
/**
 * @brief Emit backend-specific diagnostics for the last capture failure.
 * @details Writes retained sensor or CEU status through the shared bounded console helpers.
 * @pre The board console is initialized.
 * @pre A camera capture was attempted or the backend has reset diagnostics.
 * @post A backend-specific diagnostic line is queued.
 * @post Camera and capture state remain unchanged.
 * @note Intended for failure paths; output content depends on the selected backend.
 * @since 0.1.0
 */
void c6_cam_camera_report_last_error(void);

/** @brief Start continuous MIC1 capture into the SDRAM audio ring. */
[[nodiscard]] ra8_err_t c6_cam_audio_start(void);

/** @brief Snapshot the rolling microphone ring as mono PCM-S16LE WAV. */
[[nodiscard]] ra8_err_t
c6_cam_audio_snapshot_wav(const uint8_t** out_wav, uint32_t* out_bytes, uint32_t* out_timestamp_ms);

/** @brief Bind NetX to an associated raw C6 link and obtain a DHCP lease. */
[[nodiscard]] ra8_err_t
c6_cam_net_up(ra8_c6link_t* link, const ra8_c6link_mac_t* mac, c6_cam_lease_t* out);

/** @brief Serve the browser UI and fresh JPEG frames forever. */
[[noreturn]] void c6_cam_http_serve(void);

/**
 * @brief Write a bounded NUL-terminated string to SCI8.
 * @details Measures at most the helper's fixed string bound before one board-console write.
 * @param[in] text NUL-terminated text to emit.
 * @pre `text` is nonnull and terminated within the fixed bound.
 * @pre The board console is initialized.
 * @post The bounded text bytes are queued for transmission.
 * @post Caller-owned text remains unchanged.
 * @note Console write failures are intentionally ignored by this diagnostic helper.
 * @since 0.1.0
 */
void c6_cam_puts(const char* text);

/**
 * @brief Write one unsigned decimal value to SCI8.
 * @details Formats the value into fixed stack storage without libc allocation.
 * @param[in] value Value to emit.
 * @pre The board console is initialized.
 * @pre Fixed helper stack storage is available.
 * @post The decimal representation is queued for transmission.
 * @post No application state except console output is modified.
 * @note The full `uint32_t` range is supported.
 * @since 0.1.0
 */
void c6_cam_put_u32(uint32_t value);

/**
 * @brief Write one IPv4 address to SCI8.
 * @details Formats the network-order address as four dotted decimal octets.
 * @param[in] ip IPv4 address in the representation used by the C6/NetX path.
 * @pre The board console is initialized.
 * @pre The address value came from a C6 or NetX network structure.
 * @post Four decimal octets and three separators are queued.
 * @post The address value remains unchanged.
 * @note This helper emits no trailing whitespace or newline.
 * @since 0.1.0
 */
void c6_cam_put_ip(uint32_t ip);
