/**
 * @file ra8d2_i3c_regs.h
 * @brief I3C Bus Interface register layout for the Renesas RA8D2
 *
 * @details
 * Two I3C instances are available on the RA8D2 (R_I3C0 at
 * `0x4035F000` and R_I3C1 at `0x4035F100`); this driver only
 * exposes the first instance through ``ra_i3c()``.  Layout is
 * derived directly from the FSP CMSIS device header
 * ``R7KA8D2KF_core0.h`` (``R_I3C0_Type``) and matches the field
 * map in HUM Ch 40 "I3C Bus Interface (I3C)" pp 2445-2701.
 *
 * Coverage spans the bring-up registers (PRTS, CECTL, BCTL,
 * MSDVAD, RSTCTL, PRSST), the internal-error interrupt block
 * (INST, INSTE, INIE, INSTFC), the IBI / arbitration controls
 * (IBINCTL, BFCTL, SVCTL), the timing-and-rate group (REFCKCTL,
 * STDBR, EXTBR, BFRECDT, BAVLCDT, BIDLCDT), and the line-control
 * + bus-status block (OUTCTL, INCTL, TMOCTL, WUCTL, ACKCTL,
 * SCSTRCTL, SCSTLCTL, BST, BSTE, BIE, BSTFC).  The TX/RX FIFO
 * port registers (NCMDQP, NRSPQP, NTDTBP0, NIBIQP, NRSQP,
 * HCMDQP, HRSPQP, HTDTBP) and the device-address-table (DATBASn /
 * extended) blocks are intentionally omitted because the higher-
 * level CCC / IBI / HDR-DDR command engine has not been written
 * yet -- it lands with the first card-stack consumer.
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
 * @typedef ra_i3c_addr_t
 * @brief I3C peripheral base addresses.
 *
 * @details
 * Both instances share the identical ``R_I3C0_Type`` register
 * layout, so the same struct overlay can describe either base.
 */
typedef enum : uintptr_t {
  k_ra_i3c0_base_addr = 0x4035F000UL, /**< I3C0 peripheral base. */
  k_ra_i3c1_base_addr = 0x4035F100UL, /**< I3C1 peripheral base. */
  k_ra_i3c_base_addr  = k_ra_i3c0_base_addr,
} ra_i3c_addr_t;

/**
 * @typedef ra_i3c_limits_t
 * @brief Reserved-padding word counts for ``r_i3c_regs_t``.
 *
 * @details
 * Each padding slot mirrors the FSP ``R_I3C0_Type``
 * RESERVED arrays, named by the register that immediately
 * precedes the gap so that mismatches show up as a typed
 * symbol in compiler errors.
 */
typedef enum : uint8_t {
  k_ra_i3c_instance_count    = 2U,
  k_ra_i3c_pad_after_prts    = 3U, /**< RESERVED[3]   after PRTS. */
  k_ra_i3c_pad_after_msdvad  = 1U, /**< RESERVED1     after MSDVAD. */
  k_ra_i3c_pad_after_prsst   = 2U, /**< RESERVED2[2]  after PRSST. */
  k_ra_i3c_pad_after_instfc  = 1U, /**< RESERVED3     after INSTFC. */
  k_ra_i3c_pad_after_dvct    = 4U, /**< RESERVED4[4]  after DVCT. */
  k_ra_i3c_pad_after_ibinctl = 1U, /**< RESERVED5     after IBINCTL. */
  k_ra_i3c_pad_after_svctl   = 2U, /**< RESERVED6[2]  after SVCTL. */
  k_ra_i3c_pad_after_tmoctl  = 1U, /**< RESERVED7     after TMOCTL. */
  k_ra_i3c_pad_after_wuctl   = 1U, /**< RESERVED8     after WUCTL. */
  k_ra_i3c_pad_after_scstr   = 2U, /**< RESERVED9[2]  after SCSTRCTL. */
} ra_i3c_limits_t;

