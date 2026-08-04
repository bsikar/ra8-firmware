/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_spi_regs.h
 * @brief SPI_B register layout for the Renesas RA8D2
 * @ingroup grp_hal_comms
 *
 * @details
 * The RA8D2 has two channels of the SPI_B (Type-B SPI) peripheral.
 * HUM Ch 43 "Serial Peripheral Interface (SPI)" (p 2877-2985) opens
 * with the explicit note that "this is the SPI_B version of the
 * SPI peripheral module" and that SPI_B is referred to as SPI in
 * the chapter. The earlier (legacy) SPI block in ``ra8_spi_regs.h``
 * before this commit was modeled after the older 8/16-bit register
 * stack (SPCR uint8_t, SPCMD[8] uint16_t, SPPCR/SPCKD/SSLND/SPND
 * single-byte registers). That layout does not exist on RA8D2 --
 * SPI_B uses a fully 32-bit register file with FIFO and three
 * SPCR registers.
 *
 * This header is re-derived against FSP ``R_SPI_B0_Type``
 * (R7KA8D2KF_core0.h lines 25545-25946; total size 0x70 = 112 bytes
 * per channel) at base 0x4035C000 (SPI0) / 0x4035C100 (SPI1).
 *
 *  | Offset | Name    | Width | HUM | Purpose                       |
 *  |-------:|---------|------:|----:|-------------------------------|
 *  | 0x00   | SPDR    | 32    | 2881 | Data (FIFO front-end).       |
 *  | 0x04   | SPDECR  | 32    | 2883 | Delay control (SCKDL/...).   |
 *  | 0x08   | SPCR    | 32    | 2884 | Control 1 (SPE/MSTR/SPTIE/...) |
 *  | 0x0C   | SPCR2   | 32    | 2889 | Control 2 (loopback, MOIFE). |
 *  | 0x10   | SPCR3   | 32    | 2891 | Control 3 (SPBR, SSLnP).     |
 *  | 0x14   | SPCMD0  | 32    | 2893 | Command 0 (CPHA/CPOL/SPB).   |
 *  | 0x18   | SPCMD1  | 32    | 2893 | Command 1.                    |
 *  | 0x1C   | SPCMD2  | 32    | 2893 | Command 2.                    |
 *  | 0x20   | SPCMD3  | 32    | 2893 | Command 3.                    |
 *  | 0x24   | SPCMD4  | 32    | 2893 | Command 4.                    |
 *  | 0x28   | SPCMD5  | 32    | 2893 | Command 5.                    |
 *  | 0x2C   | SPCMD6  | 32    | 2893 | Command 6.                    |
 *  | 0x30   | SPCMD7  | 32    | 2893 | Command 7.                    |
 *  | 0x40   | SPDCR   | 32    | 2896 | Data control (BYSW/SPFC).    |
 *  | 0x44   | SPDCR2  | 32    | 2897 | Data control 2 (RTRG/TTRG).  |
 *  | 0x50   | SPSR    | 32    | 2898 | Status.                       |
 *  | 0x58   | SPTFSR  | 32    | 2904 | TX FIFO data stage.          |
 *  | 0x5C   | SPRFSR  | 32    | 2904 | RX FIFO data stage.          |
 *  | 0x60   | SPPSR   | 32    | 2905 | Polling status.               |
 *  | 0x68   | SPSRC   | 32    | 2905 | Status clear (write-1).      |
 *  | 0x6C   | SPFCR   | 32    | 2906 | FIFO clear (SPFRST).         |
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_attributes.h"

/**
 * @enum ra8_spi_addr_t
 * @brief SPI_B register-window base addresses (FSP R_SPI_B0_BASE / R_SPI_B1_BASE).
 */
typedef enum : uintptr_t {
  k_ra8_spi0_base_addr = 0x4035C000UL, /**< HUM Ch 43, SPI0 base. */
  k_ra8_spi1_base_addr = 0x4035C100UL, /**< HUM Ch 43, SPI1 base. */
} ra8_spi_addr_t;

/**
 * @enum ra8_spi_limits_t
 * @brief SPI_B static dimensioning constants.
 */
typedef enum : uint16_t {
  k_ra8_spi_primary_count  = 2U,     /**< Number of SPI_B channels.      */
  k_ra8_spi_channel_stride = 0x100U, /**< 256-byte channel stride.       */
  k_ra8_spi_cmd_count      = 8U,     /**< SPCMD0..SPCMD7 sequence slots. */
  k_ra8_spi_block_size     = 0x70U,  /**< 112-byte register footprint.   */
} ra8_spi_limits_t;

