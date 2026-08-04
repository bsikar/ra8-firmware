/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_i3c_regs.h
 * @brief I3C Bus Interface register layout for the Renesas RA8D2
 * @ingroup grp_hal_comms
 *
 * @details
 * Two I3C instances are available on the RA8D2 (R_I3C0 at
 * `0x4035F000` and R_I3C1 at `0x4035F100`); this driver only
 * exposes the first instance through ``ra8_i3c()``.  Layout is
 * derived directly from the FSP CMSIS device header
 * ``R7KA8D2KF_core0.h`` (``R_I3C0_Type``) and matches the field
 * map in HUM Ch 40 "I3C Bus Interface (I3C)" pp 2445-2701.
 *
 * Coverage spans the bring-up registers (PRTS, CECTL, BCTL,
 * MSDVAD, RSTCTL, PRSST), the internal-error interrupt block
 * (INST, INSTE, INIE, INSTFC), the IBI / arbitration controls
 * (IBINCTL, BFCTL, SVCTL), the timing-and-rate group (REFCKCTL,
 * STDBR, EXTBR, BFRECDT, BAVLCDT, BIDLCDT), the line-control
 * + bus-status block (OUTCTL, INCTL, TMOCTL, WUCTL, ACKCTL,
 * SCSTRCTL, SCSTLCTL), the FIFO port group used by the CCC
 * engine (NCMDQP, NRSPQP, NTDTBP0, NIBIQP, NRSQP), and the
 * Normal Transfer Status register (NTST).  Members beyond NTST
 * (HCMDQP, HRSPQP, HTDTBP, NQTHCTL, NTBTHCTL0, NRQTHCTL,
 * HQTHCTL, HTBTHCTL, BST, BSTE, BIE, BSTFC, NTSTE, NTIE, HTST,
 * BCST, SVST, DATBASn ...) are intentionally omitted because no
 * driver code touches them yet -- they land with the first
 * HDR-DDR / peripheral-mode / sync-timing consumer.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"

/**
 * @typedef ra8_i3c_addr_t
 * @brief I3C peripheral base addresses.
 *
 * @details
 * Both instances share the identical ``R_I3C0_Type`` register
 * layout, so the same struct overlay can describe either base.
 */
typedef enum : uintptr_t {
  k_ra8_i3c0_base_addr = 0x4035F000UL,         /**< I3C0 peripheral base. */
  k_ra8_i3c1_base_addr = 0x4035F100UL,         /**< I3C1 peripheral base. */
  k_ra8_i3c_base_addr  = k_ra8_i3c0_base_addr, /**< RA8 I3C base address. */
} ra8_i3c_addr_t;

/**
 * @typedef ra8_i3c_limits_t
 * @brief Reserved-padding word counts for ``r_i3c_regs_t``.
 *
 * @details
 * Each padding slot mirrors the FSP ``R_I3C0_Type``
 * RESERVED arrays, named by the register that immediately
 * precedes the gap so that mismatches show up as a typed
 * symbol in compiler errors.
 */
