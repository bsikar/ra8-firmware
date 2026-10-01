/**
 * @file usb_selftest_device.c
 * @brief Device side of the soak self-test: USBX MSC over a FAT16 MRAM volume
 *
 * @details
 * The USBFS device half of the self-loop: a USBX Mass-Storage class exposing the
 * 1 MiB MRAM window at 0x02000000 as a read-only synthesized FAT16 volume
 * (MRAM.BIN). Owns the USB framework descriptors, the FAT16 sector synthesis,
 * the MSC media callbacks, the USBX device-stack bring-up, and the device worker
 * thread entry (selftest_device_worker, spawned by tx_application_define in
 * main.c). Split out of main.c (see usb_selftest_common.h).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "usb_selftest_device.h"

#include <stdint.h>
#include <string.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_err.h"
#include "ra8_usb.h"
#include "ra8_usb_compose.h"
#include "ra8_usb_desc.h"
#include "usb_selftest_common.h"

#ifndef RA8_OFF_TARGET
#include "tx_api.h"
#include "ux_api.h"
#include "ux_dcd_ra8_usb.h"
#include "ux_device_class_storage.h"
#include "ux_device_stack.h"

/* -------------------------------------------------------------------------- */
/* Device state: USBX pool, SCSI/FAT identity strings, J-Link probe */
/* -------------------------------------------------------------------------- */

static UCHAR s_usbx_pool[k_selftest_usbx_pool_bytes];

/* SCSI INQUIRY strings -- 8 / 16 / 4 byte fields per SBC-3. */
static UCHAR s_msc_vendor_id[]   = "RA8D2   ";
static UCHAR s_msc_product_id[]  = "SELFTEST MRAM RO";
static UCHAR s_msc_product_rev[] = "0001";

/** @brief Boot-sector OEM name (8 bytes, space padded). */
static const UCHAR s_fat_oem_name[8] = {'R', 'A', '8', 'D', '2', 'F', 'W', ' '};

/** @brief Volume label, 11 bytes space padded (also the root entry). */
static const UCHAR s_fat_volume_label[11] = {'R', 'A', '8', 'D', '2', ' ', 'M', 'R', 'A', 'M', ' '};

/** @brief Filesystem-type tag, 8 bytes space padded. */
static const UCHAR s_fat_fs_type[8] = {'F', 'A', 'T', '1', '6', ' ', ' ', ' '};

/** @brief 8.3 directory name of the exposed file: "MRAM.BIN". */
static const UCHAR s_fat_file_name[11] = {'M', 'R', 'A', 'M', ' ', ' ', ' ', ' ', 'B', 'I', 'N'};

/* -------------------------------------------------------------------------- */
/* J-Link probes */
/* -------------------------------------------------------------------------- */

/** @brief Device-side media_read invocations. */
static volatile uint32_t s_dbg_read_calls;

/* -------------------------------------------------------------------------- */
/* USB descriptors (DEVICE + CONFIG + MSC interface + endpoints) */
/* -------------------------------------------------------------------------- */

/* The three frameworks below are synthesised at start-up from the config
 * structs by libs/ra8_usb_pal (RA8FW-317) rather than typed out as raw byte
 * arrays. Single-interface MSC: bulk-only transport over the SCSI transparent
 * command set, class 0x08 / subclass 0x06 / protocol 0x50, EP1 IN and EP2 OUT
 * at a 64-byte MPS, per BBB rev 1.0 sec 4 + USB 2.0 sec 9.6. The device
 * descriptor advertises class 0 so the class triple is read off the interface.
 * The configuration block's wTotalLength, 32 bytes a human used to add up by
 * hand, is now derived from what the builder actually emitted.
 */

/**
 * @enum demo_usb_identity_t
 * @brief The device identity this demo publishes.
 *
 * @details ::k_demo_usb_max_power_ma is the real milliamp draw, not the
 * halved bMaxPower encoding; the builder halves it. Bus-powered is the
 * default, because advertising self-powered alongside a 100 mA draw is the
 * contradiction this app's own descriptor comment warned about.
 */
