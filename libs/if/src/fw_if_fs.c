/**
 * @file fw_if_fs.c
 * @brief Guarded dispatch for the architecture-neutral filesystem ports.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 2 / Interface] {World: Any}
 *
 * @details Every public call validates lifecycle, path, workspace size, and
 * capability before entering a concrete adapter. No backend can accidentally
 * receive a traversal path or be asked to make a guarantee it did not report.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "fw_if_fs.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "fw_if_fs_backend.h"
#include "fw_if_fs_types.h"
#include "ra8_err.h"

/** @brief Return true when `value` is a non-zero power of two. */
static bool internal_power_of_two(uint32_t value)
{
  if (value == 0U) {
    return false;
  }
  return (value & (value - 1U)) == 0U;
}

/** @brief Validate a workspace against a backend's byte/alignment contract. */
static ra8_err_t internal_workspace(void* workspace, uint32_t bytes, uint32_t need, uint8_t align)
{
  if (workspace == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (bytes < need) {
    return k_ra8_err_no_mem;
  }
  if (!internal_power_of_two((uint32_t)align)) {
    return k_ra8_err_invalid_state;
  }
  if (((uintptr_t)workspace % (uintptr_t)align) != 0U) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/** @brief Validate a namespace facade before dispatch. */
static ra8_err_t internal_names(const fw_fs_namespace_t* names)
{
  if (names == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (names->iface == nullptr) {
    return k_ra8_err_not_initialized;
  }
  return k_ra8_ok;
}

/** @brief Validate an open file facade before dispatch. */
static ra8_err_t internal_file(const fw_fs_file_t* file)
{
  if (file == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (!file->is_open) {
    return k_ra8_err_invalid_state;
  }
  if (file->iface == nullptr) {
    return k_ra8_err_not_initialized;
  }
  return k_ra8_ok;
}

/** @brief Validate an active transaction facade before dispatch. */
static ra8_err_t internal_transaction(const fw_fs_transaction_t* transaction)
{
  if (transaction == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (!transaction->active) {
    return k_ra8_err_invalid_state;
  }
  if (transaction->iface == nullptr) {
    return k_ra8_err_not_initialized;
  }
  return k_ra8_ok;
}

/** @brief Validate one completed path component (`.` and `..` are forbidden). */
static ra8_err_t internal_component(const char* path, uint16_t start, uint16_t length)
{
  if (length == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (length == 1U) {
    if (path[start] == '.') {
      return k_ra8_err_access_denied;
    }
  }
  if (length == 2U) {
    if (path[start] == '.') {
      if (path[(uint16_t)(start + 1U)] == '.') {
        return k_ra8_err_access_denied;
      }
    }
  }
  return k_ra8_ok;
}

ra8_err_t fw_fs_path_validate(const fw_fs_caps_t* caps, const char* path)
{
  if (caps == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (path == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (caps->path_max_bytes < 2U) {
    return k_ra8_err_invalid_state;
  }
  if (caps->path_max_bytes > (uint16_t)k_fw_fs_path_cap) {
    return k_ra8_err_invalid_state;
  }
  if (caps->name_max_bytes == 0U) {
    return k_ra8_err_invalid_state;
  }
  if (path[0] != '/') {
    return k_ra8_err_invalid_arg;
  }
  if (path[1] == '\0') {
    return k_ra8_ok;
  }

  uint16_t component_start = 1U;
  uint16_t component_len   = 0U;
  for (uint16_t i = 1U; i < caps->path_max_bytes; ++i) {
    const unsigned char value = (unsigned char)path[i];
    if (value == (unsigned char)'\0') {
      return internal_component(path, component_start, component_len);
    }
    if (value == (unsigned char)'/') {
      const ra8_err_t component = internal_component(path, component_start, component_len);
      if (component != k_ra8_ok) {
        return component;
      }
      component_start = (uint16_t)(i + 1U);
      component_len   = 0U;
      continue;
    }
    if (value == (unsigned char)':') {
      return k_ra8_err_access_denied;
    }
    if (value == (unsigned char)'\\') {
      return k_ra8_err_access_denied;
    }
    if (value < 0x20U) {
      return k_ra8_err_invalid_arg;
    }
    if (value == 0x7FU) {
      return k_ra8_err_invalid_arg;
    }
    ++component_len;
    if (component_len > caps->name_max_bytes) {
      return k_ra8_err_invalid_size;
    }
  }
  return k_ra8_err_invalid_size;
}

/** @brief Validate all mandatory backend operations before binding them. */
static ra8_err_t internal_interfaces(const fw_fs_namespace_iface_t*   names,
                                     const fw_fs_stream_iface_t*      streams,
                                     const fw_fs_transaction_iface_t* transactions)
{
  if (names->stat == nullptr || names->listdir == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if (names->mkdir == nullptr || names->unlink == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if (names->rmdir == nullptr || names->rename == nullptr || names->space == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if (streams->open == nullptr || streams->read == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if (streams->write == nullptr || streams->close == nullptr || streams->sync == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if (streams->seek == nullptr || streams->tell == nullptr || streams->size == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if (transactions->begin == nullptr || transactions->write == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if (transactions->seek == nullptr || transactions->validate == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  return (transactions->commit == nullptr || transactions->abort == nullptr) ? k_ra8_err_invalid_arg
                                                                             : k_ra8_ok;
}

ra8_err_t fw_fs_bind(fw_fs_t*                         out,
                     const fw_fs_namespace_iface_t*   namespace_iface,
                     const fw_fs_stream_iface_t*      stream_iface,
                     const fw_fs_transaction_iface_t* transaction_iface,
                     void*                            ctx,
                     const fw_fs_caps_t*              caps)
{
  if (out == nullptr || namespace_iface == nullptr || stream_iface == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (transaction_iface == nullptr || ctx == nullptr || caps == nullptr) {
    return k_ra8_err_null_ptr;
  }
  const ra8_err_t interfaces =
    internal_interfaces(namespace_iface, stream_iface, transaction_iface);
  if (interfaces != k_ra8_ok) {
    return interfaces;
  }
  if (!internal_power_of_two(caps->file_workspace_align)) {
    return k_ra8_err_invalid_arg;
  }
  if (!internal_power_of_two(caps->transaction_workspace_align)) {
    return k_ra8_err_invalid_arg;
  }
  const ra8_err_t root_path = fw_fs_path_validate(caps, "/");
  if (root_path != k_ra8_ok) {
    return root_path;
  }

  out->caps               = *caps;
  out->names.iface        = namespace_iface;
  out->names.ctx          = ctx;
  out->names.caps         = *caps;
  out->streams.iface      = stream_iface;
  out->streams.ctx        = ctx;
  out->streams.caps       = *caps;
  out->transactions.iface = transaction_iface;
  out->transactions.ctx   = ctx;
  out->transactions.caps  = *caps;
  return k_ra8_ok;
}

ra8_err_t fw_fs_get_caps(const fw_fs_t* fs, fw_fs_caps_t* out)
{
  if (fs == nullptr || out == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (fs->names.iface == nullptr) {
    return k_ra8_err_not_initialized;
  }
  *out = fs->caps;
  return k_ra8_ok;
}

ra8_err_t fw_fs_stat(const fw_fs_namespace_t* names, const char* path, fw_fs_stat_t* out)
{
  const ra8_err_t valid = internal_names(names);
  if (valid != k_ra8_ok) {
    return valid;
  }
  if (out == nullptr) {
    return k_ra8_err_null_ptr;
  }
  const ra8_err_t path_err = fw_fs_path_validate(&names->caps, path);
  if (path_err != k_ra8_ok) {
    return path_err;
  }
  (void)memset(out, 0, sizeof(*out));
  return names->iface->stat(names->ctx, path, out);
}

ra8_err_t fw_fs_listdir(const fw_fs_namespace_t* names,
                        const char*              path,
                        uint32_t                 max_entries,
                        fw_fs_list_fn_t          callback,
                        void*                    callback_ctx,
                        uint32_t*                out_count,
                        bool*                    out_complete)
{
  const ra8_err_t valid = internal_names(names);
  if (valid != k_ra8_ok) {
    return valid;
  }
  if (callback == nullptr || out_count == nullptr || out_complete == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (max_entries == 0U) {
    return k_ra8_err_invalid_arg;
  }
  const ra8_err_t path_err = fw_fs_path_validate(&names->caps, path);
  if (path_err != k_ra8_ok) {
    return path_err;
  }
  *out_count    = 0U;
  *out_complete = false;
  return names->iface
    ->listdir(names->ctx, path, max_entries, callback, callback_ctx, out_count, out_complete);
}

/** @brief Common one-path namespace dispatch. */
static ra8_err_t internal_name_op(const fw_fs_namespace_t* names,
                                  const char*              path,
                                  ra8_err_t (*operation)(void*, const char*))
{
  const ra8_err_t valid = internal_names(names);
  if (valid != k_ra8_ok) {
    return valid;
  }
  if (operation == nullptr) {
    return k_ra8_err_not_supported;
  }
  const ra8_err_t path_err = fw_fs_path_validate(&names->caps, path);
  if (path_err != k_ra8_ok) {
    return path_err;
  }
  if (path[1] == '\0') {
    return k_ra8_err_access_denied;
  }
  return operation(names->ctx, path);
}

ra8_err_t fw_fs_mkdir(const fw_fs_namespace_t* names, const char* path)
{
  const ra8_err_t valid = internal_names(names);
  if (valid != k_ra8_ok) {
    return valid;
  }
  return internal_name_op(names, path, names->iface->mkdir);
}

ra8_err_t fw_fs_unlink(const fw_fs_namespace_t* names, const char* path)
{
  const ra8_err_t valid = internal_names(names);
  if (valid != k_ra8_ok) {
    return valid;
  }
  return internal_name_op(names, path, names->iface->unlink);
}

ra8_err_t fw_fs_rmdir(const fw_fs_namespace_t* names, const char* path)
{
  const ra8_err_t valid = internal_names(names);
  if (valid != k_ra8_ok) {
    return valid;
  }
  return internal_name_op(names, path, names->iface->rmdir);
}

ra8_err_t fw_fs_rename(const fw_fs_namespace_t* names,
                       const char*              old_path,
                       const char*              new_path,
                       bool                     replace)
{
  const ra8_err_t valid = internal_names(names);
  if (valid != k_ra8_ok) {
    return valid;
  }
  if (replace) {
    if ((names->caps.flags & (uint32_t)k_fw_fs_cap_atomic_replace) == 0U) {
      return k_ra8_err_not_supported;
    }
  } else if ((names->caps.flags & (uint32_t)k_fw_fs_cap_atomic_noreplace) == 0U) {
    return k_ra8_err_not_supported;
  }
  const ra8_err_t old_err = fw_fs_path_validate(&names->caps, old_path);
  if (old_err != k_ra8_ok) {
    return old_err;
  }
  const ra8_err_t new_err = fw_fs_path_validate(&names->caps, new_path);
  if (new_err != k_ra8_ok) {
    return new_err;
  }
  if (old_path[1] == '\0' || new_path[1] == '\0') {
    return k_ra8_err_access_denied;
  }
  return names->iface->rename(names->ctx, old_path, new_path, replace);
}

ra8_err_t fw_fs_space(const fw_fs_namespace_t* names, fw_fs_space_t* out)
{
  const ra8_err_t valid = internal_names(names);
  if (valid != k_ra8_ok) {
    return valid;
  }
  if (out == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if ((names->caps.flags & (uint32_t)k_fw_fs_cap_space_query) == 0U) {
    return k_ra8_err_not_supported;
  }
  if (names->iface->space == nullptr) {
    return k_ra8_err_not_supported;
  }
  return names->iface->space(names->ctx, out);
}

ra8_err_t fw_fs_open(const fw_fs_stream_port_t* streams,
                     const char*                path,
                     fw_fs_open_mode_t          mode,
                     fw_fs_file_t*              file,
                     void*                      workspace,
                     uint32_t                   workspace_size)
{
  if (streams == nullptr || file == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (streams->iface == nullptr) {
    return k_ra8_err_not_initialized;
  }
  if (file->is_open) {
    return k_ra8_err_busy;
  }
  if ((uint32_t)mode > (uint32_t)k_fw_fs_open_create_new) {
    return k_ra8_err_invalid_arg;
  }
  if (mode == k_fw_fs_open_create_new) {
    if ((streams->caps.flags & (uint32_t)k_fw_fs_cap_create_exclusive) == 0U) {
      return k_ra8_err_not_supported;
    }
  }
  const ra8_err_t path_err = fw_fs_path_validate(&streams->caps, path);
  if (path_err != k_ra8_ok) {
    return path_err;
  }
  if (path[1] == '\0') {
    return k_ra8_err_invalid_arg;
  }
  const ra8_err_t work = internal_workspace(workspace,
                                            workspace_size,
                                            streams->caps.file_workspace_bytes,
                                            streams->caps.file_workspace_align);
  if (work != k_ra8_ok) {
    return work;
  }
  const ra8_err_t opened =
    streams->iface->open(streams->ctx, path, mode, workspace, workspace_size);
  if (opened != k_ra8_ok) {
    return opened;
  }
  file->iface       = streams->iface;
  file->ctx         = streams->ctx;
  file->state       = workspace;
  file->state_bytes = workspace_size;
  file->is_open     = true;
  return k_ra8_ok;
}

ra8_err_t fw_fs_read(fw_fs_file_t* file, uint8_t* dst, uint32_t cap, uint32_t* out_read)
{
  const ra8_err_t valid = internal_file(file);
  if (valid != k_ra8_ok) {
    return valid;
  }
  if (dst == nullptr || out_read == nullptr) {
    return k_ra8_err_null_ptr;
  }
  *out_read = 0U;
  return file->iface->read(file->ctx, file->state, dst, cap, out_read);
}

ra8_err_t
fw_fs_write(fw_fs_file_t* file, const uint8_t* source, uint32_t length, uint32_t* out_written)
{
  const ra8_err_t valid = internal_file(file);
  if (valid != k_ra8_ok) {
    return valid;
  }
  if (source == nullptr || out_written == nullptr) {
    return k_ra8_err_null_ptr;
  }
  *out_written = 0U;
  return file->iface->write(file->ctx, file->state, source, length, out_written);
}

ra8_err_t fw_fs_seek(fw_fs_file_t* file, uint64_t offset)
{
  const ra8_err_t valid = internal_file(file);
  if (valid != k_ra8_ok) {
    return valid;
  }
  return file->iface->seek(file->ctx, file->state, offset);
}

ra8_err_t fw_fs_tell(fw_fs_file_t* file, uint64_t* out_offset)
{
  const ra8_err_t valid = internal_file(file);
  if (valid != k_ra8_ok) {
    return valid;
  }
  if (out_offset == nullptr) {
    return k_ra8_err_null_ptr;
  }
  return file->iface->tell(file->ctx, file->state, out_offset);
}

ra8_err_t fw_fs_file_size(fw_fs_file_t* file, uint64_t* out_size)
{
  const ra8_err_t valid = internal_file(file);
  if (valid != k_ra8_ok) {
    return valid;
  }
  if (out_size == nullptr) {
    return k_ra8_err_null_ptr;
  }
  return file->iface->size(file->ctx, file->state, out_size);
}

ra8_err_t fw_fs_sync(fw_fs_file_t* file)
{
  const ra8_err_t valid = internal_file(file);
  if (valid != k_ra8_ok) {
    return valid;
  }
  if (file->iface->sync == nullptr) {
    return k_ra8_err_not_supported;
  }
  return file->iface->sync(file->ctx, file->state);
}

ra8_err_t fw_fs_close(fw_fs_file_t* file)
{
  const ra8_err_t valid = internal_file(file);
  if (valid != k_ra8_ok) {
    return valid;
  }
  const ra8_err_t closed = file->iface->close(file->ctx, file->state);
  file->iface            = nullptr;
  file->ctx              = nullptr;
  file->state            = nullptr;
  file->state_bytes      = 0U;
  file->is_open          = false;
  return closed;
}

ra8_err_t fw_fs_transaction_begin(const fw_fs_transaction_port_t* port,
                                  const char*                     destination,
                                  fw_fs_transaction_policy_t      policy,
                                  fw_fs_transaction_t*            transaction,
                                  void*                           workspace,
                                  uint32_t                        workspace_size)
{
  if (port == nullptr || transaction == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (port->iface == nullptr) {
    return k_ra8_err_not_initialized;
  }
  if (transaction->active) {
    return k_ra8_err_busy;
  }
  if ((uint32_t)policy > (uint32_t)k_fw_fs_txn_replace_atomic) {
    return k_ra8_err_invalid_arg;
  }
  if (policy == k_fw_fs_txn_replace_atomic) {
    if ((port->caps.flags & (uint32_t)k_fw_fs_cap_atomic_replace) == 0U) {
      return k_ra8_err_not_supported;
    }
  } else if ((port->caps.flags & (uint32_t)k_fw_fs_cap_atomic_noreplace) == 0U) {
    return k_ra8_err_not_supported;
  }
  const ra8_err_t path_err = fw_fs_path_validate(&port->caps, destination);
  if (path_err != k_ra8_ok) {
    return path_err;
  }
  if (destination[1] == '\0') {
    return k_ra8_err_invalid_arg;
  }
  const ra8_err_t work = internal_workspace(workspace,
                                            workspace_size,
                                            port->caps.transaction_workspace_bytes,
                                            port->caps.transaction_workspace_align);
  if (work != k_ra8_ok) {
    return work;
  }
  const ra8_err_t begun =
    port->iface->begin(port->ctx, workspace, workspace_size, destination, policy);
  if (begun != k_ra8_ok) {
    return begun;
  }
  transaction->iface       = port->iface;
  transaction->ctx         = port->ctx;
  transaction->state       = workspace;
  transaction->state_bytes = workspace_size;
  transaction->active      = true;
  transaction->validated   = false;
  return k_ra8_ok;
}

ra8_err_t fw_fs_transaction_write(fw_fs_transaction_t* transaction,
                                  const uint8_t*       source,
                                  uint32_t             length,
                                  uint32_t*            out_written)
{
  const ra8_err_t valid = internal_transaction(transaction);
  if (valid != k_ra8_ok) {
    return valid;
  }
  if (source == nullptr || out_written == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (transaction->validated) {
    return k_ra8_err_invalid_state;
  }
  *out_written = 0U;
  return transaction->iface->write(transaction->ctx,
                                   transaction->state,
                                   source,
                                   length,
                                   out_written);
}

ra8_err_t fw_fs_transaction_seek(fw_fs_transaction_t* transaction, uint64_t absolute_offset)
{
  const ra8_err_t valid = internal_transaction(transaction);
  if (valid != k_ra8_ok) {
    return valid;
  }
  if (transaction->validated) {
    return k_ra8_err_invalid_state;
  }
  return transaction->iface->seek(transaction->ctx, transaction->state, absolute_offset);
}

ra8_err_t fw_fs_transaction_validate(fw_fs_transaction_t* transaction,
                                     fw_fs_validate_fn_t  validator,
                                     void*                validator_ctx)
{
  const ra8_err_t valid = internal_transaction(transaction);
  if (valid != k_ra8_ok) {
    return valid;
  }
  if (validator == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (transaction->validated) {
    return k_ra8_err_invalid_state;
  }
  const ra8_err_t checked =
    transaction->iface->validate(transaction->ctx, transaction->state, validator, validator_ctx);
  if (checked == k_ra8_ok) {
    transaction->validated = true;
  }
  return checked;
}

ra8_err_t fw_fs_transaction_commit(fw_fs_transaction_t* transaction, bool* out_published)
{
  const ra8_err_t valid = internal_transaction(transaction);
  if (valid != k_ra8_ok) {
    return valid;
  }
  if (out_published == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (!transaction->validated) {
    return k_ra8_err_invalid_state;
  }
  *out_published = false;
  const ra8_err_t result =
    transaction->iface->commit(transaction->ctx, transaction->state, out_published);
  if (*out_published) {
    transaction->active    = false;
    transaction->validated = false;
  }
  return result;
}

ra8_err_t fw_fs_transaction_abort(fw_fs_transaction_t* transaction)
{
  const ra8_err_t valid = internal_transaction(transaction);
  if (valid != k_ra8_ok) {
    return valid;
  }
  const ra8_err_t result = transaction->iface->abort(transaction->ctx, transaction->state);
  if (result == k_ra8_ok) {
    transaction->active    = false;
    transaction->validated = false;
  }
  return result;
}
