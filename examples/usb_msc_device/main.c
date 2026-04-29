/**
 * @file examples/usb_msc_device/main.c
 * @brief USB Mass Storage Class smoke test for EK-RA8D2 (USB-FS)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings the chip up via ``ra_cgc_init()`` (XTAL -> PLL1 -> CPUCLK0 =
 * 1 GHz, PCLKA = 125 MHz), routes the four USB-FS pins (mirrors
 * ``usb_cdc_echo``), then brings the device-mode MSC stack up via
 * ``ra_usb_pmsc``. A 32 KB SRAM RAM-disk is pre-formatted with a tiny
 * FAT12 image: one root-directory entry for ``README.TXT`` containing
 * the literal payload ``Hello from RA8D2!\r\n`` (19 bytes). After
 * enumeration the host sees a removable drive with that single file.
 *
 * The application loop pumps ``ra_usb_pmsc_step()`` (BOT phase
 * advance) and toggles LED2 once per host SCSI command so the bench
 * operator can see when the host probes the drive.
 *
 * ## FAT12 image layout (64 sectors of 512 bytes = 32768 bytes)
 *
 * | Sector | Purpose          | Notes                                 |
 * |--------|------------------|---------------------------------------|
 * | 0      | Boot sector / BPB| Media = 0xF8, FAT type "FAT12   "     |
 * | 1..2   | FAT #1 (2 sec)   | Entries 0,1,2 = 0xFF8, 0xFFF, 0xFFF   |
 * | 3..4   | FAT #2 (2 sec)   | Identical copy of FAT #1              |
 * | 5..6   | Root directory   | 32 entries x 32 B = 2 sectors         |
 * | 7      | Data cluster 2   | "Hello from RA8D2!\r\n", rest 0       |
 * | 8..63  | Data clusters 3+ | All zero (free space)                 |
 *
 * Layout follows Microsoft FAT spec rev 1.03 sec 3 ("BPB and BS"),
 * sec 4 ("FAT Data Structure") and sec 6 ("Directory Entry"). The
 * BPB / FAT / root-dir / payload bytes are pre-computed and held in
 * the four ``s_fat_*`` arrays below; ``usb_msc_format_ramdisk``
 * splats them into the right LBAs.
 *
 * The 12-bit FAT entries pack three bytes per pair of entries; the
 * first six FAT bytes are therefore:
 *   F8 FF FF -- entries 0,1 = 0xFF8, 0xFFF
 *   FF 0F 00 -- entries 2,3 = 0xFFF, 0x000 (EOC for README, then free)
 *
 * ## Pinout (USB-FS, FSP-aligned, mirrors usb_cdc_echo)
 *
 * P4_07 = VBUS, P5_00 = VBUSEN, P8_14 = D+, P8_15 = D-, all PSEL =
 * ``k_ra_psel_usb_fs``.
 *
 * ## Verification
 *
 *   - Linux:
 *       ``lsusb`` lists ``Brighton Sikarskie EK-RA8D2 RAM Disk``.
 *       ``udisksctl mount -b /dev/sdX1`` (or auto-mounts).
 *       ``ls`` shows ``README.TXT``; ``cat README.TXT`` prints
 *       ``Hello from RA8D2!``.
 *   - macOS: the volume mounts as ``/Volumes/RA8D2``; Finder shows
 *     README.TXT; ``cat /Volumes/RA8D2/README.TXT`` works.
 *
 * @par Architectural ring
 * [Ring 6 / APP] {World: S} -- application-layer code that runs in
 * the Secure world.
 *
 * @author Brighton Sikarskie
 * @date 2026-04-29
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_gpio_constants.h"
#include "ra_isr.h"
#include "ra_nsc_comms.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_time.h"
#include "ra_usb.h"
#include "ra_usb_pmsc.h"

/* =============================================================================
 * USB-FS pinout (mirrors usb_cdc_echo)
 * =============================================================================
 */

/**
 * @brief Packed ``ra_port_pin_t`` (port << 8 | pin) for the four
 *        USB-FS pins.
 */
static const ra_port_pin_t k_usb_msc_pin_vbus =
  (ra_port_pin_t)(((uint16_t)k_ra_port_4 << 8) | (uint16_t)k_ra_pin_7);
