/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hw_validated/hil/usb_selftest_hid/src/usb_selftest_hid_host.c
 * @brief USB HID self-loop host side: polled enumerate + interrupt-IN read
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The host-side cluster split out of the `usb_selftest_hid` main translation
 * unit. A self-contained polled USB host built on the first-party
 * `ra8_usb_host_*` primitives: it waits for the looped FS device to attach,
 * drives the chapter-9 enumeration ladder (bus reset -> GET_DESCRIPTOR ->
 * SET_ADDRESS -> SET_CONFIGURATION), opens the HID interrupt-IN endpoint
 * (EP1 IN) as a receive pipe, then polls several reports, byte-checking the
 * fixed pattern in each. ::hid_host_worker retries the full pass until every
 * report round verifies.
 *
 * The host worker entry is declared in `usb_selftest_hid_steps.h`; the
 * device side, the ThreadX workers' creation, and startup live in `main.c`.
 *
 * @author Brighton Sikarskie
 * @date 2026-06-13
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_err.h"
#include "ra8_time.h"
#include "ra8_usb.h"
#include "usb_selftest_hid_steps.h"

#ifndef RA8_OFF_TARGET

#include "tx_api.h"

/**
 * @enum hid_phase_t
 * @brief J-Link probe values marking host-ladder progress.
 */
typedef enum : uint32_t {
  k_hid_phase_boot   = 0U, /**< Host thread not started.    */
  k_hid_phase_init   = 1U, /**< Host controller init.       */
  k_hid_phase_enum   = 2U, /**< Enumerating.                */
  k_hid_phase_verify = 3U, /**< Reading + checking reports. */
  k_hid_phase_pass   = 4U, /**< All reports verified.       */
} hid_phase_t;

/* -------------------------------------------------------------------------- */
/* J-Link probes (host side) */
/* -------------------------------------------------------------------------- */

/** @brief Host-ladder phase marker (::hid_phase_t). */
static volatile uint32_t s_dbg_phase;
/** @brief Reports the host confirmed pattern-equal (expect ::k_hid_rounds). */
static volatile uint32_t s_dbg_rounds_ok;
/** @brief Device-reported product id captured at enumeration. */
static volatile uint32_t s_dbg_pid;
/** @brief First mismatching report round, or ::k_hid_no_mismatch. */
static volatile uint32_t s_dbg_mismatch = (uint32_t)k_hid_no_mismatch;
/** @brief Completed full passes (sticky success counter). */
static volatile uint32_t s_dbg_pass_count;
/** @brief Seq byte of the most recent report the host read back. */
static volatile uint32_t s_dbg_last_seq;

/* -------------------------------------------------------------------------- */
/* Host side: self-contained polled enumerate + HID interrupt-IN read */
/* -------------------------------------------------------------------------- */

/**
 * @enum hid_usb_req_t
 * @brief Standard chapter-9 request / descriptor constants for the host.
 */
typedef enum : uint16_t {
  k_hid_bm_std_dev_in   = 0x80U, /**< bmRequestType: Std | Device | In.  */
  k_hid_bm_std_dev_out  = 0x00U, /**< bmRequestType: Std | Device | Out. */
  k_hid_breq_get_desc   = 0x06U, /**< GET_DESCRIPTOR.                    */
  k_hid_breq_set_addr   = 0x05U, /**< SET_ADDRESS.                       */
  k_hid_breq_set_config = 0x09U, /**< SET_CONFIGURATION.                 */
  k_hid_desc_device     = 0x01U, /**< DEVICE descriptor type.            */
  k_hid_dev_desc_len    = 18U,   /**< DEVICE descriptor length.          */
  k_hid_off_dev_pid     = 10U,   /**< idProduct LSB byte offset.         */
  k_hid_byte_bits       = 8U,    /**< Bits per byte.                     */
  k_hid_config_value    = 1U,    /**< bConfigurationValue to select.     */
} hid_usb_req_t;

/**
 * @enum hid_enum_tune_t
 * @brief Timing / retry tunables for the polled enumeration ladder.
 */
