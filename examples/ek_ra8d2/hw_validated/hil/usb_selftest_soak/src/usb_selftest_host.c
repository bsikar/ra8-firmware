/**
 * @file usb_selftest_host.c
 * @brief Host side of the soak self-test: enumerate, mount, verify, soak
 *
 * @details
 * The polled first-party host MSC stack (ra8_usb_hmsc + ra8_fs) over the loop
 * cable: enumerate the device, mount its FAT16 volume, then repeat the raw 1 MiB
 * READ(10) integrity sweep k_selftest_soak_iters times (memcmp vs MRAM), confirm
 * the RO write rejection, and print the aggregate throughput. selftest_host_pass
 * is the one entry the host worker (main.c) drives; the J-Link probe vars live
 * here too. Split out of main.c (see usb_selftest_common.h).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "usb_selftest_host.h"

#include <stdint.h>
#include <string.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_time.h"
#include "ra8_usb_hmsc.h"
#include "usb_selftest_common.h"
#include "usb_selftest_console.h"

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
/* Host side: ra8_fs backend over the polled host-MSC class */
/* -------------------------------------------------------------------------- */

/**
 * @brief ra8_fs backend: read blocks via SCSI READ(10) over the loop.
 *
 * @details Single-LUN device; the 16-bit READ(10) count bound is upheld
 * by the small chunk sizes the suite issues.
 *
 * @param[in]  ctx   Unused backend cookie.
 * @param[in]  lba   First logical block address.
 * @param[in]  count Number of 512-byte blocks.
 * @param[out] buf   Destination buffer (count * 512 bytes).
 *
 * @return ra8_err_t from the host-MSC class layer.
 * @retval k_ra8_ok Blocks transferred.
 *
 * @pre ::ra8_usb_hmsc_enumerate completed on the loop device.
 * @pre @p buf is non-NULL and large enough for the transfer.
 * @post On k_ra8_ok the buffer holds the device data.
 * @post No other state changes.
 *
 * @note Blocking; bounded by the class-layer timeouts.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t
selftest_backend_read(void* ctx, uint64_t lba, uint32_t count, uint8_t* buf)
{
  (void)ctx;
  if (lba > (uint64_t)UINT32_MAX) {
    /* SCSI READ(10)/WRITE(10) carry 32-bit LBAs; past-2-TiB addressing is a
     * 64-bit-native-backend capability this transport cannot reach (#683). */
    return k_ra8_err_out_of_range;
  }
  return ra8_usb_hmsc_read10((uint8_t)k_selftest_target_lun, (uint32_t)lba, (uint16_t)count, buf);
}

/**
 * @brief ra8_fs backend: write blocks via SCSI WRITE(10) over the loop.
 *
 * @details The device LUN is write-protected; this path only exists so
 * the backend struct is total. ra8_fs never writes during the read-only
 * suite.
 *
 * @param[in] ctx   Unused backend cookie.
 * @param[in] lba   First logical block address.
 * @param[in] count Number of 512-byte blocks.
 * @param[in] buf   Source buffer (count * 512 bytes).
 *
 * @return ra8_err_t from the host-MSC class layer.
 * @retval k_ra8_ok Blocks transferred (never for this device).
 *
 * @pre ::ra8_usb_hmsc_enumerate completed on the loop device.
 * @pre @p buf is non-NULL and holds the full transfer.
 * @post On success the device sectors hold the buffer data.
 * @post No other state changes.
 *
 * @note Blocking; bounded by the class-layer timeouts.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t
selftest_backend_write(void* ctx, uint64_t lba, uint32_t count, const uint8_t* buf)
{
  (void)ctx;
  if (lba > (uint64_t)UINT32_MAX) {
    /* SCSI READ(10)/WRITE(10) carry 32-bit LBAs; past-2-TiB addressing is a
     * 64-bit-native-backend capability this transport cannot reach (#683). */
    return k_ra8_err_out_of_range;
  }
  return ra8_usb_hmsc_write10((uint8_t)k_selftest_target_lun, (uint32_t)lba, (uint16_t)count, buf);
}