typedef enum : uint16_t {
  k_demo_usb_vid          = 0x1209U, /**< idVendor, pid.codes test range. */
  k_demo_usb_pid          = 0x000EU, /**< idProduct.                      */
  k_demo_usb_bcd_device   = 0x0100U, /**< bcdDevice, release 1.00.        */
  k_demo_usb_max_power_ma = 100U,    /**< Bus draw in mA.                 */
} demo_usb_identity_t;

/**
 * @enum demo_usb_endpoint_t
 * @brief The bulk-only endpoint layout, addresses as they appear on the wire.
 *
 * @details An IN endpoint carries bit 7 set, so EP1 IN is 0x81, while EP2 OUT
 * is 0x02. That is how the byte array this block replaces wrote them, which
 * keeps the two diffable.
 */
typedef enum : uint16_t {
  k_demo_usb_in_ep      = 0x81U, /**< Bulk-IN data pipe.              */
  k_demo_usb_out_ep     = 0x02U, /**< Bulk-OUT data pipe.             */
  k_demo_usb_data_bytes = 64U,   /**< Bulk max packet size, FS.       */
  k_demo_usb_functions  = 1U,    /**< Functions the device publishes. */
} demo_usb_endpoint_t;

/**
 * @var k_demo_usb_device
 * @brief Device identity handed to the framework builders.
 * @note The three strings are string-literal storage with static duration;
 *       the builders copy them and retain no pointer.
 * @since 0.1.0
 */
static const ra8_usb_desc_device_t k_demo_usb_device = {
  .vid           = (uint16_t)k_demo_usb_vid,
  .pid           = (uint16_t)k_demo_usb_pid,
  .bcd_device    = (uint16_t)k_demo_usb_bcd_device,
  .manufacturer  = "Brighton Sikarskie",
  .product       = "RA8D2 SELFTEST",
  .serial        = "00000004",
  .langid        = (uint16_t)k_ra8_usb_desc_langid_en_us,
  .max_power_ma  = (uint16_t)k_demo_usb_max_power_ma,
  .self_powered  = false,
  .remote_wakeup = false,
};

/**
 * @var k_demo_usb_msc
 * @brief Bulk-only endpoint layout handed to the framework builder.
 * @since 0.1.0
 */
static const ra8_usb_desc_msc_t k_demo_usb_msc = {
  .in_ep      = (uint8_t)k_demo_usb_in_ep,
  .out_ep     = (uint8_t)k_demo_usb_out_ep,
  .data_bytes = (uint16_t)k_demo_usb_data_bytes,
  .high_speed = false,
};

/**
 * @var s_device_framework_fs
 * @brief Synthesised device framework: device descriptor + configuration.
 * @note Written once by ::demo_usb_build_frameworks, then read-only.
 * @since 0.1.0
 */
static uint8_t s_device_framework_fs[k_ra8_usb_desc_framework_bytes_max];

/**
 * @var s_string_framework
 * @brief Synthesised string framework: manufacturer, product, serial.
 * @note Written once by ::demo_usb_build_frameworks, then read-only.
 * @since 0.1.0
 */
static uint8_t s_string_framework[k_ra8_usb_desc_strings_bytes_max];

/**
 * @var s_language_id_framework
 * @brief Synthesised language-id framework -- US English.
 * @note Written once by ::demo_usb_build_frameworks, then read-only.
 * @since 0.1.0
 */
static uint8_t s_language_id_framework[k_ra8_usb_desc_langid_bytes];

/**
 * @var s_device_framework_len
 * @brief Bytes ::demo_usb_build_frameworks wrote to ::s_device_framework_fs.
 * @since 0.1.0
 */
static uint32_t s_device_framework_len = 0U;

/**
 * @var s_string_framework_len
 * @brief Bytes ::demo_usb_build_frameworks wrote to ::s_string_framework.
 * @since 0.1.0
 */
static uint32_t s_string_framework_len = 0U;

/**
 * @var s_language_id_framework_len
 * @brief Bytes written to ::s_language_id_framework.
 * @since 0.1.0
 */
static uint32_t s_language_id_framework_len = 0U;

