/**
 * @file ra8_fs_fat_mount.c
 * @brief FAT/exFAT volume detection, geometry, and mount/unmount.
 *
 * @details
 * Slot allocation, BPB parsing, MBR partition location, volume-type
 * detection, and the public mount/unmount/format entry points. The GPT half
 * of partition location lives in `ra8_fs_fat_gpt.c` (it outgrew this file
 * when LBAs went 64-bit, #683); the two locators are called from here.
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

/* =============================================================================
 * Module state
 * =============================================================================
 */

/** @brief Mount table -- max `k_ra8_fs_max_mounts` simultaneous volumes. */
static ra8_fs_mount_t s_mounts[k_ra8_fs_max_mounts] = {};

/** @brief File handle table -- max `k_ra8_fs_max_files` open at once. */
static ra8_fs_file_t s_files[k_ra8_fs_max_files] = {};

/**
 * @brief Single max-sector scratch buffer reused across all I/O.
 * @details Shared by every FAT/exFAT translation unit; declared `extern` in
 *          `ra8_fs_fat_internal.h` and defined here exactly once. Sized to
 *          ::k_ra8_fs_sector_max so a 4Kn medium fits without allocation.
 */
RA8_PRIV uint8_t g_fs_scratch[k_ra8_fs_sector_max] = {};

/**
 * @var s_sec_arena
 * @brief The four fixed-role sector bounce buffers (#683).
 * @details Backing storage for ::priv_sec_walk / ::priv_sec_fat /
 *          ::priv_sec_fat2 / ::priv_sec_io -- see the arena discipline in
 *          `ra8_fs_fat_bytes_internal.h`. Static because 4 KiB frames do not
 *          belong on firmware stacks; four buffers because that is the whole
 *          simultaneous-liveness depth of the call graph.
 * @note Not reentrant; the adapter is single-threaded by contract.
 * @warning Never index directly; take a buffer through its role accessor.
 * @since 0.1.0
 */
static uint8_t s_sec_arena[k_fs_sec_roles][k_ra8_fs_sector_max] = {};

/** @brief Implementation of `k_zero_sector` -- see the header contract. */
const uint8_t k_zero_sector[k_ra8_fs_sector_max] = {};

/* `priv_sec_walk()`: see header for the documented contract. */
uint8_t* priv_sec_walk(void)
{
  return s_sec_arena[k_fs_sec_role_walk];
}

/* `priv_sec_fat()`: see header for the documented contract. */
uint8_t* priv_sec_fat(void)
{
  return s_sec_arena[k_fs_sec_role_fat];
}

/* `priv_sec_fat2()`: see header for the documented contract. */
uint8_t* priv_sec_fat2(void)
{
  return s_sec_arena[k_fs_sec_role_fat2];
}

/* `priv_sec_io()`: see header for the documented contract. */
uint8_t* priv_sec_io(void)
{
  return s_sec_arena[k_fs_sec_role_io];
}

/* =============================================================================
 * Slot allocation
 * =============================================================================
 */

/**
 * @brief Allocate a free entry from the mount table; returns NULL if full.
 *
 * @details Linear scan of `s_mounts` for an entry with `in_use == 0`.
 *
 * @return Pointer to a free mount slot, or NULL if all are busy.
 * @retval non-NULL Pointer to a `ra8_fs_mount_t` with `in_use == 0`.
 * @retval NULL     Mount table is full.
 *
 * @pre Module is initialized.
 * @pre Caller serialises mount/unmount operations.
 * @post No state modified.
 * @post Returned pointer remains valid for the program lifetime.
 *
 * @note Not thread-safe; callers serialise.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_fs_mount_t* internal_alloc_mount_slot(void)
{
  for (uint32_t i = 0; i < k_ra8_fs_max_mounts; i++) {
    if (s_mounts[i].in_use == 0U) {
      return &s_mounts[i];
    }
  }
  return nullptr;
}

/* `priv_alloc_file_slot()`: see header for the documented contract. */
ra8_fs_file_t* priv_alloc_file_slot(void)
{
  for (uint32_t i = 0; i < k_ra8_fs_max_files; i++) {
    if (s_files[i].in_use == 0U) {
      return &s_files[i];
    }
  }
  return nullptr;
}