static const ra_port_pin_t k_usb_msc_pin_vbusen =
  (ra_port_pin_t)(((uint16_t)k_ra_port_5 << 8) | (uint16_t)k_ra_pin_0);
static const ra_port_pin_t k_usb_msc_pin_dp =
  (ra_port_pin_t)(((uint16_t)k_ra_port_8 << 8) | (uint16_t)k_ra_pin_14);
static const ra_port_pin_t k_usb_msc_pin_dm =
  (ra_port_pin_t)(((uint16_t)k_ra_port_8 << 8) | (uint16_t)k_ra_pin_15);

/* =============================================================================
 * RAM-disk geometry
 * =============================================================================
 */

/**
 * @enum usb_msc_geom_t
 * @brief 32 KB FAT12 RAM-disk layout.
 *
 * @details Sized so the whole disk fits in SRAM and so the FAT12
 * cluster count (57) is well above the FAT12 minimum (4084 clusters
 * is the FAT16 boundary; below that we are unambiguously FAT12 per
 * Microsoft FAT 1.03 sec 3.5 "Determination of FAT type").
 */
typedef enum : uint16_t {
  k_usb_msc_block_size   = 512U, /**< SCSI block size.        */
  k_usb_msc_total_blocks = 64U,  /**< 32 KB / 512 B.          */
  k_usb_msc_boot_lba     = 0U,   /**< Boot sector LBA.        */
  k_usb_msc_fat1_lba     = 1U,   /**< First FAT sector.       */
  k_usb_msc_fat2_lba     = 3U,   /**< Second FAT sector.      */
  k_usb_msc_root_lba     = 5U,   /**< Root dir start sector.  */
  k_usb_msc_data_lba     = 7U,   /**< Cluster 2 LBA.          */
} usb_msc_geom_t;

/**
 * @enum usb_msc_disk_size_t
 * @brief Total RAM-disk size in bytes (uint32_t to satisfy enum range).
 */
typedef enum : uint32_t {
  k_usb_msc_disk_bytes = 32768U, /**< 64 sectors x 512 bytes. */
} usb_msc_disk_size_t;

/**
 * @enum usb_msc_loop_t
 * @brief Loop pacing.
 */
typedef enum : uint16_t {
  k_usb_msc_step_period_ms = 1U, /**< BOT pump period -- short to keep
                                       the host happy on bulk traffic. */
} usb_msc_loop_t;

/* =============================================================================
 * Pre-computed FAT12 image fragments
 * =============================================================================
 *
 * These four ``static const`` byte arrays hold the BPB / FAT / root
 * directory / payload bytes. The values are byte-level data straight
 * from Microsoft FAT spec rev 1.03 -- comments alongside each row
 * decode them. clang-tidy ``readability-magic-numbers`` does not
 * flag values inside static initializers (treated as data, not
 * literals in expressions).
 */

/**
 * @var s_fat_boot_sector
 * @brief Sector 0: BPB + boot sector tail.
 *
 * @details Covers the first 64 bytes of FAT-1.03 BPB plus a boot
 * signature at offsets 510..511. The remaining 446 bytes between
 * are zero (BSS-initialised). This array sits in flash and is
 * copied into the RAM-disk by ``usb_msc_format_ramdisk``.
 */
