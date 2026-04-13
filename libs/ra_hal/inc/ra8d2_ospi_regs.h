/**
 * @file ra8d2_ospi_regs.h
 * @brief xSPI / Octo-SPI controller register layout for the Renesas RA8D2
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
 * | 0x010  | CMCFGCS[2]   | 4 x 32 x 2  | Command map configuration per slave      |
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
 * | 0x130  | CCCTLCS[16]  | 32 x 16     | Calibration config per slave (2 x 8)     |
 * | 0x170  | _r5          | 32 x 4      | Reserved                                 |
 * | 0x180  | VERSTT       | 32          | Version (read-only)                      |
 * | 0x184  | COMSTT       | 32          | Common status (MEMACC / PBUFNE / ECS)    |
 * | 0x188  | CASTTCS[2]   | 32 x 2      | Calibration status per slave             |
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
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
 * @enum ra_xspi_addr_t
 * @brief xSPI / DOTF window base addresses on the peripheral bus.
 */
typedef enum : uintptr_t {
  k_ra_xspi0_base_addr = 0x40268000UL, /**< XSPI0 register window.             */
  k_ra_xspi1_base_addr = 0x40268400UL, /**< XSPI1 register window.             */
  k_ra_dotf_base_addr  = 0x40268800UL, /**< DOTF (decryption-on-the-fly).      */
  k_ra_xspi_stride     = 0x400UL,      /**< Spacing between XSPIn windows.     */
} ra_xspi_addr_t;

/**
 * @enum ra_xspi_limits_t
 * @brief Fixed xSPI limits (instance counts, slot counts).
 */
typedef enum : uint8_t {
  k_ra_xspi_instance_count = 2U,  /**< XSPI0 and XSPI1.                        */
  k_ra_xspi_cdbuf_slots    = 4U,  /**< CDBUF has 4 x 16-byte manual slots.     */
  k_ra_xspi_cdbuf_words    = 16U, /**< Flattened CDBUF view (4 slots x 4 u32).*/
  k_ra_xspi_r4_words       = 9U,  /**< Reserved 36-byte gap at +0x10C.         */
  k_ra_xspi_ccctlcs_words  = 16U, /**< CCCTLCS is 2 slaves x 8 u32 each.       */
  k_ra_xspi_r5_words       = 4U,  /**< Reserved 16-byte gap at +0x170.          */
} ra_xspi_limits_t;

typedef enum : uint16_t {
  k_ra_xspi_regs_size = 0x019CU, /**< FSP R_XSPI0_Type size (412 bytes). */
} ra_xspi_size_t;

/**
 * @enum ra_xspi_xfer_limits_t
 * @brief Limits for direct-command flash transfers.
 */
