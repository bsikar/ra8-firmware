/**
 * @file fuzz_ra_fs_fat.c
 * @brief libFuzzer harness for the ra_fs FAT directory-entry parser
 *
 * @details
 * Wires a tiny in-memory block-device backend that returns the fuzz
 * input as the bytes of every read sector. ``ra_fs_mount`` then walks
 * the BPB / FAT / directory-entry layout against arbitrary content,
 * surfacing any out-of-bounds read or integer UB in the parser.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra_err.h"
#include "ra_fs.h"

enum : uint32_t {
  k_fuzz_fat_max_input    = 65536U,
  k_fuzz_fat_sector_bytes = 512U,
  k_fuzz_fat_block_count  = 1024U,
};

static const uint8_t* s_blob;
static size_t         s_blob_len;

static ra_err_t fuzz_fat_read_block(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  (void)ctx;
  if (buf == NULL) {
    return k_ra_err_null_ptr;
  }
  for (uint32_t s = 0U; s < count; s++) {
    for (uint32_t i = 0U; i < (uint32_t)k_fuzz_fat_sector_bytes; i++) {
      const size_t off = ((size_t)(lba + s) * (size_t)k_fuzz_fat_sector_bytes + (size_t)i) %
                         (s_blob_len > 0U ? s_blob_len : 1U);
      buf[(s * (uint32_t)k_fuzz_fat_sector_bytes) + i] = (s_blob_len > 0U) ? s_blob[off] : 0U;
    }
  }
  return k_ra_ok;
}

static ra_err_t fuzz_fat_write_block(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  (void)ctx;
  (void)lba;
  (void)count;
  (void)buf;
  return k_ra_ok;
}

static ra_err_t fuzz_fat_get_capacity(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  (void)ctx;
  if (block_count != NULL) {
    *block_count = (uint32_t)k_fuzz_fat_block_count;
  }
  if (block_size != NULL) {
    *block_size = (uint32_t)k_fuzz_fat_sector_bytes;
  }
  return k_ra_ok;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
  if (size == 0U || size > (size_t)k_fuzz_fat_max_input) {
    return 0;
  }
  s_blob     = data;
  s_blob_len = size;

  ra_fs_backend_t backend = {
    .read_block   = fuzz_fat_read_block,
    .write_block  = fuzz_fat_write_block,
    .get_capacity = fuzz_fat_get_capacity,
    .ctx          = NULL,
  };
  ra_fs_mount_t* mount = NULL;
  if (ra_fs_mount(&backend, &mount) == k_ra_ok && mount != NULL) {
    (void)ra_fs_unmount(mount);
  }
  return 0;
}