/**
 * @brief ra8_fs backend: report capacity via SCSI READ_CAPACITY(10).
 *
 * @details Pass-through to the class layer for LUN 0.
 *
 * @param[in]  ctx         Unused backend cookie.
 * @param[out] block_count Total number of blocks.
 * @param[out] block_size  Block size in bytes (512 for this volume).
 *
 * @return ra8_err_t from the host-MSC class layer.
 * @retval k_ra8_ok Outputs are valid.
 *
 * @pre ::ra8_usb_hmsc_enumerate completed on the loop device.
 * @pre Both output pointers are non-NULL.
 * @post On k_ra8_ok both outputs are filled from the device.
 * @post No other state changes.
 *
 * @note Blocking; bounded by the class-layer timeouts.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t
selftest_backend_capacity(void* ctx, uint64_t* block_count, uint32_t* block_size)
{
  (void)ctx;
  uint32_t        blocks32 = 0U;
  const ra8_err_t err =
    ra8_usb_hmsc_read_capacity((uint8_t)k_selftest_target_lun, &blocks32, block_size);
  if (err != k_ra8_ok) {
    return err;
  }
  *block_count = blocks32;
  return k_ra8_ok;
}

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
 * @details Builds the backend vtable over the polled host class and
 * prints the detected filesystem type (must be fat16 for this device).
 *
 * @param[out] out_mount Receives the mount handle on success.
 *
 * @return ra8_err_t from ::ra8_fs_mount.
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
  const ra8_fs_backend_t backend = {
    .read_block   = selftest_backend_read,
    .write_block  = selftest_backend_write,
    .get_capacity = selftest_backend_capacity,
    .ctx          = nullptr,
  };
  ra8_err_t err = ra8_fs_mount(&backend, out_mount);
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
  ra8_err_t err = selftest_print("ra8d2 selftest: host up on USB-HS, probing the loop...\r\n");
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_usb_hmsc_init(k_ra8_usb_speed_hs);
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
 * @brief Print the soak aggregate line (iterations + volume + rate).
 *
 * @details "soak: N iters, M MiB verified in T ms (R KiB/s), 0 errors".
 *
 * @param[in] total_bytes Sum of all bytes verified across the soak.
 * @param[in] total_ms    Sum of all per-iteration verify durations (ms).
 *
 * @return ra8_err_t propagated from the SCI helpers.
 * @retval k_ra8_ok The aggregate line is queued.
 *
 * @pre The soak loop completed every iteration with k_ra8_ok.
 * @pre SCI8 init already ran.
 * @post One aggregate line is in the SCI8 TX FIFO.
 *
 * @note Rate math guards the divide against a zero duration.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t selftest_print_soak_verdict(uint64_t total_bytes, uint64_t total_ms)
{
  uint64_t ms = total_ms;
  if (ms == 0U) {
    ms = 1U;
  }
  const uint32_t kib_per_s = (uint32_t)((total_bytes * (uint64_t)k_selftest_ms_per_sec) /
                                        (ms * (uint64_t)k_selftest_bytes_per_kib));
  ra8_err_t      err       = selftest_print("ra8d2 soak: ");
  if (err == k_ra8_ok) {
    err = selftest_print_dec((uint32_t)k_selftest_soak_iters);
  }
  if (err == k_ra8_ok) {
    err = selftest_print(" iters, ");
  }
  if (err == k_ra8_ok) {
    err = selftest_print_dec((uint32_t)(total_bytes / (uint64_t)k_selftest_bytes_per_mib));
  }
  if (err == k_ra8_ok) {
    err = selftest_print(" MiB verified in ");
  }
  if (err == k_ra8_ok) {
    err = selftest_print_dec((uint32_t)total_ms);
  }
  if (err == k_ra8_ok) {
    err = selftest_print(" ms (");
  }
  if (err == k_ra8_ok) {
    err = selftest_print_dec(kib_per_s);
  }
  if (err == k_ra8_ok) {
    err = selftest_print(" KiB/s), 0 errors\r\n");
  }
  return err;
}

/**
 * @brief Soak pass: enumerate + mount once, then repeat the 1 MiB verify.
 *
 * @details Enumerates and mounts to prove FAT parse, then runs
 * ::selftest_verify_mram_raw ::k_selftest_soak_iters times back to back --
 * every burst is still memcmp'd vs MRAM, so a single corrupted transfer
 * anywhere in the soak fails the run. Aggregates volume + time for a
 * stable throughput benchmark, then confirms the RO write rejection once.
 * ::s_dbg_pass_count mirrors the completed-iteration count for J-Link.
 *
 * @return First failing step's error, or k_ra8_ok.
 * @retval k_ra8_ok All iterations matched; SOAK PASS printed.
 *
 * @pre Device-side class is registered and attached (other thread).
 * @pre The self-loop cable connects J7 to J11.
 * @post On success LED2 is on and the aggregate + PASS lines are queued.
 * @post On failure the host controller is closed for a clean retry.
 *
 * @note Blocking; ~k_selftest_soak_iters MiB over the loop.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t selftest_host_pass(void)
{
  ra8_usb_hmsc_device_t device = {};
  ra8_err_t             err    = selftest_host_enumerate(&device);
  if (err != k_ra8_ok) {
    return err;
  }
  s_dbg_phase           = (uint32_t)k_selftest_phase_mount;
  ra8_fs_mount_t* mount = nullptr;
  err                   = selftest_mount_volume(&mount);
  if (err != k_ra8_ok) {
    (void)ra8_usb_hmsc_close();
    return err;
  }
  (void)ra8_fs_unmount(mount);

  s_dbg_phase          = (uint32_t)k_selftest_phase_verify;
  uint64_t total_bytes = 0U;
  uint64_t total_ms    = 0U;
  for (uint32_t it = 0U; it < (uint32_t)k_selftest_soak_iters; it++) {
    err = selftest_verify_mram_raw();
    if (err != k_ra8_ok) {
      (void)ra8_usb_hmsc_close();
      return err;
    }
    total_bytes += (uint64_t)s_dbg_verified_bytes;
    total_ms += (uint64_t)s_dbg_verify_ms;
    s_dbg_pass_count = it + 1U;
  }

  s_dbg_phase = (uint32_t)k_selftest_phase_wp;
  err         = selftest_write_protect_probe();
  if (err == k_ra8_ok) {
    err = selftest_print_soak_verdict(total_bytes, total_ms);
  }
  if (err != k_ra8_ok) {
    (void)ra8_usb_hmsc_close();
    return err;
  }
  s_dbg_phase = (uint32_t)k_selftest_phase_pass;
  err         = selftest_print("ra8d2 soak: USB SELFTEST SOAK PASS\r\n");
  if (err != k_ra8_ok) {
    return err;
  }
  (void)ra8_board_led_on(k_ra8_board_led2);
  return k_ra8_ok;
}
