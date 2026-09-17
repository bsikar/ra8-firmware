/**
 * @file
 * examples/ek_ra8d2/hw_validated/hil/usb_selftest_fs_host/src/usb_selftest_fs_host_host.c
 * @brief Host role: SCI8 console + ra8_fs backend + enumerate/verify ladder.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The HOST half of the USB self-loop config B example, split out of
 * `main.c`. The board's USBFS controller is the host: it enumerates the
 * HS device over the self-loop cable, mounts the synthesized FAT16 volume,
 * streams the 1 MiB data region back with raw multi-block READ(10) and
 * memcmp's it against the same MRAM bytes read directly, then proves a
 * WRITE(10) into the read-only LUN comes back rejected. This unit owns:
 *
 *  - the SCI8 (J-Link OB CDC) console formatters;
 *  - the ra8_io USB-MSC block device the ``ra8_fs`` mount runs on;
 *  - the enumerate / mount / verify / write-protect ladder and its J-Link
 *    progress probes;
 *  - the ThreadX host worker that retries the full pass until it succeeds.
 *
 * The compile-time constants and the cross-unit prototype live in
 * `usb_selftest_fs_host_steps.h`. Everything here stays private to this
 * translation unit; only ::selftest_host_worker is referenced from
 * `main.c` (spawned by `tx_application_define`).
 *
 * @author Brighton Sikarskie
 * @date 2026-06-13
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "usb_selftest_fs_host_steps.h"

#ifndef RA8_OFF_TARGET

#include "ra8_board_ek_ra8d2.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_io_blockdev.h"
#include "ra8_io_blockdev_usbmsc.h"
#include "ra8_time.h"
#include "ra8_usb_hmsc.h"
#include "tx_api.h"

/* -------------------------------------------------------------------------- */
/* J-Link probes (host side) */
/* -------------------------------------------------------------------------- */

/** @brief Host-ladder phase marker (::selftest_phase_t). */
static volatile uint32_t s_dbg_phase;
/** @brief Bytes content-verified so far this pass. */
static volatile uint32_t s_dbg_verified_bytes;
/** @brief First mismatching MRAM offset (::k_selftest_no_mismatch = none). */
static volatile uint32_t s_dbg_mismatch_off = (uint32_t)k_selftest_no_mismatch;
/** @brief Milliseconds the 1 MiB verify streamed for. */
static volatile uint32_t s_dbg_verify_ms;
/** @brief Completed full passes (sticky success counter). */
static volatile uint32_t s_dbg_pass_count;

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
 * @pre Caller has already masked the value to 4 bits.
 * @pre None beyond the mask contract.
 * @post Returned byte is in the printable hex range.
 * @post No state changes.
 *
 * @note Pure function.
 * @since 0.1.0
 */
static uint8_t selftest_nibble_to_hex(uint32_t nibble)
{
  if (nibble < k_selftest_hex_digit_split) {
    return (uint8_t)((uint8_t)'0' + (uint8_t)nibble);
  }
  return (uint8_t)((uint8_t)'A' + (uint8_t)nibble - (uint8_t)k_selftest_hex_digit_split);
}

/**
 * @brief Bounded ASCII string length (cap ::k_selftest_print_cap).
 *
 * @details Linear scan with a hard upper bound.
 *
 * @param[in] text NUL-terminated string.
 *
 * @return Number of bytes before the NUL, capped.
 * @retval 0 For an empty string.
 *
 * @pre @p text is non-NULL.
 * @pre @p text points to readable storage of at least the returned length.
 * @post No state changes.
 * @post Return value never exceeds ::k_selftest_print_cap.
 *
 * @note Bounded scan -- never walks past the cap on a missing NUL.
 * @since 0.1.0
 */
static uint32_t selftest_str_len(const char* text)
{
  uint32_t len = 0U;
  while (len < (uint32_t)k_selftest_print_cap) {
    if (text[len] == '\0') {
      break;
    }
    len++;
  }
  return len;
}

