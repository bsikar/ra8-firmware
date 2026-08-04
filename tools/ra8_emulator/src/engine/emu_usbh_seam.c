/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file emu_usbh_seam.c
 * @brief Virtual USB host-mode device seams (see emu_usbh_seam.h)
 *
 * @details
 * The HID boot-keyboard peer for the ra8_usb_host_* primitives and the FAT16
 * MSC disk peer for the ra8_usb_hmsc_* class API (boot/FAT/root synthesis,
 * the live-MRAM data region, and the small write overlay for the writable
 * variant) -- moved verbatim out of the ra8_emulator main translation unit.
 *
 *
 * @since 0.1.0
 */

#include "emu_usbh_seam.h"

#include <stdio.h>
#include <string.h>

#include "emu_elf.h"
#include "emu_exc.h"
#include "emu_trace.h"

/** @brief ra8_err_t values the virtual devices return to the host stack. */
typedef enum : uint32_t {
  k_ra8_err_no_data  = 0x10AU, /**< ra8_err_t value: no RX data.    */
  k_ra8_err_inval_st = 0x104U, /**< ra8_err_t value: invalid state. */
} usbh_err_code_t;

/* ============================================================================
 * Virtual USB host-mode device: a HID boot keyboard behind the ra8_usb_host_*
 * seam.
 *
 * board_usb.c models the USBFS controller in DEVICE mode (a virtual host drives
 * the firmware's device stack). The inverse case -- the firmware acting as USB
 * HOST -- drives the USBHS controller (0x40351000, unmodelled) through the
 * first-party `ra8_usb_host_*` primitives, and with no peer the control transfer
 * wedges (SUREQ never acked -> k_ra8_err_busy). Rather than model a second
 * controller register-by-register, seam those primitives the same way ra8_eth_*
 * is seamed to a virtual net peer (the register model "cannot satisfy" that
 * sequence either): present a virtual boot keyboard that answers chapter-9
 * enumeration and streams interrupt-IN reports. This lets a host example
 * (usb_host_keyboard) enumerate + read reports end to end with no hardware --
 * validating the host stack's control/data logic, not silicon timing.
 * ==========================================================================*/

/** @brief bRequest / descriptor-type / sizing constants for the virtual device. */
typedef enum : uint16_t {
  k_vkbd_breq_get_descriptor = 0x06U, /**< Standard GET_DESCRIPTOR bRequest.       */
  k_vkbd_dt_device           = 0x01U, /**< DEVICE descriptor (wValue hi byte).     */
  k_vkbd_dt_config           = 0x02U, /**< CONFIGURATION descriptor.               */
  k_vkbd_dt_string           = 0x03U, /**< STRING descriptor.                      */
  k_vkbd_dt_hid_report       = 0x22U, /**< HID REPORT descriptor.                  */
  k_vkbd_lnst_attached       = 0x02U, /**< SYSSTS0.LNST J-state (device on bus).   */
  k_vkbd_report_len          = 8U,    /**< Boot-keyboard input report width.       */
  k_vkbd_num_keys            = 5U,    /**< Keycodes typed ("R A 8 D 2").           */
  k_vkbd_dev_desc_len        = 18U,   /**< DEVICE descriptor length.               */
  k_vkbd_cfg_desc_len        = 34U,   /**< Full CONFIGURATION descriptor length.   */
  k_vkbd_stop_reports        = 8U,    /**< Reports streamed before USB_STOP fires. */
} vkbd_const_t;

/** @brief 18-byte DEVICE descriptor: class defined at interface, EP0 MPS 64. */
static const uint8_t k_vkbd_device_desc[k_vkbd_dev_desc_len] = {
  0x12,
  0x01,
  0x00,
  0x02,
  0x00,
  0x00,
  0x00,
  0x40, /* len,DEVICE,bcdUSB2.00,class0,MPS64 */
  0x6A,
  0x1A,
  0x88,
  0x42,
  0x00,
  0x01,
  0x00,
  0x00, /* idVendor 0x1A6A, idProduct 0x4288 */
  0x00,
  0x01, /* bcdDevice, iM/iP/iS=0, 1 config */
};

/** @brief 34-byte CONFIGURATION: 1 HID boot-keyboard iface, 1 interrupt-IN EP1. */
static const uint8_t k_vkbd_config_desc[k_vkbd_cfg_desc_len] = {
  0x09, 0x02, 0x22, 0x00, 0x01, 0x01, 0x00, 0xA0, 0x32, /* CONFIG: wTotalLen 34, 1 iface */
  0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x01, 0x01, 0x00, /* IFACE: HID, boot, keyboard    */
  0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x3F, 0x00, /* HID: report desc len 0x3F     */
  0x07, 0x05, 0x81, 0x03, 0x40, 0x00, 0x01,             /* EP1 IN, interrupt, MPS64, 1ms */
};