/**
 * @struct r_spi_regs_t
 * @brief SPI_B register-window mirror (matches FSP R_SPI_B0_Type, 0x70 bytes).
 *
 * @details
 * Every register is 32-bit. The layout is taken verbatim from
 * R7KA8D2KF_core0.h, with the FSP nested anonymous unions / bit
 * structs flattened to plain ``uint32_t`` words. Bit-level access
 * goes through the masks/positions enums in this header instead of
 * named bit-fields so layout is debugger-stable.
 */
typedef struct {
  volatile uint32_t SPDR;          /**< +0x00 SPI Data Register.                 */
  volatile uint32_t SPDECR;        /**< +0x04 SPI Delay Control Register.        */
  volatile uint32_t SPCR;          /**< +0x08 SPI Control Register 1.            */
  volatile uint32_t SPCR2;         /**< +0x0C SPI Control Register 2.            */
  volatile uint32_t SPCR3;         /**< +0x10 SPI Control Register 3.            */
  volatile uint32_t SPCMD[8];      /**< +0x14..0x30 Command Registers SPCMD0..7. */
  volatile uint32_t _reserved0[3]; /**< +0x34..0x3C reserved.                    */
  volatile uint32_t SPDCR;         /**< +0x40 SPI Data Control Register.         */
  volatile uint32_t SPDCR2;        /**< +0x44 SPI Data Control Register 2.       */
  volatile uint32_t _reserved1[2]; /**< +0x48..0x4C reserved.                    */
  volatile uint32_t SPSR;          /**< +0x50 SPI Status Register.               */
  volatile uint32_t _reserved2;    /**< +0x54 reserved.                          */
  volatile uint32_t SPTFSR;        /**< +0x58 TX FIFO Status Register.           */
  volatile uint32_t SPRFSR;        /**< +0x5C RX FIFO Status Register.           */
  volatile uint32_t SPPSR;         /**< +0x60 SPI Polling Register.              */
  volatile uint32_t _reserved3;    /**< +0x64 reserved.                          */
  volatile uint32_t SPSRC;         /**< +0x68 SPI Status Clear Register.         */
  volatile uint32_t SPFCR;         /**< +0x6C SPI FIFO Clear Register.           */
} r_spi_regs_t;

/**
 * @enum ra8_spcr_bit_t
 * @brief SPCR (control register 1) bit positions. HUM Ch 43.2.4 p 2884.
 */
typedef enum : uint8_t {
  k_ra8_spcr_bit_spe     = 0U,  /**< SPI Function Enable.                              */
  k_ra8_spcr_bit_sppe    = 8U,  /**< Parity Enable.                                    */
  k_ra8_spcr_bit_spoe    = 9U,  /**< Parity Mode (0=even, 1=odd).                      */
  k_ra8_spcr_bit_pte     = 11U, /**< Parity Self-Diagnosis Enable.                     */
  k_ra8_spcr_bit_sckase  = 12U, /**< RSPCK Auto-Stop Function Enable.                  */
  k_ra8_spcr_bit_bfds    = 13U, /**< Between Burst Transfer Frames Delay Sel.          */
  k_ra8_spcr_bit_modfen  = 14U, /**< Mode Fault Error Detection Enable.                */
  k_ra8_spcr_bit_speie   = 16U, /**< Error Interrupt Enable.                           */
  k_ra8_spcr_bit_sprie   = 17U, /**< RX Buffer Full Interrupt Enable.                  */
  k_ra8_spcr_bit_spiie   = 18U, /**< Idle Interrupt Enable.                            */
  k_ra8_spcr_bit_spdres  = 19U, /**< Receive Data Ready Error Select.                  */
  k_ra8_spcr_bit_sptie   = 20U, /**< TX Buffer Empty Interrupt Enable.                 */
  k_ra8_spcr_bit_cendie  = 21U, /**< Communication End Interrupt Enable.               */
  k_ra8_spcr_bit_spms    = 24U, /**< 3-Wire Mode Select (1) vs 4-Wire (0).             */
  k_ra8_spcr_bit_spfrf   = 25U, /**< Frame Format Select (Motorola/TI SSP).            */
  k_ra8_spcr_bit_txmd_lo = 28U, /**< Communication Mode bit 0.                         */
  k_ra8_spcr_bit_txmd_hi = 29U, /**< Communication Mode bit 1.                         */
  k_ra8_spcr_bit_mstr    = 30U, /**< Controller/Peripheral Mode Select (1=controller). */
  k_ra8_spcr_bit_bpen    = 31U, /**< Synchronization Circuit Bypass Enable.            */
} ra8_spcr_bit_t;

