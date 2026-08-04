/**
 * @file ra8_ether_regs.h
 * @brief Ethernet controller base addresses for the Renesas RA8D2
 * @ingroup grp_hal_net
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

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"

typedef enum : uintptr_t {
  k_ra8_etha0_base_addr = 0x403CA000UL, /**< RA8 etha0 base address. */
  k_ra8_etha1_base_addr = 0x403CC000UL, /**< RA8 etha1 base address. */
  k_ra8_rmac0_base_addr = 0x403CB000UL, /**< RA8 rmac0 base address. */
  k_ra8_rmac1_base_addr = 0x403CD000UL, /**< RA8 rmac1 base address. */
  k_ra8_mfwd_base_addr  = 0x403C0000UL, /**< RA8 mfwd base address.  */
  k_ra8_coma_base_addr  = 0x403C9000UL, /**< RA8 coma base address.  */
  k_ra8_gptp_base_addr  = 0x403E0000UL, /**< RA8 gptp base address.  */
  k_ra8_eswm_base_addr  = 0x403C8000UL, /**< RA8 eswm base address.  */
  k_ra8_gwca0_base_addr = 0x403CE000UL, /**< RA8 gwca0 base address. */
} ra8_ether_addr_t;

/**
 * @enum ra8_coma_offset_t
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
  k_ra8_coma_off_ric     = 0x000U, /**< Interrupt Configuration.                             */
  k_ra8_coma_off_rrc     = 0x004U, /**< Reset Configuration (RR bit 0).                      */
  k_ra8_coma_off_rcec    = 0x008U, /**< Clock Enable Cfg (RCE bit 16).                       */
  k_ra8_coma_off_cabpirm = 0x140U, /**< Buf-pool Init/Reset (BPIOG / BPR). HUM Ch 31 p 1599. */
} ra8_coma_offset_t;

/**
 * @enum ra8_coma_bit_t
 * @brief Bit positions in the COMA bring-up registers.
 */
typedef enum : uint32_t {
  k_ra8_coma_rrc_rr        = (1UL << 0U),  /**< RRC.RR -- pulse to reset ESWM IP.    */
  k_ra8_coma_rcec_rce      = (1UL << 16U), /**< RCEC.RCE -- enable switch clock.     */
  k_ra8_coma_rcec_ace_mask = 0x7FUL,       /**< RCEC.ACE[6:0] -- agent clock enable. */
  k_ra8_coma_cabpirm_bpiog = (1UL << 0U),  /**< CABPIRM.BPIOG -- buf-pool init kick. */
  k_ra8_coma_cabpirm_bpr   = (1UL << 1U),  /**< CABPIRM.BPR -- buf-pool ready flag.  */
} ra8_coma_bit_t;

/** @brief Get pointer to the 32-bit COMA.RRC (Reset Configuration). */
RA8_HW_REGISTER_ACCESS
static inline volatile uint32_t* ra8_coma_rrc(void)
{
  return (volatile uint32_t*)(k_ra8_coma_base_addr + (uintptr_t)k_ra8_coma_off_rrc);
}

/** @brief Get pointer to the 32-bit COMA.RCEC (Clock Enable Configuration). */
RA8_HW_REGISTER_ACCESS
static inline volatile uint32_t* ra8_coma_rcec(void)
{
  return (volatile uint32_t*)(k_ra8_coma_base_addr + (uintptr_t)k_ra8_coma_off_rcec);
}

/** @brief Get pointer to the 32-bit COMA.CABPIRM (Buf-pool Init/Reset/Monitor). */
RA8_HW_REGISTER_ACCESS
static inline volatile uint32_t* ra8_coma_cabpirm(void)
{
  return (volatile uint32_t*)(k_ra8_coma_base_addr + (uintptr_t)k_ra8_coma_off_cabpirm);
}

/**
 * @struct r_eswm_regs_t
 * @brief Minimal Ethernet Switch Module (ESWM) register window.
 *
 * @details
 * cppcheck cannot see tests/ so it flags every field as unused;
 * each member is accessed via ``ra8_eswm()`` in
 * ``libs/ra8_hal/src/ra8_eth.c``.
 */
typedef struct {
  volatile uint32_t ESWM_CTRL; /**< +0x00 Control.          */
  volatile uint32_t ESWM_STS;  /**< +0x04 Status.           */
  volatile uint32_t ESWM_IE;   /**< +0x08 Interrupt Enable. */
  volatile uint32_t ESWM_ICLR; /**< +0x0C Interrupt Clear.  */
} r_eswm_regs_t;

/** @brief Get pointer to the ESWM register block. */
RA8_HW_REGISTER_ACCESS
static inline volatile r_eswm_regs_t* ra8_eswm(void)
{
  return (volatile r_eswm_regs_t*)k_ra8_eswm_base_addr;
}

