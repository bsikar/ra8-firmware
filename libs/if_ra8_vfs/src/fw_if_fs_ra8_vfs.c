/**
 * @file fw_if_fs_ra8_vfs.c
 * @brief `fw_if_fs` adapter over one bound `ra8_io_vfs` mount.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Binds the portable filesystem operations to one caller-selected, already
 * mounted `ra8_io_vfs` volume. The adapter prefixes bounded portable paths,
 * translates metadata and directory callbacks, and stages transactional
 * writes in caller-owned storage without POSIX APIs or dynamic allocation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "fw_if_fs_ra8_vfs.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "fw_if_fs.h"
#include "fw_if_fs_backend.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_fs_meta.h"
#include "ra8_io_vfs.h"

/** @brief Bounded attempts to find an unused short staging name. */
typedef enum : uint8_t {
  k_vfs_stage_attempts = 64U,
} vfs_stage_limits_t;

/** @brief Backend state stored in a caller's file workspace. */
typedef struct {
  ra8_fs_file_t* native;
} vfs_file_state_t;

/** @brief Backend state stored in a caller's transaction workspace. */
typedef struct {
  char                       destination[k_fw_fs_path_cap];
  char                       stage[k_fw_fs_path_cap];
  vfs_file_state_t           file_state;
  fw_fs_transaction_policy_t policy;
  bool                       writer_open;
  bool                       stage_exists;
} vfs_transaction_state_t;

/** @brief Directory callback bridge state. */
typedef struct {
  fw_fs_list_fn_t callback;
  void*           callback_ctx;
  uint32_t        max_entries;
  uint32_t        count;
  ra8_err_t       callback_error;
  bool            stopped;
} vfs_list_state_t;

static const fw_fs_stream_iface_t s_stream_iface;

/** @brief Translate one decoded FAT/exFAT civil timestamp without an epoch. */
static fw_fs_timestamp_t internal_timestamp(const ra8_fs_timestamp_t* native)
{
  fw_fs_timestamp_t portable = {};
  if (native->valid) {
    portable.value.nanosecond     = (uint32_t)native->value.centisecond * 10000000UL;
    portable.value.year           = native->value.year;
    portable.value.utc_offset_min = native->value.utc_offset_min;
    portable.value.month          = native->value.month;
    portable.value.day            = native->value.day;
    portable.value.hour           = native->value.hour;
    portable.value.minute         = native->value.minute;
    portable.value.second         = native->value.second;
    portable.valid                = true;
    portable.utc_offset_valid     = native->utc_offset_valid;
  }
  return portable;
}

/** @brief Bounded string length returning the cap on unterminated input. */
static uint16_t internal_len(const char* text, uint16_t cap)
{
  uint16_t length = 0U;
  while (length < cap) {
    if (text[length] == '\0') {
      break;
    }
    ++length;
  }
  return length;
}

/** @brief Prefix one portable path into caller-owned adapter scratch. */
static ra8_err_t internal_full_path(fw_fs_ra8_vfs_state_t* state, const char* path, char* out)
{
  const uint16_t mount_len = internal_len(state->mount_name, k_ra8_io_vfs_name_max);
  const uint16_t path_len  = internal_len(path, (uint16_t)k_fw_fs_path_cap);
  if (mount_len >= (uint16_t)k_ra8_io_vfs_name_max) {
    return k_ra8_err_invalid_state;
  }
  if (path_len >= (uint16_t)k_fw_fs_path_cap) {
    return k_ra8_err_invalid_size;
  }
  uint16_t cursor = 0U;
  for (uint16_t i = 0U; i < mount_len; ++i) {
    out[cursor] = state->mount_name[i];
    ++cursor;
  }
  out[cursor] = ':';
  ++cursor;
  for (uint16_t i = 0U; i <= path_len; ++i) {
    out[cursor] = path[i];
    ++cursor;
  }
  return k_ra8_ok;
}

