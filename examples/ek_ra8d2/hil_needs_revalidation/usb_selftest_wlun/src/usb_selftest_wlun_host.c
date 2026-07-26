/**
 * @file
 * examples/ek_ra8d2/hil_needs_revalidation/usb_selftest_wlun/src/usb_selftest_wlun_host.c
 * @brief Host-side MSC ladder: enumerate, WRITE(10) the disk, read-verify it
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The polled host-side worker of the writable-LUN self-loop. It enumerates
 * the looped-back FS device, WRITE(10)s a deterministic per-LBA pattern
 * across the whole RAM disk, then READ(10)s it back and byte-checks every
 * sector -- proving the device bulk-OUT WRITE data phase round-trips intact.
 * Streams its verdicts over SCI8 via the shared console formatters and
 * mirrors host-ladder progress in J-Link-readable probes (``s_dbg_*``).
 * Split out of ``main.c`` so each translation unit stays under the per-file
 * line cap. The ``wlun_host_worker`` contract lives in
 * ``usb_selftest_wlun_steps.h``.
 *
 * @author Brighton Sikarskie
 * @date 2026-06-13
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_err.h"
#include "ra8_usb_hmsc.h"
#include "tx_api.h"
#include "usb_selftest_wlun_steps.h"

#ifndef RA8_SIMULATOR_MODE

/* -------------------------------------------------------------------------- */
/* J-Link probes (host ladder) */
/* -------------------------------------------------------------------------- */

/** @brief Host-ladder phase marker (::wlun_phase_t). */
static volatile uint32_t s_dbg_phase;
/** @brief Sectors that read back correctly after the write pass. */
static volatile uint32_t s_dbg_luns_ok;
/** @brief Device-reported GET_MAX_LUN value (expect 0, single LUN). */
static volatile uint32_t s_dbg_max_lun;
/** @brief First mismatching sector, or ::k_wlun_no_mismatch. */
static volatile uint32_t s_dbg_mismatch = (uint32_t)k_wlun_no_mismatch;
/** @brief Completed full passes (sticky success counter). */
static volatile uint32_t s_dbg_pass_count;

/* -------------------------------------------------------------------------- */
/* Host side: ra8_usb_hmsc enumerate + WRITE(10) then read-verify */
/* -------------------------------------------------------------------------- */

