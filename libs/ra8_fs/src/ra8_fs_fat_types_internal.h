/**
 * @file ra8_fs_fat_types_internal.h
 * @brief Cross-TU on-disk-layout enums and typedefs for the FAT/exFAT adapter.
 * @ingroup grp_storage
 *
 * @details
 * The TYPES half of the FAT/exFAT adapter's module-private declarations. It
 * carries the on-disk-layout enums (BPB / directory / MBR / GPT / exFAT /
 * formatter), the directory/format/exFAT cursor typedefs, and the one shared
 * scratch-sector buffer. It is pulled in by the `ra8_fs_fat_internal.h` umbrella
 * (and therefore by every `ra8_fs_fat*.c` file) and by the prototype sub-headers,
 * whose helper declarations reference these types.
 *
 * This header aggregates the module's on-disk-layout enums shared across the
 * FAT translation units.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_fs.h"

/* ===========================================================================
 * On-disk layout enums (BPB / directory / MBR / GPT / exFAT / formatter),
 * shared by every FAT/exFAT translation unit.
 * ===========================================================================
 */

/* =============================================================================
 * BPB field offsets -- MS FAT spec sec 3.1, table "BPB and BS Fields".
 * Every magic number in the on-disk layout is named here; fat dispatcher
 * code reads via these offsets only.
 * =============================================================================
 */

/**
 * @enum ra8_fs_bpb_off_t
 * @brief Byte offsets inside a 512-byte BPB / boot sector.
 */
typedef enum : uint16_t {
  k_bpb_off_bytes_per_sec = 11,  /**< MS FAT spec sec 3.1 "BPB_BytsPerSec". */
  k_bpb_off_sec_per_clus  = 13,  /**< MS FAT spec sec 3.1 "BPB_SecPerClus". */
  k_bpb_off_rsvd_sec_cnt  = 14,  /**< MS FAT spec sec 3.1 "BPB_RsvdSecCnt". */
  k_bpb_off_num_fats      = 16,  /**< MS FAT spec sec 3.1 "BPB_NumFATs".    */
  k_bpb_off_root_ent_cnt  = 17,  /**< MS FAT spec sec 3.1 "BPB_RootEntCnt". */
  k_bpb_off_tot_sec_16    = 19,  /**< MS FAT spec sec 3.1 "BPB_TotSec16".   */
  k_bpb_off_fat_sz_16     = 22,  /**< MS FAT spec sec 3.1 "BPB_FATSz16".    */
  k_bpb_off_tot_sec_32    = 32,  /**< MS FAT spec sec 3.1 "BPB_TotSec32".   */
  k_bpb_off_fat_sz_32     = 36,  /**< MS FAT spec sec 3.5 "BPB_FATSz32".    */
  k_bpb_off_root_clus     = 44,  /**< MS FAT spec sec 3.5 "BPB_RootClus".   */
  k_bpb_off_signature_lo  = 510, /**< Boot sig byte 1 (0x55).               */
  k_bpb_off_signature_hi  = 511, /**< Boot sig byte 2 (0xAA).               */
} ra8_fs_bpb_off_t;

/**
 * @enum ra8_fs_dir_off_t
 * @brief Byte offsets inside a 32-byte FAT directory entry. MS FAT spec sec 6.
 */
typedef enum : uint8_t {
  k_dir_off_name        = 0,  /**< MS FAT spec sec 6 "DIR_Name" (11 bytes). */
  k_dir_off_attr        = 11, /**< MS FAT spec sec 6 "DIR_Attr".            */
  k_dir_off_ntres       = 12, /**< MS FAT spec sec 6 "DIR_NTRes".           */
  k_dir_off_fst_clus_hi = 20, /**< MS FAT spec sec 6 "DIR_FstClusHI".       */
  k_dir_off_fst_clus_lo = 26, /**< MS FAT spec sec 6 "DIR_FstClusLO".       */
  k_dir_off_file_size   = 28, /**< MS FAT spec sec 6 "DIR_FileSize".        */
  k_dir_name_field_len  = 11, /**< 8 + 3 raw chars (no dot).                */
} ra8_fs_dir_off_t;

/**
 * @enum ra8_fs_ntres_t
 * @brief The two case flags Windows keeps in `DIR_NTRes` (byte 12).
 *
 * @details A name that differs from its 8.3 form only by being lower case
 *          needs no long-name chain at all: the entry stores the upper-case
 *          form and sets one or both of these bits, and every reader that
 *          knows them renders `data.log` rather than `DATA.LOG`. The bits are
 *          not in the Microsoft FAT specification's field table -- it reserves
 *          the byte -- but they are what every Windows, Linux and macOS driver
 *          writes and reads, so they are the interoperable encoding.
 *
 * @invariant The two bits are independent; either, both, or neither may be set.
 * @see priv_name_classify()
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ntres_base_lower = 0x08U, /**< Render DIR_Name[0..7] lower case.  */
  k_ntres_ext_lower  = 0x10U, /**< Render DIR_Name[8..10] lower case. */
} ra8_fs_ntres_t;

/**
 * @enum ra8_fs_dir_marker_t
 * @brief Special markers in DIR_Name[0]. MS FAT spec sec 6.
 */
typedef enum : uint8_t {
  k_dir_marker_free_perm = 0x00, /**< End-of-directory.          */
  k_dir_marker_free_used = 0xE5, /**< Slot was used, deleted.    */
  k_dir_marker_kanji_e5  = 0x05, /**< 0xE5 in raw name, escaped. */
  k_dir_marker_dot       = 0x2E, /**< '.' -- a "." / ".." entry. */
} ra8_fs_dir_marker_t;

/**
 * @enum ra8_fs_sig_t
 * @brief Required boot-sector signature bytes (0x55 0xAA at off 510..511).
 */
typedef enum : uint8_t {
  k_bpb_sig_lo = 0x55, /**< Bpb sig lo. */
  k_bpb_sig_hi = 0xAA, /**< Bpb sig hi. */
} ra8_fs_sig_t;

/**
 * @enum ra8_fs_mbr_off_t
 * @brief Byte offsets inside an MBR partition table (when LBA 0 is not a BPB).
 *
 * @details A standard SD card is MBR-partitioned: LBA 0 holds a partition
 * table (four 16-byte entries starting at 0x1BE) and the FAT boot sector
 * lives at the partition's first LBA. Only partition 0 is consulted.
 */
typedef enum : uint16_t {
  k_mbr_off_part0_type = 0x1C2U, /**< Partition 0 type byte (0 = unused).  */
  k_mbr_off_part0_lba  = 0x1C6U, /**< Partition 0 first-LBA (4 bytes, LE). */
} ra8_fs_mbr_off_t;

