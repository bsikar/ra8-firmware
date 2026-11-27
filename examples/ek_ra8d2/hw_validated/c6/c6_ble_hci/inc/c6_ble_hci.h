/**
 * @file examples/ek_ra8d2/hw_validated/c6/c6_ble_hci/inc/c6_ble_hci.h
 * @brief Contract, bounds, and core runner for the C6 BLE HCI example.
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

/**
 * @enum c6_ble_cfg_t
 * @brief Link, ThreadX, and polling parameters owned by `c6_ble_hci`.
 * @since 0.2.0
 */
typedef enum : uint32_t {
  k_c6_ble_uart_baud       = 115200U,  /**< Console baud rate over J-Link VCOM.   */
  k_c6_ble_sck_hz          = 5000000U, /**< SPI clock matching proven C6 rate.    */
  k_c6_ble_edge_poll_ms    = 2U,       /**< Side-band handshake poll period (ms). */
  k_c6_ble_boot_wait_ms    = 200U,     /**< Initial settle delay after port init. */
  k_c6_ble_cmd_poll_ms     = 5U,       /**< Delay between HCI dispatch polls.     */
  k_c6_ble_cmd_poll_budget = 200U,     /**< Max polls per HCI Command Complete.   */
  k_c6_ble_heartbeat_ms    = 5000U,    /**< Terminal heartbeat period (ms).       */
  k_c6_ble_worker_stack    = 8192U,    /**< ThreadX worker stack size in bytes.   */
  k_c6_ble_worker_prio     = 8U,       /**< ThreadX worker thread priority.       */
  k_c6_ble_arena_bytes     = 4096U,    /**< Protobuf decode arena for C6 link.    */
} c6_ble_cfg_t;

/**
 * @enum c6_ble_fmt_t
 * @brief Bounded widths for console formatting in `c6_ble_console.c`.
 * @since 0.2.0
 */
typedef enum : uint16_t {
  k_c6_ble_str_max       = 256U, /**< Longest console string emitted per call. */
  k_c6_ble_dec_radix     = 10U,  /**< Base-10 formatter radix.                 */
  k_c6_ble_dec_digits    = 10U,  /**< Decimal digits in widest u32 value.      */
  k_c6_ble_hex_digits    = 8U,   /**< Hex digits in widest u32 value.          */
  k_c6_ble_hex_bits      = 4U,   /**< Bits per hex nibble.                     */
  k_c6_ble_hex_mask      = 0x0FU,
  k_c6_ble_hex_alpha     = 10U,
  k_c6_ble_hex_byte      = 2U,  /**< Hex digits per printed byte.             */
  k_c6_ble_text_max      = 32U, /**< Maximum sanitized co-processor text len. */
  k_c6_ble_evt_cap_bytes = 64U, /**< Maximum Command Complete return bytes.   */
} c6_ble_fmt_t;

/**
 * @enum c6_ble_hci_op_t
 * @brief Standard Bluetooth Core HCI opcodes and event codes exercised here.
 * @since 0.2.0
 */
typedef enum : uint16_t {
  k_c6_ble_op_reset          = 0x0C03U, /**< HCI_Reset (Vol 4 Part E 7.3.2).         */
  k_c6_ble_op_read_bd_addr   = 0x1009U, /**< HCI_Read_BD_ADDR (Vol 4 Part E 7.4.6).  */
  k_c6_ble_op_le_adv_params  = 0x2006U, /**< LE_Set_Advertising_Parameters (7.8.5).  */
  k_c6_ble_op_le_adv_data    = 0x2008U, /**< LE_Set_Advertising_Data (7.8.7).        */
  k_c6_ble_op_le_adv_enable  = 0x200AU, /**< LE_Set_Advertising_Enable (7.8.9).      */
  k_c6_ble_evt_cmd_complete  = 0x000EU, /**< Command Complete event code (7.7.14).   */
  k_c6_ble_adv_interval_slot = 0x00A0U, /**< 100 ms advertising interval (0.625 ms). */
} c6_ble_hci_op_t;

/**
 * @struct c6_ble_result
 * @brief Observable outcome of one ::c6_ble_hci_run journey.
 * @since 0.2.0
 */
typedef struct c6_ble_result {
  bool                    link_opened;
  bool                    ready_ok;
  bool                    ble_opened;
  bool                    hci_bound;
  bool                    reset_ok;
  bool                    bd_addr_ok;
  bool                    adv_params_ok;
  bool                    adv_data_ok;
  bool                    adv_enable_ok;
  bool                    passed;
  ra8_c6link_fw_version_t fw;
  uint8_t                 bd_addr[k_ra8_ble_addr_bytes];
  ra8_err_t               open_err;
  ra8_err_t               ready_err;
  ra8_err_t               cmd_err;
  uint8_t                 hci_status;
  uint32_t                events_seen;
} c6_ble_result_t;

/** @brief Optional idle delay callback used between SPI dispatch polls. */
typedef void (*c6_ble_wait_fn_t)(void* ctx, uint32_t ms);

/**
 * @struct c6_ble_run_cfg
 * @brief Injected transport, storage, and pacing config for ::c6_ble_hci_run.
 * @since 0.2.0
 */
typedef struct c6_ble_run_cfg {
  ra8_c6link_t*          link;
  ra8_c6link_transport_t transport;
  uint8_t*               arena;
  uint32_t               arena_bytes;
  c6_ble_wait_fn_t       wait_ms;
  void*                  wait_ctx;
  uint16_t               poll_budget;
} c6_ble_run_cfg_t;

/**
 * @brief Execute HCI Reset, Read_BD_ADDR, and BLE advertising setup over C6 SPI.
 * @param[in]  cfg Caller-supplied link storage, transport, and pacing config.
 * @param[out] out Populated journey outcome across every stage.
 * @return true when all HCI stages received Command Complete with status 0x00.
 * @since 0.2.0
 */
[[nodiscard]] bool c6_ble_hci_run(const c6_ble_run_cfg_t* cfg, c6_ble_result_t* out);

void c6_ble_puts(const char* text);
void c6_ble_put_u32(uint32_t value);
void c6_ble_put_hex(uint32_t value, uint8_t digits);
void c6_ble_put_text(const uint8_t* text, size_t len);
void c6_ble_put_bd_addr(const uint8_t bd_addr[k_ra8_ble_addr_bytes]);
void c6_ble_print_banner(uint32_t cpuclk_hz, uint32_t pclka_hz);
