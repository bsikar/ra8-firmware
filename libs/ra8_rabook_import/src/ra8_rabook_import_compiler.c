/**
 * @file ra8_rabook_import_compiler.c
 * @brief Production adapter binding the import seam to the real compiler (#151).
 * @since Version 0.1.0
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / EPUB Import] {World: NS}
 */

#include "ra8_rabook_import_compiler.h"

#include <stddef.h>
#include <stdint.h>

#include "ra8_book.h"
#include "ra8_check.h"
#include "ra8_epub.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_rabook_pipeline.h"
#include "ra8_vmem.h"
#include "ra8_vmem_stream.h"
#include "ra8_vsource.h"

/** @brief Log tag for this adapter. @since Version 0.1.0 */
static const char* const s_tag = "ra8_rabook_import_compiler";

/**
 * @struct import_stream_t
 * @brief Per-compile streaming state living in the adapter's stack frame.
 * @details Bundles the open source file with the single-slot `ra8_vsource`
 *          registry and the byte-stream binding the opened book reads through
 *          for the whole compile (the book copies a pointer to @p st into its
 *          stream media, so this struct must out-live the compile call).
 * @invariant `file != NULL` between a successful @ref s_stream_open and the
 *            adapter's close; the other members are only valid alongside it.
 * @since Version 0.1.0
 */
typedef struct {
  ra8_fs_file_t*    file; /**< Open source `.epub` (owned until the compile ends). */
  ra8_vsource_obj_t obj;  /**< The registry's single object slot.                  */
  ra8_vsource_t     vsrc; /**< Source registry the cache's loader reads through.   */
  ra8_vmem_stream_t st;   /**< Byte-stream binding handed to the EPUB reader.      */
} import_stream_t;

/**
 * @brief `ra8_vsource_read_fn` over the open source file: seek + full read.
 * @details `ra8_vsource_loader` clamps every request to the registered object
 *          size, so a short read here means the file shrank or the backend
 *          faulted -- reported as `k_ra8_err_hw_error` rather than served as
 *          silently-truncated page bytes. `ra8_fs` offsets are 32-bit, so an
 *          offset past 4 GiB is rejected (the object size cannot exceed the
 *          `ra8_fs_size` result anyway).
 * @param[in]  ctx    The open `ra8_fs_file_t*` (as a void cookie).
 * @param[in]  offset Absolute byte offset within the source file.
 * @param[out] buf    Destination buffer (`len` writable bytes).
 * @param[in]  len    Bytes to read.
 * @return Error code.
 * @retval k_ra8_ok               Exactly @p len bytes were copied to @p buf.
 * @retval k_ra8_err_null_ptr     @p ctx or @p buf is NULL.
 * @retval k_ra8_err_out_of_range @p offset exceeds the 32-bit `ra8_fs` range.
 * @retval k_ra8_err_hw_error     Short read (file shrank / backend fault).
 * @retval k_ra8_err_*            Propagated seek / read error.
 * @pre @p ctx is a live read-mode file handle.
 * @pre @p offset + @p len lies within the registered object size.
 * @post On k_ra8_ok exactly @p len bytes were written to @p buf.
 * @post No state outside @p buf and the file offset is modified.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
static ra8_err_t s_source_read(void* ctx, uint64_t offset, uint8_t* buf, uint32_t len)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "source_read: ctx");
  RA8_CHECK_NULL_PTR(buf, s_tag, "source_read: buf");
  if (offset > (uint64_t)UINT32_MAX) {
    return k_ra8_err_out_of_range; /* GCOVR_EXCL_LINE -- ra8_fs_size is 32-bit, so offsets fit */
  }
  ra8_fs_file_t* file = (ra8_fs_file_t*)ctx;
  ra8_err_t      err  = ra8_fs_seek(file, (uint32_t)offset);
  if (err != k_ra8_ok) {
    return err; /* GCOVR_EXCL_LINE -- seek clamps to size and cannot fail on a live handle */
  }
  uint32_t got = 0U;
  err          = ra8_fs_read(file, buf, len, &got);
  if (err != k_ra8_ok) {
    return err; /* corrupt FAT chain / backend fault mid-stream */
  }
  if (got != len) {
    return k_ra8_err_hw_error; /* GCOVR_EXCL_LINE -- ra8_fs_read reports ok only when fully read */
  }
  return k_ra8_ok;
}

