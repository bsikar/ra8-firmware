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

/** @brief Module log tag. */
static const char* const s_tag = "ra8_io_vfs";

/** @brief One fixed named-mount slot. */
typedef struct {
  /** NUL-terminated mount name. */
  char name[(uint32_t)k_ra8_io_vfs_name_max];
  /** Selected format descriptor. */
  const ra8_io_fsfmt_t* format;
  /** Format-private mounted context. */
  void* mount_ctx;
  /** VFS must invoke format unmount. */
  bool owned;
  /** Context is a native ra8_fs mount. */
  bool native;
  /** Slot is occupied. */
  bool in_use;
} vfs_slot_t;

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

/**
 * @brief Compare two mount names within the fixed bound.
 * @details Stops at the first mismatch or shared terminator.
 * @param[in] a First name.
 * @param[in] b Second name.
 * @return bool Equality result.
 * @retval true Names are equal.
 * @retval false Names differ.
 * @pre @p a and @p b are non-NULL.
 * @pre Both names terminate within the VFS name bound.
 * @post No state is modified.
 * @post At most the fixed name bound is inspected.
 * @note Pure bounded comparison.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_streq(const char* a, const char* b)
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

/**
 * @brief Find an occupied mount slot by name and optionally report its index.
 * @details Searches the fixed mount table in ascending slot order.
 * @param[in] name Valid mount name.
 * @param[out] out_index Optional slot-index destination.
 * @return vfs_slot_t* Matching slot or NULL.
 * @retval non-NULL Occupied matching slot.
 * @retval NULL No slot matched.
 * @pre @p name is non-NULL and bounded.
 * @pre @p out_index is NULL or writable.
 * @post On a match the optional index identifies the returned slot.
 * @post Mount-table state is unchanged.
 * @note Not thread-safe with concurrent mount mutation.
 * @since 0.1.0
 */