/**
 * @brief Push a literal block over the board UART console (SCI8) polled.
 *
 * @details Thin wrapper over the EK-RA8D2 board console writer.
 *
 * @param[in] data Buffer to send.
 * @param[in] len  Byte count.
 *
 * @return ra8_err_t passthrough from `ra8_board_uart_console_write`.
 * @retval k_ra8_ok All bytes queued.
 *
 * @pre @p data is non-NULL; the board console init already ran.
 * @pre @p len excludes any NUL terminator.
 * @post Bytes have been pushed out the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t selftest_sci_write(const uint8_t* data, uint32_t len)
{
  return ra8_board_uart_console_write(data, (size_t)len);
}

/**
 * @brief Print a NUL-terminated ASCII string over the console.
 *
 * @details Length-bounded by ::selftest_str_len.
 *
 * @param[in] text String to print (CR/LF included by the caller).
 *
 * @return ra8_err_t propagated from the SCI helper.
 * @retval k_ra8_ok All bytes queued.
 *
 * @pre SCI8 init already ran; @p text is non-NULL.
 * @pre @p text is NUL-terminated within ::k_selftest_print_cap bytes.
 * @post The string bytes are in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t selftest_print(const char* text)
{
  return selftest_sci_write((const uint8_t*)text, selftest_str_len(text));
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
[[nodiscard]] static ra8_err_t selftest_print_dec(uint32_t value)
{
  uint8_t  scratch[k_selftest_dec_chars_u32] = {};
  uint8_t  out[k_selftest_dec_chars_u32]     = {};
  uint8_t  count                             = 0U;
  uint32_t v                                 = value;
  if (v == 0U) {
    out[0] = (uint8_t)'0';
    return selftest_sci_write(out, 1U);
  }
  while (v != 0U) {
    if (count >= (uint8_t)k_selftest_dec_chars_u32) {
      break;
    }
    scratch[count] = (uint8_t)((uint8_t)'0' + (uint8_t)(v % k_selftest_dec_radix));
    v              = v / k_selftest_dec_radix;
    count++;
  }
  for (uint8_t i = 0U; i < count; i++) {
    out[i] = scratch[count - 1U - i];
  }
  return selftest_sci_write(out, (uint32_t)count);
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
 * @pre @p digits is at most ::k_selftest_hex_chars_u32.
 * @post One fixed-width hex token is in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t selftest_print_hex(uint32_t value, uint8_t digits)
{
  uint8_t out[k_selftest_hex_chars_u32] = {};
  uint8_t width                         = digits;
  if (width > (uint8_t)k_selftest_hex_chars_u32) {
    width = (uint8_t)k_selftest_hex_chars_u32;
  }
  for (uint8_t i = 0U; i < width; i++) {
    const uint8_t shift = (uint8_t)((width - 1U - i) * k_selftest_nibble_bits);
    out[i]              = selftest_nibble_to_hex((value >> shift) & k_selftest_nibble_mask);
  }
  return selftest_sci_write(out, (uint32_t)width);
}

/**
 * @brief Print "FAIL <what> err=0xNNNNNNNN" on its own line.
 *
 * @details One-line diagnostic; print errors inside are not recoverable
 * anyway, so the first failing chunk's code is returned.
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
[[nodiscard]] static ra8_err_t selftest_print_fail(const char* what, ra8_err_t err)
{
  ra8_err_t e = selftest_print("ra8d2 selftest: FAIL ");
  if (e != k_ra8_ok) {
    return e;
  }
  e = selftest_print(what);
  if (e != k_ra8_ok) {
    return e;
  }
  e = selftest_print(" err=0x");
  if (e != k_ra8_ok) {
    return e;
  }
  e = selftest_print_hex((uint32_t)err, (uint8_t)k_selftest_hex_chars_u32);
  if (e != k_ra8_ok) {
    return e;
  }
  return selftest_print("\r\n");
}

/* -------------------------------------------------------------------------- */
/* Host side: ra8_fs mount over the ra8_io USB-MSC block device */
/* -------------------------------------------------------------------------- */

/**
 * @var s_selftest_usb_dev
 * @brief Block device fronting the hosted MSC volume for this ladder.
 *
 * @details
 * Bound once in selftest_mount_volume by ::ra8_io_blockdev_usbmsc_init, which
 * wires the ra8_io USB-MSC backend over the polled host-MSC class. File scope
 * because ::ra8_io_blockdev_as_fs_backend keeps a pointer to this handle for
 * the whole life of the mount.
 *
 * @note Single-threaded ladder; no locking.
 * @warning Do not rebind while a mount is live.
 * @since 0.1.0
 */
