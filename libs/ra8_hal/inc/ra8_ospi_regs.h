/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_ospi_regs.h
 * @brief xSPI / Octo-SPI controller register layout for the Renesas RA8D2
 * @ingroup grp_hal_memory
 *
 * @details
 * The RA8D2 xSPI controller drives the on-board 64 MiB Octo-SPI NOR
 * flash on the EK-RA8D2 and supports OPI / Octa-DDR / HyperBus via
 * two instances:
 *
 *  - ``XSPI0`` at ``0x40268000``
 *  - ``XSPI1`` at ``0x40268400``
 *
 * The DOTF (Decryption On The Fly) block at ``0x40268800`` sits
 * immediately after them for encrypted XiP execution and is addressed
 * separately.
 *
 * ## Layout source
 *
 * This header mirrors ``R_XSPI0_Type`` in the Renesas FSP CMSIS
 * header ``R7KA8D2KF_core0.h`` (lines 26943..27312), which itself is
 * generated from the RA8D2 Hardware User's Manual chapter 44
 * "Octal Serial Peripheral Interface (OSPI)" (p 2986..3047). Total
 * struct size is 412 bytes (0x19C) per instance. Sub-blocks
 * ``CMCFGCS[2]`` (0x10 each), ``CDBUF[4]`` (0x10 each), and
 * ``CCCTLCS[2]`` (0x20 each) are flattened here to ``uint32_t``
 * arrays so the driver can index individual 32-bit words without
 * pulling in FSP's bit-field sub-structures.
 *
 * ## Register map (per instance)
 *
 * | Offset | Name         | Width       | Purpose                                 |
 * |-------:|--------------|------------:|-----------------------------------------|
 * | 0x000  | WRAPCFG      | 32          | Wrapper configuration (CK/DS shift)      |
 * | 0x004  | COMCFG       | 32          | Channel arbitration + INT output enable  |
 * | 0x008  | BMCFGCH[2]   | 32 x 2      | Bridge map config per channel            |
 * | 0x010  | CMCFGCS[2]   | 4 x 32 x 2  | Command map configuration per target      |
 * | 0x030  | _r0          | 32 x 8      | Reserved                                 |
 * | 0x050  | LIOCFGCS[2]  | 32 x 2      | Link I/O config (protocol / latency)     |
 * | 0x058  | ABMCFG       | 32          | AXI bridge map configuration             |
 * | 0x05C  | _r1          | 32          | Reserved                                 |
 * | 0x060  | BMCTL0       | 32          | Bridge map control 0                     |
 * | 0x064  | BMCTL1       | 32          | Bridge map control 1 (write-only push)   |
 * | 0x068  | CMCTLCH[2]   | 32 x 2      | Command map control (XiP)                |
 * | 0x070  | CDCTL0       | 32          | Command manual control 0 (TRREQ / CSSEL) |
 * | 0x074  | CDCTL1       | 32          | Command manual control 1                 |
 * | 0x078  | CDCTL2       | 32          | Command manual control 2                 |
 * | 0x07C  | _r2          | 32          | Reserved                                 |
 * | 0x080  | CDBUF[16]    | 32 x 16     | Command manual buffer (4 slots x 4 words)|
 * | 0x0C0  | _r3          | 32 x 16     | Reserved                                 |
 * | 0x100  | LPCTL0       | 32          | Link pattern control 0                   |
 * | 0x104  | LPCTL1       | 32          | Link pattern control 1                   |
 * | 0x108  | LIOCTL       | 32          | Link I/O control (WP/RST drive)          |
 * | 0x10C  | _r4          | 32 x 9      | Reserved                                 |
 * | 0x130  | CCCTLCS[16]  | 32 x 16     | Calibration config per target (2 x 8)     |
 * | 0x170  | _r5          | 32 x 4      | Reserved                                 |
 * | 0x180  | VERSTT       | 32          | Version (read-only)                      |
 * | 0x184  | COMSTT       | 32          | Common status (MEMACC / PBUFNE / ECS)    |
 * | 0x188  | CASTTCS[2]   | 32 x 2      | Calibration status per target             |
 * | 0x190  | INTS         | 32          | Interrupt status (CMDCMP in bit 0)       |
 * | 0x194  | INTC         | 32          | Interrupt clear (write-1-to-clear)       |
 * | 0x198  | INTE         | 32          | Interrupt enable                         |
 *
 * ## Manual-command flow
 *
 * Driver code issues a direct flash command by filling one
 * ``CDBUF`` slot (opcode into ``CDBUF[N*4+0]``, address into
 * ``CDBUF[N*4+1]``, write data into ``CDBUF[N*4+2..3]``), then
 * setting ``CDCTL0.TRREQ`` to kick the transfer and polling
 * ``INTS.CMDCMP`` for completion. ``INTC.CMDCMPC`` clears the
 * pending status.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_attributes.h"