/**
 * @enum ra8_fs_gpt_t
 * @brief GPT on-disk constants (UEFI spec 2.10 ch 5 "GUID Partition Table").
 *
 * @details A GPT disk carries a protective MBR (partition 0 type 0xEE), the
 * GPT header at LBA 1 ("EFI PART"), and a partition entry array (128-byte
 * entries). FAT/exFAT data volumes live in entries whose type GUID is
 * Microsoft Basic Data; macOS additionally creates an EFI System Partition
 * first, which must be skipped to reach the user volume.
 */
typedef enum : uint16_t {
  k_gpt_part_type_protective = 0xEEU, /**< MBR type byte for a GPT disk.       */
  k_gpt_header_lba           = 1U,    /**< GPT header location.                */
  k_gpt_sig_len              = 8U,    /**< "EFI PART" signature length.        */
  k_gpt_off_entry_lba        = 0x48U, /**< Entry-array first LBA (8 bytes LE). */
  k_gpt_off_entry_count      = 0x50U, /**< Number of partition entries.        */
  k_gpt_off_entry_size       = 0x54U, /**< Bytes per partition entry.          */
  k_gpt_entry_bytes          = 128U,  /**< Standard entry size supported.      */
  k_gpt_entries_per_sector   = 4U,    /**< 512 / 128 entries per sector.       */
  k_gpt_entry_scan_max       = 128U,  /**< Bounded entry walk (UEFI minimum).  */
  k_gpt_entry_off_first_lba  = 0x20U, /**< Entry: first LBA (8 bytes LE).      */
  k_gpt_guid_len             = 16U,   /**< Type-GUID length in an entry.       */
  k_gpt_u64_hi_off           = 4U,    /**< High-word offset inside a u64.      */
} ra8_fs_gpt_t;

/**
 * @enum ra8_fs_exfat_off_t
 * @brief Byte offsets in the exFAT boot sector (VBR) and directory entries.
 *
 * @details exFAT (Microsoft exFAT spec) replaces the FAT BPB with a Volume
 * Boot Record carrying sector-relative region offsets and log2 geometry. The
 * directory uses 32-byte typed entry sets (File 0x85 + Stream 0xC0 + Name
 * 0xC1) with UTF-16LE names. Read-only support; fields are little-endian.
 */
typedef enum : uint16_t {
  k_exfat_off_fsname       = 3,    /**< "EXFAT   " filesystem-name field (8 chars). */
  k_exfat_off_fat_lba      = 0x50, /**< FatOffset (sectors from VBR).               */
  k_exfat_off_fat_len      = 0x54, /**< FatLength (sectors).                        */
  k_exfat_off_heap_lba     = 0x58, /**< ClusterHeapOffset (sectors from VBR).       */
  k_exfat_off_clus_count   = 0x5C, /**< ClusterCount.                               */
  k_exfat_off_root_clus    = 0x60, /**< FirstClusterOfRootDirectory.                */
  k_exfat_off_bps_shift    = 0x6C, /**< BytesPerSectorShift (log2).                 */
  k_exfat_off_spc_shift    = 0x6D, /**< SectorsPerClusterShift (log2).              */
  k_exfat_off_num_fats     = 0x6E, /**< NumberOfFats.                               */
  k_exfat_strm_off_flags   = 1,    /**< Stream-ext GeneralSecondaryFlags.           */
  k_exfat_strm_off_nlen    = 3,    /**< Stream-ext NameLength (UTF-16 units).       */
  k_exfat_strm_off_vlen_hi = 0x0C, /**< Stream-ext ValidDataLength high word.       */
  k_exfat_strm_off_clus    = 0x14, /**< Stream-ext FirstCluster.                    */
  k_exfat_strm_off_dlen    = 0x18, /**< Stream-ext DataLength (low 32 used).        */
  k_exfat_strm_off_dlen_hi = 0x1C, /**< Stream-ext DataLength high word.            */
  k_exfat_name_off         = 2,    /**< File-name entry character offset.           */
  k_exfat_upc_off_csum     = 4,    /**< Up-case-table entry TableChecksum (32-bit). */
} ra8_fs_exfat_off_t;

/**
 * @enum ra8_fs_exfat_val_t
 * @brief exFAT entry-type tags + small magic values used by the reader.
 */
typedef enum : uint32_t {
  k_exfat_entry_eod      = 0x00U,   /**< End-of-directory marker.                   */
  k_exfat_entry_bitmap   = 0x81U,   /**< Allocation-bitmap directory entry.         */
  k_exfat_entry_file     = 0x85U,   /**< File directory entry.                      */
  k_exfat_entry_stream   = 0xC0U,   /**< Stream-extension entry.                    */
  k_exfat_entry_name     = 0xC1U,   /**< File-name entry.                           */
  k_exfat_secflag_no_fat = 0x02U,   /**< GeneralSecondaryFlags: NoFatChain.         */
  k_exfat_secflag_poss   = 0x01U,   /**< GeneralSecondaryFlags: AllocationPossible. */
  k_exfat_secflag_alloc  = 0x03U,   /**< AllocationPossible | NoFatChain.           */
  k_exfat_attr_directory = 0x10U,   /**< FileAttributes: directory.                 */
  k_exfat_attr_archive   = 0x20U,   /**< FileAttributes: archive.                   */
  k_exfat_fsname_len     = 8U,      /**< "EXFAT   " field length.                   */
  k_exfat_name_per_entry = 15U,     /**< UTF-16 units per file-name entry.          */
  k_exfat_entry_bytes    = 32U,     /**< Directory entry size.                      */
  k_exfat_bps_shift_512  = 9U,      /**< log2(512) -- the only sector size we do.   */
  k_exfat_scan_limit     = 65536U,  /**< Max dir entries scanned (P10 bound).       */
  k_exfat_name_cap       = 64U,     /**< Longest name we store, in UTF-16 units.    */
  k_exfat_name_u8_cap    = 193U,    /**< That name in UTF-8: 3 * 64, plus a NUL.    */
  k_exfat_upc_words      = 2918U,   /**< k_exfat_fmt_upc_std_bytes / 2.             */
  k_exfat_upc_run_tag    = 0xFFFFU, /**< Up-case table: an identity run follows.    */
  k_exfat_csum_hi_bit    = 0x8000U, /**< Wrap bit for the rotate-add checksum.      */
  k_exfat_off_file_secnt = 1U,      /**< File entry: SecondaryCount.                */
  k_exfat_off_file_csum  = 2U,      /**< File entry: SetChecksum (2 bytes).         */
  k_exfat_off_file_attr  = 4U,      /**< File entry: FileAttributes (2 bytes).      */
  k_exfat_off_strm_hash  = 4U,      /**< Stream entry: NameHash (2 bytes).          */
  k_exfat_off_strm_valid = 8U,      /**< Stream entry: ValidDataLength (8 B).       */
  k_exfat_bit_mask       = 7U,      /**< Bit index within a bitmap byte.            */
  k_exfat_bit_shift      = 3U,      /**< log2(8): cluster index -> byte.            */
  k_exfat_inuse_bit      = 0x80U,   /**< Directory entry type bit 7 = in use.       */
  k_exfat_max_set_bytes  = 224U,    /**< (2 + ceil(64/15)) * 32 = 7 entries.        */
  k_exfat_path_depth     = 32U,     /**< Max nested components per exFAT path.      */
  k_exfat_dir_grow_max   = 2U,      /**< Max clusters one find-space call appends.  */
} ra8_fs_exfat_val_t;

