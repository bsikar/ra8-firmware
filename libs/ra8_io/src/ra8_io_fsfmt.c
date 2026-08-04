/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_io_fsfmt.c
 * @brief Filesystem-format registry: probe a volume + report capabilities.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Holds pointers to registered ::ra8_io_fsfmt_t descriptors and probes them in
 * order against a block device's boot sector. FAT and exFAT are built-ins whose
 * probes read block 0 directly (no `ra8_fs` internals); foreign formats register
 * through ::ra8_io_fsfmt_register. Every check is a single condition, so no MC/DC
 * vectors are due.
 */

#include "ra8_io_fsfmt.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_io_blockdev.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_io_fsfmt";

/**
 * @enum ra8_io_fsfmt_const_t
 * @brief Boot-sector offsets, signatures, and name-length caps.
 *
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_off_exfat_name = 3,    /**< exFAT VBR filesystem-name offset. */
  k_len_exfat_name = 8,    /**< "EXFAT   " field length.          */
  k_off_boot_sig0  = 510,  /**< 0x55 boot signature offset.       */
  k_off_boot_sig1  = 511,  /**< 0xAA boot signature offset.       */
  k_boot_sig0      = 0x55, /**< First boot-signature byte.        */
  k_boot_sig1      = 0xAA, /**< Second boot-signature byte.       */
  k_fat_max_name   = 12,   /**< FAT 8.3 name length incl NUL.     */
  k_exfat_max_name = 255,  /**< exFAT name length.                */
} ra8_io_fsfmt_const_t;

/** @brief Registered formats (pointers to caller-owned const descriptors). */
static const ra8_io_fsfmt_t* s_reg[(uint32_t)k_ra8_io_fsfmt_max];
/** @brief Number of registered formats. */
static uint32_t s_count = 0;

/**
 * @brief Read block 0 of a backend into `blk`, with NULL guards.
 *
 * @details Single-condition guards on the backend and its read op, then one
 *          512-byte block read; any failure yields false.
 *
 * @param[in]  be  Block-device backend.
 * @param[out] blk Destination of `k_ra8_io_block_size_bytes` bytes.
 *
 * @return bool true when block 0 was read.
 * @retval true  `blk` holds the boot sector.
 * @retval false `be` invalid or the read failed.
 *
 * @pre `blk` is writable for one block.
 * @pre `be` is the candidate backend.
 * @post On true `blk` mirrors block 0; on false `blk` is unspecified.
 * @post No registry state is touched.
 *
 * @note Not thread-safe with respect to the device.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_read_boot0(const ra8_fs_backend_t* be, uint8_t* blk)
{
  /* Not reachable: ra8_io_fsfmt_probe validates backend non-null before dispatching. */
  if (be == nullptr) {
    return false; /* GCOVR_EXCL_LINE */
  }
  if (be->read_block == nullptr) {
    return false;
  }
  return be->read_block(be->ctx, 0, 1, blk) == k_ra8_ok;
}

