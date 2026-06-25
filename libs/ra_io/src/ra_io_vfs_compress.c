/**
 * @file ra_io_vfs_compress.c
 * @brief Transparent whole-file compress-on-write / decompress-on-read over the
 *        ra_io VFS.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Composes the buffer-to-buffer codec (::ra_io_compress / ::ra_io_decompress)
 * with the VFS file ops (`ra_io_vfs_open` + `ra_fs_write` / `ra_fs_read` +
 * `ra_fs_close`). Write compresses the caller payload into the caller blob
 * buffer and streams the blob to the resolved file; read pulls the blob off the
 * file into the caller blob buffer and inflates it into the caller output. No
 * allocator is touched -- every buffer is caller-owned. All guards are
 * single-condition (no compound decisions, so no MC/DC vectors are due).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_io_vfs_compress.h"

#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_fs.h"
#include "ra_io_compress.h"
#include "ra_io_vfs.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra_io_vfs_compress";

ra_err_t ra_io_vfs_write_compressed(const char*    path,
                                    const uint8_t* src,
                                    uint32_t       src_len,
                                    void*          scratch,
                                    uint32_t       scratch_len,
                                    uint8_t*       blob_buf,
                                    uint32_t       blob_cap,
                                    uint32_t*      out_blob_len)
{
  RA_CHECK_NULL_PTR(path, s_tag, "path must not be nullptr");
  RA_CHECK_NULL_PTR(src, s_tag, "src must not be nullptr");
  RA_CHECK_NULL_PTR(scratch, s_tag, "scratch must not be nullptr");
  RA_CHECK_NULL_PTR(blob_buf, s_tag, "blob_buf must not be nullptr");
  RA_CHECK_NULL_PTR(out_blob_len, s_tag, "out_blob_len must not be nullptr");

  uint32_t blob_len = 0;
  RA_RETURN_ON_ERROR(
    ra_io_compress(src, src_len, blob_buf, blob_cap, scratch, scratch_len, &blob_len),
    s_tag,
    "compress");

  ra_fs_file_t* f = nullptr;
  RA_RETURN_ON_ERROR(ra_io_vfs_open(path, k_ra_fs_mode_write, &f), s_tag, "open write");
  const ra_err_t we = ra_fs_write(f, blob_buf, blob_len);
  const ra_err_t ce = ra_fs_close(f);
  if (we != k_ra_ok) {
    return we;
  }
  if (ce != k_ra_ok) {
    return ce;
  }
  *out_blob_len = blob_len;
  return k_ra_ok;
}

ra_err_t ra_io_vfs_read_compressed(const char* path,
                                   uint8_t*    blob_buf,
                                   uint32_t    blob_cap,
                                   uint8_t*    out,
                                   uint32_t    out_cap,
                                   uint32_t*   out_len)
{
  RA_CHECK_NULL_PTR(path, s_tag, "path must not be nullptr");
  RA_CHECK_NULL_PTR(blob_buf, s_tag, "blob_buf must not be nullptr");
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  RA_CHECK_NULL_PTR(out_len, s_tag, "out_len must not be nullptr");

  ra_fs_file_t* f = nullptr;
  RA_RETURN_ON_ERROR(ra_io_vfs_open(path, k_ra_fs_mode_read, &f), s_tag, "open read");
  uint32_t       blob_len = 0;
  const ra_err_t re       = ra_fs_read(f, blob_buf, blob_cap, &blob_len);
  const ra_err_t ce       = ra_fs_close(f);
  if (re != k_ra_ok) {
    return re;
  }
  if (ce != k_ra_ok) {
    return ce;
  }
  if (blob_len >= blob_cap) {
    return k_ra_err_invalid_size;
  }
  RA_RETURN_ON_ERROR(ra_io_decompress(blob_buf, blob_len, out, out_cap, out_len), s_tag, "inflate");
  return k_ra_ok;
}
