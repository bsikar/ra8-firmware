/**
 * @file ra8_mstp_regs.h
 * @brief Module Stop Control (MSTP) register layout for the Renesas RA8D2
 * @ingroup grp_hal_system
 *
 * @details
 * Every RA-family peripheral has a Module Stop bit. When that bit is
 * `1` the peripheral is clock-gated off and does not respond to bus
 * accesses. `Module Stop Control Registers A..E` (MSTPCRA..MSTPCRE)
 * at `0x40203000` (Secure alias) hold 32 bits each, one bit per
 * peripheral. The Non-Secure alias is at `0x50203000` and is
 * referenced once TrustZone is wired up.
 *
 * The exact MSTP bit for each peripheral is documented in HUM
 * Chapter 11 "Low Power Mode", sections 11.2.6..11.2.10 (MSTPCRA..E).
 * Bit assignments here are taken from the per-register tables on
 * pages 443..450 (Rev. 1.30) and cited inline.
 *
 * Drivers must NOT write these registers directly. The
 * substrate file ``libs/ra8_hal/src/ra8_mstp.c`` exposes a
 * ref-counted ``ra8_mstp_enable(id) / ra8_mstp_disable(id)`` pair that
 * tracks shared modules (DMAC + DTC, RSIP + TRNG, etc.) so two
 * unrelated drivers can both request the same MSTP bit without
 * trampling each other.
 *
 * ## Encoding
 *
 * ``ra8_mstp_t`` is a 16-bit value packed as ``(reg << 8) | bit``:
 * - ``reg`` 0..4 selects MSTPCRA..MSTPCRE.
 * - ``bit`` 0..31 selects the bit within the register.
 *
 * Names in the enum follow the canonical "MSTP target module"
 * column from the HUM tables, prefixed with ``k_ra8_mstp_``.
 *
 * ## Hardware constraints
 *
 * - **Read-back required**: HUM 11.2.6 Note 2 says "When changing the
 * value of this bit, only execute subsequent instructions after
 * reading this bit to check that the value was updated." The
 * ref-counted helper in ``ra8_mstp.c`` performs the read-back; the
 * register layout itself is just a memory-mapped ``r_mstp_regs_t``.
 * - **DMAC / DTC ordering**: HUM 11.2.6 Note 1 says "When rewriting
 * the MSTPAi bit from 0 to 1, disable the DMAC and DTC before
 * setting MSTPAi bit (i = 22, 23)." Caller responsibility -- ra8_mstp
 * does not poke the DMAC channel registers.
 * - **MSTPCR is NOT PRCR-protected**. The CGC, LVD and LPM groups
 * sit behind PRCR; the module-stop block does not.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

/* =============================================================================
 * Base / layout
 * =============================================================================
 */

typedef enum : uintptr_t {
#ifdef RA8_PERIPH_NS_ALIAS
  /* TrustZone Non-secure build (#96): reach MSTPCRA..E through the IDAU
   * bit[28]=1 Non-secure alias (+0x1000_0000). Per-bit module-stop security
   * follows the peripheral's PSAR attribution, so once the Secure side marks
   * USBFS0 Non-secure (PSARB11) the NS image may clear MSTPCRB.MSTPB11 here;
   * Secure-owned bits in the same word are write-ignored from NS. */
  k_ra8_mstp_base_addr = 0x50203000UL, /**< R_MSTP block base (NS alias bit[28]=1). */
#else
  k_ra8_mstp_base_addr = 0x40203000UL, /**< R_MSTP block base. */
#endif /**< (anon). */
} ra8_mstp_addr_t;

/**
 * @struct r_mstp_regs_t
 * @brief Memory layout of the Module Stop Control Register block.
 *
 * @details
 * 32-bit registers at offset 0x00 / 0x04 / 0x08 / 0x0C / 0x10.
 */