typedef enum : uint8_t {
  k_ra8_i3c_instance_count    = 2U, /**< RA8 I3C instance count.       */
  k_ra8_i3c_pad_after_prts    = 3U, /**< RESERVED[3]   after PRTS.     */
  k_ra8_i3c_pad_after_msdvad  = 1U, /**< RESERVED1     after MSDVAD.   */
  k_ra8_i3c_pad_after_prsst   = 2U, /**< RESERVED2[2]  after PRSST.    */
  k_ra8_i3c_pad_after_instfc  = 1U, /**< RESERVED3     after INSTFC.   */
  k_ra8_i3c_pad_after_dvct    = 4U, /**< RESERVED4[4]  after DVCT.     */
  k_ra8_i3c_pad_after_ibinctl = 1U, /**< RESERVED5     after IBINCTL.  */
  k_ra8_i3c_pad_after_svctl   = 2U, /**< RESERVED6[2]  after SVCTL.    */
  k_ra8_i3c_pad_after_tmoctl  = 1U, /**< RESERVED7     after TMOCTL.   */
  k_ra8_i3c_pad_after_wuctl   = 1U, /**< RESERVED8     after WUCTL.    */
  k_ra8_i3c_pad_after_scstr   = 2U, /**< RESERVED9[2]  after SCSTRCTL. */
  /* From SCSTLCTL+1 (0x0B4) through CNDCTL+RESERVED13 ending at 0x14F is
   * 0x9C bytes = 39 words.  Covers SVTDLG0, STCTL, ATCTL, ATTRG, ATCCNTE,
   * CNDCTL and the surrounding RESERVED10..RESERVED13 padding.  None of
   * these registers are touched by the current driver -- the higher-
   * level peripheral-mode + sync-timing card-stack consumer can carve them
   * out of the gap when it lands. */
  k_ra8_i3c_pad_after_scstlctl = 37U, /**< Reserved words 0x0BC..0x14F before NCMDQP. */
  /* The 0x158..0x17B gap is 8 reserved words (RESERVED14[8]) before NIBIQP. */
  k_ra8_i3c_pad_after_ntdtbp0 = 8U, /**< RESERVED14[8] after NTDTBP0. */
  /* IBI-status-queue port immediately follows NIBIQP -> NRSQP at 0x180. */
  /* From NRSQP+1 (0x184) up to NTST at 0x1E0 is 0x5C bytes = 23 words.
   * Covers HCMDQP, HRSPQP, HTDTBP, NQTHCTL, NTBTHCTL0, NRQTHCTL,
   * HQTHCTL, HTBTHCTL, BST, BSTE, BIE, BSTFC and intervening
   * reserved padding -- none touched by the current driver. */
  k_ra8_i3c_pad_after_nrsqp = 23U, /**< Reserved words 0x184..0x1DF before NTST. */
} ra8_i3c_limits_t;

/**
 * @struct r_i3c_regs_t
 * @brief Partial RA8D2 I3C0 register window.
 *
 * @details
 * Mirrors the FSP ``R_I3C0_Type`` layout up through NTST at
 * offset 0x1E0.  See HUM Ch 40 "I3C Bus Interface (I3C)" pp
 * 2445-2701 for the field-level descriptions.  Members beyond
 * that offset (NTSTE, NTIE, HTST, BCST, SVST, DATBASn ...) are
 * not declared because no driver code touches them yet.
 *
 * cppcheck cannot see the unit-test consumers that read every
 * field, so the unused-field warning is suppressed with a
 * begin/end pair instead of per-member annotations.
 */