/**
 * @enum ra8_spcr_mask_t
 * @brief SPCR bit-masks. HUM Ch 43.2.4 p 2884.
 */
typedef enum : uint32_t {
  k_ra8_spcr_mask_spe    = 0x00000001UL, /**< RA8 spcr mask spe.          */
  k_ra8_spcr_mask_modfen = 0x00004000UL, /**< RA8 spcr mask modfen.       */
  k_ra8_spcr_mask_speie  = 0x00010000UL, /**< RA8 spcr mask speie.        */
  k_ra8_spcr_mask_sprie  = 0x00020000UL, /**< RA8 spcr mask sprie.        */
  k_ra8_spcr_mask_sptie  = 0x00100000UL, /**< RA8 spcr mask sptie.        */
  k_ra8_spcr_mask_cendie = 0x00200000UL, /**< RA8 spcr mask cendie.       */
  k_ra8_spcr_mask_sckase = 0x00001000UL, /**< RA8 spcr mask sckase.       */
  k_ra8_spcr_mask_mstr   = 0x40000000UL, /**< RA8 spcr mask mstr.         */
  k_ra8_spcr_mask_txmd   = 0x30000000UL, /**< [29:28] communication mode. */
} ra8_spcr_mask_t;

/**
 * @enum ra8_spsr_bit_t
 * @brief SPSR (status register) bit positions. HUM Ch 43.2.9 p 2898.
 */
typedef enum : uint8_t {
  k_ra8_spsr_bit_spdrf = 23U, /**< Receive Data Ready Flag. */
  k_ra8_spsr_bit_ovrf  = 24U, /**< Overrun Error Flag.      */
  k_ra8_spsr_bit_idlnf = 25U, /**< Idle Flag.               */
  k_ra8_spsr_bit_modf  = 26U, /**< Mode Fault Error Flag.   */
  k_ra8_spsr_bit_perf  = 27U, /**< Parity Error Flag.       */
  k_ra8_spsr_bit_udrf  = 28U, /**< Underrun Error Flag.     */
  k_ra8_spsr_bit_sptef = 29U, /**< TX Buffer Empty Flag.    */
  k_ra8_spsr_bit_cendf = 30U, /**< Communication End Flag.  */
  k_ra8_spsr_bit_sprf  = 31U, /**< RX Buffer Full Flag.     */
} ra8_spsr_bit_t;

/**
 * @enum ra8_spsr_mask_t
 * @brief SPSR bit-masks. HUM Ch 43.2.9 p 2898.
 */
typedef enum : uint32_t {
  k_ra8_spsr_mask_spdrf = 0x00800000UL, /**< RA8 spsr mask spdrf.       */
  k_ra8_spsr_mask_ovrf  = 0x01000000UL, /**< RA8 spsr mask ovrf.        */
  k_ra8_spsr_mask_idlnf = 0x02000000UL, /**< RA8 spsr mask idlnf.       */
  k_ra8_spsr_mask_modf  = 0x04000000UL, /**< RA8 spsr mask modf.        */
  k_ra8_spsr_mask_perf  = 0x08000000UL, /**< RA8 spsr mask perf.        */
  k_ra8_spsr_mask_udrf  = 0x10000000UL, /**< RA8 spsr mask udrf.        */
  k_ra8_spsr_mask_sptef = 0x20000000UL, /**< RA8 spsr mask sptef.       */
  k_ra8_spsr_mask_cendf = 0x40000000UL, /**< RA8 spsr mask cendf.       */
  k_ra8_spsr_mask_sprf  = 0x80000000UL, /**< RA8 spsr mask sprf.        */
  k_ra8_spsr_mask_errs  = 0x1D000000UL, /**< OVRF | MODF | PERF | UDRF. */
} ra8_spsr_mask_t;

/**
 * @enum ra8_spsrc_mask_t
 * @brief SPSRC (status clear, write-1-to-clear) masks. HUM Ch 43.2.13 p 2905.
 */
