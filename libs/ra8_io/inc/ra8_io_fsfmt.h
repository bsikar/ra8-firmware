/**
 * @file ra8_io_fsfmt.h
 * @brief ra8_io pluggable filesystem-format registry (probe + capabilities).
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * A small registry that lets the fabric recognise the on-disk filesystem on a
 * block device and report what that format can do, without the upper layers
 * hard-coding a `switch` over FAT vs exFAT. Each format provides a complete
 * operations table beginning with `probe` and a capability descriptor
 * (read-only? sub-directories? streaming writes? name length?). FAT and exFAT are
 * registered as built-ins; a foreign format (a future APFS / NTFS / btrfs / ZFS
 * reader, or a test stub) registers through ::ra8_io_fsfmt_register with no
 * change to the existing code -- that is the pluggability seam.
 *
 * The capability flags are how the fabric degrades gracefully instead of
 * failing: a caller can check `caps.supports_mkdir` before attempting one, and
 * an operation a format lacks returns ::k_ra8_err_not_supported rather than
 * corrupting the volume.
 *
 * @code
 * (void)ra8_io_fsfmt_init();
 * const ra8_io_fsfmt_t* fmt = nullptr;
 * if (ra8_io_fsfmt_probe(&backend, &fmt) == k_ra8_ok) {
 *   // fmt->name e.g. "fat"; fmt->caps.supports_streaming_write etc.
 * }
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
#include "ra8_fs.h"
#include "ra8_fs_meta.h"

/**
 * @enum ra8_io_fsfmt_limits_t
 * @brief Registry sizing.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_io_fsfmt_max = 8, /**< Max registered formats (built-ins + foreign). */
} ra8_io_fsfmt_limits_t;

/**
 * @struct ra8_io_fsfmt_caps_t
 * @brief What a filesystem format supports.
 *
 * @details Lets a caller branch on a format's abilities (and lets the VFS
 *          return ::k_ra8_err_not_supported for an unsupported op) instead of
 *          assuming FAT semantics everywhere.
 *
 * @since 0.1.0
 */
typedef struct {
  uint16_t max_name_len;             /**< Max UTF-8 bytes in one name, excluding NUL.  */
  bool     read_only;                /**< true => no writes/creates/erases.            */
  bool     supports_mkdir;           /**< true => sub-directory creation.              */
  bool     supports_rmdir;           /**< true => empty-directory removal.             */
  bool     supports_streaming_write; /**< true => open-for-write streaming.            */
  bool     supports_timestamps;      /**< true => stat reports entry timestamps.       */
  bool     supports_free_space;      /**< true => a cheap free-space query exists.     */
  bool     supports_sync;            /**< true => an explicit software sync op exists. */
  bool     atomic_rename;            /**< true => rename is power-loss atomic.         */
  bool     durable_sync;             /**< true => sync reaches non-volatile media.     */
  bool     unicode_names;            /**< true => public names are strict UTF-8.       */
  bool     case_sensitive;           /**< true => names compare case-sensitive.        */
} ra8_io_fsfmt_caps_t;

/**
 * @typedef ra8_io_fsfmt_probe_fn_t
 * @brief Returns true if `backend`'s volume looks like this format.
 *
 * @param[in] backend Block-device backend to inspect (reads block 0 etc.).
 * @return true when the on-disk signature matches this format.
 * @since 0.1.0
 */
typedef bool (*ra8_io_fsfmt_probe_fn_t)(const ra8_fs_backend_t* backend);

/** @brief Mount a format over a block backend and return its private context. */
typedef ra8_err_t (*ra8_io_fsfmt_mount_fn_t)(const ra8_fs_backend_t* backend, void** out_mount);
/** @brief Release a context returned by ::ra8_io_fsfmt_mount_fn_t. */
typedef ra8_err_t (*ra8_io_fsfmt_unmount_fn_t)(void* mount_ctx);
/** @brief Open a path and return a format-private file context. */
typedef ra8_err_t (*ra8_io_fsfmt_open_fn_t)(void*         mount_ctx,
                                            const char*   path,
                                            ra8_fs_mode_t mode,
                                            void**        out_file);