static ra8_io_blockdev_t s_selftest_usb_dev;

/**
 * @var s_selftest_usb_state
 * @brief Caller-owned backend state for ::s_selftest_usb_dev.
 *
 * @details
 * Records the logical unit the backend addresses. Private to the ra8_io
 * USB-MSC backend and must out-live every call made through the device.
 *
 * @note Single-threaded ladder; no locking.
 * @warning Treat the contents as private to ra8_io.
 * @since 0.1.0
 */
static ra8_io_blockdev_usbmsc_state_t s_selftest_usb_state;

/**
 * @brief Map a detected filesystem type to a printable name.
 *
 * @details Total over the enum; unknown maps to "unknown".
 *
 * @param[in] type Mount-time detection result.
 *
 * @return Static NUL-terminated name string.
 * @retval "fat16" For ::k_ra8_fs_type_fat16 (the expected verdict).
 *
 * @pre None -- total over the enum.
 * @pre @p type came from a populated mount struct.
 * @post No state changes.
 * @post Returned pointer references static storage.
 *
 * @note Pure function.
 * @since 0.1.0
 */
static const char* selftest_fs_type_name(ra8_fs_type_t type)
{
  switch (type) {
    case k_ra8_fs_type_fat12:
      return "fat12";
    case k_ra8_fs_type_fat16:
      return "fat16";
    case k_ra8_fs_type_fat32:
      return "fat32";
    case k_ra8_fs_type_exfat:
      return "exfat";
    case k_ra8_fs_type_unknown:
    default:
      return "unknown";
  }
}

