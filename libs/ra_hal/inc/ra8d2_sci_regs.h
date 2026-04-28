/**
 * @file ra8d2_sci_regs.h
 * @brief SCI_B (Serial Communication Interface, B variant) register layout
 *        for the RA8D2
 *
 * @details
 * The RA8D2 ships the SCI_B variant of the SCI peripheral (HUM Ch 38
 * "Serial Communications Interface (SCI)" intro page literally states
 * "This is the SCI_B version of the SCI peripheral module"). SCI_B is
 * register-incompatible with the legacy 8-bit SCI: every control,
 * data, status, and clear register is 32-bit, and the bit layout is
 * different.
 *
 * RA8D2 has 10 SCI_B channels (SCI0..SCI9) at base
 * `0x40358000` (Secure alias) / `0x50358000` (Non-Secure alias) with
 * a stride of `0x100` per channel. Each channel covers a 256-byte
 * window. This header models the asynchronous-UART-relevant subset of
 * that window plus all per-mode satellite registers cited by the
 * register summary table; bits required by Manchester / Simple LIN /
 * Simple IIC / Smart-card sub-modes are not enumerated here and will
 * be added when those modes get drivers.
 *
 * ## Register map (per channel, partial)
 *
 * | Offset | Name   | Width | Purpose                                         |
 * |-------:|--------|------:|-------------------------------------------------|
 * | 0x00   | RDR    | 32    | Receive Data Register                           |
 * | 0x04   | TDR    | 32    | Transmit Data Register                          |
 * | 0x08   | CCR0   | 32    | Common Control Register 0 (TE / RE / *IE)       |
 * | 0x0C   | CCR1   | 32    | Common Control Register 1 (parity / inv / NF)   |
 * | 0x10   | CCR2   | 32    | Common Control Register 2 (BRR / CKS / MDDR)    |
 * | 0x14   | CCR3   | 32    | Common Control Register 3 (mode / FIFO / CKE)   |
 * | 0x18   | CCR4   | 32    | Common Control Register 4 (sample / cmpd)       |
 * | 0x1C   | CESR   | 8     | Communication Enable Status (TIST / RIST)       |
 * | 0x20   | ICR    | 32    | Simple IIC Control Register                     |
 * | 0x24   | FCR    | 32    | FIFO Control Register                           |
 * | 0x2C   | MCR    | 32    | Manchester Control Register                     |
 * | 0x30   | DCR    | 32    | Driver Control Register (DE assert / negate)    |
 * | 0x34   | XCR0   | 32    | Simple LIN Control Register 0                   |
 * | 0x38   | XCR1   | 32    | Simple LIN Control Register 1                   |
 * | 0x3C   | XCR2   | 32    | Simple LIN Control Register 2                   |
 * | 0x48   | CSR    | 32    | Common Status Register (TDRE / TEND / *ER)      |
 * | 0x4C   | ISR    | 32    | Simple IIC Status Register                      |
 * | 0x50   | FRSR   | 32    | FIFO Receive Status Register                    |
 * | 0x54   | FTSR   | 32    | FIFO Transmit Status Register                   |
 * | 0x58   | MSR    | 32    | Manchester Status Register                      |
 * | 0x5C   | XSR0   | 32    | Simple LIN Status Register 0                    |
 * | 0x60   | XSR1   | 32    | Simple LIN Status Register 1                    |
 * | 0x68   | CFCLR  | 32    | Common Flag Clear Register                      |
 * | 0x6C   | ICFCLR | 32    | Simple IIC Flag Clear Register                  |
 * | 0x70   | FFCLR  | 32    | FIFO Flag Clear Register                        |
 * | 0x74   | MFCLR  | 32    | Manchester Flag Clear Register                  |
 * | 0x78   | XFCLR  | 32    | Simple LIN Flag Clear Register                  |
 *
 * Citations are to the HUM Rev 1.30 (Feb 2026), section 38 starting
 * at PDF page 2174.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* =============================================================================
 * Base addresses + layout
 * =============================================================================
 */