/** @brief Close a format-private file context. */
typedef ra8_err_t (*ra8_io_fsfmt_close_fn_t)(void* file_ctx);
/** @brief Read from a format-private file context. */
typedef ra8_err_t (*ra8_io_fsfmt_read_fn_t)(void*     file_ctx,
                                            void*     buf,
                                            uint32_t  bytes,
                                            uint32_t* out_read);
/** @brief Write to a format-private file context. */
typedef ra8_err_t (*ra8_io_fsfmt_write_fn_t)(void* file_ctx, const void* buf, uint32_t bytes);
/** @brief Seek a format-private file context. */
typedef ra8_err_t (*ra8_io_fsfmt_seek_fn_t)(void* file_ctx, uint64_t offset_bytes);
/** @brief Report a format-private file context's offset. */
typedef ra8_err_t (*ra8_io_fsfmt_tell_fn_t)(const void* file_ctx, uint64_t* out_offset);
/** @brief Report a format-private file context's size. */
typedef ra8_err_t (*ra8_io_fsfmt_size_fn_t)(const void* file_ctx, uint64_t* out_bytes);
/** @brief Flush format-owned software state for one file. */
typedef ra8_err_t (*ra8_io_fsfmt_sync_fn_t)(void* file_ctx);
/** @brief Query path metadata in a mounted format. */
typedef ra8_err_t (*ra8_io_fsfmt_stat_fn_t)(void* mount_ctx, const char* path, ra8_fs_stat_t* out);
/** @brief Enumerate one directory in a mounted format. */
typedef ra8_err_t (*ra8_io_fsfmt_listdir_fn_t)(void*               mount_ctx,
                                               const char*         path,
                                               ra8_fs_listdir_cb_t cb,
                                               void*               cb_ctx);
/** @brief Apply a one-path namespace mutation. */
typedef ra8_err_t (*ra8_io_fsfmt_path_fn_t)(void* mount_ctx, const char* path);
/** @brief Rename within one mounted format. */
typedef ra8_err_t (*ra8_io_fsfmt_rename_fn_t)(void*       mount_ctx,
                                              const char* old_path,
                                              const char* new_path);
/** @brief Query capacity and free space in one mounted format. */
typedef ra8_err_t (*ra8_io_fsfmt_space_fn_t)(void* mount_ctx, ra8_fs_space_t* out);

/**
 * @struct ra8_io_fsfmt_ops_t
 * @brief Complete filesystem-format dispatch surface used by the VFS.
 *
 * @details A registered format owns these operations; the VFS never switches
 *          on FAT/exFAT or reaches into format internals. Optional operations
 *          may be NULL and are capability-gated as ::k_ra8_err_not_supported.
 *          All contexts and file handles remain implementation-owned; the VFS
 *          stores only pointers in fixed tables and allocates nothing.
 * @since 0.1.0
 */
typedef struct {
  ra8_io_fsfmt_probe_fn_t   probe;      /**< Recognise an on-disk volume.          */
  ra8_io_fsfmt_mount_fn_t   mount;      /**< Bind a block backend.                 */
  ra8_io_fsfmt_unmount_fn_t unmount;    /**< Release a mounted context.            */
  ra8_io_fsfmt_open_fn_t    open;       /**< Open a file stream.                   */
  ra8_io_fsfmt_close_fn_t   close;      /**< Close a file stream.                  */
  ra8_io_fsfmt_read_fn_t    read;       /**< Read stream bytes.                    */
  ra8_io_fsfmt_write_fn_t   write;      /**< Write stream bytes.                   */
  ra8_io_fsfmt_seek_fn_t    seek;       /**< Set stream offset.                    */
  ra8_io_fsfmt_tell_fn_t    tell;       /**< Query stream offset.                  */
  ra8_io_fsfmt_size_fn_t    size;       /**< Query stream size.                    */
  ra8_io_fsfmt_sync_fn_t    sync;       /**< Flush software state, when supported. */
  ra8_io_fsfmt_stat_fn_t    stat;       /**< Query path metadata.                  */
  ra8_io_fsfmt_listdir_fn_t listdir;    /**< Enumerate a directory.                */
  ra8_io_fsfmt_path_fn_t    unlink;     /**< Remove a plain file.                  */
  ra8_io_fsfmt_rename_fn_t  rename;     /**< Rename within the mount.              */
  ra8_io_fsfmt_path_fn_t    mkdir;      /**< Create a directory.                   */
  ra8_io_fsfmt_path_fn_t    rmdir;      /**< Remove an empty directory.            */
  ra8_io_fsfmt_space_fn_t   free_space; /**< Query free/total bytes.               */
} ra8_io_fsfmt_ops_t;