/**
 * @brief Rebuild the cookie's page cache over the open source file.
 * @details Re-initialises the single-slot registry, registers the file as a
 *          paged object of @p size bytes, re-initialises the cookie's
 *          `ra8_vmem` cache over the cookie's frame/meta/bucket arrays (a
 *          fresh, empty cache -- no frame from a previous compile can be
 *          served), and binds @p ss->st as the byte-stream reader. The frame
 *          pool is the compile front-end's entire source-side RAM budget.
 * @param[in]     ctx  Populated compiler cookie (cache storage fields).
 * @param[in,out] ss   Streaming state holding the open file; receives the
 *                     registry + stream binding.
 * @param[in]     size Source file length in bytes (> 0).
 * @return Error code.
 * @retval k_ra8_ok               Cache bound; `ss->st` is readable.
 * @retval k_ra8_err_null_ptr     A cache storage pointer is NULL.
 * @retval k_ra8_err_invalid_size @p size is 0, or a cache dimension is 0.
 * @pre @p ss->file is an open read-mode handle.
 * @pre The cookie's `cache*` fields describe caller-owned storage.
 * @post On k_ra8_ok the cookie's cache is empty and serves the source file.
 * @post On any non-ok return the cache/stream bindings are unusable.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
static ra8_err_t
s_cache_bind(const ra8_rabook_import_compiler_ctx_t* ctx, import_stream_t* ss, uint32_t size)
{
  RA8_CHECK_NULL_PTR(ctx->cache, s_tag, "cache");
  RA8_CHECK_NULL_PTR(ss->file, s_tag, "file");
  ra8_err_t err = ra8_vsource_init(&ss->vsrc, &ss->obj, 1U);
  if (err != k_ra8_ok) {
    return err; /* GCOVR_EXCL_LINE -- registry storage is the frame's own struct, never NULL/zero */
  }
  uint32_t oid = 0U;
  err = ra8_vsource_add_paged(&ss->vsrc, s_source_read, ss->file, 0U, (uint64_t)size, &oid);
  if (err != k_ra8_ok) {
    return err;
  }
  const ra8_vmem_cfg_t cfg = {
    .frame_mem    = ctx->cache_frames,
    .frame_bytes  = ctx->cache_frame_bytes,
    .frame_count  = ctx->cache_frame_count,
    .meta         = ctx->cache_meta,
    .keys         = ctx->cache_keys,
    .buckets      = ctx->cache_buckets,
    .bucket_count = ctx->cache_bucket_count,
    .loader       = ra8_vsource_loader,
    .loader_ctx   = &ss->vsrc,
  };
  *ctx->cache = (ra8_vmem_t){};
  err         = ra8_vmem_init(ctx->cache, &cfg);
  if (err != k_ra8_ok) {
    return err; /* a NULL / zero-sized cookie cache dimension */
  }
  return ra8_vmem_stream_init(&ss->st, ctx->cache, oid, (uint64_t)size);
}