/**
 * @brief WRITE(10) the per-LBA pattern across the whole RAM disk.
 *
 * @details Fills an 8-block burst from ::wlun_pattern_fill and pushes it
 * with raw ::ra8_usb_hmsc_write10 until every sector is written. This is
 * the host data-OUT phase that drives the device bulk-OUT receive path
 * (the gating mechanism for every writable / repeated-bulk matrix item).
 *
 * @param[in] lun Logical unit to write (0).
 *
 * @return ra8_err_t verdict.
 * @retval k_ra8_ok The whole disk was written.
 * @retval k_ra8_err_hw_timeout A WRITE(10) data-OUT phase did not complete.
 *
 * @pre The host has enumerated the device.
 * @pre The LUN is writable (read_only_flag = UX_FALSE).
 * @post The device RAM disk holds the pattern on success.
 * @post On failure the offending step printed its error.
 *
 * @note Blocking; 8 four-KiB WRITE(10) bursts over the self-loop.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t wlun_write_disk(uint32_t lun)
{
  static uint8_t s_wbuf[k_wlun_burst_bytes] = {};
  for (uint32_t blk = 0U; blk < (uint32_t)k_wlun_sectors; blk += (uint32_t)k_wlun_burst_blocks) {
    for (uint32_t s = 0U; s < (uint32_t)k_wlun_burst_blocks; s++) {
      wlun_pattern_fill(lun, blk + s, &s_wbuf[s * (uint32_t)k_wlun_block_size]);
    }
    const ra8_err_t err =
      ra8_usb_hmsc_write10((uint8_t)lun, blk, (uint16_t)k_wlun_burst_blocks, s_wbuf);
    if (err != k_ra8_ok) {
      (void)wlun_print_fail("WRITE(10)", err);
      return err;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Read + verify one LUN's full sector range against its pattern.
 *
 * @details READ_CAPACITY (must report ::k_wlun_sectors), then a raw
 * multi-block READ(10) sweep in 8-block bursts, checking each sector
 * against ::wlun_pattern_fill for this @p lun.
 *
 * @param[in] lun Logical unit to verify (0..1).
 *
 * @return ra8_err_t verdict.
 * @retval k_ra8_ok            The whole LUN matched its pattern.
 * @retval k_ra8_err_invalid_size  READ_CAPACITY reported wrong geometry.
 * @retval k_ra8_err_invalid_state A byte differed from the pattern.
 *
 * @pre The host has enumerated the device.
 * @pre @p lun is below ::k_wlun_count.
 * @post ::s_dbg_mismatch records (lun<<24 | sector) on mismatch.
 * @post Nothing is retained between LUNs.
 *
 * @note Blocking; 32 four-KiB READ(10) bursts over the self-loop.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t wlun_verify_one(uint32_t lun)
{
  static uint8_t s_burst[k_wlun_burst_bytes] = {};
  static uint8_t s_expect[k_wlun_block_size] = {};

  uint32_t  block_count = 0U;
  uint32_t  block_size  = 0U;
  ra8_err_t err         = ra8_usb_hmsc_read_capacity((uint8_t)lun, &block_count, &block_size);
  if (err != k_ra8_ok) {
    (void)wlun_print_fail("read_capacity", err);
    return err;
  }
  if (block_count != (uint32_t)k_wlun_sectors) {
    (void)wlun_print_fail("capacity mismatch", k_ra8_err_invalid_size);
    return k_ra8_err_invalid_size;
  }
  for (uint32_t blk = 0U; blk < (uint32_t)k_wlun_sectors; blk += (uint32_t)k_wlun_burst_blocks) {
    err = ra8_usb_hmsc_read10((uint8_t)lun, blk, (uint16_t)k_wlun_burst_blocks, s_burst);
    if (err != k_ra8_ok) {
      (void)wlun_print_fail("READ(10)", err);
      return err;
    }
    for (uint32_t s = 0U; s < (uint32_t)k_wlun_burst_blocks; s++) {
      wlun_pattern_fill(lun, blk + s, s_expect);
      const uint32_t boff = s * (uint32_t)k_wlun_block_size;
      if (memcmp(&s_burst[boff], s_expect, (size_t)k_wlun_block_size) != 0) {
        s_dbg_mismatch = (lun << (uint32_t)k_wlun_mismatch_lun_shift) | (blk + s);
        (void)wlun_print_fail("LUN data mismatch", k_ra8_err_invalid_state);
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
 * @pre ::wlun_verify_one returned k_ra8_ok for @p lun.
 * @pre SCI8 init already ran.
 * @post One ASCII line is in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t wlun_print_lun_ok(uint32_t lun)
{
  ra8_err_t err = wlun_print("ra8d2 wlun: LUN ");
  if (err != k_ra8_ok) {
    return err;
  }
  err = wlun_print_dec(lun);
  if (err != k_ra8_ok) {
    return err;
  }
  return wlun_print(" OK (64 sectors, write+read verified)\r\n");
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
[[nodiscard]] static ra8_err_t wlun_host_enumerate(ra8_usb_hmsc_device_t* device)
{
  s_dbg_phase   = (uint32_t)k_wlun_phase_enum;
  ra8_err_t err = ra8_usb_hmsc_enumerate(device);
  if (err != k_ra8_ok) {
    (void)wlun_print_fail("enumerate", err);
    (void)ra8_usb_hmsc_close();
    return err;
  }
  s_dbg_max_lun = (uint32_t)device->max_lun;
  err           = wlun_print("ra8d2 wlun: enumerated pid=0x");
  if (err != k_ra8_ok) {
    return err;
  }
  err = wlun_print_hex((uint32_t)device->product_id, (uint8_t)k_wlun_hex_chars_u16);
  if (err != k_ra8_ok) {
    return err;
  }
  err = wlun_print(", GET_MAX_LUN=");
  if (err != k_ra8_ok) {
    return err;
  }
  err = wlun_print_dec(s_dbg_max_lun);
  if (err != k_ra8_ok) {
    return err;
  }
  return wlun_print("\r\n");
}

/**
 * @brief One full host-side pass: enumerate, WRITE(10) then verify.
 *
 * @details Phases mirror ::wlun_phase_t. On any failure the host
 * controller is closed so the next retry starts from a clean attach.
 *
 * @return First failing step's error, or k_ra8_ok.
 * @retval k_ra8_ok The pass printed WRITABLE-LUN PASS.
 *
 * @pre Device-side class is registered and attached (other thread).
 * @pre The self-loop cable connects J7 to J11.
 * @post On success ::s_dbg_pass_count advanced and LED2 is on.
 * @post On failure the host controller is deinitialized again.
 *
 * @note Blocking; runs on the low-priority host thread.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t wlun_host_pass(void)
{
  s_dbg_phase   = (uint32_t)k_wlun_phase_init;
  ra8_err_t err = wlun_print("ra8d2 wlun: host up on USB-HS, probing the loop...\r\n");
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_usb_hmsc_init(k_ra8_usb_speed_hs);
  if (err != k_ra8_ok) {
    (void)wlun_print_fail("host init", err);
    return err;
  }

  ra8_usb_hmsc_device_t device = {};
  err                          = wlun_host_enumerate(&device);
  if (err != k_ra8_ok) {
    return err;
  }

  s_dbg_phase   = (uint32_t)k_wlun_phase_verify;
  s_dbg_luns_ok = 0U;
  for (uint32_t lun = 0U; lun < (uint32_t)k_wlun_count; lun++) {
    err = wlun_write_disk(lun);
    if (err != k_ra8_ok) {
      (void)ra8_usb_hmsc_close();
      return err;
    }
    err = wlun_verify_one(lun);
    if (err != k_ra8_ok) {
      (void)ra8_usb_hmsc_close();
      return err;
    }
    s_dbg_luns_ok++;
    err = wlun_print_lun_ok(lun);
    if (err != k_ra8_ok) {
      return err;
    }
  }

  s_dbg_phase = (uint32_t)k_wlun_phase_pass;
  s_dbg_pass_count++;
  err = wlun_print("ra8d2 wlun: USB SELFTEST WRITABLE-LUN PASS\r\n");
  if (err != k_ra8_ok) {
    return err;
  }
  (void)ra8_board_led_on(k_ra8_board_led2);
  return k_ra8_ok;
}

VOID wlun_host_worker(ULONG arg)
{
  (void)arg;

  tx_thread_sleep(k_wlun_boot_wait_ticks);
  for (;;) {
    const ra8_err_t err = wlun_host_pass();
    if (err == k_ra8_ok) {
      break;
    }
    tx_thread_sleep(k_wlun_retry_ticks);
  }
  while (1) {
    tx_thread_sleep(k_wlun_idle_ticks);
  }
}

#endif /* !RA8_SIMULATOR_MODE */
