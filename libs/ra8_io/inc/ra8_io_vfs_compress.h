/**
 * @file ra8_io_vfs_compress.h
 * @brief Transparent VFS-level whole-file compression (compress-on-write,
 * @ingroup grp_io
 *        decompress-on-read) over the ra8_io fabric.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * This layer makes DEFLATE compression a transparent property of a file: the
 * caller hands a plain payload and a `"name:/path"` string, and the byte stream
 * on the backing store is the compressed blob -- the app never touches the
 * codec, ::ra8_io_compress, or `ra8_fs_write_file` directly. Reading reverses the
 * pipeline: the blob is pulled off the backing store and inflated into the
 * caller's output buffer.
 *
 * Both directions are whole-file (not an incremental stream filter), which
 * matches the firmware's real `.rabook` RBKC use case and keeps the path simple
 * and heap-free. Because the seam sits above the VFS, it composes over WHATEVER
 * backend is mounted (RAM, SDRAM, SD/SDHI, OSPI, MRAM) -- the same call writes a
 * compressed file to any of them.
 *
 * Every buffer is caller-provided, so the allocator (which this firmware traps)
 * is never touched: the caller supplies the tdefl scratch, the staging blob
 * buffer, and the decompressed-output buffer.
 *
 * This header is opt-in: it pulls in ::ra8_io_compress (and through it miniz), so
 * it is NOT part of the `ra8_io.h` umbrella -- exactly like `ra8_io_compress.h`.
 * Apps that want transparent compression include it directly and add `miniz` to
 * their `ra8_add_app(... LIBS ...)`.
 *
 * @code
 * static uint8_t scratch[(size_t)k_ra8_io_compress_scratch_bytes];
 * static uint8_t blob[(size_t)k_demo_blob_cap];
 * uint32_t       blob_len = 0;
 * (void)ra8_io_vfs_write_compressed("ram:/STORY.RBK", payload, payload_len,
 *                                  scratch, sizeof(scratch),
 *                                  blob, sizeof(blob), &blob_len);
 * uint32_t out_len = 0;
 * (void)ra8_io_vfs_read_compressed("ram:/STORY.RBK", blob, sizeof(blob),
 *                                 restored, sizeof(restored), &out_len);
 * @endcode
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_io_compress.h"

/**
 * @brief Compress a payload and store it as a whole file through the VFS.
 *
 * @details
 * Resolves `path` (`"name:/sub"`) through the VFS, DEFLATE-compresses `src` into
 * the caller-provided `blob_buf` via ::ra8_io_compress (using the caller-provided
 * `scratch` for the miniz compressor), then writes the resulting blob to the
 * file over whatever backend backs the named mount. No allocator is touched:
 * both the compressor scratch and the staging blob buffer come from the caller.
 *
 * @param[in]  src          Plain payload bytes to compress.
 * @param[in]  src_len      Number of input bytes (> 0).
 * @param[in]  path         `"name:/sub"` destination string.
 * @param[in]  scratch      Caller buffer >= ::k_ra8_io_compress_scratch_bytes,
 *                          8-byte aligned (holds the miniz compressor).
 * @param[in]  scratch_len  Size of `scratch` in bytes.
 * @param[out] blob_buf     Caller staging buffer for the compressed blob.
 * @param[in]  blob_cap     Capacity of `blob_buf` in bytes.
 * @param[out] out_blob_len Compressed byte count written to the file on success.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Compressed and stored; `*out_blob_len` set.
 * @retval k_ra8_err_null_ptr     Any pointer argument was NULL.
 * @retval k_ra8_err_invalid_size `scratch_len` is below the minimum.
 * @retval k_ra8_err_no_mem       `blob_buf` was too small for the result.
 * @retval k_ra8_err_invalid_arg  `path` has no `name:` prefix.
 * @retval k_ra8_err_not_found    The mount name is absent.
 * @retval k_ra8_err_*            Propagated from the compressor or the VFS write.
 *
 * @pre `scratch` is at least ::k_ra8_io_compress_scratch_bytes and 8-aligned.
 * @pre The named volume in `path` is mounted.
 * @post On success the file holds a raw-DEFLATE blob of `*out_blob_len` bytes.
 * @post On any non-ok return no file is created and the buffers are unspecified.
 *
 * @note Not thread-safe with respect to the same scratch / blob buffer.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_io_vfs_write_compressed(const char*    path,
                                                    const uint8_t* src,
                                                    uint32_t       src_len,
                                                    void*          scratch,
                                                    uint32_t       scratch_len,
                                                    uint8_t*       blob_buf,
                                                    uint32_t       blob_cap,
                                                    uint32_t*      out_blob_len);

/**
 * @brief Read a whole compressed file through the VFS and decompress it.
 *
 * @details
 * Opens `path` (`"name:/sub"`) through the VFS, reads the entire blob into the
 * caller-provided `blob_buf`, then inflates it with ::ra8_io_decompress into
 * `out`. Heap-free: the staging blob buffer and the output buffer are both
 * caller-provided. Reverses ::ra8_io_vfs_write_compressed exactly.
 *
 * @param[in]  path     `"name:/sub"` source string.
 * @param[out] blob_buf Caller staging buffer for the on-disk compressed blob.
 * @param[in]  blob_cap Capacity of `blob_buf` (must hold the whole blob).
 * @param[out] out      Destination for the decompressed payload.
 * @param[in]  out_cap  Capacity of `out` (must hold the whole result).
 * @param[out] out_len  Decompressed byte count on success.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Read and decompressed; `*out_len` set.
 * @retval k_ra8_err_null_ptr     Any pointer argument was NULL.
 * @retval k_ra8_err_invalid_arg  `path` has no `name:` prefix.
 * @retval k_ra8_err_not_found    The mount name or the file is absent.
 * @retval k_ra8_err_invalid_size The blob exceeded `blob_cap`.
 * @retval k_ra8_err_no_mem       `out` was too small, or the blob is corrupt.
 * @retval k_ra8_err_*            Propagated from the VFS read.
 *
 * @pre The named volume in `path` is mounted and the file exists.
 * @pre `out` is large enough to hold the whole decompressed payload.
 * @post On success `out[0 .. *out_len)` holds the original payload.
 * @post On any non-ok return the buffers are unspecified.
 *
 * @note Heap-free; not thread-safe with respect to `blob_buf` / `out`.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_io_vfs_read_compressed(const char* path,
                                                   uint8_t*    blob_buf,
                                                   uint32_t    blob_cap,
                                                   uint8_t*    out,
                                                   uint32_t    out_cap,
                                                   uint32_t*   out_len);

#ifdef __cplusplus
}
#endif