/**
 * @brief Synthesise the three USB frameworks this demo enumerates with.
 *
 * @details Replaces the three hand-typed byte arrays this app used to carry.
 * Nothing here touches a controller: a synthesised framework is bytes, not an
 * attached device.
 *
 * @return ra8_err_t Result of the three encodes.
 * @retval k_ra8_ok               All three frameworks written.
 * @retval k_ra8_err_invalid_size A destination buffer is too small.
 * @retval k_ra8_err_invalid_arg  An endpoint address or packet size is wrong.
 *
 * @pre Called from thread context before ``_ux_device_stack_initialize``.
 * @post On success the three buffers hold the frameworks and the three
 *       length variables count them.
 * @post On failure the lengths of the encodes that did not run stay 0.
 *
 * @note Single-call; the builders are pure, so a repeat call is harmless.
 * @since 0.1.0
 */
static ra8_err_t demo_usb_build_frameworks(void)
{
  const ra8_usb_class_t function = {
    .kind = k_ra8_usb_class_msc,
    .msc  = k_demo_usb_msc,
  };
  const ra8_usb_device_cfg_t cfg = {
    .desc        = &k_demo_usb_device,
    .classes     = &function,
    .class_count = (uint8_t)k_demo_usb_functions,
  };
  ra8_usb_device_frameworks_t fw = {
    .device      = s_device_framework_fs,
    .device_cap  = (uint32_t)sizeof(s_device_framework_fs),
    .strings     = s_string_framework,
    .strings_cap = (uint32_t)sizeof(s_string_framework),
    .langid      = s_language_id_framework,
    .langid_cap  = (uint32_t)sizeof(s_language_id_framework),
  };

  const ra8_err_t composed = ra8_usb_device_compose(&cfg, &fw);
  if (composed != k_ra8_ok) {
    return composed;
  }

  s_device_framework_len      = fw.device_len;
  s_string_framework_len      = fw.strings_len;
  s_language_id_framework_len = fw.langid_len;
  return k_ra8_ok;
}

/* -------------------------------------------------------------------------- */
/* FAT16 synthesis (identical layout to usb_msc_mram) */
/* -------------------------------------------------------------------------- */

/**
 * @brief Write a 16-bit value little-endian into a byte buffer.
 *
 * @details Low byte first, high byte second, per the FAT on-disk layout.
 *
 * @param[out] dst   Destination (2 bytes).
 * @param[in]  value Value to store.
 *
 * @pre @p dst has 2 writable bytes.
 * @pre None beyond the buffer contract.
 * @post ``dst[0]`` holds the low byte, ``dst[1]`` the high byte.
 * @post No other state changes.
 *
 * @note Pure function.
 * @since 0.1.0
 */
static void selftest_put16(UCHAR* dst, uint16_t value)
{
  dst[0] = (UCHAR)(value & (uint16_t)k_byte_mask);
  dst[1] = (UCHAR)((value >> (uint16_t)k_byte_shift) & (uint16_t)k_byte_mask);
}

/**
 * @brief Write a 32-bit value little-endian into a byte buffer.
 *
 * @details Two ::selftest_put16 halves, low half-word first.
 *
 * @param[out] dst   Destination (4 bytes).
 * @param[in]  value Value to store.
 *
 * @pre @p dst has 4 writable bytes.
 * @pre None beyond the buffer contract.
 * @post @p dst holds the four little-endian bytes of @p value.
 * @post No other state changes.
 *
 * @note Pure function.
 * @since 0.1.0
 */
static void selftest_put32(UCHAR* dst, uint32_t value)
{
  selftest_put16(dst, (uint16_t)(value & (uint32_t)k_word_mask));
  selftest_put16(dst + 2U, (uint16_t)(value >> (uint32_t)k_word_shift));
}

/**
 * @brief Synthesize the FAT16 boot sector (MS FAT spec 1.03 sec 3.1).
 *
 * @details BPB for the padded 4146-sector volume plus the 0x55AA
 * signature; geometry constants in ::selftest_fat_geom_t.
 *
 * @param[out] out Zeroed 512-byte sector buffer.
 *
 * @pre @p out is zeroed.
 * @pre Geometry constants describe a valid FAT16 volume.
 * @post @p out holds the BPB + 0x55AA signature.
 * @post No other state changes.
 *
 * @note Pure function.
 * @since 0.1.0
 */
