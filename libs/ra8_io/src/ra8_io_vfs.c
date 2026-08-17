/**
 * @file ra8_io_vfs.c
 * @brief Named-mount VFS dispatch over pluggable filesystem-format operations.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Fixed mount and file tables route `"name:/path"` requests through the
 * selected ::ra8_io_fsfmt_ops_t. A legacy native ra8_fs mount and an
 * automatically probed foreign mount therefore share one namespace path.
 * All storage is static and all capability failures are explicit.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_io_vfs.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_io_fsfmt.h"
#include "ra8_io_vfs_internal.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_io_vfs";

/** @brief Publicly opaque stream facade, drawn from ::s_files. */
struct ra8_io_vfs_file {
  const ra8_io_fsfmt_t* format;      /**< Format owning `file_ctx`.       */
  void*                 file_ctx;    /**< Format-private open-file state. */
  uint8_t               mount_index; /**< Owning mount slot.              */
  bool                  in_use;      /**< Facade is occupied.             */
};

/** @brief Fixed mount table. */
static vfs_slot_t s_table[(uint32_t)k_ra8_io_vfs_max_mounts];
/** @brief Fixed format-neutral open-file table. */
static ra8_io_vfs_file_t s_files[(uint32_t)k_ra8_io_vfs_max_files];

RA8_PRIV bool priv_ra8_io_vfs_streq(const char* a, const char* b)
{
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_io_vfs_name_max; ++i) {
    if (a[i] != b[i]) {
      return false;
    }
    if (a[i] == '\0') {
      return true;
    }
  }
  return true;
}

/**
 * @brief Validate a non-empty bounded mount name with no path separators.
 * @details Rejects empty, unterminated, colon-bearing, and slash-bearing names.
 * @param[in] name Candidate name.
 * @return bool Validation result.
 * @retval true Name is valid.
 * @retval false Name is invalid.
 * @pre @p name is non-NULL.
 * @pre @p name addresses readable memory through its terminator or the bound.
 * @post No state is modified.
 * @post No byte beyond the fixed name bound is read.
 * @note Pure bounded validation.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_name_ok(const char* name)
{
  uint32_t i = 0U;
  for (i = 0U; i < (uint32_t)k_ra8_io_vfs_name_max; ++i) {
    const char c = name[i];
    if (c == '\0') {
      break;
    }
    if (c == ':') {
      return false;
    }
    if (c == '/') {
      return false;
    }
  }
  if (i == 0U) {
    return false;
  }
  return i < (uint32_t)k_ra8_io_vfs_name_max;
}

/**
 * @brief Copy one already-validated mount name into a fixed slot.
 * @details Performs a bounded copy and always terminates the destination.
 * @param[out] dst Fixed destination buffer.
 * @param[in] src Validated source name.
 * @return Nothing.
 * @pre @p dst and @p src are non-NULL.
 * @pre @p dst has VFS-name capacity and @p src terminates within that capacity.
 * @post @p dst contains a NUL-terminated copy.
 * @post No byte beyond the destination capacity is written.
 * @note Source and destination must not overlap.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_copy_name(char* dst, const char* src)
{
  uint32_t i = 0U;
  for (i = 0U; i < (uint32_t)k_ra8_io_vfs_name_max - 1U; ++i) {
    dst[i] = src[i];
    if (src[i] == '\0') {
      return;
    }
  }
  dst[i] = '\0';
}

RA8_PRIV vfs_slot_t* priv_ra8_io_vfs_find(const char* name, uint8_t* out_index)
{
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_io_vfs_max_mounts; ++i) {
    if (!s_table[i].in_use) {
      continue;
    }
    if (priv_ra8_io_vfs_streq(s_table[i].name, name)) {
      if (out_index != nullptr) {
        *out_index = (uint8_t)i;
      }
      return &s_table[i];
    }
  }
  return nullptr;
}

/**
 * @brief Find one free mount slot.
 * @details Searches the fixed mount table in ascending slot order.
 * @return vfs_slot_t* Free slot or NULL.
 * @retval non-NULL First free slot.
 * @retval NULL The table is full.
 * @pre VFS static state is initialized or zero-initialized.
 * @pre No concurrent mount-table mutation occurs.
 * @post The returned slot remains free.
 * @post Mount-table state is unchanged.
 * @note Performs no allocation.
 * @since 0.1.0
 */
