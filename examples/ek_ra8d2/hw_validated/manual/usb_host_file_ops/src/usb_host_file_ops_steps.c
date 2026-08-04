/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file
 * examples/ek_ra8d2/hw_validated/manual/usb_host_file_ops/src/usb_host_file_ops_steps.c
 * @brief Console helpers + ra8_fs file-op suite for the USB host file-ops app.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Implements the polled-console print helpers, the ra8_fs block-device backend
 * bridged onto the USB host-MSC class layer, the root-directory listing, the
 * nine file-operation steps, and the on-mount-failure disk-layout probe. These
 * routines were split out of `main.c` so every translation unit stays under the
 * repository file-size cap; the boot/bring-up code and the retry ladder remain
 * in `main.c`.
 *
 * @author Brighton Sikarskie
 * @date 2026-06-12
 * @since 0.1.0
 */

#include "usb_host_file_ops_steps.h"

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_usb.h"
#include "ra8_usb_hmsc.h"

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
 * @return ra8_err_t passthrough from `ra8_board_uart_console_write`.
 * @retval k_ra8_ok All bytes queued.
 * @pre @p data is non-NULL; SCI8 init already ran.
 * @pre @p len excludes any NUL terminator.
 * @post Bytes have been pushed out the SCI8 TX FIFO.
 * @post No other state changes.
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t fileops_sci_write(const uint8_t* data, uint32_t len)
{
  return ra8_board_uart_console_write(data, (size_t)len);
}

[[nodiscard]] ra8_err_t fileops_print(const char* text)
{
  return fileops_sci_write((const uint8_t*)text, fileops_str_len(text));
}

[[nodiscard]] ra8_err_t fileops_print_dec(uint32_t value)
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

[[nodiscard]] ra8_err_t fileops_print_hex(uint32_t value, uint8_t digits)
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

[[nodiscard]] ra8_err_t fileops_print_fail(const char* what, ra8_err_t err)
{
  ra8_err_t e = fileops_print("ra8d2 fileops: FAIL ");
  if (e != k_ra8_ok) {
    return e;
  }
  e = fileops_print(what);
  if (e != k_ra8_ok) {
    return e;
  }
  e = fileops_print(" err=0x");
  if (e != k_ra8_ok) {
    return e;
  }
  e = fileops_print_hex((uint32_t)err, (uint8_t)k_fileops_hex_chars_u32);
  if (e != k_ra8_ok) {
    return e;
  }
  return fileops_print("\r\n");
}

/* =============================================================================
 * ra8_fs block-device backend over USB host MSC
 * =============================================================================
 */

/**
 * @brief ra8_fs backend: read blocks via SCSI READ(10).
 *
 * @param[in]  ctx   Unused backend cookie.
 * @param[in]  lba   First logical block address.
 * @param[in]  count Number of 512-byte blocks.
 * @param[out] buf   Destination buffer (count * 512 bytes).
 * @return ra8_err_t from the host-MSC class layer.
 * @retval k_ra8_err_invalid_arg @p count exceeds the READ(10) 16-bit field.
 * @pre ::ra8_usb_hmsc_enumerate completed on the attached drive.
 * @pre @p buf is non-NULL and large enough for the transfer.
 * @post On k_ra8_ok the buffer holds the device data.
 * @post No other state changes.
 * @note Blocking; bounded by the class-layer timeouts.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t
fileops_backend_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  (void)ctx;
  if (count > (uint32_t)k_fileops_max_blocks) {
    return k_ra8_err_invalid_arg;
  }
  return ra8_usb_hmsc_read10((uint8_t)k_fileops_target_lun, lba, (uint16_t)count, buf);
}

/**
 * @brief ra8_fs backend: write blocks via SCSI WRITE(10).
 *
 * @param[in] ctx   Unused backend cookie.
 * @param[in] lba   First logical block address.
 * @param[in] count Number of 512-byte blocks.
 * @param[in] buf   Source buffer (count * 512 bytes).
 * @return ra8_err_t from the host-MSC class layer.
 * @retval k_ra8_err_invalid_arg @p count exceeds the WRITE(10) 16-bit field.
 * @pre ::ra8_usb_hmsc_enumerate completed on the attached drive.
 * @pre @p buf is non-NULL and holds the full transfer.
 * @post On k_ra8_ok the device sectors hold the buffer data.
 * @post No other state changes.
 * @note Blocking; bounded by the class-layer timeouts.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t
fileops_backend_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  (void)ctx;
  if (count > (uint32_t)k_fileops_max_blocks) {
    return k_ra8_err_invalid_arg;
  }
  return ra8_usb_hmsc_write10((uint8_t)k_fileops_target_lun, lba, (uint16_t)count, buf);
}

/**
 * @brief ra8_fs backend: report capacity via SCSI READ_CAPACITY(10).
 *
 * @param[in]  ctx         Unused backend cookie.
 * @param[out] block_count Total number of blocks.
 * @param[out] block_size  Block size in bytes (512 for the suite).
 * @return ra8_err_t from the host-MSC class layer.
 * @retval k_ra8_ok Outputs are valid.
 * @pre ::ra8_usb_hmsc_enumerate completed on the attached drive.
 * @pre Both output pointers are non-NULL.
 * @post On k_ra8_ok both outputs are filled from the device.
 * @post No other state changes.
 * @note Blocking; bounded by the class-layer timeouts.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t
fileops_backend_capacity(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  (void)ctx;
  return ra8_usb_hmsc_read_capacity((uint8_t)k_fileops_target_lun, block_count, block_size);
}

/**
 * @brief Map a detected filesystem type to a printable name.
 *
 * @param[in] type Mount-time detection result.
 * @return Static NUL-terminated name string.
 * @retval "exfat" For ::k_ra8_fs_type_exfat.
 * @pre None -- total over the enum.
 * @pre @p type came from a populated mount struct.
 * @post No state changes.
 * @post Returned pointer references static storage.
 * @note Pure function.
 * @since 0.1.0
 */