typedef struct {
  /** +0x000 Protocol Selection Register. */
  volatile uint32_t PRTS;
  /** Reserved. */
  volatile uint32_t _r_prts[k_ra8_i3c_pad_after_prts];
  volatile uint32_t CECTL;  /**< +0x010 Clock Enable Control Register.      */
  volatile uint32_t BCTL;   /**< +0x014 Bus Control Register.               */
  volatile uint32_t MSDVAD; /**< +0x018 Controller Device Address Register. */
  /** Reserved. */
  volatile uint32_t _r_msdvad[k_ra8_i3c_pad_after_msdvad];
  /** +0x020 Reset Control Register. */
  volatile uint32_t RSTCTL;
  /** +0x024 Present State Register. */
  volatile uint32_t PRSST;
  /** Reserved. */
  volatile uint32_t _r_prsst[k_ra8_i3c_pad_after_prsst];
  volatile uint32_t INST;   /**< +0x030 Internal Status Register.           */
  volatile uint32_t INSTE;  /**< +0x034 Internal Status Enable Register.    */
  volatile uint32_t INIE;   /**< +0x038 Internal Interrupt Enable Register. */
  volatile uint32_t INSTFC; /**< +0x03C Internal Status Force Register.     */
  /** Reserved. */
  volatile uint32_t _r_instfc[k_ra8_i3c_pad_after_instfc];
  volatile uint32_t DVCT; /**< +0x044 Device Characteristic Table Register. */
  /** Reserved. */
  volatile uint32_t _r_dvct[k_ra8_i3c_pad_after_dvct];
  /** +0x058 IBI Notify Control Register. */
  volatile uint32_t IBINCTL;
  /** Reserved. */
  volatile uint32_t _r_ibinctl[k_ra8_i3c_pad_after_ibinctl];
  volatile uint32_t BFCTL; /**< +0x060 Bus Function Control Register. */
  volatile uint32_t SVCTL; /**< +0x064 Peripheral Control Register.   */
  /** Reserved. */
  volatile uint32_t _r_svctl[k_ra8_i3c_pad_after_svctl];
  volatile uint32_t REFCKCTL; /**< +0x070 Reference Clock Control Register.                */
  volatile uint32_t STDBR;    /**< +0x074 Standard Bit Rate Register.                      */
  volatile uint32_t EXTBR;    /**< +0x078 Extended Bit Rate Register.                      */
  volatile uint32_t BFRECDT;  /**< +0x07C Bus Free Condition Detection Time Register.      */
  volatile uint32_t BAVLCDT;  /**< +0x080 Bus Available Condition Detection Time Register. */
  volatile uint32_t BIDLCDT;  /**< +0x084 Bus Idle Condition Detection Time Register.      */
  volatile uint32_t OUTCTL;   /**< +0x088 Output Control Register.                         */
  volatile uint32_t INCTL;    /**< +0x08C Input Control Register.                          */
  volatile uint32_t TMOCTL;   /**< +0x090 Timeout Control Register.                        */
  /** Reserved. */
  volatile uint32_t _r_tmoctl[k_ra8_i3c_pad_after_tmoctl];
  volatile uint32_t WUCTL; /**< +0x098 Wake-Up Unit Control Register. */
  /** Reserved. */
  volatile uint32_t _r_wuctl[k_ra8_i3c_pad_after_wuctl];
  volatile uint32_t ACKCTL;   /**< +0x0A0 Acknowledge Control Register. */
  volatile uint32_t SCSTRCTL; /**< +0x0A4 SCL Stretch Control Register. */
  /** Reserved. */
  volatile uint32_t _r_scstr[k_ra8_i3c_pad_after_scstr];
  volatile uint32_t SCSTLCTL;  /**< +0x0B0 SCL Stalling Control Register. */
  volatile uint32_t NSDVAD;    /**< +0x0B4 Peripheral Device Address Register
                                    (peripheral-mode dynamic address + valid).
                                    See HUM Ch 40 "Peripheral Device Address"
                                    pp 2445-2701. */
  volatile uint32_t NTIBIVCTL; /**< +0x0B8 IBI Valid Control Register
                                    (peripheral-side IBI queue valid count).
                                    See HUM Ch 40 "IBI Valid Control"
                                    pp 2445-2701. */
  /** Reserved. */
  volatile uint32_t _r_scstlctl[k_ra8_i3c_pad_after_scstlctl];
  volatile uint32_t NCMDQP;  /**< +0x150 Normal Command Queue Port Register.          */
  volatile uint32_t NRSPQP;  /**< +0x154 Normal Response Queue Port Register.         */
  volatile uint32_t NTDTBP0; /**< +0x158 Normal Transfer Data Buffer Port Register 0. */
  /** 0x15C..0x17B reserved gap. */
  volatile uint32_t _r_ntdtbp0[k_ra8_i3c_pad_after_ntdtbp0];
  volatile uint32_t NIBIQP; /**< +0x17C Normal IBI Queue Port Register.            */
  volatile uint32_t NRSQP;  /**< +0x180 Normal Receive Status Queue Port Register. */
  /** 0x184..0x1DF reserved gap. */
  volatile uint32_t _r_nrsqp[k_ra8_i3c_pad_after_nrsqp];
  volatile uint32_t NTST; /**< +0x1E0 Normal Transfer Status Register. */
} r_i3c_regs_t;

/**
 * @typedef ra8_i3c_bctl_bits_t
 * @brief BCTL register bit positions / masks.
 */
typedef enum : uint32_t {
  k_ra8_i3c_bctl_incba_mask    = 0x00000001UL, /**< INCBA bit 0.                           */
  k_ra8_i3c_bctl_bmds_mask     = 0x00000080UL, /**< BMDS  bit 7.                           */
  k_ra8_i3c_bctl_hjackctl_mask = 0x00000100UL, /**< HJACKCTL bit 8.                        */
  k_ra8_i3c_bctl_slve_mask     = 0x00010000UL, /**< SLVE  bit 16 (peripheral-mode enable). */
  k_ra8_i3c_bctl_abt_mask      = 0x20000000UL, /**< ABT   bit 29.                          */
  k_ra8_i3c_bctl_rsm_mask      = 0x40000000UL, /**< RSM   bit 30.                          */
  k_ra8_i3c_bctl_buse_mask     = 0x80000000UL, /**< BUSE  bit 31.                          */
} ra8_i3c_bctl_bits_t;