/** @brief Convert one VFS stat result to portable metadata. */
static ra8_err_t internal_stat(void* ctx, const char* path, fw_fs_stat_t* out)
{
  fw_fs_ra8_vfs_state_t* state = (fw_fs_ra8_vfs_state_t*)ctx;
  const ra8_err_t        built = internal_full_path(state, path, state->path_a);
  if (built != k_ra8_ok) {
    return built;
  }
  ra8_io_vfs_stat_t native = {};
  const ra8_err_t   result = ra8_io_vfs_stat(state->path_a, &native);
  if (result != k_ra8_ok) {
    return result;
  }
  out->exists     = native.exists;
  out->size_bytes = native.size_bytes;
  out->created    = internal_timestamp(&native.created);
  out->modified   = internal_timestamp(&native.modified);
  out->accessed   = internal_timestamp(&native.accessed);
  out->type       = k_fw_fs_node_none;
  if (native.exists) {
    out->type = native.is_directory ? k_fw_fs_node_directory : k_fw_fs_node_file;
  }
  return k_ra8_ok;
}

/** @brief Deliver one native directory entry through the bounded portable
 * callback. */
static void internal_list_entry(const char* name, uint8_t attr, uint64_t size, void* ctx)
{
  vfs_list_state_t* state = (vfs_list_state_t*)ctx;
  if (state->stopped) {
    return;
  }
  if (state->count >= state->max_entries) {
    state->stopped = true;
    return;
  }
  const uint16_t length = internal_len(name, (uint16_t)k_fw_fs_path_cap);
  if (length >= (uint16_t)k_fw_fs_path_cap) {
    state->callback_error = k_ra8_err_invalid_size;
    state->stopped        = true;
    return;
  }
  const fw_fs_dirent_t entry = {
    .name       = name,
    .size_bytes = size,
    .name_bytes = length,
    .type       = ((attr & (uint8_t)k_ra8_fs_attr_directory) != 0U) ? k_fw_fs_node_directory
                                                                    : k_fw_fs_node_file,
  };
  bool            keep_going = true;
  const ra8_err_t delivered  = state->callback(state->callback_ctx, &entry, &keep_going);
  ++state->count;
  if (delivered != k_ra8_ok) {
    state->callback_error = delivered;
    state->stopped        = true;
    return;
  }
  if (!keep_going) {
    state->stopped = true;
  }
}

/** @brief Enumerate a VFS directory while bounding callback delivery. */
static ra8_err_t internal_listdir(void*           ctx,
                                  const char*     path,
                                  uint32_t        max_entries,
                                  fw_fs_list_fn_t callback,
                                  void*           callback_ctx,
                                  uint32_t*       out_count,
                                  bool*           out_complete)
{
  fw_fs_ra8_vfs_state_t* state = (fw_fs_ra8_vfs_state_t*)ctx;
  const ra8_err_t        built = internal_full_path(state, path, state->path_a);
  if (built != k_ra8_ok) {
    return built;
  }
  vfs_list_state_t bridge = {
    .callback       = callback,
    .callback_ctx   = callback_ctx,
    .max_entries    = max_entries,
    .count          = 0U,
    .callback_error = k_ra8_ok,
    .stopped        = false,
  };
  const ra8_err_t listed = ra8_io_vfs_listdir(state->path_a, internal_list_entry, &bridge);
  *out_count             = bridge.count;
  *out_complete          = !bridge.stopped;
  if (bridge.callback_error != k_ra8_ok) {
    return bridge.callback_error;
  }
  return listed;
}

/** @brief One-path VFS dispatch helper. */
static ra8_err_t internal_path_op(void* ctx, const char* path, ra8_err_t (*operation)(const char*))
{
  fw_fs_ra8_vfs_state_t* state = (fw_fs_ra8_vfs_state_t*)ctx;
  const ra8_err_t        built = internal_full_path(state, path, state->path_a);
  if (built != k_ra8_ok) {
    return built;
  }
  return operation(state->path_a);
}

/** @brief Create one VFS directory. */
static ra8_err_t internal_mkdir(void* ctx, const char* path)
{
  return internal_path_op(ctx, path, ra8_io_vfs_mkdir);
}

/** @brief Unlink one VFS file. */
static ra8_err_t internal_unlink(void* ctx, const char* path)
{
  return internal_path_op(ctx, path, ra8_io_vfs_unlink);
}

/** @brief Remove one empty VFS directory. */
static ra8_err_t internal_rmdir(void* ctx, const char* path)
{
  return internal_path_op(ctx, path, ra8_io_vfs_rmdir);
}