/**
 * @enum ra8_fs_exfat_fmt_off_t
 * @brief VBR + directory-entry byte offsets written by the exFAT formatter.
 *
 * @details Companion to the reader's `ra8_fs_exfat_off_t`; covers the fields the
 *          reader never touches (boot signatures, serial, label entry, ...).
 *          Microsoft exFAT spec sections 3 (Main Boot Sector) and 7 (directory
 *          entries).
 */
typedef enum : uint16_t {
  k_exfat_foff_jump      = 0U,   /**< JumpBoot (3 bytes).                        */
  k_exfat_foff_part_off  = 64U,  /**< PartitionOffset (64-bit).                  */
  k_exfat_foff_vol_len   = 72U,  /**< VolumeLength (64-bit).                     */
  k_exfat_foff_serial    = 100U, /**< VolumeSerialNumber (32-bit).               */
  k_exfat_foff_fs_rev    = 104U, /**< FileSystemRevision (16-bit).               */
  k_exfat_foff_vol_flags = 106U, /**< VolumeFlags (16-bit).                      */
  k_exfat_foff_drive     = 111U, /**< DriveSelect.                               */
  k_exfat_foff_percent   = 112U, /**< PercentInUse.                              */
  k_exfat_foff_ext_sig   = 508U, /**< ExtendedBootSignature (extended sectors).  */
  k_exfat_foff_boot_sig  = 510U, /**< BootSignature 0xAA55 (16-bit).             */
  k_exfat_de_second      = 32U,  /**< Offset of the 2nd dir entry in a set.      */
  k_exfat_de_third       = 64U,  /**< Offset of the 3rd dir entry in a set.      */
  k_exfat_de_first_clus  = 20U,  /**< Bitmap/Up-case entry: FirstCluster.        */
  k_exfat_de_data_len    = 24U,  /**< Bitmap/Up-case entry: DataLength (64-bit). */
  k_exfat_de_upc_csum    = 4U,   /**< Up-case entry: TableChecksum (32-bit).     */
  k_exfat_de_lbl_cnt     = 1U,   /**< Volume-label entry: CharacterCount.        */
  k_exfat_de_lbl_name    = 2U,   /**< Volume-label entry: UTF-16 label.          */
} ra8_fs_exfat_fmt_off_t;

/**
 * @enum ra8_fs_exfat_fmt_val_t
 * @brief Magic values + geometry constants used by the exFAT formatter.
 */
typedef enum : uint32_t {
  k_exfat_entry_upcase      = 0x82U,       /**< Up-case Table directory entry.         */
  k_exfat_entry_label       = 0x83U,       /**< Volume Label directory entry.          */
  k_exfat_fmt_jump0         = 0xEBU,       /**< JumpBoot byte 0.                       */
  k_exfat_fmt_jump1         = 0x76U,       /**< JumpBoot byte 1.                       */
  k_exfat_fmt_jump2         = 0x90U,       /**< JumpBoot byte 2.                       */
  k_exfat_fmt_fs_rev        = 0x0100U,     /**< FileSystemRevision = 1.00.             */
  k_exfat_fmt_drive         = 0x80U,       /**< DriveSelect (first fixed disk).        */
  k_exfat_fmt_percent       = 0U,          /**< PercentInUse = 0% (fresh volume).      */
  k_exfat_fmt_boot_sig      = 0xAA55U,     /**< BootSignature (sector 0).              */
  k_exfat_fmt_ext_sig       = 0xAA550000U, /**< ExtendedBootSignature (sectors 1-8).   */
  k_exfat_fmt_fat_media     = 0xFFFFFFF8U, /**< FatEntry[0] media descriptor.          */
  k_exfat_fmt_fat_eoc       = 0xFFFFFFFFU, /**< FatEntry end-of-chain.                 */
  k_exfat_fmt_first_clus    = 2U,          /**< First cluster of the heap.             */
  k_exfat_fmt_num_fats      = 1U,          /**< exFAT uses a single FAT.               */
  k_exfat_fmt_boot_secs     = 24U,         /**< 12 main + 12 backup boot sectors.      */
  k_exfat_fmt_backup_lba    = 12U,         /**< Backup boot region start.              */
  k_exfat_fmt_ext_first     = 1U,          /**< First extended boot sector.            */
  k_exfat_fmt_ext_count     = 8U,          /**< Extended boot sector count.            */
  k_exfat_fmt_oem_lba       = 9U,          /**< OEM Parameters sector.                 */
  k_exfat_fmt_resv_lba      = 10U,         /**< Reserved sector.                       */
  k_exfat_fmt_csum_lba      = 11U,         /**< Boot Checksum sector.                  */
  k_exfat_fmt_csum_skip0    = 106U,        /**< Checksum-excluded byte: VolumeFlags.   */
  k_exfat_fmt_csum_skip1    = 107U,        /**< Checksum-excluded byte: VolumeFlags.   */
  k_exfat_fmt_csum_skip2    = 112U,        /**< Checksum-excluded byte: PercentInUse.  */
  k_exfat_fmt_csum_hibit    = 0x80000000U, /**< Rotate-right wrap bit (32-bit).        */
  k_exfat_fmt_csum_copies   = 128U,        /**< 512 / 4 checksum words per sector.     */
  k_exfat_fmt_min_sectors   = 65536U,      /**< Smallest exFAT volume: 32 MiB.         */
  k_exfat_fmt_thr_256m      = 524288U,     /**< <= 256 MB -> 4 KB clusters.            */
  k_exfat_fmt_thr_32g       = 67108864U,   /**< <= 32 GB -> 32 KB clusters.            */
  k_exfat_fmt_thr_256g      = 536870912U,  /**< <= 256 GB -> 128 KB clusters.          */
  k_exfat_fmt_spc_4k        = 3U,          /**< SectorsPerClusterShift for 4 KB.       */
  k_exfat_fmt_spc_32k       = 6U,          /**< ... 32 KB.                             */
  k_exfat_fmt_spc_128k      = 8U,          /**< ... 128 KB.                            */
  k_exfat_fmt_spc_256k      = 9U,          /**< ... 256 KB (> 256 GB cards).           */
  k_exfat_fmt_geom_iters    = 4U,          /**< Fixed-point geometry passes.           */
  k_exfat_fmt_upc_std_bytes = 5836U,       /**< Canonical Microsoft up-case table len. */
  k_exfat_fmt_upc_std_secs  = 12U,         /**< ceil(5836 / 512): table sector span.   */
  k_exfat_fmt_part_lba      = 2048U,       /**< exFAT partition start (1 MiB aligned). */
  k_exfat_fmt_serial        = 0x52A8E47AU, /**< Arbitrary volume-serial base.          */
  k_exfat_fmt_label_max     = 11U,         /**< Volume-label cap (UTF-16 units).       */
  k_exfat_fmt_byte_bits     = 8U,          /**< Bits per bitmap byte.                  */
  k_exfat_fmt_byte_full     = 0xFFU,       /**< A fully-allocated bitmap byte.         */
} ra8_fs_exfat_fmt_val_t;