static void selftest_fat_fill_boot(UCHAR* out)
{
  out[k_bpb_off_jmp]      = (UCHAR)k_boot_jmp0;
  out[k_bpb_off_jmp + 1U] = (UCHAR)k_boot_jmp1;
  out[k_bpb_off_jmp + 2U] = (UCHAR)k_boot_jmp2;
  (void)memcpy(&out[k_bpb_off_oem], s_fat_oem_name, sizeof(s_fat_oem_name));
  selftest_put16(&out[k_bpb_off_bps], (uint16_t)k_selftest_block_size);
  out[k_bpb_off_spc] = 1U;
  selftest_put16(&out[k_bpb_off_rsvd], (uint16_t)k_fat_reserved_sectors);
  out[k_bpb_off_nfats] = (UCHAR)k_fat_num_fats;
  selftest_put16(&out[k_bpb_off_rootent], (uint16_t)k_fat_root_entries);
  selftest_put16(&out[k_bpb_off_totsec16], (uint16_t)k_fat_total_sectors);
  out[k_bpb_off_media] = (UCHAR)k_boot_media;
  selftest_put16(&out[k_bpb_off_fatsz16], (uint16_t)k_fat_fat_sectors);
  selftest_put16(&out[k_bpb_off_spt], (uint16_t)k_boot_sec_per_trk);
  selftest_put16(&out[k_bpb_off_heads], (uint16_t)k_boot_num_heads);
  out[k_bpb_off_drvnum]  = (UCHAR)k_boot_drive_num;
  out[k_bpb_off_bootsig] = (UCHAR)k_boot_ext_sig;
  selftest_put32(&out[k_bpb_off_volid], (uint32_t)k_boot_volume_id);
  (void)memcpy(&out[k_bpb_off_label], s_fat_volume_label, sizeof(s_fat_volume_label));
  (void)memcpy(&out[k_bpb_off_fstype], s_fat_fs_type, sizeof(s_fat_fs_type));
  out[k_boot_sig_lo_off] = (UCHAR)k_boot_sig_lo;
  out[k_boot_sig_hi_off] = (UCHAR)k_boot_sig_hi;
}

/**
 * @brief Synthesize one FAT16 sector of the cluster chain.
 *
 * @details MRAM.BIN occupies clusters 2..2049 as one sequential chain
 * (entry c -> c + 1, last entry -> end-of-chain). Entries 0/1 carry
 * the media descriptor per the FAT spec; everything past the chain
 * reads as free (0x0000).
 *
 * @param[in]  fat_sector Index of the FAT sector (0-based).
 * @param[out] out        Zeroed 512-byte sector buffer.
 *
 * @pre @p out is zeroed.
 * @pre @p fat_sector is below ::k_fat_fat_sectors.
 * @post @p out holds 256 little-endian FAT16 entries.
 * @post No other state changes.
 *
 * @note Pure function.
 * @since 0.1.0
 */
static void selftest_fat_fill_fat(uint32_t fat_sector, UCHAR* out)
{
  const uint32_t first_entry = fat_sector * (uint32_t)k_fat_entries_per_sec;
  for (uint32_t j = 0U; j < (uint32_t)k_fat_entries_per_sec; j++) {
    const uint32_t entry = first_entry + j;
    uint16_t       value = 0U;
    if (entry == 0U) {
      value = (uint16_t)k_fat_entry0;
    } else if (entry == 1U) {
      value = (uint16_t)k_fat_eoc;
    } else if (entry < (uint32_t)k_fat_last_mram_clus) {
      value = (uint16_t)(entry + 1U);
    } else if (entry == (uint32_t)k_fat_last_mram_clus) {
      value = (uint16_t)k_fat_eoc;
    } else {
      value = 0U;
    }
    selftest_put16(&out[j * 2U], value);
  }
}

/**
 * @brief Synthesize one root-directory sector.
 *
 * @details Sector 0 of the root carries two entries: the volume label
 * and the read-only ``MRAM.BIN`` file (start cluster 2, size 1 MiB).
 * Every other root sector is empty.
 *
 * @param[in]  root_sector Index of the root sector (0-based).
 * @param[out] out         Zeroed 512-byte sector buffer.
 *
 * @pre @p out is zeroed.
 * @pre @p root_sector is below ::k_fat_root_sectors.
 * @post @p out holds the directory entries for that sector.
 * @post No other state changes.
 *
 * @note Pure function.
 * @since 0.1.0
 */