/**
 * @struct ra8_io_fsfmt_t
 * @brief One registered filesystem format.
 *
 * @details A format is a `const` instance the caller (or a built-in) exports;
 *          the registry holds pointers to them, so they must out-live it.
 *
 * @invariant `name`, `ops`, and every mandatory read-side operation are non-NULL.
 *
 * @since 0.1.0
 */
typedef struct {
  const char*               name; /**< Short format name, e.g. "fat" / "exfat". */
  ra8_io_fsfmt_caps_t       caps; /**< What the format supports.                */
  const ra8_io_fsfmt_ops_t* ops;  /**< Runtime dispatch table.                  */
} ra8_io_fsfmt_t;

/**
 * @brief Reset the registry and register the built-in FAT + exFAT formats.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Registry holds the two built-ins.
 *
 * @pre None.
 * @pre No probe is in flight.
 * @post Only the built-in formats are registered.
 * @post Foreign formats must be re-registered after this call.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_io_fsfmt_init(void);

/**
 * @brief Register a filesystem format (the foreign-format seam).
 *
 * @param[in] fmt Format descriptor (a const instance that out-lives the registry).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Format registered.
 * @retval k_ra8_err_null_ptr    A mandatory descriptor or operation was NULL.
 * @retval k_ra8_err_invalid_arg A capability claimed an absent optional op.
 * @retval k_ra8_err_no_mem      The registry is full.
 *
 * @pre `fmt` and its members out-live the registry.
 * @pre The registry has a free slot.
 * @post `fmt` participates in subsequent probes (lowest priority -- last).
 * @post On any non-ok return the registry is unchanged.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_io_fsfmt_register(const ra8_io_fsfmt_t* fmt);

/**
 * @brief Return the built-in descriptor serving one native ra8_fs type.
 *
 * @details FAT12/16/32 share the FAT descriptor; exFAT has its own descriptor.
 *          This lookup does not depend on registry initialization and is used
 *          to adapt an already-mounted legacy ::ra8_fs_mount_t into the same
 *          operations-dispatched VFS path as an automatically probed mount.
 *
 * @param[in]  type Native filesystem type.
 * @param[out] out  Matching built-in descriptor.
 *
 * @retval k_ra8_ok              Descriptor returned.
 * @retval k_ra8_err_null_ptr    `out` was NULL.
 * @retval k_ra8_err_invalid_arg `type` was unknown or foreign.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_io_fsfmt_get_builtin(ra8_fs_type_t type, const ra8_io_fsfmt_t** out);

/**
 * @brief Detect the format of a volume by probing registered formats in order.
 *
 * @param[in]  backend Block-device backend to inspect.
 * @param[out] out     Set to the first matching format.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok            A format matched; `*out` is set.
 * @retval k_ra8_err_null_ptr  `backend` or `out` was NULL.
 * @retval k_ra8_err_not_found No registered format claimed the volume.
 *
 * @pre At least the built-ins are registered (call ::ra8_io_fsfmt_init first).
 * @pre `out` is writable.
 * @post On success `*out` points at a registered format.
 * @post On any non-ok return `*out` is untouched.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_io_fsfmt_probe(const ra8_fs_backend_t* backend,
                                           const ra8_io_fsfmt_t**  out);

#ifdef __cplusplus
}
#endif
