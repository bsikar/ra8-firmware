/**
 * @file examples/ek_ra8d2/hw_validated/hil/usb_selftest_cdc/src/usb_selftest_cdc_steps.c
 * @brief Host-side self-test ladder, console formatters, and echo pattern
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Companion translation unit for the ``usb_selftest_cdc`` HIL example,
 * carved out of main.c so every unit stays under the file-size cap. It holds
 * the SCI8 console formatters, the shared per-round echo pattern, and the
 * self-contained polled CDC host ladder (enumerate -> open bulk pipes ->
 * run the echo rounds) that the host worker drives. main.c retains the
 * device-side USBX worker, the board bring-up, and the entry point, and only
 * reaches in through ::cdc_host_worker.
 *
 * The host-side J-Link probes (``s_dbg_*``) live here because only the host
 * routines write them; the device-side probes stay in main.c.
 *
 * @author Brighton Sikarskie
 * @date 2026-06-13
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "usb_selftest_cdc_steps.h"

#include <stdint.h>
#include <string.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_err.h"
#include "ra8_time.h"
#include "ra8_usb.h"

#ifndef RA8_OFF_TARGET

/**
 * @enum cdc_hex_t
 * @brief Hex/decimal text-formatter sizing constants.
 */
typedef enum : uint8_t {
  k_cdc_hex_chars_u16   = 4U,  /**< 16-bit value -> "ABCD".        */
  k_cdc_hex_chars_u32   = 8U,  /**< 32-bit value -> "ABCDEF01".    */
  k_cdc_dec_chars_u32   = 10U, /**< Max digits for a 32-bit count. */
  k_cdc_nibble_bits     = 4U,  /**< Bits per hex nibble.           */
  k_cdc_hex_digit_split = 10U, /**< Threshold between '0-9'/'A-F'. */
} cdc_hex_t;

/**
 * @enum cdc_mask_t
 * @brief Bit-mask constants used by the text formatters.
 */
typedef enum : uint32_t {
  k_cdc_nibble_mask = 0xFU, /**< 4-bit nibble mask.           */
  k_cdc_dec_radix   = 10U,  /**< Base for decimal conversion. */
} cdc_mask_t;

/**
 * @enum cdc_phase_t
 * @brief J-Link probe values marking host-ladder progress.
 */
typedef enum : uint32_t {
  k_cdc_phase_boot   = 0U, /**< Host thread not started.   */
  k_cdc_phase_init   = 1U, /**< Host controller init.      */
  k_cdc_phase_enum   = 2U, /**< Enumerating.               */
  k_cdc_phase_verify = 3U, /**< Running echo rounds.       */
  k_cdc_phase_pass   = 4U, /**< All rounds echoed back OK. */
} cdc_phase_t;

/* -------------------------------------------------------------------------- */
/* J-Link probes (host side) */
/* -------------------------------------------------------------------------- */

/** @brief Host-ladder phase marker (::cdc_phase_t). */
static volatile uint32_t s_dbg_phase;
/** @brief Echo rounds the host confirmed byte-equal (expect ::k_cdc_rounds). */
static volatile uint32_t s_dbg_rounds_ok;
/** @brief Device-reported product id captured at enumeration. */
static volatile uint32_t s_dbg_pid;
/** @brief First mismatching round, or ::k_cdc_no_mismatch. */
static volatile uint32_t s_dbg_mismatch = (uint32_t)k_cdc_no_mismatch;
/** @brief Completed full passes (sticky success counter). */
static volatile uint32_t s_dbg_pass_count;

/* -------------------------------------------------------------------------- */
/* Shared per-round echo pattern */
/* -------------------------------------------------------------------------- */