/**
 * @enum ra8_xspi_addr_t
 * @brief xSPI / DOTF window base addresses on the peripheral bus.
 */
typedef enum : uintptr_t {
  k_ra8_xspi0_base_addr = 0x40268000UL, /**< XSPI0 register window.         */
  k_ra8_xspi1_base_addr = 0x40268400UL, /**< XSPI1 register window.         */
  k_ra8_dotf_base_addr  = 0x40268800UL, /**< DOTF (decryption-on-the-fly).  */
  k_ra8_xspi_stride     = 0x400UL,      /**< Spacing between XSPIn windows. */
} ra8_xspi_addr_t;

/**
 * @enum ra8_xspi_limits_t
 * @brief Fixed xSPI limits (instance counts, slot counts).
 */
typedef enum : uint8_t {
  k_ra8_xspi_instance_count = 2U,  /**< XSPI0 and XSPI1.                        */
  k_ra8_xspi_cdbuf_slots    = 4U,  /**< CDBUF has 4 x 16-byte manual slots.     */
  k_ra8_xspi_cdbuf_words    = 16U, /**< Flattened CDBUF view (4 slots x 4 u32). */
  k_ra8_xspi_r4_words       = 9U,  /**< Reserved 36-byte gap at +0x10C.         */
  k_ra8_xspi_ccctlcs_words  = 16U, /**< CCCTLCS is 2 targets x 8 u32 each.      */
  k_ra8_xspi_r5_words       = 4U,  /**< Reserved 16-byte gap at +0x170.         */
} ra8_xspi_limits_t;

typedef enum : uint16_t {
  k_ra8_xspi_regs_size = 0x019CU, /**< FSP R_XSPI0_Type size (412 bytes). */
} ra8_xspi_size_t;

/**
 * @enum ra8_xspi_xfer_limits_t
 * @brief Limits for direct-command flash transfers.
 */
typedef enum : uint32_t {
  k_ra8_xspi_max_xfer   = 4096U, /**< Maximum bytes per flash read/program call. */
  k_ra8_xspi_page_len   = 256U,  /**< Page size of standard NOR flash parts.     */
  k_ra8_xspi_sector_len = 4096U, /**< Sector size for opcode 0x20 / 0x21.        */
} ra8_xspi_xfer_limits_t;

/**
 * @struct r_xspi_regs_t
 * @brief xSPI per-instance register window.
 *
 * @details
 * Byte-accurate mirror of FSP ``R_XSPI0_Type`` (412 bytes).
 * Sub-blocks that FSP models with nested structs are collapsed to
 * flat ``uint32_t`` arrays so the driver can write individual
 * 32-bit words without bringing in bit-field layouts we do not
 * touch from this codebase.
 *
 * @invariant ``sizeof(r_xspi_regs_t) == 0x19C``.
 *
 * @see k_ra8_xspi0_base_addr XSPI0 base address.
 * @see ra8_xspi()            Pointer accessor.
 * @since 0.1.0
 */