typedef enum : uint32_t {
  k_ra_xspi_max_xfer   = 4096U, /**< Maximum bytes per flash read/program call.  */
  k_ra_xspi_page_len   = 256U,  /**< Page size of standard NOR flash parts.      */
  k_ra_xspi_sector_len = 4096U, /**< Sector size for opcode 0x20 / 0x21.         */
} ra_xspi_xfer_limits_t;

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
 * @see k_ra_xspi0_base_addr XSPI0 base address.
 * @see ra_xspi()            Pointer accessor.
 * @since 0.2.0
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  volatile uint32_t WRAPCFG;                          /**< +0x000 Wrapper config (CK/DS shift).   */
  volatile uint32_t COMCFG;                           /**< +0x004 Channel arbitration + INT en.   */
  volatile uint32_t BMCFGCH[2];                       /**< +0x008..+0x00C Bridge map config.      */
  volatile uint32_t CMCFGCS[8];                       /**< +0x010..+0x02C Command map (2 x 4 u32).*/
  volatile uint32_t _r0[8];                           /**< +0x030 Reserved (32 bytes).            */
  volatile uint32_t LIOCFGCS[2];                      /**< +0x050..+0x054 Link I/O per slave.     */
  volatile uint32_t ABMCFG;                           /**< +0x058 AXI bridge map config.          */
  volatile uint32_t _r1;                              /**< +0x05C Reserved.                       */
  volatile uint32_t BMCTL0;                           /**< +0x060 Bridge map control 0.           */
  volatile uint32_t BMCTL1;                           /**< +0x064 Bridge map control 1 (WO).      */
  volatile uint32_t CMCTLCH[2];                       /**< +0x068..+0x06C XiP command control.    */
  volatile uint32_t CDCTL0;                           /**< +0x070 Manual cmd ctl 0 (TRREQ/CSSEL). */
  volatile uint32_t CDCTL1;                           /**< +0x074 Manual cmd ctl 1.               */
  volatile uint32_t CDCTL2;                           /**< +0x078 Manual cmd ctl 2.               */
  volatile uint32_t _r2;                              /**< +0x07C Reserved.                       */
  volatile uint32_t CDBUF[16];                        /**< +0x080..+0x0BC Command buf (4 x 4 u32).*/
  volatile uint32_t _r3[16];                          /**< +0x0C0 Reserved (64 bytes).            */
  volatile uint32_t LPCTL0;                           /**< +0x100 Link pattern control 0.         */
  volatile uint32_t LPCTL1;                           /**< +0x104 Link pattern control 1.         */
  volatile uint32_t LIOCTL;                           /**< +0x108 Link I/O control (WP/RST).      */
  volatile uint32_t _r4[k_ra_xspi_r4_words];          /**< +0x10C Reserved. */
  volatile uint32_t CCCTLCS[k_ra_xspi_ccctlcs_words]; /**< +0x130 Calib 2x8.*/
  volatile uint32_t _r5[k_ra_xspi_r5_words];          /**< +0x170 Reserved. */
  volatile uint32_t VERSTT;                           /**< +0x180 Version (RO).                   */
  volatile uint32_t COMSTT;                           /**< +0x184 Common status (MEMACC/PBUFNE).  */
  volatile uint32_t CASTTCS[2];                       /**< +0x188..+0x18C Calibration status.     */
  volatile uint32_t INTS;                             /**< +0x190 Interrupt status (CMDCMP=b0).   */
  volatile uint32_t INTC;                             /**< +0x194 Interrupt clear (W1C).          */
  volatile uint32_t INTE;                             /**< +0x198 Interrupt enable.               */
} r_xspi_regs_t;
/* cppcheck-suppress-end [unusedStructMember] */

static_assert(sizeof(r_xspi_regs_t) == (uint32_t)k_ra_xspi_regs_size,
              "r_xspi_regs_t must match FSP R_XSPI0_Type size (412 bytes)");

/**
 * @brief Get pointer to xSPI instance N (0..1).
 *
 * @param[in] instance xSPI instance index (0 or 1).
 *
 * @return Pointer to the register window, or ``nullptr`` if
 *         ``instance >= k_ra_xspi_instance_count``.
 *
 * @note Thread-safe; returns a constant address per instance.
 * @since 0.1.0
 */
static inline volatile r_xspi_regs_t* ra_xspi(uint8_t instance)
{
  if (instance >= (uint8_t)k_ra_xspi_instance_count) {
    return nullptr;
  }
  return (volatile r_xspi_regs_t*)(k_ra_xspi0_base_addr +
                                   ((uintptr_t)instance * (uintptr_t)k_ra_xspi_stride));
}

/**
 * @enum ra_xspi_lio_mode_t
 * @brief Protocol mode values written to ``LIOCFGCS[n].PRTMD``.
 *
 * @details
 * The RA8D2 xSPI supports 1S-1S-1S SPI, 1S-4S-4S quad, 1S-8S-8S
 * octal, and 8D-8D-8D DDR octal modes. These enum values match the
 * FSP ``spi_flash_protocol_t`` encoding so downstream bring-up can
 * share constants.
 */
typedef enum : uint8_t {
  k_ra_xspi_lio_1s1s1s = 0U, /**< Single-bit command/address/data (SPI). */
  k_ra_xspi_lio_1s2s2s = 1U, /**< Dual IO.                                */
  k_ra_xspi_lio_1s4s4s = 2U, /**< Quad IO.                                */
  k_ra_xspi_lio_1s8s8s = 3U, /**< Octal IO.                               */
  k_ra_xspi_lio_8d8d8d = 4U, /**< Octal DDR (OPI).                        */
} ra_xspi_lio_mode_t;

/**
 * @enum ra_xspi_cdctl0_bit_t
 * @brief Bit positions in ``CDCTL0`` (command manual control 0).
 *
 * @details
 * Writing ``TRREQ`` to 1 kicks a manual-command transfer out of
 * ``CDBUF``. The controller self-clears the bit once the transfer
 * finishes. ``CSSEL`` selects which chip-select (slave 0 / slave 1)
 * the command targets.
 */