/**
 * @typedef ra8_i3c_nsdvad_bits_t
 * @brief NSDVAD (Peripheral Device Address Register) bit positions / masks.
 *
 * @details
 * Peripheral-mode counterpart of MSDVAD: SDYAD occupies bits [22:16],
 * SDYADV (bit 31) marks the address as valid.  See HUM Ch 40
 * "Peripheral Device Address Register" pp 2445-2701.
 */
typedef enum : uint32_t {
  k_ra8_i3c_nsdvad_sdyad_shift = 16U,          /**< SDYAD bits [22:16].        */
  k_ra8_i3c_nsdvad_sdyad_mask  = 0x007F0000UL, /**< RA8 I3C nsdvad sdyad mask. */
  k_ra8_i3c_nsdvad_sdyadv_mask = 0x80000000UL, /**< SDYADV bit 31.             */
} ra8_i3c_nsdvad_bits_t;

/**
 * @typedef ra8_i3c_ntibivctl_bits_t
 * @brief NTIBIVCTL (IBI Valid Control) field positions.
 *
 * @details
 * NTIBIVCTL.VLCNT[7:0] is the valid-entry count programmed by
 * ``ra8_i3c_ibi_enable``.  HUM Ch 40 "IBI Valid Control"
 * pp 2445-2701.
 */
typedef enum : uint32_t {
  k_ra8_i3c_ntibivctl_vlcnt_mask  = 0x000000FFUL, /**< Valid-entry count [7:0].       */
  k_ra8_i3c_ntibivctl_vlcnt_shift = 0U,           /**< RA8 I3C ntibivctl vlcnt shift. */
} ra8_i3c_ntibivctl_bits_t;

/**
 * @typedef ra8_i3c_cmd_hdr_mode_t
 * @brief NCMDQP "transfer mode" attribute values for HDR transactions.
 *
 * @details
 * The HDR-mode field lives at NCMDQP word-0 bits [27:26].  See
 * HUM Ch 40 "Command Descriptor" pp 2445-2701.
 */
typedef enum : uint32_t {
  k_ra8_i3c_cmd_hdr_mode_sdr  = 0UL,   /**< Plain SDR transfer (default). */
  k_ra8_i3c_cmd_hdr_mode_ddr  = 1UL,   /**< HDR-DDR.                      */
  k_ra8_i3c_cmd_hdr_mode_ts   = 2UL,   /**< HDR-TS.                       */
  k_ra8_i3c_cmd_hdr_mode_mask = 0x3UL, /**< RA8 I3C cmd hdr mode mask.    */
} ra8_i3c_cmd_hdr_mode_t;

/**
 * @typedef ra8_i3c_msdvad_bits_t
 * @brief MSDVAD register bit positions / masks.
 */
typedef enum : uint32_t {
  k_ra8_i3c_msdvad_mdyad_shift = 16U,          /**< MDYAD bits [22:16].        */
  k_ra8_i3c_msdvad_mdyad_mask  = 0x007F0000UL, /**< RA8 I3C msdvad mdyad mask. */
  k_ra8_i3c_msdvad_mdyadv_mask = 0x80000000UL, /**< MDYADV bit 31.             */
  k_ra8_i3c_msdvad_addr_max    = 0x7FUL,       /**< 7-bit dynamic address.     */
} ra8_i3c_msdvad_bits_t;

/**
 * @typedef ra8_i3c_rstctl_bits_t
 * @brief RSTCTL register bit positions / masks.
 *
 * @details
 * Bring-up sequence per HUM Ch 40 RSTCTL description: assert
 * RI3CRST, wait for hardware to clear it, then optionally assert
 * INTLRST + per-FIFO resets, then clear RSTCTL to release.
 */