static const char* fileops_fs_type_name(ra8_fs_type_t type)
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

[[nodiscard]] ra8_err_t fileops_mount_volume(ra8_fs_mount_t** out_mount)
{
  const ra8_fs_backend_t backend = {
    .read_block   = fileops_backend_read,
    .write_block  = fileops_backend_write,
    .get_capacity = fileops_backend_capacity,
    .ctx          = nullptr,
  };
  ra8_err_t err = ra8_fs_mount(&backend, out_mount);
  if (err != k_ra8_ok) {
    (void)fileops_print_fail("mount", err);
    return err;
  }
  err = fileops_print("ra8d2 fileops: mounted fs=");
  if (err != k_ra8_ok) {
    return err;
  }
  err = fileops_print(fileops_fs_type_name((*out_mount)->type));
  if (err != k_ra8_ok) {
    return err;
  }
  err = fileops_print(" base_lba=");
  if (err != k_ra8_ok) {
    return err;
  }
  err = fileops_print_dec((*out_mount)->partition_base_lba);
  if (err != k_ra8_ok) {
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
 * @brief Cookie carried through ::ra8_fs_listdir for printing + matching.
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
 * @pre Invoked only by ::ra8_fs_listdir with a valid cookie.
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
  if ((attr & (uint8_t)k_ra8_fs_attr_directory) != 0U) {
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
 * @return ra8_err_t verdict.
 * @retval k_ra8_err_not_found @p want was not listed.
 * @retval k_ra8_err_exists    @p avoid was listed.
 * @pre @p mount is a live handle from ::ra8_fs_mount.
 * @pre SCI8 init already ran.
 * @post Every root entry plus the count line is queued.
 * @post No filesystem mutation occurs.
 * @note Walks the root directory over USB.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t
fileops_step_listdir(ra8_fs_mount_t* mount, const char* want, const char* avoid)
{
  fileops_listdir_ctx_t ctx = {
    .want        = want,
    .avoid       = avoid,
    .found_want  = 0U,
    .found_avoid = 0U,
    .count       = 0U,
  };
  ra8_err_t err = fileops_print("ra8d2 fileops: -- listdir / --\r\n");
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_fs_listdir(mount, "/", fileops_listdir_cb, &ctx);
  if (err != k_ra8_ok) {
    (void)fileops_print_fail("listdir", err);
    return err;
  }
  err = fileops_print("ra8d2 fileops: entries=");
  if (err != k_ra8_ok) {
    return err;
  }
  err = fileops_print_dec(ctx.count);
  if (err != k_ra8_ok) {
    return err;
  }
  err = fileops_print("\r\n");
  if (err != k_ra8_ok) {
    return err;
  }
  if (want != nullptr) {
    if (ctx.found_want == 0U) {
      (void)fileops_print_fail("listdir expected-name missing", k_ra8_err_not_found);
      return k_ra8_err_not_found;
    }
  }
  if (avoid != nullptr) {
    if (ctx.found_avoid != 0U) {
      (void)fileops_print_fail("listdir stale-name present", k_ra8_err_exists);
      return k_ra8_err_exists;
    }
  }
  return k_ra8_ok;
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
 * @return ra8_err_t verdict.
 * @retval k_ra8_err_invalid_size  Read length differs from the payload.
 * @retval k_ra8_err_invalid_state A payload byte differs.
 * @pre @p mount is a live handle from ::ra8_fs_mount.
 * @pre @p path names a file written with the payload.
 * @post The file is closed again in every path.
 * @post No filesystem mutation occurs.
 * @note Reads the file content over USB.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t fileops_read_back(ra8_fs_mount_t* mount, const char* path)
{
  static uint8_t s_read_buf[k_fileops_sector_bytes] = {};

  const uint32_t payload_len = (uint32_t)(sizeof(k_fileops_payload) - 1U);
  ra8_fs_file_t* file        = nullptr;
  ra8_err_t      err         = ra8_fs_open(mount, path, k_ra8_fs_mode_read, &file);
  if (err != k_ra8_ok) {
    (void)fileops_print_fail("open for read-back", err);
    return err;
  }
  uint32_t got = 0U;
  err          = ra8_fs_read(file, s_read_buf, (uint32_t)sizeof(s_read_buf), &got);
  (void)ra8_fs_close(file);
  if (err != k_ra8_ok) {
    (void)fileops_print_fail("read", err);
    return err;
  }
  if (got != payload_len) {
    (void)fileops_print_fail("read-back length", k_ra8_err_invalid_size);
    return k_ra8_err_invalid_size;
  }
  for (uint32_t i = 0U; i < payload_len; i++) {
    if (s_read_buf[i] != k_fileops_payload[i]) {
      (void)fileops_print_fail("read-back content", k_ra8_err_invalid_state);
      return k_ra8_err_invalid_state;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Require that @p path no longer resolves on the volume.
 *
 * @param[in] mount Live mount handle.
 * @param[in] path  Name that must not exist.
 * @return ra8_err_t verdict.
 * @retval k_ra8_err_exists The name still opened successfully.
 * @pre @p mount is a live handle from ::ra8_fs_mount.
 * @pre @p path was unlinked or renamed away.
 * @post Any accidentally opened handle is closed again.
 * @post No filesystem mutation occurs.
 * @note A lookup error other than not-found is propagated as-is.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t fileops_expect_absent(ra8_fs_mount_t* mount, const char* path)
{
  ra8_fs_file_t* file = nullptr;
  ra8_err_t      err  = ra8_fs_open(mount, path, k_ra8_fs_mode_read, &file);
  if (err == k_ra8_ok) {
    (void)ra8_fs_close(file);
    (void)fileops_print_fail("name still present", k_ra8_err_exists);
    return k_ra8_err_exists;
  }
  if (err != k_ra8_err_not_found) {
    (void)fileops_print_fail("absence lookup", err);
    return err;
  }
  return k_ra8_ok;
}

/**
 * @brief Step 3: print the banner and write the payload file.
 *
 * @param[in] mount Live mount handle.
 * @return First failing print/write error, or k_ra8_ok.
 * @retval k_ra8_ok ::k_fileops_name_a holds the payload.
 * @pre @p mount is a live handle from ::ra8_fs_mount.
 * @pre Leftover test files were already unlinked.
 * @post On k_ra8_ok the volume holds ::k_fileops_name_a with the payload.
 * @post The step banner is queued on the console.
 * @note Mutates the volume (creates the test file).
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t fileops_step_write(ra8_fs_mount_t* mount)
{
  ra8_err_t err = fileops_print("ra8d2 fileops: [3/9] write USBTEST.TXT len=");
  if (err != k_ra8_ok) {
    return err;
  }
  err = fileops_print_dec((uint32_t)(sizeof(k_fileops_payload) - 1U));
  if (err != k_ra8_ok) {
    return err;
  }
  err = fileops_print("\r\n");
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_fs_write_file(mount,
                          k_fileops_name_a,
                          k_fileops_payload,
                          (uint32_t)(sizeof(k_fileops_payload) - 1U));
  if (err != k_ra8_ok) {
    (void)fileops_print_fail("write", err);
    return err;
  }
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t fileops_suite_create(ra8_fs_mount_t* mount)
{
  ra8_err_t err = fileops_print("ra8d2 fileops: [1/9] cleanup leftovers\r\n");
  if (err != k_ra8_ok) {
    return err;
  }
  (void)ra8_fs_unlink(mount, k_fileops_name_a);
  (void)ra8_fs_unlink(mount, k_fileops_name_b);
  (void)ra8_board_led_toggle(k_ra8_board_led2);

  err = fileops_print("ra8d2 fileops: [2/9] baseline root listing\r\n");
  if (err != k_ra8_ok) {
    return err;
  }
  err = fileops_step_listdir(mount, nullptr, nullptr);
  if (err != k_ra8_ok) {
    return err;
  }
  (void)ra8_board_led_toggle(k_ra8_board_led2);

  err = fileops_step_write(mount);
  if (err != k_ra8_ok) {
    return err;
  }
  (void)ra8_board_led_toggle(k_ra8_board_led2);

  err = fileops_print("ra8d2 fileops: [4/9] read back + verify payload\r\n");
  if (err != k_ra8_ok) {
    return err;
  }
  err = fileops_read_back(mount, k_fileops_name_a);
  if (err != k_ra8_ok) {
    return err;
  }
  (void)ra8_board_led_toggle(k_ra8_board_led2);

  err = fileops_print("ra8d2 fileops: [5/9] listdir must show USBTEST.TXT\r\n");
  if (err != k_ra8_ok) {
    return err;
  }
  err = fileops_step_listdir(mount, k_fileops_name_a, nullptr);
  if (err != k_ra8_ok) {
    return err;
  }
  (void)ra8_board_led_toggle(k_ra8_board_led2);
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t fileops_suite_mutate(ra8_fs_mount_t* mount)
{
  ra8_err_t err = fileops_print("ra8d2 fileops: [6/9] rename USBTEST.TXT -> USBDONE.TXT\r\n");
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_fs_rename(mount, k_fileops_name_a, k_fileops_name_b);
  if (err != k_ra8_ok) {
    (void)fileops_print_fail("rename", err);
    return err;
  }
  (void)ra8_board_led_toggle(k_ra8_board_led2);

  err = fileops_print("ra8d2 fileops: [7/9] old name gone, new name intact\r\n");
  if (err != k_ra8_ok) {
    return err;
  }
  err = fileops_expect_absent(mount, k_fileops_name_a);
  if (err != k_ra8_ok) {
    return err;
  }
  err = fileops_read_back(mount, k_fileops_name_b);
  if (err != k_ra8_ok) {
    return err;
  }
  (void)ra8_board_led_toggle(k_ra8_board_led2);

  err = fileops_print("ra8d2 fileops: [8/9] listdir must show USBDONE.TXT only\r\n");
  if (err != k_ra8_ok) {
    return err;
  }
  err = fileops_step_listdir(mount, k_fileops_name_b, k_fileops_name_a);
  if (err != k_ra8_ok) {
    return err;
  }
  (void)ra8_board_led_toggle(k_ra8_board_led2);

  err = fileops_print("ra8d2 fileops: [9/9] unlink USBDONE.TXT\r\n");
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_fs_unlink(mount, k_fileops_name_b);
  if (err != k_ra8_ok) {
    (void)fileops_print_fail("unlink", err);
    return err;
  }
  err = fileops_expect_absent(mount, k_fileops_name_b);
  if (err != k_ra8_ok) {
    return err;
  }
  err = fileops_step_listdir(mount, nullptr, k_fileops_name_b);
  if (err != k_ra8_ok) {
    return err;
  }
  (void)ra8_board_led_toggle(k_ra8_board_led2);
  return k_ra8_ok;
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

void fileops_probe_layout(void)
{
  static uint8_t s_probe_buf[k_fileops_sector_bytes] = {};

  for (uint32_t lba = 0U; lba < (uint32_t)k_fileops_probe_lba_max; lba++) {
    if (fileops_backend_read(nullptr, lba, 1U, s_probe_buf) != k_ra8_ok) {
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