/**
 * @enum ra_sci_base_addr_t
 * @brief Per-channel SCI_B base addresses (Secure alias).
 *
 * @details
 * HUM Ch 38.2.1 "RSR : Receive Shift Register", p 2179 declares
 * `SCIn_B = 0x4035_8000 + 0x0100 * n`. The non-secure alias is
 * `0x5035_8000 + 0x0100 * n`; this driver runs in the Secure world
 * for now, so only the Secure alias is enumerated.
 */
typedef enum : uintptr_t {
  k_ra_sci0_base_addr = 0x40358000UL,
  k_ra_sci1_base_addr = 0x40358100UL,
  k_ra_sci2_base_addr = 0x40358200UL,
  k_ra_sci3_base_addr = 0x40358300UL,
  k_ra_sci4_base_addr = 0x40358400UL,
  k_ra_sci5_base_addr = 0x40358500UL,
  k_ra_sci6_base_addr = 0x40358600UL,
  k_ra_sci7_base_addr = 0x40358700UL,
  k_ra_sci8_base_addr = 0x40358800UL,
  k_ra_sci9_base_addr = 0x40358900UL,
} ra_sci_base_addr_t;

/**
 * @enum ra_sci_limits_t
 * @brief Channel-count and stride constants.
 */
typedef enum : uint16_t {
  k_ra_sci_channel_count  = 10U,    /**< SCI0..SCI9. */
  k_ra_sci_channel_max    = 9U,     /**< Highest valid channel index. */
  k_ra_sci_channel_stride = 0x100U, /**< Bytes between SCI channels. */
} ra_sci_limits_t;

/* =============================================================================
 * Register layout (per channel)
 * =============================================================================
 */

/**
 * @struct r_sci_regs_t
 * @brief Per-channel SCI_B register window.
 *
 * @details
 * Models the SCI_B 32-bit-register layout defined by HUM Ch 38.2
 * register summary tables, p 2174 onwards. Reserved gaps in the
 * 256-byte window are filled with named padding fields so the layout
 * is unambiguous.
 *
 * @invariant All members are 32-bit aligned and 32 bits wide; SCI_B
 * does not use byte-wide registers.
 * @invariant `sizeof(r_sci_regs_t)` is bounded by the channel stride
 * (`k_ra_sci_channel_stride`).
 */
typedef struct {
  volatile uint32_t RDR;      /**< +0x00 Receive Data. (HUM 38.2.2 p 2180) */
  volatile uint32_t TDR;      /**< +0x04 Transmit Data. (HUM 38.2.3 p 2181) */
  volatile uint32_t CCR0;     /**< +0x08 Common Control 0. (HUM 38.2.5 p 2182) */
  volatile uint32_t CCR1;     /**< +0x0C Common Control 1. (HUM 38.2.6 p 2185) */
  volatile uint32_t CCR2;     /**< +0x10 Common Control 2. (HUM 38.2.7 p 2189) */
  volatile uint32_t CCR3;     /**< +0x14 Common Control 3. (HUM 38.2.8 p 2203) */
  volatile uint32_t CCR4;     /**< +0x18 Common Control 4. (HUM 38.2.9 p 2210) */
  volatile uint32_t CESR;     /**< +0x1C Communication Enable Status. (HUM 38.2.29 p 2240) */
  volatile uint32_t ICR;      /**< +0x20 Simple IIC Control. (HUM 38.2.10 p 2212) */
  volatile uint32_t FCR;      /**< +0x24 FIFO Control. (HUM 38.2.11 p 2215) */
  volatile uint32_t _r_28;    /**< Reserved at offset 0x28. */
  volatile uint32_t MCR;      /**< +0x2C Manchester Control. (HUM 38.2.12 p 2216) */
  volatile uint32_t DCR;      /**< +0x30 Driver Control. (HUM 38.2.13 p 2219) */
  volatile uint32_t XCR0;     /**< +0x34 Simple LIN Control 0. (HUM 38.2.14 p 2220) */
  volatile uint32_t XCR1;     /**< +0x38 Simple LIN Control 1. (HUM 38.2.15 p 2223) */
  volatile uint32_t XCR2;     /**< +0x3C Simple LIN Control 2. (HUM 38.2.16 p 2224) */
  volatile uint32_t _r_40[2]; /**< Reserved at offsets 0x40..0x47. */
  volatile uint32_t CSR;      /**< +0x48 Common Status. (HUM 38.2.17 p 2225) */
  volatile uint32_t ISR;      /**< +0x4C Simple IIC Status. (HUM 38.2.18 p 2230) */
  volatile uint32_t FRSR;     /**< +0x50 FIFO Receive Status. (HUM 38.2.19 p 2231) */
  volatile uint32_t FTSR;     /**< +0x54 FIFO Transmit Status. (HUM 38.2.20 p 2232) */
  volatile uint32_t MSR;      /**< +0x58 Manchester Status. (HUM 38.2.21 p 2233) */
  volatile uint32_t XSR0;     /**< +0x5C Simple LIN Status 0. (HUM 38.2.22 p 2235) */
  volatile uint32_t XSR1;     /**< +0x60 Simple LIN Status 1. (HUM 38.2.23 p 2237) */
  volatile uint32_t _r_64;    /**< Reserved at offset 0x64. */
  volatile uint32_t CFCLR;    /**< +0x68 Common Flag Clear. (HUM 38.2.24 p 2238) */
  volatile uint32_t ICFCLR;   /**< +0x6C Simple IIC Flag Clear. (HUM 38.2.25 p 2239) */
  volatile uint32_t FFCLR;    /**< +0x70 FIFO Flag Clear. (HUM 38.2.26 p 2239) */
  volatile uint32_t MFCLR;    /**< +0x74 Manchester Flag Clear. (HUM 38.2.27 p 2240) */
  volatile uint32_t XFCLR;    /**< +0x78 Simple LIN Flag Clear. (HUM 38.2.28 p 2240) */
} r_sci_regs_t;

