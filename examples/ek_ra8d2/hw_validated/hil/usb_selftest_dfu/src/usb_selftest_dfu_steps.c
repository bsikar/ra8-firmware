/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hw_validated/hil/usb_selftest_dfu/src/usb_selftest_dfu_steps.c
 * @brief Host-side DFU ladder + console helpers for usb_selftest_dfu
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Holds the polled USB-HS host ladder (enumerate -> DFU_DNLOAD -> DFU_UPLOAD
 * verify) together with the SCI8 console text-formatter helpers and the shared
 * per-block firmware pattern, split out of ``main.c`` so each translation unit
 * stays under the file-size cap. ``main.c`` drives the device-side DFU class,
 * the board bring-up, and the ThreadX worker threads; it calls into here only
 * through ::dfu_host_pass.
 *
 * The J-Link probes that mark host-ladder progress (::s_dbg_phase and friends)
 * live here because only the host ladder reads or writes them; the device-side
 * probes stay in ``main.c``.
 *
 * @author Brighton Sikarskie
 * @date 2026-06-15
 * @since 0.1.0
 */

#include "usb_selftest_dfu_steps.h"

#include <stdint.h>
#include <string.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_err.h"
#include "ra8_time.h"
#include "ra8_usb.h"

#ifndef RA8_OFF_TARGET

/* -------------------------------------------------------------------------- */
/* J-Link probes (host ladder) */
/* -------------------------------------------------------------------------- */

/** @brief Host-ladder phase marker (::dfu_phase_t). */
static volatile uint32_t s_dbg_phase;
/** @brief Device-reported product id captured at enumeration. */
static volatile uint32_t s_dbg_pid;
/** @brief Blocks the host confirmed byte-equal on upload (expect k_dfu_blocks). */
static volatile uint32_t s_dbg_blocks_ok;
/** @brief First mismatching block, or ::k_dfu_no_mismatch. */
static volatile uint32_t s_dbg_mismatch = (uint32_t)k_dfu_no_mismatch;
/** @brief Completed full passes (sticky success counter). */
static volatile uint32_t s_dbg_pass_count;

/* -------------------------------------------------------------------------- */
/* Shared per-block pattern */
/* -------------------------------------------------------------------------- */

/**
 * @brief Fill a firmware block with this block's deterministic bytes.
 * @details Byte i = ``(block*131 + i*7 + 0xA5) & 0xFF`` -- distinct per block so
 *          the upload check proves the bytes read back are the ones downloaded.
 * @param[in]  block The block index (0..::k_dfu_blocks-1).
 * @param[out] out   Destination buffer.
 * @param[in]  len   Bytes to fill.
 * @return void.
 * @pre @p out has @p len writable bytes; @p len <= ::k_dfu_xfer_size.
 * @pre @p block < ::k_dfu_blocks.
 * @post @p out[0..len-1] hold the block's pattern bytes.
 * @post No global state changes.
 * @note Pure function.
 * @since 0.1.0
 */
static void dfu_pattern_fill(uint32_t block, uint8_t* out, uint32_t len)
{
  for (uint32_t i = 0U; i < len; i++) {
    const uint32_t v = (block * (uint32_t)k_dfu_pat_blk_mul) + (i * (uint32_t)k_dfu_pat_idx_mul) +
                       (uint32_t)k_dfu_pat_bias;
    out[i]           = (uint8_t)(v & (uint32_t)k_dfu_byte_mask);
  }
}

/* -------------------------------------------------------------------------- */
/* Console helpers (SCI8 -> J-Link OB CDC) */
/* -------------------------------------------------------------------------- */

/**
 * @brief Format one nibble (0..15) into an uppercase hex character.
 * @param[in] nibble 4-bit value.
 * @return ASCII '0'..'9' or 'A'..'F'.
 * @retval '0' For a zero nibble.
 * @pre Caller has masked the value to 4 bits.
 * @pre None beyond the mask contract.
 * @post Returned byte is printable hex.
 * @post No state changes.
 * @note Pure function.
 * @since 0.1.0
 */
