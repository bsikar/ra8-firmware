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
extern ra8_err_t internal_unmount_locked(ra8_fs_mount_t* handle);extern ra8_err_t internal_format_locked(const ra8_fs_backend_t* backend,
                                        const ra8_fs_format_opts_t* opts);




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
