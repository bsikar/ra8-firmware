/**
 * @file examples/ek_ra8d2/hw_validated/manual/usb_host_file_ops/main.c
 * @brief USB host MSC + ra_fs file-operations exerciser for EK-RA8D2 (USB-HS)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Manual HIL test that proves real file operations work end to end on a USB
 * thumb drive plugged into the J7 USB-HS jack: the board enumerates the stick
 * via ::ra_usb_hmsc_enumerate, mounts its FAT/exFAT volume through `ra_fs`
 * (block I/O bridged onto ::ra_usb_hmsc_read10 / ::ra_usb_hmsc_write10),
 * then runs a 9-step suite over the J-Link OB CDC console:
 *
 *  1. cleanup: best-effort unlink of leftover test files
 *  2. listdir of the root directory (every entry printed)
 *  3. write a new file `USBTEST.TXT` with a known payload
 *  4. open + read back + byte-compare the payload
 *  5. listdir must show `USBTEST.TXT`
 *  6. rename `USBTEST.TXT` -> `USBDONE.TXT`
 *  7. old name must be gone, new name must read back intact
 *  8. listdir must show `USBDONE.TXT` and not `USBTEST.TXT`
 *  9. unlink `USBDONE.TXT`; listdir must no longer show it
 *
 * Every step prints a pass/fail verdict; the run ends with
 * `ALL FILE OPS PASSED` and a solid LED1. The ladder retries every 5 s so a
 * freshly inserted drive is picked up automatically.
 *
 * Board specifics (EK-RA8D2 v1 UM):
 *  - PD07 HIGH = U18 supplies VBUS to J7 (Sec 6.2 p 34, 2 A budget).
 *  - SW4-8 must sit in the Host position; set in software through the
 *    U15 expander (::ra_board_io_expander_set_usbhs_host_mode).
 *  - P4_08 is the only PFS-muxed USBHS pin (VBUS sense, PSEL 0x14);
 *    D+/D- are dedicated PHY package balls.
 *
 * @author Brighton Sikarskie
 * @date 2026-06-12
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_fs.h"
#include "ra_gpio_constants.h"
#include "ra_isr.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_sci.h"
#include "ra_time.h"
#include "ra_usb.h"
#include "ra_usb_hmsc.h"

/* =============================================================================
 * Compile-time configuration
 * =============================================================================
 */

/**
 * @enum usb_fileops_config_t
 * @brief Compile-time settings for the host file-ops suite.
 */
typedef enum : uint32_t {
  k_fileops_baud        = 115200U, /**< J-Link OB CDC log baud.             */
  k_fileops_sci_channel = 8U,      /**< SCI8 -> J-Link OB CDC bridge.       */
  k_fileops_idle_ms     = 50U,     /**< Idle tick in the parked main loop.  */
  k_fileops_retry_ms    = 5000U,   /**< Pause between ladder retries.       */
  k_fileops_target_lun  = 0U,      /**< Exercise LUN 0 (typical stick).     */
  k_fileops_max_blocks  = 0xFFFFU, /**< READ(10)/WRITE(10) 16-bit count.    */
  k_fileops_print_cap   = 160U,    /**< Bound for console-string scans.     */
} usb_fileops_config_t;

/**
 * @enum usb_fileops_size_t
 * @brief Buffer sizing constants.
 */
typedef enum : uint16_t {
  k_fileops_sector_bytes = 512U, /**< One SCSI block / FAT sector.      */
  k_fileops_name_cap     = 64U,  /**< Listdir name compare bound.       */
} usb_fileops_size_t;

/**
 * @enum usb_fileops_hex_t
 * @brief Hex/decimal text-formatter sizing constants.
 */
typedef enum : uint8_t {
  k_fileops_hex_chars_u16   = 4U,  /**< 16-bit value -> "ABCD".         */
  k_fileops_hex_chars_u32   = 8U,  /**< 32-bit value -> "ABCDEF01".     */
  k_fileops_dec_chars_u32   = 10U, /**< Max digits for a 32-bit count.  */
  k_fileops_nibble_bits     = 4U,  /**< Bits per hex nibble.            */
  k_fileops_hex_digit_split = 10U, /**< Threshold between '0-9'/'A-F'.  */
} usb_fileops_hex_t;

/**
 * @enum usb_fileops_mask_t
 * @brief Bit-mask constants used by the text formatters.
 */
typedef enum : uint32_t {
  k_fileops_nibble_mask = 0xFU, /**< 4-bit nibble mask.           */
  k_fileops_dec_radix   = 10U,  /**< Base for decimal conversion. */
} usb_fileops_mask_t;

/**
 * @enum usb_fileops_probe_t
 * @brief Sizing for the on-mount-failure disk-layout probe dump.
 */
typedef enum : uint32_t {
  k_fileops_probe_tbl_off  = 432U, /**< LBA0 dump start (partition table). */
  k_fileops_probe_tbl_len  = 80U,  /**< LBA0 dump length (incl. 0x55AA).   */
  k_fileops_probe_head_len = 64U,  /**< LBA1/LBA2 head dump length.        */
  k_fileops_probe_row      = 16U,  /**< Hex bytes per dump row.            */
  k_fileops_probe_lba_max  = 3U,   /**< Probe reads LBAs 0..2.             */
} usb_fileops_probe_t;