/* =============================================================================
 * Public API: mount / unmount
 * =============================================================================
 */

/**
 * @brief Test whether a backend block size is one this adapter supports.
 *
 * @details The supported sizes are the powers of two from
 *          ::k_ra8_fs_sector_min to ::k_ra8_fs_sector_max (512, 1024, 2048,
 *          4096) -- exactly the values the FAT specification allows for
 *          `BPB_BytsPerSec` and exFAT allows for `BytesPerSectorShift`.
 *
 * @param[in] bs Backend-reported block size in bytes.
 *
 * @return 1 when @p bs is supported, else 0.
 * @retval 1 @p bs is a power of two in 512..4096.
 * @retval 0 Anything else.
 *
 * @pre None (total function).
 * @pre @p bs came from a backend `get_capacity` callback.
 * @post No state modified.
 * @post Result depends only on @p bs.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @note The range test is one compound decision of three conditions; its
 *       vectors live with the tests that drive it, cited as
 *       `libs/ra8_fs/src/ra8_fs_fat_mount.c@priv_bps_valid`.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static uint8_t internal_bps_valid(uint32_t bs)
{
  if ((bs < (uint32_t)k_ra8_fs_sector_min) || (bs > (uint32_t)k_ra8_fs_sector_max) ||
      ((bs & (bs - 1U)) != 0U)) {
    return 0U;
  }
  return 1U;
}

/* `priv_parse_bpb_into_mount()`: see header for the documented contract. */
ra8_err_t priv_parse_bpb_into_mount(ra8_fs_mount_t* m)
{
  if (g_fs_scratch[k_bpb_off_signature_lo] != k_bpb_sig_lo ||
      g_fs_scratch[k_bpb_off_signature_hi] != k_bpb_sig_hi) {
    return k_ra8_err_validation_failed;
  }
  const uint32_t bpb_bps = priv_rd16(&g_fs_scratch[k_bpb_off_bytes_per_sec]);
  m->sectors_per_cluster = (uint32_t)g_fs_scratch[k_bpb_off_sec_per_clus];
  m->reserved_sectors    = priv_rd16(&g_fs_scratch[k_bpb_off_rsvd_sec_cnt]);
  m->num_fats            = (uint32_t)g_fs_scratch[k_bpb_off_num_fats];
  m->root_entries        = priv_rd16(&g_fs_scratch[k_bpb_off_root_ent_cnt]);
  /* The BPB must agree with the DEVICE: `m->bytes_per_sector` was seeded from
   * the backend's reported block size before this parse ran, and a volume
   * formatted for a different sector size than the medium presents (a 512e
   * image on a 4Kn device, or vice versa) is unmountable, not reinterpretable
   * (#683). Every supported size passes ::priv_bps_valid at mount, so the
   * comparison also enforces 512..4096.

     MC/DC: the three-condition guard's vectors live with the tests that drive
     it, cited as `libs/ra8_fs/src/ra8_fs_fat_mount.c@priv_parse_bpb_into_mount`. */
  if (bpb_bps != m->bytes_per_sector || m->sectors_per_cluster == 0U || m->num_fats == 0U) {
    return k_ra8_err_validation_failed;
  }
  const uint32_t fat_sz_16  = priv_rd16(&g_fs_scratch[k_bpb_off_fat_sz_16]);
  const uint32_t fat_sz_32  = priv_rd32(&g_fs_scratch[k_bpb_off_fat_sz_32]);
  const uint32_t tot_sec_16 = priv_rd16(&g_fs_scratch[k_bpb_off_tot_sec_16]);
  const uint32_t tot_sec_32 = priv_rd32(&g_fs_scratch[k_bpb_off_tot_sec_32]);
  m->fat_size_sectors       = (fat_sz_16 != 0U) ? fat_sz_16 : fat_sz_32;
  /* Chosen in 32 bits first: assigning the composite pick straight into the
   * 64-bit field would be a MISRA 10.6 composite-widening. */
  const uint32_t tot_sec = (tot_sec_16 != 0U) ? tot_sec_16 : tot_sec_32;
  m->total_sectors       = tot_sec;
  m->root_cluster        = priv_rd32(&g_fs_scratch[k_bpb_off_root_clus]);
  return k_ra8_ok;
}

