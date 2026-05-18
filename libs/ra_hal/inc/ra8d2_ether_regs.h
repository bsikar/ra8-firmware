/**
 * @file ra8d2_ether_regs.h
 * @brief Ethernet controller base addresses for the Renesas RA8D2
 *
 * @details
 * RA8D2 has a gigabit Ethernet subsystem composed of several blocks
 * (GMAC A/B, MII forwarder, TSNSW, GPTP). Base addresses below come
 * from R7KA8D2KF. Drivers land with a real network stack.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef enum : uintptr_t {
  k_ra_etha0_base_addr = 0x403CA000UL,
  k_ra_etha1_base_addr = 0x403CC000UL,
  k_ra_rmac0_base_addr = 0x403CB000UL,
  k_ra_rmac1_base_addr = 0x403CD000UL,
  k_ra_mfwd_base_addr  = 0x403C0000UL,
  k_ra_coma_base_addr  = 0x403C9000UL,
  k_ra_gptp_base_addr  = 0x403E0000UL,
  k_ra_eswm_base_addr  = 0x403C8000UL,
  k_ra_gwca0_base_addr = 0x403CE000UL,
} ra_ether_addr_t;

/**
 * @enum ra_coma_offset_t
 * @brief Per-register offsets into the COMA (Common Agent) block.
 *
 * @details
 * COMA wraps the ESWM peripheral with the bus-arbitration counters
 * and the shared per-port descriptor fences. The first four registers
 * (RIC / RRC / RCEC / RNS) are the "bring-up" surface; without:
 *   - RRC pulsed (reset the ESWM IP),
 *   - RCEC.RCE (bit 16) set to 1 (enable the switch clock),
 *   - CABPIRM.BPIOG pulsed (reset the COMA buffer pool),
 * the per-port RMAC and ETHA register windows remain inaccessible
 * even after their MSTP gates are released and PDCTRESWM has powered
 * the domain on. Discovered via J-Link MMIO probe on EK-RA8D2 hardware;
 * FSP r_layer3_switch_reset_coma confirms.
 */
typedef enum : uint16_t {
  k_ra_coma_off_ric     = 0x000U, /**< Interrupt Configuration.          */
  k_ra_coma_off_rrc     = 0x004U, /**< Reset Configuration (RR bit 0).   */
  k_ra_coma_off_rcec    = 0x008U, /**< Clock Enable Cfg (RCE bit 16).    */
  k_ra_coma_off_cabpirm = 0x040U, /**< Buf-pool Init/Reset (BPIOG / BPR).*/
} ra_coma_offset_t;

/**
 * @enum ra_coma_bit_t
 * @brief Bit positions in the COMA bring-up registers.
 */
typedef enum : uint32_t {
  k_ra_coma_rrc_rr        = (1UL << 0U),  /**< RRC.RR -- pulse to reset ESWM IP.  */
  k_ra_coma_rcec_rce      = (1UL << 16U), /**< RCEC.RCE -- enable switch clock.   */
  k_ra_coma_rcec_ace_mask = 0x7FUL,       /**< RCEC.ACE[6:0] -- agent clock enable.*/
  k_ra_coma_cabpirm_bpiog = (1UL << 0U),  /**< CABPIRM.BPIOG -- buf-pool init kick.*/
  k_ra_coma_cabpirm_bpr   = (1UL << 1U),  /**< CABPIRM.BPR -- buf-pool ready flag. */
} ra_coma_bit_t;

/** @brief Get pointer to the 32-bit COMA.RRC (Reset Configuration). */
static inline volatile uint32_t* ra_coma_rrc(void)
{
  return (volatile uint32_t*)(k_ra_coma_base_addr + (uintptr_t)k_ra_coma_off_rrc);
}

/** @brief Get pointer to the 32-bit COMA.RCEC (Clock Enable Configuration). */
static inline volatile uint32_t* ra_coma_rcec(void)
{
  return (volatile uint32_t*)(k_ra_coma_base_addr + (uintptr_t)k_ra_coma_off_rcec);
}

/** @brief Get pointer to the 32-bit COMA.CABPIRM (Buf-pool Init/Reset/Monitor). */
static inline volatile uint32_t* ra_coma_cabpirm(void)
{
  return (volatile uint32_t*)(k_ra_coma_base_addr + (uintptr_t)k_ra_coma_off_cabpirm);
}