typedef enum : uint32_t {
  k_ra8_spsrc_mask_spdrfc = 0x00800000UL, /**< RA8 spsrc mask spdrfc.    */
  k_ra8_spsrc_mask_ovrfc  = 0x01000000UL, /**< RA8 spsrc mask ovrfc.     */
  k_ra8_spsrc_mask_modfc  = 0x04000000UL, /**< RA8 spsrc mask modfc.     */
  k_ra8_spsrc_mask_perfc  = 0x08000000UL, /**< RA8 spsrc mask perfc.     */
  k_ra8_spsrc_mask_udrfc  = 0x10000000UL, /**< RA8 spsrc mask udrfc.     */
  k_ra8_spsrc_mask_sptefc = 0x20000000UL, /**< RA8 spsrc mask sptefc.    */
  k_ra8_spsrc_mask_cendfc = 0x40000000UL, /**< RA8 spsrc mask cendfc.    */
  k_ra8_spsrc_mask_sprfc  = 0x80000000UL, /**< RA8 spsrc mask sprfc.     */
  k_ra8_spsrc_mask_all    = 0xFD800000UL, /**< Clear every flag at once. */
} ra8_spsrc_mask_t;

/**
 * @enum ra8_spcmd_bit_t
 * @brief SPCMDn (command register) bit positions. HUM Ch 43.2.7 p 2893.
 */
typedef enum : uint8_t {
  k_ra8_spcmd_bit_cpha   = 0U,  /**< Clock Phase.               */
  k_ra8_spcmd_bit_cpol   = 1U,  /**< Clock Polarity.            */
  k_ra8_spcmd_bit_brdv0  = 2U,  /**< Bit-Rate Division bit 0.   */
  k_ra8_spcmd_bit_brdv1  = 3U,  /**< Bit-Rate Division bit 1.   */
  k_ra8_spcmd_bit_sslkp  = 7U,  /**< SSL Signal Level Hold.     */
  k_ra8_spcmd_bit_lsbf   = 12U, /**< LSB-first.                 */
  k_ra8_spcmd_bit_spnden = 13U, /**< Next-Access Delay Enable.  */
  k_ra8_spcmd_bit_slnden = 14U, /**< SSL Negation Delay Enable. */
  k_ra8_spcmd_bit_sckden = 15U, /**< SCK Delay Enable.          */
  k_ra8_spcmd_bit_spb_lo = 16U, /**< Data Length [20:16] low.   */
} ra8_spcmd_bit_t;

/**
 * @enum ra8_spcmd_mask_t
 * @brief SPCMDn bit-masks (HUM Ch 43.2.7 p 2893).
 */
typedef enum : uint32_t {
  k_ra8_spcmd_mask_cpha   = 0x00000001UL, /**< RA8 spcmd mask cpha.   */
  k_ra8_spcmd_mask_cpol   = 0x00000002UL, /**< RA8 spcmd mask cpol.   */
  k_ra8_spcmd_mask_brdv   = 0x0000000CUL, /**< [3:2]                  */
  k_ra8_spcmd_mask_sslkp  = 0x00000080UL, /**< RA8 spcmd mask sslkp.  */
  k_ra8_spcmd_mask_lsbf   = 0x00001000UL, /**< RA8 spcmd mask lsbf.   */
  k_ra8_spcmd_mask_spnden = 0x00002000UL, /**< RA8 spcmd mask spnden. */
  k_ra8_spcmd_mask_slnden = 0x00004000UL, /**< RA8 spcmd mask slnden. */
  k_ra8_spcmd_mask_sckden = 0x00008000UL, /**< RA8 spcmd mask sckden. */
  k_ra8_spcmd_mask_spb    = 0x001F0000UL, /**< [20:16] data length    */
  k_ra8_spcmd_mask_ssla   = 0x07000000UL, /**< [26:24] SSL select     */
} ra8_spcmd_mask_t;

/**
 * @enum ra8_spcmd_spb_t
 * @brief SPB[4:0] data-length encodings used by the driver.
 *
 * @details
 * HUM Ch 43.2.7 lists the full SPB encoding table -- SPB = (N-1)
 * for an ``N``-bit frame width. Mirrored against FSP
 * ``spi_bit_width_t`` so the same numeric value can flow from the
 * public driver enum straight into the SPCMDn.SPB field. Values
 * not used by the bring-up driver are not exposed here.
 */
typedef enum : uint8_t {
  k_ra8_spcmd_spb_8bit  = 0x07U, /**< 0b00111 ->  8-bit frame. */
  k_ra8_spcmd_spb_16bit = 0x0FU, /**< 0b01111 -> 16-bit frame. */
  k_ra8_spcmd_spb_32bit = 0x1FU, /**< 0b11111 -> 32-bit frame. */
} ra8_spcmd_spb_t;

/**
 * @enum ra8_spcr3_bit_t
 * @brief SPCR3 bit positions (SSLnP polarity, SPBR, SPSLN). HUM Ch 43.2.6 p 2891.
 */