/**
 * @struct r_i3c_regs_t
 * @brief Partial RA8D2 I3C0 register window.
 *
 * @details
 * Mirrors the FSP ``R_I3C0_Type`` layout up through BSTFC at
 * offset 0x1DC.  See HUM Ch 40 "I3C Bus Interface (I3C)" pp
 * 2445-2701 for the field-level descriptions.  Members beyond
 * that offset (NTST, HTST, BCST, SVST, DATBASn ...) are not
 * declared because no driver code touches them yet.
 *
 * cppcheck cannot see the unit-test consumers that read every
 * field, so the unused-field warning is suppressed with a
 * begin/end pair instead of per-member annotations.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  volatile uint32_t PRTS; /**< +0x000 Protocol Selection Register. */
  volatile uint32_t _r_prts[k_ra_i3c_pad_after_prts];
  volatile uint32_t CECTL;  /**< +0x010 Clock Enable Control Register. */
  volatile uint32_t BCTL;   /**< +0x014 Bus Control Register. */
  volatile uint32_t MSDVAD; /**< +0x018 Master Device Address Register. */
  volatile uint32_t _r_msdvad[k_ra_i3c_pad_after_msdvad];
  volatile uint32_t RSTCTL; /**< +0x020 Reset Control Register. */
  volatile uint32_t PRSST;  /**< +0x024 Present State Register. */
  volatile uint32_t _r_prsst[k_ra_i3c_pad_after_prsst];
  volatile uint32_t INST;   /**< +0x030 Internal Status Register. */
  volatile uint32_t INSTE;  /**< +0x034 Internal Status Enable Register. */
  volatile uint32_t INIE;   /**< +0x038 Internal Interrupt Enable Register. */
  volatile uint32_t INSTFC; /**< +0x03C Internal Status Force Register. */
  volatile uint32_t DVCT;   /**< +0x040 Device Characteristic Table Register. */
  volatile uint32_t _r_dvct[k_ra_i3c_pad_after_dvct];
  volatile uint32_t IBINCTL; /**< +0x058 IBI Notify Control Register. */
  volatile uint32_t _r_ibinctl[k_ra_i3c_pad_after_ibinctl];
  volatile uint32_t BFCTL; /**< +0x060 Bus Function Control Register. */
  volatile uint32_t SVCTL; /**< +0x064 Slave Control Register. */
  volatile uint32_t _r_svctl[k_ra_i3c_pad_after_svctl];
  volatile uint32_t REFCKCTL; /**< +0x070 Reference Clock Control Register. */
  volatile uint32_t STDBR;    /**< +0x074 Standard Bit Rate Register. */
  volatile uint32_t EXTBR;    /**< +0x078 Extended Bit Rate Register. */
  volatile uint32_t BFRECDT;  /**< +0x07C Bus Free Condition Detection Time Register. */
  volatile uint32_t BAVLCDT;  /**< +0x080 Bus Available Condition Detection Time Register. */
  volatile uint32_t BIDLCDT;  /**< +0x084 Bus Idle Condition Detection Time Register. */
  volatile uint32_t OUTCTL;   /**< +0x088 Output Control Register. */
  volatile uint32_t INCTL;    /**< +0x08C Input Control Register. */
  volatile uint32_t TMOCTL;   /**< +0x090 Timeout Control Register. */
  volatile uint32_t _r_tmoctl[k_ra_i3c_pad_after_tmoctl];
  volatile uint32_t WUCTL; /**< +0x098 Wake-Up Unit Control Register. */
  volatile uint32_t _r_wuctl[k_ra_i3c_pad_after_wuctl];
  volatile uint32_t ACKCTL;   /**< +0x0A0 Acknowledge Control Register. */
  volatile uint32_t SCSTRCTL; /**< +0x0A4 SCL Stretch Control Register. */
  volatile uint32_t _r_scstr[k_ra_i3c_pad_after_scstr];
  volatile uint32_t SCSTLCTL; /**< +0x0B0 SCL Stalling Control Register. */
} r_i3c_regs_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @typedef ra_i3c_bctl_bits_t
 * @brief BCTL register bit positions / masks.
 */