/**
 * @enum ra8_fs_mbr_fmt_t
 * @brief Byte offsets, field strides, and CHS constants for the MBR the exFAT
 *        formatter writes at LBA 0.
 *
 * @details A PC expects removable media to carry a partition table, not a
 *          "superfloppy" volume at sector 0. The exFAT formatter therefore lays
 *          a classic DOS/MBR partition table (disk signature at 440, one
 *          primary partition entry at 446, the 0x55AA boot signature) with a
 *          single type-0x07 (exFAT/NTFS) partition aligned at
 *          ::k_exfat_fmt_part_lba. The legacy CHS fields are filled from the
 *          conventional 255-head / 63-sector geometry (or the 0xFE/0xFF/0xFF
 *          "beyond CHS, use LBA" sentinel above ::k_mbr_fmt_chs_max) so
 *          `fdisk`/`sfdisk` report the partition without a CHS-mismatch warning.
 */
typedef enum : uint32_t {
  k_mbr_fmt_disk_sig_off     = 440U,        /**< MBR disk signature (4 bytes LE).       */
  k_mbr_fmt_part0_off        = 446U,        /**< First partition-table entry.           */
  k_mbr_fmt_pe_boot          = 0U,          /**< Entry: boot flag (relative).           */
  k_mbr_fmt_pe_chs_start     = 1U,          /**< Entry: start CHS (3 bytes).            */
  k_mbr_fmt_pe_type          = 4U,          /**< Entry: partition type byte.            */
  k_mbr_fmt_pe_chs_end       = 5U,          /**< Entry: end CHS (3 bytes).              */
  k_mbr_fmt_pe_lba           = 8U,          /**< Entry: first LBA (4 bytes LE).         */
  k_mbr_fmt_pe_nsect         = 12U,         /**< Entry: sector count (4 bytes LE).      */
  k_mbr_fmt_boot_none        = 0x00U,       /**< Non-bootable partition.                */
  k_mbr_fmt_type_exfat       = 0x07U,       /**< Partition type: exFAT/NTFS/HPFS.       */
  k_mbr_fmt_chs_heads        = 255U,        /**< Conventional heads-per-cylinder.       */
  k_mbr_fmt_chs_spt          = 63U,         /**< Conventional sectors-per-track.        */
  k_mbr_fmt_chs_max          = 16450560U,   /**< 1024*255*63: first LBA past CHS.       */
  k_mbr_fmt_chs_ovf_h        = 0xFEU,       /**< Overflow CHS head byte.                */
  k_mbr_fmt_chs_ovf_m        = 0xFFU,       /**< Overflow CHS sector/cyl-hi byte.       */
  k_mbr_fmt_chs_ovf_l        = 0xFFU,       /**< Overflow CHS cylinder-low byte.        */
  k_mbr_fmt_chs_sec_mask     = 0x3FU,       /**< Low 6 bits of the CHS sector byte.     */
  k_mbr_fmt_chs_cyl_hi_mask  = 0x300U,      /**< Cylinder bits [9:8] before packing.    */
  k_mbr_fmt_chs_cyl_hi_shift = 2U,          /**< Shift packing cyl[9:8] into bits[7:6]. */
  k_mbr_fmt_disk_sig_base    = 0x1A2B3C4DU, /**< Arbitrary disk-signature base.         */
} ra8_fs_mbr_fmt_t;

/**
 * @enum ra8_fs_cluster_t
 * @brief Reserved cluster numbers used by the FAT itself.
 */
typedef enum : uint32_t {
  k_cluster_first_data      = 2,           /**< Cluster numbers start at 2.        */
  k_cluster_eoc_min_fat12   = 0x0FF8,      /**< MS FAT spec sec 4.1 EOC threshold. */
  k_cluster_eoc_min_fat16   = 0xFFF8,      /**< MS FAT spec sec 4.1 EOC threshold. */
  k_cluster_eoc_min_fat32   = 0x0FFFFFF8,  /**< MS FAT spec sec 4.2 EOC threshold. */
  k_cluster_mask_fat32      = 0x0FFFFFFFU, /**< Top 4 bits reserved on FAT32.      */
  k_cluster_eoc_min_exfat   = 0xFFFFFFF8U, /**< exFAT spec sec 4.1 EOC threshold.  */
  k_cluster_eoc_write_fat12 = 0x0FFF,      /**< Value we write to terminate.       */
  k_cluster_eoc_write_fat16 = 0xFFFF,      /**< Cluster eoc write fat16.           */
  k_cluster_eoc_write_fat32 = 0x0FFFFFFFU, /**< Cluster eoc write fat32.           */
  k_cluster_eoc_write_exfat = 0xFFFFFFFFU, /**< exFAT spec sec 4.1 EOC marker.     */
  k_cluster_free            = 0,           /**< Cluster free.                      */
  k_cluster_count_fat12_max = 4085,        /**< MS FAT spec sec 3.5 boundary.      */
  k_cluster_count_fat16_max = 65525,       /**< MS FAT spec sec 3.5 boundary.      */
  k_fat12_value_mask        = 0x0FFFU,     /**< 12-bit FAT12 entry mask.           */
  k_fat12_low_nibble_mask   = 0x000FU,     /**< Untouched nibble (even cluster).   */
  k_fat12_high_nibble_mask  = 0xF000U,     /**< Untouched nibble (odd cluster).    */
} ra8_fs_cluster_t;