/* =============================================================================
 * Pin assignments
 * =============================================================================
 */

/** @brief J-Link OB CDC TX pin (PD_02 -- SCI8 TX). */
static const ra_port_pin_t k_fileops_pin_sci_tx =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);

/** @brief J-Link OB CDC RX pin (PD_03 -- SCI8 RX). */
static const ra_port_pin_t k_fileops_pin_sci_rx =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);

/** @brief USBHS_VBUS sense pin (P4_08, PSEL = 0x14). */
static const ra_port_pin_t k_fileops_pin_hs_vbus =
  (ra_port_pin_t)(((uint16_t)k_ra_port_4 << 8) | (uint16_t)k_ra_pin_8);

/** @brief J7 host-power switch (PD07): HIGH = U18 supplies VBUS (UM 6.2). */
static const ra_port_pin_t k_fileops_pin_hs_pwr =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_7);

/** @brief Controller this app drives (the drive sits in the HS jack). */
static const ra_usb_speed_t k_fileops_speed = k_ra_usb_speed_hs;

/* =============================================================================
 * Test fixtures
 * =============================================================================
 */

/** @brief Name the payload file is created under (8.3-safe, <= 15 chars). */
static const char k_fileops_name_a[] = "USBTEST.TXT";

/** @brief Name the payload file is renamed to (8.3-safe, <= 15 chars). */
static const char k_fileops_name_b[] = "USBDONE.TXT";

/** @brief Known payload written to and read back from the drive. */
static const uint8_t k_fileops_payload[] =
  "ra8d2 usb-hs host file-ops payload 0123456789 the quick brown fox\r\n";

/* =============================================================================
 * Console helpers
 * =============================================================================
 */

/**
 * @brief Halt forever in WFI -- panic stop.
 *
 * @pre Called only after a fatal boot error.
 * @pre Interrupts may be in any state.
 * @post CPU is parked; only a debugger or external reset wakes it.
 * @post No further code runs.
 * @note Never returns.
 * @since 0.1.0
 */
static void fileops_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Format one nibble (0..15) into an uppercase hex character.
 *
 * @param[in] nibble 4-bit value.
 * @return ASCII '0'..'9' or 'A'..'F'.
 * @retval '0' For a zero nibble.
 * @pre Caller has already masked the value to 4 bits.
 * @pre None beyond the mask contract.
 * @post Returned byte is in the printable hex range.
 * @post No state changes.
 * @note Pure function.
 * @since 0.1.0
 */
static uint8_t fileops_nibble_to_hex(uint32_t nibble)
{
  if (nibble < k_fileops_hex_digit_split) {
    return (uint8_t)((uint8_t)'0' + (uint8_t)nibble);
  }
  return (uint8_t)((uint8_t)'A' + (uint8_t)nibble - (uint8_t)k_fileops_hex_digit_split);
}

/**
 * @brief Bounded ASCII string length (cap ::k_fileops_print_cap).
 *
 * @param[in] text NUL-terminated string.
 * @return Number of bytes before the NUL, capped.
 * @retval 0 For an empty string.
 * @pre @p text is non-NULL.
 * @pre @p text points to readable storage of at least the returned length.
 * @post No state changes.
 * @post Return value never exceeds ::k_fileops_print_cap.
 * @note Bounded scan -- never walks past the cap on a missing NUL.
 * @since 0.1.0
 */
static uint32_t fileops_str_len(const char* text)
{
  uint32_t len = 0U;
  while (len < (uint32_t)k_fileops_print_cap) {
    if (text[len] == '\0') {
      break;
    }
    len++;
  }
  return len;
}

/**
 * @brief Bounded ASCII string equality (cap ::k_fileops_name_cap).
 *
 * @param[in] a First NUL-terminated string.
 * @param[in] b Second NUL-terminated string.
 * @return true when both strings match through their terminators.
 * @retval false On the first differing byte.
 * @pre @p a and @p b are non-NULL.
 * @pre Both strings are NUL-terminated within the cap.
 * @post No state changes.
 * @post Comparison never walks past ::k_fileops_name_cap bytes.
 * @note Case-sensitive byte compare.
 * @since 0.1.0
 */
static bool fileops_name_eq(const char* a, const char* b)
{
  for (uint32_t i = 0U; i < (uint32_t)k_fileops_name_cap; i++) {
    if (a[i] != b[i]) {
      return false;
    }
    if (a[i] == '\0') {
      return true;
    }
  }
  return false;
}