typedef struct {
  volatile uint32_t MSTPCRA; /**< Module Stop Control Register A (+0x00). */
  volatile uint32_t MSTPCRB; /**< Module Stop Control Register B (+0x04). */
  volatile uint32_t MSTPCRC; /**< Module Stop Control Register C (+0x08). */
  volatile uint32_t MSTPCRD; /**< Module Stop Control Register D (+0x0C). */
  volatile uint32_t MSTPCRE; /**< Module Stop Control Register E (+0x10). */
} r_mstp_regs_t;

/** @brief Expected byte size of the MSTP register block. */
typedef enum : uint8_t {
  k_ra8_mstp_block_bytes = 20U, /**< 5 x uint32_t = 20 bytes. */
} ra8_mstp_block_size_t;

static_assert(sizeof(r_mstp_regs_t) == (size_t)k_ra8_mstp_block_bytes,
              "R_MSTP struct must be 20 bytes");

/** @brief Get pointer to the MSTP register block. */
static inline volatile r_mstp_regs_t* ra8_mstp(void)
{
  return (volatile r_mstp_regs_t*)k_ra8_mstp_base_addr;
}

/* =============================================================================
 * Packed (register, bit) identifier
 * =============================================================================
 */

/**
 * @enum ra8_mstp_reg_t
 * @brief Which of MSTPCRA..MSTPCRE owns the bit.
 */
typedef enum : uint8_t {
  k_ra8_mstp_reg_a = 0U, /**< RA8 mstp register a. */
  k_ra8_mstp_reg_b = 1U, /**< RA8 mstp register b. */
  k_ra8_mstp_reg_c = 2U, /**< RA8 mstp register c. */
  k_ra8_mstp_reg_d = 3U, /**< RA8 mstp register d. */
  k_ra8_mstp_reg_e = 4U, /**< RA8 mstp register e. */
} ra8_mstp_reg_t;

/**
 * @enum ra8_mstp_t
 * @brief Packed ``(reg << 8) | bit`` module-stop identifier.
 *
 * @details
 * Every peripheral the firmware can ungate has an entry below. Each
 * value packs the (MSTPCR register, bit) pair from HUM Ch 11.2.6..10
 * into a single ``uint16_t``. Drivers feed these into
 * ``ra8_mstp_enable(id) / ra8_mstp_disable(id)``, never write the
 * register directly.
 *
 * Bit numbering matches the manual exactly (0-based from the LSB),
 * which is also what the 32-bit registers expect.
 */