/**
 * @enum ra8_fs_fmt_off_t
 * @brief Extra BPB byte offsets written only by the formatter (`ra8_fs_format`).
 *
 * @details Complements `ra8_fs_bpb_off_t` (the read path) with the boot-prologue,
 *          media, extended-signature, label, and filesystem-type fields that the
 *          mount path never reads but a valid on-disk volume must carry. Offsets
 *          follow Microsoft "FAT: General Overview of On-Disk Format" sec 3.
 */
typedef enum : uint16_t {
  k_fmt_off_jmp0        = 0,   /**< Jump-boot byte 0 (0xEB).           */
  k_fmt_off_jmp1        = 1,   /**< Jump-boot byte 1 (offset).         */
  k_fmt_off_jmp2        = 2,   /**< Jump-boot byte 2 (0x90 NOP).       */
  k_fmt_off_oem         = 3,   /**< OEM name field (8 bytes).          */
  k_fmt_off_media       = 21,  /**< BPB_Media descriptor.              */
  k_fmt_off_sec_per_trk = 24,  /**< BPB_SecPerTrk.                     */
  k_fmt_off_num_heads   = 26,  /**< BPB_NumHeads.                      */
  k_fmt_off_f16_drvnum  = 36,  /**< FAT12/16 BS_DrvNum.                */
  k_fmt_off_f16_bootsig = 38,  /**< FAT12/16 BS_BootSig (0x29).        */
  k_fmt_off_f16_volid   = 39,  /**< FAT12/16 BS_VolID (4 bytes).       */
  k_fmt_off_f16_label   = 43,  /**< FAT12/16 BS_VolLab (11 bytes).     */
  k_fmt_off_f16_fstype  = 54,  /**< FAT12/16 BS_FilSysType (8 bytes).  */
  k_fmt_off_f32_fsinfo  = 48,  /**< FAT32 BPB_FSInfo sector number.    */
  k_fmt_off_f32_bkboot  = 50,  /**< FAT32 BPB_BkBootSec (backup boot). */
  k_fmt_off_f32_drvnum  = 64,  /**< FAT32 BS_DrvNum.                   */
  k_fmt_off_f32_bootsig = 66,  /**< FAT32 BS_BootSig (0x29).           */
  k_fmt_off_f32_volid   = 67,  /**< FAT32 BS_VolID (4 bytes).          */
  k_fmt_off_f32_label   = 71,  /**< FAT32 BS_VolLab (11 bytes).        */
  k_fmt_off_f32_fstype  = 82,  /**< FAT32 BS_FilSysType (8 bytes).     */
  k_fmt_fsi_off_lead    = 0,   /**< FSInfo FSI_LeadSig offset.         */
  k_fmt_fsi_off_struct  = 484, /**< FSInfo FSI_StrucSig offset.        */
  k_fmt_fsi_off_free    = 488, /**< FSInfo FSI_Free_Count offset.      */
  k_fmt_fsi_off_nxtfree = 492, /**< FSInfo FSI_Nxt_Free offset.        */
  k_fmt_fsi_off_trail   = 508, /**< FSInfo FSI_TrailSig offset.        */
} ra8_fs_fmt_off_t;

/**
 * @enum ra8_fs_fmt_val_t
 * @brief Magic values + geometry limits used by the formatter (`ra8_fs_format`).
 */
typedef enum : uint32_t {
  k_fmt_jmp_byte0       = 0xEBU,       /**< Short jump opcode.                    */
  k_fmt_jmp_byte1       = 0x58U,       /**< Jump displacement (to +0x5A).         */
  k_fmt_jmp_byte2       = 0x90U,       /**< NOP padding.                          */
  k_fmt_media_fixed     = 0xF8U,       /**< Media descriptor: non-removable.      */
  k_fmt_drvnum_hd       = 0x80U,       /**< BS_DrvNum: first fixed disk.          */
  k_fmt_ext_bootsig     = 0x29U,       /**< Extended boot signature present.      */
  k_fmt_sec_per_trk     = 63U,         /**< Conventional CHS sectors/track.       */
  k_fmt_num_heads       = 255U,        /**< Conventional CHS heads.               */
  k_fmt_num_fats        = 2U,          /**< Two FAT copies, like every mkfs.      */
  k_fmt_root_ents_f16   = 512U,        /**< FAT12/16 root-directory entries.      */
  k_fmt_resv_f16        = 1U,          /**< FAT12/16 reserved (boot) sectors.     */
  k_fmt_resv_f32        = 32U,         /**< FAT32 reserved sectors.               */
  k_fmt_root_clus_f32   = 2U,          /**< FAT32 root directory cluster.         */
  k_fmt_fsinfo_sector   = 1U,          /**< FAT32 FSInfo sector LBA.              */
  k_fmt_bkboot_sector   = 6U,          /**< FAT32 backup boot sector LBA.         */
  k_fmt_volid_base      = 0x52A8D200U, /**< Arbitrary volume-serial base.         */
  k_fmt_fsi_lead_sig    = 0x41615252U, /**< FSInfo lead signature "RRaA".         */
  k_fmt_fsi_struct_sig  = 0x61417272U, /**< FSInfo struct signature "rrAa".       */
  k_fmt_fsi_trail_sig   = 0xAA550000U, /**< FSInfo trailing signature.            */
  k_fmt_label_len       = 11U,         /**< Volume-label field width (bytes).     */
  k_fmt_spc_max         = 64U,         /**< Largest sectors-per-cluster we set.   */
  k_fmt_zero_chunk_secs = 32U,         /**< Sectors zeroed per multi-block write. */
  k_fmt_fat16_entry_cap = 256U,        /**< 512-byte sector / 2-byte FAT16 ent.   */
  k_fmt_fat32_entry_cap = 128U,        /**< 512-byte sector / 4-byte FAT32 ent.   */
  k_fmt_fat32_clus_cap  = 0x0FFFFFF0U, /**< Max FAT32 cluster count we accept.    */
  k_fmt_fsi_unknown     = 0xFFFFFFFFU, /**< FSInfo free-count "unknown".          */
  k_fmt_fat32_nxt_free  = 3U,          /**< First allocatable FAT32 cluster.      */
  /* Microsoft FAT spec "DskSzToSecPerClus" table (fatgen103 sec 3.3), in
   * 512-byte sectors. Picking the cluster size by disk size keeps the FAT
   * small: a 128 GB card at spc=1 would need a ~1 GB FAT per copy, but at
   * spc=64 (32 KB clusters) only ~15 MB. */
  k_fmt_f32_thr_260m = 532480U,   /**< <= 260 MB  -> spc 1  (512 B). */
  k_fmt_f32_thr_8g   = 16777216U, /**< <= 8 GB    -> spc 8  (4 KB).  */
  k_fmt_f32_thr_16g  = 33554432U, /**< <= 16 GB   -> spc 16 (8 KB).  */
  k_fmt_f32_thr_32g  = 67108864U, /**< <= 32 GB   -> spc 32 (16 KB). */
  k_fmt_f32_spc_512b = 1U,        /**< spc for <= 260 MB cards.      */
  k_fmt_f32_spc_4k   = 8U,        /**< spc for <= 8 GB cards.        */
  k_fmt_f32_spc_8k   = 16U,       /**< spc for <= 16 GB cards.       */
  k_fmt_f32_spc_16k  = 32U,       /**< spc for <= 32 GB cards.       */
  k_fmt_f32_spc_32k  = 64U,       /**< spc for > 32 GB cards.        */
} ra8_fs_fmt_val_t;