static const uint8_t s_fat_boot_head[64] = {
  /* JMP 0x3C / NOP. */
  0xEB,
  0x3C,
  0x90,
  /* OEM "MSDOS5.0".                                     */
  'M',
  'S',
  'D',
  'O',
  'S',
  '5',
  '.',
  '0',
  /* BPB_BytsPerSec = 512 LE.                            */
  0x00,
  0x02,
  /* BPB_SecPerClus = 1.                                 */
  0x01,
  /* BPB_RsvdSecCnt = 1 LE.                              */
  0x01,
  0x00,
  /* BPB_NumFATs = 2.                                    */
  0x02,
  /* BPB_RootEntCnt = 32 LE.                             */
  0x20,
  0x00,
  /* BPB_TotSec16 = 64 LE.                               */
  0x40,
  0x00,
  /* BPB_Media = 0xF8 (fixed disk).                      */
  0xF8,
  /* BPB_FATSz16 = 2 LE.                                 */
  0x02,
  0x00,
  /* BPB_SecPerTrk = 1 LE.                               */
  0x01,
  0x00,
  /* BPB_NumHeads = 1 LE.                                */
  0x01,
  0x00,
  /* BPB_HiddSec = 0 LE32.                               */
  0x00,
  0x00,
  0x00,
  0x00,
  /* BPB_TotSec32 = 0 LE32 (using TotSec16).             */
  0x00,
  0x00,
  0x00,
  0x00,
  /* BS_DrvNum = 0x80, BS_Reserved1 = 0, BS_BootSig = 0x29. */
  0x80,
  0x00,
  0x29,
  /* BS_VolID = 0x12345678 LE.                           */
  0x78,
  0x56,
  0x34,
  0x12,
  /* BS_VolLab = "RA8D2      " (11 bytes).               */
  'R',
  'A',
  '8',
  'D',
  '2',
  ' ',
  ' ',
  ' ',
  ' ',
  ' ',
  ' ',
  /* BS_FilSysType = "FAT12   " (8 bytes).               */
  'F',
  'A',
  'T',
  '1',
  '2',
  ' ',
  ' ',
  ' ',
};

/**
 * @var s_fat_table
 * @brief Sector 1 (and 3): FAT entries.
 *
 * @details FAT12 packs 12-bit entries; the first three
 * (0=media-type, 1=EOC, 2=README EOC) live in six bytes:
 *   F8 FF FF FF 0F 00.
 * Remaining FAT bytes stay zero (free clusters).
 */
static const uint8_t s_fat_table[6] = {0xF8, 0xFF, 0xFF, 0xFF, 0x0F, 0x00};

/**
 * @var s_fat_root_entry
 * @brief Sector 5 byte 0..31: root directory entry for README.TXT.
 *
 * @details 8.3 short name, archive attribute, starting cluster 2,
 * file size 19 (the README payload length).
 */
static const uint8_t s_fat_root_entry[32] = {
  /* DIR_Name: "README  TXT" (8.3 SPACE-padded).         */
  'R',
  'E',
  'A',
  'D',
  'M',
  'E',
  ' ',
  ' ',
  'T',
  'X',
  'T',
  /* DIR_Attr = 0x20 ATTR_ARCHIVE.                       */
  0x20,
  /* DIR_NTRes (1) + DIR_CrtTimeTenth (1) + CrtTime (2) + CrtDate (2) + LstAccDate (2) = 8. */
  0x00,
  0x00,
  0x00,
  0x00,
  0x00,
  0x00,
  0x00,
  0x00,
  /* DIR_FstClusHI (FAT12 unused).                       */
  0x00,
  0x00,
  /* DIR_WrtTime, DIR_WrtDate.                           */
  0x00,
  0x00,
  0x00,
  0x00,
  /* DIR_FstClusLO = 2 LE.                               */
  0x02,
  0x00,
  /* DIR_FileSize = 19 LE32.                             */
  0x13,
  0x00,
  0x00,
  0x00,
};

/**
 * @var s_fat_payload
 * @brief Sector 7 byte 0..18: README.TXT payload.
 */
static const uint8_t s_fat_payload[19] = {
  'H', 'e', 'l', 'l', 'o', ' ', 'f', 'r', 'o', 'm', ' ', 'R', 'A', '8', 'D', '2', '!', '\r', '\n',
};

/**
 * @var s_fat_boot_sig
 * @brief Boot signature: 0x55 0xAA at sector-0 offset 510.
 */
static const uint8_t s_fat_boot_sig[2] = {0x55, 0xAA};

/* =============================================================================
 * RAM-disk storage backing
 * =============================================================================
 */

/**
 * @var s_ramdisk
 * @brief 32 KB SRAM block backing the FAT12 image.
 *
 * @details Initialised to zero by Reset_Handler's BSS clear, then
 * patched by ``usb_msc_format_ramdisk`` at startup. Static storage
 * so the FAT image lives for the firmware's entire lifetime.
 *
 * @note Direct modification by the host through SCSI WRITE(10)
 *       commands is allowed -- the host can rewrite README.TXT and
 *       the change persists until reset.
 *
 * @since 0.1.0
 */
