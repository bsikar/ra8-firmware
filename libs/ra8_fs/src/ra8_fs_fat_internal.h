/**
 * @file ra8_fs_fat_internal.h
 * @brief Cross-TU shared declarations for the FAT/exFAT `ra8_fs` adapter.
 * @ingroup grp_storage
 *
 * @details
 * The FAT/exFAT adapter is split across several translation units so that
 * each stays under the source-size cap. This module-private header is a thin
 * umbrella that aggregates the adapter's shared declarations -- the
 * on-disk-layout enums, the directory/format/exFAT cursor typedefs, the one
 * shared scratch-sector buffer, and the prototypes of every helper that is
 * called across those translation units -- so that every `ra8_fs_fat*.c` file
 * includes a single name. The declarations themselves live in the sub-headers
 * pulled in below:
 *
 * - `ra8_fs_fat_types_internal.h`    -- on-disk-layout enums, cross-TU typedefs,
 *                             and the shared `s_scratch` extern.
 * - `ra8_fs_fat_protos_a_internal.h` -- cross-TU helper prototypes, part A of 2.
 * - `ra8_fs_fat_protos_b_internal.h` -- cross-TU helper prototypes, part B of 2.
 * - `ra8_fs_fat_time_internal.h`     -- timestamp field offsets, the FAT/exFAT
 *                             date-time packing, and the entry stampers.
 * - `ra8_fs_fat_alloc_internal.h`    -- the per-mount allocator state: next-free
 *                             hint, free-cluster count, FAT sector cache, and
 *                             the FAT32 FSInfo seed/writeback.
 * - `ra8_fs_fat_lfn_write_internal.h` -- the VFAT long-name WRITE seam: name
 *                             classification, alias generation, and the
 *                             reserve / commit / erase directory verbs.
 * - `ra8_fs_fat_exfat_stream_internal.h` -- the exFAT streaming-write mechanism:
 *                             entry-set coordinates, the allocation-bitmap
 *                             primitives, and the grow/flush engine.
 *
 * The last four are split by THEME rather than alphabetically, because each
 * owns a self-contained mechanism with its own on-disk vocabulary. Folding
 * them into the alphabetical `protos_a` / `protos_b` pair would scatter one
 * mechanism's constants across two files and push all of them past the
 * 1000-line source cap.
 *
 * This header is included by every `ra8_fs_fat*.c` file and by nothing outside
 * this module.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include "ra8_fs_fat_alloc_internal.h"
#include "ra8_fs_fat_exfat_stream_internal.h"
#include "ra8_fs_fat_lfn_write_internal.h"
#include "ra8_fs_fat_protos_a_internal.h"
#include "ra8_fs_fat_protos_b_internal.h"
#include "ra8_fs_fat_time_internal.h"
#include "ra8_fs_fat_types_internal.h"