/**
 * @enum ra8_fs_misc_t
 * @brief Misc small constants used by parsing/formatting code.
 */
typedef enum : uint16_t {
  k_byte_mask              = 0xFFU,   /**< Byte mask.                                */
  k_shift_byte             = 8,       /**< Shift byte.                               */
  k_shift_two_bytes        = 16,      /**< Shift two bytes.                          */
  k_shift_three_bytes      = 24,      /**< Shift three bytes.                        */
  k_shift_nibble           = 4,       /**< Shift nibble.                             */
  k_nibble_mask            = 0x0F,    /**< Nibble mask.                              */
  k_byte_mask_full         = 0xFF,    /**< Byte mask full.                           */
  k_word_mask              = 0xFFFFU, /**< Word mask.                                */
  k_max_8_3_name           = 11,      /**< 8.3 packed length without dot.            */
  k_dot_pos                = 8,       /**< In an 8.3 packed name, dot would go here. */
  k_filename_base_len      = 8,       /**< Filename base length.                     */
  k_filename_ext_len       = 3,       /**< Filename ext length.                      */
  k_dir_entries_per_sector = 16,      /**< 512 / 32                                  */
  k_path_max               = 64,      /**< Path maximum.                             */
} ra8_fs_misc_t;

/* ===========================================================================
 * Cross-TU typedefs (directory walk, directory location, LFN state,
 * exFAT cursor, formatter geometry).
 * ===========================================================================
 */

/**
 * @struct dir_walk_t
 * @brief Internal cursor used by the directory iterator.
 */
typedef struct {
  uint8_t  is_root_fixed;     /**< 1 = FAT12/16 fixed root dir region.     */
  uint32_t fixed_remaining;   /**< Sectors left in fixed region.           */
  uint32_t cluster;           /**< Current cluster (FAT32 root case).      */
  uint32_t sector_in_cluster; /**< 0..SPC-1 inside cluster.                */
  uint32_t cur_lba;           /**< Currently loaded LBA.                   */
  uint32_t entry_idx;         /**< Byte offset within the loaded sector.   */
  uint32_t cluster_hops;      /**< FAT-chain follows so far (cycle guard). */
} dir_walk_t;

/**
 * @struct dir_loc_t
 * @brief Identifies the directory a lookup/scan should operate in.
 *
 * @details Either the volume root (`is_root != 0`) or a subdirectory rooted at
 *          a first `cluster`. This is what generalises the root-only directory
 *          primitives to nested paths: the root walker already handles both the
 *          FAT12/16 fixed-root region and a FAT32 root cluster chain, and a
 *          subdirectory is simply that same cluster-chain case starting at an
 *          arbitrary cluster.
 *
 * @since 0.1.0
 */
typedef struct {
  uint8_t  is_root; /**< 1 => the volume root; 0 => the subdirectory at `cluster`. */
  uint32_t cluster; /**< First cluster of the subdirectory (ignored when root).    */
} dir_loc_t;

/** @brief LFN directory-entry field offsets, sequence masks, and reassembly caps. */
typedef enum : uint32_t {
  k_lfn_off_seq        = 0U,      /**< Sequence/order byte (LDIR_Ord).             */
  k_lfn_off_type       = 12U,     /**< LDIR_Type -- zero for a name component.     */
  k_lfn_off_checksum   = 13U,     /**< Checksum of the matching 8.3 name.          */
  k_lfn_off_clus_lo    = 26U,     /**< LDIR_FstClusLO -- must be written as 0.     */
  k_lfn_seq_order_mask = 0x1FU,   /**< Order = seq & 0x1F (1..20).                 */
  k_lfn_seq_last       = 0x40U,   /**< Set on the last logical group.              */
  k_lfn_chars_per_ent  = 13U,     /**< UTF-16 chars carried per LFN entry.         */
  k_lfn_max_entries    = 19U,     /**< Cap so 19*13 = 247 chars fits the buffer.   */
  k_lfn_utf8_cap       = 742U,    /**< A 247-unit name in UTF-8: 3 * 247, + NUL.   */
  k_lfn_unicode_pad    = 0xFFFFU, /**< Slot padding past the name terminator.      */
  k_sfn_csum_high_bit  = 0x80U,   /**< Rotate-in bit when the running sum is odd.  */
  k_lfn_write_max      = 247U,    /**< Longest name we WRITE (k_lfn_max_entries).  */
  k_lfn_erase_max      = 20U,     /**< Longest chain we ERASE (spec LDIR_Ord max). */
  k_lfn_alias_tail_max = 999999U, /**< Highest `~N` probed by the alias generator. */
  k_lfn_alias_base_max = 6U,      /**< Basis chars kept ahead of a one-digit `~N`. */
} ra8_fs_lfn_t;

