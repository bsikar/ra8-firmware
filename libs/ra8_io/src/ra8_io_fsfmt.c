/**
 * @file ra8_io_fsfmt.c
 * @brief Pluggable filesystem-format registry and native ra8_fs adapter.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * A format descriptor owns the complete operation table the VFS dispatches.
 * The built-in FAT and exFAT descriptors adapt the existing ra8_fs engine;
 * foreign descriptors use the same registration path and require no VFS edit.
 * Probe validation is delegated to ::ra8_fs_probe for the native formats, so
 * superfloppy, MBR and GPT volumes are recognised by the same parser as mount.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_io_fsfmt.h"

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_fs.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_io_fsfmt";

/** @brief Public UTF-8 name-byte limits of the two native implementations. */
typedef enum : uint16_t {
  k_fat_max_name_utf8   = 741U, /**< 247 UTF-16 units, worst-case 3-byte UTF-8. */
  k_exfat_max_name_utf8 = 192U, /**< 64 UTF-16 units, worst-case 3-byte UTF-8.  */
} fsfmt_name_limit_t;

/** @brief Registered formats, in probe priority order. */
static const ra8_io_fsfmt_t* s_reg[(uint32_t)k_ra8_io_fsfmt_max];
/** @brief Occupied entries in ::s_reg. */
static uint32_t s_count;

