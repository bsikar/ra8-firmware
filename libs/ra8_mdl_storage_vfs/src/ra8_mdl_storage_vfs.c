/**
 * @file ra8_mdl_storage_vfs.c
 * @brief Transactional media-download storage over named RA8 VFS mounts
 *
 * @details Implements the downloader transaction callbacks with bounded
 * validation, a caller-reserved sibling staging file, and one final VFS
 * no-replace rename. All state remains in the caller-owned adapter context.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_mdl_storage_vfs.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_io_vfs.h"

/** @brief Module-local path facts proven before any filesystem mutation. */
typedef struct {
  uint16_t length;     /**< Bytes before NUL.          */
  uint16_t last_slash; /**< Final component separator. */
} path_facts_t;

/**
 * @brief Return whether one component is exactly @c "." or @c "..".
 * @details Compares the caller-bounded component without requiring a terminator.
 * @param[in] text First component byte.
 * @param[in] len Number of bytes in the component.
 * @return Whether the component is a traversal marker.
 * @retval true The component is exactly @c "." or @c "..".
 * @retval false The component is any other byte sequence.
 * @pre @p text spans at least @p len readable bytes.
 * @pre @p len came from the bounded path parser.
 * @post No input byte is changed.
 * @post No byte at or after @p len is read.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_is_dot_component(const char* text, uint16_t len)
{
  if (len == 1U) {
    return text[0] == '.';
  }
  if (len == 2U) {
    if (text[0] == '.') {
      return text[1] == '.';
    }
  }
  return false;
}

/**
 * @brief Measure one string without reading past a fixed capacity.
 * @details Searches only the declared range and fails closed if no NUL appears.
 * @param[in] text Candidate bounded string.
 * @param[in] cap Maximum bytes that may be inspected.
 * @param[out] out_len Receives bytes before the terminator.
 * @return Canonical validation status.
 * @retval k_ra8_ok A terminator was found and length returned.
 * @retval k_ra8_err_null_ptr A required pointer was null.
 * @retval k_ra8_err_invalid_size No terminator occurred within @p cap.
 * @pre @p text is null or addresses @p cap readable bytes.
 * @pre @p out_len is null or writable.
 * @post Success initializes @p out_len with a value below @p cap.
 * @post Failure does not mutate caller storage.
 * @note Pure and thread-safe for immutable input.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_bounded_length(const char* text, uint16_t cap, uint16_t* out_len)
{
  if (text == nullptr || out_len == nullptr) {
    return k_ra8_err_null_ptr;
  }
  for (uint16_t i = 0U; i < cap; ++i) {
    if (text[i] == '\0') {
      *out_len = i;
      return k_ra8_ok;
    }
  }
  return k_ra8_err_invalid_size;
}

/**
 * @brief Validate the mount prefix and locate its colon.
 * @details Enforces the named-VFS grammar before any device lookup or mutation.
 * @param[in] path Canonical-path candidate.
 * @param[in] len Proven bytes before NUL.
 * @param[out] out_colon Receives the mount separator offset.
 * @return Canonical path-validation status.
 * @retval k_ra8_ok A non-empty bounded mount prefix was found.
 * @retval k_ra8_err_invalid_arg Prefix grammar or length was invalid.
 * @pre @p path spans @p len plus its NUL terminator.
 * @pre @p out_colon is non-null and writable.
 * @post Success returns a colon offset below @p len.
 * @post Failure performs no filesystem operation.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_mount_prefix(const char* path, uint16_t len, uint16_t* out_colon)
{
  if (len < 4U) {
    return k_ra8_err_invalid_arg;
  }
  for (uint16_t i = 0U; i < (uint16_t)k_ra8_io_vfs_name_max; ++i) {
    if (i >= len) {
      return k_ra8_err_invalid_arg;
    }
    if (path[i] == ':') {
      if (i == 0U) {
        return k_ra8_err_invalid_arg;
      }
      *out_colon = i;
      return k_ra8_ok;
    }
    if ((path[i] == '/') || (path[i] == '\\') || ((uint8_t)path[i] < 0x20U)) {
      return k_ra8_err_invalid_arg;
    }
  }
  return k_ra8_err_invalid_arg;
}

/**
 * @brief Validate canonical non-traversing components after `mount:/`.
 * @details Rejects empty, dot, control, alternate-separator, and colon components.
 * @param[in] path Full named-VFS path.
 * @param[in] len Proven bytes before NUL.
 * @param[in] colon Proven mount-separator offset.
 * @param[out] out_slash Receives the final component separator.
 * @return Canonical validation status.
 * @retval k_ra8_ok Every component is canonical and non-empty.
 * @retval k_ra8_err_invalid_arg Path grammar or a component was invalid.
 * @pre @p path spans @p len plus its NUL terminator.
 * @pre @p colon names a byte within @p path and @p out_slash is writable.
 * @post Success initializes @p out_slash before the final leaf.
 * @post Failure performs no filesystem operation.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_path_components(const char* path, uint16_t len, uint16_t colon, uint16_t* out_slash)
{
  if (path[colon + 1U] != '/') {
    return k_ra8_err_invalid_arg;
  }
  uint16_t start = (uint16_t)(colon + 2U);
  uint16_t slash = (uint16_t)(colon + 1U);
  for (uint16_t i = start; i <= len; ++i) {
    const char c = path[i];
    if ((c == '/') || (c == '\0')) {
      const uint16_t component_len = (uint16_t)(i - start);
      if (component_len == 0U) {
        return k_ra8_err_invalid_arg;
      }
      if (internal_is_dot_component(&path[start], component_len)) {
        return k_ra8_err_invalid_arg;
      }
      if (c == '/') {
        slash = i;
        start = (uint16_t)(i + 1U);
      }
      continue;
    }
    if ((c == ':') || (c == '\\') || ((uint8_t)c < 0x20U)) {
      return k_ra8_err_invalid_arg;
    }
  }
  *out_slash = slash;
  return k_ra8_ok;
}

/**
 * @brief Prove a destination is a bounded canonical named-VFS file path.
 * @details Composes bounded-length, mount-prefix, and component validation.
 * @param[in] path Destination path candidate.
 * @param[out] out Receives proven length and final separator facts.
 * @return Canonical validation status.
 * @retval k_ra8_ok The path is bounded and canonical.
 * @retval k_ra8_err_null_ptr A required pointer was null.
 * @retval other First bounded-path validation error.
 * @pre @p path is null or addresses the configured path capacity.
 * @pre @p out is non-null and writable.
 * @post Success fully initializes @p out.
 * @post Failure performs no filesystem operation.
 * @note Pure and thread-safe for immutable input.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_path_facts(const char* path, path_facts_t* out)
{
  uint16_t  len = 0U;
  ra8_err_t err = internal_bounded_length(path, k_ra8_mdl_storage_vfs_path_capacity, &len);
  if (err != k_ra8_ok) {
    return err;
  }
  uint16_t colon = 0U;
  err            = internal_mount_prefix(path, len, &colon);
  if (err != k_ra8_ok) {
    return err;
  }
  uint16_t slash = 0U;
  err            = internal_path_components(path, len, colon, &slash);
  if (err != k_ra8_ok) {
    return err;
  }
  out->length     = len;
  out->last_slash = slash;
  return k_ra8_ok;
}

/**
 * @brief Validate the caller-reserved simple staging leaf.
 * @details Requires a bounded non-dot leaf with no separator, colon, or control.
 * @param[in] leaf Candidate private filename.
 * @param[out] out_len Receives its bounded byte length.
 * @return Canonical validation status.
 * @retval k_ra8_ok The leaf is safe for same-directory staging.
 * @retval k_ra8_err_null_ptr A required pointer was null.
 * @retval k_ra8_err_invalid_arg Leaf grammar was invalid.
 * @retval k_ra8_err_invalid_size No NUL fit within the fixed capacity.
 * @pre @p leaf is null or addresses the configured leaf capacity.
 * @pre @p out_len is non-null and writable.
 * @post Success initializes @p out_len to a non-zero bounded value.
 * @post Failure performs no filesystem operation.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_stage_leaf_check(const char* leaf, uint16_t* out_len)
{
  ra8_err_t err = internal_bounded_length(leaf, k_ra8_mdl_storage_vfs_stage_leaf_capacity, out_len);
  if (err != k_ra8_ok) {
    return err;
  }
  if (*out_len == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (internal_is_dot_component(leaf, *out_len)) {
    return k_ra8_err_invalid_arg;
  }
  for (uint16_t i = 0U; i < *out_len; ++i) {
    const char c = leaf[i];
    if ((c == '/') || (c == '\\') || (c == ':') || ((uint8_t)c < 0x20U)) {
      return k_ra8_err_invalid_arg;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Copy a bounded NUL-terminated byte string.
 * @details Copies exactly @p len payload bytes and writes one terminator.
 * @param[out] dst Destination spanning at least `len + 1` bytes.
 * @param[in] src Source spanning at least @p len bytes.
 * @param[in] len Payload byte count, excluding NUL.
 * @pre Source and destination ranges are valid and non-overlapping.
 * @pre Destination capacity exceeds @p len.
 * @post Destination bytes `[0,len)` equal source bytes `[0,len)`.
 * @post `dst[len]` is NUL and no later byte is changed.
 * @note Pure apart from caller-owned destination; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_copy(char* dst, const char* src, uint16_t len)
{
  for (uint16_t i = 0U; i < len; ++i) {
    dst[i] = src[i];
  }
  dst[len] = '\0';
}

/**
 * @brief Compare a destination leaf with the reserved staging leaf.
 * @details Compares through the first NUL but never beyond fixed leaf capacity.
 * @param[in] destination_leaf Final path's last component.
 * @param[in] stage_leaf Validated reserved staging component.
 * @return Whether the complete bounded spellings are equal.
 * @retval true Both leaves match through their shared terminator.
 * @retval false A byte differs or neither terminates within capacity.
 * @pre Both inputs span the fixed leaf capacity or terminate earlier.
 * @pre @p stage_leaf passed ::internal_stage_leaf_check.
 * @post Neither input is modified.
 * @post At most the fixed leaf capacity is inspected.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_leaf_equals(const char* destination_leaf, const char* stage_leaf)
{
  for (uint16_t i = 0U; i < k_ra8_mdl_storage_vfs_stage_leaf_capacity; ++i) {
    if (destination_leaf[i] != stage_leaf[i]) {
      return false;
    }
    if (destination_leaf[i] == '\0') {
      return true;
    }
  }
  return false;
}

/**
 * @brief Build the caller-owned sibling staging path without truncation.
 * @details Reuses the final path parent and appends the reserved private leaf.
 * @param[in,out] ctx Adapter context holding destination and output path buffers.
 * @param[in] facts Proven destination path facts.
 * @param[in] stage_len Proven reserved-leaf byte length.
 * @return Canonical construction status.
 * @retval k_ra8_ok A complete same-parent staging path was built.
 * @retval k_ra8_err_invalid_arg Final and staging leaves would be identical.
 * @retval k_ra8_err_invalid_size Fixed path capacity is insufficient.
 * @pre All pointers are non-null and @p facts came from ::internal_path_facts.
 * @pre `ctx->stage_leaf` passed ::internal_stage_leaf_check with @p stage_len.
 * @post Success NUL-terminates `ctx->staging_path` without truncation.
 * @post Failure performs no filesystem operation.
 * @note Not thread-safe through @p ctx.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_build_stage(ra8_mdl_storage_vfs_t* ctx, const path_facts_t* facts, uint16_t stage_len)
{
  const uint32_t prefix = (uint32_t)facts->last_slash + 1U;
  const uint32_t needed = prefix + (uint32_t)stage_len + 1U;
  if (needed > (uint32_t)k_ra8_mdl_storage_vfs_path_capacity) {
    return k_ra8_err_invalid_size;
  }
  const char* final_leaf = &ctx->destination[facts->last_slash + 1U];
  if (internal_leaf_equals(final_leaf, ctx->stage_leaf)) {
    return k_ra8_err_invalid_arg;
  }
  for (uint32_t i = 0U; i < prefix; ++i) {
    ctx->staging_path[i] = ctx->destination[i];
  }
  for (uint16_t i = 0U; i < stage_len; ++i) {
    ctx->staging_path[prefix + i] = ctx->stage_leaf[i];
  }
  ctx->staging_path[prefix + stage_len] = '\0';
  return k_ra8_ok;
}

/**
 * @brief Require the destination parent to exist and be a directory.
 * @details Temporarily terminates the owned destination at its parent boundary.
 * @param[in,out] ctx Adapter holding the canonical destination buffer.
 * @param[in] last_slash Proven final separator offset.
 * @return Canonical VFS or validation status.
 * @retval k_ra8_ok Parent exists as a directory.
 * @retval k_ra8_err_not_found Parent does not exist.
 * @retval k_ra8_err_invalid_arg Parent exists as a non-directory.
 * @retval other Underlying VFS stat failed.
 * @pre @p ctx is non-null and destination passed ::internal_path_facts.
 * @pre @p last_slash is the proven final separator.
 * @post The complete destination spelling is restored on every path.
 * @post No filesystem object is mutated.
 * @note Not thread-safe through @p ctx during the temporary terminator change.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_parent_check(ra8_mdl_storage_vfs_t* ctx, uint16_t last_slash)
{
  const bool     root_parent = ctx->destination[last_slash - 1U] == ':';
  const uint16_t cut         = root_parent ? (uint16_t)(last_slash + 1U) : last_slash;
  const char     saved       = ctx->destination[cut];
  ctx->destination[cut]      = '\0';
  ra8_io_vfs_stat_t stat     = {};
  const ra8_err_t   err      = ra8_io_vfs_stat(ctx->destination, &stat);
  ctx->destination[cut]      = saved;
  if (err != k_ra8_ok) {
    return err;
  }
  if (!stat.exists) {
    return k_ra8_err_not_found;
  }
  if (!stat.is_directory) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/**
 * @brief Refuse an existing final, distinguishing directories as bad paths.
 * @details Implements create-new publication policy with a non-mutating stat.
 * @param[in] destination Canonical final path.
 * @return Canonical policy or VFS status.
 * @retval k_ra8_ok Destination is absent.
 * @retval k_ra8_err_exists A regular destination already exists.
 * @retval k_ra8_err_invalid_arg Destination names a directory.
 * @retval other Underlying VFS stat failed.
 * @pre @p destination is non-null and canonical.
 * @pre The selected mount remains registered for this call.
 * @post No filesystem object is mutated.
 * @post Success proves absence only for this serialized observation.
 * @note Caller repeats the check immediately before the rename.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_final_absent(const char* destination)
{
  ra8_io_vfs_stat_t stat = {};
  const ra8_err_t   err  = ra8_io_vfs_stat(destination, &stat);
  if (err != k_ra8_ok) {
    return err;
  }
  if (!stat.exists) {
    return k_ra8_ok;
  }
  if (stat.is_directory) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_err_exists;
}

/**
 * @brief Remove a stale owned regular stage, but never a directory.
 * @details Cleans only the adapter-reserved sibling name after type validation.
 * @param[in] staging_path Canonical private sibling path.
 * @return Canonical cleanup status.
 * @retval k_ra8_ok Stage was absent or was removed.
 * @retval k_ra8_err_invalid_state Reserved path names a directory.
 * @retval other Underlying VFS stat or unlink failed.
 * @pre @p staging_path is non-null and was built by ::internal_build_stage.
 * @pre The caller owns the reserved leaf policy in this parent.
 * @post Success leaves no filesystem object at @p staging_path.
 * @post A directory at @p staging_path is never removed.
 * @note Not thread-safe with another owner of the same reserved leaf.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_remove_stale_stage(const char* staging_path)
{
  ra8_io_vfs_stat_t stat = {};
  const ra8_err_t   err  = ra8_io_vfs_stat(staging_path, &stat);
  if (err != k_ra8_ok) {
    return err;
  }
  if (!stat.exists) {
    return k_ra8_ok;
  }
  if (stat.is_directory) {
    return k_ra8_err_invalid_state;
  }
  return ra8_io_vfs_unlink(staging_path);
}

/**
 * @brief Close the writer exactly once and retain staged cleanup state.
 * @details Attempts a supported sync before close and clears the live handle first.
 * @param[in,out] ctx Active adapter transaction context.
 * @return First canonical sync or close status.
 * @retval k_ra8_ok No writer was open or sync and close succeeded.
 * @retval other First supported sync failure, otherwise close failure.
 * @pre @p ctx is non-null and owns at most one VFS file.
 * @pre Any non-null file belongs to the current private staging path.
 * @post `ctx->file` is null on every path.
 * @post A formerly open transaction is left in the staged cleanup state.
 * @note A backend that reports sync unsupported is accepted explicitly.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_close_writer(ra8_mdl_storage_vfs_t* ctx)
{
  if (ctx->file == nullptr) {
    return k_ra8_ok;
  }
  ra8_io_vfs_file_t* const file = ctx->file;
  ctx->file                     = nullptr;
  ctx->state                    = k_ra8_mdl_storage_vfs_staged;
  ra8_err_t sync_err            = ra8_io_vfs_file_sync(file);
  if (sync_err == k_ra8_err_not_supported) {
    sync_err = k_ra8_ok;
  }
  const ra8_err_t close_err = ra8_io_vfs_file_close(file);
  return (sync_err == k_ra8_ok) ? close_err : sync_err;
}

/**
 * @brief Coordinator callback: create the private sibling transaction file.
 * @details Validates paths and parent policy before removing an owned stale stage
 * and opening a fresh bounded writer on the selected VFS mount.
 * @param[in,out] opaque Bound ::ra8_mdl_storage_vfs_t context.
 * @param[in] destination Canonical final named-VFS path candidate.
 * @return Canonical transaction status.
 * @retval k_ra8_ok A private writer is open in the writing state.
 * @retval k_ra8_err_null_ptr A required pointer was null.
 * @retval k_ra8_err_invalid_state A transaction is already active.
 * @retval other First path, policy, cleanup, or VFS-open error.
 * @pre @p opaque points to an initialized adapter context.
 * @pre The reserved staging leaf is exclusively owned by that adapter policy.
 * @post Success resets byte count and owns exactly one open private writer.
 * @post Failure never creates or replaces the final destination.
 * @note Not thread-safe through one adapter instance.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_begin(void* opaque, const char* destination)
{
  if (opaque == nullptr || destination == nullptr) {
    return k_ra8_err_null_ptr;
  }
  ra8_mdl_storage_vfs_t* const ctx = opaque;
  if ((ctx->state != k_ra8_mdl_storage_vfs_idle) &&
      (ctx->state != k_ra8_mdl_storage_vfs_committed)) {
    return k_ra8_err_invalid_state;
  }
  ctx->state         = k_ra8_mdl_storage_vfs_idle;
  path_facts_t facts = {};
  ra8_err_t    err   = internal_path_facts(destination, &facts);
  if (err != k_ra8_ok) {
    return err;
  }
  internal_copy(ctx->destination, destination, facts.length);
  uint16_t stage_len = 0U;
  err                = internal_stage_leaf_check(ctx->stage_leaf, &stage_len);
  if (err == k_ra8_ok) {
    err = internal_build_stage(ctx, &facts, stage_len);
  }
  if (err == k_ra8_ok) {
    err = internal_parent_check(ctx, facts.last_slash);
  }
  if (err == k_ra8_ok) {
    err = internal_final_absent(ctx->destination);
  }
  if (err == k_ra8_ok) {
    err = internal_remove_stale_stage(ctx->staging_path);
  }
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_io_vfs_file_open(ctx->staging_path, k_ra8_fs_mode_write, &ctx->file);
  if (err == k_ra8_ok) {
    ctx->bytes_written = 0U;
    ctx->state         = k_ra8_mdl_storage_vfs_writing;
  }
  return err;
}

/**
 * @brief Coordinator callback: append one all-or-error bounded chunk.
 * @details Delegates one bounded VFS write and advances accounting only on success.
 * @param[in,out] opaque Bound ::ra8_mdl_storage_vfs_t context.
 * @param[in] data Source chunk.
 * @param[in] len Chunk byte count.
 * @param[out] written Receives either zero or the complete @p len.
 * @return Canonical transaction or VFS status.
 * @retval k_ra8_ok The full chunk was accepted and accounted.
 * @retval k_ra8_err_null_ptr A required pointer was null.
 * @retval k_ra8_err_invalid_state No private writer is active.
 * @retval k_ra8_err_invalid_size Total-byte accounting would overflow.
 * @retval other Underlying VFS write failed.
 * @pre @p opaque and @p written are non-null; @p data spans @p len bytes.
 * @pre Adapter state is writing and its file belongs to the current stage.
 * @post Success sets @p written to @p len and advances bytes exactly once.
 * @post Failure sets @p written to zero and does not advance byte accounting.
 * @note The VFS write contract is all-or-error for this bounded call.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_write(void* opaque, const uint8_t* data, uint16_t len, uint16_t* written)
{
  if (opaque == nullptr || written == nullptr) {
    return k_ra8_err_null_ptr;
  }
  *written = 0U;
  if (data == nullptr) {
    return k_ra8_err_null_ptr;
  }
  ra8_mdl_storage_vfs_t* const ctx = opaque;
  if (ctx->state != k_ra8_mdl_storage_vfs_writing) {
    return k_ra8_err_invalid_state;
  }
  if (ctx->file == nullptr) {
    return k_ra8_err_invalid_state;
  }
  if (ctx->bytes_written > (UINT64_MAX - (uint64_t)len)) {
    return k_ra8_err_invalid_size;
  }
  const ra8_err_t err = ra8_io_vfs_file_write(ctx->file, data, (uint32_t)len);
  if (err == k_ra8_ok) {
    ctx->bytes_written += (uint64_t)len;
    *written = len;
  }
  return err;
}

/**
 * @brief Check the closed stage is still a regular file of the exact size.
 * @details Uses VFS metadata to detect disappearance, type substitution, or size drift.
 * @param[in] ctx Adapter context naming the closed private stage.
 * @param[in] expected Required byte length.
 * @return Canonical validation or VFS status.
 * @retval k_ra8_ok A regular stage exists at the exact size.
 * @retval k_ra8_err_not_found Stage does not exist.
 * @retval k_ra8_err_invalid_state Stage is a directory.
 * @retval k_ra8_err_invalid_size Stage length differs from @p expected.
 * @retval other Underlying VFS stat failed.
 * @pre @p ctx is non-null and its staging path is canonical.
 * @pre No writer remains open for the stage.
 * @post No filesystem object is mutated.
 * @post Success validates only the observed closed artifact.
 * @note The caller rechecks before publication.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_staged_file_check(const ra8_mdl_storage_vfs_t* ctx,
                                                         uint64_t                     expected)
{
  ra8_io_vfs_stat_t stat = {};
  const ra8_err_t   err  = ra8_io_vfs_stat(ctx->staging_path, &stat);
  if (err != k_ra8_ok) {
    return err;
  }
  if (!stat.exists) {
    return k_ra8_err_not_found;
  }
  if (stat.is_directory) {
    return k_ra8_err_invalid_state;
  }
  if (stat.size_bytes != expected) {
    return k_ra8_err_invalid_size;
  }
  return k_ra8_ok;
}

/**
 * @brief Coordinator callback: close, size-check, and delegate validation.
 * @details Closes exactly once, verifies independent size accounting, then invokes
 * the optional format validator on the still-private path.
 * @param[in,out] opaque Bound ::ra8_mdl_storage_vfs_t context.
 * @param[in] total_bytes Independently verified transfer length.
 * @param[in] sha256 Independently verified transfer digest.
 * @return Canonical validation status.
 * @retval k_ra8_ok Closed artifact is ready for publication.
 * @retval k_ra8_err_null_ptr A required pointer was null.
 * @retval k_ra8_err_invalid_state Adapter is not actively writing.
 * @retval k_ra8_err_invalid_size Transfer accounting disagrees.
 * @retval other Close, metadata, or delegated validation failed.
 * @pre @p opaque and @p sha256 are non-null and digest spans its fixed extent.
 * @pre Adapter owns the active private writer and accumulated byte count.
 * @post Success leaves no writer open and sets state to ready.
 * @post Failure never publishes the private artifact and remains abortable.
 * @note Validation receives a closed path, never an opaque host stream.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_validate(void* opaque, uint64_t total_bytes, const uint8_t sha256[k_ra8_mdl_sha256_bytes])
{
  if (opaque == nullptr || sha256 == nullptr) {
    return k_ra8_err_null_ptr;
  }
  ra8_mdl_storage_vfs_t* const ctx = opaque;
  if (ctx->state != k_ra8_mdl_storage_vfs_writing) {
    return k_ra8_err_invalid_state;
  }
  if (total_bytes != ctx->bytes_written) {
    return k_ra8_err_invalid_size;
  }
  ra8_err_t err = internal_close_writer(ctx);
  if (err == k_ra8_ok) {
    err = internal_staged_file_check(ctx, total_bytes);
  }
  if ((err == k_ra8_ok) && (ctx->validate != nullptr)) {
    err = ctx->validate(ctx->validate_ctx, ctx->staging_path, total_bytes, sha256);
  }
  if (err == k_ra8_ok) {
    ctx->state = k_ra8_mdl_storage_vfs_ready;
  }
  return err;
}

/**
 * @brief Coordinator callback: one serialized no-replace same-mount publish.
 * @details Rechecks stage and final metadata immediately before one VFS rename.
 * @param[in,out] opaque Bound ::ra8_mdl_storage_vfs_t context.
 * @return Canonical publication status.
 * @retval k_ra8_ok Artifact was already committed or is now published.
 * @retval k_ra8_err_null_ptr Context pointer was null.
 * @retval k_ra8_err_invalid_state Artifact is not validated and ready.
 * @retval other Final policy, stage recheck, or rename failed.
 * @pre @p opaque points to an initialized adapter.
 * @pre Ready state names a closed validated sibling on the same mount.
 * @post Success makes the final path visible and marks the context committed.
 * @post Failure before rename preserves any existing final destination.
 * @note Native FAT/exFAT rename does not claim power-loss durability.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_commit(void* opaque)
{
  if (opaque == nullptr) {
    return k_ra8_err_null_ptr;
  }
  ra8_mdl_storage_vfs_t* const ctx = opaque;
  if (ctx->state == k_ra8_mdl_storage_vfs_committed) {
    return k_ra8_ok;
  }
  if (ctx->state != k_ra8_mdl_storage_vfs_ready) {
    return k_ra8_err_invalid_state;
  }
  ra8_err_t err = internal_staged_file_check(ctx, ctx->bytes_written);
  if (err == k_ra8_ok) {
    err = internal_final_absent(ctx->destination);
  }
  if (err == k_ra8_ok) {
    err = ra8_io_vfs_rename(ctx->staging_path, ctx->destination);
  }
  if (err == k_ra8_ok) {
    ctx->state = k_ra8_mdl_storage_vfs_committed;
  }
  return err;
}

/**
 * @brief Finish cleanup after the writer, if any, has been closed.
 * @details Preserves the earliest error while type-checking and removing the stage.
 * @param[in,out] ctx Adapter context naming a possibly owned private artifact.
 * @param[in] first_err Prior close/sync status to preserve.
 * @return First canonical cleanup status.
 * @retval k_ra8_ok No stage remained and no prior error occurred.
 * @retval other @p first_err when non-success, otherwise stat/unlink failure.
 * @pre @p ctx is non-null and its writer is closed.
 * @pre The staging path, when present, names the reserved owned leaf.
 * @post Successful absence/removal resets state to idle.
 * @post A directory at the reserved path is never removed.
 * @note Retryable when cleanup fails after the writer is closed.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_abort_stage(ra8_mdl_storage_vfs_t* ctx, ra8_err_t first_err)
{
  ra8_io_vfs_stat_t stat     = {};
  const ra8_err_t   stat_err = ra8_io_vfs_stat(ctx->staging_path, &stat);
  if (stat_err != k_ra8_ok) {
    return (first_err == k_ra8_ok) ? stat_err : first_err;
  }
  if (!stat.exists) {
    ctx->state = k_ra8_mdl_storage_vfs_idle;
    return first_err;
  }
  if (stat.is_directory) {
    return (first_err == k_ra8_ok) ? k_ra8_err_invalid_state : first_err;
  }
  const ra8_err_t unlink_err = ra8_io_vfs_unlink(ctx->staging_path);
  if (unlink_err == k_ra8_ok) {
    ctx->state = k_ra8_mdl_storage_vfs_idle;
  }
  return (first_err == k_ra8_ok) ? unlink_err : first_err;
}

/**
 * @brief Coordinator callback: best-effort, retryable, idempotent cleanup.
 * @details Closes a live writer once and removes only the reserved private stage.
 * @param[in,out] opaque Bound ::ra8_mdl_storage_vfs_t context.
 * @return Canonical cleanup status.
 * @retval k_ra8_ok Context was idle/committed or is now clean and idle.
 * @retval k_ra8_err_null_ptr Context pointer was null.
 * @retval other First close, metadata, or unlink error.
 * @pre @p opaque is null or points to an initialized adapter.
 * @pre No concurrent callback uses the same adapter instance.
 * @post A committed final artifact is never deleted.
 * @post Success leaves no owned writer or private stage.
 * @note Safe to retry after a non-success cleanup result.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_abort(void* opaque)
{
  if (opaque == nullptr) {
    return k_ra8_err_null_ptr;
  }
  ra8_mdl_storage_vfs_t* const ctx = opaque;
  if (ctx->state == k_ra8_mdl_storage_vfs_idle) {
    return k_ra8_ok;
  }
  if (ctx->state == k_ra8_mdl_storage_vfs_committed) {
    return k_ra8_ok;
  }
  const ra8_err_t close_err = internal_close_writer(ctx);
  return internal_abort_stage(ctx, close_err);
}

ra8_err_t ra8_mdl_storage_vfs_init(ra8_mdl_storage_vfs_t*              storage,
                                   const ra8_mdl_storage_vfs_config_t* config,
                                   ra8_mdl_storage_iface_t*            out_iface)
{
  if (storage == nullptr || config == nullptr || out_iface == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (config->stage_leaf == nullptr) {
    return k_ra8_err_null_ptr;
  }
  uint16_t        stage_len = 0U;
  const ra8_err_t err       = internal_stage_leaf_check(config->stage_leaf, &stage_len);
  if (err != k_ra8_ok) {
    return err;
  }
  *storage              = (ra8_mdl_storage_vfs_t){};
  storage->validate     = config->validate;
  storage->validate_ctx = config->validate_ctx;
  internal_copy(storage->stage_leaf, config->stage_leaf, stage_len);
  out_iface->begin    = internal_begin;
  out_iface->write    = internal_write;
  out_iface->validate = internal_validate;
  out_iface->commit   = internal_commit;
  out_iface->abort    = internal_abort;
  out_iface->ctx      = storage;
  return k_ra8_ok;
}