typedef struct {
  volatile uint32_t WRAPCFG;                           /**< +0x000 Wrapper config (CK/DS shift).  */
  volatile uint32_t COMCFG;                            /**< +0x004 Channel arbitration + INT en.  */
  volatile uint32_t BMCFGCH[2];                        /**< +0x008..+0x00C Bridge map config.     */
  volatile uint32_t CMCFGCS[8];                        /**< +0x010..+0x02C Command map (2x4 u32). */
  volatile uint32_t _r0[8];                            /**< +0x030 Reserved (32 bytes).           */
  volatile uint32_t LIOCFGCS[2];                       /**< +0x050..+0x054 Link I/O per target.   */
  volatile uint32_t ABMCFG;                            /**< +0x058 AXI bridge map config.         */
  volatile uint32_t _r1;                               /**< +0x05C Reserved.                      */
  volatile uint32_t BMCTL0;                            /**< +0x060 Bridge map control 0.          */
  volatile uint32_t BMCTL1;                            /**< +0x064 Bridge map control 1 (WO).     */
  volatile uint32_t CMCTLCH[2];                        /**< +0x068..+0x06C XiP command control.   */
  volatile uint32_t CDCTL0;                            /**< +0x070 Manual cmd ctl 0 (TRREQ/CSSEL) */
  volatile uint32_t CDCTL1;                            /**< +0x074 Manual cmd ctl 1.              */
  volatile uint32_t CDCTL2;                            /**< +0x078 Manual cmd ctl 2.              */
  volatile uint32_t _r2;                               /**< +0x07C Reserved.                      */
  volatile uint32_t CDBUF[16];                         /**< +0x080..+0x0BC Command buf (4x4 u32). */
  volatile uint32_t _r3[16];                           /**< +0x0C0 Reserved (64 bytes).           */
  volatile uint32_t LPCTL0;                            /**< +0x100 Link pattern control 0.        */
  volatile uint32_t LPCTL1;                            /**< +0x104 Link pattern control 1.        */
  volatile uint32_t LIOCTL;                            /**< +0x108 Link I/O control (WP/RST).     */
  volatile uint32_t _r4[k_ra8_xspi_r4_words];          /**< +0x10C Reserved.                      */
  volatile uint32_t CCCTLCS[k_ra8_xspi_ccctlcs_words]; /**< +0x130 Calib 2x8.                     */
  volatile uint32_t _r5[k_ra8_xspi_r5_words];          /**< +0x170 Reserved.                      */
  volatile uint32_t VERSTT;                            /**< +0x180 Version (RO).                  */
  volatile uint32_t COMSTT;                            /**< +0x184 Common status (MEMACC/PBUFNE). */
  volatile uint32_t CASTTCS[2];                        /**< +0x188..+0x18C Calibration status.    */
  volatile uint32_t INTS;                              /**< +0x190 Interrupt status (CMDCMP=b0).  */
  volatile uint32_t INTC;                              /**< +0x194 Interrupt clear (W1C).         */
  volatile uint32_t INTE;                              /**< +0x198 Interrupt enable.              */
} r_xspi_regs_t;

static_assert(sizeof(r_xspi_regs_t) == (uint32_t)k_ra8_xspi_regs_size,
              "r_xspi_regs_t must match FSP R_XSPI0_Type size (412 bytes)");

/**
 * @brief Get pointer to xSPI instance N (0..1).
 *
 * @param[in] instance xSPI instance index (0 or 1).
 *
 * @return Pointer to the register window, or ``nullptr`` if
 *         ``instance >= k_ra8_xspi_instance_count``.
 *
 * @note Thread-safe; returns a constant address per instance.
 * @since 0.1.0
 */
RA8_HW_REGISTER_ACCESS
static inline volatile r_xspi_regs_t* ra8_xspi(uint8_t instance)
{
  if (instance >= k_ra8_xspi_instance_count) {
    return nullptr;
  }
  return (volatile r_xspi_regs_t*)(k_ra8_xspi0_base_addr +
                                   ((uintptr_t)instance * k_ra8_xspi_stride));
}