/**
 * @brief Mount the native ra8_fs implementation without pointer aliasing.
 * @details Delegates to ::ra8_fs_mount and converts its typed handle only after success.
 * @param[in]  backend   Device-neutral block backend.
 * @param[out] out_mount Receives the native mount as an opaque context.
 * @return ra8_err_t Native mount result.
 * @retval k_ra8_ok Context returned.
 * @retval k_ra8_err_* Error propagated from ::ra8_fs_mount.
 * @pre @p backend is non-NULL and supplies the required operations.
 * @pre @p out_mount is non-NULL and writable.
 * @post On success `*out_mount` names a live native mount.
 * @post On failure `*out_mount` is untouched.
 * @note Uses no allocation beyond ra8_fs's fixed mount pool.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t native_mount(const ra8_fs_backend_t* backend, void** out_mount)
{
  ra8_fs_mount_t* mount = nullptr;
  const ra8_err_t e     = ra8_fs_mount(backend, &mount);
  if (e != k_ra8_ok) {
    return e;
  }
  *out_mount = mount;
  return k_ra8_ok;
}

/**
 * @brief Release a native ra8_fs mount.
 * @details Delegates lifecycle and pending metadata handling to ::ra8_fs_unmount.
 * @param[in,out] mount_ctx Native mount context.
 * @return ra8_err_t Native unmount result.
 * @retval k_ra8_ok Mount released.
 * @retval k_ra8_err_* Error propagated from ::ra8_fs_unmount.
 * @pre @p mount_ctx is non-NULL.
 * @pre @p mount_ctx identifies a live native mount.
 * @post The native mount slot is released on every return.
 * @post No registry slot is modified.
 * @note ra8_fs may report a final metadata-write error after releasing the slot.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t native_unmount(void* mount_ctx)
{
  return ra8_fs_unmount((ra8_fs_mount_t*)mount_ctx);
}

/**
 * @brief Open a native ra8_fs stream without pointer aliasing.
 * @details Delegates path and mode semantics to ::ra8_fs_open.
 * @param[in,out] mount_ctx Native mounted context.
 * @param[in] path Volume-relative path.
 * @param[in] mode Requested stream mode.
 * @param[out] out_file Receives an opaque native file context.
 * @return ra8_err_t Native open result.
 * @retval k_ra8_ok Stream opened.
 * @retval k_ra8_err_* Error propagated from ::ra8_fs_open.
 * @pre @p mount_ctx and @p path are non-NULL.
 * @pre @p out_file is non-NULL and writable.
 * @post On success `*out_file` names a live native stream.
 * @post On failure `*out_file` is untouched.
 * @note Uses ra8_fs's fixed file pool.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t native_open(void* mount_ctx, const char* path, ra8_fs_mode_t mode, void** out_file)
{
  ra8_fs_file_t*  file = nullptr;
  const ra8_err_t e    = ra8_fs_open((ra8_fs_mount_t*)mount_ctx, path, mode, &file);
  if (e != k_ra8_ok) {
    return e;
  }
  *out_file = file;
  return k_ra8_ok;
}

/**
 * @brief Close a native ra8_fs stream.
 * @details Delegates final timestamp and FSInfo handling to ::ra8_fs_close.
 * @param[in,out] file_ctx Native open-file context.
 * @return ra8_err_t Native close result.
 * @retval k_ra8_ok Stream closed.
 * @retval k_ra8_err_* Error propagated from ::ra8_fs_close.
 * @pre @p file_ctx is non-NULL.
 * @pre @p file_ctx identifies a live native stream.
 * @post The native file slot is released on every return.
 * @post No format-registry state is modified.
 * @note A final metadata error does not retain the file slot.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t native_close(void* file_ctx)
{
  return ra8_fs_close((ra8_fs_file_t*)file_ctx);
}

/**
 * @brief Read a native ra8_fs stream.
 * @details Delegates bounded streaming reads to ::ra8_fs_read.
 * @param[in,out] file_ctx Native open-file context.
 * @param[out] buf Destination buffer.
 * @param[in] bytes Maximum bytes to read.
 * @param[out] out_read Actual bytes read.
 * @return ra8_err_t Native read result.
 * @retval k_ra8_ok Read completed, possibly at EOF.
 * @retval k_ra8_err_* Error propagated from ::ra8_fs_read.
 * @pre @p file_ctx and @p buf are non-NULL.
 * @pre @p out_read is non-NULL and writable.
 * @post On success `*out_read` does not exceed @p bytes.
 * @post The file offset advances by `*out_read` on success.
 * @note Payload bytes are not buffered by this adapter.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t native_read(void* file_ctx, void* buf, uint32_t bytes, uint32_t* out_read)
{
  return ra8_fs_read((ra8_fs_file_t*)file_ctx, (uint8_t*)buf, bytes, out_read);
}

/**
 * @brief Write a native ra8_fs stream.
 * @details Delegates complete streaming writes to ::ra8_fs_write.
 * @param[in,out] file_ctx Native writable-file context.
 * @param[in] buf Source bytes.
 * @param[in] bytes Number of bytes to write.
 * @return ra8_err_t Native write result.
 * @retval k_ra8_ok Every requested byte was written.
 * @retval k_ra8_err_* Error propagated from ::ra8_fs_write.
 * @pre @p file_ctx and @p buf are non-NULL.
 * @pre @p file_ctx was opened in write or append mode.
 * @post On success the file offset advances by @p bytes.
 * @post The registry and mount table are unchanged.
 * @note Capability gating occurs in VFS before this callback.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t native_write(void* file_ctx, const void* buf, uint32_t bytes)
{
  return ra8_fs_write((ra8_fs_file_t*)file_ctx, (const uint8_t*)buf, bytes);
}

/**
 * @brief Seek a native ra8_fs stream.
 * @details Delegates 64-bit offset handling to ::ra8_fs_seek.
 * @param[in,out] file_ctx Native open-file context.
 * @param[in] offset_bytes Requested absolute byte offset.
 * @return ra8_err_t Native seek result.
 * @retval k_ra8_ok Offset updated.
 * @retval k_ra8_err_* Error propagated from ::ra8_fs_seek.
 * @pre @p file_ctx is non-NULL.
 * @pre @p file_ctx identifies a live native stream.
 * @post On success the offset is clamped according to ra8_fs semantics.
 * @post File contents and size are unchanged.
 * @note The format implementation owns cluster traversal.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t native_seek(void* file_ctx, uint64_t offset_bytes)
{
  return ra8_fs_seek((ra8_fs_file_t*)file_ctx, offset_bytes);
}

/**
 * @brief Report a native ra8_fs stream offset.
 * @details Delegates the format's 64-bit position query to ::ra8_fs_tell.
 * @param[in] file_ctx Native open-file context.
 * @param[out] out_offset Receives the byte offset.
 * @return ra8_err_t Native tell result.
 * @retval k_ra8_ok Offset reported.
 * @retval k_ra8_err_* Error propagated from ::ra8_fs_tell.
 * @pre @p file_ctx is non-NULL and live.
 * @pre @p out_offset is non-NULL and writable.
 * @post On success `*out_offset` is the current position.
 * @post Stream state is unchanged.
 * @note Read-only query.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t native_tell(const void* file_ctx, uint64_t* out_offset)
{
  return ra8_fs_tell((const ra8_fs_file_t*)file_ctx, out_offset);
}

/**
 * @brief Report a native ra8_fs stream size.
 * @details Delegates the format's 64-bit length query to ::ra8_fs_size.
 * @param[in] file_ctx Native open-file context.
 * @param[out] out_bytes Receives the file length.
 * @return ra8_err_t Native size result.
 * @retval k_ra8_ok Size reported.
 * @retval k_ra8_err_* Error propagated from ::ra8_fs_size.
 * @pre @p file_ctx is non-NULL and live.
 * @pre @p out_bytes is non-NULL and writable.
 * @post On success `*out_bytes` is the on-disk file length.
 * @post Stream state is unchanged.
 * @note Supports exFAT lengths above 4 GiB.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t native_size(const void* file_ctx, uint64_t* out_bytes)
{
  return ra8_fs_size((const ra8_fs_file_t*)file_ctx, out_bytes);
}

/**
 * @brief Query native ra8_fs metadata.
 * @details Delegates entry metadata decoding to ::ra8_fs_stat.
 * @param[in,out] mount_ctx Native mounted context.
 * @param[in] path Volume-relative path.
 * @param[out] out Metadata result.
 * @return ra8_err_t Native stat result.
 * @retval k_ra8_ok Metadata reported.
 * @retval k_ra8_err_* Error propagated from ::ra8_fs_stat.
 * @pre @p mount_ctx and @p path are non-NULL.
 * @pre @p out is non-NULL and writable.
 * @post On success `*out` reflects the directory entry or root.
 * @post No file slot is consumed.
 * @note Timestamps retain independent value and UTC-offset validity.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t native_stat(void* mount_ctx, const char* path, ra8_fs_stat_t* out)
{
  return ra8_fs_stat((ra8_fs_mount_t*)mount_ctx, path, out);
}

/**
 * @brief Enumerate a native ra8_fs directory.
 * @details Delegates one callback per visible entry to ::ra8_fs_listdir.
 * @param[in,out] mount_ctx Native mounted context.
 * @param[in] path Directory path.
 * @param[in] cb Entry callback.
 * @param[in,out] cb_ctx Caller context forwarded to @p cb.
 * @return ra8_err_t Native enumeration result.
 * @retval k_ra8_ok Enumeration completed.
 * @retval k_ra8_err_* Error propagated from ::ra8_fs_listdir.
 * @pre @p mount_ctx and @p path are non-NULL.
 * @pre @p cb is non-NULL.
 * @post On success every visible entry was reported once.
 * @post The namespace is unchanged.
 * @note Callback ordering is owned by the format.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
native_listdir(void* mount_ctx, const char* path, ra8_fs_listdir_cb_t cb, void* cb_ctx)
{
  return ra8_fs_listdir((ra8_fs_mount_t*)mount_ctx, path, cb, cb_ctx);
}

/**
 * @brief Unlink a native ra8_fs file.
 * @details Delegates file removal and chain release to ::ra8_fs_unlink.
 * @param[in,out] mount_ctx Native mounted context.
 * @param[in] path File path.
 * @return ra8_err_t Native unlink result.
 * @retval k_ra8_ok File removed.
 * @retval k_ra8_err_* Error propagated from ::ra8_fs_unlink.
 * @pre @p mount_ctx and @p path are non-NULL.
 * @pre No file handle remains open on @p path.
 * @post On success @p path no longer resolves.
 * @post Capability state is unchanged.
 * @note VFS refuses this callback for read-only formats.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t native_unlink(void* mount_ctx, const char* path)
{
  return ra8_fs_unlink((ra8_fs_mount_t*)mount_ctx, path);
}

/**
 * @brief Rename inside one native ra8_fs mount.
 * @details Delegates same-format rename semantics to ::ra8_fs_rename.
 * @param[in,out] mount_ctx Native mounted context.
 * @param[in] old_path Existing path.
 * @param[in] new_path Destination path.
 * @return ra8_err_t Native rename result.
 * @retval k_ra8_ok Entry renamed.
 * @retval k_ra8_err_* Error propagated from ::ra8_fs_rename.
 * @pre All pointers are non-NULL.
 * @pre Both paths belong to @p mount_ctx.
 * @post On success @p new_path resolves to the prior entry.
 * @post No cross-mount move occurs.
 * @note Native rename is not advertised as power-loss atomic.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t native_rename(void* mount_ctx, const char* old_path, const char* new_path)
{
  return ra8_fs_rename((ra8_fs_mount_t*)mount_ctx, old_path, new_path);
}

/**
 * @brief Create a native ra8_fs directory.
 * @details Delegates directory creation to ::ra8_fs_mkdir.
 * @param[in,out] mount_ctx Native mounted context.
 * @param[in] path Directory path.
 * @return ra8_err_t Native mkdir result.
 * @retval k_ra8_ok Directory created.
 * @retval k_ra8_err_* Error propagated from ::ra8_fs_mkdir.
 * @pre @p mount_ctx and @p path are non-NULL.
 * @pre Every parent component already exists.
 * @post On success @p path resolves as a directory.
 * @post On failure the format defines rollback semantics.
 * @note VFS capability-gates this callback.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t native_mkdir(void* mount_ctx, const char* path)
{
  return ra8_fs_mkdir((ra8_fs_mount_t*)mount_ctx, path);
}

/**
 * @brief Remove a native ra8_fs directory.
 * @details Delegates empty-directory removal to ::ra8_fs_rmdir.
 * @param[in,out] mount_ctx Native mounted context.
 * @param[in] path Directory path.
 * @return ra8_err_t Native rmdir result.
 * @retval k_ra8_ok Directory removed.
 * @retval k_ra8_err_* Error propagated from ::ra8_fs_rmdir.
 * @pre @p mount_ctx and @p path are non-NULL.
 * @pre @p path does not name the volume root.
 * @post On success @p path no longer resolves.
 * @post A non-empty directory remains unchanged.
 * @note VFS capability-gates this callback.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t native_rmdir(void* mount_ctx, const char* path)
{
  return ra8_fs_rmdir((ra8_fs_mount_t*)mount_ctx, path);
}

/**
 * @brief Query native ra8_fs free space.
 * @details Delegates capacity and allocation counts to ::ra8_fs_free_space.
 * @param[in,out] mount_ctx Native mounted context.
 * @param[out] out Space result.
 * @return ra8_err_t Native free-space result.
 * @retval k_ra8_ok Space reported.
 * @retval k_ra8_err_* Error propagated from ::ra8_fs_free_space.
 * @pre @p mount_ctx is non-NULL and live.
 * @pre @p out is non-NULL and writable.
 * @post On success the result's cluster and byte invariants hold.
 * @post The volume is not modified.
 * @note VFS capability-gates this callback.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t native_free_space(void* mount_ctx, ra8_fs_space_t* out)
{
  return ra8_fs_free_space((ra8_fs_mount_t*)mount_ctx, out);
}

/**
 * @brief Test whether the authoritative native parser identifies exFAT.
 * @details Uses ::ra8_fs_probe, including superfloppy, MBR, and GPT validation.
 * @param[in] backend Candidate backend.
 * @return bool Whether exFAT was identified.
 * @retval true A valid exFAT volume was found.
 * @retval false Probe failed or found another format.
 * @pre @p backend points at stable storage state.
 * @pre The backend's callbacks obey the ra8_fs contract.
 * @post No mount or file slot is consumed.
 * @post No block is written.
 * @note Read-only and allocation-free.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool exfat_probe(const ra8_fs_backend_t* backend)
{
  ra8_fs_type_t type = k_ra8_fs_type_unknown;
  if (ra8_fs_probe(backend, &type) != k_ra8_ok) {
    return false;
  }
  return type == k_ra8_fs_type_exfat;
}

/**
 * @brief Test whether the authoritative native parser identifies a FAT variant.
 * @details Accepts FAT12, FAT16, or FAT32 returned by ::ra8_fs_probe.
 * @param[in] backend Candidate backend.
 * @return bool Whether a valid FAT volume was identified.
 * @retval true FAT12, FAT16, or FAT32 was found.
 * @retval false Probe failed or found another format.
 * @pre @p backend points at stable storage state.
 * @pre The backend's callbacks obey the ra8_fs contract.
 * @post No mount or file slot is consumed.
 * @post No block is written.
 * @note Read-only and allocation-free.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool fat_probe(const ra8_fs_backend_t* backend)
{
  ra8_fs_type_t type = k_ra8_fs_type_unknown;
  if (ra8_fs_probe(backend, &type) != k_ra8_ok) {
    return false;
  }
  if (type == k_ra8_fs_type_fat12) {
    return true;
  }
  if (type == k_ra8_fs_type_fat16) {
    return true;
  }
  return type == k_ra8_fs_type_fat32;
}

/** @brief Complete native operation table; no explicit durable-sync seam exists. */
static const ra8_io_fsfmt_ops_t k_native_ops = {
  .probe      = fat_probe,
  .mount      = native_mount,
  .unmount    = native_unmount,
  .open       = native_open,
  .close      = native_close,
  .read       = native_read,
  .write      = native_write,
  .seek       = native_seek,
  .tell       = native_tell,
  .size       = native_size,
  .sync       = nullptr,
  .stat       = native_stat,
  .listdir    = native_listdir,
  .unlink     = native_unlink,
  .rename     = native_rename,
  .mkdir      = native_mkdir,
  .rmdir      = native_rmdir,
  .free_space = native_free_space,
};