/* =============================================================================
 * Accessors
 * =============================================================================
 */

/**
 * @brief Get pointer to SCI_B channel N (0..9).
 *
 * @param[in] channel SCI channel index (0..9).
 * @return Volatile pointer to the channel's register window, or
 * `nullptr` if `channel` is out of range.
 *
 * @pre Caller has verified the SCI module's MSTP gate is open.
 * @post No side effects; pointer arithmetic only.
 */
static inline volatile r_sci_regs_t* ra_sci(uint8_t channel)
{
  if (channel > k_ra_sci_channel_max) {
    return nullptr;
  }
  return (volatile r_sci_regs_t*)(k_ra_sci0_base_addr +
                                  ((uintptr_t)channel * (uintptr_t)k_ra_sci_channel_stride));
}

/* =============================================================================
 * CCR0 -- Common Control Register 0
 * =============================================================================
 */

/**
 * @enum ra_sci_ccr0_bit_t
 * @brief Bit positions in CCR0.
 *
 * @details HUM Ch 38.2.5 "CCR0 : Common Control Register 0", p 2182.
 */
typedef enum : uint8_t {
  k_ra_sci_ccr0_bit_re    = 0U,  /**< Receive Enable. */
  k_ra_sci_ccr0_bit_te    = 4U,  /**< Transmit Enable. */
  k_ra_sci_ccr0_bit_mpie  = 8U,  /**< Multi-Processor Interrupt Enable. */
  k_ra_sci_ccr0_bit_dcme  = 9U,  /**< Data Compare Match Enable. */
  k_ra_sci_ccr0_bit_idsel = 10U, /**< ID Frame Select. */
  k_ra_sci_ccr0_bit_rie   = 16U, /**< Receive Interrupt Enable. */
  k_ra_sci_ccr0_bit_tie   = 20U, /**< Transmit Interrupt Enable. */
  k_ra_sci_ccr0_bit_teie  = 21U, /**< Transmit End Interrupt Enable. */
  k_ra_sci_ccr0_bit_sse   = 24U, /**< SSn Pin Function Enable. */
} ra_sci_ccr0_bit_t;

/* =============================================================================
 * CCR1 -- Common Control Register 1
 * =============================================================================
 */

/**
 * @enum ra_sci_ccr1_bit_t
 * @brief Bit positions in CCR1.
 *
 * @details HUM Ch 38.2.6 "CCR1 : Common Control Register 1", p 2185.
 */
