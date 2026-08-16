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
#include "fw_if_fs_ra8_vfs_contracts_internal.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_fs_meta.h"
#include "ra8_io_vfs.h"

/** @brief Bounded attempts to find an unused short staging name. */
typedef enum : uint8_t {
  k_vfs_hex_nibble_bits  = 4U, /**< Bits represented by one hex digit. */
  k_vfs_stage_hex_digits = 6U, /**< Hex digits in a stage identifier.  */
  /** @brief Highest valid index in a stage identifier. */
  k_vfs_hex_last_digit  = k_vfs_stage_hex_digits - 1U,
  k_vfs_hex_nibble_mask = 0x0FU, /**< Low-nibble mask.      */
  k_vfs_stage_attempts  = 64U,   /**< Collision-search cap. */
} vfs_stage_limits_t;

/** @brief Portable-path limits exposed by the FAT/exFAT adapter. */
typedef enum : uint16_t {
  k_vfs_stage_leaf_bytes     = 12U,  /**< `TX` + six hex digits + `.TMP`. */
  k_vfs_exfat_name_max_bytes = 192U, /**< exFAT component byte cap.       */
  k_vfs_fat_name_max_bytes   = 510U, /**< FAT LFN component byte cap.     */
} vfs_path_limits_t;

/** @brief Timestamp and transaction arithmetic constants. */
typedef enum : uint32_t {
  k_vfs_nanoseconds_per_centisecond = 10000000U,   /**< Nanoseconds in 0.01 s.  */
  k_vfs_transaction_id_mask         = 0x00FFFFFFU, /**< Six hexadecimal digits. */
} vfs_numeric_limits_t;

/** @brief Backend state stored in a caller's file workspace. */
typedef struct {
  ra8_fs_file_t* native; /**< Open repository VFS handle, or NULL when consumed. */
} vfs_file_state_t;

/** @brief Backend state stored in a caller's directory workspace. */
typedef struct {
  ra8_io_vfs_dir_t native; /**< Independent format-neutral VFS cursor. */
} vfs_directory_state_t;

/** @brief Callback-list compatibility bridge over a format-owned enumeration. */
typedef struct {
  fw_fs_list_fn_t callback;       /**< Portable callback.            */
  void*           callback_ctx;   /**< Portable callback context.    */
  uint32_t        max_entries;    /**< Delivery ceiling.             */
  uint32_t        count;          /**< Deliveries attempted.         */
  ra8_err_t       callback_error; /**< First callback/entry error.   */
  bool            stopped;        /**< Budget or callback stop seen. */
} vfs_list_state_t;

/** @brief Backend state stored in a caller's transaction workspace. */
typedef struct {
  char                       destination[k_fw_fs_path_cap]; /**< Final portable path.  */
  char                       stage[k_fw_fs_path_cap];       /**< Private stage path.   */
  vfs_file_state_t           file_state;                    /**< Stage handle state.   */
  fw_fs_transaction_policy_t policy;                        /**< Fixed publish policy. */
  bool                       writer_open;                   /**< Writer is owned.      */
  bool                       stage_exists;                  /**< Stage is owned.       */
} vfs_transaction_state_t;

static const fw_fs_stream_iface_t s_stream_iface;