static uint8_t dfu_nibble_to_hex(uint32_t nibble)
{
  if (nibble < k_dfu_hex_digit_split) {
    return (uint8_t)((uint8_t)'0' + (uint8_t)nibble);
  }
  return (uint8_t)((uint8_t)'A' + (uint8_t)nibble - (uint8_t)k_dfu_hex_digit_split);
}

/**
 * @brief Bounded ASCII string length (cap ::k_dfu_print_cap).
 * @param[in] text NUL-terminated string.
 * @return Number of bytes before the NUL, capped.
 * @retval 0 For an empty string.
 * @pre @p text is non-NULL with readable storage.
 * @pre @p text fits the cap.
 * @post No state changes.
 * @post Return value never exceeds ::k_dfu_print_cap.
 * @note Bounded scan.
 * @since 0.1.0
 */
static uint32_t dfu_str_len(const char* text)
{
  uint32_t len = 0U;
  while (len < (uint32_t)k_dfu_print_cap) {
    if (text[len] == '\0') {
      break;
    }
    len++;
  }
  return len;
}

/**
 * @brief Push a literal block over the J-Link OB CDC console (SCI8) polled.
 * @param[in] data Buffer to send.
 * @param[in] len  Byte count.
 * @return ra8_err_t passthrough from `ra8_board_uart_console_write`.
 * @retval k_ra8_ok All bytes queued.
 * @pre @p data is non-NULL; the board console init already ran.
 * @pre @p len excludes any NUL terminator.
 * @post Bytes are in the SCI8 TX FIFO.
 * @post No other state changes.
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t dfu_sci_write(const uint8_t* data, uint32_t len)
{
  return ra8_board_uart_console_write(data, (size_t)len);
}

/**
 * @brief Print a NUL-terminated ASCII string over the console.
 * @param[in] text String to print (CR/LF included by the caller).
 * @return ra8_err_t propagated from the SCI helper.
 * @retval k_ra8_ok All bytes queued.
 * @pre SCI8 init already ran; @p text is non-NULL.
 * @pre @p text is NUL-terminated within ::k_dfu_print_cap bytes.
 * @post The string bytes are in the SCI8 TX FIFO.
 * @post No other state changes.
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t dfu_print(const char* text)
{
  return dfu_sci_write((const uint8_t*)text, dfu_str_len(text));
}

/**
 * @brief Print a value as fixed-width uppercase hex.
 * @param[in] value  Value to print.
 * @param[in] digits Hex digit count (4 for u16, 8 for u32).
 * @return ra8_err_t propagated from the SCI helper.
 * @retval k_ra8_ok All bytes queued.
 * @pre SCI8 init already ran.
 * @pre @p digits is at most ::k_dfu_hex_chars_u32.
 * @post One fixed-width hex token is in the SCI8 TX FIFO.
 * @post No other state changes.
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t dfu_print_hex(uint32_t value, uint8_t digits)
{
  uint8_t out[k_dfu_hex_chars_u32] = {};
  uint8_t width                    = digits;
  if (width > (uint8_t)k_dfu_hex_chars_u32) {
    width = (uint8_t)k_dfu_hex_chars_u32;
  }
  for (uint8_t i = 0U; i < width; i++) {
    const uint8_t shift = (uint8_t)((width - 1U - i) * k_dfu_nibble_bits);
    out[i]              = dfu_nibble_to_hex((value >> shift) & k_dfu_nibble_mask);
  }
  return dfu_sci_write(out, (uint32_t)width);
}

/**
 * @brief Print a uint32_t as ASCII decimal.
 * @param[in] value Value to print.
 * @return ra8_err_t propagated from the SCI helper.
 * @retval k_ra8_ok All bytes queued.
 * @pre SCI8 init already ran.
 * @pre None beyond console readiness.
 * @post One ASCII decimal token is in the SCI8 TX FIFO.
 * @post No other state changes.
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t dfu_print_dec(uint32_t value)
{
  uint8_t  scratch[k_dfu_dec_chars_u32] = {};
  uint8_t  out[k_dfu_dec_chars_u32]     = {};
  uint8_t  count                        = 0U;
  uint32_t v                            = value;
  if (v == 0U) {
    out[0] = (uint8_t)'0';
    return dfu_sci_write(out, 1U);
  }
  while (v != 0U) {
    if (count >= (uint8_t)k_dfu_dec_chars_u32) {
      break;
    }
    scratch[count] = (uint8_t)((uint8_t)'0' + (uint8_t)(v % k_dfu_dec_radix));
    v              = v / k_dfu_dec_radix;
    count++;
  }
  for (uint8_t i = 0U; i < count; i++) {
    out[i] = scratch[count - 1U - i];
  }
  return dfu_sci_write(out, (uint32_t)count);
}

/**
 * @brief Print "FAIL <what> err=0xNNNNNNNN" on its own line.
 * @param[in] what Short description of the failed step.
 * @param[in] err  Error code returned by the step.
 * @return ra8_err_t propagated from the SCI helpers.
 * @retval k_ra8_ok The diagnostic line is queued.
 * @pre SCI8 init already ran; @p what is NUL-terminated within the cap.
 * @pre None beyond console readiness.
 * @post One diagnostic line is in the SCI8 TX FIFO.
 * @post No other state changes.
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t dfu_print_fail(const char* what, ra8_err_t err)
{
  ra8_err_t e = dfu_print("ra8d2 dfu: FAIL ");
  if (e != k_ra8_ok) {
    return e;
  }
  e = dfu_print(what);
  if (e != k_ra8_ok) {
    return e;
  }
  e = dfu_print(" err=0x");
  if (e != k_ra8_ok) {
    return e;
  }
  e = dfu_print_hex((uint32_t)err, (uint8_t)k_dfu_hex_chars_u32);
  if (e != k_ra8_ok) {
    return e;
  }
  return dfu_print("\r\n");
}

/* -------------------------------------------------------------------------- */
/* Host side: enumerate, then DFU download + upload-verify */
/* -------------------------------------------------------------------------- */