RA8_INTERNAL
static vfs_slot_t* internal_free_mount(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_io_vfs_max_mounts; ++i) {
    if (!s_table[i].in_use) {
      return &s_table[i];
    }
  }
  return nullptr;
}

RA8_PRIV ra8_err_t priv_ra8_io_vfs_split(const char* path, char* out_name, const char** out_sub)
{
  uint32_t i = 0U;
  for (i = 0U; i < (uint32_t)k_ra8_io_vfs_name_max; ++i) {
    const char c = path[i];
    if (c == '\0') {
      return k_ra8_err_invalid_arg;
    }
    if (c == ':') {
      break;
    }
    out_name[i] = c;
  }
  if (i >= (uint32_t)k_ra8_io_vfs_name_max) {
    return k_ra8_err_invalid_arg;
  }
  out_name[i] = '\0';
  *out_sub    = &path[i + 1U];
  return k_ra8_ok;
}

RA8_PRIV ra8_err_t priv_ra8_io_vfs_resolve(const char*  path,
                                           vfs_slot_t** out_slot,
                                           uint8_t*     out_index,
                                           const char** out_sub)
{
  char            name[(uint32_t)k_ra8_io_vfs_name_max];
  const ra8_err_t e = priv_ra8_io_vfs_split(path, name, out_sub);
  if (e != k_ra8_ok) {
    return e;
  }
  vfs_slot_t* slot = priv_ra8_io_vfs_find(name, out_index);
  if (slot == nullptr) {
    return k_ra8_err_not_found;
  }
  *out_slot = slot;
  return k_ra8_ok;
}