/**
 * @enum ra8_xspi_lio_mode_t
 * @brief Protocol mode values written to ``LIOCFGCS[n].PRTMD``.
 *
 * @details
 * ``PRTMD`` is a 10-bit field per FSP ``R_XSPI0_LIOCFGCS_b.PRTMD``
 * (HUM Ch 44 "Octal Serial Peripheral Interface (OSPI)" p 2986),
 * so this enum is widened to ``uint16_t`` to fit the full 10-bit
 * range required by 8D/4S protocol encodings (e.g.
 * ``SPI_FLASH_PROTOCOL_8D_8D_8D = 0x3FF``). The convenience names
 * preserved here are the simple-driver bring-up shortcuts; raw
 * 10-bit FSP-style protocol words can also be cast in directly.
 */
typedef enum : uint16_t {
  k_ra8_xspi_lio_1s1s1s = 0x000U, /**< 1S-1S-1S extended SPI.    */
  k_ra8_xspi_lio_1s2s2s = 0x048U, /**< 1S-2S-2S dual IO.         */
  k_ra8_xspi_lio_1s4s4s = 0x090U, /**< 1S-4S-4S quad IO.         */
  k_ra8_xspi_lio_1s8s8s = 0x110U, /**< 1S-8S-8S octal IO.        */
  k_ra8_xspi_lio_8d8d8d = 0x3FFU, /**< 8D-8D-8D octal DDR (OPI). */
} ra8_xspi_lio_mode_t;

/**
 * @enum ra8_xspi_liocfgcs_pos_t
 * @brief Bit positions in ``LIOCFGCS[n]`` (link I/O config per target).
 *
 * @details
 * Mirrors FSP ``R_XSPI0_LIOCFGCS_b`` field positions so the driver
 * encodes protocol + latency bits at the correct offsets when
 * programming a link-IO mode.
 */
typedef enum : uint8_t {
  k_ra8_xspi_liocfgcs_pos_prtmd  = 0U,  /**< PRTMD[9..0] protocol mode. */
  k_ra8_xspi_liocfgcs_pos_latemd = 10U, /**< LATEMD[10] latency mode.   */
  k_ra8_xspi_liocfgcs_pos_ddren  = 11U, /**< DDREN[11]   DDR enable.    */
} ra8_xspi_liocfgcs_pos_t;

/**
 * @enum ra8_xspi_liocfgcs_mask_t
 * @brief Pre-shifted bit masks for ``LIOCFGCS[n]`` enable bits.
 */
typedef enum : uint32_t {
  k_ra8_xspi_liocfgcs_mask_ddren = 1UL << 11U, /**< DDR / DTR enable bit. */
} ra8_xspi_liocfgcs_mask_t;

/**
 * @enum ra8_xspi_cmcfgcs_layout_t
 * @brief Word indices inside one ``CMCFGCS[n]`` slot (4 u32 each).
 *
 * @details
 * CMCFGCS is a 2-target x 4-word block in ``r_xspi_regs_t::CMCFGCS``.
 * Slot ``n`` lives at ``CMCFGCS[n * 4 + word]``. The layout mirrors
 * FSP ``R_XSPI0_CMCFGCS_b`` (HUM Ch 44 p 2986): word 0 holds the
 * mapping mode, word 1 the read command + dummy cycles, word 2 the
 * write command, word 3 the address-byte width.
 */
typedef enum : uint8_t {
  k_ra8_xspi_cmcfgcs_word_mode      = 0U, /**< +0x00 mapping mode word.       */
  k_ra8_xspi_cmcfgcs_word_read_cmd  = 1U, /**< +0x04 read-cmd + dummy cycles. */
  k_ra8_xspi_cmcfgcs_word_write_cmd = 2U, /**< +0x08 write-cmd + dummy.       */
  k_ra8_xspi_cmcfgcs_word_addr      = 3U, /**< +0x0C address-byte width.      */
  k_ra8_xspi_cmcfgcs_words_per_slot = 4U, /**< Each slot is 4 u32 wide.       */
} ra8_xspi_cmcfgcs_layout_t;

/**
 * @enum ra8_xspi_cmcfgcs_pos_t
 * @brief Bit positions inside the ``CMCFGCS`` read-cmd word.
 */
typedef enum : uint8_t {
  k_ra8_xspi_cmcfgcs_pos_cmd       = 16U, /**< CMD[31..16]   read opcode. */
  k_ra8_xspi_cmcfgcs_pos_addr_size = 0U,  /**< ADDSIZE[2..0] addr bytes.  */
} ra8_xspi_cmcfgcs_pos_t;