typedef enum : uint8_t {
  k_ra_xspi_cdctl0_bit_trreq = 0U, /**< Transaction request.              */
  k_ra_xspi_cdctl0_bit_cssel = 3U, /**< Chip-select select (0 = CS0).     */
} ra_xspi_cdctl0_bit_t;

/**
 * @enum ra_xspi_cdctl0_mask_t
 * @brief Pre-shifted masks for ``CDCTL0`` control bits.
 */
typedef enum : uint32_t {
  k_ra_xspi_cdctl0_mask_trreq = 1UL << 0U, /**< TRREQ (kick off xfer).       */
  k_ra_xspi_cdctl0_mask_cssel = 1UL << 3U, /**< CSSEL (target slave).        */
} ra_xspi_cdctl0_mask_t;

/**
 * @enum ra_xspi_ints_bit_t
 * @brief Bit positions in the ``INTS`` interrupt-status register.
 */
typedef enum : uint8_t {
  k_ra_xspi_ints_bit_cmdcmp = 0U, /**< Command-complete flag.               */
  k_ra_xspi_ints_bit_patcmp = 1U, /**< Pattern-complete flag.               */
  k_ra_xspi_ints_bit_inicmp = 2U, /**< Initial-sequence-complete flag.      */
} ra_xspi_ints_bit_t;

/**
 * @enum ra_xspi_ints_mask_t
 * @brief Pre-shifted masks for the ``INTS`` and ``INTC`` registers.
 *
 * @details
 * The same bit positions drive ``INTS`` (read flag), ``INTC`` (W1C
 * clear) and ``INTE`` (enable) registers, so one mask enum covers
 * all three.
 */
typedef enum : uint32_t {
  k_ra_xspi_ints_mask_cmdcmp = 1UL << 0U,    /**< CMDCMP -- command complete.   */
  k_ra_xspi_ints_mask_patcmp = 1UL << 1U,    /**< PATCMP -- pattern complete.   */
  k_ra_xspi_ints_mask_inicmp = 1UL << 2U,    /**< INICMP -- initial seq done.   */
  k_ra_xspi_ints_mask_all    = 0xFFFFFFFFUL, /**< Clear every pending flag.  */
} ra_xspi_ints_mask_t;

/**
 * @enum ra_xspi_inte_mask_t
 * @brief Pre-shifted masks for the ``INTE`` interrupt-enable register.
 */
typedef enum : uint32_t {
  k_ra_xspi_inte_mask_cmdcmp = 1UL << 0U, /**< CMDCMPE -- enable CMDCMP IRQ. */
} ra_xspi_inte_mask_t;

/**
 * @enum ra_xspi_comstt_bit_t
 * @brief Bit positions in the ``COMSTT`` (common status) register.
 *
 * @details
 * Unlike the earlier scaffold this register no longer reports
 * TRBUSY / done -- instead it reports per-channel memory access
 * activity, prefetch buffer state and per-slave ECS/INT/RSTO
 * monitors. Command completion now lives on ``INTS.CMDCMP``.
 */
typedef enum : uint8_t {
  k_ra_xspi_comstt_bit_memacc_ch0 = 0U,  /**< Mem access ongoing for ch0.  */
  k_ra_xspi_comstt_bit_memacc_ch1 = 1U,  /**< Mem access ongoing for ch1.  */
  k_ra_xspi_comstt_bit_pbufne_ch0 = 4U,  /**< Prefetch buf not-empty ch0.  */
  k_ra_xspi_comstt_bit_pbufne_ch1 = 5U,  /**< Prefetch buf not-empty ch1.  */
  k_ra_xspi_comstt_bit_ecs_cs0    = 16U, /**< ECS monitor for slave 0.     */
  k_ra_xspi_comstt_bit_int_cs0    = 17U, /**< INT monitor for slave 0.     */
  k_ra_xspi_comstt_bit_ecs_cs1    = 20U, /**< ECS monitor for slave 1.     */
  k_ra_xspi_comstt_bit_int_cs1    = 21U, /**< INT monitor for slave 1.     */
} ra_xspi_comstt_bit_t;

#ifdef __cplusplus
}
#endif