typedef enum : uint32_t {
  k_ra8_i3c_rstctl_ri3crst_mask = 0x00000001UL, /**< I3C software reset.         */
  k_ra8_i3c_rstctl_cmdqrst_mask = 0x00000002UL, /**< Command queue reset.        */
  k_ra8_i3c_rstctl_rspqrst_mask = 0x00000004UL, /**< Response queue reset.       */
  k_ra8_i3c_rstctl_tdbrst_mask  = 0x00000008UL, /**< Tx data buffer reset.       */
  k_ra8_i3c_rstctl_rdbrst_mask  = 0x00000010UL, /**< Rx data buffer reset.       */
  k_ra8_i3c_rstctl_ibiqrst_mask = 0x00000020UL, /**< IBI queue reset.            */
  k_ra8_i3c_rstctl_rsqrst_mask  = 0x00000040UL, /**< Receive status queue reset. */
  k_ra8_i3c_rstctl_intlrst_mask = 0x00010000UL, /**< Internal software reset.    */
  k_ra8_i3c_rstctl_fifo_mask =
    (k_ra8_i3c_rstctl_cmdqrst_mask | k_ra8_i3c_rstctl_rspqrst_mask | k_ra8_i3c_rstctl_tdbrst_mask |
     k_ra8_i3c_rstctl_rdbrst_mask | k_ra8_i3c_rstctl_ibiqrst_mask |
     k_ra8_i3c_rstctl_rsqrst_mask), /**< RA8 I3C rstctl FIFO mask. */
} ra8_i3c_rstctl_bits_t;

/**
 * @typedef ra8_i3c_inst_bits_t
 * @brief INST/INSTE/INIE bit positions.
 */
typedef enum : uint32_t {
  k_ra8_i3c_inst_inef_mask = 0x00000400UL, /**< Internal Error Flag bit 10. */
} ra8_i3c_inst_bits_t;

/**
 * @typedef ra8_i3c_ccc_opcode_t
 * @brief MIPI I3C v1.0 Common Command Code (CCC) opcodes.
 *
 * @details
 * Broadcast CCCs occupy 0x00..0x7F and are addressed to 0x7E (the
 * I3C broadcast address).  Direct CCCs occupy 0x80..0xFE and target
 * a specific dynamic address after the CCC is issued.  See HUM
 * Ch 40 "I3C Bus Interface (I3C)" pp 2445-2701 and the MIPI I3C
 * specification "Common Command Codes" table.
 */
typedef enum : uint8_t {
  k_ra8_i3c_ccc_b_enec      = 0x00U, /**< Broadcast Enable Peripheral Events.             */
  k_ra8_i3c_ccc_b_disec     = 0x01U, /**< Broadcast Disable Peripheral Events.            */
  k_ra8_i3c_ccc_b_entas0    = 0x02U, /**< Broadcast Enter Activity State 0.               */
  k_ra8_i3c_ccc_b_entas1    = 0x03U, /**< Broadcast Enter Activity State 1.               */
  k_ra8_i3c_ccc_b_entas2    = 0x04U, /**< Broadcast Enter Activity State 2.               */
  k_ra8_i3c_ccc_b_entas3    = 0x05U, /**< Broadcast Enter Activity State 3.               */
  k_ra8_i3c_ccc_b_rstdaa    = 0x06U, /**< Broadcast Reset Dynamic Address Assignment.     */
  k_ra8_i3c_ccc_b_entdaa    = 0x07U, /**< Broadcast Enter Dynamic Address Assignment.     */
  k_ra8_i3c_ccc_b_rstact    = 0x2AU, /**< Broadcast Peripheral Reset Action.              */
  k_ra8_i3c_ccc_d_enec      = 0x80U, /**< Direct Enable Peripheral Events.                */
  k_ra8_i3c_ccc_d_disec     = 0x81U, /**< Direct Disable Peripheral Events.               */
  k_ra8_i3c_ccc_d_entas0    = 0x82U, /**< Direct Enter Activity State 0.                  */
  k_ra8_i3c_ccc_d_entas1    = 0x83U, /**< Direct Enter Activity State 1.                  */
  k_ra8_i3c_ccc_d_entas2    = 0x84U, /**< Direct Enter Activity State 2.                  */
  k_ra8_i3c_ccc_d_entas3    = 0x85U, /**< Direct Enter Activity State 3.                  */
  k_ra8_i3c_ccc_d_setdasa   = 0x87U, /**< Direct Set Dynamic Address from Static Address. */
  k_ra8_i3c_ccc_d_getmwl    = 0x8BU, /**< Direct Get Max Write Length.                    */
  k_ra8_i3c_ccc_d_getmrl    = 0x8CU, /**< Direct Get Max Read Length.                     */
  k_ra8_i3c_ccc_d_getpid    = 0x8DU, /**< Direct Get Provisional ID.                      */
  k_ra8_i3c_ccc_d_getbcr    = 0x8EU, /**< Direct Get Bus Characteristic Register.         */
  k_ra8_i3c_ccc_d_getdcr    = 0x8FU, /**< Direct Get Device Characteristic Register.      */
  k_ra8_i3c_ccc_d_getstatus = 0x90U, /**< Direct Get Device Status.                       */
  k_ra8_i3c_ccc_d_getxtime  = 0x99U, /**< Direct Get Exchange Timing Information.         */
  k_ra8_i3c_ccc_d_rstact    = 0x9AU, /**< Direct Peripheral Reset Action.                 */
  k_ra8_i3c_ccc_direct_mask = 0x80U, /**< Bit set in direct CCC opcodes.                  */
} ra8_i3c_ccc_opcode_t;

