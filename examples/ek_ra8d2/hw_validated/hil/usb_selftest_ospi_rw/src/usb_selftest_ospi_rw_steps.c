/**
 * @file examples/ek_ra8d2/hw_validated/hil/usb_selftest_ospi_rw/src/usb_selftest_ospi_rw_steps.c
 * @brief Host-side step routines + console formatters for the writable-OSPI self-loop
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The host-side half of the `usb_selftest_ospi_rw` application, split out of
 * `main.c` so each translation unit stays below the 1000-line file-size cap.
 * It carries:
 *
 *  - the SCI8 (J-Link OB CDC) console formatters: nibble/hex/decimal text
 *    conversion and the line-printers used to stream verdicts;
 *  - the deterministic per-(LUN,LBA) sector pattern generator that both the
 *    WRITE(10) producer and the READ(10) verifier compute identically;
 *  - the first-party polled host MSC ladder (enumerate, WRITE(10) the whole
 *    OSPI window, then READ(10) it back and byte-check every sector); and
 *  - the host ThreadX worker (::ospirw_host_worker) that retries the full
 *    pass until the writable LUN verifies, then parks.
 *
 * Only ::ospirw_host_worker is externally visible; ``tx_application_define``
 * in `main.c` spawns it. Everything else here is file-private. The J-Link
 * debug probes touched solely by this side (``s_dbg_phase`` ...
 * ``s_dbg_pass_count``) live here too; the device-side probes stay in
 * `main.c`.
 *
 * @author Brighton Sikarskie
 * @date 2026-06-13
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "usb_selftest_ospi_rw_steps.h"

#include <stdint.h>
#include <string.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_err.h"
#include "ra8_usb_hmsc.h"

#ifndef RA8_OFF_TARGET
#include "tx_api.h"
#include "ux_api.h"

/* -------------------------------------------------------------------------- */
/* J-Link probes (host-ladder side) */
/* -------------------------------------------------------------------------- */

/** @brief Host-ladder phase marker (::ospirw_phase_t). */
static volatile uint32_t s_dbg_phase;
/** @brief Sectors that read back correctly after the write pass. */
static volatile uint32_t s_dbg_luns_ok;
/** @brief Device-reported GET_MAX_LUN value (expect 0, single LUN). */
static volatile uint32_t s_dbg_max_lun;
/** @brief First mismatching sector, or ::k_ospirw_no_mismatch. */
static volatile uint32_t s_dbg_mismatch = (uint32_t)k_ospirw_no_mismatch;
/** @brief Completed full passes (sticky success counter). */
static volatile uint32_t s_dbg_pass_count;

/* -------------------------------------------------------------------------- */
/* Shared per-(LUN,LBA) pattern */
/* -------------------------------------------------------------------------- */

/**
 * @brief Fill one 512-byte sector with this LUN/LBA's deterministic bytes.
 *
 * @details Byte i = ``(lun*97 + lba*7 + i + 0x5A) & 0xFF``. Distinct per
 * LUN and per LBA so the host can prove it addressed the right logical
 * unit and sector. The device media-read and the host verifier compute
 * it identically.
 *
 * @param[in]  lun The logical unit (0..1).
 * @param[in]  lba The logical block address within the LUN.
 * @param[out] out 512-byte destination buffer.
 *
 * @pre @p out has ::k_ospirw_block_size writable bytes.
 * @pre @p lun and @p lba are within the exposed geometry.
 * @post @p out holds the sector's pattern bytes.
 * @post No global state changes.
 *
 * @note Pure function.
 * @since 0.1.0
 */