/**
 * @brief Push a literal block over SCI8 polled.
 *
 * @param[in] data Buffer to send.
 * @param[in] len  Byte count.
 * @return ra_err_t passthrough from `ra_sci_write_polling`.
 * @retval k_ra_ok All bytes queued.
 * @pre @p data is non-NULL; SCI8 init already ran.
 * @pre @p len excludes any NUL terminator.
 * @post Bytes have been pushed out the SCI8 TX FIFO.
 * @post No other state changes.
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t fileops_sci_write(const uint8_t* data, uint32_t len)
{
  return ra_sci_write_polling((uint8_t)k_fileops_sci_channel, data, len);
}

/**
 * @brief Print a NUL-terminated ASCII string over the console.
 *
 * @param[in] text String to print (CR/LF included by the caller).
 * @return ra_err_t propagated from the SCI helper.
 * @retval k_ra_ok All bytes queued.
 * @pre SCI8 init already ran; @p text is non-NULL.
 * @pre @p text is NUL-terminated within ::k_fileops_print_cap bytes.
 * @post The string bytes are in the SCI8 TX FIFO.
 * @post No other state changes.
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t fileops_print(const char* text)
{
  return fileops_sci_write((const uint8_t*)text, fileops_str_len(text));
}

/**
 * @brief Print a uint32_t as ASCII decimal.
 *
 * @param[in] value Value to print.
 * @return ra_err_t propagated from the SCI helper.
 * @retval k_ra_ok All bytes queued.
 * @pre SCI8 init already ran.
 * @pre None beyond console readiness.
 * @post One ASCII decimal token is in the SCI8 TX FIFO.
 * @post No other state changes.
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t fileops_print_dec(uint32_t value)
{
  uint8_t  scratch[k_fileops_dec_chars_u32] = {};
  uint8_t  out[k_fileops_dec_chars_u32]     = {};
  uint8_t  count                            = 0U;
  uint32_t v                                = value;
  if (v == 0U) {
    out[0] = (uint8_t)'0';
    return fileops_sci_write(out, 1U);
  }
  while ((v != 0U) && (count < (uint8_t)k_fileops_dec_chars_u32)) {
    scratch[count] = (uint8_t)((uint8_t)'0' + (uint8_t)(v % k_fileops_dec_radix));
    v              = v / k_fileops_dec_radix;
    count++;
  }
  for (uint8_t i = 0U; i < count; i++) {
    out[i] = scratch[count - 1U - i];
  }
  return fileops_sci_write(out, (uint32_t)count);
}

/**
 * @brief Print a value as fixed-width uppercase hex.
 *
 * @param[in] value  Value to print.
 * @param[in] digits Hex digit count (4 for u16, 8 for u32).
 * @return ra_err_t propagated from the SCI helper.
 * @retval k_ra_ok All bytes queued.
 * @pre SCI8 init already ran.
 * @pre @p digits is at most ::k_fileops_hex_chars_u32.
 * @post One fixed-width hex token is in the SCI8 TX FIFO.
 * @post No other state changes.
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t fileops_print_hex(uint32_t value, uint8_t digits)
{
  uint8_t out[k_fileops_hex_chars_u32] = {};
  uint8_t width                        = digits;
  if (width > (uint8_t)k_fileops_hex_chars_u32) {
    width = (uint8_t)k_fileops_hex_chars_u32;
  }
  for (uint8_t i = 0U; i < width; i++) {
    const uint8_t shift = (uint8_t)((width - 1U - i) * k_fileops_nibble_bits);
    out[i]              = fileops_nibble_to_hex((value >> shift) & k_fileops_nibble_mask);
  }
  return fileops_sci_write(out, (uint32_t)width);
}

/**
 * @brief Print "FAIL <what> err=0xNNNNNNNN" on its own line.
 *
 * @param[in] what Short description of the failed step.
 * @param[in] err  Error code returned by the step.
 * @return ra_err_t propagated from the SCI helpers.
 * @retval k_ra_ok The diagnostic line is queued.
 * @pre SCI8 init already ran.
 * @pre @p what is NUL-terminated within the print cap.
 * @post One diagnostic line is in the SCI8 TX FIFO.
 * @post No other state changes.
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t fileops_print_fail(const char* what, ra_err_t err)
{
  ra_err_t e = fileops_print("ra8d2 fileops: FAIL ");
  if (e != k_ra_ok) {
    return e;
  }
  e = fileops_print(what);
  if (e != k_ra_ok) {
    return e;
  }
  e = fileops_print(" err=0x");
  if (e != k_ra_ok) {
    return e;
  }
  e = fileops_print_hex((uint32_t)err, (uint8_t)k_fileops_hex_chars_u32);
  if (e != k_ra_ok) {
    return e;
  }
  return fileops_print("\r\n");
}

/* =============================================================================
 * ra_fs block-device backend over USB host MSC
 * =============================================================================
 */