static void selftest_fat_fill_root(uint32_t root_sector, UCHAR* out)
{
  if (root_sector != 0U) {
    return;
  }
  /* Entry 0: volume label. */
  (void)memcpy(&out[0], s_fat_volume_label, (size_t)k_dir_name_bytes);
  out[k_dir_off_attr] = (UCHAR)k_dir_attr_volume;
  /* Entry 1: MRAM.BIN, read-only, cluster 2, 1 MiB. */
  UCHAR* entry = &out[k_dir_entry_bytes];
  (void)memcpy(entry, s_fat_file_name, (size_t)k_dir_name_bytes);
  entry[k_dir_off_attr] = (UCHAR)k_dir_attr_read_only;
  selftest_put16(&entry[k_dir_off_cluster_lo], (uint16_t)k_fat_first_cluster);
  selftest_put32(&entry[k_dir_off_size], (uint32_t)k_mram_bytes);
}

/**
 * @brief Synthesize one 512-byte sector of the read-only volume.
 *
 * @details Dispatches on the LBA: boot sector, FAT, root directory, or
 * data region. Data sectors inside the MRAM.BIN chain are copied
 * straight out of the 1 MiB MRAM window; padding clusters past the
 * chain read as zeros.
 *
 * @param[in]  lba Logical block address inside the volume.
 * @param[out] out 512-byte destination buffer.
 *
 * @pre @p lba is below ::k_fat_total_sectors (caller-checked).
 * @pre @p out has 512 writable bytes.
 * @post @p out holds the synthesized sector content.
 * @post No other state changes.
 *
 * @note Reads chip MRAM directly; no caching.
 * @since 0.1.0
 */
static void selftest_fat_fill_sector(uint32_t lba, UCHAR* out)
{
  (void)memset(out, 0, (size_t)k_selftest_block_size);
  if (lba == 0U) {
    selftest_fat_fill_boot(out);
    return;
  }
  if (lba < (uint32_t)k_fat_root_lba) {
    selftest_fat_fill_fat(lba - (uint32_t)k_fat_fat_lba, out);
    return;
  }
  if (lba < (uint32_t)k_fat_data_lba) {
    selftest_fat_fill_root(lba - (uint32_t)k_fat_root_lba, out);
    return;
  }
  const uint32_t cluster = (lba - (uint32_t)k_fat_data_lba) + (uint32_t)k_fat_first_cluster;
  if (cluster <= (uint32_t)k_fat_last_mram_clus) {
    const uint32_t offset =
      (cluster - (uint32_t)k_fat_first_cluster) * (uint32_t)k_selftest_block_size;
    const UCHAR* mram = (const UCHAR*)(uintptr_t)((uint32_t)k_mram_base_addr + offset);
    (void)memcpy(out, mram, (size_t)k_selftest_block_size);
  }
}

/* -------------------------------------------------------------------------- */
/* Storage class media callbacks (read / write / status) */
/* -------------------------------------------------------------------------- */

/**
 * @brief Storage media-read callback: synthesize sectors over MRAM.
 *
 * @details Bound checks the request against the volume, then fills each
 * block via ::selftest_fat_fill_sector. LED1 toggles per call so the
 * self-loop traffic is visible on the board.
 *
 * @param[in,out] storage      USBX storage class instance (unused).
 * @param[in]     lun          Logical unit number (must be 0).
 * @param[out]    data_pointer USBX-owned destination buffer.
 * @param[in]     number_blocks Number of 512-byte blocks to produce.
 * @param[in]     lba          Starting LBA.
 * @param[out]    media_status Filled with sense status word.
 *
 * @return ``UX_SUCCESS`` if the request fits the volume; otherwise
 *         ``UX_ERROR`` with media_status set to ILLEGAL REQUEST.
 * @retval UX_SUCCESS Read completed.
 * @retval UX_ERROR   Out-of-range LBA / count.
 *
 * @pre ``data_pointer`` and ``media_status`` are non-NULL (USBX
 *      guarantee).
 * @pre ``lun`` is 0 (single-LUN device).
 * @post Either ``number_blocks * 512`` bytes were synthesized or
 *       ``media_status`` is non-zero.
 * @post ::s_dbg_read_calls advanced.
 *
 * @note Called from the USBX storage class thread.
 * @since 0.1.0
 */