typedef enum : uint8_t {
  k_ra_sci_ccr1_bit_ctse   = 0U,  /**< CTS Enable. */
  k_ra_sci_ccr1_bit_ctspen = 1U,  /**< CTS External Pin Enable. */
  k_ra_sci_ccr1_bit_spb2dt = 4U,  /**< Serial Port Break Data Select. */
  k_ra_sci_ccr1_bit_spb2io = 5U,  /**< Serial Port Break I/O. */
  k_ra_sci_ccr1_bit_pe     = 8U,  /**< Parity Enable. */
  k_ra_sci_ccr1_bit_pm     = 9U,  /**< Parity Mode (0=even, 1=odd). */
  k_ra_sci_ccr1_bit_tinv   = 12U, /**< TXD Invert. */
  k_ra_sci_ccr1_bit_rinv   = 13U, /**< RXD Invert. */
  k_ra_sci_ccr1_bit_splp   = 16U, /**< Loopback Control. */
  k_ra_sci_ccr1_bit_sharps = 20U, /**< Half-Duplex Communication Select. */
  k_ra_sci_ccr1_bit_nfen   = 28U, /**< Digital Noise Filter Enable. */
  k_ra_sci_ccr1_bit_nfm    = 29U, /**< Noise Filter Mode. */
} ra_sci_ccr1_bit_t;

/* =============================================================================
 * CCR2 -- Common Control Register 2 (BRR / clock select / MDDR)
 * =============================================================================
 */

/**
 * @enum ra_sci_ccr2_bit_t
 * @brief Bit positions / shifts in CCR2.
 *
 * @details HUM Ch 38.2.7 "CCR2 : Common Control Register 2", p 2189.
 */
typedef enum : uint8_t {
  k_ra_sci_ccr2_bit_bgdm   = 4U,  /**< Baud Rate Generator Double-Speed. */
  k_ra_sci_ccr2_bit_abcs   = 5U,  /**< Async Mode Base Clock Select (0=16x). */
  k_ra_sci_ccr2_bit_abcse  = 6U,  /**< Async Mode Extended Base Clock. */
  k_ra_sci_ccr2_bit_abcse2 = 7U,  /**< Async Mode Extended Base Clock 2. */
  k_ra_sci_ccr2_shift_brr  = 8U,  /**< BRR[7:0] field shift. */
  k_ra_sci_ccr2_bit_brme   = 16U, /**< Bit Rate Modulation Enable. */
  k_ra_sci_ccr2_shift_cks  = 20U, /**< CKS[1:0] field shift. */
  k_ra_sci_ccr2_shift_mddr = 24U, /**< MDDR[7:0] field shift. */
} ra_sci_ccr2_bit_t;

/**
 * @enum ra_sci_ccr2_mask_t
 * @brief Field masks (post-shift) for CCR2.
 */
typedef enum : uint32_t {
  k_ra_sci_ccr2_mask_brr_field  = 0x000000FFU << 8U, /**< BRR[15:8] in CCR2. */
  k_ra_sci_ccr2_mask_brr_byte   = 0xFFU,             /**< 8-bit BRR value. */
  k_ra_sci_ccr2_mask_cks_field  = 0x3U << 20U,       /**< CKS[21:20] in CCR2. */
  k_ra_sci_ccr2_mask_mddr_field = 0xFFU << 24U,      /**< MDDR[31:24]. */
} ra_sci_ccr2_mask_t;

/**
 * @enum ra_sci_ccr2_brr_const_t
 * @brief Constants used in the BRR formula.
 *
 * @details HUM Ch 38.2.7 "CCR2 : Common Control Register 2", p 2189
 * Table 38.7. For Asynchronous mode with the 16x base clock
 * (ABCS=ABCSE=ABCSE2=BGDM=0), N = (TCLK / (64 * 2^(2n-1) * B)) - 1.
 * With CKS = 0 -> n = 0 -> divisor = 64 * 0.5 = 32. We program
 * CKS = 0 and BGDM = 0 for the "default 16x base clock" path so the
 * effective divisor stays 32 here.
 */