/** @brief Native FAT12/16/32 descriptor. */
static const ra8_io_fsfmt_t k_fmt_fat = {
  .name = "fat",
  .caps =
    {
      .max_name_len             = (uint16_t)k_fat_max_name_utf8,
      .read_only                = false,
      .supports_mkdir           = true,
      .supports_rmdir           = true,
      .supports_streaming_write = true,
      .supports_timestamps      = true,
      .supports_free_space      = true,
      .supports_sync            = false,
      .atomic_rename            = false,
      .durable_sync             = false,
      .unicode_names            = true,
      .case_sensitive           = false,
    },
  .ops = &k_native_ops,
};

/** @brief Native exFAT descriptor; shares byte-identical ra8_fs dispatch. */
static const ra8_io_fsfmt_ops_t k_exfat_ops = {
  .probe      = exfat_probe,
  .mount      = native_mount,
  .unmount    = native_unmount,
  .open       = native_open,
  .close      = native_close,
  .read       = native_read,
  .write      = native_write,
  .seek       = native_seek,
  .tell       = native_tell,
  .size       = native_size,
  .sync       = nullptr,
  .stat       = native_stat,
  .listdir    = native_listdir,
  .unlink     = native_unlink,
  .rename     = native_rename,
  .mkdir      = native_mkdir,
  .rmdir      = native_rmdir,
  .free_space = native_free_space,
};