/**
 * @struct r_eswm_regs_t
 * @brief Minimal Ethernet Switch Module (ESWM) register window.
 *
 * @details
 * cppcheck cannot see tests/ so it flags every field as unused;
 * each member is accessed via ``ra_eswm()`` in
 * ``libs/ra_hal/src/ra_eth.c``.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  volatile uint32_t ESWM_CTRL; /**< +0x00 Control. */
  volatile uint32_t ESWM_STS;  /**< +0x04 Status. */
  volatile uint32_t ESWM_IE;   /**< +0x08 Interrupt Enable. */
  volatile uint32_t ESWM_ICLR; /**< +0x0C Interrupt Clear. */
} r_eswm_regs_t;
/* cppcheck-suppress-end [unusedStructMember] */

/** @brief Get pointer to the ESWM register block. */
static inline volatile r_eswm_regs_t* ra_eswm(void)
{
  return (volatile r_eswm_regs_t*)k_ra_eswm_base_addr;
}

/**
 * @enum ra_eswm_mii_offset_t
 * @brief Offsets into the ESWM Media-Interface configuration window.
 *
 * @details
 * HUM Ch 29 (ESWM) -- the per-port media-interface registers live far
 * past the CTRL/STS block. Cross-checked against FSP CMSIS header
 * R7KA8D2KF_core0.h (R_ESWM_Type members MIIRR and MIICR0/1 at the
 * offsets below). Used by the RMAC PHY bring-up path to:
 *   1. Select RGMII vs RMII vs MII for each MAC port.
 *   2. Release the per-port media-interface reset (MIIRR.RGRST/RMRST).
 *
 * Without MIIRR's port-reset bit set high, MDC will NOT toggle even
 * if MPIC.PSMCS is programmed correctly.
 */
typedef enum : uint32_t {
  k_ra_eswm_off_miirr  = 0x19400UL, /**< Media Interface Reset Register.   */
  k_ra_eswm_off_miicr0 = 0x19404UL, /**< Media Interface Control 0 (RMAC0).*/
  k_ra_eswm_off_miicr1 = 0x19408UL, /**< Media Interface Control 1 (RMAC1).*/
} ra_eswm_mii_offset_t;

/**
 * @enum ra_eswm_miirr_bit_t
 * @brief Bit positions in ESWM.MIIRR (Media Interface Reset Register).
 */
typedef enum : uint32_t {
  k_ra_eswm_miirr_rgrst0 = (1UL << 0U), /**< RGMII0 interface reset (1 = reset). */
  k_ra_eswm_miirr_rgrst1 = (1UL << 1U), /**< RGMII1 interface reset.             */
  k_ra_eswm_miirr_rmrst0 = (1UL << 8U), /**< RMII0 interface reset.              */
  k_ra_eswm_miirr_rmrst1 = (1UL << 9U), /**< RMII1 interface reset.              */
} ra_eswm_miirr_bit_t;

/**
 * @enum ra_eswm_miicr_field_t
 * @brief Field encodings for ESWM.MIICRn (Media Interface Control n).
 *
 * @details
 * MIISEL[1:0] selects the MII flavour driven onto the n-th MAC pin
 * group: 0 = MII/GMII, 1 = RGMII, 2 = RMII. TXCIDE (bit 12) enables
 * the on-chip TXC internal delay for RGMII (matches the FSP default
 * "RGMII + TX skew" board profile).
 */
typedef enum : uint32_t {
  k_ra_eswm_miicr_miisel_mii   = 0UL,         /**< MIISEL = MII/GMII.              */
  k_ra_eswm_miicr_miisel_rgmii = 1UL,         /**< MIISEL = RGMII.                 */
  k_ra_eswm_miicr_miisel_rmii  = 2UL,         /**< MIISEL = RMII.                  */
  k_ra_eswm_miicr_miisel_mask  = 0x3UL,       /**< MIISEL field mask (bits 1:0).   */
  k_ra_eswm_miicr_divstp       = (1UL << 8U), /**< DIVSTP -- gate the clock div.   */
  k_ra_eswm_miicr_txcide       = (1UL << 12U),/**< TXCIDE -- TXC internal delay.   */
} ra_eswm_miicr_field_t;

/** @brief Get pointer to ESWM.MIIRR (32-bit). */
static inline volatile uint32_t* ra_eswm_miirr(void)
{
  return (volatile uint32_t*)(k_ra_eswm_base_addr + (uintptr_t)k_ra_eswm_off_miirr);
}

/** @brief Get pointer to ESWM.MIICR0 (32-bit, RMAC port 0). */
static inline volatile uint32_t* ra_eswm_miicr0(void)
{
  return (volatile uint32_t*)(k_ra_eswm_base_addr + (uintptr_t)k_ra_eswm_off_miicr0);
}