/**
 * @brief Open the source `.epub` as a cache-fronted stream (no residency).
 * @details Opens @p epub_path read-only, sizes it, binds the cookie's page
 *          cache over the open file via @ref s_cache_bind, and opens the book
 *          with `ra8_epub_open_streamed` reading through `ra8_vmem_stream_read`.
 *          On any failure the file is closed and `ss->file` is NULL; on success
 *          the file stays open (the book streams from it) until the adapter
 *          closes it after the compile.
 * @param[in]     ctx       Populated compiler cookie.
 * @param[in,out] mount     Mounted volume holding the source.
 * @param[in]     epub_path Root-level path of the source `.epub`.
 * @param[in,out] ss        Zeroed streaming state; receives the open file +
 *                          stream bindings (must out-live the compile).
 * @return Error code.
 * @retval k_ra8_ok    Book opened; `ctx->epub` streams through the cache.
 * @retval k_ra8_err_* Propagated open / cache-bind / streamed-open error.
 * @pre @p ctx->epub and the cookie's cache storage are populated.
 * @pre @p ss is zero-initialised by the caller.
 * @post On k_ra8_ok `ss->file` is the open source and `ctx->epub->in_use == 1`.
 * @post On any error `ss->file == NULL` (the file was closed here).
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
static ra8_err_t s_stream_open(const ra8_rabook_import_compiler_ctx_t* ctx,
                               ra8_fs_mount_t*                         mount,
                               const char*                             epub_path,
                               import_stream_t*                        ss)
{
  RA8_CHECK_NULL_PTR(ctx->epub, s_tag, "epub");
  RA8_CHECK_NULL_PTR(ss, s_tag, "stream state");
  ra8_err_t err = ra8_fs_open(mount, epub_path, k_ra8_fs_mode_read, &ss->file);
  if (err != k_ra8_ok) {
    return err;
  }
  /* ra8_fs_size cannot fail on the handle ra8_fs_open just returned (in_use
   * set, both out-params non-NULL); on any future contract change `size`
   * stays 0 and s_cache_bind rejects it as invalid_size. */
  uint64_t size64 = 0U;
  (void)ra8_fs_size(ss->file, &size64);
  /* Import sources are sized in 32 bits; an over-4-GiB source is clamped to
   * the reject path (0 -> invalid_size in the bind below). */
  const uint32_t size = (size64 <= (uint64_t)UINT32_MAX) ? (uint32_t)size64 : 0U;
  err                 = s_cache_bind(ctx, ss, size);
  if (err == k_ra8_ok) {
    const ra8_epub_stream_media_t media = {
      .read = ra8_vmem_stream_read,
      .ctx  = &ss->st,
      .size = (uint64_t)size,
    };
    err = ra8_epub_open_streamed(&media, epub_path, ctx->epub);
  }
  if (err != k_ra8_ok) {
    (void)ra8_fs_close(ss->file);
    ss->file = nullptr;
  }
  return err;
}

ra8_err_t ra8_rabook_import_compile_adapter(void*           compile_ctx,
                                            ra8_fs_mount_t* mount,
                                            const char*     epub_path,
                                            const char*     out_path)
{
  RA8_CHECK_NULL_PTR(compile_ctx, s_tag, "compile_ctx");
  RA8_CHECK_NULL_PTR(mount, s_tag, "mount");
  RA8_CHECK_NULL_PTR(epub_path, s_tag, "epub_path");
  RA8_CHECK_NULL_PTR(out_path, s_tag, "out_path");

  /* The cookie's epub/cache fields are validated by the stream-open helpers and
   * the bufs/scr by `ra8_rabook_compile_from_epub`, so a NULL cookie field still
   * yields k_ra8_err_null_ptr without restating those guards here. */
  const ra8_rabook_import_compiler_ctx_t* ctx =
    (const ra8_rabook_import_compiler_ctx_t*)compile_ctx;

  /* The book streams through `ss` for the whole compile, so it lives in this
   * frame, spanning open -> compile -> close. */
  import_stream_t ss  = {};
  ra8_err_t       err = s_stream_open(ctx, mount, epub_path, &ss);
  if (err != k_ra8_ok) {
    return err;
  }

  err            = ra8_rabook_compile_from_epub(ctx->epub, ctx->bufs, ctx->scr, mount, out_path);
  ra8_err_t cerr = ra8_epub_close(ctx->epub);
  (void)ra8_fs_close(ss.file); /* always release the source handle */
  if (err != k_ra8_ok) {
    return err;
  }
  return cerr;
}