/** @brief Rename without replacement inside the bound VFS mount. */
static ra8_err_t
internal_rename(void* ctx, const char* old_path, const char* new_path, bool replace)
{
  if (replace) {
    return k_ra8_err_not_supported;
  }
  fw_fs_ra8_vfs_state_t* state     = (fw_fs_ra8_vfs_state_t*)ctx;
  const ra8_err_t        old_built = internal_full_path(state, old_path, state->path_a);
  if (old_built != k_ra8_ok) {
    return old_built;
  }
  const ra8_err_t new_built = internal_full_path(state, new_path, state->path_b);
  if (new_built != k_ra8_ok) {
    return new_built;
  }
  return ra8_io_vfs_rename(state->path_a, state->path_b);
}

/** @brief Query free space directly from the matching live mount. */
static ra8_err_t internal_space(void* ctx, fw_fs_space_t* out)
{
  fw_fs_ra8_vfs_state_t* state  = (fw_fs_ra8_vfs_state_t*)ctx;
  ra8_fs_space_t         native = {};
  const ra8_err_t        result = ra8_fs_free_space(state->mount, &native);
  if (result != k_ra8_ok) {
    return result;
  }
  out->total_bytes = native.total_bytes;
  out->free_bytes  = native.free_bytes;
  out->used_bytes  = native.used_bytes;
  return k_ra8_ok;
}

/** @brief Map portable modes to the native VFS mode set. */
static ra8_err_t internal_mode(fw_fs_open_mode_t mode, ra8_fs_mode_t* out)
{
  if (mode == k_fw_fs_open_read) {
    *out = k_ra8_fs_mode_read;
    return k_ra8_ok;
  }
  if (mode == k_fw_fs_open_write_truncate) {
    *out = k_ra8_fs_mode_write;
    return k_ra8_ok;
  }
  if (mode == k_fw_fs_open_append) {
    *out = k_ra8_fs_mode_append;
    return k_ra8_ok;
  }
  return k_ra8_err_not_supported;
}

/** @brief Open a VFS file into caller workspace. */
static ra8_err_t internal_open(void*             ctx,
                               const char*       path,
                               fw_fs_open_mode_t mode,
                               void*             file_state,
                               uint32_t          state_bytes)
{
  if (state_bytes < sizeof(vfs_file_state_t)) {
    return k_ra8_err_no_mem;
  }
  ra8_fs_mode_t   native_mode = k_ra8_fs_mode_read;
  const ra8_err_t mode_err    = internal_mode(mode, &native_mode);
  if (mode_err != k_ra8_ok) {
    return mode_err;
  }
  fw_fs_ra8_vfs_state_t* state = (fw_fs_ra8_vfs_state_t*)ctx;
  const ra8_err_t        built = internal_full_path(state, path, state->path_a);
  if (built != k_ra8_ok) {
    return built;
  }
  vfs_file_state_t* file = (vfs_file_state_t*)file_state;
  file->native           = nullptr;
  return ra8_io_vfs_open(state->path_a, native_mode, &file->native);
}

/** @brief Read a native VFS file. */
static ra8_err_t
internal_read(void* ctx, void* file_state, uint8_t* dst, uint32_t cap, uint32_t* out_read)
{
  (void)ctx;
  vfs_file_state_t* file = (vfs_file_state_t*)file_state;
  return ra8_fs_read(file->native, dst, cap, out_read);
}

/** @brief Write a native VFS file; native writes are all-or-error. */
static ra8_err_t
internal_write(void* ctx, void* file_state, const uint8_t* src, uint32_t len, uint32_t* out_written)
{
  (void)ctx;
  vfs_file_state_t* file   = (vfs_file_state_t*)file_state;
  const ra8_err_t   result = ra8_fs_write(file->native, src, len);
  if (result == k_ra8_ok) {
    *out_written = len;
  }
  return result;
}

/** @brief Seek a native VFS file. */
static ra8_err_t internal_seek(void* ctx, void* file_state, uint64_t offset)
{
  (void)ctx;
  vfs_file_state_t* file = (vfs_file_state_t*)file_state;
  return ra8_fs_seek(file->native, offset);
}

/** @brief Tell a native VFS file. */
static ra8_err_t internal_tell(void* ctx, void* file_state, uint64_t* out_offset)
{
  (void)ctx;
  vfs_file_state_t* file = (vfs_file_state_t*)file_state;
  return ra8_fs_tell(file->native, out_offset);
}

/** @brief Query a native VFS file's size. */
static ra8_err_t internal_size(void* ctx, void* file_state, uint64_t* out_size)
{
  (void)ctx;
  vfs_file_state_t* file = (vfs_file_state_t*)file_state;
  return ra8_fs_size(file->native, out_size);
}

