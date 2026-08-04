/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/tz_nsc_cgc_usb/ns_usb_host.c
 * @brief Non-Secure USBHS polled HOST ladder: enumerate + bulk echo (#96).
 *
 * @par Tag
 * [Ring 6 / APP] {World: NS}
 *
 * @details
 * Split out of ns_usb.c for the 1000-line file-size cap. This TU owns the J7
 * USBHS HOST side of the self-loop: USBHS (J7) is wired host-side by the Secure
 * boot (PSARB12, host-mode expander, J7 VBUS, UTMI PLL). The ::ns_host_worker
 * enumerates the FS CDC device over the loop cable using the first-party
 * ra8_usb_host_* polled primitives -- no IRQ -- and then bulk round-trips a
 * deterministic pattern through the device's auto-echo. It runs at the SAME
 * ThreadX priority as the device dispatch worker (defined in ns_usb.c) with
 * time-slicing, so each thread yields the CPU each tick; the device's chapter-9
 * + auto-echo lands well inside the host primitives' ~10 ms polling windows.
 * Ported from usb_selftest_cdc, with the SCI console dropped (J-Link probes
 * report the verdict instead).
 *
 * The single ``tx_application_define`` that spawns this worker lives in ns_usb.c
 * and reaches ::ns_host_worker plus this TU's thread storage through
 * ns_usb_internal.h. The ThreadX-backed ::ra8_delay_ms / ::ra8_time_ms this ladder
 * uses are likewise defined in ns_usb.c (ra8_time.c is intentionally not linked).
 *
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ns_usb_internal.h"
#include "ra8_usb.h"
#include "tx_api.h"

/* =============================================================================
 * Host-side bench milestone counters (NS .bss; J-Link reads these)
 * =============================================================================
 */

/**
 * @var g_tz_usb_host_phase
 * @brief HS-host ladder phase: 0 boot, 1 host-init, 2 enumerating, 3 echoing,
 *        4 first full pass done.
 * @note Read externally by J-Link only.
 * @since 0.1.0
 */
volatile uint32_t g_tz_usb_host_phase;

/**
 * @var g_tz_usb_host_pid
 * @brief idProduct the HS host read from the looped FS device (expect 0x000A).
 * @note Read externally by J-Link only.
 * @since 0.1.0
 */
volatile uint32_t g_tz_usb_host_pid;

/**
 * @var g_tz_usb_host_rounds_ok
 * @brief Bulk echo rounds the HS host has verified byte-equal (advances
 *        forever once the loop is healthy -- the HIL gate probes this).
 * @note Read externally by J-Link only.
 * @since 0.1.0
 */
volatile uint32_t g_tz_usb_host_rounds_ok;

/**
 * @var g_tz_usb_host_err
 * @brief First non-OK ``ra8_err_t`` from the host ladder (0 = none yet).
 * @note Read externally by J-Link only.
 * @since 0.1.0
 */
volatile uint32_t g_tz_usb_host_err;

/* =============================================================================
 * HS host: polled CDC enumerate + bulk echo over the self-loop
 * =============================================================================
 */

/**
 * @var s_ns_host_thread
 * @brief ThreadX TCB for the HS host worker.
 * @note Single-writer (ThreadX in the NS image).
 * @since 0.1.0
 */
TX_THREAD s_ns_host_thread;

/**
 * @var s_ns_host_stack
 * @brief Stack backing storage for ::s_ns_host_thread.
 * @since 0.1.0
 */
UCHAR s_ns_host_stack[k_ns_host_stack_bytes];

/**
 * @var s_ns_host_thread_name
 * @brief HS host worker thread name (writable NS data; ThreadX name is CHAR*).
 * @since 0.1.0
 */
CHAR s_ns_host_thread_name[] = "ns_usb_host";

/** @brief Chapter-9 standard request / descriptor constants for the host. */
typedef enum : uint16_t {
  k_ns_bm_std_dev_in   = 0x80U, /**< bmRequestType: Std | Device | In.  */
  k_ns_bm_std_dev_out  = 0x00U, /**< bmRequestType: Std | Device | Out. */
  k_ns_breq_get_desc   = 0x06U, /**< GET_DESCRIPTOR.                    */
  k_ns_breq_set_addr   = 0x05U, /**< SET_ADDRESS.                       */
  k_ns_breq_set_config = 0x09U, /**< SET_CONFIGURATION.                 */
  k_ns_desc_device     = 0x01U, /**< DEVICE descriptor type.            */
  k_ns_dev_desc_len    = 18U,   /**< DEVICE descriptor length.          */
  k_ns_off_dev_pid     = 10U,   /**< idProduct LSB byte offset.         */
  k_ns_byte_bits       = 8U,    /**< Bits per byte.                     */
  k_ns_config_value    = 1U,    /**< bConfigurationValue to select.     */
} ns_host_req_t;

/** @brief Host enumeration timing / retry / geometry tunables. */
typedef enum : uint32_t {
  k_ns_vbus_settle_ms = 200U,      /**< VBUS settle before probing.          */
  k_ns_attach_to_ms   = 2000U,     /**< Wait for the D+ pull-up.             */
  k_ns_debounce_ms    = 500U,      /**< Post-attach debounce (>=100 ms).     */
  k_ns_reset_hold_ms  = 50U,       /**< USB bus-reset hold (>=10 ms).        */
  k_ns_recovery_ms    = 20U,       /**< Post-reset recovery (TRSTRCY).       */
  k_ns_addr_settle_ms = 5U,        /**< Post-SET_ADDRESS recovery.           */
  k_ns_enum_tries     = 8U,        /**< Reset+probe attempts.                */
  k_ns_attach_spin    = 50000000U, /**< Attach spin cap (frozen-tick guard). */
  k_ns_dev_addr       = 1U,        /**< Address the host assigns.            */
  k_ns_mps            = 64U,       /**< Bulk endpoint wMaxPacketSize (FS).   */
  k_ns_payload        = 60U,       /**< Bytes per echo round (sub-MPS).      */
  k_ns_echo_buf       = 64U,       /**< One-MPS host scratch buffer.         */
  k_ns_ep_in_num      = 1U,        /**< Device bulk-IN endpoint number.      */
  k_ns_ep_out_num     = 2U,        /**< Device bulk-OUT endpoint number.     */
  k_ns_host_pipe_in   = 1U,        /**< Host pipe for the device bulk-IN.    */
  k_ns_host_pipe_out  = 2U,        /**< Host pipe for the device bulk-OUT.   */
  k_ns_pat_round_mul  = 97U,       /**< Per-round pattern multiplier.        */
  k_ns_pat_idx_mul    = 7U,        /**< Per-index pattern multiplier.        */
  k_ns_pat_bias       = 0x5AU,     /**< Pattern constant bias.               */
  k_ns_byte_mask      = 0xFFU,     /**< Byte mask.                           */
  k_ns_boot_wait_tk   = 500U,      /**< Host start delay (ticks).            */
  k_ns_retry_tk       = 1000U,     /**< Pause between failed enum passes.    */
} ns_host_tune_t;

/**
 * @brief Fill an echo payload with this round's deterministic bytes.
 * @details Byte i = ``(round*97 + i*7 + 0x5A) & 0xFF`` -- distinct per round so
 *          the host proves it read back what it sent for that round.
 * @param[in]  round The echo round index.
 * @param[out] out   Destination buffer.
 * @param[in]  len   Bytes to fill.
 * @return void.
 * @pre @p out has @p len writable bytes.
 * @pre @p len is at most ::k_ns_payload.
 * @post @p out[0..len-1] hold the round's pattern bytes.
 * @post No global state changes.
 * @note Pure function.
 * @since 0.1.0
 */
static void ns_host_pattern_fill(uint32_t round, uint8_t* out, uint32_t len)
{
  for (uint32_t i = 0U; i < len; i++) {
    const uint32_t v = (round * (uint32_t)k_ns_pat_round_mul) + (i * (uint32_t)k_ns_pat_idx_mul) +
                       (uint32_t)k_ns_pat_bias;
    out[i]           = (uint8_t)(v & (uint32_t)k_ns_byte_mask);
  }
}

/**
 * @brief GET_DESCRIPTOR(DEVICE) over the polled host control engine.
 * @param[out] desc Receives the 18-byte device descriptor.
 * @return Read outcome.
 * @retval k_ra8_ok           All 18 bytes arrived.
 * @retval k_ra8_err_hw_error A short descriptor came back.
 * @pre The bus is reset and the DCP targets the device's current address.
 * @pre @p desc holds at least ::k_ns_dev_desc_len bytes.
 * @post @p desc carries the device descriptor on success.
 * @post No global state changes.
 * @note Blocking (polled control transfer).
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t ns_host_get_dev_desc(uint8_t* desc)
{
  const ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_ns_bm_std_dev_in,
    .b_request       = (uint8_t)k_ns_breq_get_desc,
    .w_value         = (uint16_t)((uint16_t)k_ns_desc_device << (uint16_t)k_ns_byte_bits),
    .w_index         = 0U,
    .w_length        = (uint16_t)k_ns_dev_desc_len,
  };
  uint16_t        rx = 0U;
  const ra8_err_t err =
    ra8_usb_host_control_xfer(k_ra8_usb_speed_hs, &setup, desc, (uint16_t)k_ns_dev_desc_len, &rx);
  if (err != k_ra8_ok) {
    return err;
  }
  return (rx == (uint16_t)k_ns_dev_desc_len) ? k_ra8_ok : k_ra8_err_hw_error;
}

/**
 * @brief Wait for attach, then bus-reset + read the device descriptor.
 * @param[out] desc Receives the winning 18-byte device descriptor.
 * @return Hunt outcome.
 * @retval k_ra8_ok             The device answered at address 0.
 * @retval k_ra8_err_hw_timeout Nothing attached / nothing answered.
 * @pre ::ra8_usb_host_init ran (host up, J7 VBUS supplied).
 * @pre The ThreadX 1 ms tick is live (ms delays / timeout).
 * @post On success the DCP targets address 0 with UACT on.
 * @post On failure the bus is left in the last attempt's state.
 * @note Blocking; worst case a few seconds.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t ns_host_enum_hunt(uint8_t* desc)
{
  ra8_delay_ms((uint32_t)k_ns_vbus_settle_ms);
  const uint32_t t0 = ra8_time_ms();
  for (uint32_t spin = 0U; spin < (uint32_t)k_ns_attach_spin; spin++) {
    if (ra8_usb_host_line_state(k_ra8_usb_speed_hs) != 0U) {
      break;
    }
    if ((ra8_time_ms() - t0) > (uint32_t)k_ns_attach_to_ms) {
      break;
    }
  }
  ra8_delay_ms((uint32_t)k_ns_debounce_ms);
  ra8_err_t err = k_ra8_err_hw_timeout;
  for (uint8_t attempt = 0U; attempt < (uint8_t)k_ns_enum_tries; attempt++) {
    (void)ra8_usb_host_bus_reset(k_ra8_usb_speed_hs, true);
    ra8_delay_ms((uint32_t)k_ns_reset_hold_ms);
    (void)ra8_usb_host_bus_reset(k_ra8_usb_speed_hs, false);
    (void)ra8_usb_host_set_uact(k_ra8_usb_speed_hs, true);
    ra8_delay_ms((uint32_t)k_ns_recovery_ms);
    (void)ra8_usb_host_set_target(k_ra8_usb_speed_hs, 0U);
    err = ns_host_get_dev_desc(desc);
    if (err == k_ra8_ok) {
      return k_ra8_ok;
    }
  }
  return err;
}

/**
 * @brief SET_ADDRESS to ::k_ns_dev_addr, then retarget the DCP.
 * @return First failing step's error, or k_ra8_ok.
 * @retval k_ra8_ok The DCP now targets the operating address.
 * @pre ::ns_host_enum_hunt succeeded (device answering at address 0).
 * @pre The bus is active (UACT on).
 * @post Later transfers carry tokens to ::k_ns_dev_addr.
 * @post The set-address recovery delay has elapsed.
 * @note Blocking (one control transfer + settle).
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t ns_host_set_address(void)
{
  const ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_ns_bm_std_dev_out,
    .b_request       = (uint8_t)k_ns_breq_set_addr,
    .w_value         = (uint16_t)k_ns_dev_addr,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  const ra8_err_t err = ra8_usb_host_control_xfer(k_ra8_usb_speed_hs, &setup, nullptr, 0U, nullptr);
  if (err != k_ra8_ok) {
    return err;
  }
  ra8_delay_ms((uint32_t)k_ns_addr_settle_ms);
  return ra8_usb_host_set_target(k_ra8_usb_speed_hs, (uint8_t)k_ns_dev_addr);
}

/**
 * @brief SET_CONFIGURATION(::k_ns_config_value) on the addressed device.
 * @return Control-transfer outcome.
 * @retval k_ra8_ok The device entered the Configured state.
 * @pre ::ns_host_set_address succeeded.
 * @pre The DCP targets ::k_ns_dev_addr.
 * @post On success the device's endpoints are usable.
 * @post No global state changes.
 * @note Blocking (one control transfer).
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t ns_host_set_config(void)
{
  const ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_ns_bm_std_dev_out,
    .b_request       = (uint8_t)k_ns_breq_set_config,
    .w_value         = (uint16_t)k_ns_config_value,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  return ra8_usb_host_control_xfer(k_ra8_usb_speed_hs, &setup, nullptr, 0U, nullptr);
}

/**
 * @brief Open the host bulk pipes for the device's CDC data endpoints.
 * @return First failing pipe-setup error, or k_ra8_ok.
 * @retval k_ra8_ok Both bulk pipes configured.
 * @pre ::ns_host_set_config succeeded.
 * @pre The pipes are not currently armed.
 * @post Pipe OUT -> device EP2 OUT, pipe IN -> device EP1 IN, both at MPS.
 * @post The pipes target ::k_ns_dev_addr.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t ns_host_open_pipes(void)
{
  const ra8_err_t err = ra8_usb_host_pipe_setup(k_ra8_usb_speed_hs,
                                                (uint8_t)k_ns_host_pipe_out,
                                                (uint8_t)k_ns_dev_addr,
                                                (uint8_t)k_ns_ep_out_num,
                                                false,
                                                (uint16_t)k_ns_mps);
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_usb_host_pipe_setup(k_ra8_usb_speed_hs,
                                 (uint8_t)k_ns_host_pipe_in,
                                 (uint8_t)k_ns_dev_addr,
                                 (uint8_t)k_ns_ep_in_num,
                                 true,
                                 (uint16_t)k_ns_mps);
}

/**
 * @brief Full enumeration ladder: hunt, SET_ADDRESS, SET_CONFIG, open pipes.
 * @param[out] out_pid Receives the device idProduct on success.
 * @return First failing step's error, or k_ra8_ok.
 * @retval k_ra8_ok Device enumerated; bulk pipes open.
 * @pre ::ra8_usb_host_init succeeded on this pass.
 * @pre @p out_pid is non-NULL.
 * @post @p out_pid holds the device idProduct on success.
 * @post On failure the bus is left mid-ladder for the caller to deinit.
 * @note Blocking; runs on the host worker thread.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t ns_host_enumerate(uint32_t* out_pid)
{
  uint8_t   desc[k_ns_dev_desc_len] = {};
  ra8_err_t err                     = ns_host_enum_hunt(desc);
  if (err != k_ra8_ok) {
    return err;
  }
  *out_pid = (uint32_t)desc[k_ns_off_dev_pid] |
             ((uint32_t)desc[(uint32_t)k_ns_off_dev_pid + 1U] << (uint32_t)k_ns_byte_bits);
  err      = ns_host_set_address();
  if (err != k_ra8_ok) {
    return err;
  }
  err = ns_host_set_config();
  if (err != k_ra8_ok) {
    return err;
  }
  return ns_host_open_pipes();
}

/**
 * @brief One echo round: bulk-OUT a pattern, bulk-IN the echo, compare.
 * @param[in] round The echo round index (pattern key).
 * @return ra8_err_t verdict.
 * @retval k_ra8_ok               The echo matched the sent payload.
 * @retval k_ra8_err_invalid_size The echo length differed.
 * @retval k_ra8_err_invalid_state The echo bytes differed.
 * @pre The bulk pipes were opened by ::ns_host_open_pipes.
 * @pre The device dispatch worker is auto-echoing.
 * @post Nothing is retained between rounds.
 * @post On a mismatch the caller records ::g_tz_usb_host_err.
 * @note Blocking; one bulk-OUT then one bulk-IN over the self-loop.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t ns_host_echo_round(uint32_t round)
{
  static uint8_t s_tx[k_ns_payload]  = {};
  static uint8_t s_rx[k_ns_echo_buf] = {};
  ns_host_pattern_fill(round, s_tx, (uint32_t)k_ns_payload);
  ra8_err_t err = ra8_usb_host_bulk_out(k_ra8_usb_speed_hs,
                                        (uint8_t)k_ns_host_pipe_out,
                                        s_tx,
                                        (uint16_t)k_ns_payload);
  if (err != k_ra8_ok) {
    return err;
  }
  uint16_t rx = 0U;
  err         = ra8_usb_host_bulk_in(k_ra8_usb_speed_hs,
                                     (uint8_t)k_ns_host_pipe_in,
                                     s_rx,
                                     (uint16_t)k_ns_echo_buf,
                                     &rx);
  if (err != k_ra8_ok) {
    return err;
  }
  if (rx != (uint16_t)k_ns_payload) {
    return k_ra8_err_invalid_size;
  }
  if (memcmp(s_rx, s_tx, (size_t)k_ns_payload) != 0) {
    return k_ra8_err_invalid_state;
  }
  return k_ra8_ok;
}

VOID ns_host_worker(ULONG arg)
{
  (void)arg;
  (void)tx_thread_sleep((ULONG)k_ns_boot_wait_tk);

  uint32_t pid = 0U;
  while (1) {
    g_tz_usb_host_phase = 1U;
    ra8_err_t err       = ra8_usb_host_init(k_ra8_usb_speed_hs);
    if (err != k_ra8_ok) {
      g_tz_usb_host_err = (uint32_t)err;
      (void)tx_thread_sleep((ULONG)k_ns_retry_tk);
      continue;
    }
    g_tz_usb_host_phase = 2U;
    err                 = ns_host_enumerate(&pid);
    if (err != k_ra8_ok) {
      g_tz_usb_host_err = (uint32_t)err;
      (void)ra8_usb_host_deinit(k_ra8_usb_speed_hs);
      (void)tx_thread_sleep((ULONG)k_ns_retry_tk);
      continue;
    }
    g_tz_usb_host_pid   = pid;
    g_tz_usb_host_phase = 3U;
    break;
  }

  /* Enumerated. Run bulk echo rounds forever; rounds_ok advancing is the
   * end-to-end self-loop proof (host bulk-OUT -> device auto-echo -> host
   * bulk-IN, byte-checked). */
  g_tz_usb_host_phase = 4U;
  uint32_t round      = 0U;
  while (1) {
    const ra8_err_t err = ns_host_echo_round(round);
    if (err == k_ra8_ok) {
      g_tz_usb_host_rounds_ok += 1U;
    } else if (g_tz_usb_host_err == 0U) {
      g_tz_usb_host_err = (uint32_t)err;
    }
    round += 1U;
  }
}