/** @brief Standard boot-keyboard HID REPORT descriptor (63 bytes, USB HID 1.11 E.6). */
static const uint8_t k_vkbd_report_desc[63] = {
  0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
  0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x05, 0x75, 0x01,
  0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0x95, 0x06,
  0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0,
};

/** @brief HID Usage-Table keycodes (Usage Page 0x07) for the typed string. */
typedef enum : uint8_t {
  k_vkbd_key_r = 0x15U, /**< HID usage for 'R'. */
  k_vkbd_key_a = 0x04U, /**< HID usage for 'A'. */
  k_vkbd_key_8 = 0x25U, /**< HID usage for '8'. */
  k_vkbd_key_d = 0x07U, /**< HID usage for 'D'. */
  k_vkbd_key_2 = 0x1FU, /**< HID usage for '2'. */
} vkbd_keycode_t;

/** @brief HID Usage-Table keycodes the virtual keyboard "types": R A 8 D 2. */
static const uint8_t k_vkbd_keycodes[k_vkbd_num_keys] = {k_vkbd_key_r,
                                                         k_vkbd_key_a,
                                                         k_vkbd_key_8,
                                                         k_vkbd_key_d,
                                                         k_vkbd_key_2};

static uint8_t  s_vkbd_seq           = 0U; /**< Rolling report seq (report byte 0). */
static uint32_t s_vkbd_ctrl_serviced = 0U; /**< Control transfers answered.         */
static uint32_t s_vkbd_reports_sent  = 0U; /**< Interrupt-IN reports streamed.      */

/** @brief Read the 5th (stack-passed) argument of an AAPCS call: mem32[SP]. */
static uint32_t usbh_arg5(uc_engine* uc)
{
  uint32_t sp = 0U;
  uint32_t p  = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_SP, &sp);
  (void)uc_mem_read(uc, (uint64_t)sp, &p, sizeof(p));
  return p;
}

/** @brief Hook a host primitive that just succeeds (init / reset / target / pipe). */
/* cppcheck-suppress constParameterCallback ; UC_HOOK_CODE callback ABI is void*. */
static void on_usbh_ok(uc_engine* uc, uint64_t address, uint32_t size, void* user)
{
  (void)address;
  (void)size;
  (void)user;
  eth_hook_return(uc, 0U); /* k_ra8_ok */
}

/** @brief Hook ra8_usb_host_line_state(): report the virtual device attached. */
/* cppcheck-suppress constParameterCallback ; UC_HOOK_CODE callback ABI is void*. */
static void on_usbh_line_state(uc_engine* uc, uint64_t address, uint32_t size, void* user)
{
  (void)address;
  (void)size;
  (void)user;
  eth_hook_return(uc, (uint32_t)k_vkbd_lnst_attached);
}

/** @brief Hook ra8_usb_host_control_xfer(): answer chapter-9 from the virtual device. */
/* cppcheck-suppress constParameterCallback ; UC_HOOK_CODE callback ABI is void*. */
static void on_usbh_control_xfer(uc_engine* uc, uint64_t address, uint32_t size, void* user)
{
  (void)address;
  (void)size;
  (void)user;
  uint32_t setup_ptr = 0U;
  uint32_t data_ptr  = 0U;
  uint32_t data_len  = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_R1, &setup_ptr);
  (void)uc_reg_read(uc, UC_ARM_REG_R2, &data_ptr);
  (void)uc_reg_read(uc, UC_ARM_REG_R3, &data_len);
  const uint32_t out_ptr = usbh_arg5(uc);

  uint8_t s[8] = {};
  (void)uc_mem_read(uc, (uint64_t)setup_ptr, s, sizeof(s));
  const uint8_t  b_request = s[1];
  const uint8_t  desc_type = s[3]; /* wValue high byte = descriptor type. */
  const uint8_t* src       = nullptr;
  uint16_t       src_len   = 0U;
  if (b_request == (uint8_t)k_vkbd_breq_get_descriptor) {
    if (desc_type == (uint8_t)k_vkbd_dt_device) {
      src     = k_vkbd_device_desc;
      src_len = (uint16_t)k_vkbd_dev_desc_len;
    } else if (desc_type == (uint8_t)k_vkbd_dt_config) {
      src     = k_vkbd_config_desc;
      src_len = (uint16_t)k_vkbd_cfg_desc_len;
    } else if (desc_type == (uint8_t)k_vkbd_dt_hid_report) {
      src     = k_vkbd_report_desc;
      src_len = (uint16_t)sizeof(k_vkbd_report_desc);
    }
  }
  uint16_t n = 0U;
  if ((src != nullptr) && (data_ptr != 0U)) {
    n = (src_len < (uint16_t)data_len) ? src_len : (uint16_t)data_len;
    (void)uc_mem_write(uc, (uint64_t)data_ptr, src, n);
  }
  /* No-data control writes (SET_ADDRESS / SET_CONFIGURATION / SET_IDLE /
   * SET_PROTOCOL) just leave n = 0; the host treats that as a successful ack. */
  if (out_ptr != 0U) {
    (void)uc_mem_write(uc, (uint64_t)out_ptr, &n, sizeof(n));
  }
  s_vkbd_ctrl_serviced++;
  eth_hook_return(uc, 0U); /* k_ra8_ok */
}