/** @brief Native exFAT descriptor. */
static const ra8_io_fsfmt_t k_fmt_exfat = {
  .name = "exfat",
  .caps =
    {
      .max_name_len             = (uint16_t)k_exfat_max_name_utf8,
      .read_only                = false,
      .supports_mkdir           = true,
      .supports_rmdir           = true,
      .supports_streaming_write = true,
      .supports_timestamps      = true,
      .supports_free_space      = true,
      .supports_sync            = false,
      .atomic_rename            = false,
      .durable_sync             = false,
      .unicode_names            = true,
      .case_sensitive           = false,
    },
  .ops = &k_exfat_ops,
};

/**
 * @brief Validate that every claimed optional capability has an operation.
 * @details Rejects descriptors whose advertised behavior cannot be dispatched.
 * @param[in] fmt Candidate format descriptor.
 * @return ra8_err_t Consistency result.
 * @retval k_ra8_ok Every claimed capability is backed by an operation.
 * @retval k_ra8_err_invalid_arg A claimed operation is absent or incoherent.
 * @pre @p fmt and `fmt->ops` are non-NULL.
 * @pre Mandatory operation pointers were validated by the caller.
 * @post No registry state is modified.
 * @post The descriptor remains caller-owned and unchanged.
 * @note Each capability decision is independently MC/DC-testable.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_validate_caps(const ra8_io_fsfmt_t* fmt)
{
  if (fmt->caps.supports_streaming_write) {
    if (fmt->ops->write == nullptr) {
      return k_ra8_err_invalid_arg;
    }
  }
  if (fmt->caps.supports_mkdir) {
    if (fmt->ops->mkdir == nullptr) {
      return k_ra8_err_invalid_arg;
    }
  }
  if (fmt->caps.supports_rmdir) {
    if (fmt->ops->rmdir == nullptr) {
      return k_ra8_err_invalid_arg;
    }
  }
  if (fmt->caps.supports_free_space) {
    if (fmt->ops->free_space == nullptr) {
      return k_ra8_err_invalid_arg;
    }
  }
  if (fmt->caps.supports_sync) {
    if (fmt->ops->sync == nullptr) {
      return k_ra8_err_invalid_arg;
    }
  }
  if (fmt->caps.atomic_rename) {
    if (fmt->ops->rename == nullptr) {
      return k_ra8_err_invalid_arg;
    }
  }
  if (fmt->caps.durable_sync) {
    if (!fmt->caps.supports_sync) {
      return k_ra8_err_invalid_arg;
    }
  }
  return k_ra8_ok;
}

ra8_err_t ra8_io_fsfmt_init(void)
{
  s_count = 0U;
  RA8_RETURN_ON_ERROR(ra8_io_fsfmt_register(&k_fmt_exfat), s_tag, "register exfat");
  RA8_RETURN_ON_ERROR(ra8_io_fsfmt_register(&k_fmt_fat), s_tag, "register fat");
  return k_ra8_ok;
}

ra8_err_t ra8_io_fsfmt_register(const ra8_io_fsfmt_t* fmt)
{
  RA8_CHECK_NULL_PTR(fmt, s_tag, "fmt must not be nullptr");
  RA8_CHECK_NULL_PTR(fmt->name, s_tag, "fmt->name must not be nullptr");
  RA8_CHECK_NULL_PTR(fmt->ops, s_tag, "fmt->ops must not be nullptr");
  RA8_CHECK_NULL_PTR(fmt->ops->probe, s_tag, "probe must not be nullptr");
  RA8_CHECK_NULL_PTR(fmt->ops->mount, s_tag, "mount must not be nullptr");
  RA8_CHECK_NULL_PTR(fmt->ops->unmount, s_tag, "unmount must not be nullptr");
  RA8_CHECK_NULL_PTR(fmt->ops->open, s_tag, "open must not be nullptr");
  RA8_CHECK_NULL_PTR(fmt->ops->close, s_tag, "close must not be nullptr");
  RA8_CHECK_NULL_PTR(fmt->ops->read, s_tag, "read must not be nullptr");
  RA8_CHECK_NULL_PTR(fmt->ops->seek, s_tag, "seek must not be nullptr");
  RA8_CHECK_NULL_PTR(fmt->ops->tell, s_tag, "tell must not be nullptr");
  RA8_CHECK_NULL_PTR(fmt->ops->size, s_tag, "size must not be nullptr");
  RA8_CHECK_NULL_PTR(fmt->ops->stat, s_tag, "stat must not be nullptr");
  RA8_CHECK_NULL_PTR(fmt->ops->listdir, s_tag, "listdir must not be nullptr");
  RA8_RETURN_ON_ERROR(internal_validate_caps(fmt), s_tag, "capability mismatch");
  if (s_count >= (uint32_t)k_ra8_io_fsfmt_max) {
    return k_ra8_err_no_mem;
  }
  s_reg[s_count] = fmt;
  s_count++;
  return k_ra8_ok;
}

ra8_err_t ra8_io_fsfmt_get_builtin(ra8_fs_type_t type, const ra8_io_fsfmt_t** out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  if (type == k_ra8_fs_type_exfat) {
    *out = &k_fmt_exfat;
    return k_ra8_ok;
  }
  if (type == k_ra8_fs_type_fat12) {
    *out = &k_fmt_fat;
    return k_ra8_ok;
  }
  if (type == k_ra8_fs_type_fat16) {
    *out = &k_fmt_fat;
    return k_ra8_ok;
  }
  if (type == k_ra8_fs_type_fat32) {
    *out = &k_fmt_fat;
    return k_ra8_ok;
  }
  return k_ra8_err_invalid_arg;
}

ra8_err_t ra8_io_fsfmt_probe(const ra8_fs_backend_t* backend, const ra8_io_fsfmt_t** out)
{
  RA8_CHECK_NULL_PTR(backend, s_tag, "backend must not be nullptr");
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  for (uint32_t i = 0U; i < s_count; ++i) {
    if (s_reg[i]->ops->probe(backend)) {
      *out = s_reg[i];
      return k_ra8_ok;
    }
  }
  return k_ra8_err_not_found;
}