/* see header for full description */
RA8_INTERNAL static fw_fs_timestamp_t internal_timestamp(const ra8_fs_timestamp_t* native)
{
  fw_fs_timestamp_t portable = {};
  if (native->valid) {
    portable.value.nanosecond =
      (uint32_t)native->value.centisecond * k_vfs_nanoseconds_per_centisecond;
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

/* see header for full description */
RA8_INTERNAL static uint16_t internal_len(const char* text, uint16_t cap)
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

/* see header for full description */
RA8_INTERNAL static ra8_err_t
internal_full_path(fw_fs_ra8_vfs_state_t* state, const char* path, char* out)
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

/* see header for full description */
RA8_INTERNAL static ra8_err_t internal_stat(void* ctx, const char* path, fw_fs_stat_t* out)
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

/* see header for full description */
RA8_INTERNAL static ra8_err_t
internal_dir_open(void* ctx, const char* path, void* directory_state, uint32_t state_bytes)
{
  fw_fs_ra8_vfs_state_t* state = (fw_fs_ra8_vfs_state_t*)ctx;
  if (state_bytes < (uint32_t)sizeof(vfs_directory_state_t)) {
    return k_ra8_err_no_mem;
  }
  vfs_directory_state_t* directory = (vfs_directory_state_t*)directory_state;
  *directory                       = (vfs_directory_state_t){};
  const ra8_err_t built            = internal_full_path(state, path, state->path_a);
  if (built != k_ra8_ok) {
    return built;
  }
  const uintptr_t first       = (uintptr_t)directory_state + (uintptr_t)sizeof(*directory);
  const uintptr_t align       = (uintptr_t)state->directory_workspace_align;
  const uintptr_t native_base = (first + align - 1U) & ~(align - 1U);
  const uintptr_t consumed    = native_base - (uintptr_t)directory_state;
  if (consumed > (uintptr_t)state_bytes) {
    return k_ra8_err_no_mem;
  }
  if ((uint64_t)state->directory_workspace_bytes > ((uint64_t)state_bytes - consumed)) {
    return k_ra8_err_no_mem;
  }
  return ra8_io_vfs_dir_open(state->path_a,
                             &directory->native,
                             (void*)native_base,
                             state_bytes - (uint32_t)consumed);
}

/* see header for full description */
RA8_INTERNAL static ra8_err_t
internal_dir_next(void* ctx, void* directory_state, fw_fs_dirent_value_t* out, bool* out_entry)
{
  (void)ctx;
  vfs_directory_state_t* directory = (vfs_directory_state_t*)directory_state;
  ra8_fs_dirent_t        native    = {};
  const ra8_err_t        err       = ra8_io_vfs_dir_next(&directory->native, &native, out_entry);
  if (err != k_ra8_ok) {
    return err;
  }
  if (!*out_entry) {
    return err;
  }
  const uint16_t length = internal_len(native.name, (uint16_t)k_fw_fs_path_cap);
  if (length >= (uint16_t)k_fw_fs_path_cap) {
    return k_ra8_err_invalid_size;
  }
  (void)memcpy(out->name, native.name, (size_t)length + 1U);
  out->name_bytes = length;
  out->size_bytes = native.size_bytes;
  out->type = ((native.attr & (uint8_t)k_ra8_fs_attr_directory) != 0U) ? k_fw_fs_node_directory
                                                                       : k_fw_fs_node_file;
  return k_ra8_ok;
}

/* see header for full description */
RA8_INTERNAL static ra8_err_t internal_dir_close(void* ctx, void* directory_state)
{
  (void)ctx;
  vfs_directory_state_t* directory = (vfs_directory_state_t*)directory_state;
  return ra8_io_vfs_dir_close(&directory->native);
}

/**
 * @brief Translate one format-list callback into the legacy bounded facade.
 * @details Bounds and classifies one native entry before forwarding it to the
 *          portable callback, preserving stop and error state in the bridge.
 * @param[in] name NUL-terminated native entry leaf.
 * @param[in] attr Native filesystem attribute bits.
 * @param[in] size Native file size; ignored for directory classification.
 * @param[in,out] ctx Active bounded-list bridge state.
 * @pre @p name and @p ctx are non-NULL.
 * @pre @p ctx contains a live callback and coherent delivery bounds.
 * @post At most one callback is delivered.
 * @post The bridge records every capacity stop, callback stop, or callback error.
 * @note An overlong native name is converted to a fail-closed bridge error.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void
internal_list_entry(const char* name, uint8_t attr, uint64_t size, void* ctx)
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
  bool keep_going       = true;
  state->callback_error = state->callback(state->callback_ctx, &entry, &keep_going);
  ++state->count;
  state->stopped = state->callback_error != k_ra8_ok;
  if (!keep_going) {
    state->stopped = true;
  }
}

/* see header for full description */
RA8_INTERNAL static ra8_err_t internal_listdir(void*           ctx,
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
  vfs_list_state_t bridge = {.callback       = callback,
                             .callback_ctx   = callback_ctx,
                             .max_entries    = max_entries,
                             .callback_error = k_ra8_ok};
  const ra8_err_t  listed = ra8_io_vfs_listdir(state->path_a, internal_list_entry, &bridge);
  *out_count              = bridge.count;
  *out_complete           = !bridge.stopped;
  return (bridge.callback_error == k_ra8_ok) ? listed : bridge.callback_error;
}

/** @brief One-path VFS dispatch helper. */
RA8_INTERNAL static ra8_err_t
internal_path_op(void* ctx, const char* path, ra8_err_t (*operation)(const char*))
{
  fw_fs_ra8_vfs_state_t* state = (fw_fs_ra8_vfs_state_t*)ctx;
  const ra8_err_t        built = internal_full_path(state, path, state->path_a);
  if (built != k_ra8_ok) {
    return built;
  }
  return operation(state->path_a);
}

/* see header for full description */
RA8_INTERNAL static ra8_err_t internal_mkdir(void* ctx, const char* path)
{
  return internal_path_op(ctx, path, ra8_io_vfs_mkdir);
}

/* see header for full description */
RA8_INTERNAL static ra8_err_t internal_unlink(void* ctx, const char* path)
{
  return internal_path_op(ctx, path, ra8_io_vfs_unlink);
}

/* see header for full description */
RA8_INTERNAL static ra8_err_t internal_rmdir(void* ctx, const char* path)
{
  return internal_path_op(ctx, path, ra8_io_vfs_rmdir);
}

/* see header for full description */
RA8_INTERNAL static ra8_err_t
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

/* see header for full description */
RA8_INTERNAL static ra8_err_t internal_space(void* ctx, fw_fs_space_t* out)
{
  fw_fs_ra8_vfs_state_t* state  = (fw_fs_ra8_vfs_state_t*)ctx;
  ra8_fs_space_t         native = {};
  const ra8_err_t        result = ra8_io_vfs_free_space(state->mount_name, &native);
  if (result != k_ra8_ok) {
    return result;
  }
  out->total_bytes = native.total_bytes;
  out->free_bytes  = native.free_bytes;
  out->used_bytes  = native.used_bytes;
  return k_ra8_ok;
}

/* see header for full description */
RA8_INTERNAL static ra8_err_t internal_mode(fw_fs_open_mode_t mode, ra8_fs_mode_t* out)
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

/* see header for full description */
RA8_INTERNAL static ra8_err_t internal_open(void*             ctx,
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

/* see header for full description */
RA8_INTERNAL static ra8_err_t
internal_read(void* ctx, void* file_state, uint8_t* dst, uint32_t cap, uint32_t* out_read)
{
  (void)ctx;
  vfs_file_state_t* file = (vfs_file_state_t*)file_state;
  return ra8_fs_read(file->native, dst, cap, out_read);
}

/* see header for full description */
RA8_INTERNAL static ra8_err_t
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

/* see header for full description */
RA8_INTERNAL static ra8_err_t internal_seek(void* ctx, void* file_state, uint64_t offset)
{
  (void)ctx;
  vfs_file_state_t* file = (vfs_file_state_t*)file_state;
  return ra8_fs_seek(file->native, offset);
}

/* see header for full description */
RA8_INTERNAL static ra8_err_t internal_tell(void* ctx, void* file_state, uint64_t* out_offset)
{
  (void)ctx;
  vfs_file_state_t* file = (vfs_file_state_t*)file_state;
  return ra8_fs_tell(file->native, out_offset);
}

/* see header for full description */
RA8_INTERNAL static ra8_err_t internal_size(void* ctx, void* file_state, uint64_t* out_size)
{
  (void)ctx;
  vfs_file_state_t* file = (vfs_file_state_t*)file_state;
  return ra8_fs_size(file->native, out_size);
}

/* see header for full description */
RA8_INTERNAL static ra8_err_t internal_close(void* ctx, void* file_state)
{
  (void)ctx;
  vfs_file_state_t* file   = (vfs_file_state_t*)file_state;
  const ra8_err_t   result = ra8_fs_close(file->native);
  file->native             = nullptr;
  return result;
}

/* see header for full description */
RA8_INTERNAL static ra8_err_t internal_copy_path(char* out, const char* path)
{
  for (uint16_t i = 0U; i < (uint16_t)k_fw_fs_path_cap; ++i) {
    out[i] = path[i];
    if (path[i] == '\0') {
      return k_ra8_ok;
    }
  }
  return k_ra8_err_invalid_size;
}

/**
 * @brief Render a fixed-width six-digit hexadecimal transaction id.
 * @details Emits uppercase nibbles from most to least significant without a NUL;
 *          callers place the exact field inside an already bounded stage name.
 * @param[out] out Six-byte hexadecimal field.
 * @param[in] value Transaction identifier whose low 24 bits are rendered.
 * @pre @p out addresses ::k_vfs_stage_hex_digits writable bytes.
 * @pre The destination does not overlap read-only digit storage.
 * @post Exactly six uppercase hexadecimal characters are written.
 * @post Bytes outside the six-byte field are unchanged.
 * @note Pure apart from caller output and thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_hex6(char out[k_vfs_stage_hex_digits], uint32_t value)
{
  static const char digits[] = "0123456789ABCDEF";
  for (uint8_t i = 0U; i < (uint8_t)k_vfs_stage_hex_digits; ++i) {
    const uint8_t shift =
      (uint8_t)(((uint8_t)k_vfs_hex_last_digit - i) * (uint8_t)k_vfs_hex_nibble_bits);
    out[i] = digits[(value >> shift) & (uint32_t)k_vfs_hex_nibble_mask];
  }
}

/* see header for full description */
RA8_INTERNAL static ra8_err_t internal_stage_path(const char* destination, uint32_t id, char* out)
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
  const uint16_t stage_length = (uint16_t)(last_slash + 1U + k_vfs_stage_leaf_bytes);
  if (stage_length >= (uint16_t)k_fw_fs_path_cap) {
    return k_ra8_err_invalid_size;
  }
  for (uint16_t i = 0U; i <= last_slash; ++i) {
    out[i] = destination[i];
  }
  uint16_t cursor = (uint16_t)(last_slash + 1U);
  out[cursor++]   = 'T';
  out[cursor++]   = 'X';
  internal_hex6(&out[cursor], id & k_vfs_transaction_id_mask);
  cursor        = (uint16_t)(cursor + k_vfs_stage_hex_digits);
  out[cursor++] = '.';
  out[cursor++] = 'T';
  out[cursor++] = 'M';
  out[cursor++] = 'P';
  out[cursor]   = '\0';
  return k_ra8_ok;
}

/**
 * @brief Open an unused VFS staging file after a bounded collision search.
 * @details Tries monotonically numbered sibling names, stats before open, and
 *          handles a create race by continuing only on an exists result.
 * @param[in,out] state Bound adapter state and transaction-id source.
 * @param[in,out] txn Transaction state receiving stage path and writer.
 * @return Bounded stage-open status.
 * @retval k_ra8_ok A new staging writer is open and tracked.
 * @retval k_ra8_err_no_mem Every bounded candidate collided.
 * @retval k_ra8_err_* Naming, stat, or open failure.
 * @pre @p txn contains a validated terminated destination path.
 * @pre Adapter state and VFS mount remain live for all attempts.
 * @post Success sets both writer_open and stage_exists.
 * @post At most ::k_vfs_stage_attempts candidates are examined.
 * @note Not thread-safe; advances the adapter transaction id.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_stage_open(fw_fs_ra8_vfs_state_t*   state,
                                                  vfs_transaction_state_t* txn)
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

/* see header for full description */
RA8_INTERNAL static ra8_err_t internal_txn_begin(void*                      ctx,
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

/* see header for full description */
RA8_INTERNAL static ra8_err_t internal_txn_write(void*          ctx,
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

/* see header for full description */
RA8_INTERNAL static ra8_err_t internal_txn_seek(void* ctx, void* transaction_state, uint64_t offset)
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

/* see header for full description */
RA8_INTERNAL static ra8_err_t internal_txn_validate(void*               ctx,
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

/* see header for full description */
RA8_INTERNAL static ra8_err_t
internal_txn_commit(void* ctx, void* transaction_state, bool* out_published)
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

/* see header for full description */
RA8_INTERNAL static ra8_err_t internal_txn_abort(void* ctx, void* transaction_state)
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
  .stat      = internal_stat,
  .listdir   = internal_listdir,
  .dir_open  = internal_dir_open,
  .dir_next  = internal_dir_next,
  .dir_close = internal_dir_close,
  .mkdir     = internal_mkdir,
  .unlink    = internal_unlink,
  .rmdir     = internal_rmdir,
  .rename    = internal_rename,
  .space     = internal_space,
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

/* see header for full description */
RA8_INTERNAL static ra8_err_t internal_mount_name(fw_fs_ra8_vfs_state_t* state, const char* name)
{
  uint16_t length = 0U;
  while (length < (uint16_t)k_ra8_io_vfs_name_max) {
    const char value = name[length];
    if (value == '\0') {
      break;
    }
    if (value == ':') {
      return k_ra8_err_invalid_arg;
    }
    if (value == '/') {
      return k_ra8_err_invalid_arg;
    }
    ++length;
  }
  if (length == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (length >= (uint16_t)k_ra8_io_vfs_name_max) {
    return k_ra8_err_invalid_arg;
  }
  for (uint16_t i = 0U; i <= length; ++i) {
    state->mount_name[i] = name[i];
  }
  return k_ra8_ok;
}

/**
 * @brief Query the named format and compose aligned adapter cursor requirements.
 * @details Resolves the registered format through VFS, retains its exact inner
 *          cursor facts, and adds checked space for the adapter wrapper.
 * @param[in,out] state Adapter with a retained registered mount name.
 * @param[out] out_bytes Complete outer workspace bytes.
 * @param[out] out_align Complete outer workspace alignment.
 * @param[out] out_max_open Format-reported concurrency ceiling.
 * @return VFS resolution, capability, or overflow status.
 * @retval k_ra8_ok All returned and cached requirements are coherent.
 * @retval k_ra8_err_* VFS capability, path, or checked-size failure.
 * @pre Required pointers are non-NULL and the named mount is registered.
 * @pre @p state owns writable adapter path scratch.
 * @post Success caches the inner format requirements in @p state.
 * @post Failure does not publish partial requirement outputs.
 * @note This preserves the format-neutral #159 dispatch seam.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_dir_requirements(fw_fs_ra8_vfs_state_t* state,
                                                        uint32_t*              out_bytes,
                                                        uint8_t*               out_align,
                                                        uint16_t*              out_max_open)
{
  ra8_err_t err = internal_full_path(state, "/", state->path_a);
  if (err != k_ra8_ok) {
    return err;
  }
  uint32_t native_bytes = 0U;
  uint8_t  native_align = 0U;
  err = ra8_io_vfs_dir_requirements(state->path_a, &native_bytes, &native_align, out_max_open);
  if (err != k_ra8_ok) {
    return err;
  }
  const uint8_t  outer_align = (native_align > (uint8_t)_Alignof(vfs_directory_state_t))
                                 ? native_align
                                 : (uint8_t)_Alignof(vfs_directory_state_t);
  const uint64_t total =
    (uint64_t)sizeof(vfs_directory_state_t) + (uint64_t)native_align - 1U + (uint64_t)native_bytes;
  if (total > (uint64_t)UINT32_MAX) {
    return k_ra8_err_invalid_size;
  }
  state->directory_workspace_bytes = native_bytes;
  state->directory_workspace_align = native_align;
  state->max_open_directories      = *out_max_open;
  *out_bytes                       = (uint32_t)total;
  *out_align                       = outer_align;
  return k_ra8_ok;
}

/**
 * @brief Compose the portable capability record for one mounted VFS format.
 * @details Combines fixed adapter services with format-dependent file/name
 *          limits and the previously queried directory workspace contract.
 * @param[in] cfg Validated adapter configuration.
 * @param[in] directory_bytes Complete directory workspace requirement.
 * @param[in] directory_alignment Required directory workspace alignment.
 * @param[in] max_directories Format-reported concurrent cursor ceiling.
 * @return Fully initialized portable filesystem capabilities.
 * @retval fw_fs_caps_t Capability record matching the selected mount.
 * @pre @p cfg and its mounted format are non-NULL and initialized.
 * @pre Directory requirement arguments came from ::internal_dir_requirements.
 * @post The returned flags describe every adapter service exposed below.
 * @post No caller, mount, or adapter state is modified.
 * @note Removable-media capability follows the caller configuration exactly.
 * @since 0.1.0
 */
RA8_INTERNAL static fw_fs_caps_t internal_capabilities(const fw_fs_ra8_vfs_cfg_t* cfg,
                                                       uint32_t                   directory_bytes,
                                                       uint8_t  directory_alignment,
                                                       uint16_t max_directories)
{
  fw_fs_caps_t caps = {
    .max_file_bytes = (cfg->mount->type == k_ra8_fs_type_exfat)
                        ? UINT64_MAX
                        : (uint64_t)k_ra8_fs_fat_max_file_bytes,
    .flags          = (uint32_t)k_fw_fs_cap_namespace | (uint32_t)k_fw_fs_cap_stream |
                      (uint32_t)k_fw_fs_cap_space_query | (uint32_t)k_fw_fs_cap_same_volume_rename |
                      (uint32_t)k_fw_fs_cap_atomic_noreplace | (uint32_t)k_fw_fs_cap_transactions |
                      (uint32_t)k_fw_fs_cap_rejects_symlink_walk,
    .file_workspace_bytes        = sizeof(vfs_file_state_t),
    .directory_workspace_bytes   = directory_bytes,
    .transaction_workspace_bytes = sizeof(vfs_transaction_state_t),
    .path_max_bytes              = (uint16_t)k_fw_fs_path_cap,
    .name_max_bytes       = (cfg->mount->type == k_ra8_fs_type_exfat) ? k_vfs_exfat_name_max_bytes
                                                                      : k_vfs_fat_name_max_bytes,
    .max_open_files       = (uint16_t)k_ra8_fs_max_files,
    .max_open_directories = max_directories,
    .file_workspace_align = (uint8_t)_Alignof(vfs_file_state_t),
    .directory_workspace_align   = directory_alignment,
    .transaction_workspace_align = (uint8_t)_Alignof(vfs_transaction_state_t),
  };
  caps.flags |= (uint32_t)k_fw_fs_cap_created_time | (uint32_t)k_fw_fs_cap_modified_time |
                (uint32_t)k_fw_fs_cap_accessed_time;
  if (cfg->removable_media) {
    caps.flags |= (uint32_t)k_fw_fs_cap_removable_media;
  }
  return caps;
}

ra8_err_t
fw_fs_ra8_vfs_init(fw_fs_t* out, fw_fs_ra8_vfs_state_t* state, const fw_fs_ra8_vfs_cfg_t* cfg)
{
  if (out == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (state == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (cfg == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (cfg->mount_name == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (cfg->mount == nullptr) {
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
  state->mount                        = cfg->mount;
  state->removable_media              = cfg->removable_media;
  uint32_t        directory_bytes     = 0U;
  uint16_t        max_directories     = 0U;
  uint8_t         directory_alignment = 0U;
  const ra8_err_t directory_requirements =
    internal_dir_requirements(state, &directory_bytes, &directory_alignment, &max_directories);
  if (directory_requirements != k_ra8_ok) {
    return directory_requirements;
  }
  const fw_fs_caps_t caps =
    internal_capabilities(cfg, directory_bytes, directory_alignment, max_directories);
  return fw_fs_bind(out, &s_namespace_iface, &s_stream_iface, &s_transaction_iface, state, &caps);
}
