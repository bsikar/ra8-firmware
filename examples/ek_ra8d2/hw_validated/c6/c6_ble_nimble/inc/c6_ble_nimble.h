/**
 * @file examples/ek_ra8d2/hw_validated/c6/c6_ble_nimble/inc/c6_ble_nimble.h
 * @brief Contract, configuration, and testable NimBLE peripheral core over C6 HCI.
 * @ingroup grp_hal_system
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ra8_ble.h"
#include "ra8_c6link.h"
#include "ra8_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum c6_ble_nimble_cfg_t
 * @brief Link, ThreadX, and GAP profile defaults for `c6_ble_nimble`.
 * @since 0.2.0
 */
typedef enum : uint32_t {
  k_c6_ble_nimble_uart_baud     = 115200U,  /**< Console baud rate over J-Link VCOM.   */
  k_c6_ble_nimble_sck_hz        = 5000000U, /**< SPI clock matching proven C6 rate.    */
  k_c6_ble_nimble_edge_poll_ms  = 2U,       /**< Side-band handshake poll period (ms). */
  k_c6_ble_nimble_boot_wait_ms  = 200U,     /**< Initial settle delay after port init. */
  k_c6_ble_nimble_pump_sleep_ms = 2U,       /**< HCI dispatch thread cadence (ms).     */
  k_c6_ble_nimble_heartbeat_ms  = 5000U,    /**< Console heartbeat period (ms).        */
  k_c6_ble_nimble_host_stack    = 8192U,    /**< NimBLE host ThreadX stack (bytes).    */
  k_c6_ble_nimble_pump_stack    = 8192U,    /**< HCI dispatch ThreadX stack (bytes).   */
  k_c6_ble_nimble_beat_stack    = 4096U,    /**< Heartbeat ThreadX stack (bytes).      */
  k_c6_ble_nimble_pump_prio     = 7U,       /**< Pump priority (higher wakes host).    */
  k_c6_ble_nimble_host_prio     = 8U,       /**< NimBLE host priority.                 */
  k_c6_ble_nimble_beat_prio     = 10U,      /**< Periodic heartbeat priority.          */
  k_c6_ble_nimble_arena_bytes   = 4096U,    /**< Protobuf decode arena for C6 link.    */
  k_c6_ble_nimble_battery_init  = 85U,      /**< Initial Battery Level (%).            */
  k_c6_ble_nimble_bas_uuid16    = 0x180FU,  /**< Assigned Battery Service UUID16.      */
} c6_ble_nimble_cfg_t;

/**
 * @enum c6_ble_nimble_fmt_t
 * @brief Bounded widths for console formatting in `c6_ble_nimble_console.c`.
 * @since 0.2.0
 */
typedef enum : uint16_t {
  k_c6_ble_nimble_str_max    = 256U, /**< Longest console string emitted per call. */
  k_c6_ble_nimble_dec_radix  = 10U,  /**< Base-10 formatter radix.                 */
  k_c6_ble_nimble_dec_digits = 10U,  /**< Decimal digits in widest u32 value.      */
  k_c6_ble_nimble_hex_digits = 8U,   /**< Hex digits in widest u32 value.          */
  k_c6_ble_nimble_hex_bits   = 4U,   /**< Bits per hex nibble.                     */
  k_c6_ble_nimble_hex_mask   = 0x0FU,
  k_c6_ble_nimble_hex_alpha  = 10U,
  k_c6_ble_nimble_hex_byte   = 2U, /**< Hex digits per printed byte. */
  k_c6_ble_nimble_name_max   = 31U,
} c6_ble_nimble_fmt_t;

/** @brief Event callback kind emitted by the peripheral state machine. */
typedef enum : uint8_t {
  k_c6_ble_nimble_evt_sync_ok    = 0U,
  k_c6_ble_nimble_evt_adv_ok     = 1U,
  k_c6_ble_nimble_evt_adv_fail   = 2U,
  k_c6_ble_nimble_evt_connected  = 3U,
  k_c6_ble_nimble_evt_conn_fail  = 4U,
  k_c6_ble_nimble_evt_disconn    = 5U,
  k_c6_ble_nimble_evt_subscribe  = 6U,
  k_c6_ble_nimble_evt_mtu        = 7U,
  k_c6_ble_nimble_evt_host_reset = 8U,
} c6_ble_nimble_evt_t;