/**
 * @brief Mount the loop device's volume through the USB-MSC backend.
 *
 * @details Binds the ra8_io USB-MSC block device over the polled host
 * class, bridges it to an ra8_fs backend, and prints the detected
 * filesystem type (must be fat16 for this device).
 *
 * @param[out] out_mount Receives the mount handle on success.
 *
 * @return ra8_err_t from the block-device bind, the ra8_fs bridge, or
 *         ::ra8_fs_mount.
 * @retval k_ra8_ok Volume mounted; the type line was printed.
 *
 * @pre ::ra8_usb_hmsc_enumerate completed on the loop device.
 * @pre @p out_mount is non-NULL.
 * @post On k_ra8_ok the mount handle is live and must be unmounted later.
 * @post The "mounted fs=" line is queued on success.
 *
 * @note Reads the BPB chain over the self-loop cable.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t selftest_mount_volume(ra8_fs_mount_t** out_mount)
{
  ra8_err_t err = ra8_io_blockdev_usbmsc_init(&s_selftest_usb_dev,
                                              &s_selftest_usb_state,
                                              (uint8_t)k_selftest_target_lun);
  if (err != k_ra8_ok) {
    (void)selftest_print_fail("blockdev", err);
    return err;
  }
  ra8_fs_backend_t backend = {};
  err                      = ra8_io_blockdev_as_fs_backend(&s_selftest_usb_dev, &backend);
  if (err != k_ra8_ok) {
    (void)selftest_print_fail("backend", err);
    return err;
  }
  err = ra8_fs_mount(&backend, out_mount);
  if (err != k_ra8_ok) {
    (void)selftest_print_fail("mount", err);
    return err;
  }
  err = selftest_print("ra8d2 selftest: mounted fs=");
  if (err != k_ra8_ok) {
    return err;
  }
  err = selftest_print(selftest_fs_type_name((*out_mount)->type));
  if (err != k_ra8_ok) {
    return err;
  }
  return selftest_print("\r\n");
}

/**
 * @brief Raw multi-block READ(10) of the MRAM data region vs MRAM.
 *
 * @details Streams the 1 MiB data region straight through the SCSI
 * READ(10) entry point in 8-block (4 KiB) bursts -- one CBW per burst,
 * not per sector -- and memcmp's each burst against the same offset of
 * the real MRAM window at 0x02000000. This deliberately bypasses
 * ra8_fs: the filesystem walk re-reads FAT/metadata sectors per cluster
 * (a ~250x device-read amplification that throttled the loop to under
 * 1 sector/s), whereas the raw path is the direct integrity proof --
 * "the SCSI transport returns the chip's flash byte for byte" -- and
 * runs at the cable's real rate. The mount step (separate phase)
 * already proved the host can PARSE the FAT16 volume over the loop.
 *
 * Data region: FAT16 LBA ::k_fat_data_lba is cluster 2 = MRAM offset 0,
 * so LBA (data_lba + b) holds MRAM[b * 512].
 *
 * @return ra8_err_t verdict.
 * @retval k_ra8_ok            All 1 MiB matched byte for byte.
 * @retval k_ra8_err_invalid_state A byte differed from MRAM.
 *
 * @pre ::ra8_usb_hmsc_enumerate completed on the loop device.
 * @pre The device side exposes the synthesized MRAM volume.
 * @post ::s_dbg_verified_bytes / ::s_dbg_verify_ms / ::s_dbg_mismatch_off
 *       reflect the outcome.
 * @post No filesystem handle is held (raw SCSI path).
 *
 * @note Blocking; 256 four-KiB READ(10) bursts over the self-loop.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t selftest_verify_mram_raw(void)
{
  static uint8_t s_burst[k_selftest_burst_bytes] = {};

  s_dbg_verified_bytes = 0U;
  s_dbg_mismatch_off   = (uint32_t)k_selftest_no_mismatch;
  const uint32_t t0    = ra8_time_ms();
  for (uint32_t blk = 0U; blk < (uint32_t)k_fat_mram_clusters;
       blk += (uint32_t)k_selftest_burst_blocks) {
    const uint32_t lba = (uint32_t)k_fat_data_lba + blk;
    ra8_err_t      err = ra8_usb_hmsc_read10((uint8_t)k_selftest_target_lun,
                                             lba,
                                             (uint16_t)k_selftest_burst_blocks,
                                             s_burst);
    if (err != k_ra8_ok) {
      (void)selftest_print_fail("READ(10) burst", err);
      return err;
    }
    const uint32_t offset = blk * (uint32_t)k_selftest_block_size;
    const uint8_t* mram   = (const uint8_t*)(uintptr_t)((uint32_t)k_mram_base_addr + offset);
    if (memcmp(s_burst, mram, (size_t)k_selftest_burst_bytes) != 0) {
      s_dbg_mismatch_off = offset;
      (void)selftest_print_fail("content mismatch at 0x", (ra8_err_t)offset);
      return k_ra8_err_invalid_state;
    }
    s_dbg_verified_bytes = offset + (uint32_t)k_selftest_burst_bytes;
  }
  s_dbg_verify_ms = ra8_time_ms() - t0;
  return k_ra8_ok;
}

/**
 * @brief Print the verify verdict line (bytes + duration + rate).
 *
 * @details "verified 1048576 bytes in N ms (M KiB/s)".
 *
 * @return ra8_err_t propagated from the SCI helpers.
 * @retval k_ra8_ok The verdict line is queued.
 *
 * @pre ::selftest_verify_mram_file returned k_ra8_ok this pass.
 * @pre SCI8 init already ran.
 * @post One verdict line is in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Rate math guards the divide against a zero duration.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t selftest_print_verify_verdict(void)
{
  ra8_err_t err = selftest_print("ra8d2 selftest: verified ");
  if (err != k_ra8_ok) {
    return err;
  }
  err = selftest_print_dec(s_dbg_verified_bytes);
  if (err != k_ra8_ok) {
    return err;
  }
  err = selftest_print(" bytes vs MRAM in ");
  if (err != k_ra8_ok) {
    return err;
  }
  err = selftest_print_dec(s_dbg_verify_ms);
  if (err != k_ra8_ok) {
    return err;
  }
  err = selftest_print(" ms (");
  if (err != k_ra8_ok) {
    return err;
  }
  uint32_t ms = s_dbg_verify_ms;
  if (ms == 0U) {
    ms = 1U;
  }
  const uint32_t kib_per_s =
    (uint32_t)(((uint64_t)s_dbg_verified_bytes * (uint64_t)k_selftest_ms_per_sec) /
               ((uint64_t)ms * (uint64_t)k_selftest_bytes_per_kib));
  err = selftest_print_dec(kib_per_s);
  if (err != k_ra8_ok) {
    return err;
  }
  return selftest_print(" KiB/s)\r\n");
}

/**
 * @brief WRITE(10) into the read-only LUN must be rejected.
 *
 * @details Issues a 1-block WRITE(10) into the data region. The device
 * LUN is write-protected (MODE SENSE WP bit + media_write returns
 * DATA PROTECT), so the host's write entry point must surface an error
 * rather than k_ra8_ok -- the MRAM is never modified. This is the
 * write-protection proof.
 *
 * It deliberately stops there: terminating a data-out phase against a
 * write-protected device STALLs the bulk-OUT endpoint, and recovering
 * the BOT transport afterwards (Bulk-Only Mass Storage Reset + Clear
 * Feature ENDPOINT_HALT on both bulk pipes) is not yet implemented in
 * the host class. That STALL/ClearFeature recovery path is tracked as
 * GitHub issue #92's robustness sweep; this pass parks on success, so
 * the post-STALL desync does not affect the verdict.
 *
 * @return ra8_err_t verdict.
 * @retval k_ra8_ok            The write was rejected (protection works).
 * @retval k_ra8_err_invalid_state The write was unexpectedly accepted.
 *
 * @pre ::ra8_usb_hmsc_enumerate completed on the loop device.
 * @pre The device LUN reports write-protected.
 * @post The device volume is untouched (the write never lands).
 * @post One verdict line is queued on the console.
 *
 * @note Leaves the bulk-OUT pipe halted (see details); pass parks next.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t selftest_write_protect_probe(void)
{
  static uint8_t s_wp_buf[k_selftest_block_size] = {};

  ra8_err_t err = selftest_print("ra8d2 selftest: WRITE(10) into RO LUN must be rejected...\r\n");
  if (err != k_ra8_ok) {
    return err;
  }
  const ra8_err_t wr = ra8_usb_hmsc_write10((uint8_t)k_selftest_target_lun,
                                            (uint32_t)k_selftest_wp_probe_lba,
                                            1U,
                                            s_wp_buf);
  if (wr == k_ra8_ok) {
    (void)selftest_print_fail("write unexpectedly accepted", k_ra8_err_invalid_state);
    return k_ra8_err_invalid_state;
  }
  err = selftest_print("ra8d2 selftest: write rejected (code 0x");
  if (err != k_ra8_ok) {
    return err;
  }
  err = selftest_print_hex((uint32_t)wr, (uint8_t)k_selftest_hex_chars_u32);
  if (err != k_ra8_ok) {
    return err;
  }
  return selftest_print("), MRAM protected\r\n");
}

/**
 * @brief Bring the host controller up and enumerate the loop device.
 *
 * @details Initializes the USBHS host, enumerates the downstream FS
 * device over the cable, and prints its VID/PID. On any failure the
 * host controller is closed so the caller's retry starts clean.
 *
 * @param[out] out_device Receives the enumerated device snapshot.
 *
 * @return First failing step's error, or k_ra8_ok.
 * @retval k_ra8_ok Host up and device enumerated; identity printed.
 *
 * @pre Device-side class is registered and attached (other thread).
 * @pre @p out_device is non-NULL.
 * @post On k_ra8_ok the host controller is live and @p out_device filled.
 * @post On failure the host controller has been closed again.
 *
 * @note Blocking; runs on the low-priority host thread.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t selftest_host_enumerate(ra8_usb_hmsc_device_t* out_device)
{
  s_dbg_phase   = (uint32_t)k_selftest_phase_host_init;
  ra8_err_t err = selftest_print("ra8d2 selftest: host up on USB-FS, probing the loop...\r\n");
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_usb_hmsc_init(k_ra8_usb_speed_fs);
  if (err != k_ra8_ok) {
    (void)selftest_print_fail("host init", err);
    return err;
  }
  s_dbg_phase = (uint32_t)k_selftest_phase_enum;
  err         = ra8_usb_hmsc_enumerate(out_device);
  if (err != k_ra8_ok) {
    (void)selftest_print_fail("enumerate", err);
    (void)ra8_usb_hmsc_close();
    return err;
  }
  err = selftest_print("ra8d2 selftest: enumerated vid=0x");
  if (err != k_ra8_ok) {
    return err;
  }
  err = selftest_print_hex((uint32_t)out_device->vendor_id, (uint8_t)k_selftest_hex_chars_u16);
  if (err != k_ra8_ok) {
    return err;
  }
  err = selftest_print(" pid=0x");
  if (err != k_ra8_ok) {
    return err;
  }
  err = selftest_print_hex((uint32_t)out_device->product_id, (uint8_t)k_selftest_hex_chars_u16);
  if (err != k_ra8_ok) {
    return err;
  }
  return selftest_print(" over the loop cable\r\n");
}

/**
 * @brief One full host-side pass: enumerate, mount, verify, WP.
 *
 * @details Phases mirror ::selftest_phase_t and are mirrored into
 * ::s_dbg_phase for J-Link readout. On any failure the host controller
 * is closed so the next retry starts from a clean attach.
 *
 * @return First failing step's error, or k_ra8_ok.
 * @retval k_ra8_ok The pass printed CONFIG B PASS.
 *
 * @pre Device-side class is registered and attached (other thread).
 * @pre The self-loop cable connects J7 to J11.
 * @post On success ::s_dbg_pass_count advanced and LED2 is on.
 * @post On failure the host controller is deinitialized again.
 *
 * @note Blocking; runs on the low-priority host thread.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t selftest_host_pass(void)
{
  ra8_usb_hmsc_device_t device = {};
  ra8_err_t             err    = selftest_host_enumerate(&device);
  if (err != k_ra8_ok) {
    return err;
  }

  /* Mount proves the host can PARSE the FAT16 volume over the loop;
   * then unmount and do the heavy integrity check with raw multi-block
   * READ(10) (ra8_fs's per-cluster metadata re-reads are far too slow
   * for a 1 MiB sweep). */
  s_dbg_phase           = (uint32_t)k_selftest_phase_mount;
  ra8_fs_mount_t* mount = nullptr;
  err                   = selftest_mount_volume(&mount);
  if (err != k_ra8_ok) {
    (void)ra8_usb_hmsc_close();
    return err;
  }
  (void)ra8_fs_unmount(mount);

  s_dbg_phase = (uint32_t)k_selftest_phase_verify;
  err         = selftest_verify_mram_raw();
  if (err == k_ra8_ok) {
    err = selftest_print_verify_verdict();
  }
  if (err == k_ra8_ok) {
    s_dbg_phase = (uint32_t)k_selftest_phase_wp;
    err         = selftest_write_protect_probe();
  }
  if (err != k_ra8_ok) {
    (void)ra8_usb_hmsc_close();
    return err;
  }

  s_dbg_phase = (uint32_t)k_selftest_phase_pass;
  s_dbg_pass_count++;
  err = selftest_print("ra8d2 selftest: USB SELFTEST CONFIG B PASS\r\n");
  if (err != k_ra8_ok) {
    return err;
  }
  (void)ra8_board_led_on(k_ra8_board_led2);
  return k_ra8_ok;
}

/**
 * @brief Host-side worker: retry the full pass until it succeeds.
 *
 * @details Waits for the device side to attach, then loops
 * ::selftest_host_pass with a retry pause until the whole config B
 * ladder passes; afterwards parks so the verdict stays on the wire.
 *
 * @param[in] arg ThreadX entry argument (unused).
 *
 * @pre tx_application_define created this thread (lower priority than
 *      the USBX device-side threads).
 * @pre The FS host pins, VBUSEN, and 48 MHz clock are up (main).
 * @post On success the pass counter and LED2 are latched.
 * @post Retries forever otherwise; each failure prints its step.
 *
 * @note Polled host stack: blocking calls, ms timeouts via ra8_time.
 * @since 0.1.0
 */
VOID selftest_host_worker(ULONG arg)
{
  (void)arg;

  tx_thread_sleep(k_selftest_boot_wait_ticks);
  for (;;) {
    const ra8_err_t err = selftest_host_pass();
    if (err == k_ra8_ok) {
      break;
    }
    tx_thread_sleep(k_selftest_retry_ticks);
  }
  while (1) {
    tx_thread_sleep(k_selftest_idle_ticks);
  }
}

#endif /* !RA8_OFF_TARGET */