typedef enum : uint32_t {
  k_hid_vbus_settle_ms = 200U,      /**< VBUS settle before probing.          */
  k_hid_attach_to_ms   = 2000U,     /**< Wait for the D+ pull-up.             */
  k_hid_debounce_ms    = 500U,      /**< Post-attach debounce (>=100 ms).     */
  k_hid_reset_hold_ms  = 50U,       /**< USB bus-reset hold (>=10 ms).        */
  k_hid_recovery_ms    = 20U,       /**< Post-reset recovery (TRSTRCY).       */
  k_hid_addr_settle_ms = 5U,        /**< Post-SET_ADDRESS recovery.           */
  k_hid_enum_tries     = 8U,        /**< Reset+probe attempts.                */
  k_hid_attach_spin    = 50000000U, /**< Attach spin cap (frozen-tick guard). */
} hid_enum_tune_t;

/**
 * @brief GET_DESCRIPTOR(DEVICE) over the polled control engine.
 *
 * @param[out] desc Receives the 18-byte device descriptor.
 *
 * @return Read outcome.
 * @retval k_ra8_ok           All 18 bytes arrived.
 * @retval k_ra8_err_hw_error A short descriptor came back.
 *
 * @pre The bus is reset and the DCP targets the device's current address.
 * @pre @p desc holds at least ::k_hid_dev_desc_len bytes.
 * @post @p desc carries the device descriptor on success.
 * @post No global state changes.
 *
 * @note Blocking (polled control transfer).
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t hid_ctrl_get_dev_desc(uint8_t* desc)
{
  const ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_hid_bm_std_dev_in,
    .b_request       = (uint8_t)k_hid_breq_get_desc,
    .w_value         = (uint16_t)((uint16_t)k_hid_desc_device << (uint16_t)k_hid_byte_bits),
    .w_index         = 0U,
    .w_length        = (uint16_t)k_hid_dev_desc_len,
  };
  uint16_t        rx = 0U;
  const ra8_err_t err =
    ra8_usb_host_control_xfer(k_ra8_usb_speed_hs, &setup, desc, (uint16_t)k_hid_dev_desc_len, &rx);
  if (err != k_ra8_ok) {
    return err;
  }
  if (rx != (uint16_t)k_hid_dev_desc_len) {
    return k_ra8_err_hw_error;
  }
  return k_ra8_ok;
}

/**
 * @brief Wait for the device to attach, then bus-reset + read its descriptor.
 *
 * @details Waits for the D+ pull-up (LNST leaves SE0) plus the spec
 * debounce, then each attempt drives a full bus reset (returns the device
 * to address 0), re-enables SOF, targets address 0, and reads the device
 * descriptor. The first attempt that returns all 18 bytes wins.
 *
 * @param[out] desc Receives the winning 18-byte device descriptor.
 *
 * @return Hunt outcome.
 * @retval k_ra8_ok             The device answered at address 0.
 * @retval k_ra8_err_hw_timeout Nothing attached / nothing answered.
 *
 * @pre ::ra8_usb_host_init ran (host mode up, VBUS supplied).
 * @pre ::ra8_time_init has run (ms delays).
 * @post On success the DCP targets address 0 with UACT on.
 * @post On failure the bus is left in the last attempt's state.
 *
 * @note Blocking; worst case a few seconds.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t hid_enum_hunt(uint8_t* desc)
{
  ra8_delay_ms(k_hid_vbus_settle_ms);
  const uint32_t t0 = ra8_time_ms();
  for (uint32_t spin = 0U; spin < (uint32_t)k_hid_attach_spin; spin++) {
    if (ra8_usb_host_line_state(k_ra8_usb_speed_hs) != 0U) {
      break;
    }
    if ((ra8_time_ms() - t0) > (uint32_t)k_hid_attach_to_ms) {
      break;
    }
  }
  ra8_delay_ms(k_hid_debounce_ms);
  ra8_err_t err = k_ra8_err_hw_timeout;
  for (uint8_t attempt = 0U; attempt < (uint8_t)k_hid_enum_tries; attempt++) {
    (void)ra8_usb_host_bus_reset(k_ra8_usb_speed_hs, true);
    ra8_delay_ms(k_hid_reset_hold_ms);
    (void)ra8_usb_host_bus_reset(k_ra8_usb_speed_hs, false);
    (void)ra8_usb_host_set_uact(k_ra8_usb_speed_hs, true);
    ra8_delay_ms(k_hid_recovery_ms);
    (void)ra8_usb_host_set_target(k_ra8_usb_speed_hs, 0U);
    err = hid_ctrl_get_dev_desc(desc);
    if (err == k_ra8_ok) {
      return k_ra8_ok;
    }
  }
  return err;
}

/**
 * @brief SET_ADDRESS to ::k_hid_dev_addr, then retarget the DCP.
 *
 * @return First failing step's error, or k_ra8_ok.
 * @retval k_ra8_ok The DCP now targets the operating address.
 *
 * @pre ::hid_enum_hunt succeeded (device answering at address 0).
 * @pre The bus is active (UACT on).
 * @post Later transfers carry tokens to ::k_hid_dev_addr.
 * @post The set-address recovery delay has elapsed.
 *
 * @note Blocking (one control transfer + settle).
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t hid_enum_set_address(void)
{
  const ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_hid_bm_std_dev_out,
    .b_request       = (uint8_t)k_hid_breq_set_addr,
    .w_value         = (uint16_t)k_hid_dev_addr,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  ra8_err_t err = ra8_usb_host_control_xfer(k_ra8_usb_speed_hs, &setup, nullptr, 0U, nullptr);
  if (err != k_ra8_ok) {
    return err;
  }
  ra8_delay_ms(k_hid_addr_settle_ms);
  return ra8_usb_host_set_target(k_ra8_usb_speed_hs, (uint8_t)k_hid_dev_addr);
}

/**
 * @brief SET_CONFIGURATION(::k_hid_config_value) on the addressed device.
 *
 * @return Control-transfer outcome.
 * @retval k_ra8_ok The device entered the Configured state.
 *
 * @pre ::hid_enum_set_address succeeded.
 * @pre The DCP targets ::k_hid_dev_addr.
 * @post On success the device's endpoints are usable.
 * @post No global state changes.
 *
 * @note Blocking (one control transfer).
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t hid_enum_set_config(void)
{
  const ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_hid_bm_std_dev_out,
    .b_request       = (uint8_t)k_hid_breq_set_config,
    .w_value         = (uint16_t)k_hid_config_value,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  return ra8_usb_host_control_xfer(k_ra8_usb_speed_hs, &setup, nullptr, 0U, nullptr);
}

/**
 * @brief Open the host receive pipe for the device's HID interrupt-IN endpoint.
 *
 * @details Pipe ::k_hid_pipe_in -> device EP1 IN (host receives), 64-byte
 * MPS. The endpoint is interrupt on the device; the host SIE drives it as a
 * receive pipe (issues IN tokens on demand), which is sufficient for the
 * polled report read. HID is input-only, so there is no OUT pipe.
 *
 * @return The pipe-setup error, or k_ra8_ok.
 * @retval k_ra8_ok The IN pipe is configured and parked NAK.
 *
 * @pre ::hid_enum_set_config succeeded.
 * @pre The pipe is not currently armed.
 * @post The pipe targets ::k_hid_dev_addr EP1 with DATA0 forced.
 * @post The pipe's status bits are cleared.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t hid_open_pipes(void)
{
  return ra8_usb_host_pipe_setup(k_ra8_usb_speed_hs,
                                 (uint8_t)k_hid_pipe_in,
                                 (uint8_t)k_hid_dev_addr,
                                 (uint8_t)k_hid_ep_in_num,
                                 true,
                                 (uint16_t)k_hid_mps);
}

/**
 * @brief Run the full enumeration ladder and open the bulk pipes.
 *
 * @details hunt -> SET_ADDRESS -> SET_CONFIGURATION -> open pipes, and
 * extract idProduct from the device descriptor. Each step prints its own
 * failure tag. The caller deinitializes the host controller on error.
 *
 * @param[out] out_pid Receives the device's idProduct on success.
 *
 * @return First failing step's error, or k_ra8_ok.
 * @retval k_ra8_ok Device enumerated; bulk pipes open.
 *
 * @pre ::ra8_usb_host_init has succeeded on this pass.
 * @pre @p out_pid is non-NULL.
 * @post @p out_pid holds the device idProduct on success.
 * @post On failure the offending step printed its tag.
 *
 * @note Blocking; runs on the low-priority host thread.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t hid_enumerate(uint32_t* out_pid)
{
  uint8_t   desc[k_hid_dev_desc_len] = {};
  ra8_err_t err                      = hid_enum_hunt(desc);
  if (err != k_ra8_ok) {
    (void)hid_print_fail("enumerate", err);
    return err;
  }
  *out_pid = (uint32_t)desc[k_hid_off_dev_pid] |
             ((uint32_t)desc[(uint32_t)k_hid_off_dev_pid + 1U] << (uint32_t)k_hid_byte_bits);
  err      = hid_enum_set_address();
  if (err != k_ra8_ok) {
    (void)hid_print_fail("set_address", err);
    return err;
  }
  err = hid_enum_set_config();
  if (err != k_ra8_ok) {
    (void)hid_print_fail("set_config", err);
    return err;
  }
  err = hid_open_pipes();
  if (err != k_ra8_ok) {
    (void)hid_print_fail("open_pipes", err);
  }
  return err;
}

/**
 * @brief Print "enumerated pid=0xNNNN" for the looped device.
 *
 * @param[in] pid The device idProduct to report.
 *
 * @return ra8_err_t propagated from the SCI helpers.
 * @retval k_ra8_ok The line is queued.
 *
 * @pre ::hid_enumerate succeeded.
 * @pre SCI8 init already ran.
 * @post One ASCII line is in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t hid_print_enum(uint32_t pid)
{
  ra8_err_t err = hid_print("ra8d2 hid: enumerated pid=0x");
  if (err != k_ra8_ok) {
    return err;
  }
  err = hid_print_hex(pid, (uint8_t)k_hid_hex_chars_u16);
  if (err != k_ra8_ok) {
    return err;
  }
  return hid_print("\r\n");
}

/**
 * @brief Print "N reports verified -- USB SELFTEST HID PASS".
 *
 * @return ra8_err_t propagated from the SCI helpers.
 * @retval k_ra8_ok The verdict line is queued.
 *
 * @pre All ::k_hid_rounds report rounds verified.
 * @pre SCI8 init already ran.
 * @post One ASCII verdict line is in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t hid_print_pass(void)
{
  ra8_err_t err = hid_print("ra8d2 hid: ");
  if (err != k_ra8_ok) {
    return err;
  }
  err = hid_print_dec((uint32_t)k_hid_rounds);
  if (err != k_ra8_ok) {
    return err;
  }
  return hid_print(" reports verified -- USB SELFTEST HID PASS\r\n");
}

/**
 * @brief One report round: read an interrupt-IN report, check its body.
 *
 * @details Polls one report off the device's interrupt-IN endpoint, checks
 * the read length equals ::k_hid_report_len, and byte-checks the fixed body
 * (bytes 1..N-1) against ::hid_fill_report_body. Byte 0 (the seq) is
 * recorded in ::s_dbg_last_seq as a liveness witness. Each report is well
 * under the endpoint MPS, so it arrives as one short packet.
 *
 * @param[in] round The report round index (0..::k_hid_rounds-1).
 *
 * @return ra8_err_t verdict.
 * @retval k_ra8_ok               The report body matched.
 * @retval k_ra8_err_invalid_size The report length differed.
 * @retval k_ra8_err_invalid_state The report body bytes differed.
 *
 * @pre The interrupt-IN pipe was opened by ::hid_open_pipes.
 * @pre The device send worker is queuing reports.
 * @post ::s_dbg_mismatch records @p round on any mismatch.
 * @post ::s_dbg_last_seq holds the read report's seq on success.
 *
 * @note Blocking; one interrupt-IN read over the self-loop.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t hid_read_round(uint32_t round)
{
  static uint8_t s_expect[k_hid_read_buf] = {};
  static uint8_t s_rx[k_hid_read_buf]     = {};
  hid_fill_report_body(s_expect, (uint32_t)k_hid_report_len);
  uint16_t        rx  = 0U;
  const ra8_err_t err = ra8_usb_host_bulk_in(k_ra8_usb_speed_hs,
                                             (uint8_t)k_hid_pipe_in,
                                             s_rx,
                                             (uint16_t)k_hid_read_buf,
                                             &rx);
  if (err != k_ra8_ok) {
    (void)hid_print_fail("report read", err);
    return err;
  }
  if (rx != (uint16_t)k_hid_report_len) {
    s_dbg_mismatch = round;
    (void)hid_print_fail("report length", k_ra8_err_invalid_size);
    return k_ra8_err_invalid_size;
  }
  const size_t body = (size_t)((uint32_t)k_hid_report_len - (uint32_t)k_hid_body_idx);
  if (memcmp(&s_rx[k_hid_body_idx], &s_expect[k_hid_body_idx], body) != 0) {
    s_dbg_mismatch = round;
    (void)hid_print_fail("report pattern mismatch", k_ra8_err_invalid_state);
    return k_ra8_err_invalid_state;
  }
  s_dbg_last_seq = (uint32_t)s_rx[k_hid_seq_idx];
  return k_ra8_ok;
}

/**
 * @brief One full host-side pass: enumerate, then read + check reports.
 *
 * @details Phases mirror ::hid_phase_t. On any failure the host controller
 * is deinitialized so the next retry starts from a clean attach.
 *
 * @return First failing step's error, or k_ra8_ok.
 * @retval k_ra8_ok The pass printed HID PASS.
 *
 * @pre Device-side HID class is registered and sending (other thread).
 * @pre The self-loop cable connects J7 to J11.
 * @post On success ::s_dbg_pass_count advanced and LED2 is on.
 * @post On failure the host controller is deinitialized.
 *
 * @note Blocking; runs on the low-priority host thread.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t hid_host_pass(void)
{
  s_dbg_phase   = (uint32_t)k_hid_phase_init;
  ra8_err_t err = hid_print("ra8d2 hid: host up on USB-HS, probing the loop...\r\n");
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_usb_host_init(k_ra8_usb_speed_hs);
  if (err != k_ra8_ok) {
    (void)hid_print_fail("host init", err);
    return err;
  }

  s_dbg_phase  = (uint32_t)k_hid_phase_enum;
  uint32_t pid = 0U;
  err          = hid_enumerate(&pid);
  if (err != k_ra8_ok) {
    (void)ra8_usb_host_deinit(k_ra8_usb_speed_hs);
    return err;
  }
  s_dbg_pid = pid;
  err       = hid_print_enum(pid);
  if (err != k_ra8_ok) {
    return err;
  }

  s_dbg_phase     = (uint32_t)k_hid_phase_verify;
  s_dbg_rounds_ok = 0U;
  for (uint32_t r = 0U; r < (uint32_t)k_hid_rounds; r++) {
    err = hid_read_round(r);
    if (err != k_ra8_ok) {
      (void)ra8_usb_host_deinit(k_ra8_usb_speed_hs);
      return err;
    }
    s_dbg_rounds_ok++;
  }

  s_dbg_phase = (uint32_t)k_hid_phase_pass;
  s_dbg_pass_count++;
  err = hid_print_pass();
  if (err != k_ra8_ok) {
    return err;
  }
  (void)ra8_board_led_on(k_ra8_board_led2);
  return k_ra8_ok;
}

VOID hid_host_worker(ULONG arg)
{
  (void)arg;

  tx_thread_sleep(k_hid_boot_wait_ticks);
  for (;;) {
    const ra8_err_t err = hid_host_pass();
    if (err == k_ra8_ok) {
      break;
    }
    tx_thread_sleep(k_hid_retry_ticks);
  }
  while (1) {
    tx_thread_sleep(k_hid_idle_ticks);
  }
}

#endif /* !RA8_OFF_TARGET */