typedef enum : uint32_t {
  k_ra_sci_brr_async_divisor = 32U, /**< 64 * 2^(2*0 - 1) for n = 0. */
} ra_sci_ccr2_brr_const_t;

/**
 * @enum ra_sci_ccr2_mddr_const_t
 * @brief Modulation-duty-setting constants.
 *
 * @details HUM Ch 38.2.7 "CCR2 : Common Control Register 2", p 2189.
 * The reset value of MDDR[7:0] is 0xFF; with BRME = 0 the modulation
 * function is disabled and the field is treated as a no-op. We
 * preserve the reset value when re-writing CCR2 so the field stays
 * "modulation off".
 */
typedef enum : uint32_t {
  k_ra_sci_mddr_default = 0xFFU, /**< MDDR reset value (modulation disabled). */
} ra_sci_ccr2_mddr_const_t;

/* =============================================================================
 * CCR3 -- Common Control Register 3 (mode / FIFO / clock-enable)
 * =============================================================================
 */

/**
 * @enum ra_sci_ccr3_bit_t
 * @brief Bit positions / shifts in CCR3.
 *
 * @details HUM Ch 38.2.8 "CCR3 : Common Control Register 3", p 2203.
 */
typedef enum : uint8_t {
  k_ra_sci_ccr3_bit_cpha    = 0U,  /**< Clock Phase (sync only). */
  k_ra_sci_ccr3_bit_cpol    = 1U,  /**< Clock Polarity (sync only). */
  k_ra_sci_ccr3_bit_bpen    = 7U,  /**< Synchronizer Bypass Enable. */
  k_ra_sci_ccr3_shift_chr   = 8U,  /**< CHR[1:0] data-length field. */
  k_ra_sci_ccr3_bit_lsbf    = 12U, /**< LSB First. */
  k_ra_sci_ccr3_bit_sinv    = 13U, /**< Data Invert. */
  k_ra_sci_ccr3_bit_stp     = 14U, /**< Stop Bit Length (0=1, 1=2). */
  k_ra_sci_ccr3_bit_rxdesel = 15U, /**< Async Start Bit Edge Detect. */
  k_ra_sci_ccr3_shift_mod   = 16U, /**< MOD[2:0] mode select. */
  k_ra_sci_ccr3_bit_mp      = 19U, /**< Multi-Processor mode. */
  k_ra_sci_ccr3_bit_fm      = 20U, /**< FIFO Mode Select. */
  k_ra_sci_ccr3_bit_den     = 21U, /**< RS-485 Driver Enable. */
  k_ra_sci_ccr3_shift_cke   = 24U, /**< CKE[1:0] clock enable. */
  k_ra_sci_ccr3_bit_acs0    = 26U, /**< Async Mode Clock Source. */
  k_ra_sci_ccr3_bit_gm      = 28U, /**< Smart-card GSM Mode. */
  k_ra_sci_ccr3_bit_blk     = 29U, /**< Smart-card Block Transfer. */
} ra_sci_ccr3_bit_t;

/**
 * @enum ra_sci_ccr3_chr_t
 * @brief Encoded values for CCR3.CHR[1:0] (HUM Ch 38.2.8, p 2204).
 */
typedef enum : uint32_t {
  k_ra_sci_ccr3_chr_9bit_a = 0x0U, /**< 00 = 9-bit data. */
  k_ra_sci_ccr3_chr_9bit_b = 0x1U, /**< 01 = 9-bit data. */
  k_ra_sci_ccr3_chr_8bit   = 0x2U, /**< 10 = 8-bit data (initial value). */
  k_ra_sci_ccr3_chr_7bit   = 0x3U, /**< 11 = 7-bit data. */
} ra_sci_ccr3_chr_t;

/**
 * @enum ra_sci_ccr3_mod_t
 * @brief Encoded values for CCR3.MOD[2:0] (HUM Ch 38.2.8, p 2204).
 */