/**
 * @brief Fill an echo payload with this round's deterministic bytes.
 *
 * @details Byte i = ``(round*97 + i*7 + 0x5A) & 0xFF``. Distinct per round
 * so the host proves the bytes it read back are the ones it sent for that
 * round (not a stale buffer). The device merely echoes, so only the host
 * computes this; both the OUT payload and the IN check use it.
 *
 * @param[in]  round The echo round index (0..::k_cdc_rounds-1).
 * @param[out] out   Destination buffer.
 * @param[in]  len   Bytes to fill.
 *
 * @pre @p out has @p len writable bytes.
 * @pre @p len is at most ::k_cdc_payload.
 * @post @p out[0..len-1] hold the round's pattern bytes.
 * @post No global state changes.
 *
 * @note Pure function.
 * @since 0.1.0
 */
static void cdc_pattern_fill(uint32_t round, uint8_t* out, uint32_t len)
{
  for (uint32_t i = 0U; i < len; i++) {
    const uint32_t v = (round * (uint32_t)k_cdc_pat_round_mul) + (i * (uint32_t)k_cdc_pat_idx_mul) +
                       (uint32_t)k_cdc_pat_bias;
    out[i]           = (uint8_t)(v & (uint32_t)k_cdc_byte_mask);
  }
}

/* -------------------------------------------------------------------------- */
/* Console helpers (SCI8 -> J-Link OB CDC) */
/* -------------------------------------------------------------------------- */

/**
 * @brief Format one nibble (0..15) into an uppercase hex character.
 *
 * @details Standard '0'-'9' then 'A'-'F' mapping.
 *
 * @param[in] nibble 4-bit value.
 *
 * @return ASCII '0'..'9' or 'A'..'F'.
 * @retval '0' For a zero nibble.
 *
 * @pre Caller has masked the value to 4 bits.
 * @pre None beyond the mask contract.
 * @post Returned byte is printable hex.
 * @post No state changes.
 *
 * @note Pure function.
 * @since 0.1.0
 */
static uint8_t cdc_nibble_to_hex(uint32_t nibble)
{
  if (nibble < k_cdc_hex_digit_split) {
    return (uint8_t)((uint8_t)'0' + (uint8_t)nibble);
  }
  return (uint8_t)((uint8_t)'A' + (uint8_t)nibble - (uint8_t)k_cdc_hex_digit_split);
}

/**
 * @brief Bounded ASCII string length (cap ::k_cdc_print_cap).
 *
 * @details Linear scan with a hard upper bound.
 *
 * @param[in] text NUL-terminated string.
 *
 * @return Number of bytes before the NUL, capped.
 * @retval 0 For an empty string.
 *
 * @pre @p text is non-NULL.
 * @pre @p text points to readable storage of at least the length.
 * @post No state changes.
 * @post Return value never exceeds ::k_cdc_print_cap.
 *
 * @note Bounded scan.
 * @since 0.1.0
 */
static uint32_t cdc_str_len(const char* text)
{
  uint32_t len = 0U;
  while (len < (uint32_t)k_cdc_print_cap) {
    if (text[len] == '\0') {
      break;
    }
    len++;
  }
  return len;
}