/**
 * @brief ra_fs backend: read blocks via SCSI READ(10).
 *
 * @param[in]  ctx   Unused backend cookie.
 * @param[in]  lba   First logical block address.
 * @param[in]  count Number of 512-byte blocks.
 * @param[out] buf   Destination buffer (count * 512 bytes).
 * @return ra_err_t from the host-MSC class layer.
 * @retval k_ra_err_invalid_arg @p count exceeds the READ(10) 16-bit field.
 * @pre ::ra_usb_hmsc_enumerate completed on the attached drive.
 * @pre @p buf is non-NULL and large enough for the transfer.
 * @post On k_ra_ok the buffer holds the device data.
 * @post No other state changes.
 * @note Blocking; bounded by the class-layer timeouts.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t
fileops_backend_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  (void)ctx;
  if (count > (uint32_t)k_fileops_max_blocks) {
    return k_ra_err_invalid_arg;
  }
  return ra_usb_hmsc_read10((uint8_t)k_fileops_target_lun, lba, (uint16_t)count, buf);
}

/**
 * @brief ra_fs backend: write blocks via SCSI WRITE(10).
 *
 * @param[in] ctx   Unused backend cookie.
 * @param[in] lba   First logical block address.
 * @param[in] count Number of 512-byte blocks.
 * @param[in] buf   Source buffer (count * 512 bytes).
 * @return ra_err_t from the host-MSC class layer.
 * @retval k_ra_err_invalid_arg @p count exceeds the WRITE(10) 16-bit field.
 * @pre ::ra_usb_hmsc_enumerate completed on the attached drive.
 * @pre @p buf is non-NULL and holds the full transfer.
 * @post On k_ra_ok the device sectors hold the buffer data.
 * @post No other state changes.
 * @note Blocking; bounded by the class-layer timeouts.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t
fileops_backend_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  (void)ctx;
  if (count > (uint32_t)k_fileops_max_blocks) {
    return k_ra_err_invalid_arg;
  }
  return ra_usb_hmsc_write10((uint8_t)k_fileops_target_lun, lba, (uint16_t)count, buf);
}

/**
 * @brief ra_fs backend: report capacity via SCSI READ_CAPACITY(10).
 *
 * @param[in]  ctx         Unused backend cookie.
 * @param[out] block_count Total number of blocks.
 * @param[out] block_size  Block size in bytes (512 for the suite).
 * @return ra_err_t from the host-MSC class layer.
 * @retval k_ra_ok Outputs are valid.
 * @pre ::ra_usb_hmsc_enumerate completed on the attached drive.
 * @pre Both output pointers are non-NULL.
 * @post On k_ra_ok both outputs are filled from the device.
 * @post No other state changes.
 * @note Blocking; bounded by the class-layer timeouts.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t
fileops_backend_capacity(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  (void)ctx;
  return ra_usb_hmsc_read_capacity((uint8_t)k_fileops_target_lun, block_count, block_size);
}

/**
 * @brief Map a detected filesystem type to a printable name.
 *
 * @param[in] type Mount-time detection result.
 * @return Static NUL-terminated name string.
 * @retval "exfat" For ::k_ra_fs_type_exfat.
 * @pre None -- total over the enum.
 * @pre @p type came from a populated mount struct.
 * @post No state changes.
 * @post Returned pointer references static storage.
 * @note Pure function.
 * @since 0.1.0
 */
static const char* fileops_fs_type_name(ra_fs_type_t type)
{
  switch (type) {
    case k_ra_fs_type_fat12:
      return "fat12";
    case k_ra_fs_type_fat16:
      return "fat16";
    case k_ra_fs_type_fat32:
      return "fat32";
    case k_ra_fs_type_exfat:
      return "exfat";
    case k_ra_fs_type_unknown:
    default:
      return "unknown";
  }
}

/**
 * @brief Mount the attached drive through the USB-MSC backend.
 *
 * @param[out] out_mount Receives the mount handle on success.
 * @return ra_err_t from ::ra_fs_mount.
 * @retval k_ra_ok Volume mounted; the type line was printed.
 * @pre ::ra_usb_hmsc_enumerate completed on the attached drive.
 * @pre @p out_mount is non-NULL.
 * @post On k_ra_ok the mount handle is live and must be unmounted later.
 * @post The "mounted fs=" line is queued on success.
 * @note Reads the MBR/BPB chain over USB.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t fileops_mount_volume(ra_fs_mount_t** out_mount)
{
  const ra_fs_backend_t backend = {
    .read_block   = fileops_backend_read,
    .write_block  = fileops_backend_write,
    .get_capacity = fileops_backend_capacity,
    .ctx          = nullptr,
  };
  ra_err_t err = ra_fs_mount(&backend, out_mount);
  if (err != k_ra_ok) {
    (void)fileops_print_fail("mount", err);
    return err;
  }
  err = fileops_print("ra8d2 fileops: mounted fs=");
  if (err != k_ra_ok) {
    return err;
  }
  err = fileops_print(fileops_fs_type_name((*out_mount)->type));
  if (err != k_ra_ok) {
    return err;
  }
  err = fileops_print(" base_lba=");
  if (err != k_ra_ok) {
    return err;
  }
  err = fileops_print_dec((*out_mount)->partition_base_lba);
  if (err != k_ra_ok) {
    return err;
  }
  return fileops_print("\r\n");
}

/* =============================================================================
 * Directory listing
 * =============================================================================
 */

/**
 * @struct fileops_listdir_ctx_t
 * @brief Cookie carried through ::ra_fs_listdir for printing + matching.
 */
typedef struct {
  const char* want;        /**< Name expected present (nullptr = none). */
  const char* avoid;       /**< Name expected absent (nullptr = none).  */
  uint8_t     found_want;  /**< 1 when @ref want was seen.              */
  uint8_t     found_avoid; /**< 1 when @ref avoid was seen.             */
  uint32_t    count;       /**< Total entries reported.                 */
} fileops_listdir_ctx_t;

/**
 * @brief Listdir callback: print one entry, match the expectation names.
 *
 * @param[in]     name NUL-terminated entry name.
 * @param[in]     attr FAT attribute byte.
 * @param[in]     size File size in bytes (0 for directories).
 * @param[in,out] ctx  ::fileops_listdir_ctx_t cookie.
 * @pre Invoked only by ::ra_fs_listdir with a valid cookie.
 * @pre SCI8 init already ran.
 * @post The entry line is queued; counters/flags are updated.
 * @post No other state changes.
 * @note Print errors are swallowed -- the walk must finish.
 * @since 0.1.0
 */