static uint8_t s_ramdisk[k_usb_msc_disk_bytes];

/**
 * @brief Splat ``len`` source bytes into ``s_ramdisk`` at byte
 *        offset ``offset``.
 *
 * @param[in] offset Byte offset into ``s_ramdisk``.
 * @param[in] src    Source byte array.
 * @param[in] len    Number of bytes to copy.
 *
 * @pre offset + len <= k_usb_msc_disk_bytes.
 *
 * @post len bytes of ``s_ramdisk`` starting at ``offset`` carry the
 *       source contents.
 *
 * @since 0.1.0
 */
static void usb_msc_splat(uint32_t offset, const uint8_t* src, uint32_t len)
{
  for (uint32_t i = 0U; i < len; i++) {
    s_ramdisk[offset + i] = src[i];
  }
}

/**
 * @brief Patch the FAT12 image into ``s_ramdisk``.
 *
 * @details Splats the four pre-computed byte arrays into the right
 * LBAs. BSS-zero already covers all the gaps (FAT free entries,
 * unused root-dir slots, free data clusters). This keeps the
 * function under 60 lines per NASA Rule 4 and clang-tidy
 * ``readability-function-size``.
 *
 * @pre s_ramdisk fully zeroed (BSS clear).
 *
 * @post Boot sector / both FATs / root-dir / payload all in place.
 *
 * @since 0.1.0
 */
static void usb_msc_format_ramdisk(void)
{
  const uint32_t block_size      = (uint32_t)k_usb_msc_block_size;
  const uint32_t boot_sig_offset = 510U;

  /* Sector 0: BPB + boot signature. */
  usb_msc_splat(((uint32_t)k_usb_msc_boot_lba) * block_size,
                s_fat_boot_head,
                (uint32_t)sizeof(s_fat_boot_head));
  usb_msc_splat((((uint32_t)k_usb_msc_boot_lba) * block_size) + boot_sig_offset,
                s_fat_boot_sig,
                (uint32_t)sizeof(s_fat_boot_sig));

  /* Sectors 1..2: FAT #1. */
  usb_msc_splat(((uint32_t)k_usb_msc_fat1_lba) * block_size,
                s_fat_table,
                (uint32_t)sizeof(s_fat_table));
  /* Sectors 3..4: FAT #2 (identical copy). */
  usb_msc_splat(((uint32_t)k_usb_msc_fat2_lba) * block_size,
                s_fat_table,
                (uint32_t)sizeof(s_fat_table));

  /* Sector 5..6: root directory entry for README.TXT. */
  usb_msc_splat(((uint32_t)k_usb_msc_root_lba) * block_size,
                s_fat_root_entry,
                (uint32_t)sizeof(s_fat_root_entry));

  /* Sector 7: README payload (cluster 2). */
  usb_msc_splat(((uint32_t)k_usb_msc_data_lba) * block_size,
                s_fat_payload,
                (uint32_t)sizeof(s_fat_payload));
}

/* =============================================================================
 * Storage callbacks (DIP into ra_usb_pmsc)
 * =============================================================================
 */