/**
 * @enum ra8_eswm_mii_offset_t
 * @brief Offsets into the ESWM Media-Interface configuration window.
 *
 * @details
 * HUM Ch 29 (ESWM) -- the per-port media-interface registers live far
 * past the CTRL/STS block. Cross-checked against FSP CMSIS header
 * R7KA8D2KF_core0.h (R_ESWM_Type members MIIRR and MIICR0/1 at the
 * offsets below). Used by the RMAC PHY bring-up path to:
 *   1. Select RGMII vs RMII vs MII for each MAC port.
 *   2. Enable the per-port media-interface block (MIIRR.RGRST/RMRST).
 *
 * Without MIIRR's per-port enable bit set, the media interface block
 * stays in reset -- no TXC, no MDC, no RGMII clocking.
 */
typedef enum : uint32_t {
  k_ra8_eswm_off_miirr  = 0x19400UL, /**< Media Interface Reset Register.    */
  k_ra8_eswm_off_miicr0 = 0x19404UL, /**< Media Interface Control 0 (RMAC0). */
  k_ra8_eswm_off_miicr1 = 0x19408UL, /**< Media Interface Control 1 (RMAC1). */
} ra8_eswm_mii_offset_t;

/**
 * @enum ra8_eswm_miirr_bit_t
 * @brief Bit positions in ESWM.MIIRR (Media Interface Reset Register).
 *
 * @details HUM Ch 29.2.1.2 "MIIRR" p 1289 -- each RGRSTn / RMRSTn bit
 * is an ENABLE, not an active-high reset: 0 = Reset (block held in
 * reset), 1 = Enable (block operational). Set the bit to bring the
 * media-interface block out of reset.
 */
typedef enum : uint32_t {
  k_ra8_eswm_miirr_rgrst0 = (1UL << 0U), /**< RGMII0 enable (1 = enable, 0 = reset). */
  k_ra8_eswm_miirr_rgrst1 = (1UL << 1U), /**< RGMII1 enable (1 = enable, 0 = reset). */
  k_ra8_eswm_miirr_rmrst0 = (1UL << 8U), /**< RMII0 enable (1 = enable, 0 = reset).  */
  k_ra8_eswm_miirr_rmrst1 = (1UL << 9U), /**< RMII1 enable (1 = enable, 0 = reset).  */
} ra8_eswm_miirr_bit_t;

/**
 * @enum ra8_eswm_miicr_field_t
 * @brief Field encodings for ESWM.MIICRn (Media Interface Control n).
 *
 * @details
 * MIISEL[1:0] selects the MII flavour driven onto the n-th MAC pin
 * group: 0 = MII/GMII, 1 = RGMII, 2 = RMII. TXCIDE (bit 12) enables
 * the on-chip TXC internal delay for RGMII (matches the FSP default
 * "RGMII + TX skew" board profile).
 */
typedef enum : uint32_t {
  k_ra8_eswm_miicr_miisel_mii   = 0UL,          /**< MIISEL = MII/GMII.            */
  k_ra8_eswm_miicr_miisel_rgmii = 1UL,          /**< MIISEL = RGMII.               */
  k_ra8_eswm_miicr_miisel_rmii  = 2UL,          /**< MIISEL = RMII.                */
  k_ra8_eswm_miicr_miisel_mask  = 0x3UL,        /**< MIISEL field mask (bits 1:0). */
  k_ra8_eswm_miicr_divstp       = (1UL << 8U),  /**< DIVSTP -- gate the clock div. */
  k_ra8_eswm_miicr_txcide       = (1UL << 12U), /**< TXCIDE -- TXC internal delay. */
} ra8_eswm_miicr_field_t;

/** @brief Get pointer to ESWM.MIIRR (32-bit). */
RA8_HW_REGISTER_ACCESS
static inline volatile uint32_t* ra8_eswm_miirr(void)
{
  return (volatile uint32_t*)(k_ra8_eswm_base_addr + (uintptr_t)k_ra8_eswm_off_miirr);
}

/** @brief Get pointer to ESWM.MIICR0 (32-bit, RMAC port 0). */
RA8_HW_REGISTER_ACCESS
static inline volatile uint32_t* ra8_eswm_miicr0(void)
{
  return (volatile uint32_t*)(k_ra8_eswm_base_addr + (uintptr_t)k_ra8_eswm_off_miicr0);
}