/** @brief Close a native VFS file. */
static ra8_err_t internal_close(void* ctx, void* file_state)
{
  (void)ctx;
  vfs_file_state_t* file   = (vfs_file_state_t*)file_state;
  const ra8_err_t   result = ra8_fs_close(file->native);
  file->native             = nullptr;
  return result;
}

/** @brief Copy a bounded portable path. */
static ra8_err_t internal_copy_path(char* out, const char* path)
{
  for (uint16_t i = 0U; i < (uint16_t)k_fw_fs_path_cap; ++i) {
    out[i] = path[i];
    if (path[i] == '\0') {
      return k_ra8_ok;
    }
  }
  return k_ra8_err_invalid_size;
}

/** @brief Render a six-digit hexadecimal transaction id. */
static void internal_hex6(char out[6], uint32_t value)
{
  static const char digits[] = "0123456789ABCDEF";
  for (uint8_t i = 0U; i < 6U; ++i) {
    const uint8_t shift = (uint8_t)((5U - i) * 4U);
    out[i]              = digits[(value >> shift) & 0x0FU];
  }
}

/** @brief Build an 8.3-compatible sibling stage path. */
static ra8_err_t internal_stage_path(const char* destination, uint32_t id, char* out)
{
  uint16_t last_slash = 0U;
  uint16_t length     = 0U;
  while (length < (uint16_t)k_fw_fs_path_cap) {
    const char value = destination[length];
    if (value == '\0') {
      break;
    }
    if (value == '/') {
      last_slash = length;
    }
    ++length;
  }
  if (length >= (uint16_t)k_fw_fs_path_cap) {
    return k_ra8_err_invalid_size;
  }
  const uint16_t stage_length = (uint16_t)(last_slash + 1U + 12U);
  if (stage_length >= (uint16_t)k_fw_fs_path_cap) {
    return k_ra8_err_invalid_size;
  }
  for (uint16_t i = 0U; i <= last_slash; ++i) {
    out[i] = destination[i];
  }
  uint16_t cursor = (uint16_t)(last_slash + 1U);
  out[cursor++]   = 'T';
  out[cursor++]   = 'X';
  internal_hex6(&out[cursor], id & 0x00FFFFFFUL);
  cursor        = (uint16_t)(cursor + 6U);
  out[cursor++] = '.';
  out[cursor++] = 'T';
  out[cursor++] = 'M';
  out[cursor++] = 'P';
  out[cursor]   = '\0';
  return k_ra8_ok;
}

/** @brief Open an unused VFS staging file after a bounded collision search. */
static ra8_err_t internal_stage_open(fw_fs_ra8_vfs_state_t* state, vfs_transaction_state_t* txn)
{
  for (uint8_t attempt = 0U; attempt < (uint8_t)k_vfs_stage_attempts; ++attempt) {
    ++state->transaction_id;
    const ra8_err_t named =
      internal_stage_path(txn->destination, state->transaction_id, txn->stage);
    if (named != k_ra8_ok) {
      return named;
    }
    fw_fs_stat_t    stage_stat = {};
    const ra8_err_t stated     = internal_stat(state, txn->stage, &stage_stat);
    if (stated != k_ra8_ok) {
      return stated;
    }
    if (stage_stat.exists) {
      continue;
    }
    const ra8_err_t opened = internal_open(state,
                                           txn->stage,
                                           k_fw_fs_open_write_truncate,
                                           &txn->file_state,
                                           sizeof(txn->file_state));
    if (opened == k_ra8_ok) {
      txn->writer_open  = true;
      txn->stage_exists = true;
      return k_ra8_ok;
    }
    if (opened != k_ra8_err_exists) {
      return opened;
    }
  }
  return k_ra8_err_no_mem;
}