/**
 * @enum ra8_xspi_wrapcfg_pos_t
 * @brief Bit positions inside ``WRAPCFG`` (DQS shift per target).
 *
 * @details
 * ``DSSFTCS0[12..8]`` and ``DSSFTCS1[28..24]`` are 5-bit calibration
 * codes (HUM Ch 44 p 2986). FSP's ``R_OSPI_B_AutoCalibrate`` sweeps
 * these to find the optimum sample window; this driver exposes the
 * shift slots directly so callers can write a known-good offset.
 */
typedef enum : uint8_t {
  k_ra8_xspi_wrapcfg_pos_dssft_cs0 = 8U,  /**< DSSFTCS0[12..8].  */
  k_ra8_xspi_wrapcfg_pos_dssft_cs1 = 24U, /**< DSSFTCS1[28..24]. */
} ra8_xspi_wrapcfg_pos_t;

typedef enum : uint32_t {
  k_ra8_xspi_wrapcfg_dssft_mask = 0x1FUL, /**< 5-bit DQS shift code mask. */
} ra8_xspi_wrapcfg_mask_t;

/**
 * @enum ra8_xspi_calib_t
 * @brief Reserved-bit positions in ``CCCTLCS[n].CCCTL0`` (calibration).
 *
 * @details
 * The CCCTLCS bank holds the auto-calibration descriptor per target.
 * For project use we only need the enable bit; ``CCCTL0_b.CAEN`` lives
 * at bit 0 (HUM Ch 44 p 2986). The full 8-word descriptor is filled
 * in by ``ra8_xspi_calibrate_dqs()`` from FSP-style defaults.
 */
typedef enum : uint8_t {
  k_ra8_xspi_ccctl0_pos_caen      = 0U, /**< CAEN[0] auto-calibration enable. */
  k_ra8_xspi_ccctlcs_words_per_cs = 8U, /**< 8 u32 per chip-select bank.      */
} ra8_xspi_calib_t;

typedef enum : uint32_t {
  k_ra8_xspi_ccctl0_mask_caen = 1UL << 0U, /**< CAEN enable mask. */
} ra8_xspi_calib_mask_t;

/**
 * @enum ra8_xspi_cdbuf_slot_word_t
 * @brief Per-slot word indices inside one ``CDBUF`` slot (4 u32 each).
 *
 * @details
 * Each manual-command slot in ``CDBUF`` is 16 bytes laid out as four
 * 32-bit words: CDT (type / opcode), CDA (address), CDD0 (data0..3),
 * CDD1 (data4..7). FSP names these ``CDBUF[n].CDT/CDA/CDD0/CDD1``.
 * The flat ``CDBUF[16]`` array in ``r_xspi_regs_t`` indexes these as
 * ``slot * 4 + word``.
 */
typedef enum : uint8_t {
  k_ra8_xspi_cdbuf_word_cdt       = 0U, /**< +0x00 CDT  -- cmd/size/type word. */
  k_ra8_xspi_cdbuf_word_cda       = 1U, /**< +0x04 CDA  -- address word.       */
  k_ra8_xspi_cdbuf_word_cdd0      = 2U, /**< +0x08 CDD0 -- data bytes 0..3.    */
  k_ra8_xspi_cdbuf_word_cdd1      = 3U, /**< +0x0C CDD1 -- data bytes 4..7.    */
  k_ra8_xspi_cdbuf_words_per_slot = 4U, /**< Each CDBUF slot is 4 u32 wide.    */
} ra8_xspi_cdbuf_slot_word_t;

/**
 * @enum ra8_xspi_cdt_pos_t
 * @brief Bit positions inside the ``CDBUF[n].CDT`` (Command Type) word.
 *
 * @details
 * Mirrors FSP ``R_XSPI0_CDBUF_CDT_*`` positions per HUM Ch 44 p 2986.
 * The Command Manual engine reads CMDSIZE/ADDSIZE/DATASIZE/LATE/TRTYPE
 * out of CDT to drive the SPI bus phases; the actual JEDEC opcode
 * goes in the high half (``CMD`` at bits [31..16]).
 */