/**
 * @struct lfn_state_t
 * @brief In-progress reassembly of one LFN chain across the directory scan.
 *
 * @details The name accumulates as UTF-16 CODE UNITS, which is what the slots
 *          carry, and is converted to UTF-8 only where a caller is handed it.
 *          Holding it as `char` was the shape that forced the old reader to
 *          substitute `?` for every unit above 0x7F: a group writes at a fixed
 *          UNIT offset, and no byte-indexed buffer can take a variable-width
 *          encoding at a fixed index (#606).
 *
 *          A unit of zero terminates the name, exactly as the NUL did before,
 *          which is why ::priv_lfn_reset() clears the whole array: a group that
 *          never arrived leaves zeros, and the name stops there rather than
 *          running on into another chain's characters.
 *
 * @invariant `units` holds at most ::k_lfn_write_max units, the same cap the
 *            write side enforces, so a name that fits on disk fits here.
 * @see priv_lfn_add()
 * @since 0.1.0
 */
typedef struct {
  uint16_t units[k_lfn_write_max]; /**< Reassembled UTF-16LE units; 0 ends it. */
  uint8_t  checksum;               /**< 8.3 checksum the chain claims.         */
  uint8_t  have;                   /**< 1 once any group has been accumulated. */
} lfn_state_t;

/**
 * @enum ra8_fs_name_kind_t
 * @brief How a leaf component has to be stored in a FAT directory.
 *
 * @details Three answers, because there are three on-disk shapes: a name that
 *          is already an 8.3 name occupies one slot; a name that differs from
 *          one only in case occupies one slot plus a `DIR_NTRes` bit; and
 *          anything else needs a VFAT chain plus a generated `~N` alias. The
 *          distinction is made once, by ::priv_name_classify(), so that
 *          `open`, `mkdir` and `rename` cannot disagree about it.
 *
 * @invariant `k_name_kind_short` implies the packed 8.3 output is valid.
 * @see priv_name_classify()
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_name_kind_invalid = 0U, /**< Empty, over-long, or holds an illegal character. */
  k_name_kind_short   = 1U, /**< Fits 8.3; store one entry (+ NTRes case bits).   */
  k_name_kind_long    = 2U, /**< Needs a VFAT chain and a generated `~N` alias.   */
} ra8_fs_name_kind_t;

/**
 * @struct dir_pos_t
 * @brief The on-disk address of one 32-byte directory slot.
 *
 * @details A (sector, byte-offset) pair is how every directory primitive here
 *          already names a slot; giving it a type lets the deleter carry a
 *          whole LFN chain's worth of addresses without a walk per entry.
 *
 * @invariant `off` is a multiple of `k_ra8_fs_dir_entry_bytes` below one sector.
 * @see priv_dir_erase_chain()
 * @since 0.1.0
 */
typedef struct {
  uint32_t lba; /**< Sector holding the slot.                */
  uint32_t off; /**< Byte offset of the slot in that sector. */
} dir_pos_t;

/**
 * @struct dir_slot_t
 * @brief A directory cursor that addresses one entry, not just one sector.
 *
 * @details ::dir_walk_t steps a sector at a time, which is all a scan needs.
 *          Writing a run of entries needs to step an entry at a time and to
 *          cross into the next sector in the middle of the run, so the walker
 *          is paired with the index of the entry inside its current sector.
 *
 * @invariant `ent` is in `0 .. k_dir_entries_per_sector - 1`.
 * @see priv_dir_insert()
 * @since 0.1.0
 */
typedef struct {
  dir_walk_t w;   /**< Sector-level cursor for the containing directory. */
  uint32_t   ent; /**< Entry index within `w.cur_lba`.                   */
} dir_slot_t;

/**
 * @struct dir_target_t
 * @brief One resolved directory entry: where it is, and what is in it.
 *
 * @details `unlink`, `rmdir` and `rename` all have to answer the same three
 *          questions before they touch anything -- which directory holds the
 *          entry, which slot it is, and what the entry says -- and all three
 *          then need the parent again to sweep up the long-name chain. Passing
 *          them as one value stops each verb inventing its own out-parameter
 *          quartet and forgetting one of them.
 *
 * @invariant `entry` is the 32 bytes currently on disk at (`lba`, `off`).
 * @see priv_dir_erase_chain()
 * @since 0.1.0
 */
typedef struct {
  dir_loc_t parent;                          /**< Directory the entry lives in.   */
  uint32_t  lba;                             /**< Sector holding the entry.       */
  uint32_t  off;                             /**< Byte offset within that sector. */
  uint8_t   entry[k_ra8_fs_dir_entry_bytes]; /**< The entry as read from disk.    */
} dir_target_t;

/**
 * @struct dir_insert_t
 * @brief Everything decided about a new directory entry before anything is written.
 *
 * @details Adding an entry is split into a reservation and a commit so the
 *          caller can fail on a full directory BEFORE it allocates a cluster
 *          for the thing it was about to file: `mkdir` reserves, then
 *          allocates, then commits, and a directory with no room left costs no
 *          cluster. It also means the alias probing and the free-run search --
 *          the only two parts that read the directory -- happen exactly once.
 *
 *          The name travels as CODE UNITS rather than as a pointer to the
 *          caller's UTF-8. Decoding once, in ::priv_dir_reserve(), is what lets
 *          the commit fill slots without re-deriving a length -- and it removes
 *          the old rule that the caller's buffer had to outlive the commit.
 *
 * @invariant `lfn_entries` is 0 exactly when the name fits an 8.3 entry.
 * @invariant `nunits` is the unit count ::priv_name_classify() produced.
 * @see priv_dir_reserve()
 * @since 0.1.0
 */
typedef struct {
  dir_slot_t start;                  /**< First slot of the reserved run.          */
  uint16_t   units[k_lfn_write_max]; /**< The name, as UTF-16LE code units.        */
  uint32_t   nunits;                 /**< How many of `units` the name occupies.   */
  uint8_t    name83[k_max_8_3_name]; /**< Packed 8.3 name, or the generated alias. */
  uint8_t    ntres;                  /**< `DIR_NTRes` case flags for the entry.    */
  uint8_t    lfn_entries;            /**< Chain slots ahead of the 8.3 entry.      */
} dir_insert_t;

/**
 * @struct exfat_cursor_t
 * @brief Linear cursor over a directory's 32-byte entries.
 *
 * @details `contig_end` is what lets one cursor walk both shapes an exFAT
 *          directory comes in. The volume root is always a FAT chain, so it
 *          leaves the field 0 and the walk follows ::priv_fat_get. A
 *          subdirectory this driver creates is one contiguous run with
 *          NoFatChain set, and per the exFAT specification the FAT entries for
 *          such a run are explicitly invalid -- following them would read a
 *          free entry (0), decide it is not end-of-chain, and walk off into
 *          cluster 0. The run bound is the only correct successor there.
 *
 * @invariant `entry_in_cluster` is <= the entries one cluster holds.
 * @invariant `contig_end` is 0, or greater than the run's first cluster.
 * @see priv_exfat_cursor_init()
 * @since 0.1.0
 */