/**
 * @typedef ra8_i3c_cmd_desc_bits_t
 * @brief NCMDQP command-descriptor field positions (FSP I3C_CMD_DESC_*).
 *
 * @details
 * The command queue accepts two 32-bit words per descriptor; this
 * enum names every shift/mask used to assemble the first word, and
 * the second-word XFER_LENGTH field for non-immediate transfers.
 * See HUM Ch 40 "Command Descriptor" pp 2445-2701.
 */
typedef enum : uint32_t {
  k_ra8_i3c_cmd_attr_shift        = 0U,       /**< RA8 I3C cmd attr shift.            */
  k_ra8_i3c_cmd_attr_xfer         = 0U,       /**< Regular transfer attribute.        */
  k_ra8_i3c_cmd_attr_immed        = 1U,       /**< Immediate-data transfer attribute. */
  k_ra8_i3c_cmd_attr_addr_assgn   = 2U,       /**< Address-assignment attribute.      */
  k_ra8_i3c_cmd_tid_shift         = 3U,       /**< Transaction ID position.           */
  k_ra8_i3c_cmd_code_shift        = 7U,       /**< CCC opcode position.               */
  k_ra8_i3c_cmd_dev_index_shift   = 16U,      /**< Device-table index.                */
  k_ra8_i3c_cmd_cp_shift          = 15U,      /**< Command Present (CCC) flag.        */
  k_ra8_i3c_cmd_xfer_mode_shift   = 26U,      /**< RA8 I3C cmd xfer mode shift.       */
  k_ra8_i3c_cmd_addr_count_shift  = 26U,      /**< ENTDAA device count.               */
  k_ra8_i3c_cmd_rnw_shift         = 29U,      /**< Read-not-Write direction bit.      */
  k_ra8_i3c_cmd_roc_shift         = 30U,      /**< Response-on-Completion.            */
  k_ra8_i3c_cmd_toc_shift         = 31U,      /**< Terminate-on-Completion (STOP).    */
  k_ra8_i3c_cmd_immed_bytes_shift = 23U,      /**< Immediate byte count [24:23].      */
  k_ra8_i3c_cmd_xfer_length_shift = 16U,      /**< Transfer length [31:16].           */
  k_ra8_i3c_cmd_xfer_length_max   = 0xFFFFUL, /**< RA8 I3C cmd xfer length maximum.   */
} ra8_i3c_cmd_desc_bits_t;

/**
 * @typedef ra8_i3c_ntst_bits_t
 * @brief NTST (Normal Transfer Status) flag positions.
 */
typedef enum : uint32_t {
  k_ra8_i3c_ntst_tdbef0_mask  = 0x00000001UL, /**< Tx Buffer Empty bit 0.       */
  k_ra8_i3c_ntst_rdbff0_mask  = 0x00000002UL, /**< Rx Buffer Full bit 1.        */
  k_ra8_i3c_ntst_ibiqeff_mask = 0x00000004UL, /**< IBI Queue Empty/Full bit 2.  */
  k_ra8_i3c_ntst_cmdqef_mask  = 0x00000008UL, /**< Cmd Queue Empty bit 3.       */
  k_ra8_i3c_ntst_rspqff_mask  = 0x00000010UL, /**< Rsp Queue Full bit 4.        */
  k_ra8_i3c_ntst_rsqff_mask   = 0x00100000UL, /**< Rx Status Queue Full bit 20. */
} ra8_i3c_ntst_bits_t;