static void fileops_listdir_cb(const char* name, uint8_t attr, uint32_t size, void* ctx)
{
  fileops_listdir_ctx_t* c = (fileops_listdir_ctx_t*)ctx;
  c->count++;
  (void)fileops_print("ra8d2 fileops:   - ");
  (void)fileops_print(name);
  if ((attr & (uint8_t)k_ra_fs_attr_directory) != 0U) {
    (void)fileops_print(" <dir>");
  } else {
    (void)fileops_print(" size=");
    (void)fileops_print_dec(size);
  }
  (void)fileops_print("\r\n");
  if (c->want != nullptr) {
    if (fileops_name_eq(name, c->want)) {
      c->found_want = 1U;
    }
  }
  if (c->avoid != nullptr) {
    if (fileops_name_eq(name, c->avoid)) {
      c->found_avoid = 1U;
    }
  }
}

/**
 * @brief List the root directory and check presence/absence expectations.
 *
 * @param[in] mount Live mount handle.
 * @param[in] want  Name that must appear (nullptr = no check).
 * @param[in] avoid Name that must not appear (nullptr = no check).
 * @return ra_err_t verdict.
 * @retval k_ra_err_not_found @p want was not listed.
 * @retval k_ra_err_exists    @p avoid was listed.
 * @pre @p mount is a live handle from ::ra_fs_mount.
 * @pre SCI8 init already ran.
 * @post Every root entry plus the count line is queued.
 * @post No filesystem mutation occurs.
 * @note Walks the root directory over USB.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t
fileops_step_listdir(ra_fs_mount_t* mount, const char* want, const char* avoid)
{
  fileops_listdir_ctx_t ctx = {
    .want        = want,
    .avoid       = avoid,
    .found_want  = 0U,
    .found_avoid = 0U,
    .count       = 0U,
  };
  ra_err_t err = fileops_print("ra8d2 fileops: -- listdir / --\r\n");
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_fs_listdir(mount, "/", fileops_listdir_cb, &ctx);
  if (err != k_ra_ok) {
    (void)fileops_print_fail("listdir", err);
    return err;
  }
  err = fileops_print("ra8d2 fileops: entries=");
  if (err != k_ra_ok) {
    return err;
  }
  err = fileops_print_dec(ctx.count);
  if (err != k_ra_ok) {
    return err;
  }
  err = fileops_print("\r\n");
  if (err != k_ra_ok) {
    return err;
  }
  if (want != nullptr) {
    if (ctx.found_want == 0U) {
      (void)fileops_print_fail("listdir expected-name missing", k_ra_err_not_found);
      return k_ra_err_not_found;
    }
  }
  if (avoid != nullptr) {
    if (ctx.found_avoid != 0U) {
      (void)fileops_print_fail("listdir stale-name present", k_ra_err_exists);
      return k_ra_err_exists;
    }
  }
  return k_ra_ok;
}

/* =============================================================================
 * File-operation steps
 * =============================================================================
 */

