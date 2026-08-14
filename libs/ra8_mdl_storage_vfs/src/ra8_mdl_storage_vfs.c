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

#include "ra8_err.h"
#include "ra8_io_vfs.h"

/** @brief Module-local path facts proven before any filesystem mutation. */
typedef struct {
  uint16_t length;     /**< Bytes before NUL.          */
  uint16_t last_slash; /**< Final component separator. */
} path_facts_t;

/** @brief Return whether one component is exactly `.` or `..`. */
static bool priv_is_dot_component(const char* text, uint16_t len)
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

/** @brief Measure one string without reading past a fixed capacity. */
static ra8_err_t priv_bounded_length(const char* text, uint16_t cap, uint16_t* out_len)
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

/** @brief Validate the mount prefix and locate its colon. */
static ra8_err_t priv_mount_prefix(const char* path, uint16_t len, uint16_t* out_colon)
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

/** @brief Validate canonical non-traversing components after `mount:/`. */
static ra8_err_t
priv_path_components(const char* path, uint16_t len, uint16_t colon, uint16_t* out_slash)
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
      if (priv_is_dot_component(&path[start], component_len)) {
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

/** @brief Prove a destination is a bounded canonical named-VFS file path. */
static ra8_err_t priv_path_facts(const char* path, path_facts_t* out)
{
  uint16_t  len = 0U;
  ra8_err_t err = priv_bounded_length(path, k_ra8_mdl_storage_vfs_path_capacity, &len);
  if (err != k_ra8_ok) {
    return err;
  }
  uint16_t colon = 0U;
  err            = priv_mount_prefix(path, len, &colon);
  if (err != k_ra8_ok) {
    return err;
  }
  uint16_t slash = 0U;
  err            = priv_path_components(path, len, colon, &slash);
  if (err != k_ra8_ok) {
    return err;
  }
  out->length     = len;
  out->last_slash = slash;
  return k_ra8_ok;
}

/** @brief Validate the caller-reserved simple staging leaf. */
static ra8_err_t priv_stage_leaf_check(const char* leaf, uint16_t* out_len)
{
  ra8_err_t err = priv_bounded_length(leaf, k_ra8_mdl_storage_vfs_stage_leaf_capacity, out_len);
  if (err != k_ra8_ok) {
    return err;
  }
  if (*out_len == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (priv_is_dot_component(leaf, *out_len)) {
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

/** @brief Copy a bounded NUL-terminated byte string. */
static void priv_copy(char* dst, const char* src, uint16_t len)
{
  for (uint16_t i = 0U; i < len; ++i) {
    dst[i] = src[i];
  }
  dst[len] = '\0';
}

/** @brief Compare a destination leaf with the reserved staging leaf. */
static bool priv_leaf_equals(const char* destination_leaf, const char* stage_leaf)
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

/** @brief Build the caller-owned sibling staging path without truncation. */
static ra8_err_t
priv_build_stage(ra8_mdl_storage_vfs_t* ctx, const path_facts_t* facts, uint16_t stage_len)
{
  const uint32_t prefix = (uint32_t)facts->last_slash + 1U;
  const uint32_t needed = prefix + (uint32_t)stage_len + 1U;
  if (needed > (uint32_t)k_ra8_mdl_storage_vfs_path_capacity) {
    return k_ra8_err_invalid_size;
  }
  const char* final_leaf = &ctx->destination[facts->last_slash + 1U];
  if (priv_leaf_equals(final_leaf, ctx->stage_leaf)) {
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

/** @brief Require the destination parent to exist and be a directory. */
static ra8_err_t priv_parent_check(ra8_mdl_storage_vfs_t* ctx, uint16_t last_slash)
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

/** @brief Refuse an existing final, distinguishing directories as bad paths. */
static ra8_err_t priv_final_absent(const char* destination)
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

/** @brief Remove a stale owned regular stage, but never a directory. */
static ra8_err_t priv_remove_stale_stage(const char* staging_path)
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

/** @brief Close the writer exactly once and retain staged cleanup state. */
static ra8_err_t priv_close_writer(ra8_mdl_storage_vfs_t* ctx)
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

/** @brief Coordinator callback: create the private sibling transaction file. */
static ra8_err_t priv_begin(void* opaque, const char* destination)
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
  ra8_err_t    err   = priv_path_facts(destination, &facts);
  if (err != k_ra8_ok) {
    return err;
  }
  priv_copy(ctx->destination, destination, facts.length);
  uint16_t stage_len = 0U;
  err                = priv_stage_leaf_check(ctx->stage_leaf, &stage_len);
  if (err == k_ra8_ok) {
    err = priv_build_stage(ctx, &facts, stage_len);
  }
  if (err == k_ra8_ok) {
    err = priv_parent_check(ctx, facts.last_slash);
  }
  if (err == k_ra8_ok) {
    err = priv_final_absent(ctx->destination);
  }
  if (err == k_ra8_ok) {
    err = priv_remove_stale_stage(ctx->staging_path);
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

/** @brief Coordinator callback: append one all-or-error bounded chunk. */
static ra8_err_t priv_write(void* opaque, const uint8_t* data, uint16_t len, uint16_t* written)
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

/** @brief Check the closed stage is still a regular file of the exact size. */
static ra8_err_t priv_staged_file_check(const ra8_mdl_storage_vfs_t* ctx, uint64_t expected)
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

/** @brief Coordinator callback: close, size-check, and delegate validation. */
static ra8_err_t
priv_validate(void* opaque, uint64_t total_bytes, const uint8_t sha256[k_ra8_mdl_sha256_bytes])
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
  ra8_err_t err = priv_close_writer(ctx);
  if (err == k_ra8_ok) {
    err = priv_staged_file_check(ctx, total_bytes);
  }
  if ((err == k_ra8_ok) && (ctx->validate != nullptr)) {
    err = ctx->validate(ctx->validate_ctx, ctx->staging_path, total_bytes, sha256);
  }
  if (err == k_ra8_ok) {
    ctx->state = k_ra8_mdl_storage_vfs_ready;
  }
  return err;
}

/** @brief Coordinator callback: one serialized no-replace same-mount publish. */
static ra8_err_t priv_commit(void* opaque)
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
  ra8_err_t err = priv_staged_file_check(ctx, ctx->bytes_written);
  if (err == k_ra8_ok) {
    err = priv_final_absent(ctx->destination);
  }
  if (err == k_ra8_ok) {
    err = ra8_io_vfs_rename(ctx->staging_path, ctx->destination);
  }
  if (err == k_ra8_ok) {
    ctx->state = k_ra8_mdl_storage_vfs_committed;
  }
  return err;
}

/** @brief Finish cleanup after the writer (if any) has been closed. */
static ra8_err_t priv_abort_stage(ra8_mdl_storage_vfs_t* ctx, ra8_err_t first_err)
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

/** @brief Coordinator callback: best-effort, retryable, idempotent cleanup. */
static ra8_err_t priv_abort(void* opaque)
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
  const ra8_err_t close_err = priv_close_writer(ctx);
  return priv_abort_stage(ctx, close_err);
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
  const ra8_err_t err       = priv_stage_leaf_check(config->stage_leaf, &stage_len);
  if (err != k_ra8_ok) {
    return err;
  }
  *storage              = (ra8_mdl_storage_vfs_t){};
  storage->validate     = config->validate;
  storage->validate_ctx = config->validate_ctx;
  priv_copy(storage->stage_leaf, config->stage_leaf, stage_len);
  out_iface->begin    = priv_begin;
  out_iface->write    = priv_write;
  out_iface->validate = priv_validate;
  out_iface->commit   = priv_commit;
  out_iface->abort    = priv_abort;
  out_iface->ctx      = storage;
  return k_ra8_ok;
}