/**
 * @typedef ra8_i3c_ibi_status_bits_t
 * @brief IBI status descriptor field positions (FSP I3C_IBI_STATUS_DESC_*).
 *
 * @details
 * The first word read from NIBIQP is an IBI status descriptor
 * carrying length, IBI ID, error flags, and the IBI/HJ/MR type
 * encoded by IBI_ST.  See HUM Ch 40 "IBI Status Descriptor".
 */
typedef enum : uint32_t {
  k_ra8_i3c_ibi_status_length_shift = 0U,           /**< RA8 I3C ibi status length shift. */
  k_ra8_i3c_ibi_status_length_mask  = 0x000000FFUL, /**< RA8 I3C ibi status length mask.  */
  k_ra8_i3c_ibi_status_id_shift     = 8U,           /**< RA8 I3C ibi status ID shift.     */
  k_ra8_i3c_ibi_status_id_mask      = 0x0000FF00UL, /**< RA8 I3C ibi status ID mask.      */
  k_ra8_i3c_ibi_status_last_shift   = 24U,          /**< RA8 I3C ibi status last shift.   */
  k_ra8_i3c_ibi_status_ibi_st_mask  = 0x80000000UL, /**< IBI status type bit 31.          */
} ra8_i3c_ibi_status_bits_t;

/**
 * @typedef ra8_i3c_misc_t
 * @brief Miscellaneous protocol limits.
 */
typedef enum : uint32_t {
  k_ra8_i3c_max_targets           = 8U,    /**< Cap on ENTDAA device-table size.  */
  k_ra8_i3c_pid_bytes             = 6U,    /**< 48-bit Provisional ID.            */
  k_ra8_i3c_setdasa_payload_bytes = 1U,    /**< SETDASA carries one address byte. */
  k_ra8_i3c_immediate_max_bytes   = 4U,    /**< Immediate-data transfer cap.      */
  k_ra8_i3c_addr_mask             = 0x7FU, /**< 7-bit dynamic address mask.       */
} ra8_i3c_misc_t;

/**
 * @typedef ra8_i3c_byte_t
 * @brief Byte-pack helpers used to assemble / disassemble 32-bit FIFO words.
 */
typedef enum : uint32_t {
  k_ra8_i3c_byte_mask            = 0xFFU, /**< Mask covering one byte.     */
  k_ra8_i3c_byte_shift           = 8U,    /**< Bits per byte.              */
  k_ra8_i3c_shift_b1             = 8U,    /**< Shift for byte 1 in a word. */
  k_ra8_i3c_shift_b2             = 16U,   /**< Shift for byte 2 in a word. */
  k_ra8_i3c_shift_b3             = 24U,   /**< Shift for byte 3 in a word. */
  k_ra8_i3c_addr_shift_in_ibi_id = 1U,    /**< IBI ID = (addr << 1) | RnW. */
  k_ra8_i3c_word_size            = 4U,    /**< Bytes per FIFO word.        */
} ra8_i3c_byte_t;

/**
 * @typedef ra8_i3c_offset_t
 * @brief FSP-canonical byte offsets used by the layout asserts below.
 */