typedef enum : uint32_t {
  k_ra_i3c_bctl_incba_mask    = 0x00000001UL, /**< INCBA bit 0. */
  k_ra_i3c_bctl_bmds_mask     = 0x00000080UL, /**< BMDS  bit 7. */
  k_ra_i3c_bctl_hjackctl_mask = 0x00000100UL, /**< HJACKCTL bit 8. */
  k_ra_i3c_bctl_abt_mask      = 0x20000000UL, /**< ABT   bit 29. */
  k_ra_i3c_bctl_rsm_mask      = 0x40000000UL, /**< RSM   bit 30. */
  k_ra_i3c_bctl_buse_mask     = 0x80000000UL, /**< BUSE  bit 31. */
} ra_i3c_bctl_bits_t;

/**
 * @typedef ra_i3c_msdvad_bits_t
 * @brief MSDVAD register bit positions / masks.
 */
typedef enum : uint32_t {
  k_ra_i3c_msdvad_mdyad_shift = 16U, /**< MDYAD bits [22:16]. */
  k_ra_i3c_msdvad_mdyad_mask  = 0x007F0000UL,
  k_ra_i3c_msdvad_mdyadv_mask = 0x80000000UL, /**< MDYADV bit 31. */
  k_ra_i3c_msdvad_addr_max    = 0x7FUL,       /**< 7-bit dynamic address. */
} ra_i3c_msdvad_bits_t;

/**
 * @typedef ra_i3c_rstctl_bits_t
 * @brief RSTCTL register bit positions / masks.
 *
 * @details
 * Bring-up sequence per HUM Ch 40 RSTCTL description: assert
 * RI3CRST, wait for hardware to clear it, then optionally assert
 * INTLRST + per-FIFO resets, then clear RSTCTL to release.
 */
typedef enum : uint32_t {
  k_ra_i3c_rstctl_ri3crst_mask = 0x00000001UL, /**< I3C software reset. */
  k_ra_i3c_rstctl_cmdqrst_mask = 0x00000002UL, /**< Command queue reset. */
  k_ra_i3c_rstctl_rspqrst_mask = 0x00000004UL, /**< Response queue reset. */
  k_ra_i3c_rstctl_tdbrst_mask  = 0x00000008UL, /**< Tx data buffer reset. */
  k_ra_i3c_rstctl_rdbrst_mask  = 0x00000010UL, /**< Rx data buffer reset. */
  k_ra_i3c_rstctl_ibiqrst_mask = 0x00000020UL, /**< IBI queue reset. */
  k_ra_i3c_rstctl_rsqrst_mask  = 0x00000040UL, /**< Receive status queue reset. */
  k_ra_i3c_rstctl_intlrst_mask = 0x00010000UL, /**< Internal software reset. */
  k_ra_i3c_rstctl_fifo_mask =
    (k_ra_i3c_rstctl_cmdqrst_mask | k_ra_i3c_rstctl_rspqrst_mask | k_ra_i3c_rstctl_tdbrst_mask |
     k_ra_i3c_rstctl_rdbrst_mask | k_ra_i3c_rstctl_ibiqrst_mask | k_ra_i3c_rstctl_rsqrst_mask),
} ra_i3c_rstctl_bits_t;

/**
 * @typedef ra_i3c_inst_bits_t
 * @brief INST/INSTE/INIE bit positions.
 */
typedef enum : uint32_t {
  k_ra_i3c_inst_inef_mask = 0x00000400UL, /**< Internal Error Flag bit 10. */
} ra_i3c_inst_bits_t;

/**
 * @brief Get pointer to I3C0.
 * @return Volatile pointer to instance 0 register window.
 */
static inline volatile r_i3c_regs_t* ra_i3c(void)
{
  return (volatile r_i3c_regs_t*)k_ra_i3c0_base_addr;
}

#ifdef __cplusplus
}
#endif