/** @brief Hook ra8_usb_host_bulk_in(): stream one boot-keyboard input report. */
/* cppcheck-suppress constParameterCallback ; UC_HOOK_CODE callback ABI is void*. */
static void on_usbh_bulk_in(uc_engine* uc, uint64_t address, uint32_t size, void* user)
{
  (void)address;
  (void)size;
  (void)user;
  uint32_t buf     = 0U;
  uint32_t max_len = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_R2, &buf);
  (void)uc_reg_read(uc, UC_ARM_REG_R3, &max_len);
  const uint32_t out_ptr = usbh_arg5(uc);
  /* Boot-keyboard report: [seq][reserved 0][keycodes R A 8 D 2][0]. The host
   * ignores byte 0 and pattern-checks bytes 1.. -- it streams "RA8D2". */
  uint8_t rep[k_vkbd_report_len] = {};
  rep[0]                         = s_vkbd_seq++;
  for (uint32_t i = 0U; i < (uint32_t)k_vkbd_num_keys; i++) {
    rep[2U + i] = k_vkbd_keycodes[i];
  }
  uint16_t n = (uint16_t)k_vkbd_report_len;
  if ((uint32_t)n > max_len) {
    n = (uint16_t)max_len;
  }
  if (buf != 0U) {
    (void)uc_mem_write(uc, (uint64_t)buf, rep, n);
  }
  if (out_ptr != 0U) {
    (void)uc_mem_write(uc, (uint64_t)out_ptr, &n, sizeof(n));
  }
  s_vkbd_reports_sent++;
  eth_hook_return(uc, 0U); /* k_ra8_ok */
}

/* ----------------------------------------------------------------------------
 * Virtual USB host-mode MSC device: a read-only FAT16 disk whose one file
 * MRAM.BIN is the 1 MiB MRAM code window. Seams the first-party `ra8_usb_hmsc_*`
 * class API (one level above the BOT/SCSI bulk transport) so a host MSC app
 * (usb_host_msc_browse) enumerates, READ_CAPACITYs, mounts the FAT16, browses
 * the root directory, and content-verifies MRAM.BIN -- all with no peer device.
 * The boot/FAT/root sectors are a byte-identical replica of the device's
 * selftest_fat_fill_sector; the data region is read live from emulated MRAM, so
 * it matches the host's own MRAM compare byte-for-byte.
 * --------------------------------------------------------------------------*/

/** @brief FAT16 geometry + boot/dir layout for the virtual MSC volume. */
typedef enum : uint32_t {
  k_vmsc_block_size     = 512U,        /**< Logical block size.                    */
  k_vmsc_total_sectors  = 4146U,       /**< 1 reserved + 17 FAT + 32 root + 4096.  */
  k_vmsc_root_lba       = 18U,         /**< First root-directory LBA.              */
  k_vmsc_data_lba       = 50U,         /**< First data-region LBA (cluster 2).     */
  k_vmsc_first_cluster  = 2U,          /**< FAT data area starts at cluster 2.     */
  k_vmsc_last_mram_clus = 2049U,       /**< Last cluster of MRAM.BIN.              */
  k_vmsc_entries_per_fs = 256U,        /**< FAT16 entries per 512-byte sector.     */
  k_vmsc_overlay_slots  = 64U,         /**< Overwritten sectors the overlay holds. */
  k_vmsc_mram_base      = 0x02000000U, /**< MRAM window base (MRAM.BIN data).      */
  k_vmsc_fat_entry0     = 0xFFF8U,     /**< FAT[0]: media F8 + filler.             */
  k_vmsc_fat_eoc        = 0xFFFFU,     /**< End-of-chain marker.                   */
  k_vmsc_file_bytes     = 0x00100000U, /**< MRAM.BIN size: 1 MiB.                  */
  k_vmsc_volid          = 0x52A8D20AU, /**< Boot-sector volume serial.             */
} vmsc_const_t;