/**
 * @brief Push a literal block over SCI8 polled.
 *
 * @details Thin wrapper fixing the console channel.
 *
 * @param[in] data Buffer to send.
 * @param[in] len  Byte count.
 *
 * @return ra8_err_t passthrough from `ra8_board_uart_console_write`.
 * @retval k_ra8_ok All bytes queued.
 *
 * @pre @p data is non-NULL; SCI8 init already ran.
 * @pre @p len excludes any NUL terminator.
 * @post Bytes are in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t cdc_sci_write(const uint8_t* data, uint32_t len)
{
  return ra8_board_uart_console_write(data, (size_t)len);
}

/**
 * @brief Print a NUL-terminated ASCII string over the console.
 *
 * @details Length-bounded by ::cdc_str_len.
 *
 * @param[in] text String to print (CR/LF included by the caller).
 *
 * @return ra8_err_t propagated from the SCI helper.
 * @retval k_ra8_ok All bytes queued.
 *
 * @pre SCI8 init already ran; @p text is non-NULL.
 * @pre @p text is NUL-terminated within ::k_cdc_print_cap bytes.
 * @post The string bytes are in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t cdc_print(const char* text)
{
  return cdc_sci_write((const uint8_t*)text, cdc_str_len(text));
}

/**
 * @brief Print a uint32_t as ASCII decimal.
 *
 * @details Digit-reversal into a bounded scratch buffer.
 *
 * @param[in] value Value to print.
 *
 * @return ra8_err_t propagated from the SCI helper.
 * @retval k_ra8_ok All bytes queued.
 *
 * @pre SCI8 init already ran.
 * @pre None beyond console readiness.
 * @post One ASCII decimal token is in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t cdc_print_dec(uint32_t value)
{
  uint8_t  scratch[k_cdc_dec_chars_u32] = {};
  uint8_t  out[k_cdc_dec_chars_u32]     = {};
  uint8_t  count                        = 0U;
  uint32_t v                            = value;
  if (v == 0U) {
    out[0] = (uint8_t)'0';
    return cdc_sci_write(out, 1U);
  }
  while (v != 0U) {
    if (count >= (uint8_t)k_cdc_dec_chars_u32) {
      break;
    }
    scratch[count] = (uint8_t)((uint8_t)'0' + (uint8_t)(v % k_cdc_dec_radix));
    v              = v / k_cdc_dec_radix;
    count++;
  }
  for (uint8_t i = 0U; i < count; i++) {
    out[i] = scratch[count - 1U - i];
  }
  return cdc_sci_write(out, (uint32_t)count);
}

/**
 * @brief Print a value as fixed-width uppercase hex.
 *
 * @details Width is clamped to 8 hex digits.
 *
 * @param[in] value  Value to print.
 * @param[in] digits Hex digit count (4 for u16, 8 for u32).
 *
 * @return ra8_err_t propagated from the SCI helper.
 * @retval k_ra8_ok All bytes queued.
 *
 * @pre SCI8 init already ran.
 * @pre @p digits is at most ::k_cdc_hex_chars_u32.
 * @post One fixed-width hex token is in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t cdc_print_hex(uint32_t value, uint8_t digits)
{
  uint8_t out[k_cdc_hex_chars_u32] = {};
  uint8_t width                    = digits;
  if (width > (uint8_t)k_cdc_hex_chars_u32) {
    width = (uint8_t)k_cdc_hex_chars_u32;
  }
  for (uint8_t i = 0U; i < width; i++) {
    const uint8_t shift = (uint8_t)((width - 1U - i) * k_cdc_nibble_bits);
    out[i]              = cdc_nibble_to_hex((value >> shift) & k_cdc_nibble_mask);
  }
  return cdc_sci_write(out, (uint32_t)width);
}

/**
 * @brief Print "FAIL <what> err=0xNNNNNNNN" on its own line.
 *
 * @details One-line diagnostic; first failing chunk's code returned.
 *
 * @param[in] what Short description of the failed step.
 * @param[in] err  Error code returned by the step.
 *
 * @return ra8_err_t propagated from the SCI helpers.
 * @retval k_ra8_ok The diagnostic line is queued.
 *
 * @pre SCI8 init already ran.
 * @pre @p what is NUL-terminated within the print cap.
 * @post One diagnostic line is in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t cdc_print_fail(const char* what, ra8_err_t err)
{
  ra8_err_t e = cdc_print("ra8d2 cdc: FAIL ");
  if (e != k_ra8_ok) {
    return e;
  }
  e = cdc_print(what);
  if (e != k_ra8_ok) {
    return e;
  }
  e = cdc_print(" err=0x");
  if (e != k_ra8_ok) {
    return e;
  }
  e = cdc_print_hex((uint32_t)err, (uint8_t)k_cdc_hex_chars_u32);
  if (e != k_ra8_ok) {
    return e;
  }
  return cdc_print("\r\n");
}

/* -------------------------------------------------------------------------- */
/* Host side: self-contained polled CDC enumerate + bulk echo */
/* -------------------------------------------------------------------------- */