/**
 * @brief Open @p path and verify it reads back the known payload.
 *
 * @param[in] mount Live mount handle.
 * @param[in] path  File expected to hold ::k_fileops_payload.
 * @return ra_err_t verdict.
 * @retval k_ra_err_invalid_size  Read length differs from the payload.
 * @retval k_ra_err_invalid_state A payload byte differs.
 * @pre @p mount is a live handle from ::ra_fs_mount.
 * @pre @p path names a file written with the payload.
 * @post The file is closed again in every path.
 * @post No filesystem mutation occurs.
 * @note Reads the file content over USB.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t fileops_read_back(ra_fs_mount_t* mount, const char* path)
{
  static uint8_t s_read_buf[k_fileops_sector_bytes] = {};

  const uint32_t payload_len = (uint32_t)(sizeof(k_fileops_payload) - 1U);
  ra_fs_file_t*  file        = nullptr;
  ra_err_t       err         = ra_fs_open(mount, path, k_ra_fs_mode_read, &file);
  if (err != k_ra_ok) {
    (void)fileops_print_fail("open for read-back", err);
    return err;
  }
  uint32_t got = 0U;
  err          = ra_fs_read(file, s_read_buf, (uint32_t)sizeof(s_read_buf), &got);
  (void)ra_fs_close(file);
  if (err != k_ra_ok) {
    (void)fileops_print_fail("read", err);
    return err;
  }
  if (got != payload_len) {
    (void)fileops_print_fail("read-back length", k_ra_err_invalid_size);
    return k_ra_err_invalid_size;
  }
  for (uint32_t i = 0U; i < payload_len; i++) {
    if (s_read_buf[i] != k_fileops_payload[i]) {
      (void)fileops_print_fail("read-back content", k_ra_err_invalid_state);
      return k_ra_err_invalid_state;
    }
  }
  return k_ra_ok;
}

/**
 * @brief Require that @p path no longer resolves on the volume.
 *
 * @param[in] mount Live mount handle.
 * @param[in] path  Name that must not exist.
 * @return ra_err_t verdict.
 * @retval k_ra_err_exists The name still opened successfully.
 * @pre @p mount is a live handle from ::ra_fs_mount.
 * @pre @p path was unlinked or renamed away.
 * @post Any accidentally opened handle is closed again.
 * @post No filesystem mutation occurs.
 * @note A lookup error other than not-found is propagated as-is.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t fileops_expect_absent(ra_fs_mount_t* mount, const char* path)
{
  ra_fs_file_t* file = nullptr;
  ra_err_t      err  = ra_fs_open(mount, path, k_ra_fs_mode_read, &file);
  if (err == k_ra_ok) {
    (void)ra_fs_close(file);
    (void)fileops_print_fail("name still present", k_ra_err_exists);
    return k_ra_err_exists;
  }
  if (err != k_ra_err_not_found) {
    (void)fileops_print_fail("absence lookup", err);
    return err;
  }
  return k_ra_ok;
}

/**
 * @brief Step 3: print the banner and write the payload file.
 *
 * @param[in] mount Live mount handle.
 * @return First failing print/write error, or k_ra_ok.
 * @retval k_ra_ok ::k_fileops_name_a holds the payload.
 * @pre @p mount is a live handle from ::ra_fs_mount.
 * @pre Leftover test files were already unlinked.
 * @post On k_ra_ok the volume holds ::k_fileops_name_a with the payload.
 * @post The step banner is queued on the console.
 * @note Mutates the volume (creates the test file).
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t fileops_step_write(ra_fs_mount_t* mount)
{
  ra_err_t err = fileops_print("ra8d2 fileops: [3/9] write USBTEST.TXT len=");
  if (err != k_ra_ok) {
    return err;
  }
  err = fileops_print_dec((uint32_t)(sizeof(k_fileops_payload) - 1U));
  if (err != k_ra_ok) {
    return err;
  }
  err = fileops_print("\r\n");
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_fs_write_file(mount,
                         k_fileops_name_a,
                         k_fileops_payload,
                         (uint32_t)(sizeof(k_fileops_payload) - 1U));
  if (err != k_ra_ok) {
    (void)fileops_print_fail("write", err);
    return err;
  }
  return k_ra_ok;
}

/**
 * @brief Suite steps 1..5: cleanup, baseline listdir, write, verify, list.
 *
 * @param[in] mount Live mount handle.
 * @return First failing step's error, or k_ra_ok.
 * @retval k_ra_ok Steps 1-5 all passed and printed their verdicts.
 * @pre @p mount is a live handle from ::ra_fs_mount.
 * @pre SCI8 init already ran.
 * @post On k_ra_ok the volume holds ::k_fileops_name_a with the payload.
 * @post Each completed step toggled LED2 and queued its verdict line.
 * @note Mutates the volume (creates the test file).
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t fileops_suite_create(ra_fs_mount_t* mount)
{
  ra_err_t err = fileops_print("ra8d2 fileops: [1/9] cleanup leftovers\r\n");
  if (err != k_ra_ok) {
    return err;
  }
  (void)ra_fs_unlink(mount, k_fileops_name_a);
  (void)ra_fs_unlink(mount, k_fileops_name_b);
  (void)ra_board_led_toggle(k_ra_board_led2);

  err = fileops_print("ra8d2 fileops: [2/9] baseline root listing\r\n");
  if (err != k_ra_ok) {
    return err;
  }
  err = fileops_step_listdir(mount, nullptr, nullptr);
  if (err != k_ra_ok) {
    return err;
  }
  (void)ra_board_led_toggle(k_ra_board_led2);

  err = fileops_step_write(mount);
  if (err != k_ra_ok) {
    return err;
  }
  (void)ra_board_led_toggle(k_ra_board_led2);

  err = fileops_print("ra8d2 fileops: [4/9] read back + verify payload\r\n");
  if (err != k_ra_ok) {
    return err;
  }
  err = fileops_read_back(mount, k_fileops_name_a);
  if (err != k_ra_ok) {
    return err;
  }
  (void)ra_board_led_toggle(k_ra_board_led2);

  err = fileops_print("ra8d2 fileops: [5/9] listdir must show USBTEST.TXT\r\n");
  if (err != k_ra_ok) {
    return err;
  }
  err = fileops_step_listdir(mount, k_fileops_name_a, nullptr);
  if (err != k_ra_ok) {
    return err;
  }
  (void)ra_board_led_toggle(k_ra_board_led2);
  return k_ra_ok;
}

/**
 * @brief Suite steps 6..9: rename, old-gone/new-intact, list, delete.
 *
 * @param[in] mount Live mount handle.
 * @return First failing step's error, or k_ra_ok.
 * @retval k_ra_ok Steps 6-9 all passed and printed their verdicts.
 * @pre ::fileops_suite_create completed on this mount.
 * @pre SCI8 init already ran.
 * @post On k_ra_ok the volume no longer holds either test name.
 * @post Each completed step toggled LED2 and queued its verdict line.
 * @note Mutates the volume (rename + unlink).
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t fileops_suite_mutate(ra_fs_mount_t* mount)
{
  ra_err_t err = fileops_print("ra8d2 fileops: [6/9] rename USBTEST.TXT -> USBDONE.TXT\r\n");
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_fs_rename(mount, k_fileops_name_a, k_fileops_name_b);
  if (err != k_ra_ok) {
    (void)fileops_print_fail("rename", err);
    return err;
  }
  (void)ra_board_led_toggle(k_ra_board_led2);

  err = fileops_print("ra8d2 fileops: [7/9] old name gone, new name intact\r\n");
  if (err != k_ra_ok) {
    return err;
  }
  err = fileops_expect_absent(mount, k_fileops_name_a);
  if (err != k_ra_ok) {
    return err;
  }
  err = fileops_read_back(mount, k_fileops_name_b);
  if (err != k_ra_ok) {
    return err;
  }
  (void)ra_board_led_toggle(k_ra_board_led2);

  err = fileops_print("ra8d2 fileops: [8/9] listdir must show USBDONE.TXT only\r\n");
  if (err != k_ra_ok) {
    return err;
  }
  err = fileops_step_listdir(mount, k_fileops_name_b, k_fileops_name_a);
  if (err != k_ra_ok) {
    return err;
  }
  (void)ra_board_led_toggle(k_ra_board_led2);

  err = fileops_print("ra8d2 fileops: [9/9] unlink USBDONE.TXT\r\n");
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_fs_unlink(mount, k_fileops_name_b);
  if (err != k_ra_ok) {
    (void)fileops_print_fail("unlink", err);
    return err;
  }
  err = fileops_expect_absent(mount, k_fileops_name_b);
  if (err != k_ra_ok) {
    return err;
  }
  err = fileops_step_listdir(mount, nullptr, k_fileops_name_b);
  if (err != k_ra_ok) {
    return err;
  }
  (void)ra_board_led_toggle(k_ra_board_led2);
  return k_ra_ok;
}

/* =============================================================================
 * Mount-failure layout probe (diagnostic)
 * =============================================================================
 */