static UINT selftest_msc_read(VOID*  storage,
                              ULONG  lun,
                              UCHAR* data_pointer,
                              ULONG  number_blocks,
                              ULONG  lba,
                              ULONG* media_status)
{
  (void)storage;
  (void)lun;
  s_dbg_read_calls++;
  if ((lba + number_blocks) > (ULONG)k_fat_total_sectors) {
    *media_status = UX_DEVICE_CLASS_STORAGE_SENSE_STATUS(k_scsi_sense_illegal_request,
                                                         k_scsi_asc_lba_out_of_range,
                                                         k_scsi_ascq_none);
    return UX_ERROR;
  }
  for (ULONG i = 0UL; i < number_blocks; i++) {
    selftest_fat_fill_sector((uint32_t)(lba + i), &data_pointer[i * (ULONG)k_selftest_block_size]);
  }
  *media_status = 0UL;
  (void)ra8_board_led_toggle(k_ra8_board_led1);
  return UX_SUCCESS;
}

/**
 * @brief Storage media-write callback: always rejects (write-protected).
 *
 * @details The host side of this very app probes exactly this rejection
 * (WRITE(10) must fail with DATA PROTECT and the transport must keep
 * working afterwards).
 *
 * @param[in,out] storage      USBX storage class instance (unused).
 * @param[in]     lun          Logical unit number (unused).
 * @param[in]     data_pointer USBX-owned source buffer (unused).
 * @param[in]     number_blocks Number of blocks the host tried (unused).
 * @param[in]     lba          Starting LBA (unused).
 * @param[out]    media_status Filled with DATA PROTECT sense.
 *
 * @return Always ``UX_ERROR``.
 * @retval UX_ERROR The medium is write-protected.
 *
 * @pre ``media_status`` is non-NULL (USBX guarantee).
 * @pre The LUN also reports write-protected via MODE SENSE.
 * @post ``*media_status`` carries the DATA PROTECT sense triple.
 * @post The MRAM window is untouched.
 *
 * @note Hosts honouring the MODE SENSE WP bit never call this.
 * @since 0.1.0
 */
static UINT selftest_msc_write(VOID*  storage,
                               ULONG  lun,
                               UCHAR* data_pointer,
                               ULONG  number_blocks,
                               ULONG  lba,
                               ULONG* media_status)
{
  (void)storage;
  (void)lun;
  (void)data_pointer;
  (void)number_blocks;
  (void)lba;
  *media_status = UX_DEVICE_CLASS_STORAGE_SENSE_STATUS(k_scsi_sense_data_protect,
                                                       k_scsi_asc_write_protected,
                                                       k_scsi_ascq_none);
  return UX_ERROR;
}

/**
 * @brief Storage media-status callback. Always reports media-present.
 *
 * @details The synthesized volume cannot go away; status is constant 0.
 *
 * @param[in,out] storage      USBX storage class instance (unused).
 * @param[in]     lun          Logical unit number (unused).
 * @param[in]     media_id     Media id (unused).
 * @param[out]    media_status Filled with 0 (no fault).
 *
 * @return Always ``UX_SUCCESS``.
 * @retval UX_SUCCESS Media is present and ready.
 *
 * @pre ``media_status`` is non-NULL (USBX guarantee).
 * @pre The class instance is live.
 * @post ``*media_status`` is 0.
 * @post No other state changes.
 *
 * @note Synthesized volume; never reports media-not-present.
 * @since 0.1.0
 */