/**
 * @brief Storage backend READ(10) hook.
 *
 * @param[in]  ctx Unused.
 * @param[in]  lba Starting logical block address.
 * @param[in]  block_count Number of blocks.
 * @param[out] buf Destination buffer.
 *
 * @return Error code.
 * @retval k_ra_ok                Bytes copied.
 * @retval k_ra_err_invalid_arg   Out of range.
 *
 * @pre ``buf`` non-NULL.
 *
 * @post On success ``block_count * 512`` bytes copied into ``buf``.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t
usb_msc_storage_read(void* ctx, uint32_t lba, uint32_t block_count, uint8_t* buf)
{
  (void)ctx;
  if ((lba + block_count) > (uint32_t)k_usb_msc_total_blocks) {
    return k_ra_err_invalid_arg;
  }
  uint32_t       bytes  = block_count * (uint32_t)k_usb_msc_block_size;
  const uint8_t* source = &s_ramdisk[(size_t)lba * (size_t)k_usb_msc_block_size];
  for (uint32_t i = 0U; i < bytes; i++) {
    buf[i] = source[i];
  }
  return k_ra_ok;
}

/**
 * @brief Storage backend WRITE(10) hook.
 *
 * @param[in] ctx Unused.
 * @param[in] lba Starting logical block address.
 * @param[in] block_count Number of blocks.
 * @param[in] buf Source buffer.
 *
 * @return Error code.
 * @retval k_ra_ok                Bytes written.
 * @retval k_ra_err_invalid_arg   Out of range.
 *
 * @pre ``buf`` non-NULL.
 *
 * @post On success ``block_count * 512`` bytes patched into the disk.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t
usb_msc_storage_write(void* ctx, uint32_t lba, uint32_t block_count, const uint8_t* buf)
{
  (void)ctx;
  if ((lba + block_count) > (uint32_t)k_usb_msc_total_blocks) {
    return k_ra_err_invalid_arg;
  }
  uint32_t bytes = block_count * (uint32_t)k_usb_msc_block_size;
  uint8_t* dest  = &s_ramdisk[(size_t)lba * (size_t)k_usb_msc_block_size];
  for (uint32_t i = 0U; i < bytes; i++) {
    dest[i] = buf[i];
  }
  return k_ra_ok;
}

/**
 * @brief Storage backend READ_CAPACITY(10) hook.
 *
 * @param[in]  ctx Unused.
 * @param[out] block_count Total block count (NOT count - 1).
 * @param[out] block_size  Bytes per block (512).
 *
 * @return ``k_ra_ok``.
 *
 * @pre Both pointers non-NULL.
 *
 * @post ``*block_count == 64``, ``*block_size == 512``.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t
usb_msc_storage_capacity(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  (void)ctx;
  *block_count = (uint32_t)k_usb_msc_total_blocks;
  *block_size  = (uint32_t)k_usb_msc_block_size;
  return k_ra_ok;
}

/**
 * @var s_inq_vendor
 * @brief 8-byte SPACE-padded T10 vendor ID returned by INQUIRY.
 */
static const uint8_t s_inq_vendor[8] = {'R', 'A', '8', 'D', '2', ' ', ' ', ' '};

/**
 * @var s_inq_product
 * @brief 16-byte SPACE-padded product ID returned by INQUIRY.
 */
static const uint8_t s_inq_product[16] = {
  'R',
  'A',
  'M',
  ' ',
  'D',
  'i',
  's',
  'k',
  ' ',
  ' ',
  ' ',
  ' ',
  ' ',
  ' ',
  ' ',
  ' ',
};

/**
 * @var s_inq_revision
 * @brief 4-byte product revision returned by INQUIRY.
 */
static const uint8_t s_inq_revision[4] = {'0', '0', '0', '1'};

/**
 * @brief Storage backend INQUIRY hook.
 *
 * @details Vendor / product / revision are SPACE-padded ASCII per
 * SBC-4 sec 6.6 "Standard INQUIRY data".
 *
 * @param[in]  ctx Unused.
 * @param[out] vendor8   8 bytes "RA8D2   ".
 * @param[out] product16 16 bytes "RAM Disk        ".
 * @param[out] revision4 4 bytes "0001".
 *
 * @return ``k_ra_ok``.
 *
 * @pre All three pointers non-NULL.
 *
 * @post Output buffers fully written with SPACE-padded strings.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t
usb_msc_storage_inquiry(void* ctx, uint8_t* vendor8, uint8_t* product16, uint8_t* revision4)
{
  (void)ctx;
  for (uint8_t i = 0U; i < (uint8_t)sizeof(s_inq_vendor); i++) {
    vendor8[i] = s_inq_vendor[i];
  }
  for (uint8_t i = 0U; i < (uint8_t)sizeof(s_inq_product); i++) {
    product16[i] = s_inq_product[i];
  }
  for (uint8_t i = 0U; i < (uint8_t)sizeof(s_inq_revision); i++) {
    revision4[i] = s_inq_revision[i];
  }
  return k_ra_ok;
}

/**
 * @var s_storage
 * @brief Storage backend bound to ``ra_usb_pmsc_attach_storage``.
 *
 * @details Four-pointer DIP struct -- the class layer calls these
 * back from BOT state-machine steps.
 *
 * @since 0.1.0
 */
