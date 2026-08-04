/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/ra8_io_sdram_demo/ra8_io_roundtrip.c
 * @brief Shared ra8_io VFS round-trip implementation for the FAT demos (#155).
 *
 * @details
 * Implements ::ra8_io_roundtrip_mount, ::ra8_io_roundtrip_root_file, and
 * ::ra8_io_roundtrip_subdir_file -- the backend-agnostic core that the RAM,
 * SDRAM, OSPI/xSPI NOR, and SD-over-SPI demos all share. The block device is
 * bound by the caller (its `ra8_io_blockdev_*_init` already ran); everything
 * above the block-device seam is identical, so it lives here once. The helpers
 * are silent: each demo's `main.c` owns its PASS banner so the existing
 * ra8_emulator smoke expectations stay byte-identical.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_io_roundtrip.h"

#include <string.h>

#include "ra8_check.h"

/**
 * @enum ra8_io_roundtrip_const_t
 * @brief Fixed sizing + payload-pattern knobs (no magic numbers).
 *
 * @details The scratch buffers are sized to the largest payload any demo
 *          round-trips (the SD demo's 512-byte sector); a payload longer than
 *          this is rejected at runtime. The pattern constants make the written
 *          bytes deterministic so the read-back compare is a strong end-to-end
 *          check of the whole stack.
 *
 * @invariant `k_ra8_io_roundtrip_max_payload >= every demo's payload length`.
 */
typedef enum : uint32_t {
  k_ra8_io_roundtrip_max_payload = 512U, /**< Largest payload (SD sector).        */
  k_ra8_io_roundtrip_seed_mul    = 7U,   /**< Root payload pattern multiplier.    */
  k_ra8_io_roundtrip_seed_add    = 1U,   /**< Root payload pattern additive bias. */
  k_ra8_io_roundtrip_note_base   = 65U,  /**< Subdir note alphabet base ('A').    */
  k_ra8_io_roundtrip_note_mod    = 26U,  /**< Subdir note alphabet size.          */
} ra8_io_roundtrip_const_t;

/** @brief Shared payload + read-back scratch (no heap; NASA Rule 3). */
static uint8_t s_payload[k_ra8_io_roundtrip_max_payload];
static uint8_t s_readback[k_ra8_io_roundtrip_max_payload];

/**
 * @brief Fill ::s_payload with a deterministic linear byte pattern.
 *
 * @details Writes `byte[i] = i * mul + add` over the first @p len bytes of the
 *          shared payload buffer, giving a reproducible content whose read-back
 *          compare validates the whole fabric path. The pattern is the same one
 *          the original per-demo `main.c` files used for their root payloads.
 *
 * @param[in] len Number of bytes to fill (must be <= the buffer capacity).
 *
 * @return None.
 *
 * @pre @p len is at most ::k_ra8_io_roundtrip_max_payload.
 * @pre ::s_payload is file-scope storage (always allocated).
 * @post The first @p len bytes of ::s_payload hold the reproducible pattern.
 * @post No other state is modified.
 *
 * @note Not thread-safe; mutates the shared payload buffer.
 * @since 0.1.0
 */
static void ra8_io_roundtrip_fill_linear(uint32_t len)
{
  for (uint32_t i = 0U; i < len; ++i) {
    s_payload[i] = (uint8_t)((i * (uint32_t)k_ra8_io_roundtrip_seed_mul) +
                             (uint32_t)k_ra8_io_roundtrip_seed_add);
  }
}

/**
 * @brief Fill ::s_payload with a deterministic rolling-alphabet pattern.
 *
 * @details Writes `byte[i] = 'A' + (i % 26)` over the first @p len bytes of the
 *          shared payload buffer -- the same note pattern the original per-demo
 *          subdir round-trips used, kept distinct from the linear root pattern
 *          so a swapped buffer would be caught.
 *
 * @param[in] len Number of bytes to fill (must be <= the buffer capacity).
 *
 * @return None.
 *
 * @pre @p len is at most ::k_ra8_io_roundtrip_max_payload.
 * @pre ::s_payload is file-scope storage (always allocated).
 * @post The first @p len bytes of ::s_payload hold the alphabet pattern.
 * @post No other state is modified.
 *
 * @note Not thread-safe; mutates the shared payload buffer.
 * @since 0.1.0
 */
static void ra8_io_roundtrip_fill_alpha(uint32_t len)
{
  for (uint32_t i = 0U; i < len; ++i) {
    s_payload[i] = (uint8_t)((uint32_t)k_ra8_io_roundtrip_note_base +
                             (i % (uint32_t)k_ra8_io_roundtrip_note_mod));
  }
}

/**
 * @brief Read a VFS file back into ::s_readback and compare against ::s_payload.
 *
 * @details Opens @p path for reading through the VFS, reads up to @p len bytes
 *          into the shared read-back buffer, closes the handle on every path,
 *          then verifies both the returned length and every byte match the
 *          payload most recently written by a fill helper.
 *
 * @param[in] path Full `<prefix>:/...` VFS path to read.
 * @param[in] len  Expected byte count (the payload length just written).
 * @param[in] tag  ra8_log tag for the open/read step diagnostics.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                    Length and content both matched.
 * @retval k_ra8_err_invalid_size      The returned length differed from @p len.
 * @retval k_ra8_err_checksum_mismatch The byte content differed.
 * @retval (other)                    The first failing VFS / ra8_fs step's code.
 *
 * @pre @p path and @p tag are non-NULL.
 * @pre @p len is at most ::k_ra8_io_roundtrip_max_payload.
 * @post ::s_readback holds the bytes read from @p path.
 * @post No file handle is left open on any return path.
 *
 * @note Not thread-safe; mutates the shared read-back buffer.
 * @since 0.1.0
 */
static ra8_err_t ra8_io_roundtrip_read_verify(const char* path, uint32_t len, const char* tag)
{
  ra8_fs_file_t* rf = nullptr;
  RA8_RETURN_ON_ERROR(ra8_io_vfs_open(path, k_ra8_fs_mode_read, &rf), tag, "open r");
  memset(s_readback, 0, sizeof(s_readback));
  uint32_t  got = 0U;
  ra8_err_t err = ra8_fs_read(rf, s_readback, len, &got);
  (void)ra8_fs_close(rf);
  if (err != k_ra8_ok) {
    return err;
  }
  if (got != len) {
    return k_ra8_err_invalid_size;
  }
  if (memcmp(s_payload, s_readback, (size_t)len) != 0) {
    return k_ra8_err_checksum_mismatch;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_io_roundtrip_mount(ra8_io_blockdev_t*               bd,
                                 const ra8_io_roundtrip_params_t* p,
                                 ra8_fs_backend_t*                be,
                                 ra8_fs_mount_t**                 out_mount)
{
  RA8_CHECK_NULL_PTR(bd, "ra8_io_roundtrip", "bd");
  RA8_CHECK_NULL_PTR(p, "ra8_io_roundtrip", "params");
  RA8_CHECK_NULL_PTR(be, "ra8_io_roundtrip", "backend");
  RA8_CHECK_NULL_PTR(out_mount, "ra8_io_roundtrip", "out_mount");
  *out_mount = nullptr;

  RA8_RETURN_ON_ERROR(ra8_io_blockdev_as_fs_backend(bd, be), p->log_tag, "fs bridge");
  ra8_fs_format_opts_t opts = {};
  opts.type                 = p->fat_type;
  opts.label                = p->volume_label;
  RA8_RETURN_ON_ERROR(ra8_fs_format(be, &opts), p->log_tag, "format");
  ra8_fs_mount_t* mnt = nullptr;
  RA8_RETURN_ON_ERROR(ra8_fs_mount(be, &mnt), p->log_tag, "mount");
  RA8_RETURN_ON_ERROR(ra8_io_vfs_mount(p->vfs_prefix, mnt), p->log_tag, "vfs mount");
  *out_mount = mnt;
  return k_ra8_ok;
}

ra8_err_t ra8_io_roundtrip_root_file(ra8_fs_mount_t* mnt, const ra8_io_roundtrip_params_t* p)
{
  RA8_CHECK_NULL_PTR(mnt, "ra8_io_roundtrip", "mnt");
  RA8_CHECK_NULL_PTR(p, "ra8_io_roundtrip", "params");
  if (p->root_bytes > (uint32_t)k_ra8_io_roundtrip_max_payload) {
    return k_ra8_err_invalid_size;
  }

  ra8_io_roundtrip_fill_linear(p->root_bytes);
  RA8_RETURN_ON_ERROR(ra8_fs_write_file(mnt, p->root_file, s_payload, p->root_bytes),
                      p->log_tag,
                      "write");
  return ra8_io_roundtrip_read_verify(p->root_path, p->root_bytes, p->log_tag);
}

ra8_err_t ra8_io_roundtrip_subdir_file(const ra8_io_roundtrip_params_t* p)
{
  RA8_CHECK_NULL_PTR(p, "ra8_io_roundtrip", "params");
  if (p->subdir_bytes > (uint32_t)k_ra8_io_roundtrip_max_payload) {
    return k_ra8_err_invalid_size;
  }

  RA8_RETURN_ON_ERROR(ra8_io_vfs_mkdir(p->subdir_path), p->log_tag, "mkdir");
  ra8_io_roundtrip_fill_alpha(p->subdir_bytes);

  ra8_fs_file_t* wf = nullptr;
  RA8_RETURN_ON_ERROR(ra8_io_vfs_open(p->subdir_file, k_ra8_fs_mode_write, &wf),
                      p->log_tag,
                      "open w");
  ra8_err_t werr = ra8_fs_write(wf, s_payload, p->subdir_bytes);
  RA8_RETURN_ON_ERROR(ra8_fs_close(wf), p->log_tag, "close w");
  RA8_RETURN_ON_ERROR(werr, p->log_tag, "write");

  return ra8_io_roundtrip_read_verify(p->subdir_file, p->subdir_bytes, p->log_tag);
}
