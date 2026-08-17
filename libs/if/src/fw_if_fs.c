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
#include "ra8_attributes.h"
#include "ra8_err.h"

/** @brief ASCII byte boundaries used by portable path validation. */
typedef enum : uint8_t {
  k_fw_fs_ascii_space  = 0x20U, /**< First non-control ASCII byte. */
  k_fw_fs_ascii_delete = 0x7FU, /**< DEL control byte.             */
} fw_fs_ascii_byte_t;

/**
 * @brief Test whether an unsigned value is a non-zero power of two.
 * @details Uses the one-bit identity `value & (value - 1)` after handling zero,
 *          avoiding loops and making the alignment-contract check bounded.
 * @param[in] value Candidate unsigned value.
 * @return Whether exactly one bit is set.
 * @retval true @p value is a non-zero power of two.
 * @retval false @p value is zero or contains more than one set bit.
 * @pre @p value is an ordinary 32-bit value; no external state is required.
 * @pre Unsigned subtraction and bitwise operations use standard C semantics.
 * @post No memory or external state is modified.
 * @post The result depends only on @p value.
 * @note Pure and thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL static bool internal_power_of_two(uint32_t value)
{
  if (value == 0U) {
    return false;
  }
  return (value & (value - 1U)) == 0U;
}

/**
 * @brief Validate a workspace against a backend byte/alignment contract.
 * @details Rejects absent or undersized storage, invalid alignment metadata,
 *          and bases that do not satisfy the advertised power-of-two boundary.
 * @param[in,out] workspace Caller-owned workspace to validate.
 * @param[in] bytes Accessible bytes beginning at @p workspace.
 * @param[in] need Minimum backend workspace size in bytes.
 * @param[in] align Required power-of-two byte alignment.
 * @return Workspace validation status.
 * @retval k_ra8_ok The storage meets both size and alignment requirements.
 * @retval k_ra8_err_null_ptr @p workspace is NULL.
 * @retval k_ra8_err_no_mem @p bytes is smaller than @p need.
 * @retval k_ra8_err_invalid_state @p align is not a non-zero power of two.
 * @retval k_ra8_err_invalid_arg The workspace base is misaligned.
 * @pre @p bytes truthfully describes the accessible caller-owned span.
 * @pre @p need and @p align came from the immutable bound capability record.
 * @post The workspace contents are unchanged.
 * @post Success proves the backend may place its state in the supplied span.
 * @note Pure and thread-safe for immutable capability metadata.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_workspace(void* workspace, uint32_t bytes, uint32_t need, uint8_t align)
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

/**
 * @brief Validate a namespace facade before dispatch.
 * @details Requires both a facade object and the immutable namespace vtable
 *          installed by ::fw_fs_bind before any backend operation is called.
 * @param[in] names Namespace facade to inspect.
 * @return Facade validation status.
 * @retval k_ra8_ok The facade can dispatch namespace operations.
 * @retval k_ra8_err_null_ptr @p names is NULL.
 * @retval k_ra8_err_not_initialized The facade has no bound interface.
 * @pre The caller does not concurrently mutate @p names.
 * @pre Any non-NULL interface pointer remains valid for the call duration.
 * @post No facade or backend state is modified.
 * @post Success establishes a non-NULL dispatch interface.
 * @note Thread-safe when binding lifetime is externally synchronized.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_names(const fw_fs_namespace_t* names)
{
  if (names == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (names->iface == nullptr) {
    return k_ra8_err_not_initialized;
  }
  return k_ra8_ok;
}

/**
 * @brief Validate an open file facade before dispatch.
 * @details Checks the explicit lifecycle bit before accepting the stored stream
 *          interface, so closed and partially initialized handles fail closed.
 * @param[in] file File facade to inspect.
 * @return File-handle validation status.
 * @retval k_ra8_ok The handle is open and has a dispatch interface.
 * @retval k_ra8_err_null_ptr @p file is NULL.
 * @retval k_ra8_err_invalid_state The handle is not open.
 * @retval k_ra8_err_not_initialized The open handle lacks an interface.
 * @pre The caller does not concurrently open or close @p file.
 * @pre Any non-NULL interface pointer remains valid for the call duration.
 * @post No file or backend state is modified.
 * @post Success establishes that stream dispatch is safe to attempt.
 * @note Thread-safe only with external handle-lifecycle synchronization.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_file(const fw_fs_file_t* file)
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

/**
 * @brief Validate an active transaction facade before dispatch.
 * @details Enforces the transaction lifecycle before accepting the bound
 *          transaction vtable, preventing writes through consumed handles.
 * @param[in] transaction Transaction facade to inspect.
 * @return Transaction validation status.
 * @retval k_ra8_ok The transaction is active and dispatchable.
 * @retval k_ra8_err_null_ptr @p transaction is NULL.
 * @retval k_ra8_err_invalid_state The transaction is inactive.
 * @retval k_ra8_err_not_initialized The active facade lacks an interface.
 * @pre The caller does not concurrently commit or abort @p transaction.
 * @pre Any non-NULL interface pointer remains valid for the call duration.
 * @post No transaction or backend state is modified.
 * @post Success establishes that transaction dispatch is safe to attempt.
 * @note Thread-safe only with external transaction-lifecycle synchronization.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_transaction(const fw_fs_transaction_t* transaction)
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

/**
 * @brief Validate one completed portable path component.
 * @details Rejects empty components and the traversal tokens `.` and `..`;
 *          other byte and length rules are enforced by ::fw_fs_path_validate.
 * @param[in] path Canonical path buffer containing the component.
 * @param[in] start Byte offset of the component's first character.
 * @param[in] length Component length in bytes.
 * @return Component validation status.
 * @retval k_ra8_ok The component is non-empty and is not a traversal token.
 * @retval k_ra8_err_invalid_arg @p length is zero.
 * @retval k_ra8_err_access_denied The component is `.` or `..`.
 * @pre @p path addresses at least `start + length` readable bytes.
 * @pre @p start and @p length were derived without integer wrap.
 * @post The path buffer is unchanged.
 * @post Success permits the outer validator to continue with the next byte.
 * @note Pure and thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_component(const char* path, uint16_t start, uint16_t length)
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

/**
 * @brief Walk a validated non-root path byte-by-byte, checking every component.
 * @details Splits @p path on `/` boundaries, rejects control characters, DEL,
 *          `:`, and `\`, enforces the per-component length cap, and delegates
 *          traversal-token and empty-component rejection to ::internal_component
 *          at each boundary and at the terminating NUL.
 * @param[in] caps Bound capability limits (path and name length caps).
 * @param[in] path NUL-terminated candidate path; `path[0] == '/'` and
 *            `path[1] != '\0'` are already established by the caller.
 * @return Path validation status.
 * @retval k_ra8_ok Every component is well-formed and within its length cap.
 * @retval k_ra8_err_access_denied A component is a traversal token, or the
 *         path contains `:` or `\`.
 * @retval k_ra8_err_invalid_arg A control character (below space) appears.
 * @retval k_ra8_err_invalid_size A component exceeds `caps->name_max_bytes`,
 *         or no terminating NUL was found within `caps->path_max_bytes`.
 * @pre @p caps and @p path are non-NULL.
 * @pre @p path is NUL-terminated within `caps->path_max_bytes` bytes, or this
 *      returns k_ra8_err_invalid_size.
 * @post The path buffer is unchanged.
 * @note Pure and thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_fw_fs_scan_components(const fw_fs_caps_t* caps,
                                                             const char*         path)
{
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
    if (value < (unsigned char)k_fw_fs_ascii_space) {
      return k_ra8_err_invalid_arg;
    }
    if (value == (unsigned char)k_fw_fs_ascii_delete) {
      return k_ra8_err_invalid_arg;
    }
    ++component_len;
    if (component_len > caps->name_max_bytes) {
      return k_ra8_err_invalid_size;
    }
  }
  return k_ra8_err_invalid_size;
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
  return internal_fw_fs_scan_components(caps, path);
}

/**
 * @brief Validate all mandatory backend operations before binding them.
 * @details Requires the complete namespace and stream contracts, then either
 *          no transaction interface or a complete begin/write/seek/validate/
 *          commit/abort transaction contract.
 * @param[in] names Candidate namespace-operation table.
 * @param[in] streams Candidate stream-operation table.
 * @param[in] transactions Optional candidate transaction-operation table.
 * @return Interface-table validation status.
 * @retval k_ra8_ok Every required function pointer is present.
 * @retval k_ra8_err_invalid_arg A mandatory operation pointer is NULL.
 * @pre @p names and @p streams are non-NULL readable objects.
 * @pre @p transactions is NULL or addresses a readable interface object.
 * @post No interface table or backend state is modified.
 * @post Success proves later guarded dispatch cannot call a missing operation.
 * @note Pure and thread-safe for immutable interface tables.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_interfaces(const fw_fs_namespace_iface_t*   names,
                                                  const fw_fs_stream_iface_t*      streams,
                                                  const fw_fs_transaction_iface_t* transactions)
{
  if (names->stat == nullptr || names->listdir == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if (names->dir_open == nullptr || names->dir_next == nullptr || names->dir_close == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if (names->mkdir == nullptr || names->unlink == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if (names->rmdir == nullptr || names->rename == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if (streams->open == nullptr || streams->read == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if (streams->write == nullptr || streams->close == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if (streams->seek == nullptr || streams->tell == nullptr || streams->size == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if (transactions == nullptr) {
    return k_ra8_ok;
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

/**
 * @brief Validate capability flags and workspace alignments before a bind.
 * @details Requires the mandatory namespace and stream capability bits,
 *          cross-checks every optional flag against the interface function
 *          pointer or companion flag it depends on, and requires every
 *          workspace alignment to be a power of two.
 * @param[in] namespace_iface Candidate namespace-operation table.
 * @param[in] stream_iface Candidate stream-operation table.
 * @param[in] transaction_iface Optional candidate transaction-operation table.
 * @param[in] caps Candidate capability and workspace-sizing descriptor.
 * @return Capability validation status.
 * @retval k_ra8_ok Every capability flag and alignment is internally consistent.
 * @retval k_ra8_err_invalid_arg A required flag, interface pointer, companion
 *         flag, or alignment is missing or not a power of two.
 * @pre @p namespace_iface, @p stream_iface, and @p caps are non-NULL.
 * @pre @p transaction_iface is NULL or addresses a readable interface object.
 * @post No interface table or backend state is modified.
 * @note Pure and thread-safe for immutable interface and capability tables.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_fw_fs_caps_validate(const fw_fs_namespace_iface_t*   namespace_iface,
                             const fw_fs_stream_iface_t*      stream_iface,
                             const fw_fs_transaction_iface_t* transaction_iface,
                             const fw_fs_caps_t*              caps)
{
  const uint32_t required = (uint32_t)k_fw_fs_cap_namespace | (uint32_t)k_fw_fs_cap_stream;
  if ((caps->flags & required) != required) {
    return k_ra8_err_invalid_arg;
  }
  if (((caps->flags & (uint32_t)k_fw_fs_cap_space_query) != 0U) &&
      (namespace_iface->space == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  if (((caps->flags & (uint32_t)k_fw_fs_cap_file_sync) != 0U) && (stream_iface->sync == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  if (((caps->flags & (uint32_t)k_fw_fs_cap_durable_file_sync) != 0U) &&
      ((caps->flags & (uint32_t)k_fw_fs_cap_file_sync) == 0U)) {
    return k_ra8_err_invalid_arg;
  }
  if (((caps->flags & (uint32_t)k_fw_fs_cap_transactions) != 0U) &&
      (transaction_iface == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  if (!internal_power_of_two(caps->file_workspace_align)) {
    return k_ra8_err_invalid_arg;
  }
  if ((caps->directory_workspace_bytes == 0U) || (caps->max_open_directories == 0U) ||
      !internal_power_of_two(caps->directory_workspace_align)) {
    return k_ra8_err_invalid_arg;
  }
  if (!internal_power_of_two(caps->transaction_workspace_align)) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
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
  if (ctx == nullptr || caps == nullptr) {
    return k_ra8_err_null_ptr;
  }
  const ra8_err_t interfaces =
    internal_interfaces(namespace_iface, stream_iface, transaction_iface);
  if (interfaces != k_ra8_ok) {
    return interfaces;
  }
  const ra8_err_t caps_status =
    internal_fw_fs_caps_validate(namespace_iface, stream_iface, transaction_iface, caps);
  if (caps_status != k_ra8_ok) {
    return caps_status;
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
  const ra8_err_t result = names->iface->stat(names->ctx, path, out);
  if (result == k_ra8_ok) {
    const bool type_invalid = (uint32_t)out->type > (uint32_t)k_fw_fs_node_other;
    const bool missing_invalid =
      !out->exists && ((out->type != k_fw_fs_node_none) || (out->size_bytes != 0U));
    const bool present_invalid   = out->exists && (out->type == k_fw_fs_node_none);
    const bool directory_invalid = (out->type == k_fw_fs_node_directory) && (out->size_bytes != 0U);
    if (type_invalid || missing_invalid || present_invalid || directory_invalid) {
      (void)memset(out, 0, sizeof(*out));
      return k_ra8_err_invalid_state;
    }
  }
  return result;
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
  const ra8_err_t result =
    names->iface
      ->listdir(names->ctx, path, max_entries, callback, callback_ctx, out_count, out_complete);
  if (*out_count > max_entries) {
    *out_count    = 0U;
    *out_complete = false;
    return k_ra8_err_invalid_state;
  }
  return result;
}

/** @brief Common one-path namespace dispatch. */
RA8_INTERNAL static ra8_err_t internal_name_op(const fw_fs_namespace_t* names,
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
  (void)memset(out, 0, sizeof(*out));
  const ra8_err_t result = names->iface->space(names->ctx, out);
  if ((result == k_ra8_ok) &&
      ((out->free_bytes > out->total_bytes) || (out->used_bytes > out->total_bytes))) {
    (void)memset(out, 0, sizeof(*out));
    return k_ra8_err_invalid_state;
  }
  return result;
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
  *out_read              = 0U;
  const ra8_err_t result = file->iface->read(file->ctx, file->state, dst, cap, out_read);
  if (*out_read > cap) {
    *out_read = 0U;
    return k_ra8_err_invalid_state;
  }
  return result;
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
  *out_written           = 0U;
  const ra8_err_t result = file->iface->write(file->ctx, file->state, source, length, out_written);
  if (*out_written > length) {
    *out_written = 0U;
    return k_ra8_err_invalid_state;
  }
  return result;
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
  *out_offset = 0U;
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
  *out_size = 0U;
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

/**
 * @brief Validate a transaction port, in-flight state, and policy before begin.
 * @details Requires a bound transaction interface, the transactions capability
 *          bit, an idle transaction slot, an in-range policy, and the specific
 *          atomic-replace or atomic-noreplace capability bit the requested
 *          policy needs.
 * @param[in] port Candidate transaction port (interface, ctx, capabilities).
 * @param[in] transaction Candidate transaction slot to begin into.
 * @param[in] policy Requested commit policy.
 * @return Preamble validation status.
 * @retval k_ra8_ok The port, slot, and policy are ready for begin.
 * @retval k_ra8_err_not_initialized Transactions are capable but not bound.
 * @retval k_ra8_err_not_supported Transactions, or the requested policy's
 *         atomic mode, are not offered by this port.
 * @retval k_ra8_err_busy @p transaction already has an active transaction.
 * @retval k_ra8_err_invalid_arg @p policy is out of range.
 * @pre @p port and @p transaction are non-NULL.
 * @post No transaction, workspace, or backend state is modified.
 * @note Pure and thread-safe for immutable port and policy inputs.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_fw_fs_transaction_preamble(const fw_fs_transaction_port_t* port,
                                    const fw_fs_transaction_t*      transaction,
                                    fw_fs_transaction_policy_t      policy)
{
  if (port->iface == nullptr) {
    return ((port->caps.flags & (uint32_t)k_fw_fs_cap_transactions) == 0U)
             ? k_ra8_err_not_supported
             : k_ra8_err_not_initialized;
  }
  if ((port->caps.flags & (uint32_t)k_fw_fs_cap_transactions) == 0U) {
    return k_ra8_err_not_supported;
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
  return k_ra8_ok;
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
  const ra8_err_t preamble = internal_fw_fs_transaction_preamble(port, transaction, policy);
  if (preamble != k_ra8_ok) {
    return preamble;
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
  const ra8_err_t result =
    transaction->iface->write(transaction->ctx, transaction->state, source, length, out_written);
  if (*out_written > length) {
    *out_written = 0U;
    return k_ra8_err_invalid_state;
  }
  return result;
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
  if ((result == k_ra8_ok) && !*out_published) {
    return k_ra8_err_invalid_state;
  }
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