typedef enum : uint32_t {
  k_ra_sci_ccr3_mod_async      = 0x0U, /**< Asynchronous (UART/multi-proc). */
  k_ra_sci_ccr3_mod_smartcard  = 0x1U, /**< Smart Card. */
  k_ra_sci_ccr3_mod_clocksync  = 0x2U, /**< Clock-synchronous. */
  k_ra_sci_ccr3_mod_simple_spi = 0x3U, /**< Simple SPI. */
  k_ra_sci_ccr3_mod_simple_iic = 0x4U, /**< Simple IIC. */
  k_ra_sci_ccr3_mod_manchester = 0x5U, /**< Manchester. */
  k_ra_sci_ccr3_mod_simple_lin = 0x6U, /**< Simple LIN. */
} ra_sci_ccr3_mod_t;

/* =============================================================================
 * CSR -- Common Status Register
 * =============================================================================
 */

/**
 * @enum ra_sci_csr_bit_t
 * @brief Bit positions in CSR.
 *
 * @details HUM Ch 38.2.17 "CSR : Common Status Register", p 2225.
 */
typedef enum : uint8_t {
  k_ra_sci_csr_bit_ers    = 4U,  /**< Smart-card Error Signal Status. */
  k_ra_sci_csr_bit_rxdmon = 15U, /**< RXDn Pin Monitor. */
  k_ra_sci_csr_bit_dcmf   = 16U, /**< Data Compare Match. */
  k_ra_sci_csr_bit_dper   = 17U, /**< Compare-Match Parity Error. */
  k_ra_sci_csr_bit_dfer   = 18U, /**< Compare-Match Framing Error. */
  k_ra_sci_csr_bit_orer   = 24U, /**< Overrun Error. */
  k_ra_sci_csr_bit_mff    = 26U, /**< Mode Fault Error (SPI). */
  k_ra_sci_csr_bit_per    = 27U, /**< Parity Error. */
  k_ra_sci_csr_bit_fer    = 28U, /**< Framing Error. */
  k_ra_sci_csr_bit_tdre   = 29U, /**< Transmit Data Empty. */
  k_ra_sci_csr_bit_tend   = 30U, /**< Transmit End. */
  k_ra_sci_csr_bit_rdrf   = 31U, /**< Receive Data Full. */
} ra_sci_csr_bit_t;

/* =============================================================================
 * CFCLR -- Common Flag Clear Register
 * =============================================================================
 */

/**
 * @enum ra_sci_cfclr_bit_t
 * @brief Bit positions in CFCLR.
 *
 * @details HUM Ch 38.2.24 "CFCLR : Common Flag Clear Register",
 * p 2238. Writing 1 to a bit clears the corresponding CSR flag;
 * read returns 0.
 */
typedef enum : uint8_t {
  k_ra_sci_cfclr_bit_ersc  = 4U,  /**< Clear CSR.ERS. */
  k_ra_sci_cfclr_bit_dcmfc = 16U, /**< Clear CSR.DCMF. */
  k_ra_sci_cfclr_bit_dperc = 17U, /**< Clear CSR.DPER. */
  k_ra_sci_cfclr_bit_dferc = 18U, /**< Clear CSR.DFER. */
  k_ra_sci_cfclr_bit_orerc = 24U, /**< Clear CSR.ORER. */
  k_ra_sci_cfclr_bit_mffc  = 26U, /**< Clear CSR.MFF. */
  k_ra_sci_cfclr_bit_perc  = 27U, /**< Clear CSR.PER. */
  k_ra_sci_cfclr_bit_ferc  = 28U, /**< Clear CSR.FER. */
  k_ra_sci_cfclr_bit_tdrec = 29U, /**< Clear CSR.TDRE. */
  k_ra_sci_cfclr_bit_rdrfc = 31U, /**< Clear CSR.RDRF. */
} ra_sci_cfclr_bit_t;

/* =============================================================================
 * RDR / TDR field masks
 * =============================================================================
 */

/**
 * @enum ra_sci_data_mask_t
 * @brief Masks for RDR/TDR data fields (HUM 38.2.2 p 2180, 38.2.3 p 2181).
 */
typedef enum : uint32_t {
  k_ra_sci_rdr_mask_data8 = 0xFFU, /**< RDAT[7:0] for 8-bit reception. */
  k_ra_sci_tdr_mask_data8 = 0xFFU, /**< TDAT[7:0] for 8-bit transmission. */
} ra_sci_data_mask_t;

#ifdef __cplusplus
}
#endif
