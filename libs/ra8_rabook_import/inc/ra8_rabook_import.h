/**
 * @file ra8_rabook_import.h
 * @brief On-import EPUB -> .rabook compile-and-cache manager (#151).
 * @ingroup grp_ereader
 *
 * @details
 * `ra8_rabook_import` makes the e-reader a self-contained appliance: drop a raw
 * `.epub` on the SD card and the first open compiles it **once** into the flat,
 * execute-in-place `.rabook` blob (via the injected compiler seam), caches the
 * result on the card, and takes the fast cached path on every subsequent open.
 *
 * This header is the cache manager + import state machine only. It owns:
 *   - the on-disk cache convention (a root-level 8.3 name keyed by the source's
 *     CRC-32: `XXXXXXXX.RBK` cache + `XXXXXXXX.RBM` freshness marker),
 *   - the freshness / staleness gate (a sidecar marker that stores the source
 *     size + CRC-32, the `.rabook` format version, and an importer version --
 *     any mismatch forces a re-derive),
 *   - the compile-on-miss + write-through path with a crash-safe temp+rename,
 *   - read-through on a hit (the cached `.rabook` is reused without recompiling).
 *
 * @par Dependency-inversion seam
 * The actual EPUB -> RABOOK1 compile is reached only through the injected
 * @ref ra8_rabook_import_compile_fn function pointer, and the filesystem only
 * through @ref ra8_fs_mount_t. The manager itself depends on nothing but
 * `ra8_fs` and `ra8_core`, so it is host-testable with a lightweight compile spy
 * (no EPUB parser, no image decoder, no 24 MiB arenas). Production wires the
 * real `ra8_rabook_compile_from_epub` behind the seam -- see
 * @ref ra8_rabook_import_compiler.h for the supplied adapter.
 *
 * @par Crash safety
 * The miss path writes `XXXXXXXX.RBT` (temp), then atomically renames it over
 * `XXXXXXXX.RBK`, then writes the marker. A power loss mid-import leaves at most
 * a stale temp (cleaned on the next retry) and never a half-written cache; the
 * `.rabook` body CRC-32 (checked by `ra8_book_open`) is a second line of defence.
 * The source `.epub` is never modified or deleted.
 *
 * @note Not thread-safe: one import at a time on the single reader core.
 * @see ra8_rabook_import_compiler.h  Production adapter onto the real compiler.
 * @see ra8_fs.h                      Filesystem the cache lives on.
 * @since Version 0.1.0
 *
 * [Ring 4 / EPUB Import] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fs.h"

/**
 * @enum ra8_rabook_import_size_t
 * @brief Fixed sizing constants for the cache-name layout.
 * @details The cache entries live in the FAT root with 8.3 short names so the
 *          reader's root-only `ra8_fs_open()` can re-read them. The name is the
 *          8 uppercase-hex digits of the source CRC-32 plus a 3-char extension.
 * @since Version 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_rabook_import_key_hex_len = 8U,  /**< Hex digits of CRC-32 in the name.   */
  k_ra8_rabook_import_name_cap    = 13U, /**< "XXXXXXXX.EXT" + NUL (8.3 + term).  */
  k_ra8_rabook_import_stamp_bytes = 20U, /**< Pinned size of the freshness stamp. */
} ra8_rabook_import_size_t;

/**
 * @enum ra8_rabook_import_magic_t
 * @brief Sentinel stamped into the freshness marker so a stray file is rejected.
 * @details A marker that does not start with this tag (e.g. a same-named file
 *          left by another tool, or a torn write) is treated as absent and the
 *          book is re-derived.
 * @since Version 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_rabook_import_stamp_magic = 0x52424B31U, /**< Importer marker tag "RBK1". */
} ra8_rabook_import_magic_t;

/**
 * @enum ra8_rabook_import_outcome_t
 * @brief How @ref ra8_rabook_import_open satisfied the request.
 * @since Version 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_rabook_import_hit      = 0U, /**< Fresh cache reused; the compiler was NOT invoked. */
  k_ra8_rabook_import_compiled = 1U, /**< Miss/stale; the book was compiled and cached.     */
} ra8_rabook_import_outcome_t;

/**
 * @struct ra8_rabook_import_stamp_t
 * @brief Body of the sidecar freshness marker (`XXXXXXXX.RBM`).
 *
 * @details Written verbatim as raw bytes after a successful compile and read
 *          back to gate the fast path. It is an opaque self-describing record
 *          consumed only by the same firmware build that wrote it, so the
 *          little-endian field encoding never crosses a byte-order boundary.
 *          The fast path is taken only when the on-disk stamp equals the stamp
 *          the running firmware expects for the current source (a single
 *          `memcmp`, so there is no compound freshness decision to MC/DC).
 *
 * @invariant `magic == k_ra8_rabook_import_stamp_magic` for a valid marker.
 * @since Version 0.1.0
 */
typedef struct {
  uint32_t magic;            /**< @ref k_ra8_rabook_import_stamp_magic.          */
  uint32_t format_version;   /**< `.rabook` (`RABOOK1`) on-disk layout revision. */
  uint32_t importer_version; /**< Compiler/importer stamp; bump to invalidate.   */
  uint32_t source_size;      /**< Byte length of the source `.epub`.             */
  uint32_t source_crc32;     /**< CRC-32/ISO-HDLC of the source `.epub` bytes.   */
} ra8_rabook_import_stamp_t;

static_assert(sizeof(ra8_rabook_import_stamp_t) == (uint32_t)k_ra8_rabook_import_stamp_bytes,
              "import stamp layout pinned");

