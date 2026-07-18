/**
 * @file fuzz_ra8_fs_fat.c
 * @brief libFuzzer harness for the ra8_fs FAT directory-entry parser
 *
 * @details
 * Wires a tiny in-memory block-device backend that returns the fuzz
 * input as the bytes of every read sector. ``ra8_fs_mount`` then walks
 * the BPB / FAT / directory-entry layout against arbitrary content,
 * surfacing any out-of-bounds read or integer UB in the parser.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "fuzz_entry.h"
#include "ra8_err.h"
#include "ra8_fs.h"

enum : uint32_t {
  k_fuzz_fat_max_input    = 65536U, /**< Fuzz fat maximum input. */
  k_fuzz_fat_sector_bytes = 512U,   /**< Fuzz fat sector bytes.  */
  k_fuzz_fat_block_count  = 1024U,  /**< Fuzz fat block count.   */
};

static const uint8_t* s_blob;
static size_t         s_blob_len;

static ra8_err_t fuzz_fat_read_block(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  (void)ctx;
  if (buf == nullptr) {
    return k_ra8_err_null_ptr;
  }
  for (uint32_t s = 0U; s < count; s++) {
    for (uint32_t i = 0U; i < (uint32_t)k_fuzz_fat_sector_bytes; i++) {
      const size_t off = (((size_t)(lba + s) * (size_t)k_fuzz_fat_sector_bytes) + (size_t)i) %
                         (s_blob_len > 0U ? s_blob_len : 1U);
      buf[(s * (uint32_t)k_fuzz_fat_sector_bytes) + i] = (s_blob_len > 0U) ? s_blob[off] : 0U;
    }
  }
  return k_ra8_ok;
}

static ra8_err_t fuzz_fat_write_block(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  (void)ctx;
  (void)lba;
  (void)count;
  (void)buf;
  return k_ra8_ok;
}

static ra8_err_t fuzz_fat_get_capacity(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  (void)ctx;
  if (block_count != nullptr) {
    *block_count = (uint32_t)k_fuzz_fat_block_count;
  }
  if (block_size != nullptr) {
    *block_size = (uint32_t)k_fuzz_fat_sector_bytes;
  }
  return k_ra8_ok;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
  if (size == 0U || size > (size_t)k_fuzz_fat_max_input) {
    return 0;
  }
  s_blob     = data;
  s_blob_len = size;

  ra8_fs_backend_t backend = {
    .read_block   = fuzz_fat_read_block,
    .write_block  = fuzz_fat_write_block,
    .get_capacity = fuzz_fat_get_capacity,
    .ctx          = nullptr,
  };
  ra8_fs_mount_t* mount = nullptr;
  if (ra8_fs_mount(&backend, &mount) == k_ra8_ok && mount != nullptr) {
    (void)ra8_fs_unmount(mount);
  }
  return 0;
}