/** @brief Get pointer to ESWM.MIICR1 (32-bit, RMAC port 1). */
static inline volatile uint32_t* ra_eswm_miicr1(void)
{
  return (volatile uint32_t*)(k_ra_eswm_base_addr + (uintptr_t)k_ra_eswm_off_miicr1);
}

/**
 * @struct r_mfwd_regs_t
 * @brief Minimal Message Forwarding Engine (MFWD) register window.
 *
 * @details
 * The MFWD block sits between the GMAC ports and the CPU agent
 * (GWCA). The minimal CTRL/STS/IE/ICLR layout below is enough to
 * bring the block in and out of run mode + service IRQs; the
 * full forwarding table programming surface lands with the first
 * routing-aware consumer.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  volatile uint32_t MFWD_CTRL; /**< +0x00 Forwarding Control. */
  volatile uint32_t MFWD_STS;  /**< +0x04 Status. */
  volatile uint32_t MFWD_IE;   /**< +0x08 Interrupt Enable. */
  volatile uint32_t MFWD_ICLR; /**< +0x0C Interrupt Clear. */
} r_mfwd_regs_t;
/* cppcheck-suppress-end [unusedStructMember] */

/** @brief Get pointer to the MFWD register block. */
static inline volatile r_mfwd_regs_t* ra_mfwd(void)
{
  return (volatile r_mfwd_regs_t*)k_ra_mfwd_base_addr;
}

/**
 * @struct r_coma_regs_t
 * @brief Minimal Common Agent (COMA) register window.
 *
 * @details
 * COMA is the management agent that owns the bus arbitration
 * counters and the shared per-port descriptor fences. The
 * minimal control surface mirrors the other ethernet blocks.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  volatile uint32_t COMA_CTRL; /**< +0x00 Common Agent Control. */
  volatile uint32_t COMA_STS;  /**< +0x04 Status. */
  volatile uint32_t COMA_IE;   /**< +0x08 Interrupt Enable. */
  volatile uint32_t COMA_ICLR; /**< +0x0C Interrupt Clear. */
} r_coma_regs_t;
/* cppcheck-suppress-end [unusedStructMember] */

/** @brief Get pointer to the COMA register block. */
static inline volatile r_coma_regs_t* ra_coma(void)
{
  /* COMA shares the MFWD window in the documented layout; if a
   * future revision moves it out, the linker constant above
   * needs a separate k_ra_coma_base_addr entry. */
  return (volatile r_coma_regs_t*)k_ra_mfwd_base_addr;
}

/**
 * @struct r_gwca_regs_t
 * @brief Minimal CPU Agent (GWCA) register window.
 *
 * @details
 * GWCA is the bridge between MFWD/COMA and CPU memory; it owns
 * the per-channel descriptor rings the host uses for TX/RX
 * staging. The + NIC consumer programmes the descriptor
 * rings; this scaffold just covers the lifecycle + IRQ surface.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  volatile uint32_t GWCA_CTRL; /**< +0x00 CPU Agent Control. */
  volatile uint32_t GWCA_STS;  /**< +0x04 Status. */
  volatile uint32_t GWCA_IE;   /**< +0x08 Interrupt Enable. */
  volatile uint32_t GWCA_ICLR; /**< +0x0C Interrupt Clear. */
} r_gwca_regs_t;
/* cppcheck-suppress-end [unusedStructMember] */

/** @brief Get pointer to the GWCA register block. */
static inline volatile r_gwca_regs_t* ra_gwca(void)
{
  return (volatile r_gwca_regs_t*)k_ra_gwca0_base_addr;
}

/**
 * @struct r_gptp_regs_t
 * @brief Minimal Generic PTP Timer (GPTP) register window.
 *
 * @details
 * GPTP is the IEEE-1588 hardware timestamp counter shared by the
 * GMAC ports for boundary-clock and ordinary-clock applications.
 * The scaffold covers the lifecycle + IRQ surface; the actual
 * timestamp counter / sync-frame encode-decode logic lands with
 * a real PTP stack.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  volatile uint32_t GPTP_CTRL; /**< +0x00 PTP Timer Control. */
  volatile uint32_t GPTP_STS;  /**< +0x04 Status. */
  volatile uint32_t GPTP_IE;   /**< +0x08 Interrupt Enable. */
  volatile uint32_t GPTP_ICLR; /**< +0x0C Interrupt Clear. */
} r_gptp_regs_t;
/* cppcheck-suppress-end [unusedStructMember] */

/** @brief Get pointer to the GPTP register block. */
static inline volatile r_gptp_regs_t* ra_gptp(void)
{
  return (volatile r_gptp_regs_t*)k_ra_gptp_base_addr;
}

#ifdef __cplusplus
}
#endif