/** @brief FAT16 BPB byte offsets, fixed field values, and store shifts. */
typedef enum : uint32_t {
  k_bpb_shift8         = 8U,    /**< Byte 1 store shift.                */
  k_bpb_shift16        = 16U,   /**< Byte 2 store shift.                */
  k_bpb_shift24        = 24U,   /**< Byte 3 store shift.                */
  k_bpb_jmp0           = 0xEBU, /**< BS_jmpBoot[0]: short jump opcode.  */
  k_bpb_jmp1           = 0x3CU, /**< BS_jmpBoot[1]: jump displacement.  */
  k_bpb_jmp2           = 0x90U, /**< BS_jmpBoot[2]: NOP.                */
  k_bpb_off_oem        = 3U,    /**< BS_OEMName offset.                 */
  k_bpb_off_bytspersec = 11U,   /**< BPB_BytsPerSec offset.             */
  k_bpb_off_secperclus = 13U,   /**< BPB_SecPerClus offset.             */
  k_bpb_off_rsvdseccnt = 14U,   /**< BPB_RsvdSecCnt offset.             */
  k_bpb_off_numfats    = 16U,   /**< BPB_NumFATs offset.                */
  k_bpb_off_rootentcnt = 17U,   /**< BPB_RootEntCnt offset.             */
  k_bpb_off_totsec16   = 19U,   /**< BPB_TotSec16 offset.               */
  k_bpb_off_media      = 21U,   /**< BPB_Media offset.                  */
  k_bpb_off_fatsz16    = 22U,   /**< BPB_FATSz16 offset.                */
  k_bpb_off_secpertrk  = 24U,   /**< BPB_SecPerTrk offset.              */
  k_bpb_off_numheads   = 26U,   /**< BPB_NumHeads offset.               */
  k_bpb_off_drvnum     = 36U,   /**< BS_DrvNum offset.                  */
  k_bpb_off_bootsig    = 38U,   /**< BS_BootSig offset.                 */
  k_bpb_off_volid      = 39U,   /**< BS_VolID offset.                   */
  k_bpb_off_vollab     = 43U,   /**< BS_VolLab offset.                  */
  k_bpb_off_filsystype = 54U,   /**< BS_FilSysType offset.              */
  k_bpb_off_sig0       = 510U,  /**< 0x55 signature byte.               */
  k_bpb_off_sig1       = 511U,  /**< 0xAA signature byte.               */
  k_bpb_secperclus_1   = 1U,    /**< 1 sector per cluster.              */
  k_bpb_rsvdseccnt_1   = 1U,    /**< 1 reserved sector.                 */
  k_bpb_numfats_1      = 1U,    /**< 1 FAT copy.                        */
  k_bpb_rootentcnt_512 = 512U,  /**< 512 root-directory entries.        */
  k_bpb_media_f8       = 0xF8U, /**< Fixed-disk media descriptor.       */
  k_bpb_fatsz16_17     = 17U,   /**< 17 sectors per FAT.                */
  k_bpb_secpertrk_32   = 32U,   /**< 32 sectors per track.              */
  k_bpb_numheads_16    = 16U,   /**< 16 heads.                          */
  k_bpb_drvnum_80      = 0x80U, /**< Drive number (first fixed disk).   */
  k_bpb_bootsig_29     = 0x29U, /**< Extended boot signature.           */
  k_bpb_sig0_55        = 0x55U, /**< Boot-sector signature byte 0.      */
  k_bpb_sig1_aa        = 0xAAU, /**< Boot-sector signature byte 1.      */
  k_dir_off_attr       = 11U,   /**< Directory-entry attribute byte.    */
  k_dir_off_entry      = 32U,   /**< Second 32-byte directory entry.    */
  k_dir_off_fstcluslo  = 26U,   /**< DIR_FstClusLO offset within entry. */
  k_dir_off_filesize   = 28U,   /**< DIR_FileSize offset within entry.  */
  k_dir_attr_vollabel  = 0x08U, /**< ATTR_VOLUME_ID.                    */
  k_dir_attr_readonly  = 0x01U, /**< ATTR_READ_ONLY.                    */
} vmsc_bpb_t;

static const uint8_t k_vmsc_oem[8]    = {'R', 'A', '8', 'D', '2', 'F', 'W', ' '};
static const uint8_t k_vmsc_label[11] = {'R', 'A', '8', 'D', '2', ' ', 'M', 'R', 'A', 'M', ' '};
static const uint8_t k_vmsc_fstype[8] = {'F', 'A', 'T', '1', '6', ' ', ' ', ' '};
static const uint8_t k_vmsc_fname[11] = {'M', 'R', 'A', 'M', ' ', ' ', ' ', ' ', 'B', 'I', 'N'};

/** @brief Set once the host attempts a WRITE(10) into the READ-ONLY disk -- the
 *  last host step before usb_host_msc_browse's PASS (read by the USBH_STOP guard).*/
static bool s_vmsc_write_seen = false;

/** @brief True when the virtual disk is writable (usb_host_file_ops links
 *  `fileops_backend_write`); else the disk is read-only and WRITE(10) is rejected.*/
static bool s_vmsc_writable = false;

/**
 * @struct vmsc_overlay_t
 * @brief One overwritten sector of the otherwise-synthesized FAT16 volume.
 * @details A writable host (usb_host_file_ops) creates a file: ra8_fs rewrites a
 * few FAT / root / data sectors. We keep those writes in a small overlay so the
 * read-back reads them back; everything else is still synthesized on the fly.
 */
typedef struct {
  uint32_t lba;                     /**< Overwritten LBA.             */
  bool     valid;                   /**< Slot in use.                 */
  uint8_t  data[k_vmsc_block_size]; /**< The written 512-byte sector. */
} vmsc_overlay_t;

