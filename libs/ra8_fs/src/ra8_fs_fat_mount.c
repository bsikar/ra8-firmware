/**
 * @file ra8_fs_fat_mount.c
 * @brief FAT/exFAT volume detection, geometry, and mount/unmount.
 *
 * @details
 * Slot allocation, BPB parsing, MBR/GPT partition location, volume-type
 * detection, and the public mount/unmount/format entry points.
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
 * @brief Single 512-byte sector scratch buffer reused across all I/O.
 * @details Shared by every FAT/exFAT translation unit; declared `extern` in
 *          `ra8_fs_fat_internal.h` and defined here exactly once.
 */
uint8_t s_scratch[k_ra8_fs_bytes_per_sector] = {};

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
static ra8_fs_mount_t* priv_alloc_mount_slot(void)
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

/* `priv_parse_bpb_into_mount()`: see header for the documented contract. */
ra8_err_t priv_parse_bpb_into_mount(ra8_fs_mount_t* m)
{
  if (s_scratch[k_bpb_off_signature_lo] != k_bpb_sig_lo ||
      s_scratch[k_bpb_off_signature_hi] != k_bpb_sig_hi) {
    return k_ra8_err_validation_failed;
  }
  m->bytes_per_sector    = priv_rd16(&s_scratch[k_bpb_off_bytes_per_sec]);
  m->sectors_per_cluster = (uint32_t)s_scratch[k_bpb_off_sec_per_clus];
  m->reserved_sectors    = priv_rd16(&s_scratch[k_bpb_off_rsvd_sec_cnt]);
  m->num_fats            = (uint32_t)s_scratch[k_bpb_off_num_fats];
  m->root_entries        = priv_rd16(&s_scratch[k_bpb_off_root_ent_cnt]);
  if (m->bytes_per_sector != k_ra8_fs_bytes_per_sector || m->sectors_per_cluster == 0U ||
      m->num_fats == 0U) {
    return k_ra8_err_validation_failed;
  }
  const uint32_t fat_sz_16  = priv_rd16(&s_scratch[k_bpb_off_fat_sz_16]);
  const uint32_t fat_sz_32  = priv_rd32(&s_scratch[k_bpb_off_fat_sz_32]);
  const uint32_t tot_sec_16 = priv_rd16(&s_scratch[k_bpb_off_tot_sec_16]);
  const uint32_t tot_sec_32 = priv_rd32(&s_scratch[k_bpb_off_tot_sec_32]);
  m->fat_size_sectors       = (fat_sz_16 != 0U) ? fat_sz_16 : fat_sz_32;
  m->total_sectors          = (tot_sec_16 != 0U) ? tot_sec_16 : tot_sec_32;
  m->root_cluster           = priv_rd32(&s_scratch[k_bpb_off_root_clus]);
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
static ra8_err_t priv_compute_geometry(ra8_fs_mount_t* m)
{
  m->first_fat_lba = m->reserved_sectors;
  const uint32_t root_dir_sectors =
    ((m->root_entries * k_ra8_fs_dir_entry_bytes) + (k_ra8_fs_bytes_per_sector - 1U)) /
    k_ra8_fs_bytes_per_sector;
  m->first_root_lba = m->first_fat_lba + (m->num_fats * m->fat_size_sectors);
  m->first_data_lba = m->first_root_lba + root_dir_sectors;
  if (m->total_sectors < m->first_data_lba) {
    return k_ra8_err_validation_failed;
  }
  const uint32_t data_sectors = m->total_sectors - m->first_data_lba;
  m->count_of_clusters        = data_sectors / m->sectors_per_cluster;
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
static uint32_t priv_mbr_part0_lba(const uint8_t* buf)
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

/** @brief GPT header signature ("EFI PART", UEFI spec 2.10 table 5.5). */
static const uint8_t k_gpt_signature[k_gpt_sig_len] = {
  0x45U,
  0x46U,
  0x49U,
  0x20U,
  0x50U,
  0x41U,
  0x52U,
  0x54U,
};

/** @brief Microsoft Basic Data type GUID, on-disk byte order
 *         (EBD0A0A2-B9E5-4433-87C0-68B6B72699C7). */
static const uint8_t k_gpt_guid_basic_data[k_gpt_guid_len] = {
  0xA2U,
  0xA0U,
  0xD0U,
  0xEBU,
  0xE5U,
  0xB9U,
  0x33U,
  0x44U,
  0x87U,
  0xC0U,
  0x68U,
  0xB6U,
  0xB7U,
  0x26U,
  0x99U,
  0xC7U,
};

/**
 * @brief Extract a usable first-LBA from one GPT partition entry.
 *
 * @details An entry is usable when its type GUID is non-zero (the slot is
 * allocated) and its 64-bit first LBA fits in 32 bits (this backend
 * addresses 512-byte blocks with 32-bit LBAs, i.e. disks up to 2 TiB).
 *
 * @param[in] entry One 128-byte partition entry.
 * @return The entry's first LBA, or 0 when the entry is unusable.
 * @retval 0 Unused slot, zero first-LBA, or first-LBA above 32 bits.
 * @pre @p entry is non-NULL and holds ::k_gpt_entry_bytes bytes.
 * @pre @p entry came from the GPT partition entry array.
 * @post No state modified.
 * @post @p entry is unmodified.
 * @note Pure function.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t priv_gpt_entry_first_lba(const uint8_t* entry)
{
  uint32_t nonzero = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_gpt_guid_len; i++) {
    if (entry[i] != 0U) {
      nonzero = 1U;
    }
  }
  if (nonzero == 0U) {
    return 0U;
  }
  if (priv_rd32(&entry[(uint32_t)k_gpt_entry_off_first_lba + (uint32_t)k_gpt_u64_hi_off]) != 0U) {
    return 0U;
  }
  return priv_rd32(&entry[k_gpt_entry_off_first_lba]);
}

/**
 * @brief Test whether a GPT entry's type GUID is Microsoft Basic Data.
 *
 * @details Basic Data is where FAT/exFAT user volumes live on a GPT disk;
 * other common entries (EFI System Partition, Microsoft Reserved) do not
 * carry the volume this layer should mount.
 *
 * @param[in] entry One 128-byte partition entry.
 * @return 1 when the type GUID matches, else 0.
 * @retval 1 The entry is a Basic Data partition.
 * @pre @p entry is non-NULL and holds ::k_gpt_entry_bytes bytes.
 * @pre @p entry came from the GPT partition entry array.
 * @post No state modified.
 * @post @p entry is unmodified.
 * @note Pure function.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t priv_gpt_entry_is_basic_data(const uint8_t* entry)
{
  for (uint32_t i = 0U; i < (uint32_t)k_gpt_guid_len; i++) {
    if (entry[i] != k_gpt_guid_basic_data[i]) {
      return 0U;
    }
  }
  return 1U;
}

/**
 * @brief Fold one GPT entry into the candidate base-LBA bookkeeping.
 *
 * @details Prefers the first Microsoft Basic Data entry (the conventional
 * home of FAT/exFAT volumes); the first allocated entry of any other type
 * is kept as a fallback. Unusable entries (see
 * ::priv_gpt_entry_first_lba) are ignored.
 *
 * @param[in]     entry     One 128-byte partition entry.
 * @param[in,out] basic_lba First Basic Data candidate (0 = none yet).
 * @param[in,out] any_lba   First allocated-entry candidate (0 = none yet).
 * @pre @p entry is non-NULL and holds ::k_gpt_entry_bytes bytes.
 * @pre @p basic_lba and @p any_lba are non-NULL.
 * @post Candidates are updated only from 0 (first match wins).
 * @post @p entry is unmodified.
 * @note Pure bookkeeping; no I/O.
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_gpt_note_entry(const uint8_t* entry, uint32_t* basic_lba, uint32_t* any_lba)
{
  const uint32_t first = priv_gpt_entry_first_lba(entry);
  if (first == 0U) {
    return;
  }
  if (*any_lba == 0U) {
    *any_lba = first;
  }
  if (*basic_lba == 0U) {
    if (priv_gpt_entry_is_basic_data(entry) != 0U) {
      *basic_lba = first;
    }
  }
}

/**
 * @brief Read the GPT entry array sector-by-sector and pick a base LBA.
 *
 * @details Reads each entry sector once and feeds every 128-byte entry to
 * ::priv_gpt_note_entry; the Basic Data candidate wins over the first
 * allocated entry of any other type.
 *
 * @param[in,out] m         Mount whose backend supplies the sectors.
 * @param[in]     entry_lba First LBA of the partition entry array.
 * @param[in]     count     Number of entries to scan (already clamped).
 * @param[out]    out_base  Receives the chosen partition's first LBA.
 * @return Error code.
 * @retval k_ra8_ok            A candidate partition was found.
 * @retval k_ra8_err_not_found No allocated entry was usable.
 * @retval k_ra8_err_*         Backend read failure.
 * @pre ``m->partition_base_lba`` is still 0 (reads are absolute).
 * @pre @p out_base is non-NULL.
 * @post On k_ra8_ok @p out_base holds a non-zero LBA.
 * @post ::s_scratch holds the last entry sector read.
 * @note Not thread-safe -- uses module-level scratch.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
priv_gpt_scan_entries(ra8_fs_mount_t* m, uint32_t entry_lba, uint32_t count, uint32_t* out_base)
{
  uint32_t basic_lba = 0U;
  uint32_t any_lba   = 0U;
  for (uint32_t i = 0U; i < count; i++) {
    const uint32_t sector = i / (uint32_t)k_gpt_entries_per_sector;
    const uint32_t offset = (i % (uint32_t)k_gpt_entries_per_sector) * (uint32_t)k_gpt_entry_bytes;
    if (offset == 0U) {
      const ra8_err_t err = priv_read_sector(m, entry_lba + sector, s_scratch);
      if (err != k_ra8_ok) {
        return err;
      }
    }
    priv_gpt_note_entry(&s_scratch[offset], &basic_lba, &any_lba);
  }
  if (basic_lba != 0U) {
    *out_base = basic_lba;
    return k_ra8_ok;
  }
  if (any_lba != 0U) {
    *out_base = any_lba;
    return k_ra8_ok;
  }
  return k_ra8_err_not_found;
}

/**
 * @brief Locate the mountable partition on a GPT disk.
 *
 * @details Validates the "EFI PART" header at LBA 1, bounds-checks the
 * entry-array geometry (128-byte entries, 32-bit array LBA), then scans the
 * entries via ::priv_gpt_scan_entries.
 *
 * @param[in,out] m        Mount whose backend supplies the sectors.
 * @param[out]    out_base Receives the chosen partition's first LBA.
 * @return Error code.
 * @retval k_ra8_ok                    @p out_base holds the volume base.
 * @retval k_ra8_err_validation_failed No "EFI PART" header at LBA 1.
 * @retval k_ra8_err_not_supported     Non-standard entry size or array LBA.
 * @retval k_ra8_err_*                 Backend read failure or no entry found.
 * @pre ``m->partition_base_lba`` is still 0 (reads are absolute).
 * @pre @p out_base is non-NULL.
 * @post On k_ra8_ok @p out_base holds a non-zero LBA.
 * @post ::s_scratch is overwritten (callers must re-read their sector).
 * @note Not thread-safe -- uses module-level scratch.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_gpt_locate_volume(ra8_fs_mount_t* m, uint32_t* out_base)
{
  ra8_err_t err = priv_read_sector(m, (uint32_t)k_gpt_header_lba, s_scratch);
  if (err != k_ra8_ok) {
    return err;
  }
  for (uint32_t i = 0U; i < (uint32_t)k_gpt_sig_len; i++) {
    if (s_scratch[i] != k_gpt_signature[i]) {
      return k_ra8_err_validation_failed;
    }
  }
  if (priv_rd32(&s_scratch[(uint32_t)k_gpt_off_entry_lba + (uint32_t)k_gpt_u64_hi_off]) != 0U) {
    return k_ra8_err_not_supported;
  }
  const uint32_t entry_lba  = priv_rd32(&s_scratch[k_gpt_off_entry_lba]);
  const uint32_t entry_size = priv_rd32(&s_scratch[k_gpt_off_entry_size]);
  uint32_t       count      = priv_rd32(&s_scratch[k_gpt_off_entry_count]);
  if (entry_lba == 0U) {
    return k_ra8_err_validation_failed;
  }
  if (entry_size != (uint32_t)k_gpt_entry_bytes) {
    return k_ra8_err_not_supported;
  }
  if (count > (uint32_t)k_gpt_entry_scan_max) {
    count = (uint32_t)k_gpt_entry_scan_max;
  }
  return priv_gpt_scan_entries(m, entry_lba, count, out_base);
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
static ra8_err_t priv_format_locked(const ra8_fs_backend_t*     backend,
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
  uint32_t  block_count = 0U;
  uint32_t  block_size  = 0U;
  ra8_err_t err         = backend->get_capacity(backend->ctx, &block_count, &block_size);
  if (err != k_ra8_ok) {
    return err;
  }
  if (block_size != (uint32_t)k_ra8_fs_bytes_per_sector || block_count == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (opts->type == k_ra8_fs_type_exfat) {
    return priv_exfat_format(backend, block_count, opts->label);
  }
  ra8_fs_fmt_geom_t geom = {};
  geom.type              = opts->type;
  geom.total_sectors     = block_count;
  geom.reserved_sectors  = priv_fmt_reserved_for(opts->type);
  geom.root_entries      = (opts->type == k_ra8_fs_type_fat32) ? 0U : (uint32_t)k_fmt_root_ents_f16;
  geom.root_sectors      = ((geom.root_entries * (uint32_t)k_ra8_fs_dir_entry_bytes) +
                       ((uint32_t)k_ra8_fs_bytes_per_sector - 1U)) /
                      (uint32_t)k_ra8_fs_bytes_per_sector;
  err = priv_fmt_choose_geometry(&geom, opts->sectors_per_cluster);
  if (err != k_ra8_ok) {
    return err;
  }
  return priv_fmt_emit_volume(backend, &geom, opts->label);
}

/**
 * @brief Read + parse the boot sector, transparently following MBR or GPT.
 *
 * @details Tries the BPB/VBR at the current base (LBA 0 for a superfloppy).
 * If that parse fails and sector 0 is an MBR, retargets the mount: for a
 * plain MBR to partition 0's start LBA, for a protective MBR (type 0xEE) to
 * the partition chosen from the GPT entry array (Basic Data preferred), and
 * re-parses. Leaves the original error in force when sector 0 is neither a
 * volume, an MBR, nor a usable GPT.
 *
 * @param[in,out] m Mount with backend bound and ``partition_base_lba == 0``.
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
static ra8_err_t priv_read_boot_sector(ra8_fs_mount_t* m)
{
  ra8_err_t err = priv_read_sector(m, 0, s_scratch);
  if (err != k_ra8_ok) {
    return err;
  }
  err = priv_parse_volume(m);
  if (err == k_ra8_ok) {
    return k_ra8_ok;
  }
  uint32_t base = priv_mbr_part0_lba(s_scratch);
  if (base == 0U) {
    return err;
  }
  if (s_scratch[k_mbr_off_part0_type] == (uint8_t)k_gpt_part_type_protective) {
    const ra8_err_t gpt_err = priv_gpt_locate_volume(m, &base);
    if (gpt_err != k_ra8_ok) {
      return gpt_err;
    }
  }
  m->partition_base_lba = base;
  err                   = priv_read_sector(m, 0, s_scratch);
  if (err != k_ra8_ok) {
    return err;
  }
  return priv_parse_volume(m);
}

/**
 * @brief Mount a volume -- the guarded body of ::ra8_fs_mount().
 *
 * @details Allocates a mount slot, reads the boot sector, parses the
 *          BPB, and computes the geometry.
 *
 * @param[in]  backend    Block-device backend to drive.
 * @param[out] out_handle On success, opaque mount handle.
 *
 * @return Error code.
 * @retval k_ra8_ok                     Volume mounted.
 * @retval k_ra8_err_null_ptr           NULL `backend` or `out_handle`.
 * @retval k_ra8_err_invalid_arg        Backend missing required callbacks.
 * @retval k_ra8_err_no_mem             Mount table is full.
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
static ra8_err_t priv_mount_locked(const ra8_fs_backend_t* backend, ra8_fs_mount_t** out_handle)
{
  if (backend == nullptr || out_handle == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (backend->read_block == nullptr || backend->write_block == nullptr ||
      backend->get_capacity == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  ra8_fs_mount_t* m = priv_alloc_mount_slot();
  if (m == nullptr) {
    return k_ra8_err_no_mem;
  }
  m->backend            = *backend;
  m->partition_base_lba = 0U;
  ra8_err_t err         = priv_read_boot_sector(m);
  if (err != k_ra8_ok) {
    return err;
  }
  /* exFAT geometry is parsed directly in priv_exfat_parse; the FAT-BPB
   * geometry computation applies only to FAT12/16/32. */
  if (m->type != k_ra8_fs_type_exfat) {
    err = priv_compute_geometry(m);
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
static ra8_err_t priv_unmount_locked(ra8_fs_mount_t* handle)
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
  const ra8_err_t err = priv_format_locked(backend, opts);
  priv_lock_release();
  return err;
}

RA8_OWNS_RESOURCE("ra8_fs_lock")
ra8_err_t ra8_fs_mount(const ra8_fs_backend_t* backend, ra8_fs_mount_t** out_handle)
{
  priv_lock_acquire();
  const ra8_err_t err = priv_mount_locked(backend, out_handle);
  priv_lock_release();
  return err;
}

RA8_OWNS_RESOURCE("ra8_fs_lock")
ra8_err_t ra8_fs_unmount(ra8_fs_mount_t* handle)
{
  priv_lock_acquire();
  const ra8_err_t err = priv_unmount_locked(handle);
  priv_lock_release();
  return err;
}