/**
 * @brief Hex-dump @p len bytes of @p data starting at @p offset.
 *
 * @param[in] data   Sector buffer.
 * @param[in] offset First byte index to print.
 * @param[in] len    Byte count to print.
 * @pre @p data holds at least @p offset + @p len bytes.
 * @pre SCI8 init already ran.
 * @post The dump rows are queued on the console.
 * @post No other state changes.
 * @note Print errors are swallowed -- diagnostic only.
 * @since 0.1.0
 */
static void fileops_dump_rows(const uint8_t* data, uint32_t offset, uint32_t len)
{
  for (uint32_t i = 0U; i < len; i++) {
    if ((i % (uint32_t)k_fileops_probe_row) == 0U) {
      (void)fileops_print("ra8d2 fileops:   0x");
      (void)fileops_print_hex(offset + i, (uint8_t)k_fileops_hex_chars_u16);
      (void)fileops_print(":");
    }
    (void)fileops_print(" ");
    (void)fileops_print_hex((uint32_t)data[offset + i], 2U);
    if ((i % (uint32_t)k_fileops_probe_row) == ((uint32_t)k_fileops_probe_row - 1U)) {
      (void)fileops_print("\r\n");
    }
  }
}

/**
 * @brief Dump the partition table + LBA1/LBA2 heads after a mount failure.
 *
 * @details Reads LBA 0 (MBR partition entries at 0x1BE), LBA 1, and LBA 2
 * straight through the USB backend and prints hex excerpts, so an
 * unrecognized disk layout (e.g. GPT) can be identified from the log.
 *
 * @pre ::ra_usb_hmsc_enumerate completed on the attached drive.
 * @pre SCI8 init already ran.
 * @post Three dump blocks are queued on the console.
 * @post No filesystem state changes.
 * @note Print/read errors are swallowed -- diagnostic only.
 * @since 0.1.0
 */
static void fileops_probe_layout(void)
{
  static uint8_t s_probe_buf[k_fileops_sector_bytes] = {};

  for (uint32_t lba = 0U; lba < (uint32_t)k_fileops_probe_lba_max; lba++) {
    if (fileops_backend_read(nullptr, lba, 1U, s_probe_buf) != k_ra_ok) {
      (void)fileops_print("ra8d2 fileops: probe read failed\r\n");
      return;
    }
    (void)fileops_print("ra8d2 fileops: probe lba=");
    (void)fileops_print_dec(lba);
    (void)fileops_print("\r\n");
    if (lba == 0U) {
      fileops_dump_rows(s_probe_buf,
                        (uint32_t)k_fileops_probe_tbl_off,
                        (uint32_t)k_fileops_probe_tbl_len);
    } else {
      fileops_dump_rows(s_probe_buf, 0U, (uint32_t)k_fileops_probe_head_len);
    }
  }
}

/* =============================================================================
 * Bring-up + ladder
 * =============================================================================
 */

/**
 * @brief Bring CGC + USB60CLK + SysTick + SCI8 + LEDs + USB host up.
 *        Panic-halts on any failure.
 *
 * @details Board steps for the J7 host role: PD07 HIGH (U18 supplies the
 * jack), SW4-8 to Host via the U15 expander, P4_08 routed to USBHS_VBUS,
 * then `ra_usb_hmsc_init` (which runs the full HS PHY bring-up).
 *
 * @pre Reset_Handler has finished C runtime init.
 * @pre SystemInit has run.
 * @post Console prints work; the USBHS controller is in host mode.
 * @post LED1/LED2 are initialized.
 * @note Called exactly once from main.
 * @since 0.1.0
 */