/** @brief Write overlay for the writable disk (file_ops touches only a handful). */
static vmsc_overlay_t s_vmsc_overlay[k_vmsc_overlay_slots];

/** @brief Return an overwritten sector if @p lba is in the overlay. */
static bool vmsc_overlay_get(uint32_t lba, uint8_t* out)
{
  for (uint32_t i = 0U; i < (uint32_t)(sizeof(s_vmsc_overlay) / sizeof(s_vmsc_overlay[0])); i++) {
    if (s_vmsc_overlay[i].valid && (s_vmsc_overlay[i].lba == lba)) {
      (void)memcpy(out, s_vmsc_overlay[i].data, (size_t)k_vmsc_block_size);
      return true;
    }
  }
  return false;
}

/** @brief Record an overwritten sector (update existing slot or take a free one). */
static void vmsc_overlay_put(uint32_t lba, const uint8_t* in)
{
  const uint32_t slots = (uint32_t)(sizeof(s_vmsc_overlay) / sizeof(s_vmsc_overlay[0]));
  for (uint32_t i = 0U; i < slots; i++) {
    if (s_vmsc_overlay[i].valid && (s_vmsc_overlay[i].lba == lba)) {
      (void)memcpy(s_vmsc_overlay[i].data, in, (size_t)k_vmsc_block_size);
      return;
    }
  }
  for (uint32_t i = 0U; i < slots; i++) {
    if (!s_vmsc_overlay[i].valid) {
      s_vmsc_overlay[i].lba   = lba;
      s_vmsc_overlay[i].valid = true;
      (void)memcpy(s_vmsc_overlay[i].data, in, (size_t)k_vmsc_block_size);
      return;
    }
  }
}

/** @brief Little-endian 16-bit store into a sector buffer. */
static void vmsc_put16(uint8_t* p, uint16_t v)
{
  p[0] = (uint8_t)(v & (uint16_t)k_byte_mask);
  p[1] = (uint8_t)((v >> (uint16_t)k_bpb_shift8) & (uint16_t)k_byte_mask);
}

/** @brief Little-endian 32-bit store into a sector buffer. */
static void vmsc_put32(uint8_t* p, uint32_t v)
{
  p[0] = (uint8_t)(v & (uint32_t)k_byte_mask);
  p[1] = (uint8_t)((v >> (uint32_t)k_bpb_shift8) & (uint32_t)k_byte_mask);
  p[2] = (uint8_t)((v >> (uint32_t)k_bpb_shift16) & (uint32_t)k_byte_mask);
  p[3] = (uint8_t)((v >> (uint32_t)k_bpb_shift24) & (uint32_t)k_byte_mask);
}

/** @brief Synthesize the FAT16 boot sector (BPB), mirroring the device side. */
static void vmsc_fill_boot(uint8_t* out)
{
  out[0] = (uint8_t)k_bpb_jmp0;
  out[1] = (uint8_t)k_bpb_jmp1;
  out[2] = (uint8_t)k_bpb_jmp2; /* jmp + nop */
  (void)memcpy(&out[k_bpb_off_oem], k_vmsc_oem, sizeof(k_vmsc_oem));
  vmsc_put16(&out[k_bpb_off_bytspersec], (uint16_t)k_vmsc_block_size);
  out[k_bpb_off_secperclus] = (uint8_t)k_bpb_secperclus_1;                /* sectors/cluster  */
  vmsc_put16(&out[k_bpb_off_rsvdseccnt], (uint16_t)k_bpb_rsvdseccnt_1);   /* reserved sectors */
  out[k_bpb_off_numfats] = (uint8_t)k_bpb_numfats_1;                      /* number of FATs   */
  vmsc_put16(&out[k_bpb_off_rootentcnt], (uint16_t)k_bpb_rootentcnt_512); /* root entries     */
  vmsc_put16(&out[k_bpb_off_totsec16], (uint16_t)k_vmsc_total_sectors);
  out[k_bpb_off_media] = (uint8_t)k_bpb_media_f8;                      /* media descriptor   */
  vmsc_put16(&out[k_bpb_off_fatsz16], (uint16_t)k_bpb_fatsz16_17);     /* sectors per FAT    */
  vmsc_put16(&out[k_bpb_off_secpertrk], (uint16_t)k_bpb_secpertrk_32); /* sectors per track  */
  vmsc_put16(&out[k_bpb_off_numheads], (uint16_t)k_bpb_numheads_16);   /* heads              */
  out[k_bpb_off_drvnum]  = (uint8_t)k_bpb_drvnum_80;                   /* drive number       */
  out[k_bpb_off_bootsig] = (uint8_t)k_bpb_bootsig_29;                  /* ext boot signature */
  vmsc_put32(&out[k_bpb_off_volid], (uint32_t)k_vmsc_volid);
  (void)memcpy(&out[k_bpb_off_vollab], k_vmsc_label, sizeof(k_vmsc_label));
  (void)memcpy(&out[k_bpb_off_filsystype], k_vmsc_fstype, sizeof(k_vmsc_fstype));
  out[k_bpb_off_sig0] = (uint8_t)k_bpb_sig0_55;
  out[k_bpb_off_sig1] = (uint8_t)k_bpb_sig1_aa;
}