/** @brief Begin a create-new transaction without touching the destination. */
static ra8_err_t internal_txn_begin(void*                      ctx,
                                    void*                      transaction_state,
                                    uint32_t                   state_bytes,
                                    const char*                destination,
                                    fw_fs_transaction_policy_t policy)
{
  if (state_bytes < sizeof(vfs_transaction_state_t)) {
    return k_ra8_err_no_mem;
  }
  if (policy != k_fw_fs_txn_create_new) {
    return k_ra8_err_not_supported;
  }
  fw_fs_ra8_vfs_state_t* state            = (fw_fs_ra8_vfs_state_t*)ctx;
  fw_fs_stat_t           destination_stat = {};
  const ra8_err_t        stated           = internal_stat(state, destination, &destination_stat);
  if (stated != k_ra8_ok) {
    return stated;
  }
  if (destination_stat.exists) {
    return k_ra8_err_exists;
  }
  vfs_transaction_state_t* txn = (vfs_transaction_state_t*)transaction_state;
  (void)memset(txn, 0, sizeof(*txn));
  txn->policy            = policy;
  const ra8_err_t copied = internal_copy_path(txn->destination, destination);
  if (copied != k_ra8_ok) {
    return copied;
  }
  return internal_stage_open(state, txn);
}

/** @brief Write transaction bytes to the open VFS stage. */
static ra8_err_t internal_txn_write(void*          ctx,
                                    void*          transaction_state,
                                    const uint8_t* src,
                                    uint32_t       len,
                                    uint32_t*      out_written)
{
  vfs_transaction_state_t* txn = (vfs_transaction_state_t*)transaction_state;
  if (!txn->writer_open) {
    return k_ra8_err_invalid_state;
  }
  return internal_write(ctx, &txn->file_state, src, len, out_written);
}

/** @brief Seek within the open VFS stage for header/table backfill. */
static ra8_err_t internal_txn_seek(void* ctx, void* transaction_state, uint64_t offset)
{
  vfs_transaction_state_t* txn = (vfs_transaction_state_t*)transaction_state;
  if (!txn->writer_open) {
    return k_ra8_err_invalid_state;
  }
  uint64_t        size  = 0U;
  const ra8_err_t sized = internal_size(ctx, &txn->file_state, &size);
  if (sized != k_ra8_ok) {
    return sized;
  }
  if (offset > size) {
    return k_ra8_err_invalid_size;
  }
  return internal_seek(ctx, &txn->file_state, offset);
}

/** @brief Close and reopen the VFS stage so a validator reads committed bytes.
 */
static ra8_err_t internal_txn_validate(void*               ctx,
                                       void*               transaction_state,
                                       fw_fs_validate_fn_t validator,
                                       void*               validator_ctx)
{
  vfs_transaction_state_t* txn = (vfs_transaction_state_t*)transaction_state;
  if (!txn->writer_open) {
    return k_ra8_err_invalid_state;
  }
  const ra8_err_t closed = internal_close(ctx, &txn->file_state);
  txn->writer_open       = false;
  if (closed != k_ra8_ok) {
    return closed;
  }
  const ra8_err_t opened =
    internal_open(ctx, txn->stage, k_fw_fs_open_read, &txn->file_state, sizeof(txn->file_state));
  if (opened != k_ra8_ok) {
    return opened;
  }
  fw_fs_file_t staged = {
    .iface       = &s_stream_iface,
    .ctx         = ctx,
    .state       = &txn->file_state,
    .state_bytes = sizeof(txn->file_state),
    .is_open     = true,
  };
  const ra8_err_t checked = validator(validator_ctx, &staged);
  const ra8_err_t shut    = fw_fs_close(&staged);
  if (checked != k_ra8_ok) {
    return checked;
  }
  return shut;
}

/** @brief Publish an absent-destination stage by same-mount rename. */
static ra8_err_t internal_txn_commit(void* ctx, void* transaction_state, bool* out_published)
{
  vfs_transaction_state_t* txn = (vfs_transaction_state_t*)transaction_state;
  if (txn->writer_open) {
    return k_ra8_err_invalid_state;
  }
  const ra8_err_t renamed = internal_rename(ctx, txn->stage, txn->destination, false);
  if (renamed == k_ra8_ok) {
    txn->stage_exists = false;
    *out_published    = true;
  }
  return renamed;
}

/** @brief Close and unlink a VFS stage, preserving the destination. */
static ra8_err_t internal_txn_abort(void* ctx, void* transaction_state)
{
  vfs_transaction_state_t* txn   = (vfs_transaction_state_t*)transaction_state;
  ra8_err_t                first = k_ra8_ok;
  if (txn->writer_open) {
    first            = internal_close(ctx, &txn->file_state);
    txn->writer_open = false;
  }
  if (txn->stage_exists) {
    const ra8_err_t removed = internal_unlink(ctx, txn->stage);
    if (first == k_ra8_ok) {
      first = removed;
    }
    if (removed == k_ra8_ok) {
      txn->stage_exists = false;
    }
  }
  return first;
}