/**
 * @brief GET_DESCRIPTOR(DEVICE) over the polled control engine.
 * @param[out] desc Receives the 18-byte device descriptor.
 * @return Read outcome.
 * @retval k_ra8_ok           All 18 bytes arrived.
 * @retval k_ra8_err_hw_error A short descriptor came back.
 * @pre The bus is reset and the DCP targets the device's current address.
 * @pre @p desc holds at least ::k_dfu_dev_desc_len bytes.
 * @post @p desc carries the device descriptor on success.
 * @post No global state changes.
 * @note Blocking (polled control transfer).
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t dfu_ctrl_get_dev_desc(uint8_t* desc)
{
  const ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_dfu_bm_std_dev_in,
    .b_request       = (uint8_t)k_dfu_breq_get_desc,
    .w_value         = (uint16_t)((uint16_t)k_dfu_desc_device << (uint16_t)k_dfu_byte_bits),
    .w_index         = 0U,
    .w_length        = (uint16_t)k_dfu_dev_desc_len,
  };
  uint16_t        rx = 0U;
  const ra8_err_t err =
    ra8_usb_host_control_xfer(k_ra8_usb_speed_hs, &setup, desc, (uint16_t)k_dfu_dev_desc_len, &rx);
  if (err != k_ra8_ok) {
    return err;
  }
  return (rx == (uint16_t)k_dfu_dev_desc_len) ? k_ra8_ok : k_ra8_err_hw_error;
}

/**
 * @brief Wait for attach, then bus-reset + read the device descriptor.
 * @param[out] desc Receives the winning 18-byte device descriptor.
 * @return Hunt outcome.
 * @retval k_ra8_ok             The device answered at address 0.
 * @retval k_ra8_err_hw_timeout Nothing attached / nothing answered.
 * @pre ::ra8_usb_host_init ran (host up, J7 VBUS supplied).
 * @pre ::ra8_time_init has run (ms delays).
 * @post On success the DCP targets address 0 with UACT on.
 * @post On failure the bus is left in the last attempt's state.
 * @note Blocking; worst case a few seconds.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t dfu_enum_hunt(uint8_t* desc)
{
  ra8_delay_ms(k_dfu_vbus_settle_ms);
  const uint32_t t0 = ra8_time_ms();
  for (uint32_t spin = 0U; spin < (uint32_t)k_dfu_attach_spin; spin++) {
    if (ra8_usb_host_line_state(k_ra8_usb_speed_hs) != 0U) {
      break;
    }
    if ((ra8_time_ms() - t0) > (uint32_t)k_dfu_attach_to_ms) {
      break;
    }
  }
  ra8_delay_ms(k_dfu_debounce_ms);
  ra8_err_t err = k_ra8_err_hw_timeout;
  for (uint8_t attempt = 0U; attempt < (uint8_t)k_dfu_enum_tries; attempt++) {
    (void)ra8_usb_host_bus_reset(k_ra8_usb_speed_hs, true);
    ra8_delay_ms(k_dfu_reset_hold_ms);
    (void)ra8_usb_host_bus_reset(k_ra8_usb_speed_hs, false);
    (void)ra8_usb_host_set_uact(k_ra8_usb_speed_hs, true);
    ra8_delay_ms(k_dfu_recovery_ms);
    (void)ra8_usb_host_set_target(k_ra8_usb_speed_hs, 0U);
    err = dfu_ctrl_get_dev_desc(desc);
    if (err == k_ra8_ok) {
      return k_ra8_ok;
    }
  }
  return err;
}

/**
 * @brief SET_ADDRESS to ::k_dfu_dev_addr, then retarget the DCP.
 * @return First failing step's error, or k_ra8_ok.
 * @retval k_ra8_ok The DCP now targets the operating address.
 * @pre ::dfu_enum_hunt succeeded (device answering at address 0).
 * @pre The bus is active (UACT on).
 * @post Later transfers carry tokens to ::k_dfu_dev_addr.
 * @post The set-address recovery delay has elapsed.
 * @note Blocking (one control transfer + settle).
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t dfu_enum_set_address(void)
{
  const ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_dfu_bm_std_dev_out,
    .b_request       = (uint8_t)k_dfu_breq_set_addr,
    .w_value         = (uint16_t)k_dfu_dev_addr,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  const ra8_err_t err = ra8_usb_host_control_xfer(k_ra8_usb_speed_hs, &setup, nullptr, 0U, nullptr);
  if (err != k_ra8_ok) {
    return err;
  }
  ra8_delay_ms(k_dfu_addr_settle_ms);
  return ra8_usb_host_set_target(k_ra8_usb_speed_hs, (uint8_t)k_dfu_dev_addr);
}

/**
 * @brief SET_CONFIGURATION(::k_dfu_config_val) on the addressed device.
 * @return Control-transfer outcome.
 * @retval k_ra8_ok The device entered the Configured state (dfuIDLE).
 * @pre ::dfu_enum_set_address succeeded.
 * @pre The DCP targets ::k_dfu_dev_addr.
 * @post On success the DFU class is active in DFU mode.
 * @post No global state changes.
 * @note Blocking (one control transfer).
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t dfu_enum_set_config(void)
{
  const ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_dfu_bm_std_dev_out,
    .b_request       = (uint8_t)k_dfu_breq_set_config,
    .w_value         = (uint16_t)k_dfu_config_val,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  return ra8_usb_host_control_xfer(k_ra8_usb_speed_hs, &setup, nullptr, 0U, nullptr);
}

/**
 * @brief DFU_GETSTATUS: read the 6-byte status and return the bState field.
 * @param[out] out_state Receives the DFU state machine byte.
 * @return Control-transfer outcome.
 * @retval k_ra8_ok           Status read; @p out_state valid.
 * @retval k_ra8_err_hw_error A short status payload came back.
 * @pre The device is configured in DFU mode.
 * @pre @p out_state is non-NULL.
 * @post @p out_state holds bState on success.
 * @post No global state changes.
 * @note Blocking (one control-IN transfer).
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t dfu_getstatus(uint8_t* out_state)
{
  uint8_t               status[k_dfu_getstatus_len] = {};
  const ra8_usb_setup_t setup                       = {
    .bm_request_type = (uint8_t)k_dfu_bm_class_if_in,
    .b_request       = (uint8_t)k_dfu_breq_getstatus,
    .w_value         = 0U,
    .w_index         = (uint16_t)k_dfu_intf,
    .w_length        = (uint16_t)k_dfu_getstatus_len,
  };
  uint16_t        rx  = 0U;
  const ra8_err_t err = ra8_usb_host_control_xfer(k_ra8_usb_speed_hs,
                                                  &setup,
                                                  status,
                                                  (uint16_t)k_dfu_getstatus_len,
                                                  &rx);
  if (err != k_ra8_ok) {
    return err;
  }
  if (rx != (uint16_t)k_dfu_getstatus_len) {
    return k_ra8_err_hw_error;
  }
  *out_state = status[k_dfu_off_status_state];
  return k_ra8_ok;
}

/**
 * @brief Poll DFU_GETSTATUS until the device reports @p want_state.
 * @param[in] want_state The DFU bState the caller is waiting for.
 * @return Poll outcome.
 * @retval k_ra8_ok             The device reached @p want_state.
 * @retval k_ra8_err_hw_timeout It did not within ::k_dfu_status_tries.
 * @pre The device is configured in DFU mode.
 * @pre A control transfer (DNLOAD) preceded this.
 * @post On success the state machine is at @p want_state.
 * @post On failure the last poll's state is whatever the device held.
 * @note Blocking; bounded poll with ms pacing.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t dfu_wait_state(uint8_t want_state)
{
  for (uint32_t i = 0U; i < (uint32_t)k_dfu_status_tries; i++) {
    uint8_t         state = 0U;
    const ra8_err_t err   = dfu_getstatus(&state);
    if (err != k_ra8_ok) {
      return err;
    }
    if (state == want_state) {
      return k_ra8_ok;
    }
    ra8_delay_ms(k_dfu_status_poll_ms);
  }
  return k_ra8_err_hw_timeout;
}

/**
 * @brief DFU_DNLOAD one block, then poll to dfuDNLOAD-IDLE.
 * @param[in] block The DFU block sequence number (wValue).
 * @param[in] data  Block payload (control-OUT data stage).
 * @param[in] len   Block length in bytes.
 * @return First failing step's error, or k_ra8_ok.
 * @retval k_ra8_ok The block was downloaded and the device is DNLOAD-IDLE.
 * @pre The device is configured in DFU mode (dfuIDLE / dfuDNLOAD-IDLE).
 * @pre @p data holds @p len bytes.
 * @post The device captured the block; the state machine is DNLOAD-IDLE.
 * @post No global state changes.
 * @note Blocking; uses the host control-OUT data stage.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t dfu_dnload_block(uint16_t block, uint8_t* data, uint16_t len)
{
  const ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_dfu_bm_class_if_out,
    .b_request       = (uint8_t)k_dfu_breq_dnload,
    .w_value         = block,
    .w_index         = (uint16_t)k_dfu_intf,
    .w_length        = len,
  };
  const ra8_err_t err = ra8_usb_host_control_xfer(k_ra8_usb_speed_hs, &setup, data, len, nullptr);
  if (err != k_ra8_ok) {
    return err;
  }
  return dfu_wait_state((uint8_t)k_dfu_state_dnload_idle);
}

/**
 * @brief Download the whole rehearsal image, then DFU_ABORT back to dfuIDLE.
 * @return First failing step's error, or k_ra8_ok.
 * @retval k_ra8_ok All blocks downloaded; the device is back in dfuIDLE.
 * @pre The device is configured in DFU mode.
 * @pre The DFU functional descriptor advertises CAN_DOWNLOAD.
 * @post The device captured ::k_dfu_image_bytes; the state machine is dfuIDLE.
 * @post On failure the caller logs the offending step.
 * @note Blocking; runs on the host worker thread. The "real" DFU end-of-download
 *       (zero-length DFU_DNLOAD -> MANIFEST) is intentionally NOT used here: this
 *       USBX DFU class is not manifestation-tolerant, so after MANIFEST it parks
 *       in dfuMANIFEST-WAIT-RESET and only a USB bus reset returns it to a usable
 *       state -- which would tear down the in-place UPLOAD round-trip. DFU_ABORT
 *       returns dfuDNLOAD-IDLE -> dfuIDLE without a reset, so the same enumerated
 *       device can immediately be UPLOAD-verified.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t dfu_download_all(void)
{
  for (uint16_t b = 0U; b < (uint16_t)k_dfu_blocks; b++) {
    uint8_t block[k_dfu_xfer_size] = {};
    dfu_pattern_fill((uint32_t)b, block, (uint32_t)k_dfu_xfer_size);
    const ra8_err_t err = dfu_dnload_block(b, block, (uint16_t)k_dfu_xfer_size);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  /* DFU_ABORT closes the download (dfuDNLOAD-IDLE -> dfuIDLE) without the
   * manifest's wait-for-reset, so the UPLOAD phase runs on the same device. */
  const ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_dfu_bm_class_if_out,
    .b_request       = (uint8_t)k_dfu_breq_abort,
    .w_value         = 0U,
    .w_index         = (uint16_t)k_dfu_intf,
    .w_length        = 0U,
  };
  const ra8_err_t err = ra8_usb_host_control_xfer(k_ra8_usb_speed_hs, &setup, nullptr, 0U, nullptr);
  if (err != k_ra8_ok) {
    return err;
  }
  return dfu_wait_state((uint8_t)k_dfu_state_idle);
}