/**
 * @typedef ra8_rabook_import_compile_fn
 * @brief Dependency-injection seam: compile one EPUB into a `.rabook` blob.
 *
 * @details The manager calls this on a cache miss to produce the bare `RABOOK1`
 *          body at @p out_path. Production binds the real
 *          `ra8_rabook_compile_from_epub` (through the supplied adapter); host
 *          tests bind a spy that records the call count.
 *
 * @param[in]     compile_ctx Opaque cookie supplied in the config; carries the
 *                            EPUB handle + builder arenas in production.
 * @param[in,out] mount       Mounted volume holding the source and the output.
 * @param[in]     epub_path   Root-level 8.3 path of the source `.epub`.
 * @param[in]     out_path    Root-level 8.3 path to write the `.rabook` body to
 *                            (the manager passes a temp name, then renames it).
 * @return `k_ra8_ok` on success; any non-zero @ref ra8_err_t aborts the import
 *         and the manager falls back without touching the cache.
 * @since Version 0.1.0
 */
typedef ra8_err_t (*ra8_rabook_import_compile_fn)(void*           compile_ctx,
                                                  ra8_fs_mount_t* mount,
                                                  const char*     epub_path,
                                                  const char*     out_path);

/**
 * @struct ra8_rabook_import_cfg_t
 * @brief Injected dependencies + versioning for @ref ra8_rabook_import_open.
 *
 * @details Open/Closed seam: the manager owns no globals; every external
 *          collaborator (the volume, the compiler, the streaming scratch) is
 *          passed in. @p format_version and @p importer_version are the two
 *          stamps that gate reuse -- pass `k_ra8_book_format_version` and the
 *          firmware's compiler revision so a build that changes the format or
 *          the compiler transparently re-derives stale caches.
 *
 * @invariant `mount`, `compile`, and `scratch` are non-NULL; `scratch_cap > 0`.
 * @since Version 0.1.0
 */
typedef struct {
  ra8_fs_mount_t*              mount;            /**< Mounted FAT volume (cache + source). */
  ra8_rabook_import_compile_fn compile;          /**< Compile-on-miss seam (non-NULL).     */
  void*                        compile_ctx;      /**< Cookie forwarded to @p compile.      */
  uint8_t*                     scratch;          /**< Streaming buffer for the source CRC. */
  uint32_t                     scratch_cap;      /**< Capacity of @p scratch in bytes.     */
  uint32_t                     format_version;   /**< `.rabook` format version stamp.      */
  uint32_t                     importer_version; /**< Importer/compiler version stamp.     */
} ra8_rabook_import_cfg_t;

/**
 * @brief Resolve a source `.epub` to a fresh cached `.rabook`, compiling once.
 *
 * @details
 * State machine on "open `epub_path`":
 *  -# Stream the source bytes through CRC-32 to derive `(size, crc)`; the crc
 *     names the cache entry (`XXXXXXXX.RBK`), so renaming the source does not
 *     orphan its cache and a content change keys a different entry.
 *  -# If the marker `XXXXXXXX.RBM` matches the expected stamp AND the cache file
 *     is present -> **hit**: emit the cache path, do not invoke @p compile.
 *  -# Otherwise -> **miss/stale**: clear any stale temp, call @p compile to
 *     `XXXXXXXX.RBT`, atomically rename it over `XXXXXXXX.RBK`, write the fresh
 *     marker, and emit the cache path.
 *
 * @param[in]  cfg            Injected dependencies + versioning (see struct).
 * @param[in]  epub_path      Root-level 8.3 path of the source `.epub`.
 * @param[out] out_cache_path Receives the NUL-terminated cache `.rabook` path
 *                            (8.3) the caller then opens with `ra8_book_open`.
 * @param[in]  cache_path_cap Capacity of @p out_cache_path in bytes
 *                            (>= @ref k_ra8_rabook_import_name_cap).
 * @param[out] out_outcome    Receives @ref k_ra8_rabook_import_hit or
 *                            @ref k_ra8_rabook_import_compiled on success.
 *
 * @return Error code.
 * @retval k_ra8_ok            Cache is fresh (hit) or freshly written (compiled).
 * @retval k_ra8_err_null_ptr  A required pointer (incl. an injected dependency)
 *                            is NULL.
 * @retval k_ra8_err_invalid_size `cache_path_cap` is too small, or `scratch_cap`
 *                            is zero.
 * @retval k_ra8_err_not_found The source `.epub` does not exist.
 * @retval k_ra8_err_*         Propagated filesystem or compiler error (on a
 *                            compile error the cache is left untouched).
 *
 * @pre `cfg`, `epub_path`, `out_cache_path`, and `out_outcome` are non-NULL.
 * @pre `cfg->mount`, `cfg->compile`, and `cfg->scratch` are non-NULL.
 * @post On `k_ra8_ok`, `out_cache_path` is a NUL-terminated 8.3 path to a present
 *       cache file and `*out_outcome` records hit vs compiled.
 * @post On any error the source `.epub` is unmodified and no half cache remains.
 *
 * @note Not thread-safe; callers serialise imports.
 * @see ra8_rabook_import_compiler.h
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rabook_import_open(const ra8_rabook_import_cfg_t* cfg,
                                               const char*                    epub_path,
                                               char*                          out_cache_path,
                                               uint32_t                       cache_path_cap,
                                               ra8_rabook_import_outcome_t*   out_outcome);

#ifdef __cplusplus
}
#endif
