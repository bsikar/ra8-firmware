/**
 * @file ra8_fs_fat_gpt_internal.h
 * @brief Cross-TU prototypes for the GPT partition locators.
 * @ingroup grp_storage
 *
 * @details
 * The two entry points `ra8_fs_fat_gpt.c` offers the mount path: locate the
 * first mountable partition on a GPT disk, and locate one by entry index.
 * Split out as a themed sub-header (like the exFAT stream and directory
 * seams) rather than grown onto the alphabetical prototype headers, which sit
 * against the source-size cap.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include "ra8_attributes.h"
#include "ra8_fs_fat_types_internal.h"

/**
 * @brief Locate the first mountable partition on a GPT disk (auto-select).
 *
 * @details Reads the header geometry via ::priv_gpt_read_geom, then scans the
 * entries via ::priv_gpt_scan_entries (Microsoft Basic Data preferred, else the
 * first allocated entry of any type).
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
RA8_PRIV
ra8_err_t priv_gpt_locate_volume(ra8_fs_mount_t* m, uint64_t* out_base);

/**
 * @brief Locate a GPT partition chosen by entry-array index.
 *
 * @details Reads the header geometry via ::priv_gpt_read_geom, bounds-checks
 * @p index against the (clamped) entry count, reads the sector holding that
 * entry, and validates it via ::priv_gpt_entry_select.
 *
 * @param[in,out] m        Mount whose backend supplies the sectors.
 * @param[in]     index    Zero-based GPT entry-array index.
 * @param[out]    out_base Receives the selected partition's first LBA.
 * @return Error code.
 * @retval k_ra8_ok                    @p out_base holds the volume base.
 * @retval k_ra8_err_out_of_range      @p index is past the entry count.
 * @retval k_ra8_err_not_found         The selected entry is empty.
 * @retval k_ra8_err_not_supported     Entry-array geometry, or an entry LBA,
 *                                     this backend cannot address.
 * @retval k_ra8_err_validation_failed No header, or a malformed entry.
 * @retval k_ra8_err_*                 Backend read failure.
 * @pre ``m->partition_base_lba`` is still 0 (reads are absolute).
 * @pre @p out_base is non-NULL.
 * @post On k_ra8_ok @p out_base holds a non-zero LBA.
 * @post ::s_scratch is overwritten (callers must re-read their sector).
 * @note Not thread-safe -- uses module-level scratch.
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_gpt_locate_partition(ra8_fs_mount_t* m, uint8_t index, uint64_t* out_base);