static UINT selftest_msc_status(VOID* storage, ULONG lun, ULONG media_id, ULONG* media_status)
{
  (void)storage;
  (void)lun;
  (void)media_id;
  *media_status = 0UL;
  return UX_SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* Threads */
/* -------------------------------------------------------------------------- */

/**
 * @brief Brings USBX system + FS device stack up.
 *
 * @details One-shot USBX pool + device-stack initialization for the
 * FS-only framework.
 *
 * @return UINT UX_SUCCESS on success.
 * @retval UX_SUCCESS Stack ready.
 *
 * @pre File-scope pool reserved.
 * @pre Thread context.
 * @post Device stack accepts class registrations.
 * @post On failure, USBX state is undefined.
 *
 * @note Single-call; not idempotent.
 * @since 0.1.0
 */
static UINT selftest_usbx_stack_up(void)
{
  if (_ux_system_initialize(s_usbx_pool, k_selftest_usbx_pool_bytes, UX_NULL, 0) != UX_SUCCESS) {
    return UX_ERROR;
  }
  if (demo_usb_build_frameworks() != k_ra8_ok) {
    return UX_ERROR;
  }
  return _ux_device_stack_initialize((UCHAR*)UX_NULL,
                                     0,
                                     (UCHAR*)s_device_framework_fs,
                                     (ULONG)s_device_framework_len,
                                     (UCHAR*)s_string_framework,
                                     (ULONG)s_string_framework_len,
                                     (UCHAR*)s_language_id_framework,
                                     (ULONG)s_language_id_framework_len,
                                     UX_NULL);
}

/**
 * @brief Registers the Mass-Storage class with the read-only MRAM LUN.
 *
 * @details Single LUN, write-protected, FAT16 geometry from
 * ::selftest_fat_geom_t, media callbacks above.
 *
 * @return UINT UX_SUCCESS on success.
 * @retval UX_SUCCESS Class registered.
 *
 * @pre ::selftest_usbx_stack_up has succeeded.
 * @pre Media read/write/status callbacks are defined.
 * @post MSC class bound to configuration 1, interface 0.
 * @post LUN0 advertises the read-only synthesized FAT16 volume.
 *
 * @note Not re-entrant.
 * @since 0.1.0
 */
static UINT selftest_msc_class_register(void)
{
  UX_SLAVE_CLASS_STORAGE_PARAMETER msc_params;
  (void)memset(&msc_params, 0, sizeof(msc_params));
  msc_params.ux_slave_class_storage_parameter_number_lun  = 1UL;
  msc_params.ux_slave_class_storage_parameter_vendor_id   = s_msc_vendor_id;
  msc_params.ux_slave_class_storage_parameter_product_id  = s_msc_product_id;
  msc_params.ux_slave_class_storage_parameter_product_rev = s_msc_product_rev;

  msc_params.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_last_lba =
    (ULONG)k_fat_total_sectors - 1UL;
  msc_params.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_block_length =
    (ULONG)k_selftest_block_size;
  msc_params.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_type =
    UX_SLAVE_CLASS_STORAGE_MEDIA_FAT_DISK;
  msc_params.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_removable_flag =
    UX_SLAVE_CLASS_STORAGE_MEDIA_IS_REMOVABLE;
  msc_params.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_read_only_flag =
    UX_TRUE;
  msc_params.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_read =
    selftest_msc_read;
  msc_params.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_write =
    selftest_msc_write;
  msc_params.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_status =
    selftest_msc_status;

  static UCHAR s_class_name[] = "ux_slave_class_storage";

  return _ux_device_stack_class_register(s_class_name,
                                         _ux_device_class_storage_entry,
                                         1,
                                         0,
                                         &msc_params);
}

/**
 * @brief Device-side worker: bring the FS device stack up, then park.
 *
 * @details USBX system + device stack + MSC class + DCD bridge on the
 * USBFS controller, then DPRPU attach. USBX runs the SCSI/BBB state
 * machine on its own class threads after this.
 *
 * @param[in] arg ThreadX entry argument (unused).
 *
 * @pre tx_application_define created this thread.
 * @pre USB-FS pins + 48 MHz clock are up (main did both).
 * @post The FS device is attached and serviceable.
 * @post On any bring-up failure the thread exits (probes show where).
 *
 * @note Runs once; loops forever on success.
 * @since 0.1.0
 */
VOID selftest_device_worker(ULONG arg)
{
  (void)arg;

  if (selftest_usbx_stack_up() != UX_SUCCESS) {
    return;
  }
  if (selftest_msc_class_register() != UX_SUCCESS) {
    return;
  }
  if (ux_dcd_ra8_usb_initialize(k_ra8_usb_speed_fs) != k_ra8_ok) {
    return;
  }
  if (ra8_usb_device_attach(k_ra8_usb_speed_fs, true) != k_ra8_ok) {
    return;
  }

  /* Idle. USBX runs the SCSI/BBB state machine on its own threads. */
  while (1) {
    tx_thread_sleep(k_selftest_idle_ticks);
  }
}

#endif /* !RA8_OFF_TARGET */