typedef enum : uint8_t {
  k_ra8_xspi_cdt_pos_cmdsize  = 0U,  /**< CMDSIZE[1..0]  command bytes.  */
  k_ra8_xspi_cdt_pos_addsize  = 2U,  /**< ADDSIZE[4..2]  address bytes.  */
  k_ra8_xspi_cdt_pos_datasize = 5U,  /**< DATASIZE[8..5] data bytes.     */
  k_ra8_xspi_cdt_pos_late     = 9U,  /**< LATE[13..9]    latency cycles. */
  k_ra8_xspi_cdt_pos_trtype   = 15U, /**< TRTYPE[15]     0=read 1=write. */
  k_ra8_xspi_cdt_pos_cmd      = 16U, /**< CMD[31..16]    JEDEC opcode.   */
} ra8_xspi_cdt_pos_t;

/**
 * @enum ra8_xspi_cdt_addsize_t
 * @brief Encoded address-byte counts for ``CDT.ADDSIZE``.
 */
typedef enum : uint8_t {
  k_ra8_xspi_cdt_addsize_0 = 0U, /**< No address phase (e.g. WREN, RDID). */
  k_ra8_xspi_cdt_addsize_3 = 3U, /**< 3-byte address (24-bit JEDEC).      */
  k_ra8_xspi_cdt_addsize_4 = 4U, /**< 4-byte address (32-bit JEDEC).      */
} ra8_xspi_cdt_addsize_t;

/**
 * @enum ra8_xspi_cdt_field_mask_t
 * @brief Width-limited masks for the sub-fields packed into ``CDT``.
 *
 * @details
 * Each mask covers exactly the bit-count of its target field per the
 * FSP CMSIS bit-field layout (HUM Ch 44 p 2986): CMDSIZE is 2 bits,
 * ADDSIZE is 3 bits, DATASIZE is 4 bits, TRTYPE is 1 bit. The masks
 * exist so the CDT-encoder can clamp caller-provided values without
 * relying on raw hexadecimal magic numbers.
 */
typedef enum : uint32_t {
  k_ra8_xspi_cdt_mask_cmdsize  = 0x3UL,    /**< CMDSIZE field is 2 bits.       */
  k_ra8_xspi_cdt_mask_addsize  = 0x7UL,    /**< ADDSIZE field is 3 bits.       */
  k_ra8_xspi_cdt_mask_datasize = 0xFUL,    /**< DATASIZE field is 4 bits.      */
  k_ra8_xspi_cdt_mask_trtype   = 0x1UL,    /**< TRTYPE field is 1 bit.         */
  k_ra8_xspi_cdt_mask_cmd      = 0xFFFFUL, /**< CMD[31..16] is a 16-bit field. */
} ra8_xspi_cdt_field_mask_t;

/**
 * @enum ra8_xspi_cdt_cmdsize_t
 * @brief Encoded command-byte counts for ``CDT.CMDSIZE``.
 */
typedef enum : uint8_t {
  k_ra8_xspi_cdt_cmdsize_0 = 0U, /**< No command byte (rare).        */
  k_ra8_xspi_cdt_cmdsize_1 = 1U, /**< 1-byte command (standard SPI). */
  k_ra8_xspi_cdt_cmdsize_2 = 2U, /**< 2-byte command (DDR / OPI).    */
} ra8_xspi_cdt_cmdsize_t;

/**
 * @enum ra8_xspi_cdt_trtype_t
 * @brief ``CDT.TRTYPE`` direction selector.
 */
typedef enum : uint8_t {
  k_ra8_xspi_cdt_trtype_read  = 0U, /**< Read transaction (host samples). */
  k_ra8_xspi_cdt_trtype_write = 1U, /**< Write transaction (host drives). */
} ra8_xspi_cdt_trtype_t;