static void ospirw_pattern_fill(uint32_t lun, uint32_t lba, UCHAR* out)
{
  for (uint32_t i = 0U; i < (uint32_t)k_ospirw_block_size; i++) {
    const uint32_t v = (lun * (uint32_t)k_ospirw_pat_lun_mul) +
                       (lba * (uint32_t)k_ospirw_pat_lba_mul) + i + (uint32_t)k_ospirw_pat_bias;
    out[i] = (UCHAR)(v & (uint32_t)k_ospirw_byte_mask);
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
static uint8_t ospirw_nibble_to_hex(uint32_t nibble)
{
  if (nibble < k_ospirw_hex_digit_split) {
    return (uint8_t)((uint8_t)'0' + (uint8_t)nibble);
  }
  return (uint8_t)((uint8_t)'A' + (uint8_t)nibble - (uint8_t)k_ospirw_hex_digit_split);
}

/**
 * @brief Bounded ASCII string length (cap ::k_ospirw_print_cap).
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
 * @post Return value never exceeds ::k_ospirw_print_cap.
 *
 * @note Bounded scan.
 * @since 0.1.0
 */
static uint32_t ospirw_str_len(const char* text)
{
  uint32_t len = 0U;
  while (len < (uint32_t)k_ospirw_print_cap) {
    if (text[len] == '\0') {
      break;
    }
    len++;
  }
  return len;
}

/**
 * @brief Push a literal block over the BSP UART console (SCI8) polled.
 *
 * @details Thin wrapper fixing the console channel.
 *
 * @param[in] data Buffer to send.
 * @param[in] len  Byte count.
 *
 * @return ra8_err_t passthrough from `ra8_board_uart_console_write`.
 * @retval k_ra8_ok All bytes queued.
 *
 * @pre @p data is non-NULL; the BSP console init already ran.
 * @pre @p len excludes any NUL terminator.
 * @post Bytes are in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t ospirw_sci_write(const uint8_t* data, uint32_t len)
{
  return ra8_board_uart_console_write(data, (size_t)len);
}

/**
 * @brief Print a NUL-terminated ASCII string over the console.
 *
 * @details Length-bounded by ::ospirw_str_len.
 *
 * @param[in] text String to print (CR/LF included by the caller).
 *
 * @return ra8_err_t propagated from the SCI helper.
 * @retval k_ra8_ok All bytes queued.
 *
 * @pre SCI8 init already ran; @p text is non-NULL.
 * @pre @p text is NUL-terminated within ::k_ospirw_print_cap bytes.
 * @post The string bytes are in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t ospirw_print(const char* text)
{
  return ospirw_sci_write((const uint8_t*)text, ospirw_str_len(text));
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
[[nodiscard]] static ra8_err_t ospirw_print_dec(uint32_t value)
{
  uint8_t  scratch[k_ospirw_dec_chars_u32] = {};
  uint8_t  out[k_ospirw_dec_chars_u32]     = {};
  uint8_t  count                           = 0U;
  uint32_t v                               = value;
  if (v == 0U) {
    out[0] = (uint8_t)'0';
    return ospirw_sci_write(out, 1U);
  }
  while (v != 0U) {
    if (count >= (uint8_t)k_ospirw_dec_chars_u32) {
      break;
    }
    scratch[count] = (uint8_t)((uint8_t)'0' + (uint8_t)(v % k_ospirw_dec_radix));
    v              = v / k_ospirw_dec_radix;
    count++;
  }
  for (uint8_t i = 0U; i < count; i++) {
    out[i] = scratch[count - 1U - i];
  }
  return ospirw_sci_write(out, (uint32_t)count);
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
 * @pre @p digits is at most ::k_ospirw_hex_chars_u32.
 * @post One fixed-width hex token is in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t ospirw_print_hex(uint32_t value, uint8_t digits)
{
  uint8_t out[k_ospirw_hex_chars_u32] = {};
  uint8_t width                       = digits;
  if (width > (uint8_t)k_ospirw_hex_chars_u32) {
    width = (uint8_t)k_ospirw_hex_chars_u32;
  }
  for (uint8_t i = 0U; i < width; i++) {
    const uint8_t shift = (uint8_t)((width - 1U - i) * k_ospirw_nibble_bits);
    out[i]              = ospirw_nibble_to_hex((value >> shift) & k_ospirw_nibble_mask);
  }
  return ospirw_sci_write(out, (uint32_t)width);
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
[[nodiscard]] static ra8_err_t ospirw_print_fail(const char* what, ra8_err_t err)
{
  ra8_err_t e = ospirw_print("ra8d2 ospirw: FAIL ");
  if (e != k_ra8_ok) {
    return e;
  }
  e = ospirw_print(what);
  if (e != k_ra8_ok) {
    return e;
  }
  e = ospirw_print(" err=0x");
  if (e != k_ra8_ok) {
    return e;
  }
  e = ospirw_print_hex((uint32_t)err, (uint8_t)k_ospirw_hex_chars_u32);
  if (e != k_ra8_ok) {
    return e;
  }
  return ospirw_print("\r\n");
}

/* -------------------------------------------------------------------------- */
/* Host side: ra8_usb_hmsc enumerate + WRITE(10) then read-verify */
/* -------------------------------------------------------------------------- */

/**
 * @brief WRITE(10) the per-LBA pattern across the whole OSPI window.
 *
 * @details Fills an 8-block burst from ::ospirw_pattern_fill and pushes it
 * with raw ::ra8_usb_hmsc_write10 until every sector is written. This is
 * the host data-OUT phase that drives the device bulk-OUT receive path
 * (the gating mechanism for every writable / repeated-bulk matrix item).
 *
 * @param[in] lun Logical unit to write (0).
 *
 * @return ra8_err_t verdict.
 * @retval k_ra8_ok The whole window was written.
 * @retval k_ra8_err_hw_timeout A WRITE(10) data-OUT phase did not complete.
 *
 * @pre The host has enumerated the device.
 * @pre The LUN is writable (read_only_flag = UX_FALSE).
 * @post The device OSPI window holds the pattern on success.
 * @post On failure the offending step printed its error.
 *
 * @note Blocking; 8 four-KiB WRITE(10) bursts over the self-loop.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t ospirw_write_disk(uint32_t lun)
{
  static uint8_t s_wbuf[k_ospirw_burst_bytes] = {};
  for (uint32_t blk = 0U; blk < (uint32_t)k_ospirw_sectors;
       blk += (uint32_t)k_ospirw_burst_blocks) {
    for (uint32_t s = 0U; s < (uint32_t)k_ospirw_burst_blocks; s++) {
      ospirw_pattern_fill(lun, blk + s, &s_wbuf[s * (uint32_t)k_ospirw_block_size]);
    }
    const ra8_err_t err =
      ra8_usb_hmsc_write10((uint8_t)lun, blk, (uint16_t)k_ospirw_burst_blocks, s_wbuf);
    if (err != k_ra8_ok) {
      (void)ospirw_print_fail("WRITE(10)", err);
      return err;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Read + verify one LUN's full sector range against its pattern.
 *
 * @details READ_CAPACITY (must report ::k_ospirw_sectors), then a raw
 * multi-block READ(10) sweep in 8-block bursts, checking each sector
 * against ::ospirw_pattern_fill for this @p lun.
 *
 * @param[in] lun Logical unit to verify (0..1).
 *
 * @return ra8_err_t verdict.
 * @retval k_ra8_ok            The whole LUN matched its pattern.
 * @retval k_ra8_err_invalid_size  READ_CAPACITY reported wrong geometry.
 * @retval k_ra8_err_invalid_state A byte differed from the pattern.
 *
 * @pre The host has enumerated the device.
 * @pre @p lun is below ::k_ospirw_count.
 * @post ::s_dbg_mismatch records (lun<<24 | sector) on mismatch.
 * @post Nothing is retained between LUNs.
 *
 * @note Blocking; 32 four-KiB READ(10) bursts over the self-loop.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t ospirw_verify_one(uint32_t lun)
{
  static uint8_t s_burst[k_ospirw_burst_bytes] = {};
  static uint8_t s_expect[k_ospirw_block_size] = {};

  uint32_t  block_count = 0U;
  uint32_t  block_size  = 0U;
  ra8_err_t err         = ra8_usb_hmsc_read_capacity((uint8_t)lun, &block_count, &block_size);
  if (err != k_ra8_ok) {
    (void)ospirw_print_fail("read_capacity", err);
    return err;
  }
  if (block_count != (uint32_t)k_ospirw_sectors) {
    (void)ospirw_print_fail("capacity mismatch", k_ra8_err_invalid_size);
    return k_ra8_err_invalid_size;
  }
  for (uint32_t blk = 0U; blk < (uint32_t)k_ospirw_sectors;
       blk += (uint32_t)k_ospirw_burst_blocks) {
    err = ra8_usb_hmsc_read10((uint8_t)lun, blk, (uint16_t)k_ospirw_burst_blocks, s_burst);
    if (err != k_ra8_ok) {
      (void)ospirw_print_fail("READ(10)", err);
      return err;
    }
    for (uint32_t s = 0U; s < (uint32_t)k_ospirw_burst_blocks; s++) {
      ospirw_pattern_fill(lun, blk + s, s_expect);
      const uint32_t boff = s * (uint32_t)k_ospirw_block_size;
      if (memcmp(&s_burst[boff], s_expect, (size_t)k_ospirw_block_size) != 0) {
        s_dbg_mismatch = (lun << (uint32_t)k_ospirw_mismatch_lun_shift) | (blk + s);
        (void)ospirw_print_fail("LUN data mismatch", k_ra8_err_invalid_state);
        return k_ra8_err_invalid_state;
      }
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Print "LUN n OK" for the verified writable unit.
 *
 * @param[in] lun The LUN that just verified.
 *
 * @return ra8_err_t propagated from the SCI helpers.
 * @retval k_ra8_ok The line is queued.
 *
 * @pre ::ospirw_verify_one returned k_ra8_ok for @p lun.
 * @pre SCI8 init already ran.
 * @post One ASCII line is in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t ospirw_print_lun_ok(uint32_t lun)
{
  ra8_err_t err = ospirw_print("ra8d2 ospirw: LUN ");
  if (err != k_ra8_ok) {
    return err;
  }
  err = ospirw_print_dec(lun);
  if (err != k_ra8_ok) {
    return err;
  }
  return ospirw_print(" OK (64 sectors, write+read verified)\r\n");
}

/**
 * @brief Enumerate the looped device and print its PID + GET_MAX_LUN.
 *
 * @details Sets the enum phase, runs ::ra8_usb_hmsc_enumerate, records
 * the reported max-LUN in ::s_dbg_max_lun, and streams the
 * ``enumerated pid=... GET_MAX_LUN=...`` banner. On enumerate failure
 * the host controller is closed so the next retry re-attaches clean.
 *
 * @param[out] device Receives the enumerated descriptor snapshot.
 *
 * @return First failing step's error, or k_ra8_ok.
 * @retval k_ra8_ok Device enumerated and the banner printed.
 *
 * @pre @p device is non-null.
 * @pre ::ra8_usb_hmsc_init has succeeded on this pass.
 * @post ::s_dbg_max_lun mirrors the device's GET_MAX_LUN.
 * @post On failure the host controller is deinitialized.
 *
 * @note Blocking; runs on the low-priority host thread.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t ospirw_host_enumerate(ra8_usb_hmsc_device_t* device)
{
  s_dbg_phase   = (uint32_t)k_ospirw_phase_enum;
  ra8_err_t err = ra8_usb_hmsc_enumerate(device);
  if (err != k_ra8_ok) {
    (void)ospirw_print_fail("enumerate", err);
    (void)ra8_usb_hmsc_close();
    return err;
  }
  s_dbg_max_lun = (uint32_t)device->max_lun;
  err           = ospirw_print("ra8d2 ospirw: enumerated pid=0x");
  if (err != k_ra8_ok) {
    return err;
  }
  err = ospirw_print_hex((uint32_t)device->product_id, (uint8_t)k_ospirw_hex_chars_u16);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ospirw_print(", GET_MAX_LUN=");
  if (err != k_ra8_ok) {
    return err;
  }
  err = ospirw_print_dec(s_dbg_max_lun);
  if (err != k_ra8_ok) {
    return err;
  }
  return ospirw_print("\r\n");
}

/**
 * @brief One full host-side pass: enumerate, WRITE(10) then verify.
 *
 * @details Phases mirror ::ospirw_phase_t. On any failure the host
 * controller is closed so the next retry starts from a clean attach.
 *
 * @return First failing step's error, or k_ra8_ok.
 * @retval k_ra8_ok The pass printed WRITABLE-OSPI PASS.
 *
 * @pre Device-side class is registered and attached (other thread).
 * @pre The self-loop cable connects J7 to J11.
 * @post On success ::s_dbg_pass_count advanced and LED2 is on.
 * @post On failure the host controller is deinitialized again.
 *
 * @note Blocking; runs on the low-priority host thread.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t ospirw_host_pass(void)
{
  s_dbg_phase   = (uint32_t)k_ospirw_phase_init;
  ra8_err_t err = ospirw_print("ra8d2 ospirw: host up on USB-HS, probing the loop...\r\n");
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_usb_hmsc_init(k_ra8_usb_speed_hs);
  if (err != k_ra8_ok) {
    (void)ospirw_print_fail("host init", err);
    return err;
  }

  ra8_usb_hmsc_device_t device = {};
  err                          = ospirw_host_enumerate(&device);
  if (err != k_ra8_ok) {
    return err;
  }

  s_dbg_phase   = (uint32_t)k_ospirw_phase_verify;
  s_dbg_luns_ok = 0U;
  for (uint32_t lun = 0U; lun < (uint32_t)k_ospirw_count; lun++) {
    err = ospirw_write_disk(lun);
    if (err != k_ra8_ok) {
      (void)ra8_usb_hmsc_close();
      return err;
    }
    err = ospirw_verify_one(lun);
    if (err != k_ra8_ok) {
      (void)ra8_usb_hmsc_close();
      return err;
    }
    s_dbg_luns_ok++;
    err = ospirw_print_lun_ok(lun);
    if (err != k_ra8_ok) {
      return err;
    }
  }

  s_dbg_phase = (uint32_t)k_ospirw_phase_pass;
  s_dbg_pass_count++;
  err = ospirw_print("ra8d2 ospirw: USB SELFTEST WRITABLE-OSPI PASS\r\n");
  if (err != k_ra8_ok) {
    return err;
  }
  (void)ra8_board_led_on(k_ra8_board_led2);
  return k_ra8_ok;
}

VOID ospirw_host_worker(ULONG arg)
{
  (void)arg;

  tx_thread_sleep(k_ospirw_boot_wait_ticks);
  for (;;) {
    const ra8_err_t err = ospirw_host_pass();
    if (err == k_ra8_ok) {
      break;
    }
    tx_thread_sleep(k_ospirw_retry_ticks);
  }
  while (1) {
    tx_thread_sleep(k_ospirw_idle_ticks);
  }
}

#endif /* !RA8_OFF_TARGET */