/** @brief Synthesize one FAT sector: MRAM.BIN chains clusters 2..2049. */
static void vmsc_fill_fat(uint32_t fat_sector, uint8_t* out)
{
  const uint32_t first = fat_sector * (uint32_t)k_vmsc_entries_per_fs;
  for (uint32_t j = 0U; j < (uint32_t)k_vmsc_entries_per_fs; j++) {
    const uint32_t entry = first + j;
    uint16_t       value = 0U;
    if (entry == 0U) {
      value = (uint16_t)k_vmsc_fat_entry0;
    } else if ((entry == 1U) || (entry == (uint32_t)k_vmsc_last_mram_clus)) {
      /* Entry 1 is the reserved media/EOC marker and the last cluster ends the
       * chain -- both hold the same EOC word. */
      value = (uint16_t)k_vmsc_fat_eoc;
    } else if (entry < (uint32_t)k_vmsc_last_mram_clus) {
      value = (uint16_t)(entry + 1U);
    }
    vmsc_put16(&out[(size_t)j * 2U], value);
  }
}

/** @brief Synthesize root-directory sector 0: volume label + MRAM.BIN entry. */
static void vmsc_fill_root(uint32_t root_sector, uint8_t* out)
{
  if (root_sector != 0U) {
    return;
  }
  (void)memcpy(&out[0], k_vmsc_label, sizeof(k_vmsc_label));
  out[k_dir_off_attr] = (uint8_t)k_dir_attr_vollabel; /* volume-label attribute */
  uint8_t* e          = &out[k_dir_off_entry];
  (void)memcpy(e, k_vmsc_fname, sizeof(k_vmsc_fname));
  e[k_dir_off_attr] = (uint8_t)k_dir_attr_readonly; /* read-only attribute */
  vmsc_put16(&e[k_dir_off_fstcluslo], (uint16_t)k_vmsc_first_cluster);
  vmsc_put32(&e[k_dir_off_filesize], (uint32_t)k_vmsc_file_bytes);
}

/** @brief Fill one 512-byte volume sector (boot / FAT / root / live MRAM data). */
static void vmsc_fill_sector(uc_engine* uc, uint32_t lba, uint8_t* out)
{
  (void)memset(out, 0, (size_t)k_vmsc_block_size);
  if (lba == 0U) {
    vmsc_fill_boot(out);
  } else if (lba < (uint32_t)k_vmsc_root_lba) {
    vmsc_fill_fat(lba - 1U, out);
  } else if (lba < (uint32_t)k_vmsc_data_lba) {
    vmsc_fill_root(lba - (uint32_t)k_vmsc_root_lba, out);
  } else {
    const uint32_t cluster = (lba - (uint32_t)k_vmsc_data_lba) + (uint32_t)k_vmsc_first_cluster;
    if (cluster <= (uint32_t)k_vmsc_last_mram_clus) {
      const uint32_t off = (cluster - (uint32_t)k_vmsc_first_cluster) * (uint32_t)k_vmsc_block_size;
      (void)
        uc_mem_read(uc, (uint64_t)k_vmsc_mram_base + (uint64_t)off, out, (size_t)k_vmsc_block_size);
    }
  }
}

/** @brief Hook a host MSC primitive that just succeeds (init / close). */
/* cppcheck-suppress constParameterCallback ; UC_HOOK_CODE callback ABI is void*. */
static void on_hmsc_ok(uc_engine* uc, uint64_t address, uint32_t size, void* user)
{
  (void)address;
  (void)size;
  (void)user;
  eth_hook_return(uc, 0U); /* k_ra8_ok */
}

/** @brief ra8_usb_hmsc_device_t field offsets + reported bulk EP packet/VID/PID. */
typedef enum : uint32_t {
  k_hmsc_dev_bytes  = 14U,     /**< Marshalled ra8_usb_hmsc_device_t size.  */
  k_hmsc_off_vid    = 10U,     /**< vid offset in ra8_usb_hmsc_device_t.    */
  k_hmsc_off_pid    = 12U,     /**< pid offset in ra8_usb_hmsc_device_t.    */
  k_hmsc_bulk_mps   = 64U,     /**< Reported bulk-endpoint max packet size. */
  k_hmsc_vendor_id  = 0x1A6AU, /**< Reported USB vendor_id.                 */
  k_hmsc_product_id = 0x4288U, /**< Reported USB product_id.                */
} hmsc_dev_t;