typedef enum : uint8_t {
  k_ra8_spcr3_bit_ssl0p = 0U,  /**< SSL0 polarity (0=active low).           */
  k_ra8_spcr3_bit_ssl1p = 1U,  /**< SSL1 polarity.                          */
  k_ra8_spcr3_bit_ssl2p = 2U,  /**< SSL2 polarity.                          */
  k_ra8_spcr3_bit_ssl3p = 3U,  /**< SSL3 polarity.                          */
  k_ra8_spcr3_bit_spbr  = 8U,  /**< SPBR field [15:8] -- bit-rate divider.  */
  k_ra8_spcr3_bit_spsln = 24U, /**< SPSLN field [26:24] -- sequence length. */
} ra8_spcr3_bit_t;

/**
 * @enum ra8_spcr3_mask_t
 * @brief SPCR3 masks. HUM Ch 43.2.6 p 2891.
 */
typedef enum : uint32_t {
  k_ra8_spcr3_mask_ssl_pol = 0x0000000FUL, /**< [3:0] SSLnP.             */
  k_ra8_spcr3_mask_spbr    = 0x0000FF00UL, /**< [15:8] bit-rate divider. */
  k_ra8_spcr3_mask_spsln   = 0x07000000UL, /**< [26:24] sequence length. */
} ra8_spcr3_mask_t;

/**
 * @enum ra8_spcr3_misc_t
 * @brief SPCR3 misc constants used in driver-side bit-shifting math.
 */
typedef enum : uint8_t {
  k_ra8_spbr_max = 0xFFU, /**< SPBR is 8-bit unsigned. */
} ra8_spcr3_misc_t;

/**
 * @enum ra8_spfcr_mask_t
 * @brief SPFCR (FIFO clear) masks. HUM Ch 43.2.14 p 2906.
 */
typedef enum : uint32_t {
  k_ra8_spfcr_mask_spfrst = 0x00000001UL, /**< Reset both FIFOs. */
} ra8_spfcr_mask_t;

/**
 * @enum ra8_spcr2_mask_t
 * @brief SPCR2 (control 2) masks. HUM Ch 43.2.4 p 2889.
 */
typedef enum : uint32_t {
  k_ra8_spcr2_mask_splp  = 0x00010000UL, /**< Inverting loopback (rx = ~tx).    */
  k_ra8_spcr2_mask_splp2 = 0x00020000UL, /**< Non-inverting loopback (rx = tx). */
  k_ra8_spcr2_mask_moife = 0x00200000UL, /**< COPI Idle Fixed Value Enable.     */
  k_ra8_spcr2_mask_moifv = 0x00100000UL, /**< COPI Idle Fixed Value.            */
} ra8_spcr2_mask_t;

/**
 * @enum ra8_sppsr_mask_t
 * @brief SPPSR (polling status) masks. HUM Ch 43.2.12 p 2905.
 */
typedef enum : uint32_t {
  k_ra8_sppsr_mask_speps = 0x00000001UL, /**< Asserted while SPI is busy. */
} ra8_sppsr_mask_t;

/**
 * @enum ra8_spi_b_channel_count_t
 * @brief Channel count addressed by the SPI_B driver.
 *
 * @details
 * Shared between ``ra8_spi_b.c`` (core ops + ISR dispatch) and
 * ``ra8_spi_b_dma.c`` (DMA pipes) so both translation units bound their
 * per-channel tables and channel-range checks against the same constant.
 */
typedef enum : uint8_t {
  k_ra8_spi_b_channel_count = 2U, /**< SPI0 + SPI1. */
} ra8_spi_b_channel_count_t;

/**
 * @brief Get pointer to SPI_B channel @p channel (0..1).
 *
 * @param[in] channel Channel index, must be < ::k_ra8_spi_primary_count.
 * @return Volatile pointer to the channel register window, or
 *         ``nullptr`` for an out-of-range channel.
 *
 * @pre @p channel encoded as ``uint8_t`` (0..255).
 * @pre Caller has enabled the SPI MSTP gate before any RW access.
 * @post No side effect on the register block.
 *
 * @note Thread-safe (pure pointer arithmetic, no shared state).
 *
 * @since 0.1.0
 */
RA8_HW_REGISTER_ACCESS
static inline volatile r_spi_regs_t* ra8_spi(uint8_t channel)
{
  if ((uint16_t)channel >= k_ra8_spi_primary_count) {
    return nullptr;
  }
  return (volatile r_spi_regs_t*)(k_ra8_spi0_base_addr +
                                  ((uintptr_t)channel * (uintptr_t)k_ra8_spi_channel_stride));
}

#ifdef __cplusplus
}
#endif
