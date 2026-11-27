/**
 * @file translate_e2e_sd.c
 * @brief Native SDHI block backend + `ra8_fs` loader for `/TPAK.BIN` on EK-RA8D2.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <limits.h>
#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_fs.h"
#include "ra8_sdcard.h"
#include "ra8_sdhi.h"
#include "translate_e2e.h"

typedef enum : uint32_t {
  k_sd_instance    = 1U,   /**< SDHI1_B drives the micro-SD slot on EK-RA8D2. */
  k_sd_sector_size = 512U, /**< Fixed 512-byte card sector size.              */
  k_sd_read_chunk  = 4096U /**< Per-call streaming read chunk size.           */
} te2e_sd_const_t;

static ra8_err_t sd_blk_read(void* ctx, uint64_t lba, uint32_t count, uint8_t* buf)
{
  (void)ctx;
  if (lba > (uint64_t)UINT32_MAX) {
    return k_ra8_err_out_of_range;
  }
  return ra8_sdcard_read_blocks((uint32_t)lba, buf, count);
}

static ra8_err_t sd_blk_write(void* ctx, uint64_t lba, uint32_t count, const uint8_t* buf)
{
  (void)ctx;
  if (lba > (uint64_t)UINT32_MAX) {
    return k_ra8_err_out_of_range;
  }
  return ra8_sdcard_write_blocks((uint32_t)lba, buf, count);
}

static ra8_err_t sd_blk_capacity(void* ctx, uint64_t* block_count, uint32_t* block_size)
{
  (void)ctx;
  if ((block_count == nullptr) || (block_size == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  uint32_t  blocks = 0U;
  ra8_err_t err    = ra8_sdcard_get_capacity(&blocks);
  if (err != k_ra8_ok) {
    return err;
  }
  *block_count = (uint64_t)blocks;
  *block_size  = (uint32_t)k_sd_sector_size;
  return k_ra8_ok;
}

static ra8_err_t
sd_read_file_all(ra8_fs_file_t* file, uint8_t* out_buf, size_t out_cap, size_t* out_len)
{
  uint64_t  file_size = 0U;
  ra8_err_t err       = ra8_fs_size(file, &file_size);
  if (err != k_ra8_ok) {
    return err;
  }
  if ((file_size == 0U) || (file_size > (uint64_t)out_cap)) {
    return k_ra8_err_invalid_size;
  }
  size_t total = 0U;
  while (total < (size_t)file_size) {
    const size_t   rem = (size_t)file_size - total;
    const uint32_t chunk =
      (rem > (size_t)k_sd_read_chunk) ? (uint32_t)k_sd_read_chunk : (uint32_t)rem;
    uint32_t got = 0U;
    err          = ra8_fs_read(file, &out_buf[total], chunk, &got);
    if (err != k_ra8_ok) {
      return err;
    }
    if (got == 0U) {
      return k_ra8_err_validation_failed;
    }
    total += (size_t)got;
  }
  *out_len = total;
  return k_ra8_ok;
}

ra8_err_t translate_e2e_sd_load_tpak(uint8_t* out_buf, size_t out_cap, size_t* out_len)
{
  if ((out_buf == nullptr) || (out_len == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *out_len      = 0U;
  ra8_err_t err = ra8_board_sdhi_pins_init();
  if (err != k_ra8_ok) {
    return k_ra8_err_not_found;
  }
  const uint8_t instances[2] = {(uint8_t)k_sd_instance, 0U};
  bool          card_ready   = false;
  for (size_t idx = 0U; idx < 2U; ++idx) {
    const ra8_sdcard_cfg_t card_cfg = {.instance  = instances[idx],
                                       .bus_width = k_ra8_sdhi_bus_width_4bit};
    if (ra8_sdcard_init(&card_cfg) == k_ra8_ok) {
      card_ready = true;
      break;
    }
    (void)ra8_sdcard_deinit();
  }
  if (!card_ready) {
    return k_ra8_err_not_found;
  }
  const ra8_fs_backend_t backend = {
    .read_block   = sd_blk_read,
    .write_block  = sd_blk_write,
    .get_capacity = sd_blk_capacity,
  };
  ra8_fs_mount_t* mnt = nullptr;
  err                 = ra8_fs_mount(&backend, &mnt);
  if (err != k_ra8_ok) {
    (void)ra8_sdcard_deinit();
    return k_ra8_err_not_found;
  }
  ra8_fs_file_t* file = nullptr;
  err                 = ra8_fs_open(mnt, "TPAK.BIN", k_ra8_fs_mode_read, &file);
  if (err == k_ra8_ok) {
    err = sd_read_file_all(file, out_buf, out_cap, out_len);
    (void)ra8_fs_close(file);
  }
  (void)ra8_fs_unmount(mnt);
  (void)ra8_sdcard_deinit();
  return err;
}