/**
 * @brief Read a whole file off @p mount into @p buf, closing it on every path.
 * @details Opens @p path read-only, rejects a file larger than @p cap with
 *          `k_ra8_err_no_mem` (a transport overflow -- the M33 offload needs
 *          the source resident in shared memory, so an oversized source must
 *          surface as a fallback trigger, not a silent truncation), pulls the
 *          bytes into @p buf with a single @ref ra8_fs_read (byte count
 *          returned in @p *out_len), then closes the handle unconditionally.
 *          The read error is returned in preference to the close error so a
 *          partial read is not masked.
 * @param[in]  mount   Mounted volume (non-NULL).
 * @param[in]  path    Root-level source path (non-NULL).
 * @param[out] buf     Destination buffer (non-NULL).
 * @param[in]  cap     Capacity of @p buf in bytes.
 * @param[out] out_len Receives the number of bytes read on success.
 * @return Error code.
 * @retval k_ra8_ok         The file was read; @p *out_len holds its length.
 * @retval k_ra8_err_no_mem The file is larger than @p cap (fallback trigger).
 * @retval k_ra8_err_*      Propagated open / read / close error.
 * @pre @p buf and @p out_len are non-NULL.
 * @pre @p path names an existing file on @p mount.
 * @post On success @p buf holds @p *out_len bytes and the file is closed.
 * @post On any error the file is closed if it was opened.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
static ra8_err_t s_read_whole_file(ra8_fs_mount_t* mount,
                                   const char*     path,
                                   uint8_t*        buf,
                                   uint32_t        cap,
                                   uint32_t*       out_len)
{
  ra8_fs_file_t* file = nullptr;
  ra8_err_t      err  = ra8_fs_open(mount, path, k_ra8_fs_mode_read, &file);
  if (err != k_ra8_ok) {
    return err;
  }
  /* ra8_fs_size cannot fail on a just-opened handle; a lying 0 simply reads 0
   * bytes below and the dispatch rejects the empty source. */
  uint64_t size64 = 0U;
  (void)ra8_fs_size(file, &size64);
  const uint32_t size = (size64 <= (uint64_t)UINT32_MAX) ? (uint32_t)size64 : UINT32_MAX;
  if (size > cap) {
    /* Transport overflow: the source cannot fit the cross-core buffer. Report
     * no_mem (an offload-failure class) so the streamed in-core fallback --
     * which has no size ceiling (#230) -- can still import the book. */
    (void)ra8_fs_close(file);
    return k_ra8_err_no_mem;
  }
  err            = ra8_fs_read(file, buf, cap, out_len);
  ra8_err_t cerr = ra8_fs_close(file);
  if (err != k_ra8_ok) {
    return err;
  }
  return cerr;
}

/**
 * @brief Dispatch the staged compile to the secondary core, validate, cache it.
 * @details Calls the cookie's @p dispatch seam to run the compile on the M33,
 *          which emits a RABOOK1 blob (length in @p blob_len) into
 *          @p ctx->blob_buf; the blob is then run through @ref ra8_book_validate
 *          to catch a cross-core transfer slip or worker fault before it is
 *          written to @p out_path with @ref ra8_fs_write_file.
 * @param[in]     ctx      Populated M33 cookie (dispatch + buffers).
 * @param[in]     epub_len Source length already in @p ctx->epub_load_buf.
 * @param[in,out] mount    Mounted volume the validated blob is written to.
 * @param[in]     out_path Path to write the RABOOK1 body to (importer temp name).
 * @return Error code.
 * @retval k_ra8_ok    Blob dispatched, validated, and written to @p out_path.
 * @retval k_ra8_err_* Propagated dispatch / validate / write error.
 * @pre @p ctx->dispatch and @p ctx->blob_buf are non-NULL.
 * @pre @p ctx->epub_load_buf holds @p epub_len readable bytes.
 * @post On success @p out_path holds a `ra8_book_validate`-clean blob.
 * @post On any error @p out_path is not written.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
static ra8_err_t s_dispatch_and_cache(const ra8_rabook_import_compiler_m33_ctx_t* ctx,
                                      uint32_t                                    epub_len,
                                      ra8_fs_mount_t*                             mount,
                                      const char*                                 out_path)
{
  uint32_t  blob_len = 0U;
  ra8_err_t err      = ctx->dispatch(ctx->dispatch_ctx,
                                     ctx->epub_load_buf,
                                     epub_len,
                                     ctx->blob_buf,
                                     ctx->blob_cap,
                                     &blob_len);
  if (err != k_ra8_ok) {
    return err;
  }
  /* Validate the worker-produced blob before caching -- a cross-core transfer
   * slip or a worker fault could yield a structurally-broken blob. */
  err = ra8_book_validate(ctx->blob_buf, (size_t)blob_len);
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_fs_write_file(mount, out_path, ctx->blob_buf, blob_len);
}