typedef struct {
  uint32_t cluster;          /**< Current directory cluster.                 */
  uint32_t entry_in_cluster; /**< Next entry index within the cluster.       */
  uint32_t scanned;          /**< Total entries read (P10 bound).            */
  uint32_t contig_end;       /**< One past the run's last cluster; 0 => FAT. */
} exfat_cursor_t;

/**
 * @struct exfat_dir_t
 * @brief Identifies the exFAT directory a lookup, scan or link operates in.
 *
 * @details The exFAT counterpart of ::dir_loc_t, and the thing that turns this
 *          driver's flat root-only namespace into a tree: every scan used to
 *          start at `m->root_cluster` at exactly one call site each, and each
 *          of those now starts at one of these instead. The root needs no
 *          special case -- it is the directory whose first cluster is
 *          `m->root_cluster` and whose run is FAT-chained.
 *
 *          `self_cluster` / `self_index` locate the directory's OWN File-entry
 *          set inside its parent, captured while ::priv_exfat_resolve_parent
 *          descends into it. Growing a subdirectory has to rewrite that set's
 *          Stream entry -- `DataLength` and the `NoFatChain` flag both change --
 *          so a directory that may need to grow carries the coordinates of the
 *          metadata that describes its allocation (#677). The volume ROOT has
 *          no such set (its extent is the boot sector's FAT chain), so it leaves
 *          `self_cluster` 0, which ::priv_exfat_grow_dir reads as "no entry set
 *          to patch -- just extend the FAT chain".
 *
 * @invariant `cluster` is a real heap cluster (>= ::k_cluster_first_data).
 * @invariant `contig_end` is 0 (FAT-chained) or > `cluster` (contiguous run).
 * @invariant `self_cluster` is 0 (the root, or a location never captured) or a
 *            heap cluster holding this directory's File entry.
 * @see priv_exfat_dir_root()
 * @see priv_exfat_dir_from_set()
 * @since 0.1.0
 */
typedef struct {
  uint32_t cluster;      /**< First cluster of the directory.                          */
  uint32_t contig_end;   /**< One past the run's last cluster; 0 => FAT-chained.       */
  uint32_t self_cluster; /**< Parent cluster holding this dir's File entry; 0 => root. */
  uint32_t self_index;   /**< File-entry index within `self_cluster`.                  */
} exfat_dir_t;

/**
 * @enum exfat_set_const_t
 * @brief Bounds on one exFAT directory entry set.
 *
 * @details exFAT spec sec 7.4: a File entry carries a SecondaryCount, and the
 *          set is that many 32-byte entries plus the File entry itself. This
 *          adapter's name cap (::k_exfat_name_cap, 64 UTF-16 units) needs one
 *          Stream entry plus `ceil(64 / 15)` Name entries, so ::k_exfat_set_writable
 *          is the largest set it can rewrite. ::k_exfat_set_max_entries is the
 *          largest the FORMAT allows and bounds the walk over a set some other
 *          implementation wrote.
 *
 * @invariant `k_exfat_set_writable <= k_exfat_set_max_entries`.
 *
 * @par Example:
 * @code
 * exfat_setpos_t pos[k_exfat_set_max_entries] = {};
 * @endcode
 *
 * @see priv_exfat_find_set()
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_exfat_set_max_entries = 19U, /**< 1 File + 1 Stream + 17 Name entries.      */
  k_exfat_set_writable    = 7U,  /**< 1 File + 1 Stream + 5 Name (64 chars).    */
  k_exfat_set_min_entries = 3U,  /**< The smallest legal set: File+Stream+Name. */
} exfat_set_const_t;

/**
 * @struct exfat_setpos_t
 * @brief Directory position (cluster + entry index) of one 32-byte entry.
 *
 * @details A directory is a cluster chain, so an entry's address is a cluster
 *          plus an index inside it and NOT a flat offset -- a set can straddle
 *          a cluster boundary and its entries then live in two different
 *          clusters. Everything that rewrites an entry in place carries these
 *          coordinates rather than recomputing them.
 *
 * @invariant `index` is below the directory's entries-per-cluster.
 *
 * @par Example:
 * @code
 * const exfat_setpos_t head = {.cluster = f->entry_set_cluster,
 *                              .index   = f->entry_set_index};
 * @endcode
 *
 * @see priv_exfat_find_set()
 * @since 0.1.0
 */
typedef struct {
  uint32_t cluster; /**< Directory cluster holding the entry.       */
  uint32_t index;   /**< Entry index within that cluster (0-based). */
} exfat_setpos_t;

/**
 * @struct ra8_fs_fmt_geom_t
 * @brief Computed on-disk geometry for one `ra8_fs_format()` run.
 *
 * @details Filled by `priv_fmt_choose_geometry()` from the requested type and
 *          the backend capacity, then consumed by the BPB / FAT writers. All
 *          counts are in 512-byte sectors.
 *
 * @invariant `sectors_per_cluster` is a power of two in 1..`k_fmt_spc_max`.
 * @invariant `count_of_clusters` lands in the band valid for `type`.
 */
typedef struct {
  ra8_fs_type_t type;                /**< Resolved FAT variant.                */
  uint32_t      total_sectors;       /**< Whole-device sector count.           */
  uint32_t      sectors_per_cluster; /**< Chosen cluster size (sectors).       */
  uint32_t      reserved_sectors;    /**< Boot + (FAT32) FSInfo/backup region. */
  uint32_t      fat_size_sectors;    /**< Sectors per FAT copy.                */
  uint32_t      root_entries;        /**< FAT12/16 root entries (0 on FAT32).  */
  uint32_t      root_sectors;        /**< FAT12/16 fixed-root sector span.     */
  uint32_t      count_of_clusters;   /**< Resulting data-region cluster count. */
} ra8_fs_fmt_geom_t;

/**
 * @var s_scratch
 * @brief Single 512-byte sector scratch buffer reused across all I/O.
 * @details One module-wide bounce buffer for every BPB, FAT, directory, and
 *          data-sector access. Defined once in `ra8_fs_fat_mount.c`.
 * @note Not reentrant; the adapter is single-threaded by contract.
 * @warning Do not access concurrently; callers serialise all FS operations.
 * @since 0.1.0
 */
extern uint8_t s_scratch[k_ra8_fs_bytes_per_sector];