/**
 * @enum ra8_xspi_bmctl0_t
 * @brief Pre-shifted masks for ``BMCTL0`` (system-bus access enable).
 *
 * @details
 * ``BMCTL0`` enables AHB read/write traffic from each system-bus
 * channel to each target. Two bits per (channel, target) pair: 01 =
 * read-only, 10 = write-only, 11 = read/write. FSP names the
 * conventional patterns ``READ_ONLY=0x55`` / ``READ_WRITE=0xFF``.
 */
typedef enum : uint32_t {
  k_ra8_xspi_bmctl0_disabled   = 0x00UL, /**< All AHB access disabled. */
  k_ra8_xspi_bmctl0_read_only  = 0x55UL, /**< 01 per pair: read-only.  */
  k_ra8_xspi_bmctl0_write_only = 0xAAUL, /**< 10 per pair: write-only. */
  k_ra8_xspi_bmctl0_read_write = 0xFFUL, /**< 11 per pair: read/write. */
} ra8_xspi_bmctl0_t;

/**
 * @enum ra8_xspi_bmctl1_t
 * @brief Pre-shifted control masks for ``BMCTL1`` (write-only).
 */
typedef enum : uint32_t {
  k_ra8_xspi_bmctl1_pbufclr_mask = 0x03UL << 10U, /**< Clear prefetch ch0+ch1. */
  k_ra8_xspi_bmctl1_mwrpush_mask = 0x03UL << 8U,  /**< Push combine ch0+ch1.   */
} ra8_xspi_bmctl1_t;

/**
 * @enum ra8_xspi_cmctlch_t
 * @brief Pre-shifted bit masks for ``CMCTLCH[n]`` (XiP control).
 */
typedef enum : uint32_t {
  k_ra8_xspi_cmctlch_xipen_pos     = 16U,        /**< XIPEN bit position.    */
  k_ra8_xspi_cmctlch_xipen_mask    = 1UL << 16U, /**< XIPEN[16] enable.      */
  k_ra8_xspi_cmctlch_xipencode_pos = 0U,         /**< XIPENCODE[7..0] enter. */
  k_ra8_xspi_cmctlch_xipexcode_pos = 8U,         /**< XIPEXCODE[15..8] exit. */
} ra8_xspi_cmctlch_t;

/**
 * @enum ra8_xspi_cdctl0_bit_t
 * @brief Bit positions in ``CDCTL0`` (command manual control 0).
 *
 * @details
 * Writing ``TRREQ`` to 1 kicks a manual-command transfer out of
 * ``CDBUF``. The controller self-clears the bit once the transfer
 * finishes. ``CSSEL`` selects which chip-select (target 0 / target 1)
 * the command targets.
 */
typedef enum : uint8_t {
  k_ra8_xspi_cdctl0_bit_trreq = 0U, /**< Transaction request.          */
  k_ra8_xspi_cdctl0_bit_cssel = 3U, /**< Chip-select select (0 = CS0). */
} ra8_xspi_cdctl0_bit_t;

/**
 * @enum ra8_xspi_cdctl0_mask_t
 * @brief Pre-shifted masks for ``CDCTL0`` control bits.
 */
typedef enum : uint32_t {
  k_ra8_xspi_cdctl0_mask_trreq = 1UL << 0U, /**< TRREQ (kick off xfer). */
  k_ra8_xspi_cdctl0_mask_cssel = 1UL << 3U, /**< CSSEL (target select). */
} ra8_xspi_cdctl0_mask_t;

/**
 * @enum ra8_xspi_lioctl_mask_t
 * @brief Pre-shifted masks for ``LIOCTL`` (link I/O control: WP / RST drive).
 *
 * @details
 * ``LIOCTL`` drives the per-instance write-protect and reset output pins
 * that fan out to the selected chip-select's flash. Mirrors FSP CMSIS
 * ``R_XSPI0_LIOCTL_*`` (HUM Ch 44 p 2986): ``WPCS`` at bit 0 drives the
 * active-low write-protect strap, ``RSTCS`` at bit 16 drives the
 * active-low hardware-reset strap. Both reset to 1 (deasserted). The
 * vendor EK-RA8D2 bring-up (``ospi_b_ep.c`` ``ospi_b_init`` and
 * ``ospi_flash_issi_is25lx512.c`` ``reset_ospi_device``) pulses
 * ``RSTCS`` low-then-high AFTER the controller protocol/timing config to
 * return the IS25LX512M to a known SPI protocol state before the first
 * JEDEC command; without it a device left in OPI/DOPI by a prior loader
 * never answers a 1S RDID and the bus floats to 0x00FFFFFF.
 */
