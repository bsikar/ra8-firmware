/**
 * @file ra8_fs_fat_mount.c
 * @brief FAT/exFAT volume detection, geometry, and mount/unmount.
 *
 * @details
 * Slot allocation, BPB parsing, MBR partition location, volume-type
 * detection, and the public mount/unmount/format entry points. The GPT half
 * of partition location lives in `ra8_fs_gpt_abi.zig` (it outgrew this file
 * when LBAs went 64-bit); the two locators are called from here.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_fs.h"
#include "ra8_fs_fat_internal.h"
/* Ported to Zig (RA8FW-1130): the geometry and partition-selection layer. */
extern uint8_t internal_bps_valid(uint32_t bs);
extern ra8_err_t internal_compute_geometry(ra8_fs_mount_t* m);
extern uint32_t internal_mbr_part0_lba(const uint8_t* buf);
extern ra8_err_t internal_mbr_select_entry(const uint8_t* buf, uint8_t index, uint64_t* out_base);
extern ra8_err_t internal_locate_indexed(const ra8_fs_mount_t* m, uint8_t index, uint64_t* out_base);


/* The shared module state and both slot allocators now live in Zig
 * (RA8FW-1143); the mount allocator is declared here for the mount and
 * unmount paths. `g_fs_scratch` and `k_zero_sector` come in through
 * `ra8_fs_fat_internal.h` and `ra8_fs_fat_bytes_internal.h` as before. */
extern ra8_fs_mount_t* internal_alloc_mount_slot(void);extern ra8_err_t internal_read_boot_sector(ra8_fs_mount_t* m, uint8_t index);
extern ra8_err_t internal_mount_probe_bps(const ra8_fs_backend_t* backend, ra8_fs_mount_t* m);
extern ra8_err_t internal_probe_locked(const ra8_fs_backend_t* backend, ra8_fs_type_t* out_type);extern ra8_err_t internal_mount_locked(const ra8_fs_backend_t* backend, uint8_t index,
                                        ra8_fs_mount_t** out_handle);
extern ra8_err_t internal_unmount_locked(ra8_fs_mount_t* handle);



/* =============================================================================
 * Module state
 * =============================================================================
 */

/**
 * @brief Single max-sector scratch buffer reused across all I/O.
 * @details Shared by every FAT/exFAT translation unit; declared `extern` in
 *          `ra8_fs_fat_internal.h` and defined here exactly once. Sized to
 *          ::k_ra8_fs_sector_max so a 4Kn medium fits without allocation.
 */
/**
 * @brief Lay down a fresh volume -- the guarded body of ::ra8_fs_format().
 *
 * @details Validates the backend and the requested geometry, then dispatches to
 *          the FAT or exFAT formatter. The public ::ra8_fs_format() is the
 *          wrapper that brackets this call with the library lock; the full
 *          contract is documented there.
 *
 * @param[in] backend Block-device backend to write.
 * @param[in] opts    Format options (variant, label, cluster-size hint).
 *
 * @return Error code.
 * @retval k_ra8_ok              Volume formatted.
 * @retval k_ra8_err_null_ptr    @p backend or @p opts was NULL.
 * @retval k_ra8_err_invalid_arg Backend callbacks or geometry are unusable.
 * @retval k_ra8_err_*           As documented for ::ra8_fs_format().
 *
 * @pre The library lock is held (or none is installed).
 * @pre No volume from this backend is currently mounted.
 * @post On success the backend holds a mountable @p opts->type volume.
 * @post On an argument error the backend is untouched.
 *
 * @note Never call this from outside `ra8_fs`; it is the unlocked half.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_EXPECTS_LOCK("ra8_fs_lock")
static ra8_err_t internal_format_locked(const ra8_fs_backend_t*     backend,
                                        const ra8_fs_format_opts_t* opts)
{
  if (backend == nullptr || opts == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (backend->write_block == nullptr || backend->get_capacity == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if (opts->type != k_ra8_fs_type_fat12 && opts->type != k_ra8_fs_type_fat16 &&
      opts->type != k_ra8_fs_type_fat32 && opts->type != k_ra8_fs_type_exfat) {
    return k_ra8_err_not_supported;
  }
  if (!priv_fmt_spc_valid(opts->sectors_per_cluster)) {
    return k_ra8_err_invalid_arg;
  }
  uint64_t  block_count = 0U;
  uint32_t  block_size  = 0U;
  ra8_err_t err         = backend->get_capacity(backend->ctx, &block_count, &block_size);
  if (err != k_ra8_ok) {
    return err;
  }
  if ((internal_bps_valid(block_size) == 0U) || (block_count == 0U)) {
    return k_ra8_err_invalid_arg;
  }
  if (opts->type == k_ra8_fs_type_exfat) {
    return priv_exfat_format(backend, block_count, block_size, opts->label);
  }
  ra8_fs_fmt_geom_t geom = {};
  geom.type              = opts->type;
  geom.total_sectors     = block_count;
  geom.bytes_per_sector  = block_size;
  geom.reserved_sectors  = priv_fmt_reserved_for(opts->type);
  geom.root_entries      = (opts->type == k_ra8_fs_type_fat32) ? 0U : (uint32_t)k_fmt_root_ents_f16;
  geom.root_sectors =
    ((geom.root_entries * (uint32_t)k_ra8_fs_dir_entry_bytes) + (block_size - 1U)) / block_size;
  err = priv_fmt_choose_geometry(&geom, opts->sectors_per_cluster);
  if (err != k_ra8_ok) {
    return err;
  }
  return priv_fmt_emit_volume(backend, &geom, opts->label);
}

/* =============================================================================
 * Public entry points -- the lock brackets
 * =============================================================================
 */

RA8_OWNS_RESOURCE("ra8_fs_lock")
ra8_err_t ra8_fs_format(const ra8_fs_backend_t* backend, const ra8_fs_format_opts_t* opts)
{
  priv_lock_acquire();
  const ra8_err_t err = internal_format_locked(backend, opts);
  priv_lock_release();
  return err;
}

RA8_OWNS_RESOURCE("ra8_fs_lock")
ra8_err_t ra8_fs_probe(const ra8_fs_backend_t* backend, ra8_fs_type_t* out_type)
{
  priv_lock_acquire();
  const ra8_err_t err = internal_probe_locked(backend, out_type);
  priv_lock_release();
  return err;
}

RA8_OWNS_RESOURCE("ra8_fs_lock")
ra8_err_t ra8_fs_mount(const ra8_fs_backend_t* backend, ra8_fs_mount_t** out_handle)
{
  priv_lock_acquire();
  const ra8_err_t err =
    internal_mount_locked(backend, (uint8_t)k_ra8_fs_partition_auto, out_handle);
  priv_lock_release();
  return err;
}

RA8_OWNS_RESOURCE("ra8_fs_lock")
ra8_err_t
ra8_fs_mount_partition(const ra8_fs_backend_t* backend, uint8_t index, ra8_fs_mount_t** out_handle)
{
  priv_lock_acquire();
  const ra8_err_t err = internal_mount_locked(backend, index, out_handle);
  priv_lock_release();
  return err;
}

RA8_OWNS_RESOURCE("ra8_fs_lock")
ra8_err_t ra8_fs_unmount(ra8_fs_mount_t* handle)
{
  priv_lock_acquire();
  const ra8_err_t err = internal_unmount_locked(handle);
  priv_lock_release();
  return err;
}