/**
 * @brief Classify an offload result: does @p err warrant an in-core retry?
 * @details A TIMEOUT/FAULT -- the worker stalled, faulted, or never finished --
 *          surfaces from the dispatch seam as @ref k_ra8_err_hw_error; a blob too
 *          large for the cross-core transport surfaces as @ref k_ra8_err_no_mem.
 *          Both mean the OFFLOAD failed though the source `.epub` may compile
 *          cleanly, so the in-core path is worth a try. A caller bug
 *          (`k_ra8_err_null_ptr`) or an FS slip would only recur in-core and so is
 *          not a fallback trigger; success is handled before this is reached.
 * @param[in] err Non-OK error returned by @ref s_dispatch_and_cache.
 * @return Whether @p err warrants the in-core fallback.
 * @retval true  @p err is @ref k_ra8_err_hw_error or @ref k_ra8_err_no_mem.
 * @retval false Any other code.
 * @pre @p err is a value @ref s_dispatch_and_cache can return.
 * @pre Reached only after the offload returned non-OK.
 * @post No state is mutated (pure predicate).
 * @post The result depends only on @p err.
 * @note Pure; thread-safe.
 * @note The two-condition decision below is the only compound boolean in this
 *       module; its N+1 = 3 MC/DC vectors live in tests/test_ra8_rabook_import_m33.c
 *       (the fallback-on-timeout / fallback-on-oom / no-fallback-on-other-error
 *       cases), which cite this function in their `@par MC/DC:` blocks.
 * @since Version 0.1.0
 */
static bool s_is_dispatch_failure(ra8_err_t err)
{
  return (err == k_ra8_err_hw_error) || (err == k_ra8_err_no_mem);
}