/**
 * @brief Compute first_fat_lba, first_root_lba, first_data_lba, count_of_clusters.
 *
 * @details Derives region LBAs from the BPB and chooses FAT type using
 *          the cluster-count thresholds in MS FAT spec sec 3.5.
 *
 * @param[in,out] m Mount with BPB fields already populated.
 *
 * @return Error code.
 * @retval k_ra8_ok                     Geometry computed.
 * @retval k_ra8_err_validation_failed  Total sectors smaller than data start.
 *
 * @pre `m` is non-NULL.
 * @pre `priv_parse_bpb_into_mount` has populated the BPB-derived fields.
 * @post On success, geometry fields and `m->type` are valid.
 * @post On failure, `m` is left in an inconsistent state.
 *
 * @note Pure computation; thread-safe vs other readers.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_compute_geometry(ra8_fs_mount_t* m)
{
  m->first_fat_lba = m->reserved_sectors;
  const uint32_t root_dir_sectors =
    ((m->root_entries * k_ra8_fs_dir_entry_bytes) + (priv_bps(m) - 1U)) / priv_bps(m);
  m->first_root_lba = m->first_fat_lba + ((uint64_t)m->num_fats * m->fat_size_sectors);
  m->first_data_lba = m->first_root_lba + root_dir_sectors;
  if (m->total_sectors < m->first_data_lba) {
    return k_ra8_err_validation_failed;
  }
  const uint64_t data_sectors = m->total_sectors - m->first_data_lba;
  m->count_of_clusters        = (uint32_t)(data_sectors / m->sectors_per_cluster);
  if (m->count_of_clusters < k_cluster_count_fat12_max) {
    m->type = k_ra8_fs_type_fat12;
  } else if (m->count_of_clusters < k_cluster_count_fat16_max) {
    m->type = k_ra8_fs_type_fat16;
  } else {
    m->type = k_ra8_fs_type_fat32;
  }
  return k_ra8_ok;
}

/**
 * @brief Return partition 0's first LBA from an MBR in @p buf, or 0.
 *
 * @details Used only after the LBA-0 BPB parse fails: a standard SD card is
 * MBR-partitioned, so the FAT boot sector lives at partition 0's start LBA,
 * not at LBA 0. Returns 0 when @p buf is not a usable MBR (no 0x55AA, or an
 * unused partition 0), which leaves the caller's original error in force.
 *
 * @param[in] buf Sector-0 contents (>= 512 bytes).
 * @return Partition 0 first LBA, or 0 if not an MBR with a live partition 0.
 * @retval 0 ``buf`` is not a usable MBR (no 0x55AA or an unused partition 0).
 * @retval non-zero Partition 0's first LBA.
 * @pre @p buf is non-NULL and holds at least one sector.
 * @pre @p buf is the contents of LBA 0.
 * @post No state modified.
 * @post @p buf is unmodified.
 * @note Not thread-safe (reads caller-owned memory only).
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_mbr_part0_lba(const uint8_t* buf)
{
  if (buf[k_bpb_off_signature_lo] != (uint8_t)k_bpb_sig_lo) {
    return 0U;
  }
  if (buf[k_bpb_off_signature_hi] != (uint8_t)k_bpb_sig_hi) {
    return 0U;
  }
  if (buf[k_mbr_off_part0_type] == 0U) {
    return 0U;
  }
  return priv_rd32(&buf[k_mbr_off_part0_lba]);
}

/**
 * @brief Return the first LBA of MBR primary entry @p index.
 *
 * @details Enumerates the four 16-byte primary entries. The type byte and
 * first-LBA of entry @p index are read at ::k_mbr_off_part0_type and
 * ::k_mbr_off_part0_lba plus @p index times ::k_mbr_part_entry_stride. Unlike
 * the auto path (::priv_mbr_part0_lba), an out-of-range index, an empty slot,
 * or a zero first-LBA are reported distinctly rather than folded to 0.
 *
 * @param[in]  buf      Sector-0 (MBR) contents (>= 512 bytes).
 * @param[in]  index    Zero-based primary-entry index.
 * @param[out] out_base Receives the entry's first LBA on success.
 * @return Error code.
 * @retval k_ra8_ok                    @p out_base holds the entry's first LBA.
 * @retval k_ra8_err_out_of_range      @p index is 4 or greater.
 * @retval k_ra8_err_not_found         The selected entry is unused (type 0).
 * @retval k_ra8_err_validation_failed Allocated entry with a zero first LBA.
 * @pre @p buf is non-NULL and holds at least one sector.
 * @pre @p out_base is non-NULL.
 * @post No state modified.
 * @post @p buf is unmodified.
 * @note Pure function.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_mbr_select_entry(const uint8_t* buf, uint8_t index, uint64_t* out_base)
{
  if ((uint32_t)index >= (uint32_t)k_mbr_part_entry_count) {
    return k_ra8_err_out_of_range;
  }
  const uint32_t slot     = (uint32_t)index * (uint32_t)k_mbr_part_entry_stride;
  const uint32_t type_off = (uint32_t)k_mbr_off_part0_type + slot;
  const uint32_t lba_off  = (uint32_t)k_mbr_off_part0_lba + slot;
  if (buf[type_off] == 0U) {
    return k_ra8_err_not_found;
  }
  const uint32_t base = priv_rd32(&buf[lba_off]);
  if (base == 0U) {
    return k_ra8_err_validation_failed;
  }
  *out_base = base;
  return k_ra8_ok;
}

/**
 * @brief Resolve an explicit partition index to its first LBA.
 *
 * @details Dispatches on the sector-0 partition-table type: a GPT disk
 * (protective MBR type ::k_gpt_part_type_protective) selects GPT entry
 * @p index, any other 0xAA55 MBR selects primary entry @p index. A sector 0
 * that carries no boot signature is not a partition table at all, so an
 * explicit index has nothing to select and the caller's superfloppy request is
 * refused. The signature bytes are tested separately (not as one compound
 * decision) to mirror ::priv_mbr_part0_lba.
 *
 * @param[in,out] m        Mount with sector 0 already in ::g_fs_scratch.
 * @param[in]     index    Zero-based partition index.
 * @param[out]    out_base Receives the selected partition's first LBA.
 * @return Error code.
 * @retval k_ra8_ok                    @p out_base holds the volume base.
 * @retval k_ra8_err_not_found         No partition table, or an empty entry.
 * @retval k_ra8_err_out_of_range      @p index is past the table.
 * @retval k_ra8_err_not_supported     Unaddressable GPT geometry.
 * @retval k_ra8_err_validation_failed Malformed entry or GPT header.
 * @retval k_ra8_err_*                 Backend read failure.
 * @pre ::g_fs_scratch holds the contents of LBA 0.
 * @pre ``m->partition_base_lba`` is still 0 (reads are absolute).
 * @pre @p out_base is non-NULL.
 * @post On k_ra8_ok @p out_base holds a non-zero LBA.
 * @post ::g_fs_scratch may be overwritten (GPT path re-reads the entry array).
 * @note Not thread-safe -- uses module-level scratch.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_locate_indexed(ra8_fs_mount_t* m, uint8_t index, uint64_t* out_base)
{
  if (g_fs_scratch[k_bpb_off_signature_lo] != (uint8_t)k_bpb_sig_lo) {
    return k_ra8_err_not_found;
  }
  if (g_fs_scratch[k_bpb_off_signature_hi] != (uint8_t)k_bpb_sig_hi) {
    return k_ra8_err_not_found;
  }
  if (g_fs_scratch[k_mbr_off_part0_type] == (uint8_t)k_gpt_part_type_protective) {
    return priv_gpt_locate_partition(m, index, out_base);
  }
  return internal_mbr_select_entry(g_fs_scratch, index, out_base);
}

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

/**
 * @brief Read + parse the boot sector, transparently following MBR or GPT.
 *
 * @details When @p index is ::k_ra8_fs_partition_auto this tries the BPB/VBR at
 * the current base (LBA 0 for a superfloppy); if that parse fails and sector 0
 * is an MBR it retargets to partition 0's start LBA, or -- for a protective MBR
 * (type 0xEE) -- to the GPT partition the entry array selects (Basic Data
 * preferred), then re-parses, leaving the original error in force when sector 0
 * is neither a volume, an MBR, nor a usable GPT. For any other @p index the
 * superfloppy attempt is skipped and the chosen primary/GPT entry supplies the
 * base directly (::priv_locate_indexed); an unaddressable or empty entry fails
 * cleanly.
 *
 * @param[in,out] m     Mount with backend bound and ``partition_base_lba == 0``.
 * @param[in]     index Partition selector, or ::k_ra8_fs_partition_auto.
 * @return Error code from the (re-)parse.
 * @retval k_ra8_ok ``m`` holds the volume's BPB fields and base LBA.
 * @retval k_ra8_err_* The backend read failed or no volume was found.
 * @pre ``m->partition_base_lba`` is 0 on entry.
 * @pre ``m->backend`` is bound with a valid ``read_block``.
 * @post On success ``m`` holds the volume's BPB fields and base LBA.
 * @post On failure ``m`` is left unmounted.
 * @note Not thread-safe; serialize mount operations.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_read_boot_sector(ra8_fs_mount_t* m, uint8_t index)
{
  ra8_err_t err = priv_read_sector(m, 0, g_fs_scratch);
  if (err != k_ra8_ok) {
    return err;
  }
  uint64_t base = 0U;
  if (index != (uint8_t)k_ra8_fs_partition_auto) {
    const ra8_err_t lerr = internal_locate_indexed(m, index, &base);
    if (lerr != k_ra8_ok) {
      return lerr;
    }
  } else {
    err = priv_parse_volume(m);
    if (err == k_ra8_ok) {
      return k_ra8_ok;
    }
    base = internal_mbr_part0_lba(g_fs_scratch);
    if (base == 0U) {
      return err;
    }
    if (g_fs_scratch[k_mbr_off_part0_type] == (uint8_t)k_gpt_part_type_protective) {
      const ra8_err_t gpt_err = priv_gpt_locate_volume(m, &base);
      if (gpt_err != k_ra8_ok) {
        return gpt_err;
      }
    }
  }
  m->partition_base_lba = base;
  err                   = priv_read_sector(m, 0, g_fs_scratch);
  if (err != k_ra8_ok) {
    return err;
  }
  return priv_parse_volume(m);
}

/**
 * @brief Probe the backend's block size into a claimed mount slot.
 *
 * @details The sector size is the DEVICE's block size, probed before the
 *          boot-sector parse so the parse can hold the volume to it (#683). A
 *          backend reporting a size outside 512..4096, or one that is not a
 *          power of two, is not mountable. Split out of ::priv_mount_locked
 *          so that function stays inside the function-size gate.
 *
 * @param[in]     backend Block-device backend being mounted.
 * @param[in,out] m       Claimed mount slot; `bytes_per_sector` is written.
 *
 * @return Error code.
 * @retval k_ra8_ok              `m->bytes_per_sector` holds the device size.
 * @retval k_ra8_err_invalid_arg The reported block size is unsupported.
 * @retval k_ra8_err_*           The `get_capacity` callback failed.
 *
 * @pre @p backend and @p m are non-NULL; `get_capacity` is non-NULL.
 * @pre @p m was zeroed by the claim.
 * @post On success `m->bytes_per_sector` is a power of two in 512..4096.
 * @post On failure @p m is left for the caller to abandon.
 *
 * @note Not thread-safe; callers serialise mount operations.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_mount_probe_bps(const ra8_fs_backend_t* backend, ra8_fs_mount_t* m)
{
  uint64_t        dev_blocks = 0U;
  uint32_t        dev_bps    = 0U;
  const ra8_err_t cerr       = backend->get_capacity(backend->ctx, &dev_blocks, &dev_bps);
  if (cerr != k_ra8_ok) {
    return cerr;
  }
  if (internal_bps_valid(dev_bps) == 0U) {
    return k_ra8_err_invalid_arg;
  }
  m->bytes_per_sector = dev_bps;
  return k_ra8_ok;
}

/**
 * @brief Parse a backend into temporary state and return only its format type.
 *
 * @param[in]  backend  Readable backend to probe.
 * @param[out] out_type Detected filesystem type.
 * @return Error code from argument, capacity, partition, or volume validation.
 * @pre The library lock is held (or none is installed).
 * @post No mount/allocator slot is claimed and the medium is not written.
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_EXPECTS_LOCK("ra8_fs_lock")
static ra8_err_t internal_probe_locked(const ra8_fs_backend_t* backend, ra8_fs_type_t* out_type)
{
  if (backend == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (out_type == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (backend->read_block == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if (backend->get_capacity == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  ra8_fs_mount_t probe = {};
  probe.backend        = *backend;
  ra8_err_t err        = internal_mount_probe_bps(backend, &probe);
  if (err != k_ra8_ok) {
    return err;
  }
  err = internal_read_boot_sector(&probe, (uint8_t)k_ra8_fs_partition_auto);
  if (err != k_ra8_ok) {
    return err;
  }
  if (probe.type != k_ra8_fs_type_exfat) {
    err = internal_compute_geometry(&probe);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  *out_type = probe.type;
  return k_ra8_ok;
}

/**
 * @brief Mount a volume -- the guarded body of ::ra8_fs_mount() and
 *        ::ra8_fs_mount_partition().
 *
 * @details Allocates a mount slot, reads the boot sector for the requested
 *          @p index (::k_ra8_fs_partition_auto for first-partition auto-select),
 *          parses the BPB, and computes the geometry.
 *
 * @param[in]  backend    Block-device backend to drive.
 * @param[in]  index      Partition selector, or ::k_ra8_fs_partition_auto.
 * @param[out] out_handle On success, opaque mount handle.
 *
 * @return Error code.
 * @retval k_ra8_ok                     Volume mounted.
 * @retval k_ra8_err_null_ptr           NULL `backend` or `out_handle`.
 * @retval k_ra8_err_invalid_arg        Backend missing required callbacks.
 * @retval k_ra8_err_no_mem             Mount table is full.
 * @retval k_ra8_err_out_of_range       `index` is past the partition table.
 * @retval k_ra8_err_not_found          Selected entry empty, or no table.
 * @retval k_ra8_err_validation_failed  Not a recognisable FAT volume.
 * @retval k_ra8_err_*                  Backend read failure.
 *
 * @pre The library lock is held (or none is installed).
 * @pre `backend` and `out_handle` are non-NULL.
 * @pre Backend's read/write/get_capacity callbacks are non-NULL.
 * @post On success, `*out_handle` is a valid mount.
 * @post On failure, no mount slot is marked in-use.
 *
 * @note Not thread-safe; callers serialise.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_EXPECTS_LOCK("ra8_fs_lock")
static ra8_err_t
internal_mount_locked(const ra8_fs_backend_t* backend, uint8_t index, ra8_fs_mount_t** out_handle)
{
  if (backend == nullptr || out_handle == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (backend->read_block == nullptr || backend->write_block == nullptr ||
      backend->get_capacity == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  ra8_fs_mount_t* m = internal_alloc_mount_slot();
  if (m == nullptr) {
    return k_ra8_err_no_mem;
  }
  /* Zero the whole slot on claim (#684). The mount table is a fixed array, so
   * this ra8_fs_mount_t address has very likely served another volume already,
   * and the FAT and exFAT parse paths each populate a DIFFERENT subset of the
   * struct. Clearing every field here makes "repopulate everything" a structural
   * invariant instead of something each future field author must remember: no
   * value can survive a card-type change into a field the new parse never
   * touches. `in_use` is set last, on success. */
  *m                    = (ra8_fs_mount_t){};
  m->backend            = *backend;
  m->partition_base_lba = 0U;
  const ra8_err_t cerr  = internal_mount_probe_bps(backend, m);
  if (cerr != k_ra8_ok) {
    return cerr;
  }
  /* The mount table is a fixed array, so this is very likely the slot some
   * earlier volume used. Both fields below are reset for the same reason the
   * allocator's own state is (see ra8_fs_fat_alloc_internal.h): a slot handed
   * out again carrying the last volume's answers hands them to this one.
   * `exfat_upcase_ok` says "this build's up-case table is the one this VOLUME
   * carries", and a fresh mount vouches for nothing until it has looked (#606). */
  m->exfat_upcase_ok = 0U;
  ra8_err_t err      = internal_read_boot_sector(m, index);
  if (err != k_ra8_ok) {
    return err;
  }
  /* exFAT geometry is parsed directly in priv_exfat_parse; the FAT-BPB
   * geometry computation applies only to FAT12/16/32. */
  if (m->type != k_ra8_fs_type_exfat) {
    err = internal_compute_geometry(m);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  /* Claim a fresh allocator slot BEFORE seeding it: the mount table is a fixed
   * array, so this ra8_fs_mount_t address has very likely served another volume
   * already, and its cached FAT sector and next-free hint describe that disk. */
  priv_alloc_state_bind(m);
  err = priv_fsinfo_seed(m);
  if (err != k_ra8_ok) {
    priv_alloc_state_release(m);
    return err;
  }
  m->in_use   = 1;
  *out_handle = m;
  return k_ra8_ok;
}

