/**
 * @file ra8_io_vfs_compress.c
 * @brief Transparent whole-file compress-on-write / decompress-on-read over the
 *        ra8_io VFS.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Composes the buffer-to-buffer codec (::ra8_io_compress / ::ra8_io_decompress)
 * with the VFS file ops (`ra8_io_vfs_open` + `ra8_fs_write` / `ra8_fs_read` +
 * `ra8_fs_close`). Write compresses the caller payload into the caller blob
 * buffer and streams the blob to the resolved file; read pulls the blob off the
 * file into the caller blob buffer and inflates it into the caller output. No
 * allocator is touched -- every buffer is caller-owned. All guards are
 * single-condition (no compound decisions, so no MC/DC vectors are due).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_io_vfs_compress.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_io_compress.h"
#include "ra8_io_vfs.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_io_vfs_compress";

/**
 * @brief Validate the five caller pointers handed to
 *        ::ra8_io_vfs_write_compressed.
 *
 * @details
 * Rejects any NULL pointer argument before the compressor or the VFS is
 * touched, keeping the public entry point's body short and its statement
 * count under the NASA Rule 4 / clang-tidy threshold. Each guard is a single
 * condition (`ptr == nullptr`), so no compound decision is split across this
 * helper and its caller and no MC/DC vector is due.
 *
 * @param[in] path         `"name:/sub"` destination string.
 * @param[in] src          Plain payload bytes to compress.
 * @param[in] scratch      Caller compressor scratch buffer.
 * @param[in] blob_buf     Caller staging buffer for the compressed blob.
 * @param[in] out_blob_len Out-param for the compressed byte count.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Every pointer argument is non-NULL.
 * @retval k_ra8_err_null_ptr Some pointer argument was NULL.
 *
 * @pre The caller passes the same pointers it received from its own caller.
 * @pre `s_tag` is initialised (it is a file-scope constant).
 * @post On `k_ra8_ok` all five pointers are safe to dereference.
 * @post On `k_ra8_err_null_ptr` no pointer was dereferenced.
 *
 * @note Pure validation; not thread-safe-sensitive (reads no shared state).
 *
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static ra8_err_t
internal_ra8_io_vfs_compress_write_validate(const char*     path,
                                            const uint8_t*  src,
                                            const void*     scratch,
                                            const uint8_t*  blob_buf,
                                            const uint32_t* out_blob_len)
{
  RA8_CHECK_NULL_PTR(path, s_tag, "path must not be nullptr");
  RA8_CHECK_NULL_PTR(src, s_tag, "src must not be nullptr");
  RA8_CHECK_NULL_PTR(scratch, s_tag, "scratch must not be nullptr");
  RA8_CHECK_NULL_PTR(blob_buf, s_tag, "blob_buf must not be nullptr");
  RA8_CHECK_NULL_PTR(out_blob_len, s_tag, "out_blob_len must not be nullptr");
  return k_ra8_ok;
}

/**
 * @brief Open `path` for write, stream the compressed blob, and close.
 *
 * @details
 * Resolves `path` through the VFS in write mode, writes the whole `blob`, and
 * closes the file. The write and close error codes are captured separately so
 * the file is always closed even when the write fails; the write error takes
 * precedence over the close error. Each guard is a single condition, so no
 * compound decision is split across this helper and its caller.
 *
 * @param[in] path     `"name:/sub"` destination string (already validated).
 * @param[in] blob     Compressed bytes to store.
 * @param[in] blob_len Number of bytes in `blob` to write.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok    Blob written and file closed cleanly.
 * @retval k_ra8_err_* Propagated from the VFS open, write, or close.
 *
 * @pre `path` and `blob` are non-NULL (the caller validated them).
 * @pre The named volume in `path` is mounted.
 * @post On `k_ra8_ok` the file holds exactly `blob_len` bytes.
 * @post On any non-ok return the file handle is closed (no leak).
 *
 * @note Not thread-safe with respect to the same backing file.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static ra8_err_t
internal_ra8_io_vfs_compress_store_blob(const char* path, const uint8_t* blob, uint32_t blob_len)
{
  ra8_fs_file_t* f = nullptr;
  RA8_RETURN_ON_ERROR(ra8_io_vfs_open(path, k_ra8_fs_mode_write, &f), s_tag, "open write");
  const ra8_err_t we = ra8_fs_write(f, blob, blob_len);
  const ra8_err_t ce = ra8_fs_close(f);
  if (we != k_ra8_ok) {
    return we;
  }
  if (ce != k_ra8_ok) {
    return ce;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_io_vfs_write_compressed(const char*    path,
                                      const uint8_t* src,
                                      uint32_t       src_len,
                                      void*          scratch,
                                      uint32_t       scratch_len,
                                      uint8_t*       blob_buf,
                                      uint32_t       blob_cap,
                                      uint32_t*      out_blob_len)
{
  RA8_RETURN_ON_ERROR(
    internal_ra8_io_vfs_compress_write_validate(path, src, scratch, blob_buf, out_blob_len),
    s_tag,
    "validate args");

  uint32_t blob_len = 0;
  RA8_RETURN_ON_ERROR(
    ra8_io_compress(src, src_len, blob_buf, blob_cap, scratch, scratch_len, &blob_len),
    s_tag,
    "compress");

  RA8_RETURN_ON_ERROR(internal_ra8_io_vfs_compress_store_blob(path, blob_buf, blob_len),
                      s_tag,
                      "store");
  *out_blob_len = blob_len;
  return k_ra8_ok;
}

/**
 * @brief Validate the four caller pointers handed to
 *        ::ra8_io_vfs_read_compressed.
 *
 * @details
 * Rejects any NULL pointer argument before the VFS or the codec is touched,
 * keeping the public entry point's body short and its statement count under
 * the NASA Rule 4 / clang-tidy threshold. Each guard is a single condition
 * (`ptr == nullptr`), so no compound decision is split across this helper and
 * its caller and no MC/DC vector is due.
 *
 * @param[in] path     `"name:/sub"` source string.
 * @param[in] blob_buf Caller staging buffer for the on-disk compressed blob.
 * @param[in] out      Destination for the decompressed payload.
 * @param[in] out_len  Out-param for the decompressed byte count.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Every pointer argument is non-NULL.
 * @retval k_ra8_err_null_ptr Some pointer argument was NULL.
 *
 * @pre The caller passes the same pointers it received from its own caller.
 * @pre `s_tag` is initialised (it is a file-scope constant).
 * @post On `k_ra8_ok` all four pointers are safe to dereference.
 * @post On `k_ra8_err_null_ptr` no pointer was dereferenced.
 *
 * @note Pure validation; not thread-safe-sensitive (reads no shared state).
 *
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static ra8_err_t internal_ra8_io_vfs_compress_read_validate(const char*     path,
                                                                         const uint8_t*  blob_buf,
                                                                         const uint8_t*  out,
                                                                         const uint32_t* out_len)
{
  RA8_CHECK_NULL_PTR(path, s_tag, "path must not be nullptr");
  RA8_CHECK_NULL_PTR(blob_buf, s_tag, "blob_buf must not be nullptr");
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  RA8_CHECK_NULL_PTR(out_len, s_tag, "out_len must not be nullptr");
  return k_ra8_ok;
}

/**
 * @brief Open `path` for read, pull the whole blob into `blob_buf`, and close.
 *
 * @details
 * Resolves `path` through the VFS in read mode, copies up to `blob_cap` bytes
 * into `blob_buf`, and closes the file. The read and close error codes are
 * captured separately so the file is always closed even when the read fails;
 * the read error takes precedence over the close error. Because `ra8_fs_read`
 * caps the copy at `blob_cap`, a blob that fills `blob_buf` exactly cannot be
 * distinguished from a truncated larger file, so a full-buffer read is rejected
 * as ::k_ra8_err_invalid_size. Each guard is a single condition, so no compound
 * decision is split across this helper and its caller.
 *
 * @param[in]  path         `"name:/sub"` source string (already validated).
 * @param[out] blob_buf     Caller staging buffer for the on-disk blob.
 * @param[in]  blob_cap     Capacity of `blob_buf` in bytes.
 * @param[out] out_blob_len Byte count read into `blob_buf` on success.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Blob read and `*out_blob_len` set.
 * @retval k_ra8_err_invalid_size The blob filled `blob_buf` (possible truncation).
 * @retval k_ra8_err_*            Propagated from the VFS open, read, or close.
 *
 * @pre `path`, `blob_buf`, and `out_blob_len` are non-NULL (caller validated).
 * @pre The named volume in `path` is mounted and the file exists.
 * @post On `k_ra8_ok` `blob_buf[0 .. *out_blob_len)` holds the on-disk blob.
 * @post On any non-ok return the file handle is closed (no leak).
 *
 * @note Not thread-safe with respect to `blob_buf`.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static ra8_err_t internal_ra8_io_vfs_compress_load_blob(const char* path,
                                                                     uint8_t*    blob_buf,
                                                                     uint32_t    blob_cap,
                                                                     uint32_t*   out_blob_len)
{
  ra8_fs_file_t* f = nullptr;
  RA8_RETURN_ON_ERROR(ra8_io_vfs_open(path, k_ra8_fs_mode_read, &f), s_tag, "open read");
  uint32_t        blob_len = 0;
  const ra8_err_t re       = ra8_fs_read(f, blob_buf, blob_cap, &blob_len);
  const ra8_err_t ce       = ra8_fs_close(f);
  if (re != k_ra8_ok) {
    return re;
  }
  if (ce != k_ra8_ok) {
    return ce;
  }
  if (blob_len >= blob_cap) {
    return k_ra8_err_invalid_size;
  }
  *out_blob_len = blob_len;
  return k_ra8_ok;
}

ra8_err_t ra8_io_vfs_read_compressed(const char* path,
                                     uint8_t*    blob_buf,
                                     uint32_t    blob_cap,
                                     uint8_t*    out,
                                     uint32_t    out_cap,
                                     uint32_t*   out_len)
{
  RA8_RETURN_ON_ERROR(internal_ra8_io_vfs_compress_read_validate(path, blob_buf, out, out_len),
                      s_tag,
                      "validate args");

  uint32_t blob_len = 0;
  RA8_RETURN_ON_ERROR(internal_ra8_io_vfs_compress_load_blob(path, blob_buf, blob_cap, &blob_len),
                      s_tag,
                      "load");

  RA8_RETURN_ON_ERROR(ra8_io_decompress(blob_buf, blob_len, out, out_cap, out_len),
                      s_tag,
                      "inflate");
  return k_ra8_ok;
}