typedef enum : uint16_t {
  /* ---- MSTPCRA -- HUM 11.2.6 p 443 -- DMA, SRAM ----------------- */
  k_ra8_mstp_sram0 = (uint16_t)(k_ra8_mstp_reg_a << 8),        /**< MSTPA0 SRAM0 */
  k_ra8_mstp_sram1 = (uint16_t)((k_ra8_mstp_reg_a << 8) | 1U), /**< MSTPA1 SRAM1 */
  k_ra8_mstp_sram2 = (uint16_t)((k_ra8_mstp_reg_a << 8) | 2U), /**< MSTPA2 SRAM2 */
  k_ra8_mstp_sram3 = (uint16_t)((k_ra8_mstp_reg_a << 8) | 3U), /**< MSTPA3 SRAM3 */
  k_ra8_mstp_dmac0_dtc0 =
    (uint16_t)((k_ra8_mstp_reg_a << 8) | 22U), /**< MSTPA22 DMAC0+DTC0 (shared) */
  k_ra8_mstp_dmac1_dtc1 =
    (uint16_t)((k_ra8_mstp_reg_a << 8) | 23U), /**< MSTPA23 DMAC1+DTC1 (shared) */

  /* ---- MSTPCRB -- HUM 11.2.7 p 444..445 -- comms ---------------- */
  k_ra8_mstp_i3c   = (uint16_t)((k_ra8_mstp_reg_b << 8) | 4U),  /**< MSTPB4 I3C          */
  k_ra8_mstp_iic2  = (uint16_t)((k_ra8_mstp_reg_b << 8) | 7U),  /**< MSTPB7 IIC2         */
  k_ra8_mstp_iic1  = (uint16_t)((k_ra8_mstp_reg_b << 8) | 8U),  /**< MSTPB8 IIC1         */
  k_ra8_mstp_iic0  = (uint16_t)((k_ra8_mstp_reg_b << 8) | 9U),  /**< MSTPB9 IIC0         */
  k_ra8_mstp_usbfs = (uint16_t)((k_ra8_mstp_reg_b << 8) | 11U), /**< MSTPB11 USBFS0      */
  k_ra8_mstp_usbhs = (uint16_t)((k_ra8_mstp_reg_b << 8) | 12U), /**< MSTPB12 USBHS       */
  k_ra8_mstp_ospi0 = (uint16_t)((k_ra8_mstp_reg_b << 8) | 16U), /**< MSTPB16 OSPI0+DOTF0 */
  k_ra8_mstp_ospi1 = (uint16_t)((k_ra8_mstp_reg_b << 8) | 17U), /**< MSTPB17 OSPI1+DOTF1 */
  k_ra8_mstp_spi1  = (uint16_t)((k_ra8_mstp_reg_b << 8) | 18U), /**< MSTPB18 SPI1        */
  k_ra8_mstp_spi0  = (uint16_t)((k_ra8_mstp_reg_b << 8) | 19U), /**< MSTPB19 SPI0        */
  k_ra8_mstp_sci9  = (uint16_t)((k_ra8_mstp_reg_b << 8) | 22U), /**< MSTPB22 SCI9        */
  k_ra8_mstp_sci8  = (uint16_t)((k_ra8_mstp_reg_b << 8) | 23U), /**< MSTPB23 SCI8        */
  k_ra8_mstp_sci7  = (uint16_t)((k_ra8_mstp_reg_b << 8) | 24U), /**< MSTPB24 SCI7        */
  k_ra8_mstp_sci6  = (uint16_t)((k_ra8_mstp_reg_b << 8) | 25U), /**< MSTPB25 SCI6        */
  k_ra8_mstp_sci5  = (uint16_t)((k_ra8_mstp_reg_b << 8) | 26U), /**< MSTPB26 SCI5        */
  k_ra8_mstp_sci4  = (uint16_t)((k_ra8_mstp_reg_b << 8) | 27U), /**< MSTPB27 SCI4        */
  k_ra8_mstp_sci3  = (uint16_t)((k_ra8_mstp_reg_b << 8) | 28U), /**< MSTPB28 SCI3        */
  k_ra8_mstp_sci2  = (uint16_t)((k_ra8_mstp_reg_b << 8) | 29U), /**< MSTPB29 SCI2        */
  k_ra8_mstp_sci1  = (uint16_t)((k_ra8_mstp_reg_b << 8) | 30U), /**< MSTPB30 SCI1        */
  k_ra8_mstp_sci0  = (uint16_t)((k_ra8_mstp_reg_b << 8) | 31U), /**< MSTPB31 SCI0        */

  /* ---- MSTPCRC -- HUM 11.2.8 p 446..447 -- video, security ----- */
  k_ra8_mstp_cac       = (uint16_t)(k_ra8_mstp_reg_c << 8),         /**< MSTPC0 CAC              */
  k_ra8_mstp_crc       = (uint16_t)((k_ra8_mstp_reg_c << 8) | 1U),  /**< MSTPC1 CRC              */
  k_ra8_mstp_glcdc     = (uint16_t)((k_ra8_mstp_reg_c << 8) | 4U),  /**< MSTPC4 GLCDC            */
  k_ra8_mstp_drw       = (uint16_t)((k_ra8_mstp_reg_c << 8) | 6U),  /**< MSTPC6 2D Drawing       */
  k_ra8_mstp_ssie1     = (uint16_t)((k_ra8_mstp_reg_c << 8) | 7U),  /**< MSTPC7 SSIE1            */
  k_ra8_mstp_ssie0     = (uint16_t)((k_ra8_mstp_reg_c << 8) | 8U),  /**< MSTPC8 SSIE0            */
  k_ra8_mstp_mipi_dsi  = (uint16_t)((k_ra8_mstp_reg_c << 8) | 10U), /**< MSTPC10 MIPI DSI        */
  k_ra8_mstp_sdhi1     = (uint16_t)((k_ra8_mstp_reg_c << 8) | 11U), /**< MSTPC11 SDHI1           */
  k_ra8_mstp_sdhi0     = (uint16_t)((k_ra8_mstp_reg_c << 8) | 12U), /**< MSTPC12 SDHI0           */
  k_ra8_mstp_doc       = (uint16_t)((k_ra8_mstp_reg_c << 8) | 13U), /**< MSTPC13 DOC             */
  k_ra8_mstp_elc       = (uint16_t)((k_ra8_mstp_reg_c << 8) | 14U), /**< MSTPC14 ELC             */
  k_ra8_mstp_ceu       = (uint16_t)((k_ra8_mstp_reg_c << 8) | 16U), /**< MSTPC16 CEU             */
  k_ra8_mstp_mipi_csi  = (uint16_t)((k_ra8_mstp_reg_c << 8) | 17U), /**< MSTPC17 MIPI CSI        */
  k_ra8_mstp_pdmif     = (uint16_t)((k_ra8_mstp_reg_c << 8) | 24U), /**< MSTPC24 PDM-IF          */
  k_ra8_mstp_canfd1    = (uint16_t)((k_ra8_mstp_reg_c << 8) | 26U), /**< MSTPC26 CANFD1          */
  k_ra8_mstp_canfd0    = (uint16_t)((k_ra8_mstp_reg_c << 8) | 27U), /**< MSTPC27 CANFD0          */
  k_ra8_mstp_ethphyclk = (uint16_t)((k_ra8_mstp_reg_c << 8) | 28U), /**< MSTPC28 Ether-PHY clock */
  k_ra8_mstp_eswm      = (uint16_t)((k_ra8_mstp_reg_c << 8) | 30U), /**< MSTPC30 ESWM            */
  k_ra8_mstp_rsip      = (uint16_t)((k_ra8_mstp_reg_c << 8) | 31U), /**< MSTPC31 RSIP-E50D       */

  /* ---- MSTPCRD -- HUM 11.2.9 p 448..449 -- timers, analog ------ */
  k_ra8_mstp_agt1    = (uint16_t)((k_ra8_mstp_reg_d << 8) | 4U),  /**< MSTPD4 AGT1          */
  k_ra8_mstp_agt0    = (uint16_t)((k_ra8_mstp_reg_d << 8) | 5U),  /**< MSTPD5 AGT0          */
  k_ra8_mstp_pdg     = (uint16_t)((k_ra8_mstp_reg_d << 8) | 6U),  /**< MSTPD6 PDG           */
  k_ra8_mstp_poeg_d  = (uint16_t)((k_ra8_mstp_reg_d << 8) | 11U), /**< MSTPD11 POEG group D */
  k_ra8_mstp_poeg_c  = (uint16_t)((k_ra8_mstp_reg_d << 8) | 12U), /**< MSTPD12 POEG group C */
  k_ra8_mstp_poeg_b  = (uint16_t)((k_ra8_mstp_reg_d << 8) | 13U), /**< MSTPD13 POEG group B */
  k_ra8_mstp_poeg_a  = (uint16_t)((k_ra8_mstp_reg_d << 8) | 14U), /**< MSTPD14 POEG group A */
  k_ra8_mstp_dac12_1 = (uint16_t)((k_ra8_mstp_reg_d << 8) | 19U), /**< MSTPD19 DAC12 ch 1   */
  k_ra8_mstp_dac12_0 = (uint16_t)((k_ra8_mstp_reg_d << 8) | 20U), /**< MSTPD20 DAC12 ch 0   */
  k_ra8_mstp_adc16h  = (uint16_t)((k_ra8_mstp_reg_d << 8) | 21U), /**< MSTPD21 ADC16H       */
  k_ra8_mstp_tsn     = (uint16_t)((k_ra8_mstp_reg_d << 8) | 22U), /**< MSTPD22 Temperature  */
  k_ra8_mstp_acmphs3 = (uint16_t)((k_ra8_mstp_reg_d << 8) | 25U), /**< MSTPD25 ACMPHS3      */
  k_ra8_mstp_acmphs2 = (uint16_t)((k_ra8_mstp_reg_d << 8) | 26U), /**< MSTPD26 ACMPHS2      */
  k_ra8_mstp_acmphs1 = (uint16_t)((k_ra8_mstp_reg_d << 8) | 27U), /**< MSTPD27 ACMPHS1      */
  k_ra8_mstp_acmphs0 = (uint16_t)((k_ra8_mstp_reg_d << 8) | 28U), /**< MSTPD28 ACMPHS0      */

  /* ---- MSTPCRE -- HUM 11.2.10 p 449..450 -- ULPT, GPT ---------- */
  k_ra8_mstp_ulpt1  = (uint16_t)((k_ra8_mstp_reg_e << 8) | 8U),  /**< MSTPE8 ULPT1       */
  k_ra8_mstp_ulpt0  = (uint16_t)((k_ra8_mstp_reg_e << 8) | 9U),  /**< MSTPE9 ULPT0       */
  k_ra8_mstp_gpt13  = (uint16_t)((k_ra8_mstp_reg_e << 8) | 18U), /**< MSTPE18 GPT13      */
  k_ra8_mstp_gpt12  = (uint16_t)((k_ra8_mstp_reg_e << 8) | 19U), /**< MSTPE19 GPT12      */
  k_ra8_mstp_gpt11  = (uint16_t)((k_ra8_mstp_reg_e << 8) | 20U), /**< MSTPE20 GPT11      */
  k_ra8_mstp_gpt10  = (uint16_t)((k_ra8_mstp_reg_e << 8) | 21U), /**< MSTPE21 GPT10      */
  k_ra8_mstp_gpt4_9 = (uint16_t)((k_ra8_mstp_reg_e << 8) | 27U), /**< MSTPE27 GPT4..GPT9 */
  k_ra8_mstp_gpt3   = (uint16_t)((k_ra8_mstp_reg_e << 8) | 28U), /**< MSTPE28 GPT3       */
  k_ra8_mstp_gpt2   = (uint16_t)((k_ra8_mstp_reg_e << 8) | 29U), /**< MSTPE29 GPT2       */
  k_ra8_mstp_gpt1   = (uint16_t)((k_ra8_mstp_reg_e << 8) | 30U), /**< MSTPE30 GPT1       */
  k_ra8_mstp_gpt0   = (uint16_t)((k_ra8_mstp_reg_e << 8) | 31U), /**< MSTPE31 GPT0       */
} ra8_mstp_t;

/**
 * @brief Extract the register index from a packed MSTP id.
 * @param[in] id Packed id.
 * @return Register index 0..4.
 *
 * @details
 * Real (non-inline) function so coverage tooling can attribute
 * line counts to a single defining translation unit. The body
 * is in ``libs/ra8_hal/src/ra8_mstp.c``.
 *
 * @since 0.1.0
 *
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 */
ra8_mstp_reg_t ra8_mstp_id_reg(ra8_mstp_t id);

/**
 * @brief Extract the bit number from a packed MSTP id.
 * @param[in] id Packed id.
 * @return Bit position 0..31.
 *
 * @details
 * Defined in ``libs/ra8_hal/src/ra8_mstp.c``.
 *
 * @since 0.1.0
 *
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 */
uint8_t ra8_mstp_id_bit(ra8_mstp_t id);

#ifdef __cplusplus
}
#endif