/**
 * @brief Test whether a boot sector carries the exFAT filesystem name.
 *
 * @details Compares the eight bytes at the VBR filesystem-name offset against
 *          the `"EXFAT   "` literal.
 *
 * @param[in] blk Boot sector (>= 512 bytes).
 *
 * @return bool true when the exFAT name is present.
 * @retval true  Volume is exFAT.
 * @retval false Volume is not exFAT.
 *
 * @pre `blk` holds at least one block.
 * @pre `blk` is non-NULL.
 * @post No state is mutated.
 * @post The return reflects only the name comparison.
 *
 * @note Thread-safe (pure comparison).
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_is_exfat(const uint8_t* blk)
{
  return memcmp(&blk[(uint32_t)k_off_exfat_name], "EXFAT   ", (size_t)k_len_exfat_name) == 0;
}

/**
 * @brief Built-in FAT probe: a 0x55AA boot signature and not exFAT.
 *
 * @details Reads block 0, rejects exFAT, then requires both boot-signature
 *          bytes. Each predicate is its own single-condition check.
 *
 * @param[in] be Block-device backend to inspect.
 *
 * @return bool true when the volume looks like FAT12/16/32.
 * @retval true  FAT signature present.
 * @retval false Not FAT (exFAT, no signature, or read failure).
 *
 * @pre `be` is the candidate backend.
 * @pre The device is readable.
 * @post No state is mutated.
 * @post The return reflects only the boot-signature bytes.
 *
 * @note Not thread-safe with respect to the device.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static bool fat_probe(const ra8_fs_backend_t* be)
{
  uint8_t blk[(size_t)k_ra8_io_block_size_bytes];
  if (!internal_read_boot0(be, blk)) {
    return false;
  }
  if (internal_is_exfat(blk)) {
    return false;
  }
  if (blk[(uint32_t)k_off_boot_sig0] != (uint8_t)k_boot_sig0) {
    return false;
  }
  if (blk[(uint32_t)k_off_boot_sig1] != (uint8_t)k_boot_sig1) {
    return false;
  }
  return true;
}

/**
 * @brief Built-in exFAT probe: the exFAT VBR filesystem name.
 *
 * @details Reads block 0 and checks the exFAT name field.
 *
 * @param[in] be Block-device backend to inspect.
 *
 * @return bool true when the volume is exFAT.
 * @retval true  exFAT name present.
 * @retval false Not exFAT (or read failure).
 *
 * @pre `be` is the candidate backend.
 * @pre The device is readable.
 * @post No state is mutated.
 * @post The return reflects only the exFAT name field.
 *
 * @note Not thread-safe with respect to the device.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static bool exfat_probe(const ra8_fs_backend_t* be)
{
  uint8_t blk[(size_t)k_ra8_io_block_size_bytes];
  if (!internal_read_boot0(be, blk)) {
    return false;
  }
  return internal_is_exfat(blk);
}

/** @brief Built-in FAT12/16/32 format descriptor. */
static const ra8_io_fsfmt_t k_fmt_fat = {
  .name = "fat",
  .caps =
    {
      .max_name_len             = (uint16_t)k_fat_max_name,
      .read_only                = false,
      .supports_mkdir           = false,
      .supports_streaming_write = true,
      .case_sensitive           = false,
    },
  .probe = fat_probe,
};

/** @brief Built-in exFAT format descriptor (whole-file writes only). */
static const ra8_io_fsfmt_t k_fmt_exfat = {
  .name = "exfat",
  .caps =
    {
      .max_name_len             = (uint16_t)k_exfat_max_name,
      .read_only                = false,
      .supports_mkdir           = false,
      .supports_streaming_write = false,
      .case_sensitive           = false,
    },
  .probe = exfat_probe,
};

ra8_err_t ra8_io_fsfmt_init(void)
{
  s_count = 0;
  RA8_RETURN_ON_ERROR(ra8_io_fsfmt_register(&k_fmt_exfat), s_tag, "register exfat");
  RA8_RETURN_ON_ERROR(ra8_io_fsfmt_register(&k_fmt_fat), s_tag, "register fat");
  return k_ra8_ok;
}

ra8_err_t ra8_io_fsfmt_register(const ra8_io_fsfmt_t* fmt)
{
  RA8_CHECK_NULL_PTR(fmt, s_tag, "fmt must not be nullptr");
  RA8_CHECK_NULL_PTR(fmt->name, s_tag, "fmt->name must not be nullptr");
  RA8_CHECK_NULL_PTR(fmt->probe, s_tag, "fmt->probe must not be nullptr");
  if (s_count >= (uint32_t)k_ra8_io_fsfmt_max) {
    return k_ra8_err_no_mem;
  }
  s_reg[s_count] = fmt;
  s_count++;
  return k_ra8_ok;
}

ra8_err_t ra8_io_fsfmt_probe(const ra8_fs_backend_t* backend, const ra8_io_fsfmt_t** out)
{
  RA8_CHECK_NULL_PTR(backend, s_tag, "backend must not be nullptr");
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  for (uint32_t i = 0; i < s_count; ++i) {
    if (s_reg[i]->probe(backend)) {
      *out = s_reg[i];
      return k_ra8_ok;
    }
  }
  return k_ra8_err_not_found;
}
