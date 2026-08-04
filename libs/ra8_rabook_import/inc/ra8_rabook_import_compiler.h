/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_rabook_import_compiler.h
 * @brief Production adapter binding the import seam to the real compiler (#151).
 * @ingroup grp_ereader
 *
 * @details
 * @ref ra8_rabook_import.h keeps the cache manager decoupled from the EPUB parser
 * and the RABOOK1 emitter by reaching the compile step through the injected
 * @ref ra8_rabook_import_compile_fn seam. This adapter is the production binding
 * of that seam: it STREAMS the source `.epub` off the filesystem through a
 * bounded `ra8_vmem` page cache (#230 -- no whole-file load buffer, so a source
 * far larger than RAM compiles inside a fixed frame-pool budget), drives
 * @ref ra8_rabook_compile_from_epub, and closes the book -- matching the seam's
 * exact signature so an app passes `ra8_rabook_import_compile_adapter` (with a
 * @ref ra8_rabook_import_compiler_ctx_t cookie) straight into
 * @ref ra8_rabook_import_cfg_t.
 *
 * Host tests bypass this adapter entirely with a lightweight compile spy, which
 * is the whole point of the seam -- they need neither the parser nor the
 * multi-megabyte builder arenas.
 *
 * @note Not thread-safe; the cookie's storage is single-owner.
 * @see ra8_rabook_import.h          The cache manager that calls this seam.
 * @see ra8_rabook_compile_from_epub The compiler this wraps.
 * @since Version 0.1.0
 *
 * [Ring 4 / EPUB Import] {World: NS}
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "ra8_dual_core_job.h"
#include "ra8_epub.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_rabook_pipeline.h"
#include "ra8_vmem.h"

/**
 * @struct ra8_rabook_import_compiler_ctx_t
 * @brief Cookie carrying the storage @ref ra8_rabook_import_compile_adapter needs.
 *
 * @details All storage is caller-owned. The adapter never loads the whole
 *          `.epub` (#230): it opens the source file, registers it as a paged
 *          `ra8_vsource` object, re-initialises @p cache over the `cache_*`
 *          arrays (so no stale frame from a previous compile can be served),
 *          and streams every ZIP read through `ra8_vmem_stream_read`. The
 *          fixed frame pool (`cache_frame_count * cache_frame_bytes` bytes)
 *          is therefore the compile front-end's entire source-side RAM
 *          high-water mark, independent of the book size. After a compile the
 *          caller may read `ra8_vmem_stats(cache, ...)` for cache telemetry.
 *          The compile itself runs via the builder arenas in @p bufs and the
 *          decode scratch in @p scr.
 *
 * @invariant Every pointer is non-NULL and each array holds its stated count.
 * @invariant `cache_meta` has `cache_frame_count` entries; `cache_frames` has
 *            `cache_frame_count * cache_frame_bytes` bytes; `cache_buckets`
 *            has `cache_bucket_count` heads.
 * @see ra8_vmem_stream_read
 * @since Version 0.1.0
 */
typedef struct {
  ra8_epub_book_t*            epub;               /**< Caller-owned open-book storage.        */
  ra8_vmem_t*                 cache;              /**< Page cache re-initialised per compile. */
  uint8_t*                    cache_frames;       /**< Frame pool backing the cache.          */
  uint32_t                    cache_frame_bytes;  /**< Bytes per cache frame (e.g. 4096).     */
  uint32_t                    cache_frame_count;  /**< Frames in the pool (the RAM budget).   */
  ra8_vmem_frame_t*           cache_meta;         /**< Per-frame metadata array.              */
  ra8_vmem_key_t*             cache_keys;         /**< Per-frame key-storage array.           */
  int32_t*                    cache_buckets;      /**< Cache hash-bucket heads.               */
  uint32_t                    cache_bucket_count; /**< Number of hash buckets (>= 1).         */
  const ra8_rabook_buffers_t* bufs;               /**< RABOOK1 builder arenas.                */
  const ra8_rabook_pipeline_scratch_t* scr;       /**< XHTML + image decode scratch.          */
} ra8_rabook_import_compiler_ctx_t;

/**
 * @brief Import-seam adapter: stream a `.epub` off @p mount and compile it.
 *
 * @details Signature matches @ref ra8_rabook_import_compile_fn. Opens the source
 *          file, fronts it with the cookie's bounded `ra8_vmem` page cache, opens
 *          the book with `ra8_epub_open_streamed` (no whole-file residency, #230),
 *          runs `ra8_rabook_compile_from_epub` to @p out_path, and closes the
 *          book and the source file on every path.
 *
 * @param[in]     compile_ctx Pointer to a @ref ra8_rabook_import_compiler_ctx_t.
 * @param[in,out] mount       Mounted volume holding the source and the output.
 * @param[in]     epub_path   Root-level 8.3 path of the source `.epub`.
 * @param[in]     out_path    Path to write the RABOOK1 body to (the importer's
 *                            temp name).
 *
 * @return Error code.
 * @retval k_ra8_ok           Book compiled and written to @p out_path.
 * @retval k_ra8_err_null_ptr A required pointer (incl. a cookie field) is NULL.
 * @retval k_ra8_err_*        Propagated open / cache-bind / compile error.
 *
 * @pre @p compile_ctx points at a fully-populated cookie.
 * @pre @p mount, @p epub_path, and @p out_path are non-NULL.
 * @post On `k_ra8_ok`, @p out_path holds a valid RABOOK1 blob and the book is closed.
 * @post The source file handle is closed on every return path.
 *
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rabook_import_compile_adapter(void*           compile_ctx,
                                                          ra8_fs_mount_t* mount,
                                                          const char*     epub_path,
                                                          const char*     out_path);

/**
 * @struct ra8_rabook_import_compiler_m33_ctx_t
 * @brief Cookie for @ref ra8_rabook_import_compile_adapter_m33 (the M33-offload binding).
 *
 * @details Carries no parser/builder arenas -- those live on the secondary core.
 *          The adapter reads the source `.epub` into @p epub_load_buf, hands the
 *          bytes to @p dispatch (which compiles on the M33 and returns the blob in
 *          @p blob_buf), validates, and writes the blob out. @p dispatch is the
 *          DI seam: production binds the shared-mailbox shim, host tests bind a mock.
 *          The whole-file read is inherent to the cross-core transport (the M33
 *          needs the source resident in shared memory); a source larger than
 *          @p epub_load_cap is reported as a transport overflow
 *          (`k_ra8_err_no_mem`) so the streamed in-core fallback -- which has no
 *          size ceiling (#230) -- can still import it.
 *
 *          @p fallback is the robustness seam: when the offload fails with a
 *          TIMEOUT/FAULT or a transport overflow (the `.epub` itself may be fine),
 *          the adapter retries the compile IN-CORE through this cookie so the
 *          import still yields a valid `.rabook`. It is OPTIONAL -- leave it NULL
 *          to keep the adapter offload-only (the offload error then propagates).
 *
 * @invariant @p epub_load_buf, @p blob_buf and @p dispatch are non-NULL and each
 *            buffer holds its stated capacity.
 * @invariant @p fallback is either NULL or a fully-populated in-core cookie.
 * @see ra8_dual_core_compile_dispatch_fn
 * @see ra8_rabook_import_compiler_ctx_t
 * @since Version 0.1.0
 */
typedef struct {
  uint8_t* epub_load_buf; /**< Buffer the source `.epub` is read into.      */
  uint32_t epub_load_cap; /**< Capacity of @p epub_load_buf in bytes.       */
  uint8_t* blob_buf;      /**< Buffer the dispatched RABOOK1 blob lands in. */
  uint32_t blob_cap;      /**< Capacity of @p blob_buf in bytes.            */
  /** @brief Cross-core compile seam (non-NULL). */
  ra8_dual_core_compile_dispatch_fn dispatch;
  void*                             dispatch_ctx; /**< Cookie forwarded to @p dispatch. */
  /** @brief In-core retry cookie used if the offload fails; NULL = offload-only. */
  ra8_rabook_import_compiler_ctx_t* fallback;
} ra8_rabook_import_compiler_m33_ctx_t;

/**
 * @brief Import-seam adapter that offloads the compile to the Cortex-M33 (#149).
 *
 * @details Signature matches @ref ra8_rabook_import_compile_fn. Reads the source
 *          `.epub` off @p mount, dispatches the compile to the secondary core via
 *          the cookie's @ref ra8_dual_core_compile_dispatch_fn, validates the
 *          returned blob with `ra8_book_validate`, then writes it to @p out_path
 *          (the manager renames it into the cache). The M85 owns the filesystem;
 *          the M33 only produces the blob in shared memory.
 *
 *          If the offload fails with a TIMEOUT/FAULT (`k_ra8_err_hw_error`) or a
 *          transport overflow (`k_ra8_err_no_mem`) and the cookie carries a
 *          @ref ra8_rabook_import_compiler_ctx_t.fallback, the compile is retried
 *          IN-CORE so a transient secondary-core failure never fails the import.
 *          A clean offload result is used directly; other errors propagate.
 *
 * @param[in]     compile_ctx Pointer to a @ref ra8_rabook_import_compiler_m33_ctx_t.
 * @param[in,out] mount       Mounted volume holding the source and the output.
 * @param[in]     epub_path   Root-level 8.3 path of the source `.epub`.
 * @param[in]     out_path    Path to write the RABOOK1 body to (importer temp name).
 *
 * @return Error code.
 * @retval k_ra8_ok           Blob produced (on the M33, or in-core on fallback),
 *                           validated, and written to @p out_path.
 * @retval k_ra8_err_null_ptr A required pointer (incl. a cookie field) is NULL.
 * @retval k_ra8_err_*        Propagated read / dispatch / validate / write error
 *                           (the in-core fallback error when that path is taken).
 *
 * @pre @p compile_ctx points at a fully-populated cookie with a non-NULL dispatch.
 * @pre @p mount, @p epub_path, and @p out_path are non-NULL.
 * @post On `k_ra8_ok`, @p out_path holds a `ra8_book_validate`-clean RABOOK1 blob.
 * @post On any error, @p out_path is not written.
 *
 * @note Not thread-safe.
 * @see ra8_dual_core_compile_dispatch_fn
 * @see ra8_rabook_import_compile_adapter
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rabook_import_compile_adapter_m33(void*           compile_ctx,
                                                              ra8_fs_mount_t* mount,
                                                              const char*     epub_path,
                                                              const char*     out_path);

#ifdef __cplusplus
}
#endif