/**
 * @brief Test whether a descriptor is one of the native ra8_fs built-ins.
 * @details Compares descriptor identity against the FAT and exFAT built-ins.
 * @param[in] format Candidate descriptor.
 * @return bool Native-format result.
 * @retval true Descriptor uses native ra8_fs contexts.
 * @retval false Descriptor is foreign.
 * @pre @p format is non-NULL.
 * @pre Built-in descriptors are available.
 * @post No state is modified.
 * @post Registry initialization order is irrelevant.
 * @note Descriptor pointer identity is stable for program lifetime.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_is_native(const ra8_io_fsfmt_t* format)
{
  const ra8_io_fsfmt_t* native = nullptr;
  (void)ra8_io_fsfmt_get_builtin(k_ra8_fs_type_fat16, &native);
  if (format == native) {
    return true;
  }
  (void)ra8_io_fsfmt_get_builtin(k_ra8_fs_type_exfat, &native);
  return format == native;
}

/**
 * @brief Populate a previously selected free mount slot.
 * @details Copies the name and records format, context, ownership, and native status.
 * @param[out] slot Free mount slot.
 * @param[in] name Validated mount name.
 * @param[in] format Selected format descriptor.
 * @param[in,out] mount_ctx Format-private mounted context.
 * @param[in] owned Whether VFS must invoke unmount.
 * @return Nothing.
 * @pre All pointer arguments are non-NULL.
 * @pre @p slot is free and @p name fits the fixed name buffer.
 * @post @p slot is occupied and resolves under @p name.
 * @post Ownership and native-format facts are recorded exactly.
 * @note Performs no allocation.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_store_mount(vfs_slot_t*           slot,
                                 const char*           name,
                                 const ra8_io_fsfmt_t* format,
                                 void*                 mount_ctx,
                                 bool                  owned)
{
  internal_copy_name(slot->name, name);
  slot->format    = format;
  slot->mount_ctx = mount_ctx;
  slot->owned     = owned;
  slot->native    = internal_is_native(format);
  slot->in_use    = true;
}

/**
 * @brief Test whether a value is one of the three public stream modes.
 * @details Accepts read, write, and append only.
 * @param[in] mode Candidate mode.
 * @return bool Mode-validation result.
 * @retval true Mode is supported by the API vocabulary.
 * @retval false Mode is outside the enum's defined values.
 * @pre @p mode is an arbitrary value representable by its enum type.
 * @pre No format callback is in progress.
 * @post No state is modified.
 * @post The result depends only on @p mode.
 * @note Pure validation.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_mode_valid(ra8_fs_mode_t mode)
{
  if (mode == k_ra8_fs_mode_read) {
    return true;
  }
  if (mode == k_ra8_fs_mode_write) {
    return true;
  }
  return mode == k_ra8_fs_mode_append;
}

/**
 * @brief Capability-gate a requested open mode.
 * @details Read is universal; writes require a writable streaming implementation.
 * @param[in] format Selected format descriptor.
 * @param[in] mode Requested mode.
 * @return ra8_err_t Capability result.
 * @retval k_ra8_ok The mode may be dispatched.
 * @retval k_ra8_err_invalid_arg The mode is invalid.
 * @retval k_ra8_err_not_supported The format cannot provide the mode.
 * @pre @p format and `format->ops` are non-NULL.
 * @pre The descriptor passed registry validation.
 * @post No callback is invoked on rejection.
 * @post No state is modified.
 * @note Independent decisions support direct MC/DC vectors.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_can_open(const ra8_io_fsfmt_t* format, ra8_fs_mode_t mode)
{
  if (!internal_mode_valid(mode)) {
    return k_ra8_err_invalid_arg;
  }
  if (mode == k_ra8_fs_mode_read) {
    return k_ra8_ok;
  }
  if (format->caps.read_only) {
    return k_ra8_err_not_supported;
  }
  if (!format->caps.supports_streaming_write) {
    return k_ra8_err_not_supported;
  }
  if (format->ops->write == nullptr) {
    return k_ra8_err_not_supported;
  }
  return k_ra8_ok;
}

/**
 * @brief Find one free generic-file facade.
 * @details Searches the fixed file table in ascending slot order.
 * @return ra8_io_vfs_file_t* Free facade or NULL.
 * @retval non-NULL First free facade.
 * @retval NULL The facade table is full.
 * @pre VFS static state is initialized or zero-initialized.
 * @pre No concurrent file-table mutation occurs.
 * @post The returned facade remains free.
 * @post File-table state is unchanged.
 * @note Performs no allocation.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_io_vfs_file_t* internal_free_file(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_io_vfs_max_files; ++i) {
    if (!s_files[i].in_use) {
      return &s_files[i];
    }
  }
  return nullptr;
}

/**
 * @brief Verify a facade is currently owned by the fixed file table.
 * @details Rejects NULL, foreign pointers, and released facade slots.
 * @param[in] file Candidate facade.
 * @return bool Facade-validation result.
 * @retval true Pointer names a live table entry.
 * @retval false Pointer is NULL, foreign, or released.
 * @pre @p file may be any pointer value supplied by a caller.
 * @pre No concurrent file-table mutation occurs.
 * @post No state is modified.
 * @post At most the fixed file table is inspected.
 * @note Pointer comparison occurs before any dereference of foreign storage.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_file_valid(const ra8_io_vfs_file_t* file)
{
  if (file == nullptr) {
    return false;
  }
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_io_vfs_max_files; ++i) {
    if (file == &s_files[i]) {
      return file->in_use;
    }
  }
  return false;
}

/**
 * @brief Reset one mount slot for global reinit, unmounting it first if owned.
 * @details Unmounts through the bound format only when the slot is both
 *          in use and owned by the VFS (a caller-supplied mount is never
 *          unmounted here), then always clears the slot.
 * @param[in,out] slot Mount slot to tear down and reset.
 * @return The owned unmount's status, or k_ra8_ok when no unmount was needed.
 * @retval k_ra8_ok The slot was already idle, borrowed, or unmounted cleanly.
 * @retval other The bound format's unmount reported a failure.
 * @pre @p slot is non-NULL.
 * @pre No open file facade still names this slot's mount index.
 * @post @p slot is zero-initialized.
 * @post An owned mount's format unmount is invoked exactly once.
 * @note Not thread-safe; caller serializes VFS-table access.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_vfs_init_slot(vfs_slot_t* slot)
{
  ra8_err_t e = k_ra8_ok;
  if (slot->in_use && slot->owned) {
    e = slot->format->ops->unmount(slot->mount_ctx);
  }
  *slot = (vfs_slot_t){};
  return e;
}

ra8_err_t ra8_io_vfs_init(void)
{
  ra8_err_t first_error = k_ra8_ok;
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_io_vfs_max_files; ++i) {
    s_files[i] = (ra8_io_vfs_file_t){};
  }
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_io_vfs_max_mounts; ++i) {
    const ra8_err_t e = internal_vfs_init_slot(&s_table[i]);
    if (first_error == k_ra8_ok) {
      first_error = e;
    }
  }
  return first_error;
}

ra8_err_t ra8_io_vfs_mount(const char* name, ra8_fs_mount_t* mount)
{
  RA8_CHECK_NULL_PTR(name, s_tag, "name must not be nullptr");
  RA8_CHECK_NULL_PTR(mount, s_tag, "mount must not be nullptr");
  if (!internal_name_ok(name)) {
    return k_ra8_err_invalid_arg;
  }
  if (priv_ra8_io_vfs_find(name, nullptr) != nullptr) {
    return k_ra8_err_exists;
  }
  vfs_slot_t* slot = internal_free_mount();
  if (slot == nullptr) {
    return k_ra8_err_no_mem;
  }
  const ra8_io_fsfmt_t* format = nullptr;
  RA8_RETURN_ON_ERROR(ra8_io_fsfmt_get_builtin(mount->type, &format), s_tag, "native type");
  internal_store_mount(slot, name, format, mount, false);
  return k_ra8_ok;
}

/**
 * @brief Probe a backend's format and mount it through that format, guarded.
 * @details Wraps the two format-layer calls each already guarded by
 *          ::RA8_RETURN_ON_ERROR so their expansion is counted against a
 *          dedicated frame rather than the caller's.
 * @param[in] backend Candidate raw storage backend to probe and mount.
 * @param[out] out_format Probed format on success.
 * @param[out] out_mount_ctx Opaque mount context on success.
 * @return Probe-then-mount status.
 * @retval k_ra8_ok @p out_format and @p out_mount_ctx are both valid.
 * @retval other The probe or the mount call failed; both outputs are
 *         indeterminate.
 * @pre @p backend, @p out_format, and @p out_mount_ctx are non-NULL.
 * @pre A free mount slot is already reserved for the probed result.
 * @post On failure no format is registered as owning @p backend.
 * @post On success exactly one format owns the returned mount context.
 * @note Not thread-safe; caller serializes VFS-table access.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_vfs_probe_and_mount(const ra8_fs_backend_t* backend,
                                                           const ra8_io_fsfmt_t**  out_format,
                                                           void**                  out_mount_ctx)
{
  RA8_RETURN_ON_ERROR(ra8_io_fsfmt_probe(backend, out_format), s_tag, "probe format");
  RA8_RETURN_ON_ERROR((*out_format)->ops->mount(backend, out_mount_ctx), s_tag, "mount format");
  return k_ra8_ok;
}

ra8_err_t ra8_io_vfs_mount_auto(const char* name, const ra8_fs_backend_t* backend)
{
  RA8_CHECK_NULL_PTR(name, s_tag, "name must not be nullptr");
  RA8_CHECK_NULL_PTR(backend, s_tag, "backend must not be nullptr");
  if (!internal_name_ok(name)) {
    return k_ra8_err_invalid_arg;
  }
  if (priv_ra8_io_vfs_find(name, nullptr) != nullptr) {
    return k_ra8_err_exists;
  }
  vfs_slot_t* slot = internal_free_mount();
  if (slot == nullptr) {
    return k_ra8_err_no_mem;
  }
  const ra8_io_fsfmt_t* format    = nullptr;
  void*                 mount_ctx = nullptr;
  const ra8_err_t       probed    = internal_vfs_probe_and_mount(backend, &format, &mount_ctx);
  if (probed != k_ra8_ok) {
    return probed;
  }
  internal_store_mount(slot, name, format, mount_ctx, true);
  return k_ra8_ok;
}

ra8_err_t ra8_io_vfs_unmount(const char* name)
{
  RA8_CHECK_NULL_PTR(name, s_tag, "name must not be nullptr");
  uint8_t     index = 0U;
  vfs_slot_t* slot  = priv_ra8_io_vfs_find(name, &index);
  if (slot == nullptr) {
    return k_ra8_err_not_found;
  }
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_io_vfs_max_files; ++i) {
    if (s_files[i].in_use) {
      if (s_files[i].mount_index == index) {
        return k_ra8_err_busy;
      }
    }
  }
  ra8_err_t e = k_ra8_ok;
  if (slot->owned) {
    e = slot->format->ops->unmount(slot->mount_ctx);
  }
  *slot = (vfs_slot_t){};
  return e;
}

/**
 * @brief Resolve a raw-open path and require the target format be native.
 * @details Reuses ::priv_ra8_io_vfs_resolve to locate the mount and sub-path, then
 *          requires the mount's format to hand back a native handle, then
 *          requires the format to support @p mode.
 * @param[in] path Full VFS path to resolve.
 * @param[in] mode Requested open mode.
 * @param[out] out_slot Resolved mount slot on success.
 * @param[out] out_sub Sub-path within the mount on success.
 * @return Resolve-and-capability status.
 * @retval k_ra8_ok @p out_slot and @p out_sub are both valid for a raw open.
 * @retval k_ra8_err_not_found No mount matches the resolved name.
 * @retval k_ra8_err_not_supported The mount's format has no native handle,
 *         or does not support @p mode.
 * @pre @p path, @p out_slot, and @p out_sub are non-NULL.
 * @pre @p mode is the mode the caller will hand to the format's open.
 * @post No mount table or file table entry is modified.
 * @post On failure no format callback has run and no output is meaningful.
 * @note Not thread-safe; caller serializes VFS-table access.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_vfs_open_resolve(const char*   path,
                                                        ra8_fs_mode_t mode,
                                                        vfs_slot_t**  out_slot,
                                                        const char**  out_sub)
{
  RA8_RETURN_ON_ERROR(priv_ra8_io_vfs_resolve(path, out_slot, nullptr, out_sub), s_tag, "resolve");
  if (!(*out_slot)->native) {
    return k_ra8_err_not_supported;
  }
  RA8_RETURN_ON_ERROR(internal_can_open((*out_slot)->format, mode), s_tag, "open capability");
  return k_ra8_ok;
}

ra8_err_t ra8_io_vfs_open(const char* path, ra8_fs_mode_t mode, ra8_fs_file_t** out_file)
{
  RA8_CHECK_NULL_PTR(path, s_tag, "path must not be nullptr");
  RA8_CHECK_NULL_PTR(out_file, s_tag, "out_file must not be nullptr");
  vfs_slot_t*     slot     = nullptr;
  const char*     sub      = nullptr;
  const ra8_err_t resolved = internal_vfs_open_resolve(path, mode, &slot, &sub);
  if (resolved != k_ra8_ok) {
    return resolved;
  }
  void* file_ctx = nullptr;
  RA8_RETURN_ON_ERROR(slot->format->ops->open(slot->mount_ctx, sub, mode, &file_ctx),
                      s_tag,
                      "open");
  *out_file = (ra8_fs_file_t*)file_ctx;
  return k_ra8_ok;
}

/**
 * @brief Resolve a facade-open path and require the format support @p mode.
 * @details Reuses ::priv_ra8_io_vfs_resolve to locate the mount, sub-path, and
 *          mount-table index, then requires the format to support @p mode.
 * @param[in] path Full VFS path to resolve.
 * @param[in] mode Requested open mode.
 * @param[out] out_slot Resolved mount slot on success.
 * @param[out] out_index Resolved mount-table index on success.
 * @param[out] out_sub Sub-path within the mount on success.
 * @return Resolve-and-capability status.
 * @retval k_ra8_ok @p out_slot, @p out_index, and @p out_sub are all valid.
 * @retval k_ra8_err_not_found No mount matches the resolved name.
 * @retval k_ra8_err_not_supported The mount's format does not support @p mode.
 * @pre @p path, @p out_slot, @p out_index, and @p out_sub are non-NULL.
 * @pre @p mode is the mode the caller will hand to the format's open.
 * @post No mount table or file table entry is modified.
 * @post On success @p out_index names the same slot as @p out_slot.
 * @note Not thread-safe; caller serializes VFS-table access.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_vfs_file_open_resolve(const char*   path,
                                                             ra8_fs_mode_t mode,
                                                             vfs_slot_t**  out_slot,
                                                             uint8_t*      out_index,
                                                             const char**  out_sub)
{
  RA8_RETURN_ON_ERROR(priv_ra8_io_vfs_resolve(path, out_slot, out_index, out_sub),
                      s_tag,
                      "resolve");
  RA8_RETURN_ON_ERROR(internal_can_open((*out_slot)->format, mode), s_tag, "open capability");
  return k_ra8_ok;
}

ra8_err_t ra8_io_vfs_file_open(const char* path, ra8_fs_mode_t mode, ra8_io_vfs_file_t** out_file)
{
  RA8_CHECK_NULL_PTR(path, s_tag, "path must not be nullptr");
  RA8_CHECK_NULL_PTR(out_file, s_tag, "out_file must not be nullptr");
  vfs_slot_t*     slot     = nullptr;
  uint8_t         index    = 0U;
  const char*     sub      = nullptr;
  const ra8_err_t resolved = internal_vfs_file_open_resolve(path, mode, &slot, &index, &sub);
  if (resolved != k_ra8_ok) {
    return resolved;
  }
  ra8_io_vfs_file_t* file = internal_free_file();
  if (file == nullptr) {
    return k_ra8_err_no_mem;
  }
  void* file_ctx = nullptr;
  RA8_RETURN_ON_ERROR(slot->format->ops->open(slot->mount_ctx, sub, mode, &file_ctx),
                      s_tag,
                      "open");
  file->format      = slot->format;
  file->file_ctx    = file_ctx;
  file->mount_index = index;
  file->in_use      = true;
  *out_file         = file;
  return k_ra8_ok;
}

ra8_err_t ra8_io_vfs_file_close(ra8_io_vfs_file_t* file)
{
  RA8_CHECK_NULL_PTR(file, s_tag, "file must not be nullptr");
  if (!internal_file_valid(file)) {
    return k_ra8_err_invalid_state;
  }
  const ra8_err_t e = file->format->ops->close(file->file_ctx);
  *file             = (ra8_io_vfs_file_t){};
  return e;
}

ra8_err_t
ra8_io_vfs_file_read(ra8_io_vfs_file_t* file, void* buf, uint32_t bytes, uint32_t* out_read)
{
  RA8_CHECK_NULL_PTR(file, s_tag, "file must not be nullptr");
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  RA8_CHECK_NULL_PTR(out_read, s_tag, "out_read must not be nullptr");
  if (!internal_file_valid(file)) {
    return k_ra8_err_invalid_state;
  }
  return file->format->ops->read(file->file_ctx, buf, bytes, out_read);
}

ra8_err_t ra8_io_vfs_file_write(ra8_io_vfs_file_t* file, const void* buf, uint32_t bytes)
{
  RA8_CHECK_NULL_PTR(file, s_tag, "file must not be nullptr");
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  if (!internal_file_valid(file)) {
    return k_ra8_err_invalid_state;
  }
  if (file->format->caps.read_only) {
    return k_ra8_err_not_supported;
  }
  if (!file->format->caps.supports_streaming_write) {
    return k_ra8_err_not_supported;
  }
  if (file->format->ops->write == nullptr) {
    return k_ra8_err_not_supported;
  }
  return file->format->ops->write(file->file_ctx, buf, bytes);
}

ra8_err_t ra8_io_vfs_file_seek(ra8_io_vfs_file_t* file, uint64_t offset_bytes)
{
  RA8_CHECK_NULL_PTR(file, s_tag, "file must not be nullptr");
  if (!internal_file_valid(file)) {
    return k_ra8_err_invalid_state;
  }
  return file->format->ops->seek(file->file_ctx, offset_bytes);
}

ra8_err_t ra8_io_vfs_file_tell(const ra8_io_vfs_file_t* file, uint64_t* out_offset)
{
  RA8_CHECK_NULL_PTR(file, s_tag, "file must not be nullptr");
  RA8_CHECK_NULL_PTR(out_offset, s_tag, "out_offset must not be nullptr");
  if (!internal_file_valid(file)) {
    return k_ra8_err_invalid_state;
  }
  return file->format->ops->tell(file->file_ctx, out_offset);
}

ra8_err_t ra8_io_vfs_file_size(const ra8_io_vfs_file_t* file, uint64_t* out_bytes)
{
  RA8_CHECK_NULL_PTR(file, s_tag, "file must not be nullptr");
  RA8_CHECK_NULL_PTR(out_bytes, s_tag, "out_bytes must not be nullptr");
  if (!internal_file_valid(file)) {
    return k_ra8_err_invalid_state;
  }
  return file->format->ops->size(file->file_ctx, out_bytes);
}

ra8_err_t ra8_io_vfs_file_sync(ra8_io_vfs_file_t* file)
{
  RA8_CHECK_NULL_PTR(file, s_tag, "file must not be nullptr");
  if (!internal_file_valid(file)) {
    return k_ra8_err_invalid_state;
  }
  if (!file->format->caps.supports_sync) {
    return k_ra8_err_not_supported;
  }
  if (file->format->ops->sync == nullptr) {
    return k_ra8_err_not_supported;
  }
  return file->format->ops->sync(file->file_ctx);
}

ra8_err_t ra8_io_vfs_get_caps(const char* name, ra8_io_fsfmt_caps_t* out)
{
  RA8_CHECK_NULL_PTR(name, s_tag, "name must not be nullptr");
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  vfs_slot_t* slot = priv_ra8_io_vfs_find(name, nullptr);
  if (slot == nullptr) {
    return k_ra8_err_not_found;
  }
  *out = slot->format->caps;
  return k_ra8_ok;
}

ra8_err_t ra8_io_vfs_free_space(const char* name, ra8_fs_space_t* out)
{
  RA8_CHECK_NULL_PTR(name, s_tag, "name must not be nullptr");
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  vfs_slot_t* slot = priv_ra8_io_vfs_find(name, nullptr);
  if (slot == nullptr) {
    return k_ra8_err_not_found;
  }
  if (!slot->format->caps.supports_free_space) {
    return k_ra8_err_not_supported;
  }
  if (slot->format->ops->free_space == nullptr) {
    return k_ra8_err_not_supported;
  }
  return slot->format->ops->free_space(slot->mount_ctx, out);
}