RA8_INTERNAL
static vfs_slot_t* internal_find(const char* name, uint8_t* out_index)
{
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_io_vfs_max_mounts; ++i) {
    if (!s_table[i].in_use) {
      continue;
    }
    if (internal_streq(s_table[i].name, name)) {
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

/**
 * @brief Split `"name:sub"` into a bounded name and sub-path pointer.
 * @details Finds the colon within the name bound and copies only the prefix.
 * @param[in] path Qualified VFS path.
 * @param[out] out_name Fixed mount-name buffer.
 * @param[out] out_sub Receives a pointer after the colon.
 * @return ra8_err_t Split result.
 * @retval k_ra8_ok Path split.
 * @retval k_ra8_err_invalid_arg No bounded colon exists.
 * @pre All pointers are non-NULL.
 * @pre @p out_name has VFS-name capacity.
 * @post On success both outputs are assigned.
 * @post No mount-table state is modified.
 * @note The sub-path aliases the caller's input string.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_split(const char* path, char* out_name, const char** out_sub)
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

/**
 * @brief Resolve `"name:sub"` to a mount slot and sub-path.
 * @details Splits the prefix then looks up the named fixed-table slot.
 * @param[in] path Qualified VFS path.
 * @param[out] out_slot Receives the mounted slot.
 * @param[out] out_index Optional slot-index destination.
 * @param[out] out_sub Receives the volume-relative sub-path.
 * @return ra8_err_t Resolution result.
 * @retval k_ra8_ok Mount resolved.
 * @retval k_ra8_err_not_found Name is not mounted.
 * @retval k_ra8_err_invalid_arg Path cannot be split.
 * @pre @p path, @p out_slot, and @p out_sub are non-NULL.
 * @pre @p out_index is NULL or writable.
 * @post On success the slot and sub-path outputs are assigned.
 * @post Mount-table state is unchanged.
 * @note Not thread-safe with concurrent unmount.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_resolve(const char* path, vfs_slot_t** out_slot, uint8_t* out_index, const char** out_sub)
{
  char            name[(uint32_t)k_ra8_io_vfs_name_max];
  const ra8_err_t e = internal_split(path, name, out_sub);
  if (e != k_ra8_ok) {
    return e;
  }
  vfs_slot_t* slot = internal_find(name, out_index);
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

ra8_err_t ra8_io_vfs_init(void)
{
  ra8_err_t first_error = k_ra8_ok;
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_io_vfs_max_files; ++i) {
    s_files[i] = (ra8_io_vfs_file_t){};
  }
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_io_vfs_max_mounts; ++i) {
    if (s_table[i].in_use) {
      if (s_table[i].owned) {
        const ra8_err_t e = s_table[i].format->ops->unmount(s_table[i].mount_ctx);
        if (first_error == k_ra8_ok) {
          first_error = e;
        }
      }
    }
    s_table[i] = (vfs_slot_t){};
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
  if (internal_find(name, nullptr) != nullptr) {
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

ra8_err_t ra8_io_vfs_mount_auto(const char* name, const ra8_fs_backend_t* backend)
{
  RA8_CHECK_NULL_PTR(name, s_tag, "name must not be nullptr");
  RA8_CHECK_NULL_PTR(backend, s_tag, "backend must not be nullptr");
  if (!internal_name_ok(name)) {
    return k_ra8_err_invalid_arg;
  }
  if (internal_find(name, nullptr) != nullptr) {
    return k_ra8_err_exists;
  }
  vfs_slot_t* slot = internal_free_mount();
  if (slot == nullptr) {
    return k_ra8_err_no_mem;
  }
  const ra8_io_fsfmt_t* format = nullptr;
  RA8_RETURN_ON_ERROR(ra8_io_fsfmt_probe(backend, &format), s_tag, "probe format");
  void* mount_ctx = nullptr;
  RA8_RETURN_ON_ERROR(format->ops->mount(backend, &mount_ctx), s_tag, "mount format");
  internal_store_mount(slot, name, format, mount_ctx, true);
  return k_ra8_ok;
}

ra8_err_t ra8_io_vfs_unmount(const char* name)
{
  RA8_CHECK_NULL_PTR(name, s_tag, "name must not be nullptr");
  uint8_t     index = 0U;
  vfs_slot_t* slot  = internal_find(name, &index);
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

ra8_err_t ra8_io_vfs_open(const char* path, ra8_fs_mode_t mode, ra8_fs_file_t** out_file)
{
  RA8_CHECK_NULL_PTR(path, s_tag, "path must not be nullptr");
  RA8_CHECK_NULL_PTR(out_file, s_tag, "out_file must not be nullptr");
  vfs_slot_t* slot = nullptr;
  const char* sub  = nullptr;
  RA8_RETURN_ON_ERROR(internal_resolve(path, &slot, nullptr, &sub), s_tag, "resolve");
  if (!slot->native) {
    return k_ra8_err_not_supported;
  }
  RA8_RETURN_ON_ERROR(internal_can_open(slot->format, mode), s_tag, "open capability");
  void* file_ctx = nullptr;
  RA8_RETURN_ON_ERROR(slot->format->ops->open(slot->mount_ctx, sub, mode, &file_ctx),
                      s_tag,
                      "open");
  *out_file = (ra8_fs_file_t*)file_ctx;
  return k_ra8_ok;
}

ra8_err_t ra8_io_vfs_file_open(const char* path, ra8_fs_mode_t mode, ra8_io_vfs_file_t** out_file)
{
  RA8_CHECK_NULL_PTR(path, s_tag, "path must not be nullptr");
  RA8_CHECK_NULL_PTR(out_file, s_tag, "out_file must not be nullptr");
  vfs_slot_t* slot  = nullptr;
  uint8_t     index = 0U;
  const char* sub   = nullptr;
  RA8_RETURN_ON_ERROR(internal_resolve(path, &slot, &index, &sub), s_tag, "resolve");
  RA8_RETURN_ON_ERROR(internal_can_open(slot->format, mode), s_tag, "open capability");
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

ra8_err_t ra8_io_vfs_unlink(const char* path)
{
  RA8_CHECK_NULL_PTR(path, s_tag, "path must not be nullptr");
  vfs_slot_t* slot = nullptr;
  const char* sub  = nullptr;
  RA8_RETURN_ON_ERROR(internal_resolve(path, &slot, nullptr, &sub), s_tag, "resolve");
  if (slot->format->caps.read_only) {
    return k_ra8_err_not_supported;
  }
  if (slot->format->ops->unlink == nullptr) {
    return k_ra8_err_not_supported;
  }
  return slot->format->ops->unlink(slot->mount_ctx, sub);
}

ra8_err_t ra8_io_vfs_rename(const char* old_path, const char* new_path)
{
  RA8_CHECK_NULL_PTR(old_path, s_tag, "old_path must not be nullptr");
  RA8_CHECK_NULL_PTR(new_path, s_tag, "new_path must not be nullptr");
  char        old_name[(uint32_t)k_ra8_io_vfs_name_max];
  char        new_name[(uint32_t)k_ra8_io_vfs_name_max];
  const char* old_sub = nullptr;
  const char* new_sub = nullptr;
  RA8_RETURN_ON_ERROR(internal_split(old_path, old_name, &old_sub), s_tag, "old path");
  RA8_RETURN_ON_ERROR(internal_split(new_path, new_name, &new_sub), s_tag, "new path");
  if (!internal_streq(old_name, new_name)) {
    return k_ra8_err_invalid_arg;
  }
  vfs_slot_t* slot = internal_find(old_name, nullptr);
  if (slot == nullptr) {
    return k_ra8_err_not_found;
  }
  if (slot->format->caps.read_only) {
    return k_ra8_err_not_supported;
  }
  if (slot->format->ops->rename == nullptr) {
    return k_ra8_err_not_supported;
  }
  return slot->format->ops->rename(slot->mount_ctx, old_sub, new_sub);
}

ra8_err_t ra8_io_vfs_stat(const char* path, ra8_io_vfs_stat_t* out)
{
  RA8_CHECK_NULL_PTR(path, s_tag, "path must not be nullptr");
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  *out             = (ra8_io_vfs_stat_t){};
  vfs_slot_t* slot = nullptr;
  const char* sub  = nullptr;
  RA8_RETURN_ON_ERROR(internal_resolve(path, &slot, nullptr, &sub), s_tag, "resolve");
  ra8_fs_stat_t   stat = {};
  const ra8_err_t e    = slot->format->ops->stat(slot->mount_ctx, sub, &stat);
  if (e == k_ra8_err_not_found) {
    return k_ra8_ok;
  }
  if (e != k_ra8_ok) {
    return e;
  }
  out->size_bytes   = stat.size_bytes;
  out->created      = stat.created;
  out->modified     = stat.modified;
  out->accessed     = stat.accessed;
  out->attr         = stat.attr;
  out->is_directory = stat.is_directory;
  out->exists       = true;
  return k_ra8_ok;
}

ra8_err_t ra8_io_vfs_get_caps(const char* name, ra8_io_fsfmt_caps_t* out)
{
  RA8_CHECK_NULL_PTR(name, s_tag, "name must not be nullptr");
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  vfs_slot_t* slot = internal_find(name, nullptr);
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
  vfs_slot_t* slot = internal_find(name, nullptr);
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

ra8_err_t ra8_io_vfs_listdir(const char* path, ra8_fs_listdir_cb_t cb, void* ctx)
{
  RA8_CHECK_NULL_PTR(path, s_tag, "path must not be nullptr");
  RA8_CHECK_NULL_PTR(cb, s_tag, "cb must not be nullptr");
  vfs_slot_t* slot = nullptr;
  const char* sub  = nullptr;
  RA8_RETURN_ON_ERROR(internal_resolve(path, &slot, nullptr, &sub), s_tag, "resolve");
  return slot->format->ops->listdir(slot->mount_ctx, sub, cb, ctx);
}

ra8_err_t ra8_io_vfs_dir_requirements(const char* path,
                                      uint32_t*   out_bytes,
                                      uint8_t*    out_align,
                                      uint16_t*   out_max_open)
{
  if ((path == nullptr) || (out_bytes == nullptr) || (out_align == nullptr) ||
      (out_max_open == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *out_bytes               = 0U;
  *out_align               = 0U;
  *out_max_open            = 0U;
  vfs_slot_t*     slot     = nullptr;
  const char*     sub      = nullptr;
  const ra8_err_t resolved = internal_resolve(path, &slot, nullptr, &sub);
  (void)sub;
  if (resolved != k_ra8_ok) {
    return resolved;
  }
  if (!slot->format->caps.supports_dir_cursor) {
    return k_ra8_err_not_supported;
  }
  *out_bytes    = slot->format->caps.directory_workspace_bytes;
  *out_align    = slot->format->caps.directory_workspace_align;
  *out_max_open = slot->format->caps.max_open_directories;
  return k_ra8_ok;
}

ra8_err_t ra8_io_vfs_dir_open(const char*       path,
                              ra8_io_vfs_dir_t* directory,
                              void*             workspace,
                              uint32_t          workspace_bytes)
{
  if ((path == nullptr) || (directory == nullptr) || (workspace == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (directory->is_open) {
    return k_ra8_err_busy;
  }
  vfs_slot_t*     slot     = nullptr;
  const char*     sub      = nullptr;
  const ra8_err_t resolved = internal_resolve(path, &slot, nullptr, &sub);
  if (resolved != k_ra8_ok) {
    return resolved;
  }
  const ra8_io_fsfmt_caps_t* caps = &slot->format->caps;
  if (!caps->supports_dir_cursor) {
    return k_ra8_err_not_supported;
  }
  if (workspace_bytes < caps->directory_workspace_bytes) {
    return k_ra8_err_no_mem;
  }
  if (((uintptr_t)workspace % (uintptr_t)caps->directory_workspace_align) != 0U) {
    return k_ra8_err_invalid_arg;
  }
  const ra8_err_t opened =
    slot->format->ops->dir_open(slot->mount_ctx, sub, workspace, workspace_bytes);
  if (opened != k_ra8_ok) {
    return opened;
  }
  *directory = (ra8_io_vfs_dir_t){.format      = slot->format,
                                  .state       = workspace,
                                  .state_bytes = workspace_bytes,
                                  .is_open     = true};
  return k_ra8_ok;
}

ra8_err_t ra8_io_vfs_dir_next(ra8_io_vfs_dir_t* directory, ra8_fs_dirent_t* out, bool* out_entry)
{
  if ((directory == nullptr) || (out == nullptr) || (out_entry == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (!directory->is_open || (directory->format == nullptr)) {
    return k_ra8_err_invalid_state;
  }
  *out       = (ra8_fs_dirent_t){};
  *out_entry = false;
  return directory->format->ops->dir_next(directory->state, out, out_entry);
}

ra8_err_t ra8_io_vfs_dir_close(ra8_io_vfs_dir_t* directory)
{
  if (directory == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (!directory->is_open || (directory->format == nullptr)) {
    return k_ra8_err_invalid_state;
  }
  const ra8_err_t closed = directory->format->ops->dir_close(directory->state);
  *directory             = (ra8_io_vfs_dir_t){};
  return closed;
}

ra8_err_t ra8_io_vfs_mkdir(const char* path)
{
  RA8_CHECK_NULL_PTR(path, s_tag, "path must not be nullptr");
  vfs_slot_t* slot = nullptr;
  const char* sub  = nullptr;
  RA8_RETURN_ON_ERROR(internal_resolve(path, &slot, nullptr, &sub), s_tag, "resolve");
  if (slot->format->caps.read_only) {
    return k_ra8_err_not_supported;
  }
  if (!slot->format->caps.supports_mkdir) {
    return k_ra8_err_not_supported;
  }
  if (slot->format->ops->mkdir == nullptr) {
    return k_ra8_err_not_supported;
  }
  return slot->format->ops->mkdir(slot->mount_ctx, sub);
}

ra8_err_t ra8_io_vfs_rmdir(const char* path)
{
  RA8_CHECK_NULL_PTR(path, s_tag, "path must not be nullptr");
  vfs_slot_t* slot = nullptr;
  const char* sub  = nullptr;
  RA8_RETURN_ON_ERROR(internal_resolve(path, &slot, nullptr, &sub), s_tag, "resolve");
  if (slot->format->caps.read_only) {
    return k_ra8_err_not_supported;
  }
  if (!slot->format->caps.supports_rmdir) {
    return k_ra8_err_not_supported;
  }
  if (slot->format->ops->rmdir == nullptr) {
    return k_ra8_err_not_supported;
  }
  return slot->format->ops->rmdir(slot->mount_ctx, sub);
}