typedef enum : uint32_t {
  k_ra8_xspi_lioctl_mask_wpcs  = 1UL << 0U,  /**< WPCS  -- write-protect drive.  */
  k_ra8_xspi_lioctl_mask_rstcs = 1UL << 16U, /**< RSTCS -- hardware-reset drive. */
} ra8_xspi_lioctl_mask_t;

/**
 * @enum ra8_xspi_ints_bit_t
 * @brief Bit positions in the ``INTS`` interrupt-status register.
 */
typedef enum : uint8_t {
  k_ra8_xspi_ints_bit_cmdcmp = 0U, /**< Command-complete flag.          */
  k_ra8_xspi_ints_bit_patcmp = 1U, /**< Pattern-complete flag.          */
  k_ra8_xspi_ints_bit_inicmp = 2U, /**< Initial-sequence-complete flag. */
} ra8_xspi_ints_bit_t;

/**
 * @enum ra8_xspi_ints_mask_t
 * @brief Pre-shifted masks for the ``INTS`` and ``INTC`` registers.
 *
 * @details
 * The same bit positions drive ``INTS`` (read flag), ``INTC`` (W1C
 * clear) and ``INTE`` (enable) registers, so one mask enum covers
 * all three.
 */
typedef enum : uint32_t {
  k_ra8_xspi_ints_mask_cmdcmp = 1UL << 0U,    /**< CMDCMP -- command complete. */
  k_ra8_xspi_ints_mask_patcmp = 1UL << 1U,    /**< PATCMP -- pattern complete. */
  k_ra8_xspi_ints_mask_inicmp = 1UL << 2U,    /**< INICMP -- initial seq done. */
  k_ra8_xspi_ints_mask_all    = 0xFFFFFFFFUL, /**< Clear every pending flag.   */
} ra8_xspi_ints_mask_t;

/**
 * @enum ra8_xspi_inte_mask_t
 * @brief Pre-shifted masks for the ``INTE`` interrupt-enable register.
 */
typedef enum : uint32_t {
  k_ra8_xspi_inte_mask_cmdcmp = 1UL << 0U, /**< CMDCMPE -- enable CMDCMP IRQ. */
} ra8_xspi_inte_mask_t;

/**
 * @enum ra8_xspi_comstt_bit_t
 * @brief Bit positions in the ``COMSTT`` (common status) register.
 *
 * @details
 * Unlike the earlier scaffold this register no longer reports
 * TRBUSY / done -- instead it reports per-channel memory access
 * activity, prefetch buffer state and per-target ECS/INT/RSTO
 * monitors. Command completion now lives on ``INTS.CMDCMP``.
 */
typedef enum : uint8_t {
  k_ra8_xspi_comstt_bit_memacc_ch0 = 0U,  /**< Mem access ongoing for ch0. */
  k_ra8_xspi_comstt_bit_memacc_ch1 = 1U,  /**< Mem access ongoing for ch1. */
  k_ra8_xspi_comstt_bit_pbufne_ch0 = 4U,  /**< Prefetch buf not-empty ch0. */
  k_ra8_xspi_comstt_bit_pbufne_ch1 = 5U,  /**< Prefetch buf not-empty ch1. */
  k_ra8_xspi_comstt_bit_ecs_cs0    = 16U, /**< ECS monitor for target 0.   */
  k_ra8_xspi_comstt_bit_int_cs0    = 17U, /**< INT monitor for target 0.   */
  k_ra8_xspi_comstt_bit_ecs_cs1    = 20U, /**< ECS monitor for target 1.   */
  k_ra8_xspi_comstt_bit_int_cs1    = 21U, /**< INT monitor for target 1.   */
} ra8_xspi_comstt_bit_t;

#ifdef __cplusplus
}
#endif