/**
 * @brief Release a mount slot -- the guarded body of ::ra8_fs_unmount().
 *
 * @details Writes the FAT32 FSInfo free count back if anything was allocated
 *          or freed, drops the allocator state, and marks the mount slot free.
 *          File data is not buffered, so there is nothing else to flush --
 *          callers must still close all files first.
 *
 * @param[in] handle Mount handle from `ra8_fs_mount()`.
 *
 * @return Error code.
 * @retval k_ra8_ok                Volume unmounted.
 * @retval k_ra8_err_null_ptr      `handle` was NULL.
 * @retval k_ra8_err_invalid_state `handle` is not currently mounted.
 * @retval k_ra8_err_*             The FSInfo writeback failed; the volume is
 *                                 still unmounted.
 *
 * @pre The library lock is held (or none is installed).
 * @pre `handle` is non-NULL and currently in use.
 * @pre All files opened on this mount have been closed.
 * @post Mount slot is free for reuse, whatever the FSInfo writeback did.
 * @post `handle->type` is reset to `k_ra8_fs_type_unknown`.
 *
 * @note Not thread-safe; callers serialise.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_EXPECTS_LOCK("ra8_fs_lock")
static ra8_err_t internal_unmount_locked(ra8_fs_mount_t* handle)
{
  if (handle == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (handle->in_use == 0U) {
    return k_ra8_err_invalid_state;
  }
  /* Last chance to make the on-disk free count true (#607). Reported, not
   * swallowed: a card whose FSInfo could not be updated is a card a desktop
   * will report the wrong free space for, and the caller is the only one who
   * can decide whether that matters. The slot is released either way, so a
   * failed flush cannot strand the mount. */
  const ra8_err_t ferr = priv_fsinfo_flush(handle);
  priv_alloc_state_release(handle);
  handle->in_use = 0;
  handle->type   = k_ra8_fs_type_unknown;
  return ferr;
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