/** @brief Hook ra8_usb_hmsc_enumerate(out_device*): report the virtual disk. */
/* cppcheck-suppress constParameterCallback ; UC_HOOK_CODE callback ABI is void*. */
static void on_hmsc_enumerate(uc_engine* uc, uint64_t address, uint32_t size, void* user)
{
  (void)address;
  (void)size;
  (void)user;
  uint32_t dev_ptr = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_R0, &dev_ptr);
  if (dev_ptr != 0U) {
    /* ra8_usb_hmsc_device_t: addr,bin_ep,bout_ep,max_lun,iface,[pad],in_mps,
     * out_mps,vid,pid. */
    uint8_t d[k_hmsc_dev_bytes] = {};
    d[0]                        = 1U;                            /* device_address      */
    d[1]                        = 1U;                            /* bulk_in_ep          */
    d[2]                        = 2U;                            /* bulk_out_ep         */
    vmsc_put16(&d[6], (uint16_t)k_hmsc_bulk_mps);                /* bulk_in_max_packet  */
    vmsc_put16(&d[8], (uint16_t)k_hmsc_bulk_mps);                /* bulk_out_max_packet */
    vmsc_put16(&d[k_hmsc_off_vid], (uint16_t)k_hmsc_vendor_id);  /* vendor_id           */
    vmsc_put16(&d[k_hmsc_off_pid], (uint16_t)k_hmsc_product_id); /* product_id          */
    (void)uc_mem_write(uc, (uint64_t)dev_ptr, d, sizeof(d));
  }
  eth_hook_return(uc, 0U); /* k_ra8_ok */
}

/** @brief Hook ra8_usb_hmsc_read_capacity(lun, *block_count, *block_size). */
/* cppcheck-suppress constParameterCallback ; UC_HOOK_CODE callback ABI is void*. */
static void on_hmsc_read_capacity(uc_engine* uc, uint64_t address, uint32_t size, void* user)
{
  (void)address;
  (void)size;
  (void)user;
  uint32_t bc_ptr = 0U;
  uint32_t bs_ptr = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_R1, &bc_ptr);
  (void)uc_reg_read(uc, UC_ARM_REG_R2, &bs_ptr);
  const uint32_t block_count = (uint32_t)k_vmsc_total_sectors;
  const uint32_t block_size  = (uint32_t)k_vmsc_block_size;
  if (bc_ptr != 0U) {
    (void)uc_mem_write(uc, (uint64_t)bc_ptr, &block_count, sizeof(block_count));
  }
  if (bs_ptr != 0U) {
    (void)uc_mem_write(uc, (uint64_t)bs_ptr, &block_size, sizeof(block_size));
  }
  eth_hook_return(uc, 0U); /* k_ra8_ok */
}

/** @brief Hook ra8_usb_hmsc_read10(lun, lba, count, out_buf): serve sectors. */
/* cppcheck-suppress constParameterCallback ; UC_HOOK_CODE callback ABI is void*. */
static void on_hmsc_read10(uc_engine* uc, uint64_t address, uint32_t size, void* user)
{
  (void)address;
  (void)size;
  (void)user;
  uint32_t lba   = 0U;
  uint32_t count = 0U;
  uint32_t buf   = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_R1, &lba);
  (void)uc_reg_read(uc, UC_ARM_REG_R2, &count);
  (void)uc_reg_read(uc, UC_ARM_REG_R3, &buf);
  count &= (uint32_t)k_lo16_mask; /* block_count is a uint16_t argument. */
  for (uint32_t i = 0U; (i < count) && (buf != 0U); i++) {
    uint8_t sec[k_vmsc_block_size];
    if (!vmsc_overlay_get(lba + i, sec)) { /* a host WRITE(10) wins over the synthesis. */
      vmsc_fill_sector(uc, lba + i, sec);
    }
    (void)uc_mem_write(uc,
                       (uint64_t)buf + ((uint64_t)i * (uint64_t)k_vmsc_block_size),
                       sec,
                       sizeof(sec));
  }
  eth_hook_return(uc, 0U); /* k_ra8_ok */
}

/** @brief Hook ra8_usb_hmsc_write10(): reject -- the volume is read-only. */
/* cppcheck-suppress constParameterCallback ; UC_HOOK_CODE callback ABI is void*. */
static void on_hmsc_write10(uc_engine* uc, uint64_t address, uint32_t size, void* user)
{
  (void)address;
  (void)size;
  (void)user;
  if (!s_vmsc_writable) {
    /* Read-only disk (usb_host_msc_browse): reject. This is that app's last step
     * before PASS, so flag it for the host early-stop. */
    s_vmsc_write_seen = true;
    eth_hook_return(uc, (uint32_t)k_ra8_err_inval_st); /* write protected */
    return;
  }
  /* Writable disk (usb_host_file_ops): stash the written sectors in the overlay
   * so the host's read-back sees them, and accept. The write is mid-ladder here,
   * so do NOT trip the write-seen early-stop -- that app stops on its banner. */
  uint32_t lba   = 0U;
  uint32_t count = 0U;
  uint32_t buf   = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_R1, &lba);
  (void)uc_reg_read(uc, UC_ARM_REG_R2, &count);
  (void)uc_reg_read(uc, UC_ARM_REG_R3, &buf);
  count &= (uint32_t)k_lo16_mask;
  for (uint32_t i = 0U; (i < count) && (buf != 0U); i++) {
    uint8_t sec[k_vmsc_block_size];
    (void)uc_mem_read(uc,
                      (uint64_t)buf + ((uint64_t)i * (uint64_t)k_vmsc_block_size),
                      sec,
                      sizeof(sec));
    vmsc_overlay_put(lba + i, sec);
  }
  eth_hook_return(uc, 0U); /* k_ra8_ok -- write accepted */
}