/**
 * @struct c6_ble_nimble_state
 * @brief Observable state and counters of the NimBLE peripheral core.
 * @since 0.2.0
 */
typedef struct c6_ble_nimble_state {
  bool                    link_opened;
  bool                    ready_ok;
  bool                    ble_opened;
  bool                    c6_bound;
  bool                    hs_bound;
  bool                    synced;
  bool                    advertising;
  bool                    connected;
  uint8_t                 own_addr_type;
  uint8_t                 battery_level;
  uint8_t                 bd_addr[k_ra8_ble_addr_bytes];
  uint8_t                 peer_addr[k_ra8_ble_addr_bytes];
  uint16_t                conn_handle;
  uint16_t                mtu;
  uint16_t                subscribe_attr_handle;
  int32_t                 last_status;
  int32_t                 reset_reason;
  uint32_t                sync_count;
  uint32_t                adv_starts;
  uint32_t                connect_count;
  uint32_t                disconnect_count;
  uint32_t                subscribe_count;
  ra8_c6link_fw_version_t fw;
  ra8_err_t               open_err;
  ra8_err_t               ready_err;
  ra8_err_t               ble_err;
} c6_ble_nimble_state_t;

/** @brief Optional notification hook invoked on GAP / host lifecycle transitions. */
typedef void (*c6_ble_nimble_notify_fn_t)(void*                        ctx,
                                          c6_ble_nimble_evt_t          evt,
                                          const c6_ble_nimble_state_t* state);

/**
 * @struct c6_ble_nimble_link_cfg
 * @brief Caller-provided storage and transport seam for ::c6_ble_nimble_bind_link.
 * @since 0.2.0
 */
typedef struct c6_ble_nimble_link_cfg {
  ra8_c6link_t*          link;
  ra8_c6link_transport_t transport;
  uint8_t*               arena;
  uint32_t               arena_bytes;
} c6_ble_nimble_link_cfg_t;

/**
 * @struct c6_ble_nimble_app_cfg
 * @brief GAP device name and lifecycle observer for ::c6_ble_nimble_stack_init.
 * @since 0.2.0
 */
typedef struct c6_ble_nimble_app_cfg {
  const char*               device_name;
  uint8_t                   initial_battery_pct;
  c6_ble_nimble_notify_fn_t notify_cb;
  void*                     notify_ctx;
} c6_ble_nimble_app_cfg_t;

/**
 * @brief Open `ra8_c6link`, await C6 ready, open `ra8_ble`, and bind `ble_hci_c6link` +
 *        `ble_hci_ra8_ble`.
 * @param[in]  cfg Transport/link configuration; non-null.
 * @return true when C6 HCI transport is live and bound to NimBLE adapter.
 * @since 0.2.0
 */
[[nodiscard]] bool c6_ble_nimble_bind_link(const c6_ble_nimble_link_cfg_t* cfg);

/**
 * @brief Initialize NimBLE port, GAP/GATT/BAS services, RAM store, and sync callback.
 * @param[in] cfg Peripheral GAP name, battery reading, and optional observer; non-null.
 * @return true on clean setup before `nimble_port_run()`.
 * @since 0.2.0
 */
[[nodiscard]] bool c6_ble_nimble_stack_init(const c6_ble_nimble_app_cfg_t* cfg);

/**
 * @brief Return a snapshot of the peripheral's current state and counters.
 * @param[out] out Snapshot destination; non-null.
 * @since 0.2.0
 */
void c6_ble_nimble_get_state(c6_ble_nimble_state_t* out);

/**
 * @brief Reset module state and unbind NimBLE/C6 transport seams cleanly.
 * @since 0.2.0
 */
void c6_ble_nimble_reset_state(void);

void c6_ble_nimble_console_lock_init(void);
void c6_ble_nimble_console_lock(void);
void c6_ble_nimble_console_unlock(void);
void c6_ble_nimble_puts(const char* text);
void c6_ble_nimble_put_u32(uint32_t value);
void c6_ble_nimble_put_i32(int32_t value);
void c6_ble_nimble_put_hex(uint32_t value, uint8_t digits);
void c6_ble_nimble_put_bd_addr_le(const uint8_t bd_addr[k_ra8_ble_addr_bytes]);
void c6_ble_nimble_print_banner(uint32_t cpuclk_hz, uint32_t pclka_hz);

#ifdef __cplusplus
}
#endif
