/**
 * @file ra8_fs_fat_gpt.c
 * @brief GPT partition-table parsing for the `ra8_fs` mount path.
 *
 * @details
 * The GUID Partition Table half of volume location (UEFI spec 2.10 ch 5),
 * split out of `ra8_fs_fat_mount.c` when 64-bit LBAs (#683) pushed that file
 * past the source-size cap: header validation, the bounded entry-array walk,
 * Basic-Data preference, and the two locate entry points the mount path calls
 * -- auto-select (::priv_gpt_locate_volume) and by explicit index
 * (::priv_gpt_locate_partition). Entry-array and entry LBAs are read as the
 * full 64-bit fields they are, so partitions past 2 TiB select and mount.
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

/** @brief GPT header signature ("EFI PART", UEFI spec 2.10 table 5.5). */
static const uint8_t s_gpt_signature[k_gpt_sig_len] = {
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
static const uint8_t s_gpt_guid_basic_data[k_gpt_guid_len] = {
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
 * allocated) and its first LBA is non-zero. The LBA is the entry's full
 * 64-bit field: partitions past the 32-bit-LBA reach (2 TiB at 512-byte
 * sectors) are addressable now that the backend interface is 64-bit (#683).
 *
 * @param[in] entry One 128-byte partition entry.
 * @return The entry's first LBA, or 0 when the entry is unusable.
 * @retval 0 Unused slot or zero first-LBA.
 * @pre @p entry is non-NULL and holds ::k_gpt_entry_bytes bytes.
 * @pre @p entry came from the GPT partition entry array.
 * @post No state modified.
 * @post @p entry is unmodified.
 * @note Pure function.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint64_t internal_gpt_entry_first_lba(const uint8_t* entry)
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
  return priv_rd64(&entry[k_gpt_entry_off_first_lba]);
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
static uint32_t internal_gpt_entry_is_basic_data(const uint8_t* entry)
{
  for (uint32_t i = 0U; i < (uint32_t)k_gpt_guid_len; i++) {
    if (entry[i] != s_gpt_guid_basic_data[i]) {
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
static void internal_gpt_note_entry(const uint8_t* entry, uint64_t* basic_lba, uint64_t* any_lba)
{
  const uint64_t first = internal_gpt_entry_first_lba(entry);
  if (first == 0U) {
    return;
  }
  if (*any_lba == 0U) {
    *any_lba = first;
  }
  if (*basic_lba == 0U) {
    if (internal_gpt_entry_is_basic_data(entry) != 0U) {
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
 * @post ::priv_scratch holds the last entry sector read.
 * @note Not thread-safe -- uses module-level scratch.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_gpt_scan_entries(ra8_fs_mount_t* m, uint64_t entry_lba, uint32_t count, uint64_t* out_base)
{
  const uint32_t eps       = priv_bps(m) / (uint32_t)k_gpt_entry_bytes;
  uint64_t       basic_lba = 0U;
  uint64_t       any_lba   = 0U;
  for (uint32_t i = 0U; i < count; i++) {
    const uint32_t sector = i / eps;
    const uint32_t offset = (i % eps) * (uint32_t)k_gpt_entry_bytes;
    if (offset == 0U) {
      const ra8_err_t err = priv_read_sector(m, entry_lba + sector, priv_scratch);
      if (err != k_ra8_ok) {
        return err;
      }
    }
    internal_gpt_note_entry(&priv_scratch[offset], &basic_lba, &any_lba);
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
 * @brief Read and validate the GPT header, returning the entry-array geometry.
 *
 * @details Reads the "EFI PART" header at LBA 1, checks the signature, rejects
 * an entry-array LBA above 32 bits or a non-standard entry size, and clamps the
 * entry count to the bounded scan cap. Shared by the auto locator
 * (::priv_gpt_locate_volume) and the indexed one (::priv_gpt_locate_partition)
 * so the two agree byte-for-byte on what a usable GPT is.
 *
 * @param[in,out] m             Mount whose backend supplies the sectors.
 * @param[out]    out_entry_lba First LBA of the partition entry array.
 * @param[out]    out_count     Entry count, clamped to ::k_gpt_entry_scan_max.
 * @return Error code.
 * @retval k_ra8_ok                    Geometry read and validated.
 * @retval k_ra8_err_validation_failed No "EFI PART" header, or entry_lba is 0.
 * @retval k_ra8_err_not_supported     Non-standard entry size or 64-bit array LBA.
 * @retval k_ra8_err_*                 Backend read failure.
 * @pre ``m->partition_base_lba`` is still 0 (reads are absolute).
 * @pre @p out_entry_lba and @p out_count are non-NULL.
 * @post On k_ra8_ok both outputs are set; ::priv_scratch holds the GPT header.
 * @post On failure the outputs are unchanged.
 * @note Not thread-safe -- uses module-level scratch.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_gpt_read_geom(ra8_fs_mount_t* m, uint64_t* out_entry_lba, uint32_t* out_count)
{
  const ra8_err_t err = priv_read_sector(m, (uint64_t)k_gpt_header_lba, priv_scratch);
  if (err != k_ra8_ok) {
    return err;
  }
  for (uint32_t i = 0U; i < (uint32_t)k_gpt_sig_len; i++) {
    if (priv_scratch[i] != s_gpt_signature[i]) {
      return k_ra8_err_validation_failed;
    }
  }
  const uint64_t entry_lba  = priv_rd64(&priv_scratch[k_gpt_off_entry_lba]);
  const uint32_t entry_size = priv_rd32(&priv_scratch[k_gpt_off_entry_size]);
  uint32_t       count      = priv_rd32(&priv_scratch[k_gpt_off_entry_count]);
  if (entry_lba == 0U) {
    return k_ra8_err_validation_failed;
  }
  if (entry_size != (uint32_t)k_gpt_entry_bytes) {
    return k_ra8_err_not_supported;
  }
  if (count > (uint32_t)k_gpt_entry_scan_max) {
    count = (uint32_t)k_gpt_entry_scan_max;
  }
  *out_entry_lba = entry_lba;
  *out_count     = count;
  return k_ra8_ok;
}

/* `priv_gpt_locate_volume()`: see header for the documented contract. */
ra8_err_t priv_gpt_locate_volume(ra8_fs_mount_t* m, uint64_t* out_base)
{
  uint64_t        entry_lba = 0U;
  uint32_t        count     = 0U;
  const ra8_err_t err       = internal_gpt_read_geom(m, &entry_lba, &count);
  if (err != k_ra8_ok) {
    return err;
  }
  return internal_gpt_scan_entries(m, entry_lba, count, out_base);
}

/**
 * @brief Extract the first LBA of one explicitly selected GPT entry.
 *
 * @details The indexed counterpart of ::priv_gpt_entry_first_lba: rather than
 * skipping an unusable entry it reports why. An all-zero type GUID is an empty
 * slot; a zero first LBA is a malformed allocated entry. The full 64-bit LBA
 * is honoured -- entries past 2 TiB select cleanly now that the backend
 * addresses 64-bit LBAs (#683).
 *
 * @param[in]  entry    One 128-byte partition entry.
 * @param[out] out_base Receives the entry's first LBA on success.
 * @return Error code.
 * @retval k_ra8_ok                    @p out_base holds the entry's first LBA.
 * @retval k_ra8_err_not_found         The entry is an empty (zero-GUID) slot.
 * @retval k_ra8_err_validation_failed Allocated entry with a zero first LBA.
 * @pre @p entry is non-NULL and holds ::k_gpt_entry_bytes bytes.
 * @pre @p out_base is non-NULL.
 * @post No state modified.
 * @post @p entry is unmodified.
 * @note Pure function.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_gpt_entry_select(const uint8_t* entry, uint64_t* out_base)
{
  uint32_t nonzero = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_gpt_guid_len; i++) {
    if (entry[i] != 0U) {
      nonzero = 1U;
    }
  }
  if (nonzero == 0U) {
    return k_ra8_err_not_found;
  }
  const uint64_t base = priv_rd64(&entry[k_gpt_entry_off_first_lba]);
  if (base == 0U) {
    return k_ra8_err_validation_failed;
  }
  *out_base = base;
  return k_ra8_ok;
}

/* `priv_gpt_locate_partition()`: see header for the documented contract. */
ra8_err_t priv_gpt_locate_partition(ra8_fs_mount_t* m, uint8_t index, uint64_t* out_base)
{
  uint64_t        entry_lba = 0U;
  uint32_t        count     = 0U;
  const ra8_err_t err       = internal_gpt_read_geom(m, &entry_lba, &count);
  if (err != k_ra8_ok) {
    return err;
  }
  if ((uint32_t)index >= count) {
    return k_ra8_err_out_of_range;
  }
  const uint32_t  eps    = priv_bps(m) / (uint32_t)k_gpt_entry_bytes;
  const uint32_t  sector = (uint32_t)index / eps;
  const uint32_t  offset = ((uint32_t)index % eps) * (uint32_t)k_gpt_entry_bytes;
  const ra8_err_t rerr   = priv_read_sector(m, entry_lba + sector, priv_scratch);
  if (rerr != k_ra8_ok) {
    return rerr;
  }
  return internal_gpt_entry_select(&priv_scratch[offset], out_base);
}