/** @brief Immutable firmware namespace vtable. */
static const fw_fs_namespace_iface_t s_namespace_iface = {
  .stat    = internal_stat,
  .listdir = internal_listdir,
  .mkdir   = internal_mkdir,
  .unlink  = internal_unlink,
  .rmdir   = internal_rmdir,
  .rename  = internal_rename,
  .space   = internal_space,
};

/** @brief Immutable firmware stream vtable. */
static const fw_fs_stream_iface_t s_stream_iface = {
  .open  = internal_open,
  .read  = internal_read,
  .write = internal_write,
  .seek  = internal_seek,
  .tell  = internal_tell,
  .size  = internal_size,
  .sync  = nullptr,
  .close = internal_close,
};

/** @brief Immutable firmware transaction vtable. */
static const fw_fs_transaction_iface_t s_transaction_iface = {
  .begin    = internal_txn_begin,
  .write    = internal_txn_write,
  .seek     = internal_txn_seek,
  .validate = internal_txn_validate,
  .commit   = internal_txn_commit,
  .abort    = internal_txn_abort,
};

/** @brief Validate and copy a VFS mount name into adapter state. */
static ra8_err_t internal_mount_name(fw_fs_ra8_vfs_state_t* state, const char* name)
{
  uint16_t length = 0U;
  while (length < (uint16_t)k_ra8_io_vfs_name_max) {
    const char value = name[length];
    if (value == '\0') {
      break;
    }
    if (value == ':' || value == '/') {
      return k_ra8_err_invalid_arg;
    }
    ++length;
  }
  if (length == 0U || length >= (uint16_t)k_ra8_io_vfs_name_max) {
    return k_ra8_err_invalid_arg;
  }
  for (uint16_t i = 0U; i <= length; ++i) {
    state->mount_name[i] = name[i];
  }
  return k_ra8_ok;
}

ra8_err_t
fw_fs_ra8_vfs_init(fw_fs_t* out, fw_fs_ra8_vfs_state_t* state, const fw_fs_ra8_vfs_cfg_t* cfg)
{
  if (out == nullptr || state == nullptr || cfg == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (cfg->mount_name == nullptr || cfg->mount == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (cfg->mount->in_use == 0U) {
    return k_ra8_err_not_initialized;
  }
  (void)memset(state, 0, sizeof(*state));
  const ra8_err_t named = internal_mount_name(state, cfg->mount_name);
  if (named != k_ra8_ok) {
    return named;
  }
  state->mount           = cfg->mount;
  state->removable_media = cfg->removable_media;

  fw_fs_caps_t caps = {
    .max_file_bytes = (cfg->mount->type == k_ra8_fs_type_exfat)
                        ? UINT64_MAX
                        : (uint64_t)k_ra8_fs_fat_max_file_bytes,
    .flags          = (uint32_t)k_fw_fs_cap_namespace | (uint32_t)k_fw_fs_cap_stream |
                      (uint32_t)k_fw_fs_cap_space_query | (uint32_t)k_fw_fs_cap_same_volume_rename |
                      (uint32_t)k_fw_fs_cap_atomic_noreplace | (uint32_t)k_fw_fs_cap_transactions |
                      (uint32_t)k_fw_fs_cap_rejects_symlink_walk,
    .file_workspace_bytes        = sizeof(vfs_file_state_t),
    .transaction_workspace_bytes = sizeof(vfs_transaction_state_t),
    .path_max_bytes              = (uint16_t)k_fw_fs_path_cap,
    .name_max_bytes              = (cfg->mount->type == k_ra8_fs_type_exfat) ? 192U : 510U,
    .max_open_files              = (uint16_t)k_ra8_fs_max_files,
    .file_workspace_align        = (uint8_t)_Alignof(vfs_file_state_t),
    .transaction_workspace_align = (uint8_t)_Alignof(vfs_transaction_state_t),
  };
  caps.flags |= (uint32_t)k_fw_fs_cap_created_time | (uint32_t)k_fw_fs_cap_modified_time |
                (uint32_t)k_fw_fs_cap_accessed_time;
  if (cfg->removable_media) {
    caps.flags |= (uint32_t)k_fw_fs_cap_removable_media;
  }
  return fw_fs_bind(out, &s_namespace_iface, &s_stream_iface, &s_transaction_iface, state, &caps);
}