/**
 * @brief Retry a failed offload in-core, or propagate the offload error.
 * @details The shared classify-and-retry tail of both offload stages (the
 *          source read and the dispatch): a TIMEOUT/FAULT or transport
 *          overflow (see @ref s_is_dispatch_failure) means the source `.epub`
 *          is likely fine, so the compile is retried IN-CORE via the cookie's
 *          @p fallback -- the import still yields a valid `.rabook`. Other
 *          errors, and the no-fallback case, propagate @p err unchanged.
 * @param[in]     ctx       Populated M33 cookie (optional fallback).
 * @param[in]     err       The non-OK offload-stage error to classify.
 * @param[in,out] mount     Mounted volume holding the source and the output.
 * @param[in]     epub_path Root-level path of the source `.epub` (for the retry).
 * @param[in]     out_path  Path to write the RABOOK1 body to (importer temp name).
 * @return Error code.
 * @retval k_ra8_ok    The in-core fallback compiled and wrote the blob.
 * @retval k_ra8_err_* @p err propagated, or the in-core fallback's error.
 * @pre @p err is a non-OK offload-stage result.
 * @pre @p ctx->fallback is NULL or a fully-populated in-core cookie.
 * @post On `k_ra8_ok`, @p out_path holds a `ra8_book_validate`-clean blob.
 * @post On any error, @p out_path is not written.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
static ra8_err_t s_fallback_or_propagate(const ra8_rabook_import_compiler_m33_ctx_t* ctx,
                                         ra8_err_t                                   err,
                                         ra8_fs_mount_t*                             mount,
                                         const char*                                 epub_path,
                                         const char*                                 out_path)
{
  if (!s_is_dispatch_failure(err)) {
    return err;
  }
  if (ctx->fallback == nullptr) {
    return err;
  }
  ra8_log_warn(s_tag, "M33 offload failed; compiling in-core instead");
  return ra8_rabook_import_compile_adapter(ctx->fallback, mount, epub_path, out_path);
}

/**
 * @brief Run the M33 offload, retrying in-core when the offload itself failed.
 * @details Dispatches to @ref s_dispatch_and_cache; a clean result is used
 *          as-is and any failure is classified by @ref s_fallback_or_propagate.
 * @param[in]     ctx      Populated M33 cookie (dispatch + optional fallback).
 * @param[in]     epub_len Source length already in @p ctx->epub_load_buf.
 * @param[in,out] mount    Mounted volume the validated blob is written to.
 * @param[in]     epub_path Root-level path of the source `.epub` (for the retry).
 * @param[in]     out_path Path to write the RABOOK1 body to (importer temp name).
 * @return Error code.
 * @retval k_ra8_ok    Blob produced (on the M33, or in-core on fallback) and written.
 * @retval k_ra8_err_* Propagated offload error, or the in-core fallback error.
 * @pre @p ctx is non-NULL with a non-NULL dispatch.
 * @pre @p ctx->epub_load_buf holds @p epub_len readable bytes.
 * @post On `k_ra8_ok`, @p out_path holds a `ra8_book_validate`-clean blob.
 * @post On any error, @p out_path is not written.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
static ra8_err_t s_offload_or_fallback(const ra8_rabook_import_compiler_m33_ctx_t* ctx,
                                       uint32_t                                    epub_len,
                                       ra8_fs_mount_t*                             mount,
                                       const char*                                 epub_path,
                                       const char*                                 out_path)
{
  ra8_err_t err = s_dispatch_and_cache(ctx, epub_len, mount, out_path);
  if (err == k_ra8_ok) {
    return k_ra8_ok;
  }
  return s_fallback_or_propagate(ctx, err, mount, epub_path, out_path);
}

ra8_err_t ra8_rabook_import_compile_adapter_m33(void*           compile_ctx,
                                                ra8_fs_mount_t* mount,
                                                const char*     epub_path,
                                                const char*     out_path)
{
  RA8_CHECK_NULL_PTR(compile_ctx, s_tag, "compile_ctx");
  RA8_CHECK_NULL_PTR(mount, s_tag, "mount");
  RA8_CHECK_NULL_PTR(epub_path, s_tag, "epub_path");
  RA8_CHECK_NULL_PTR(out_path, s_tag, "out_path");

  ra8_rabook_import_compiler_m33_ctx_t* ctx = (ra8_rabook_import_compiler_m33_ctx_t*)compile_ctx;
  RA8_CHECK_NULL_PTR(ctx->dispatch, s_tag, "dispatch");

  /* ctx->epub_load_buf / ctx->blob_buf are caller-owned per the cookie invariant
   * and reached through ra8_fs_read / the dispatch, so a NULL there is a caller
   * bug -- matching how the in-core adapter leans on its downstream guards. */

  /* Read the source .epub off the mount (the M85 owns the FS), then offload. A
   * read-stage transport overflow (source larger than the load buffer, #230)
   * is an offload failure like any other: the streamed in-core fallback still
   * imports the book. */
  uint32_t  epub_len = 0U;
  ra8_err_t err =
    s_read_whole_file(mount, epub_path, ctx->epub_load_buf, ctx->epub_load_cap, &epub_len);
  if (err != k_ra8_ok) {
    return s_fallback_or_propagate(ctx, err, mount, epub_path, out_path);
  }
  return s_offload_or_fallback(ctx, epub_len, mount, epub_path, out_path);
}