/**
 * @enum cdc_usb_req_t
 * @brief Standard chapter-9 request / descriptor constants for the host.
 */
typedef enum : uint16_t {
  k_cdc_bm_std_dev_in   = 0x80U, /**< bmRequestType: Std | Device | In.  */
  k_cdc_bm_std_dev_out  = 0x00U, /**< bmRequestType: Std | Device | Out. */
  k_cdc_breq_get_desc   = 0x06U, /**< GET_DESCRIPTOR.                    */
  k_cdc_breq_set_addr   = 0x05U, /**< SET_ADDRESS.                       */
  k_cdc_breq_set_config = 0x09U, /**< SET_CONFIGURATION.                 */
  k_cdc_desc_device     = 0x01U, /**< DEVICE descriptor type.            */
  k_cdc_dev_desc_len    = 18U,   /**< DEVICE descriptor length.          */
  k_cdc_off_dev_pid     = 10U,   /**< idProduct LSB byte offset.         */
  k_cdc_byte_bits       = 8U,    /**< Bits per byte.                     */
  k_cdc_config_value    = 1U,    /**< bConfigurationValue to select.     */
} cdc_usb_req_t;

/**
 * @enum cdc_enum_tune_t
 * @brief Timing / retry tunables for the polled enumeration ladder.
 */