/**
 * @brief DFU_UPLOAD the image back block by block and byte-check each.
 * @return Verify outcome.
 * @retval k_ra8_ok                All blocks matched their downloaded pattern.
 * @retval k_ra8_err_invalid_size  A block returned the wrong length.
 * @retval k_ra8_err_invalid_state A block's bytes differed.
 * @pre ::dfu_download_all succeeded; the device is in dfuIDLE.
 * @pre The DFU functional descriptor advertises CAN_UPLOAD.
 * @post ::s_dbg_blocks_ok counts verified blocks; ::s_dbg_mismatch records the
 *       first bad block on failure.
 * @post No device state retained between blocks.
 * @note Blocking; one control-IN per block.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t dfu_upload_verify(void)
{
  s_dbg_blocks_ok = 0U;
  for (uint16_t b = 0U; b < (uint16_t)k_dfu_blocks; b++) {
    uint8_t               got[k_dfu_xfer_size] = {};
    const ra8_usb_setup_t setup                = {
      .bm_request_type = (uint8_t)k_dfu_bm_class_if_in,
      .b_request       = (uint8_t)k_dfu_breq_upload,
      .w_value         = b,
      .w_index         = (uint16_t)k_dfu_intf,
      .w_length        = (uint16_t)k_dfu_xfer_size,
    };
    uint16_t        rx = 0U;
    const ra8_err_t err =
      ra8_usb_host_control_xfer(k_ra8_usb_speed_hs, &setup, got, (uint16_t)k_dfu_xfer_size, &rx);
    if (err != k_ra8_ok) {
      return err;
    }
    if (rx != (uint16_t)k_dfu_xfer_size) {
      s_dbg_mismatch = (uint32_t)b;
      return k_ra8_err_invalid_size;
    }
    uint8_t want[k_dfu_xfer_size] = {};
    dfu_pattern_fill((uint32_t)b, want, (uint32_t)k_dfu_xfer_size);
    if (memcmp(got, want, (size_t)k_dfu_xfer_size) != 0) {
      s_dbg_mismatch = (uint32_t)b;
      return k_ra8_err_invalid_state;
    }
    s_dbg_blocks_ok++;
  }
  return k_ra8_ok;
}

/**
 * @brief Enumerate the looped-back device: descriptor hunt, address, config.
 * @details
 * Runs the ::k_dfu_phase_enum portion of the host pass -- fetch the device
 * descriptor, latch the product id into ::s_dbg_pid, assign the bus address,
 * select configuration 1, and print the enumerated pid. Any failing step
 * deinitializes the host controller so the caller can retry cleanly.
 * @return ra8_err_t First failing step's error, or k_ra8_ok.
 * @retval k_ra8_ok Device enumerated and the pid line printed.
 * @retval k_ra8_err_invalid_state Propagated descriptor / control-transfer error.
 * @pre ::ra8_usb_host_init has already succeeded (controller is up).
 * @pre The self-loop cable connects J7 to J11.
 * @post On success ::s_dbg_pid holds the device product id.
 * @post On failure the host controller is deinitialized.
 * @note Blocking; runs on the low-priority host thread.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t dfu_host_enumerate(void)
{
  s_dbg_phase                        = (uint32_t)k_dfu_phase_enum;
  uint8_t   desc[k_dfu_dev_desc_len] = {};
  ra8_err_t err                      = dfu_enum_hunt(desc);
  if (err != k_ra8_ok) {
    (void)dfu_print_fail("enumerate", err);
    (void)ra8_usb_host_deinit(k_ra8_usb_speed_hs);
    return err;
  }
  s_dbg_pid = (uint32_t)desc[k_dfu_off_dev_pid] |
              ((uint32_t)desc[(uint32_t)k_dfu_off_dev_pid + 1U] << (uint32_t)k_dfu_byte_bits);
  err       = dfu_enum_set_address();
  if (err != k_ra8_ok) {
    (void)dfu_print_fail("set_address", err);
    (void)ra8_usb_host_deinit(k_ra8_usb_speed_hs);
    return err;
  }
  err = dfu_enum_set_config();
  if (err != k_ra8_ok) {
    (void)dfu_print_fail("set_config", err);
    (void)ra8_usb_host_deinit(k_ra8_usb_speed_hs);
    return err;
  }
  err = dfu_print("ra8d2 dfu: enumerated pid=0x");
  if (err == k_ra8_ok) {
    err = dfu_print_hex(s_dbg_pid, (uint8_t)k_dfu_hex_chars_u16);
  }
  if (err == k_ra8_ok) {
    err = dfu_print("\r\n");
  }
  return err;
}

[[nodiscard]] ra8_err_t dfu_host_pass(void)
{
  s_dbg_phase   = (uint32_t)k_dfu_phase_init;
  ra8_err_t err = dfu_print("ra8d2 dfu: host up on USB-HS, probing the loop...\r\n");
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_usb_host_init(k_ra8_usb_speed_hs);
  if (err != k_ra8_ok) {
    (void)dfu_print_fail("host init", err);
    return err;
  }

  err = dfu_host_enumerate();
  if (err != k_ra8_ok) {
    return err;
  }

  s_dbg_phase = (uint32_t)k_dfu_phase_download;
  err         = dfu_download_all();
  if (err != k_ra8_ok) {
    (void)dfu_print_fail("dnload", err);
    (void)ra8_usb_host_deinit(k_ra8_usb_speed_hs);
    return err;
  }

  s_dbg_phase = (uint32_t)k_dfu_phase_upload;
  err         = dfu_upload_verify();
  if (err != k_ra8_ok) {
    (void)dfu_print_fail("upload verify", err);
    (void)ra8_usb_host_deinit(k_ra8_usb_speed_hs);
    return err;
  }

  s_dbg_phase = (uint32_t)k_dfu_phase_pass;
  s_dbg_pass_count++;
  err = dfu_print("ra8d2 dfu: ");
  if (err == k_ra8_ok) {
    err = dfu_print_dec((uint32_t)k_dfu_blocks);
  }
  if (err == k_ra8_ok) {
    err = dfu_print(" blocks downloaded + verified -- USB SELFTEST DFU PASS\r\n");
  }
  if (err != k_ra8_ok) {
    return err;
  }
  (void)ra8_board_led_on(k_ra8_board_led2);
  return k_ra8_ok;
}

#endif /* !RA8_OFF_TARGET */
