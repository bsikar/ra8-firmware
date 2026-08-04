/**
 * @file ra8_io_vfs.h
 * @brief ra8_io virtual filesystem -- mount many volumes, address them by name.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * The VFS is a small mount table over `ra8_fs`: register a mounted volume under a
 * short name (e.g. `"sd"`, `"ospi"`, `"ram"`), then reach files through a
 * `"name:/path"` string. This is the "address storage by name, not by
 * peripheral" layer -- several filesystems on different media coexist and are
 * told apart by their mount name. Each registered mount is an `ra8_fs_mount_t`
 * the caller obtained by mounting an `ra8_fs_backend_t` (typically one produced
 * by ::ra8_io_blockdev_as_fs_backend over any block device).
 *
 * Path-resolving operations (open / unlink / rename / stat / listdir / mkdir)
 * live here; once a file is open the caller drives it with the existing
 * `ra8_fs_read` / `ra8_fs_write` / `ra8_fs_seek` / `ra8_fs_close` file operations.
 *
 * @code
 * ra8_fs_backend_t be = {};
 * (void)ra8_io_blockdev_as_fs_backend(&sd_bd, &be);
 * ra8_fs_mount_t* m = nullptr;
 * (void)ra8_fs_mount(&be, &m);
 * (void)ra8_io_vfs_mount("sd", m);
 * ra8_fs_file_t* f = nullptr;
 * (void)ra8_io_vfs_open("sd:/book.epb", k_ra8_fs_mode_read, &f);
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

/**
 * @enum ra8_io_vfs_limits_t
 * @brief Static limits for the VFS mount table.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_io_vfs_max_mounts = 4,  /**< Concurrent named mounts.    */
  k_ra8_io_vfs_name_max   = 16, /**< Mount name length incl NUL. */
} ra8_io_vfs_limits_t;

/**
 * @struct ra8_io_vfs_stat_t
 * @brief Metadata returned by ::ra8_io_vfs_stat.
 *
 * @details `exists` is false (with ::k_ra8_ok) when the path resolves to a mount
 *          but the file is absent, so callers can probe without treating a miss
 *          as an error.
 *
 *          `attr` and `is_directory` are read out of the on-disk directory
 *          entry, so a folder reports as a folder. Anything built over this --
 *          a file browser, a USB/MTP gateway, an importer -- can tell the two
 *          apart, which it could not when both fields were hardcoded.
 *
 * @invariant `is_directory` equals `(attr & k_ra8_fs_attr_directory) != 0`.
 * @invariant `size_bytes` is 0 whenever `is_directory` is true.
 *
 * @since 0.1.0
 */
typedef struct {
  uint32_t size_bytes;   /**< File size in bytes (0 for directories). */
  uint8_t  attr;         /**< The entry's own FAT attribute byte.     */
  bool     is_directory; /**< true => path names a directory.         */
  bool     exists;       /**< true => the path resolves to an entry.  */
} ra8_io_vfs_stat_t;