/** @brief Get pointer to ESWM.MIICR1 (32-bit, RMAC port 1). */
RA8_HW_REGISTER_ACCESS
static inline volatile uint32_t* ra8_eswm_miicr1(void)
{
  return (volatile uint32_t*)(k_ra8_eswm_base_addr + (uintptr_t)k_ra8_eswm_off_miicr1);
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
typedef struct {
  volatile uint32_t MFWD_CTRL; /**< +0x00 Forwarding Control. */
  volatile uint32_t MFWD_STS;  /**< +0x04 Status.             */
  volatile uint32_t MFWD_IE;   /**< +0x08 Interrupt Enable.   */
  volatile uint32_t MFWD_ICLR; /**< +0x0C Interrupt Clear.    */
} r_mfwd_regs_t;

/** @brief Get pointer to the MFWD register block. */
RA8_HW_REGISTER_ACCESS
static inline volatile r_mfwd_regs_t* ra8_mfwd(void)
{
  return (volatile r_mfwd_regs_t*)k_ra8_mfwd_base_addr;
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
typedef struct {
  volatile uint32_t COMA_CTRL; /**< +0x00 Common Agent Control. */
  volatile uint32_t COMA_STS;  /**< +0x04 Status.               */
  volatile uint32_t COMA_IE;   /**< +0x08 Interrupt Enable.     */
  volatile uint32_t COMA_ICLR; /**< +0x0C Interrupt Clear.      */
} r_coma_regs_t;

/** @brief Get pointer to the COMA register block. */
RA8_HW_REGISTER_ACCESS
static inline volatile r_coma_regs_t* ra8_coma(void)
{
  /* COMA shares the MFWD window in the documented layout; if a
   * future revision moves it out, the linker constant above
   * needs a separate k_ra8_coma_base_addr entry. */
  return (volatile r_coma_regs_t*)k_ra8_mfwd_base_addr;
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
 *
 * @warning Field names here pre-date the FSP R_GWCA0_Type reference
 * (cloned from github.com/renesas/fsp). The first 16 bytes
 * actually map to:
 *   - +0x00: GWMC (Mode Configuration -- OPC bits drive the
 *            ESWM state machine: 00=RESET, 01=DISABLE, 10=CONFIG,
 *            11=OPERATION; HUM Ch 34.3.1)
 *   - +0x04: GWMS (Mode Status -- mirror of OPC)
 *   - +0x08-0x0C: reserved
 * The "GWCA_CTRL/STS/IE/ICLR" names below are inherited from the
 * RA6 EDMAC port and are misnamed for RA8D2 ESWM. The +0x08/+0x0C
 * fields are NOT separate "IE/ICLR" registers; do not rely on
 * them. Keep the legacy names for source compatibility with the
 * existing ra8_eth_gwca.c API until the full port lands. The real
 * register set lives in r_gwca_regs_full_t below.
 */
typedef struct {
  volatile uint32_t GWCA_CTRL; /**< +0x00 GWMC (Mode Configuration) -- misnamed. */
  volatile uint32_t GWCA_STS;  /**< +0x04 GWMS (Mode Status) -- misnamed.        */
  volatile uint32_t GWCA_IE;   /**< +0x08 reserved -- misnamed as "IE".          */
  volatile uint32_t GWCA_ICLR; /**< +0x0C reserved -- misnamed as "ICLR".        */
} r_gwca_regs_t;

/** @brief Get pointer to the GWCA register block. */
RA8_HW_REGISTER_ACCESS
static inline volatile r_gwca_regs_t* ra8_gwca(void)
{
  return (volatile r_gwca_regs_t*)k_ra8_gwca0_base_addr;
}

/**
 * @enum ra8_gwmc_opc_t
 * @brief GWMC.OPC operation-mode values (HUM Ch 34.3.1 "GWMC" p 1797).
 *
 * @details The ESWM state machine accepts mode transitions written
 * here. The FSP-named LAYER3_SWITCH_AGENT_MODE_xxx enum maps 1:1.
 * Writing the new mode and polling GWMS.OPS until it reflects the
 * value is the "transition request" protocol used by the FSP
 * r_layer3_switch driver.
 */
typedef enum : uint32_t {
  k_ra8_gwmc_opc_reset     = 0x0U, /**< RR: Reset request.                           */
  k_ra8_gwmc_opc_disable   = 0x1U, /**< DR: Disable request.                         */
  k_ra8_gwmc_opc_config    = 0x2U, /**< CR: Config request -- LINKFIX writable here. */
  k_ra8_gwmc_opc_operation = 0x3U, /**< OR: Operation request -- frames flow.        */
  k_ra8_gwmc_opc_mask      = 0x3U, /**< RA8 gwmc opc mask.                           */
} ra8_gwmc_opc_t;

/**
 * @enum ra8_gwdcc_dt_t
 * @brief Descriptor type field values (HUM Ch 34.5.1.2.2 "Descriptor Types").
 *
 * @details Set on the 4-bit DT field of every basic descriptor. See
 * `layer3_switch_basic_descriptor_t` below.
 */
typedef enum : uint8_t {
  k_ra8_gwdcc_dt_linkfix   = 0U,  /**< LINKFIX  -- chain head pointer.        */
  k_ra8_gwdcc_dt_fempty_is = 1U,  /**< FEMPTY_IS -- empty, incremental start. */
  k_ra8_gwdcc_dt_fempty_ic = 2U,  /**< FEMPTY_IC -- empty, incremental cont.  */
  k_ra8_gwdcc_dt_fempty_nd = 3U,  /**< FEMPTY_ND -- reject data.              */
  k_ra8_gwdcc_dt_fempty    = 4U,  /**< FEMPTY    -- empty, full frame slot.   */
  k_ra8_gwdcc_dt_fsingle   = 8U,  /**< FSINGLE   -- single-fragment frame.    */
  k_ra8_gwdcc_dt_fstart    = 9U,  /**< FSTART    -- multi-fragment start.     */
  k_ra8_gwdcc_dt_fmid      = 10U, /**< FMID      -- multi-fragment middle.    */
  k_ra8_gwdcc_dt_fend      = 11U, /**< FEND      -- multi-fragment end.       */
  k_ra8_gwdcc_dt_lempty    = 12U, /**< LEMPTY    -- queue disabled.           */
  k_ra8_gwdcc_dt_eempty    = 13U, /**< EEMPTY    -- TX queue paused.          */
  k_ra8_gwdcc_dt_link      = 14U, /**< LINK      -- chain continuation.       */
  k_ra8_gwdcc_dt_eos       = 15U, /**< EOS       -- end-of-set.               */
} ra8_gwdcc_dt_t;

/**
 * @struct ra8_gwca_basic_descriptor_t
 * @brief 8-byte basic descriptor (HUM Ch 34.5.1.2 "General Formats").
 *
 * @details Layout matches the FSP `layer3_switch_basic_descriptor_t`
 * (r_layer3_switch.h). All fields are little-endian on the LE
 * targets we support (Cortex-M85 + host x86_64). The chip walks
 * arrays of these; LINKFIX/LINK entries also use this format with
 * dt set to k_ra8_gwdcc_dt_linkfix or k_ra8_gwdcc_dt_link.
 */
typedef struct {
  volatile uint8_t  ds_l;      /**< 0..7   Descriptor size (low byte).       */
  volatile uint8_t  ds_h : 4;  /**< 8..11  Descriptor size (high nibble).    */
  volatile uint8_t  info0 : 4; /**< 12..15 Information 0.                    */
  volatile uint8_t  err : 3;   /**< 16..18 Error bits (data/AXI errors).     */
  volatile uint8_t  die : 1;   /**< 19     Descriptor interrupt enable.      */
  volatile uint8_t  dt : 4;    /**< 20..23 Descriptor type (ra8_gwdcc_dt_t). */
  volatile uint8_t  ptr_h;     /**< 24..31 Pointer high byte (PTR[39:32]).   */
  volatile uint32_t ptr_l;     /**< 32..63 Pointer low 32 bits.              */
} ra8_gwca_basic_descriptor_t;

static_assert(sizeof(ra8_gwca_basic_descriptor_t) == 8U,
              "GWCA basic descriptor must be 8 bytes (HUM Ch 34.5.1.2.1).");

/**
 * @struct ra8_gwca_ext_descriptor_t
 * @brief 16-byte extended descriptor (HUM Ch 34.5.1.2 "Extended
 *        descriptor", GWDCCi.ETS == 0 && GWDCCi.EDE == 1).
 *
 * @details Bytes 0..7 are the basic descriptor (identical layout to
 * ::ra8_gwca_basic_descriptor_t -- DS / INFO0 / DT / PTR). Bytes 8..15
 * are INFO1[63:0]. On a TX queue INFO1 carries the transmit metadata
 * (matches FSP `layer3_switch_descriptor_t` `info1_tx`): the
 * descriptor format bit and the destination vector that route the
 * frame. The two INFO1 words are exposed as plain ``uint32_t`` and
 * the named fields are composed via ::ra8_gwca_info1_tx_t so the
 * volatile bit-field ABI never varies between toolchains.
 *
 * @see ra8_gwca_info1_tx_t
 */
typedef struct {
  volatile uint8_t  ds_l;      /**< 0..7   Descriptor size (low byte).       */
  volatile uint8_t  ds_h : 4;  /**< 8..11  Descriptor size (high nibble).    */
  volatile uint8_t  info0 : 4; /**< 12..15 Information 0.                    */
  volatile uint8_t  err : 3;   /**< 16..18 Error bits (data/AXI errors).     */
  volatile uint8_t  die : 1;   /**< 19     Descriptor interrupt enable.      */
  volatile uint8_t  dt : 4;    /**< 20..23 Descriptor type (ra8_gwdcc_dt_t). */
  volatile uint8_t  ptr_h;     /**< 24..31 Pointer high byte (PTR[39:32]).   */
  volatile uint32_t ptr_l;     /**< 32..63 Pointer low 32 bits.              */
  volatile uint32_t info1_lo;  /**< 64..95  INFO1[31:0].                     */
  volatile uint32_t info1_hi;  /**< 96..127 INFO1[63:32].                    */
} ra8_gwca_ext_descriptor_t;

static_assert(sizeof(ra8_gwca_ext_descriptor_t) == 16U,
              "GWCA extended descriptor must be 16 bytes (HUM Ch 34.5.1.2.1).");

/**
 * @enum ra8_gwca_info1_tx_t
 * @brief Bit composition of the INFO1 half of a TX extended descriptor.
 *
 * @details HUM Ch 34.5.3 + FSP `st_info1_tx`. INFO1[63:0] spans
 * ::ra8_gwca_ext_descriptor_t info1_lo (INFO1[31:0]) and info1_hi
 * (INFO1[63:32]). The two fields the TX path programs:
 *  - FMT  (INFO1 bit 2, in info1_lo): 0 = ethernet descriptor (the
 *    MFWD forwarding engine routes by destination MAC), 1 = direct
 *    descriptor (the DV field routes the frame, forwarding bypassed).
 *  - DV[6:0] (INFO1 bits 48..54, in info1_hi bits 22..16): the
 *    destination-port bit vector, used only when FMT = direct.
 * FI (INFO1 bit 0) stays 0 so the RMAC appends the FCS.
 */
typedef enum : uint32_t {
  k_ra8_gwca_info1_tx_fmt_direct = (1UL << 2U),   /**< info1_lo: FMT = direct descriptor. */
  k_ra8_gwca_info1_tx_dv_shift   = 16U,           /**< info1_hi: DV[6:0] field shift.     */
  k_ra8_gwca_info1_tx_dv_mask    = 0x7FUL << 16U, /**< info1_hi: DV[6:0] field mask.      */
} ra8_gwca_info1_tx_t;

/**
 * @enum ra8_gwca_offset_t
 * @brief Offsets into R_GWCA0 for the registers the upcoming port needs.
 *
 * @details Subset of the full R_GWCA0_Type layout from the FSP
 * `R7KA8D2KF_core0.h` CMSIS header. Reading these via the existing
 * r_gwca_regs_t legacy view is unsafe (the struct only covers the
 * first 16 bytes); use explicit pointer arithmetic from
 * k_ra8_gwca0_base_addr + offset until the full struct lands.
 *
 * @see reference_fsp_source memory note for the full register list.
 */
typedef enum : uint16_t {
  k_ra8_gwca_off_gwmc       = 0x0000U, /**< Mode Configuration.                                 */
  k_ra8_gwca_off_gwms       = 0x0004U, /**< Mode Status.                                        */
  k_ra8_gwca_off_gwdcbac0   = 0x0194U, /**< Descriptor chain base addr 0 (upper).               */
  k_ra8_gwca_off_gwdcbac1   = 0x0198U, /**< Descriptor chain base addr 1 (lower).               */
  k_ra8_gwca_off_gwtrc0     = 0x0200U, /**< TX Request Cfg, queues 0..31.                       */
  k_ra8_gwca_off_gwtrc1     = 0x0204U, /**< TX Request Cfg, queues 32..63.                      */
  k_ra8_gwca_off_gwarirm    = 0x0380U, /**< AXI RAM Init Req Monitoring (FSP-confirmed offset). */
  k_ra8_gwca_off_gwdcc_base = 0x0400U, /**< GWDCC[0]; stride 4 bytes.                           */
  k_ra8_gwca_off_gwaarss    = 0x0800U, /**< AXI Addr RAM Searching Setting.                     */
  k_ra8_gwca_off_gwaarsr0   = 0x0804U, /**< AXI Addr RAM Searching Result0.                     */
  k_ra8_gwca_off_gwaarsr1   = 0x0808U, /**< AXI Addr RAM Searching Result1.                     */
} ra8_gwca_offset_t;

/**
 * @enum ra8_gwdcc_bits_t
 * @brief Bit positions/masks for GWDCC[i] (HUM Ch 34 + FSP CMSIS header).
 *
 * @details Per-queue Descriptor Chain Configuration. SM[1:0] is the
 * Synchronization Mode (00b = Normal / full descriptor write-back,
 * 01b = No-write-back) -- always left 0, so it has no named constant
 * here. EDE / ETS toggle extended descriptors; SL selects a
 * "stop-on-last" mode; DQT picks TX (1) vs RX (0); DCP carries the
 * 3-bit class priority; BALR (Base Address Load Request) reloads the
 * AXI address RAM current_address to the chain base; OSID is the
 * outstanding-transaction stream ID.
 */
typedef enum : uint32_t {
  k_ra8_gwdcc_ede        = 1UL << 8U,    /**< RA8 gwdcc ede.              */
  k_ra8_gwdcc_ets        = 1UL << 9U,    /**< RA8 gwdcc ets.              */
  k_ra8_gwdcc_sl         = 1UL << 10U,   /**< RA8 gwdcc sl.               */
  k_ra8_gwdcc_dqt        = 1UL << 11U,   /**< 0 = RX queue, 1 = TX queue. */
  k_ra8_gwdcc_dcp_shift  = 16U,          /**< RA8 gwdcc dcp shift.        */
  k_ra8_gwdcc_dcp_mask   = 0x7UL << 16U, /**< RA8 gwdcc dcp mask.         */
  k_ra8_gwdcc_balr       = 1UL << 24U,   /**< RA8 gwdcc balr.             */
  k_ra8_gwdcc_osid_shift = 28U,          /**< RA8 gwdcc osid shift.       */
  k_ra8_gwdcc_osid_mask  = 0x7UL << 28U, /**< RA8 gwdcc osid mask.        */
} ra8_gwdcc_bits_t;

/**
 * @brief Get pointer to GWDCC[queue_index] (per-queue descriptor chain cfg).
 *
 * @param[in] queue_index Queue number 0..31.
 * @return Volatile pointer to GWDCC[queue_index], or nullptr when
 *         the index is out of range.
 */
RA8_HW_REGISTER_ACCESS
static inline volatile uint32_t* ra8_gwca_gwdcc(uint32_t queue_index)
{
  /* cppcheck-suppress unusedScopedObject ; `enum : uint32_t` is a C23 type
     declaration, not an object. cppcheck reads the underlying type as a
     temporary and calls it destroyed immediately; the constant is read on
     the next line. Only bites when this header is checked standalone. */
  enum : uint32_t { k_ra8_gwca_max_queues = 32U /**< RA8 gwca maximum queues. */ };
  if (queue_index >= k_ra8_gwca_max_queues) {
    return nullptr;
  }
  return (volatile uint32_t*)(k_ra8_gwca0_base_addr + (uintptr_t)k_ra8_gwca_off_gwdcc_base +
                              ((uintptr_t)queue_index * sizeof(uint32_t)));
}

/**
 * @enum ra8_gptp_timer_idx_t
 * @brief GPTP timer instance index (HUM Table 35.3 lists `t = 0, 1`).
 *
 * @details
 * The GPTP block instantiates two independent timer units. Every
 * per-timer register is placed at `base + 0x0020 + 0x40 * t`, which is
 * exactly the stride of ::r_gptp_timer_regs_t.
 *
 * @invariant A timer index is always `< k_ra8_gptp_timer_count`.
 *
 * @par Example:
 * @code
 * volatile r_gptp_timer_regs_t* t0 = &ra8_gptp()->TIMER[k_ra8_gptp_timer_0];
 * @endcode
 *
 * @see r_gptp_timer_regs_t
 */
typedef enum : uint8_t {
  k_ra8_gptp_timer_0     = 0U, /**< Timer unit 0 (registers at +0x0020). */
  k_ra8_gptp_timer_1     = 1U, /**< Timer unit 1 (registers at +0x0060). */
  k_ra8_gptp_timer_count = 2U, /**< Number of timer units on this part.  */
} ra8_gptp_timer_idx_t;

/**
 * @enum ra8_gptp_layout_t
 * @brief Byte offsets that ::r_gptp_regs_t / ::r_gptp_timer_regs_t must hit.
 *
 * @details
 * Mirrors HUM Table 35.3 "gPTP timer registers" p 1926 so the
 * `static_assert`s below cannot drift from the manual. Offsets marked
 * `_t` are relative to a single timer block; the rest are absolute
 * within the GPTP window.
 *
 * @invariant Every value is a multiple of 4 (all GPTP registers are 32 bit).
 *
 * @par Example:
 * @code
 * static_assert(offsetof(r_gptp_regs_t, PTPTMEC) == k_ra8_gptp_off_ptptmec, "");
 * @endcode
 *
 * @see r_gptp_regs_t
 */
typedef enum : uint8_t {
  k_ra8_gptp_off_ptpipv       = 0x0000U, /**< PTPIPV     IP Version.             */
  k_ra8_gptp_off_ptptmec      = 0x0010U, /**< PTPTMEC    Timer Enable Cfg.       */
  k_ra8_gptp_off_ptptmdc      = 0x0014U, /**< PTPTMDC    Timer Disable Cfg.      */
  k_ra8_gptp_off_timer0       = 0x0020U, /**< First per-timer block.             */
  k_ra8_gptp_timer_stride     = 0x0040U, /**< Per-timer block stride (0x40 x t). */
  k_ra8_gptp_off_t_ptptivc    = 0x0000U, /**< PTPTIVCt   in-block offset.        */
  k_ra8_gptp_off_t_ptptovcl   = 0x0010U, /**< PTPTOVCtL  in-block offset.        */
  k_ra8_gptp_off_t_ptptovcm   = 0x0014U, /**< PTPTOVCtM  in-block offset.        */
  k_ra8_gptp_off_t_ptptovcu   = 0x0018U, /**< PTPTOVCtU  in-block offset.        */
  k_ra8_gptp_off_t_ptpavtptml = 0x0020U, /**< PTPAVTPTMtL in-block offset.       */
  k_ra8_gptp_off_t_ptpavtptmu = 0x0024U, /**< PTPAVTPTMtU in-block offset.       */
  k_ra8_gptp_off_t_ptpgptptml = 0x0030U, /**< PTPGPTPTMtL in-block offset.       */
  k_ra8_gptp_off_t_ptpgptptmm = 0x0034U, /**< PTPGPTPTMtM in-block offset.       */
  k_ra8_gptp_off_t_ptpgptptmu = 0x0038U, /**< PTPGPTPTMtU in-block offset.       */
} ra8_gptp_layout_t;

/**
 * @struct r_gptp_timer_regs_t
 * @brief One GPTP timer unit's register block (HUM Ch 35.3.2 / 35.3.3).
 *
 * @details
 * The block repeats every ::k_ra8_gptp_timer_stride bytes starting at
 * ::k_ra8_gptp_off_timer0, matching the `+ 0x40 x t` addressing of HUM
 * Table 35.3 p 1926. Reserved gaps are spelled out so the members always
 * line up with the manual.
 *
 * The unit holds three views of the same counter: the 78-bit GPTP time
 * (`{PTPGPTPTMtU, PTPGPTPTMtM, PTPGPTPTMtL}`, seconds in bits [77:30]
 * and nanoseconds in [29:0]), and the 64-/32-bit AVTP nanosecond views
 * derived from it (HUM 35.5.1 p 1951). `PTPTIVCt` sets the per-clk
 * increment and `{PTPTOVCtU, PTPTOVCtM, PTPTOVCtL}` the additive offset.
 *
 * @invariant Reading `PTPGPTPTMtL` latches `PTPGPTPTMtM` / `PTPGPTPTMtU`,
 *            so an ordered read of L, M, U is atomic (HUM 35.4.1.3.4 p 1946).
 * @invariant Writing `PTPTOVCtL` commits the whole 78-bit offset, so U and M
 *            must be written first (HUM 35.4.1.3.1 p 1944-1945).
 *
 * @par Example:
 * @code
 * volatile r_gptp_timer_regs_t* t = &ra8_gptp()->TIMER[k_ra8_gptp_timer_0];
 * const uint32_t ns = t->PTPGPTPTML;
 * @endcode
 *
 * @see r_gptp_regs_t
 */
typedef struct {
  volatile uint32_t PTPTIVC;       /**< +0x00 Timer Increment Value Cfg. */
  volatile uint32_t reserved04[3]; /**< +0x04..0x0C Reserved.            */
  volatile uint32_t PTPTOVCL;      /**< +0x10 Timer Offset Value Cfg L.  */
  volatile uint32_t PTPTOVCM;      /**< +0x14 Timer Offset Value Cfg M.  */
  volatile uint32_t PTPTOVCU;      /**< +0x18 Timer Offset Value Cfg U.  */
  volatile uint32_t reserved1c;    /**< +0x1C Reserved.                  */
  volatile uint32_t PTPAVTPTML;    /**< +0x20 AVTP Timer Monitoring L.   */
  volatile uint32_t PTPAVTPTMU;    /**< +0x24 AVTP Timer Monitoring U.   */
  volatile uint32_t reserved28[2]; /**< +0x28..0x2C Reserved.            */
  volatile uint32_t PTPGPTPTML;    /**< +0x30 GPTP Timer Monitoring L.   */
  volatile uint32_t PTPGPTPTMM;    /**< +0x34 GPTP Timer Monitoring M.   */
  volatile uint32_t PTPGPTPTMU;    /**< +0x38 GPTP Timer Monitoring U.   */
  volatile uint32_t reserved3c;    /**< +0x3C Reserved.                  */
} r_gptp_timer_regs_t;

/**
 * @struct r_gptp_regs_t
 * @brief Ethernet Generic PTP Timer (GPTP) register window, HUM Ch 35.
 *
 * @details
 * Declares the timer surface of HUM Table 35.3 p 1926 -- the only part of
 * the block this HAL addresses. The same window also carries media-clock
 * capture (`PTPMCCCm`, +0x0200), media-clock recovery (`PTPMCRCm`, +0x0300),
 * media-clock pin mapping (`PTPMCPCm`, +0x0400), cyclic compare
 * (`PTPCCCc0/1`, +0x0500), the media-clock interrupt registers
 * (`PTPIS0/IE0/ID0` +0x0700, `PTPIS1/IE1/ID1` +0x0710), security
 * configuration (`PTPSCR0/1/2`, +0x0780) and the pulse-output timer
 * submodule (`POTCFGR` and friends, +0x1000, HUM Table 35.9 p 1954).
 * Those need the MEDIA_IN / MEDIA_OUT / CYCLIC_COMP pins, which no driver
 * or board file in this tree routes, so they are deliberately not declared
 * here rather than declared and left unused.
 *
 * **This block is a timer, not a protocol engine.** HUM Ch 35 defines no
 * Sync / Announce / Follow_Up generator, no PTP domain, no clockIdentity
 * and no BMCA: an IEEE 1588 / 802.1AS stack on this part is software above
 * the HAL, fed by this counter and by the RMAC receive-timestamp capture
 * (`MTRC` / `MPFCt`, HUM Ch 33).
 *
 * @invariant While `PTPTMEC.TEq` is 0 the corresponding timer's counters
 *            read 0 (HUM 35.3.2.1 p 1927).
 *
 * @par Example:
 * @code
 * volatile r_gptp_regs_t* g = ra8_gptp();
 * const uint32_t ipv = g->PTPIPV;
 * @endcode
 *
 * @see r_gptp_timer_regs_t
 */
typedef struct {
  volatile uint32_t PTPIPV;        /**< +0x0000 IP Version (read-only).      */
  volatile uint32_t reserved04[3]; /**< +0x0004..0x000C Reserved.            */
  volatile uint32_t PTPTMEC;       /**< +0x0010 Timer Enable Configuration.  */
  volatile uint32_t PTPTMDC;       /**< +0x0014 Timer Disable Configuration. */
  volatile uint32_t reserved18[2]; /**< +0x0018..0x001C Reserved.            */
  volatile r_gptp_timer_regs_t
    TIMER[k_ra8_gptp_timer_count]; /**< +0x0020 + 0x40 x t Per-timer blocks. */
} r_gptp_regs_t;

static_assert(sizeof(r_gptp_timer_regs_t) == (size_t)k_ra8_gptp_timer_stride,
              "GPTP per-timer block must be 0x40 bytes (HUM Table 35.3)");
static_assert(offsetof(r_gptp_timer_regs_t, PTPTIVC) == (size_t)k_ra8_gptp_off_t_ptptivc,
              "PTPTIVCt offset");
static_assert(offsetof(r_gptp_timer_regs_t, PTPTOVCL) == (size_t)k_ra8_gptp_off_t_ptptovcl,
              "PTPTOVCtL offset");
static_assert(offsetof(r_gptp_timer_regs_t, PTPTOVCM) == (size_t)k_ra8_gptp_off_t_ptptovcm,
              "PTPTOVCtM offset");
static_assert(offsetof(r_gptp_timer_regs_t, PTPTOVCU) == (size_t)k_ra8_gptp_off_t_ptptovcu,
              "PTPTOVCtU offset");
static_assert(offsetof(r_gptp_timer_regs_t, PTPAVTPTML) == (size_t)k_ra8_gptp_off_t_ptpavtptml,
              "PTPAVTPTMtL offset");
static_assert(offsetof(r_gptp_timer_regs_t, PTPAVTPTMU) == (size_t)k_ra8_gptp_off_t_ptpavtptmu,
              "PTPAVTPTMtU offset");
static_assert(offsetof(r_gptp_timer_regs_t, PTPGPTPTML) == (size_t)k_ra8_gptp_off_t_ptpgptptml,
              "PTPGPTPTMtL offset");
static_assert(offsetof(r_gptp_timer_regs_t, PTPGPTPTMM) == (size_t)k_ra8_gptp_off_t_ptpgptptmm,
              "PTPGPTPTMtM offset");
static_assert(offsetof(r_gptp_timer_regs_t, PTPGPTPTMU) == (size_t)k_ra8_gptp_off_t_ptpgptptmu,
              "PTPGPTPTMtU offset");
static_assert(offsetof(r_gptp_regs_t, PTPIPV) == (size_t)k_ra8_gptp_off_ptpipv, "PTPIPV offset");
static_assert(offsetof(r_gptp_regs_t, PTPTMEC) == (size_t)k_ra8_gptp_off_ptptmec, "PTPTMEC offset");
static_assert(offsetof(r_gptp_regs_t, PTPTMDC) == (size_t)k_ra8_gptp_off_ptptmdc, "PTPTMDC offset");
static_assert(offsetof(r_gptp_regs_t, TIMER) == (size_t)k_ra8_gptp_off_timer0, "TIMER[0] offset");

/** @brief Get pointer to the GPTP register block. */
RA8_HW_REGISTER_ACCESS
static inline volatile r_gptp_regs_t* ra8_gptp(void)
{
  /* HUM Ch 35.2 "gPTP Register List" Table 35.3 p 1926 -- base GPTP =
   * 0x403E_0000 (secure) / GPTP_NS = 0x503E_0000. */
  return (volatile r_gptp_regs_t*)k_ra8_gptp_base_addr;
}

#ifdef __cplusplus
}
#endif