typedef enum : uint16_t {
  k_ra8_i3c_off_bctl     = 0x014U, /**< RA8 I3C off bctl.     */
  k_ra8_i3c_off_msdvad   = 0x018U, /**< RA8 I3C off msdvad.   */
  k_ra8_i3c_off_rstctl   = 0x020U, /**< RA8 I3C off rstctl.   */
  k_ra8_i3c_off_inst     = 0x030U, /**< RA8 I3C off inst.     */
  k_ra8_i3c_off_dvct     = 0x044U, /**< RA8 I3C off dvct.     */
  k_ra8_i3c_off_ibinctl  = 0x058U, /**< RA8 I3C off ibinctl.  */
  k_ra8_i3c_off_bfctl    = 0x060U, /**< RA8 I3C off bfctl.    */
  k_ra8_i3c_off_refckctl = 0x070U, /**< RA8 I3C off refckctl. */
  k_ra8_i3c_off_ackctl   = 0x0A0U, /**< RA8 I3C off ackctl.   */
  k_ra8_i3c_off_scstlctl = 0x0B0U, /**< RA8 I3C off scstlctl. */
  k_ra8_i3c_off_ncmdqp   = 0x150U, /**< RA8 I3C off ncmdqp.   */
  k_ra8_i3c_off_nrspqp   = 0x154U, /**< RA8 I3C off nrspqp.   */
  k_ra8_i3c_off_ntdtbp0  = 0x158U, /**< RA8 I3C off ntdtbp0.  */
  k_ra8_i3c_off_nibiqp   = 0x17CU, /**< RA8 I3C off nibiqp.   */
  k_ra8_i3c_off_nrsqp    = 0x180U, /**< RA8 I3C off nrsqp.    */
  k_ra8_i3c_off_ntst     = 0x1E0U, /**< RA8 I3C off ntst.     */
} ra8_i3c_offset_t;

/* Compile-time guards: every register that the I3C driver reads or
 * writes must sit at the FSP-canonical offset.  The constants on the
 * RHS come from the RA8D2 CMSIS device header
 * ``R7KA8D2KF_core0.h::R_I3C0_Type``. */
static_assert(offsetof(r_i3c_regs_t, INST) == (size_t)k_ra8_i3c_off_inst, "INST offset");
static_assert(offsetof(r_i3c_regs_t, MSDVAD) == (size_t)k_ra8_i3c_off_msdvad, "MSDVAD offset");
static_assert(offsetof(r_i3c_regs_t, BCTL) == (size_t)k_ra8_i3c_off_bctl, "BCTL offset");
static_assert(offsetof(r_i3c_regs_t, RSTCTL) == (size_t)k_ra8_i3c_off_rstctl, "RSTCTL offset");
static_assert(offsetof(r_i3c_regs_t, DVCT) == (size_t)k_ra8_i3c_off_dvct, "DVCT offset");
static_assert(offsetof(r_i3c_regs_t, IBINCTL) == (size_t)k_ra8_i3c_off_ibinctl, "IBINCTL offset");
static_assert(offsetof(r_i3c_regs_t, BFCTL) == (size_t)k_ra8_i3c_off_bfctl, "BFCTL offset");
static_assert(offsetof(r_i3c_regs_t, REFCKCTL) == (size_t)k_ra8_i3c_off_refckctl,
              "REFCKCTL offset");
static_assert(offsetof(r_i3c_regs_t, ACKCTL) == (size_t)k_ra8_i3c_off_ackctl, "ACKCTL offset");
static_assert(offsetof(r_i3c_regs_t, SCSTLCTL) == (size_t)k_ra8_i3c_off_scstlctl,
              "SCSTLCTL offset");
static_assert(offsetof(r_i3c_regs_t, NCMDQP) == (size_t)k_ra8_i3c_off_ncmdqp, "NCMDQP offset");
static_assert(offsetof(r_i3c_regs_t, NRSPQP) == (size_t)k_ra8_i3c_off_nrspqp, "NRSPQP offset");
static_assert(offsetof(r_i3c_regs_t, NTDTBP0) == (size_t)k_ra8_i3c_off_ntdtbp0, "NTDTBP0 offset");
static_assert(offsetof(r_i3c_regs_t, NIBIQP) == (size_t)k_ra8_i3c_off_nibiqp, "NIBIQP offset");
static_assert(offsetof(r_i3c_regs_t, NRSQP) == (size_t)k_ra8_i3c_off_nrsqp, "NRSQP offset");
static_assert(offsetof(r_i3c_regs_t, NTST) == (size_t)k_ra8_i3c_off_ntst, "NTST offset");

/**
 * @brief Get pointer to I3C0.
 * @return Volatile pointer to instance 0 register window.
 */
RA8_HW_REGISTER_ACCESS
static inline volatile r_i3c_regs_t* ra8_i3c(void)
{
  return (volatile r_i3c_regs_t*)k_ra8_i3c0_base_addr;
}

#ifdef __cplusplus
}
#endif