/**
 * @brief Reset the VFS mount table to empty.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok All mount slots released.
 *
 * @pre None.
 * @pre No file opened through the VFS is still in use.
 * @post Every mount slot is free.
 * @post Subsequent path operations fail until a volume is mounted.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_io_vfs_init(void);

/**
 * @brief Register a mounted volume under a name.
 *
 * @param[in] name  Mount name (1..15 chars, no `:` or `/`).
 * @param[in] mount Live `ra8_fs_mount_t` (must out-live the registration).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Volume registered.
 * @retval k_ra8_err_null_ptr     `name` or `mount` was NULL.
 * @retval k_ra8_err_invalid_arg  Name empty or too long.
 * @retval k_ra8_err_exists       The name is already mounted.
 * @retval k_ra8_err_no_mem       The mount table is full.
 *
 * @pre `mount` was returned by `ra8_fs_mount` and stays alive while registered.
 * @pre `name` contains no `:` or `/`.
 * @post On success `"name:/..."` paths resolve to `mount`.
 * @post On any non-ok return the table is unchanged.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_io_vfs_mount(const char* name, ra8_fs_mount_t* mount);

/**
 * @brief Remove a named mount from the table.
 *
 * @param[in] name Mount name to release.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok            Mount released.
 * @retval k_ra8_err_null_ptr  `name` was NULL.
 * @retval k_ra8_err_not_found The name was not mounted.
 *
 * @pre No file opened on this mount is still in use.
 * @pre `name` is non-NULL.
 * @post `"name:/..."` paths no longer resolve.
 * @post The underlying `ra8_fs_mount_t` is left untouched (caller owns it).
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_io_vfs_unmount(const char* name);

/**
 * @brief Open a file by `"name:/path"`.
 *
 * @param[in]  path     `"name:/path"` string.
 * @param[in]  mode     Open mode (read / write / append).
 * @param[out] out_file Populated file handle on success.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              File opened.
 * @retval k_ra8_err_null_ptr    `path` or `out_file` was NULL.
 * @retval k_ra8_err_invalid_arg `path` has no `name:` prefix.
 * @retval k_ra8_err_not_found   The mount name or the file is absent.
 * @retval k_ra8_err_*           Propagated from `ra8_fs_open`.
 *
 * @pre The named volume is mounted.
 * @pre `out_file` is writable.
 * @post On success drive the handle with `ra8_fs_read` / `ra8_fs_write` / etc.
 * @post On any non-ok return `out_file` is untouched.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_io_vfs_open(const char* path, ra8_fs_mode_t mode, ra8_fs_file_t** out_file);

/**
 * @brief Delete a file by `"name:/path"`.
 *
 * @param[in] path `"name:/path"` string.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              File unlinked.
 * @retval k_ra8_err_null_ptr    `path` was NULL.
 * @retval k_ra8_err_invalid_arg `path` has no `name:` prefix.
 * @retval k_ra8_err_not_found   The mount name or the file is absent.
 *
 * @pre The named volume is mounted and the file is closed.
 * @pre `path` is non-NULL.
 * @post On success the file no longer resolves.
 * @post On any non-ok return the volume is unchanged.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_io_vfs_unlink(const char* path);

/**
 * @brief Rename a file within one mount.
 *
 * @param[in] old_path `"name:/old"` string.
 * @param[in] new_path `"name:/new"` string (same mount as `old_path`).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               File renamed.
 * @retval k_ra8_err_null_ptr     Either argument was NULL.
 * @retval k_ra8_err_invalid_arg  Missing prefix, or the two mounts differ.
 * @retval k_ra8_err_not_found    The mount or `old_path` is absent.
 * @retval k_ra8_err_exists       `new_path` already exists.
 *
 * @pre Both paths name the same mounted volume and the file is closed.
 * @pre Both arguments are non-NULL.
 * @post On success `new_path` resolves to the prior file data.
 * @post On any non-ok return the volume is unchanged.
 *
 * @note Not thread-safe. Cross-mount moves are not supported.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_io_vfs_rename(const char* old_path, const char* new_path);

/**
 * @brief Query metadata for `"name:/path"`.
 *
 * @details Delegates to ::ra8_fs_stat(), which reads the directory entry
 *          without opening it. A directory therefore reports
 *          `is_directory == true` and `size_bytes == 0`, and `attr` is the
 *          entry's real attribute byte -- read-only, hidden and system all
 *          survive the trip. A missing name is ::k_ra8_ok with
 *          `exists == false`, not an error: absence is an answer.
 *
 *          `"name:/"` names the volume root, which always exists and is
 *          always a directory.
 *
 * @param[in]  path `"name:/path"` string.
 * @param[out] out  Metadata snapshot (`exists` reflects presence).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Metadata resolved (`exists` may be false).
 * @retval k_ra8_err_null_ptr    `path` or `out` was NULL.
 * @retval k_ra8_err_invalid_arg `path` has no `name:` prefix, or a component
 *                               is not a valid 8.3 name.
 * @retval k_ra8_err_not_found   The mount name is absent.
 *
 * @pre The named volume is mounted.
 * @pre `out` is writable.
 * @post On success `*out` describes the entry (or `exists == false`).
 * @post No volume state is mutated and no file handle is consumed.
 *
 * @note Not thread-safe unless a lock is installed (see ::ra8_fs_set_lock()).
 *
 * @see ra8_fs_stat()  The primitive this reports.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_io_vfs_stat(const char* path, ra8_io_vfs_stat_t* out);

/**
 * @brief Enumerate a directory named `"name:/path"`.
 *
 * @param[in] path `"name:/path"` directory string (`"name:/"` for the root).
 * @param[in] cb   Per-entry callback (non-NULL).
 * @param[in] ctx  Cookie forwarded to `cb`.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Enumeration complete.
 * @retval k_ra8_err_null_ptr    `path` or `cb` was NULL.
 * @retval k_ra8_err_invalid_arg `path` has no `name:` prefix.
 * @retval k_ra8_err_not_found   The mount name is absent.
 * @retval k_ra8_err_*           Propagated from `ra8_fs_listdir`.
 *
 * @pre The named volume is mounted.
 * @pre `cb` is non-NULL.
 * @post `cb` was invoked once per visible entry.
 * @post No volume state is mutated.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_io_vfs_listdir(const char* path, ra8_fs_listdir_cb_t cb, void* ctx);

/**
 * @brief Create a directory named `"name:/path"`.
 *
 * @details Routes to the named mount and delegates to `ra8_fs_mkdir`, which
 *          creates the final path component as a new FAT directory (nested paths
 *          are supported when each intermediate component already exists). exFAT
 *          volumes return ::k_ra8_err_not_supported.
 *
 * @param[in] path `"name:/path"` directory string.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Directory created.
 * @retval k_ra8_err_null_ptr      `path` was NULL.
 * @retval k_ra8_err_invalid_arg   `path` has no `name:` prefix, or a bad leaf.
 * @retval k_ra8_err_not_found     The mount name or a path component is absent.
 * @retval k_ra8_err_exists        The directory already exists.
 * @retval k_ra8_err_not_supported The volume is exFAT.
 *
 * @pre The named volume is mounted.
 * @pre `path` is non-NULL.
 * @post On success the directory resolves.
 * @post On any non-ok return the volume is unchanged.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_io_vfs_mkdir(const char* path);

/**
 * @brief Remove the empty directory named `"name:/path"`.
 *
 * @details Routes to the named mount and delegates to `ra8_fs_rmdir`, which
 *          removes the final path component when it is an existing directory
 *          holding nothing but its own "." and ".." links. The volume root and
 *          plain files are refused; exFAT volumes return
 *          ::k_ra8_err_not_supported, matching `ra8_io_vfs_mkdir()`.
 *
 * @param[in] path `"name:/path"` directory string.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                 Directory removed.
 * @retval k_ra8_err_null_ptr       `path` was NULL.
 * @retval k_ra8_err_invalid_arg    `path` has no `name:` prefix, names the
 *                                  root, or names a file.
 * @retval k_ra8_err_not_found      The mount name or a path component is absent.
 * @retval k_ra8_err_not_empty      The directory still holds entries.
 * @retval k_ra8_err_not_supported  The volume is exFAT.
 *
 * @pre The named volume is mounted.
 * @pre `path` is non-NULL.
 * @post On success the directory no longer resolves.
 * @post On any non-ok return the volume is unchanged.
 *
 * @note Not thread-safe.
 *
 * @see ra8_io_vfs_mkdir()   Creates the directory this removes.
 * @see ra8_io_vfs_unlink()  Removes a file instead.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_io_vfs_rmdir(const char* path);

#ifdef __cplusplus
}
#endif