/**
 * @brief Install the virtual USB host-mode device seam if the host stack is linked.
 *
 * @details Picks the virtual device class from the firmware's linked host stack:
 * an MSC host (links `ra8_usb_hmsc_read10`) gets a read-only FAT16 disk seamed at
 * the `ra8_usb_hmsc_*` class API; otherwise a USB-host-capable firmware (links
 * `ra8_usb_host_control_xfer`) gets a HID boot keyboard seamed at the
 * `ra8_usb_host_*` primitives. Device-mode apps link neither call path, so the
 * hooks are inert there and board_usb.c's device-mode virtual host is untouched.
 *
 * @param[in,out] uc  Active Unicorn engine.
 * @param[in]     elf Loaded ELF image (symbol resolution).
 * @param[in]     len ELF image length in bytes.
 *
 * @pre @p uc is initialised and @p elf holds @p len bytes.
 * @post On a host app, the linked host API answers a virtual device.
 *
 * @note No effect on device-mode apps (the hooked symbols are never called).
 * @return true when a seam family was installed (the register-level USBHS
 *         host model must then stay dormant -- see board_usb_host.h).
 * @retval true  hmsc- or primitive-level seams now shadow the host API.
 * @retval false No usb-host seams; the register path is the real one.
 * @since 0.1.0
 */
bool usbh_seam_install(uc_engine* uc, const uint8_t* elf, long len)
{
  const uint32_t msc = elf_sym_addr(elf, len, "ra8_usb_hmsc_read10", nullptr);
  if (msc != 0U) {
    /* usb_host_file_ops creates a file (it links fileops_backend_write) -> the
     * virtual disk is writable; usb_host_msc_browse tests a read-only LUN. */
    s_vmsc_writable = (elf_sym_addr(elf, len, "fileops_backend_write", nullptr) != 0U);
    eth_seam_hook(uc, elf, len, "ra8_usb_hmsc_init", (void*)on_hmsc_ok);
    eth_seam_hook(uc, elf, len, "ra8_usb_hmsc_enumerate", (void*)on_hmsc_enumerate);
    eth_seam_hook(uc, elf, len, "ra8_usb_hmsc_read_capacity", (void*)on_hmsc_read_capacity);
    eth_seam_hook(uc, elf, len, "ra8_usb_hmsc_read10", (void*)on_hmsc_read10);
    eth_seam_hook(uc, elf, len, "ra8_usb_hmsc_write10", (void*)on_hmsc_write10);
    eth_seam_hook(uc, elf, len, "ra8_usb_hmsc_close", (void*)on_hmsc_ok);
    (void)fprintf(stderr,
                  "  usb-host seam : hmsc=0x%08X (virtual MSC FAT16 disk, file MRAM.BIN, %s)\n",
                  msc,
                  s_vmsc_writable ? "read-write" : "read-only");
    return true;
  }
  const uint32_t cx = elf_sym_addr(elf, len, "ra8_usb_host_control_xfer", nullptr);
  if (cx == 0U) {
    return false; /* not a USB-host-capable firmware -- nothing to seam. */
  }
  eth_seam_hook(uc, elf, len, "ra8_usb_host_line_state", (void*)on_usbh_line_state);
  eth_seam_hook(uc, elf, len, "ra8_usb_host_control_xfer", (void*)on_usbh_control_xfer);
  eth_seam_hook(uc, elf, len, "ra8_usb_host_bulk_in", (void*)on_usbh_bulk_in);
  eth_seam_hook(uc, elf, len, "ra8_usb_host_bus_reset", (void*)on_usbh_ok);
  eth_seam_hook(uc, elf, len, "ra8_usb_host_set_uact", (void*)on_usbh_ok);
  eth_seam_hook(uc, elf, len, "ra8_usb_host_set_target", (void*)on_usbh_ok);
  eth_seam_hook(uc, elf, len, "ra8_usb_host_pipe_setup", (void*)on_usbh_ok);
  (void)fprintf(stderr,
                "  usb-host seam : control=0x%08X (virtual HID boot keyboard \"RA8D2\")\n",
                cx);
  return true;
}

/** @brief Implementation of `emu_usbh_done()` -- the USBH early-stop predicate. */
bool emu_usbh_done(void)
{
  return (s_vkbd_reports_sent >= (uint32_t)k_vkbd_stop_reports) || s_vmsc_write_seen;
}