static void fileops_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;
  if (ra_cgc_init() != k_ra_ok) {
    fileops_panic_halt();
  }
  if (ra_cgc_usbhs_pll_enable() != k_ra_ok) {
    fileops_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    fileops_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz) != k_ra_ok) {
    fileops_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    fileops_panic_halt();
  }
  if (ra_pfs_route_peripheral(k_fileops_pin_sci_tx, k_ra_psel_sci_async, "fileops.txd8") !=
      k_ra_ok) {
    fileops_panic_halt();
  }
  if (ra_pfs_route_peripheral(k_fileops_pin_sci_rx, k_ra_psel_sci_async, "fileops.rxd8") !=
      k_ra_ok) {
    fileops_panic_halt();
  }
  const ra_sci_cfg_t sci_cfg = {
    .baud      = k_fileops_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_fileops_sci_channel, &sci_cfg) != k_ra_ok) {
    fileops_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    fileops_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led2) != k_ra_ok) {
    fileops_panic_halt();
  }
  if (ra_board_io_expander_set_usbhs_host_mode() != k_ra_ok) {
    fileops_panic_halt();
  }
  if (ra_gpio_output_init(k_fileops_pin_hs_pwr, k_ra_level_high) != k_ra_ok) {
    fileops_panic_halt();
  }
  if (ra_pfs_route_peripheral(k_fileops_pin_hs_vbus, k_ra_psel_usb_hs, "fileops.hs_vbus") !=
      k_ra_ok) {
    fileops_panic_halt();
  }
  if (ra_usb_hmsc_init(k_fileops_speed) != k_ra_ok) {
    fileops_panic_halt();
  }
}

/**
 * @brief One full pass: enumerate, mount, run the 9-step file-op suite.
 *
 * @details The mount handle is always released before returning so the
 * static mount-slot pool cannot leak across retry cycles.
 *
 * @return First failing step's error, or k_ra_ok.
 * @retval k_ra_ok The suite printed `ALL FILE OPS PASSED`.
 * @pre ::fileops_setup_or_halt completed.
 * @pre The USB drive is inserted in the HS jack.
 * @post On success LED1 is on and all verdict lines are printed.
 * @post The volume's test files are removed again (suite is self-cleaning).
 * @note Blocking; bounded by the class-layer timeouts.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t fileops_run_ladder(void)
{
  ra_usb_hmsc_device_t device = {};
  ra_err_t             err    = ra_usb_hmsc_enumerate(&device);
  if (err != k_ra_ok) {
    return err;
  }
  err = fileops_print("ra8d2 fileops: device attached vid=0x");
  if (err != k_ra_ok) {
    return err;
  }
  err = fileops_print_hex((uint32_t)device.vendor_id, (uint8_t)k_fileops_hex_chars_u16);
  if (err != k_ra_ok) {
    return err;
  }
  err = fileops_print(" pid=0x");
  if (err != k_ra_ok) {
    return err;
  }
  err = fileops_print_hex((uint32_t)device.product_id, (uint8_t)k_fileops_hex_chars_u16);
  if (err != k_ra_ok) {
    return err;
  }
  err = fileops_print("\r\n");
  if (err != k_ra_ok) {
    return err;
  }

  ra_fs_mount_t* mount = nullptr;
  err                  = fileops_mount_volume(&mount);
  if (err != k_ra_ok) {
    fileops_probe_layout();
    return err;
  }

  err = fileops_suite_create(mount);
  if (err == k_ra_ok) {
    err = fileops_suite_mutate(mount);
  }
  (void)ra_fs_unmount(mount);
  if (err != k_ra_ok) {
    return err;
  }

  err = fileops_print("ra8d2 fileops: ALL FILE OPS PASSED\r\n");
  if (err != k_ra_ok) {
    return err;
  }
  (void)ra_board_led_on(k_ra_board_led1);
  return k_ra_ok;
}

/* =============================================================================
 * Entry point
 * =============================================================================
 */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry. Brings up the HS host and runs the file-op
 *        suite until it fully passes.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 * @post On a clean run every suite verdict line is printed, LED1 is lit,
 *       and the CPU parks in WFI.
 * @post On any HAL init failure the function halts in WFI.
 * @since 0.1.0
 */
int32_t main(void)
{
  fileops_setup_or_halt();

  ra_isr_globals_enable();

  if (fileops_print("ra8d2 fileops: ready (USB-HS), plug a USB drive into the HS jack\r\n") !=
      k_ra_ok) {
    fileops_panic_halt();
  }

  /* Run the suite until it fully passes: a freshly inserted (or reseated)
   * drive is picked up by the next retry cycle automatically. */
  ra_err_t err = fileops_run_ladder();
  while (err != k_ra_ok) {
    (void)fileops_print_fail("ladder", err);
    (void)fileops_print("ra8d2 fileops: retrying in 5 s (reseat the drive any time)\r\n");
    ra_delay_ms(k_fileops_retry_ms);
    err = fileops_run_ladder();
  }

  while (1) {
    ra_delay_ms(k_fileops_idle_ms);
    __asm__ volatile("wfi");
  }

  return 0;
}
#pragma GCC diagnostic pop