typedef enum : uint32_t {
  k_cdc_vbus_settle_ms = 200U,      /**< VBUS settle before probing.          */
  k_cdc_attach_to_ms   = 2000U,     /**< Wait for the D+ pull-up.             */
  k_cdc_debounce_ms    = 500U,      /**< Post-attach debounce (>=100 ms).     */
  k_cdc_reset_hold_ms  = 50U,       /**< USB bus-reset hold (>=10 ms).        */
  k_cdc_recovery_ms    = 20U,       /**< Post-reset recovery (TRSTRCY).       */
  k_cdc_addr_settle_ms = 5U,        /**< Post-SET_ADDRESS recovery.           */
  k_cdc_enum_tries     = 8U,        /**< Reset+probe attempts.                */
  k_cdc_attach_spin    = 50000000U, /**< Attach spin cap (frozen-tick guard). */
} cdc_enum_tune_t;

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
 * @pre @p desc holds at least ::k_cdc_dev_desc_len bytes.
 * @post @p desc carries the device descriptor on success.
 * @post No global state changes.
 *
 * @note Blocking (polled control transfer).
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t cdc_ctrl_get_dev_desc(uint8_t* desc)
{
  const ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_cdc_bm_std_dev_in,
    .b_request       = (uint8_t)k_cdc_breq_get_desc,
    .w_value         = (uint16_t)((uint16_t)k_cdc_desc_device << (uint16_t)k_cdc_byte_bits),
    .w_index         = 0U,
    .w_length        = (uint16_t)k_cdc_dev_desc_len,
  };
  uint16_t        rx = 0U;
  const ra8_err_t err =
    ra8_usb_host_control_xfer(k_ra8_usb_speed_hs, &setup, desc, (uint16_t)k_cdc_dev_desc_len, &rx);
  if (err != k_ra8_ok) {
    return err;
  }
  if (rx != (uint16_t)k_cdc_dev_desc_len) {
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
[[nodiscard]] static ra8_err_t cdc_enum_hunt(uint8_t* desc)
{
  ra8_delay_ms(k_cdc_vbus_settle_ms);
  const uint32_t t0 = ra8_time_ms();
  for (uint32_t spin = 0U; spin < (uint32_t)k_cdc_attach_spin; spin++) {
    if (ra8_usb_host_line_state(k_ra8_usb_speed_hs) != 0U) {
      break;
    }
    if ((ra8_time_ms() - t0) > (uint32_t)k_cdc_attach_to_ms) {
      break;
    }
  }
  ra8_delay_ms(k_cdc_debounce_ms);
  ra8_err_t err = k_ra8_err_hw_timeout;
  for (uint8_t attempt = 0U; attempt < (uint8_t)k_cdc_enum_tries; attempt++) {
    (void)ra8_usb_host_bus_reset(k_ra8_usb_speed_hs, true);
    ra8_delay_ms(k_cdc_reset_hold_ms);
    (void)ra8_usb_host_bus_reset(k_ra8_usb_speed_hs, false);
    (void)ra8_usb_host_set_uact(k_ra8_usb_speed_hs, true);
    ra8_delay_ms(k_cdc_recovery_ms);
    (void)ra8_usb_host_set_target(k_ra8_usb_speed_hs, 0U);
    err = cdc_ctrl_get_dev_desc(desc);
    if (err == k_ra8_ok) {
      return k_ra8_ok;
    }
  }
  return err;
}

/**
 * @brief SET_ADDRESS to ::k_cdc_dev_addr, then retarget the DCP.
 *
 * @return First failing step's error, or k_ra8_ok.
 * @retval k_ra8_ok The DCP now targets the operating address.
 *
 * @pre ::cdc_enum_hunt succeeded (device answering at address 0).
 * @pre The bus is active (UACT on).
 * @post Later transfers carry tokens to ::k_cdc_dev_addr.
 * @post The set-address recovery delay has elapsed.
 *
 * @note Blocking (one control transfer + settle).
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t cdc_enum_set_address(void)
{
  const ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_cdc_bm_std_dev_out,
    .b_request       = (uint8_t)k_cdc_breq_set_addr,
    .w_value         = (uint16_t)k_cdc_dev_addr,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  ra8_err_t err = ra8_usb_host_control_xfer(k_ra8_usb_speed_hs, &setup, nullptr, 0U, nullptr);
  if (err != k_ra8_ok) {
    return err;
  }
  ra8_delay_ms(k_cdc_addr_settle_ms);
  return ra8_usb_host_set_target(k_ra8_usb_speed_hs, (uint8_t)k_cdc_dev_addr);
}

/**
 * @brief SET_CONFIGURATION(::k_cdc_config_value) on the addressed device.
 *
 * @return Control-transfer outcome.
 * @retval k_ra8_ok The device entered the Configured state.
 *
 * @pre ::cdc_enum_set_address succeeded.
 * @pre The DCP targets ::k_cdc_dev_addr.
 * @post On success the device's endpoints are usable.
 * @post No global state changes.
 *
 * @note Blocking (one control transfer).
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t cdc_enum_set_config(void)
{
  const ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_cdc_bm_std_dev_out,
    .b_request       = (uint8_t)k_cdc_breq_set_config,
    .w_value         = (uint16_t)k_cdc_config_value,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  return ra8_usb_host_control_xfer(k_ra8_usb_speed_hs, &setup, nullptr, 0U, nullptr);
}

/**
 * @brief Open the host bulk pipes for the device's CDC data endpoints.
 *
 * @details Pipe ::k_cdc_pipe_out -> device EP2 OUT (host transmits),
 * pipe ::k_cdc_pipe_in -> device EP1 IN (host receives), both 64-byte MPS.
 * Endpoint numbers come from this app's own device framework.
 *
 * @return First failing pipe-setup error, or k_ra8_ok.
 * @retval k_ra8_ok Both bulk pipes configured and parked NAK.
 *
 * @pre ::cdc_enum_set_config succeeded.
 * @pre The pipes are not currently armed.
 * @post Both pipes target ::k_cdc_dev_addr with DATA0 forced.
 * @post The pipes' status bits are cleared.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t cdc_open_pipes(void)
{
  ra8_err_t err = ra8_usb_host_pipe_setup(k_ra8_usb_speed_hs,
                                          (uint8_t)k_cdc_pipe_out,
                                          (uint8_t)k_cdc_dev_addr,
                                          (uint8_t)k_cdc_ep_out_num,
                                          false,
                                          (uint16_t)k_cdc_mps);
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_usb_host_pipe_setup(k_ra8_usb_speed_hs,
                                 (uint8_t)k_cdc_pipe_in,
                                 (uint8_t)k_cdc_dev_addr,
                                 (uint8_t)k_cdc_ep_in_num,
                                 true,
                                 (uint16_t)k_cdc_mps);
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
[[nodiscard]] static ra8_err_t cdc_enumerate(uint32_t* out_pid)
{
  uint8_t   desc[k_cdc_dev_desc_len] = {};
  ra8_err_t err                      = cdc_enum_hunt(desc);
  if (err != k_ra8_ok) {
    (void)cdc_print_fail("enumerate", err);
    return err;
  }
  *out_pid = (uint32_t)desc[k_cdc_off_dev_pid] |
             ((uint32_t)desc[(uint32_t)k_cdc_off_dev_pid + 1U] << (uint32_t)k_cdc_byte_bits);
  err      = cdc_enum_set_address();
  if (err != k_ra8_ok) {
    (void)cdc_print_fail("set_address", err);
    return err;
  }
  err = cdc_enum_set_config();
  if (err != k_ra8_ok) {
    (void)cdc_print_fail("set_config", err);
    return err;
  }
  err = cdc_open_pipes();
  if (err != k_ra8_ok) {
    (void)cdc_print_fail("open_pipes", err);
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
 * @pre ::cdc_enumerate succeeded.
 * @pre SCI8 init already ran.
 * @post One ASCII line is in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t cdc_print_enum(uint32_t pid)
{
  ra8_err_t err = cdc_print("ra8d2 cdc: enumerated pid=0x");
  if (err != k_ra8_ok) {
    return err;
  }
  err = cdc_print_hex(pid, (uint8_t)k_cdc_hex_chars_u16);
  if (err != k_ra8_ok) {
    return err;
  }
  return cdc_print("\r\n");
}

/**
 * @brief Print "N rounds echoed -- USB SELFTEST CDC-ECHO PASS".
 *
 * @return ra8_err_t propagated from the SCI helpers.
 * @retval k_ra8_ok The verdict line is queued.
 *
 * @pre All ::k_cdc_rounds echo rounds verified.
 * @pre SCI8 init already ran.
 * @post One ASCII verdict line is in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t cdc_print_pass(void)
{
  ra8_err_t err = cdc_print("ra8d2 cdc: ");
  if (err != k_ra8_ok) {
    return err;
  }
  err = cdc_print_dec((uint32_t)k_cdc_rounds);
  if (err != k_ra8_ok) {
    return err;
  }
  return cdc_print(" rounds echoed -- USB SELFTEST CDC-ECHO PASS\r\n");
}

/**
 * @brief One echo round: bulk-OUT a pattern, bulk-IN the echo, compare.
 *
 * @details Fills ::k_cdc_payload bytes from ::cdc_pattern_fill keyed on
 * @p round, ships them on the bulk-OUT pipe (one sub-MPS packet), then
 * reads the bulk-IN echo and byte-checks it. A sub-MPS payload returns as
 * a single short packet, so no MPS-exact ZLP is involved.
 *
 * @param[in] round The echo round index (0..::k_cdc_rounds-1).
 *
 * @return ra8_err_t verdict.
 * @retval k_ra8_ok               The echo matched the sent payload.
 * @retval k_ra8_err_invalid_size The echo length differed.
 * @retval k_ra8_err_invalid_state The echo bytes differed.
 *
 * @pre The bulk pipes were opened by ::cdc_open_pipes.
 * @pre The device echo worker is running.
 * @post ::s_dbg_mismatch records @p round on any mismatch.
 * @post Nothing is retained between rounds.
 *
 * @note Blocking; one bulk-OUT then one bulk-IN over the self-loop.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t cdc_echo_round(uint32_t round)
{
  static uint8_t s_tx[k_cdc_payload]  = {};
  static uint8_t s_rx[k_cdc_echo_buf] = {};
  cdc_pattern_fill(round, s_tx, (uint32_t)k_cdc_payload);
  ra8_err_t err = ra8_usb_host_bulk_out(k_ra8_usb_speed_hs,
                                        (uint8_t)k_cdc_pipe_out,
                                        s_tx,
                                        (uint16_t)k_cdc_payload);
  if (err != k_ra8_ok) {
    (void)cdc_print_fail("bulk_out", err);
    return err;
  }
  uint16_t rx = 0U;
  err         = ra8_usb_host_bulk_in(k_ra8_usb_speed_hs,
                                     (uint8_t)k_cdc_pipe_in,
                                     s_rx,
                                     (uint16_t)k_cdc_echo_buf,
                                     &rx);
  if (err != k_ra8_ok) {
    (void)cdc_print_fail("bulk_in", err);
    return err;
  }
  if (rx != (uint16_t)k_cdc_payload) {
    s_dbg_mismatch = round;
    (void)cdc_print_fail("echo length", k_ra8_err_invalid_size);
    return k_ra8_err_invalid_size;
  }
  if (memcmp(s_rx, s_tx, (size_t)k_cdc_payload) != 0) {
    s_dbg_mismatch = round;
    (void)cdc_print_fail("echo data mismatch", k_ra8_err_invalid_state);
    return k_ra8_err_invalid_state;
  }
  return k_ra8_ok;
}

/**
 * @brief One full host-side pass: enumerate, then run the echo rounds.
 *
 * @details Phases mirror ::cdc_phase_t. On any failure the host controller
 * is deinitialized so the next retry starts from a clean attach.
 *
 * @return First failing step's error, or k_ra8_ok.
 * @retval k_ra8_ok The pass printed CDC-ECHO PASS.
 *
 * @pre Device-side CDC class is registered and echoing (other thread).
 * @pre The self-loop cable connects J7 to J11.
 * @post On success ::s_dbg_pass_count advanced and LED2 is on.
 * @post On failure the host controller is deinitialized.
 *
 * @note Blocking; runs on the low-priority host thread.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t cdc_host_pass(void)
{
  s_dbg_phase   = (uint32_t)k_cdc_phase_init;
  ra8_err_t err = cdc_print("ra8d2 cdc: host up on USB-HS, probing the loop...\r\n");
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_usb_host_init(k_ra8_usb_speed_hs);
  if (err != k_ra8_ok) {
    (void)cdc_print_fail("host init", err);
    return err;
  }

  s_dbg_phase  = (uint32_t)k_cdc_phase_enum;
  uint32_t pid = 0U;
  err          = cdc_enumerate(&pid);
  if (err != k_ra8_ok) {
    (void)ra8_usb_host_deinit(k_ra8_usb_speed_hs);
    return err;
  }
  s_dbg_pid = pid;
  err       = cdc_print_enum(pid);
  if (err != k_ra8_ok) {
    return err;
  }

  s_dbg_phase     = (uint32_t)k_cdc_phase_verify;
  s_dbg_rounds_ok = 0U;
  for (uint32_t r = 0U; r < (uint32_t)k_cdc_rounds; r++) {
    err = cdc_echo_round(r);
    if (err != k_ra8_ok) {
      (void)ra8_usb_host_deinit(k_ra8_usb_speed_hs);
      return err;
    }
    s_dbg_rounds_ok++;
  }

  s_dbg_phase = (uint32_t)k_cdc_phase_pass;
  s_dbg_pass_count++;
  err = cdc_print_pass();
  if (err != k_ra8_ok) {
    return err;
  }
  (void)ra8_board_led_on(k_ra8_board_led2);
  return k_ra8_ok;
}

VOID cdc_host_worker(ULONG arg)
{
  (void)arg;

  tx_thread_sleep(k_cdc_boot_wait_ticks);
  for (;;) {
    const ra8_err_t err = cdc_host_pass();
    if (err == k_ra8_ok) {
      break;
    }
    tx_thread_sleep(k_cdc_retry_ticks);
  }
  while (1) {
    tx_thread_sleep(k_cdc_idle_ticks);
  }
}

#endif /* !RA8_OFF_TARGET */
