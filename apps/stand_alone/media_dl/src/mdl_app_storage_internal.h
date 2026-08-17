/**
 * @file mdl_app_storage_internal.h
 * @brief Private portable filesystem operations shared by CLI application
 * modes.
 * @details Keeps application policy over the existing ::mdl_storage_t and
 *          `fw_fs` contracts testable without introducing another file handle,
 *          path facade, allocator, or host-library dependency.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "mdl_storage.h"
#include "ra8_attributes.h"
#include "ra8_err.h"

/**
 * @brief Ensure one canonical portable path names a real directory.
 * @param[in,out] storage Initialized exclusive filesystem binding.
 * @param[in] path Canonical absolute directory path.
 * @return Canonical namespace status.
 * @retval k_ra8_ok The directory already existed or was created and rechecked.
 * @retval k_ra8_err_access_denied The path names a symbolic link.
 * @retval k_ra8_err_invalid_arg The path names another node type.
 * @retval other Stat or mkdir failure propagated from the injected backend.
 * @pre @p path is NUL-terminated and confined by @p storage.
 * @post Success proves the final node is a directory.
 * @post Failure never follows a symbolic link or removes an existing node.
 * @note Not thread-safe against concurrent mutation of the same namespace.
 * @since 0.1.0

 * @details Uses canonical paths and caller-owned filesystem workspace only.
 *          Special nodes are rejected before any namespace mutation.
 * @pre Every required pointer is non-null and remains valid for the call.
 */
RA8_PRIV ra8_err_t priv_mdl_app_storage_ensure_directory(mdl_storage_t* storage, const char* path);

/**
 * @brief Remove one regular file while refusing symbolic or special nodes.
 * @param[in,out] storage Initialized exclusive filesystem binding.
 * @param[in] path Canonical absolute file path.
 * @param[out] out_removed Whether an existing regular file was removed.
 * @return Canonical namespace status; an absent path succeeds.
 * @retval k_ra8_ok The path was absent or its regular file was removed.
 * @retval k_ra8_err_access_denied The path names a symbolic link.
 * @retval k_ra8_err_invalid_arg The path names a directory or special node.
 * @retval other Stat or unlink failure propagated from the injected backend.
 * @pre @p path is NUL-terminated and confined by @p storage.
 * @post Success leaves no node at @p path and initializes @p out_removed.
 * @post Failure never follows or removes a symbolic link.
 * @note A final-component replacement race remains harmless because unlink
 *       removes the named directory entry rather than following it.
 * @since 0.1.0

 * @details Uses canonical paths and caller-owned filesystem workspace only.
 *          Special nodes are rejected before any namespace mutation.
 * @pre Every required pointer is non-null and remains valid for the call.
 */
RA8_PRIV ra8_err_t priv_mdl_app_storage_unlink_regular(mdl_storage_t* storage,
                                                       const char*    path,
                                                       bool*          out_removed);

/**
 * @brief Render and create one starter site descriptor transactionally.
 * @details Formats the bounded fixed template in the storage I/O scratch, then
 *          uses a create-new transaction whose exact size and FNV identity are
 *          independently reread before publication. Existing descriptors are
 *          never replaced on any backend.
 * @param[in,out] storage Initialized exclusive filesystem binding.
 * @param[in] destination Canonical absent `.conf` path.
 * @param[in] url Original absolute site URL.
 * @param[in] host Complete normalized host.
 * @param[in] name Derived display name.
 * @return Canonical render, transaction, validation, or publication status.
 * @retval k_ra8_ok The independently validated template was published once.
 * @retval k_ra8_err_exists The destination already exists unchanged.
 * @retval k_ra8_err_invalid_size The rendered template exceeded bounded
 * scratch.
 * @retval other Storage failure propagated without publishing partial bytes.
 * @pre All strings are non-NULL, NUL-terminated, and remain live for the call.
 * @pre @p destination is confined by the bound filesystem.
 * @post Success exposes exactly the generated descriptor bytes.
 * @post Failure preserves an existing destination byte-for-byte.
 * @note Not thread-safe for shared storage or concurrent same-path creation.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_app_storage_publish_site(mdl_storage_t* storage,
                                                     const char*    destination,
                                                     const char*    url,
                                                     const char*    host,
                                                     const char*    name);