static const ra_usb_pmsc_storage_t s_storage = {
  .read_block   = usb_msc_storage_read,
  .write_block  = usb_msc_storage_write,
  .get_capacity = usb_msc_storage_capacity,
  .get_inquiry  = usb_msc_storage_inquiry,
  .ctx          = nullptr,
};

/* =============================================================================
 * Helpers
 * =============================================================================
 */

/**
 * @brief Halt forever in WFI -- panic stop on init failure.
 *
 * @since 0.1.0
 */
static void usb_msc_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Route the four USB-FS pins (VBUS, VBUSEN, D+, D-) to the
 *        USBFS controller via PFS PSEL.
 *
 * @return Error code from the first failing route call, or k_ra_ok.
 *
 * @retval k_ra_ok All four pins routed.
 * @retval other   PFS routing failure.
 *
 * @pre IOPORT module reachable.
 * @pre Single-threaded init context.
 *
 * @post On success the four USB-FS pins are in USB peripheral mode.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t usb_msc_pins_init(void)
{
  ra_err_t err = ra_pfs_route_peripheral(k_usb_msc_pin_vbus, k_ra_psel_usb_fs, "usb_msc.vbus");
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_pfs_route_peripheral(k_usb_msc_pin_vbusen, k_ra_psel_usb_fs, "usb_msc.vbusen");
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_pfs_route_peripheral(k_usb_msc_pin_dp, k_ra_psel_usb_fs, "usb_msc.dp");
  if (err != k_ra_ok) {
    return err;
  }
  return ra_pfs_route_peripheral(k_usb_msc_pin_dm, k_ra_psel_usb_fs, "usb_msc.dm");
}

/**
 * @brief Bring up CGC, SysTick, USB-FS pin mux, LED2, USB controller,
 *        the device-MSC class, and attach the RAM-disk storage.
 *        Panic-halts on any failure.
 *
 * @since 0.1.0
 */
static void usb_msc_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;

  if (ra_cgc_init() != k_ra_ok) {
    usb_msc_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    usb_msc_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    usb_msc_panic_halt();
  }
  if (ra_gpio_output_init(k_ra_pin_led2, k_ra_level_low) != k_ra_ok) {
    usb_msc_panic_halt();
  }
  if (usb_msc_pins_init() != k_ra_ok) {
    usb_msc_panic_halt();
  }

  usb_msc_format_ramdisk();

  if (ra_nsc_usb_init(k_ra_usb_speed_fs) != k_ra_ok) {
    usb_msc_panic_halt();
  }
  if (ra_usb_pmsc_init(k_ra_usb_speed_fs) != k_ra_ok) {
    usb_msc_panic_halt();
  }
  if (ra_usb_pmsc_attach_storage(&s_storage) != k_ra_ok) {
    usb_msc_panic_halt();
  }
  if (ra_nsc_usb_attach(k_ra_usb_speed_fs, true) != k_ra_ok) {
    usb_msc_panic_halt();
  }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry. Brings up CGC + USB-FS + MSC + RAM-disk
 *        storage, then pumps the BOT state machine forever.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 *
 * @post On clean entry the CPU stays in the BOT pump loop forever.
 * @post On any HAL init failure the function halts in WFI.
 *
 * @since 0.1.0
 */
int32_t main(void)
{
  usb_msc_setup_or_halt();
  ra_isr_globals_enable();

  while (1) {
    ra_err_t err = ra_usb_pmsc_step();
    if (err != k_ra_ok) {
      /* No CBW pending -> nothing to do this tick. Other failures
       * also fall through to the back-off so the operator can see
       * LED2 stop toggling and reset the board. */
      ra_delay_ms((uint32_t)k_usb_msc_step_period_ms);
      continue;
    }
    /* One LED2 toggle per BOT phase advance == per host SCSI command. */
    if (ra_gpio_toggle(k_ra_pin_led2) != k_ra_ok) {
      break;
    }
  }

  usb_msc_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
