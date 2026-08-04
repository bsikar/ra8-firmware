/**
 * @file ra8_etha_regs.h
 * @brief Ethernet Agent (ETHA) per-port register layout for the RA8D2
 * @ingroup grp_hal_net
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * ETHA is the per-port "Ethernet Agent" block that sits between the
 * top-level Ethernet Switch Module (ESWM, HUM Ch 29) and the on-chip
 * Ethernet MAC (RMAC, HUM Ch 33). One ETHA instance exists per
 * Ethernet port (m = 0, 1) and each instance is mapped 0x2000 apart:
 *
 *   ETHA0 = 0x403C_A000   (k_ra8_etha0_base_addr in ra8_ether_regs.h)
 *   ETHA1 = 0x403C_C000   (k_ra8_etha1_base_addr in ra8_ether_regs.h)
 *
 * The register layout is HUM Table 32.3 "ETHA registers" (p 1628-1630),
 * which runs EAMC (+0x0000) to EAEID2 (+0x0528). Every named register
 * here corresponds 1:1 with a row of that table and every offset is
 * pinned by a ``static_assert`` below; reserved gaps are kept as
 * ``reservedNN`` placeholders so offset arithmetic matches the chip.
 *
 * The window was previously sized 0x584 to mirror the Renesas FSP CMSIS
 * type ``R_ETHA0_Type``, which carries a trailing ``EASCR`` at +0x0580.
 * Chapter 32 does not mention that symbol anywhere, so it was an FSP
 * artefact rather than a documented register and has been removed along
 * with the driver entry point that wrote it (#539). The manual, not the
 * vendor header, is the authority for what exists.
 *
 * Eight per-traffic-class arrays (EATMFSC, EATDQDC, EATDQM, EATDQMLM,
 * EACAIVC, EACAULC, EACOIVM, EACOULM, EATASENC, EATASENM) follow the
 * 802.1Q/802.1Qbv (TAS) traffic-class convention used by the chip --
 * index 0 = best-effort, index 7 = highest priority.
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
#include "ra8_ether_regs.h"

/**
 * @enum ra8_etha_port_t
 * @brief ETHA port index (m = 0, 1).
 *
 * @details
 * Used as the first argument to every ra8_etha_* call. Maps directly
 * to the per-port base addresses defined in ra8_ether_regs.h.
 */
typedef enum : uint8_t {
  k_ra8_etha_port_0     = 0U, /**< ETHA port 0 (base 0x403C_A000). */
  k_ra8_etha_port_1     = 1U, /**< ETHA port 1 (base 0x403C_C000). */
  k_ra8_etha_port_count = 2U, /**< Number of ETHA ports.           */
} ra8_etha_port_t;

/**
 * @enum ra8_etha_tc_t
 * @brief Traffic-class index (q = 0..7) used by every per-class array.
 *
 * @details
 * From HUM Ch 32 figure 32.1 ("Block diagram"): each ETHA port owns
 * eight traffic classes feeding the TX scheduler. Class 0 carries
 * best-effort traffic; class 7 the highest priority. Used as the
 * common index across EATMFSCq, EATDQDCq, EATDQMq, EATDQMLMq,
 * EACAIVCq, EACAULCq, EACOIVMq, EACOULMq, EATASENCi, EATASENMi.
 */
typedef enum : uint8_t {
  k_ra8_etha_tc_0     = 0U, /**< Best-effort.                        */
  k_ra8_etha_tc_1     = 1U, /**< AVB class B / general queue.        */
  k_ra8_etha_tc_2     = 2U, /**< AVB class A.                        */
  k_ra8_etha_tc_3     = 3U, /**< Reserved priority.                  */
  k_ra8_etha_tc_4     = 4U, /**< Reserved priority.                  */
  k_ra8_etha_tc_5     = 5U, /**< Reserved priority.                  */
  k_ra8_etha_tc_6     = 6U, /**< Network control.                    */
  k_ra8_etha_tc_7     = 7U, /**< Highest priority / control / PTP.   */
  k_ra8_etha_tc_count = 8U, /**< Number of traffic classes per port. */
} ra8_etha_tc_t;

/**
 * @enum ra8_etha_offset_t
 * @brief Selected ETHA per-port register byte offsets (HUM Ch 32.3).
 *
 * @details
 * Provided for callers that need to reach a register by offset (e.g.
 * generic dump routines). The full layout is captured by
 * ::r_etha_regs_t and each driver call uses the named field directly.
 */
typedef enum : uint16_t {
  k_ra8_etha_off_eamc       = 0x0000U, /**< EAMC: Mode Configuration.        */
  k_ra8_etha_off_eams       = 0x0004U, /**< EAMS: Mode Status.               */
  k_ra8_etha_off_eairc      = 0x0010U, /**< EAIRC: IPV remap.                */
  k_ra8_etha_off_eatdqsc    = 0x0014U, /**< EATDQSC: TX queue security.      */
  k_ra8_etha_off_eatdqc     = 0x0018U, /**< EATDQC: TX queue control.        */
  k_ra8_etha_off_eatdqac    = 0x001CU, /**< EATDQAC: TX queue arbitration.   */
  k_ra8_etha_off_eatpec     = 0x0020U, /**< EATPEC: TX preemption cfg.       */
  k_ra8_etha_off_eatmfsc0   = 0x0040U, /**< EATMFSC0: per-class max frame.   */
  k_ra8_etha_off_eatdqdc0   = 0x0060U, /**< EATDQDC0: per-class queue cfg.   */
  k_ra8_etha_off_eatdqm0    = 0x0080U, /**< EATDQM0: per-class queue mon.    */
  k_ra8_etha_off_eatdqmlm0  = 0x00A0U, /**< EATDQMLM0: max queue level.      */
  k_ra8_etha_off_eactqc     = 0x0100U, /**< EACTQC: cut-through queue cfg.   */
  k_ra8_etha_off_eactdqdc   = 0x0104U, /**< EACTDQDC: cut-through depth.     */
  k_ra8_etha_off_eactdqm    = 0x0108U, /**< EACTDQM: cut-through queue mon.  */
  k_ra8_etha_off_eactdqmlm  = 0x010CU, /**< EACTDQMLM: cut-through peak.     */
  k_ra8_etha_off_eavcc      = 0x0130U, /**< EAVCC: VLAN control.             */
  k_ra8_etha_off_eavtc      = 0x0134U, /**< EAVTC: VLAN tag config.          */
  k_ra8_etha_off_eartfc     = 0x0138U, /**< EARTFC: RX tag filter cfg.       */
  k_ra8_etha_off_eacaec     = 0x0200U, /**< EACAEC: CBS admin enable.        */
  k_ra8_etha_off_eacc       = 0x0204U, /**< EACC: CBS configuration.         */
  k_ra8_etha_off_eacaivc0   = 0x0220U, /**< EACAIVC0: CBS admin increment.   */
  k_ra8_etha_off_eacaulc0   = 0x0240U, /**< EACAULC0: CBS admin upper limit. */
  k_ra8_etha_off_eacoem     = 0x0260U, /**< EACOEM: CBS oper enable mon.     */
  k_ra8_etha_off_eacoivm0   = 0x0280U, /**< EACOIVM0: CBS oper increment.    */
  k_ra8_etha_off_eacoulm0   = 0x02A0U, /**< EACOULM0: CBS oper upper limit.  */
  k_ra8_etha_off_eacgsm     = 0x02C0U, /**< EACGSM: CBS gate state monitor.  */
  k_ra8_etha_off_eatasc     = 0x0300U, /**< EATASC: TAS configuration.       */
  k_ra8_etha_off_eatasigsc  = 0x0304U, /**< EATASIGSC: TAS init gate state.  */
  k_ra8_etha_off_eatasenc0  = 0x0320U, /**< EATASENC0: TAS entry-number cfg. */
  k_ra8_etha_off_eatasctenc = 0x0340U, /**< EATASCTENC: TAS cut-through cfg. */
  k_ra8_etha_off_eatasenm0  = 0x0360U, /**< EATASENM0: TAS entry-number mon. */
  k_ra8_etha_off_eatasctenm = 0x0380U, /**< EATASCTENM: TAS cut-through mon. */
  k_ra8_etha_off_eatascstc0 = 0x03A0U, /**< EATASCSTC0: TAS cycle start lo.  */
  k_ra8_etha_off_eatascstc1 = 0x03A4U, /**< EATASCSTC1: TAS cycle start hi.  */
  k_ra8_etha_off_eatascstm0 = 0x03A8U, /**< EATASCSTM0: cycle start mon lo.  */
  k_ra8_etha_off_eatascstm1 = 0x03ACU, /**< EATASCSTM1: cycle start mon hi.  */
  k_ra8_etha_off_eatasctc   = 0x03B0U, /**< EATASCTC: TAS cycle time cfg.    */
  k_ra8_etha_off_eatasctm   = 0x03B4U, /**< EATASCTM: TAS cycle time mon.    */
  k_ra8_etha_off_eatasgl0   = 0x03C0U, /**< EATASGL0: TAS gate learn 0.      */
  k_ra8_etha_off_eatasgl1   = 0x03C4U, /**< EATASGL1: TAS gate learn 1.      */
  k_ra8_etha_off_eatasglr   = 0x03C8U, /**< EATASGLR: TAS gate learn result. */
  k_ra8_etha_off_eatasgr    = 0x03D0U, /**< EATASGR: TAS gate read.          */
  k_ra8_etha_off_eatasgrr   = 0x03D4U, /**< EATASGRR: TAS gate read result.  */
  k_ra8_etha_off_eatashcc   = 0x03E0U, /**< EATASHCC: TAS HW calibration.    */
  k_ra8_etha_off_eatasrirm  = 0x03E4U, /**< EATASRIRM: TAS RAM init monitor. */
  k_ra8_etha_off_eatassm    = 0x03E8U, /**< EATASSM: TAS status monitor.     */
  k_ra8_etha_off_eausmfsecn = 0x0400U, /**< EAUSMFSECN: under-min FS err.    */
  k_ra8_etha_off_eatfecn    = 0x0404U, /**< EATFECN: tag filter err.         */
  k_ra8_etha_off_eafsecn    = 0x0408U, /**< EAFSECN: frame size err.         */
  k_ra8_etha_off_eadqoecn   = 0x040CU, /**< EADQOECN: queue overflow err.    */
  k_ra8_etha_off_eadqsecn   = 0x0410U, /**< EADQSECN: queue security err.    */
  k_ra8_etha_off_eaeis0     = 0x0500U, /**< EAEIS0: error IRQ status 0.      */
  k_ra8_etha_off_eaeie0     = 0x0504U, /**< EAEIE0: error IRQ enable 0.      */
  k_ra8_etha_off_eaeid0     = 0x0508U, /**< EAEID0: error IRQ disable 0.     */
  k_ra8_etha_off_eaeis1     = 0x0510U, /**< EAEIS1: error IRQ status 1.      */
  k_ra8_etha_off_eaeie1     = 0x0514U, /**< EAEIE1: error IRQ enable 1.      */
  k_ra8_etha_off_eaeid1     = 0x0518U, /**< EAEID1: error IRQ disable 1.     */
  k_ra8_etha_off_eaeis2     = 0x0520U, /**< EAEIS2: error IRQ status 2.      */
  k_ra8_etha_off_eaeie2     = 0x0524U, /**< EAEIE2: error IRQ enable 2.      */
  k_ra8_etha_off_eaeid2     = 0x0528U, /**< EAEID2: error IRQ disable 2.     */
} ra8_etha_offset_t;

/**
 * @enum ra8_etha_opc_t
 * @brief ETHA EAMC.OPC[1:0] operating mode commands.
 *
 * @details
 * From HUM Ch 32.3.1.1 "EAMC : Mode Configuration Register" p 1630.
 */
typedef enum : uint8_t {
  k_ra8_etha_opc_reset     = 0U, /**< Enter RESET mode.     */
  k_ra8_etha_opc_disable   = 1U, /**< Enter DISABLE mode.   */
  k_ra8_etha_opc_config    = 2U, /**< Enter CONFIG mode.    */
  k_ra8_etha_opc_operation = 3U, /**< Enter OPERATION mode. */
} ra8_etha_opc_t;

/**
 * @enum ra8_etha_ops_t
 * @brief ETHA EAMS.OPS[1:0] operating-mode status flag values.
 *
 * @details
 * From HUM Ch 32.3.1.2 "EAMS : Mode Status Register" p 1631.
 */
typedef enum : uint8_t {
  k_ra8_etha_ops_reset     = 0U, /**< RESET mode active.     */
  k_ra8_etha_ops_disable   = 1U, /**< DISABLE mode active.   */
  k_ra8_etha_ops_config    = 2U, /**< CONFIG mode active.    */
  k_ra8_etha_ops_operation = 3U, /**< OPERATION mode active. */
} ra8_etha_ops_t;

/**
 * @enum ra8_etha_mask_t
 * @brief ETHA bit masks for EAMC / EAMS / per-class fields.
 */
typedef enum : uint32_t {
  k_ra8_etha_mask_opc       = 0x00000003UL, /**< EAMC.OPC[1:0].          */
  k_ra8_etha_mask_ops       = 0x00000003UL, /**< EAMS.OPS[1:0].          */
  k_ra8_etha_mask_ipv       = 0x00000007UL, /**< EAIRC.IPVRq[2:0].       */
  k_ra8_etha_mask_tc        = 0x000000FFUL, /**< Per-TC byte mask.       */
  k_ra8_etha_mask_tdqa      = 0x0000000FUL, /**< EATDQAC.TDQAq[3:0].     */
  k_ra8_etha_mask_mfs       = 0x0000FFFFUL, /**< EATMFSCq.MFS[15:0].     */
  k_ra8_etha_mask_dqd       = 0x000007FFUL, /**< EATDQDCq.DQD[10:0].     */
  k_ra8_etha_mask_dnq       = 0x000007FFUL, /**< EATDQMq.DNQ[10:0].      */
  k_ra8_etha_mask_ctqd      = 0x0000FFFFUL, /**< EACTQC.CTQD[15:0].      */
  k_ra8_etha_mask_ctdqd     = 0x0000000FUL, /**< EACTDQDC.CTDQD[3:0].    */
  k_ra8_etha_mask_ctqdn     = 0x000003FFUL, /**< EACTDQM.CTQDN[9:0].     */
  k_ra8_etha_mask_vlan_vid  = 0x00000FFFUL, /**< EAVTC VID 12-bit field. */
  k_ra8_etha_mask_vlan_pcp  = 0x00000007UL, /**< EAVTC PCP 3-bit field.  */
  k_ra8_etha_mask_vlan_dei  = 0x00000001UL, /**< EAVTC DEI 1-bit field.  */
  k_ra8_etha_mask_civ       = 0x000FFFFFUL, /**< EACAIVCq.CIV[19:0].     */
  k_ra8_etha_mask_cul       = 0x7FFFFFFFUL, /**< EACAULCq.CUL[30:0].     */
  k_ra8_etha_mask_aen       = 0x000001FFUL, /**< EATASENC.TASAEN[8:0].   */
  k_ra8_etha_mask_tas_cycle = 0xFFFFFFFFUL, /**< EATASCTC TASACT 32-bit. */
  k_ra8_etha_mask_tas_jit   = 0x0000FFFFUL, /**< EATASHCC.TASJ[15:0].    */
  k_ra8_etha_mask_tas_gtl   = 0x0FFFFFFFUL, /**< EATASGL1.TASGTL[27:0].  */
  k_ra8_etha_mask_tas_gal   = 0x000000FFUL, /**< EATASGL0.TASGAL[7:0].   */
  k_ra8_etha_mask_tas_igs   = 0x000000FFUL, /**< EATASIGSC.TASIGS7..0.   */
} ra8_etha_mask_t;

/**
 * @enum ra8_etha_tas_bits_t
 * @brief Single-bit positions in the TAS learn / read / RAM-init registers.
 *
 * @details
 * Named from the HUM field tables so the TAS entry flow cannot repeat the
 * defect it was written to fix: ``EATASGL1`` bit 28 is ``TASGSL``, the
 * entry's one-bit GATE STATE (HUM Ch 32.3.5.14 "EATASGL1 : TAS Gate Learn
 * Register 1" p 1652), and had been carrying an unrelated "cut-through"
 * flag while the gate state was written into ``EATASGL0``, whose
 * ``TASGAL[7:0]`` field is the TAS RAM ENTRY ADDRESS (#539).
 *
 * @invariant Every value is a bit index in 0..31.
 * @see ra8_etha_set_tas_schedule
 */
typedef enum : uint8_t {
  k_ra8_etha_eatasgl1_tasgsl_pos   = 28U, /**< EATASGL1.TASGSL: entry gate state. */
  k_ra8_etha_eatasglr_gl_pos       = 31U, /**< EATASGLR.GL: learn in progress.    */
  k_ra8_etha_eatasgrr_gr_pos       = 31U, /**< EATASGRR.GR: read in progress.     */
  k_ra8_etha_eatasgrr_tasgsr_pos   = 28U, /**< EATASGRR.TASGSR: read gate state.  */
  k_ra8_etha_eatasrirm_tasriog_pos = 0U,  /**< EATASRIRM.TASRIOG: init ongoing.   */
  k_ra8_etha_eatasrirm_tasrr_pos   = 1U,  /**< EATASRIRM.TASRR: TAS RAM ready.    */
} ra8_etha_tas_bits_t;

/**
 * @enum ra8_etha_tas_limits_t
 * @brief Capacity limits the TAS RAM imposes on a schedule.
 *
 * @details
 * ``TASGAL[7:0]`` addresses 256 TAS RAM entries, but HUM Ch 32.3.5.3
 * "EATASENCi : TAS Entry Number Configuration Register i" p 1647 caps the
 * usable total: the sum of every ``EATASENCi.TASAEN`` (plus
 * ``EATASCTENC.TASCTAEN``) must be <= 119 when the schedule is changed
 * while ``EATASC.TASE`` is already set, and <= 247 otherwise. The driver
 * enforces the conservative 119 so a schedule accepted at bring-up stays
 * legal if it is later re-programmed live.
 *
 * @see ra8_etha_set_tas_schedule
 */
typedef enum : uint16_t {
  k_ra8_etha_tas_entries_max = 119U, /**< Max total entries across all queues.     */
  k_ra8_etha_tas_addr_max    = 255U, /**< Highest TAS RAM address TASGAL can hold. */
  k_ra8_etha_tas_aen_max     = 511U, /**< EATASENCi.TASAEN[8:0] field ceiling.     */
} ra8_etha_tas_limits_t;

/**
 * @enum ra8_etha_field_pos_t
 * @brief Convenience right-shift positions for EATPEC / EAVCC bit fields.
 */
typedef enum : uint8_t {
  k_ra8_etha_eatpec_afs_pos       = 16U, /**< EATPEC.AFS[17:16].           */
  k_ra8_etha_eavcc_vem_pos        = 16U, /**< EAVCC.VEM[18:16].            */
  k_ra8_etha_eavtc_ctv_pos        = 0U,  /**< EAVTC.CTV[11:0] C-VLAN VID.  */
  k_ra8_etha_eavtc_ctp_pos        = 12U, /**< EAVTC.CTP[14:12] C-VLAN PCP. */
  k_ra8_etha_eavtc_ctd_pos        = 15U, /**< EAVTC.CTD[15] C-VLAN DEI.    */
  k_ra8_etha_eavtc_stv_pos        = 16U, /**< EAVTC.STV[27:16] S-VLAN VID. */
  k_ra8_etha_eavtc_stp_pos        = 28U, /**< EAVTC.STP[30:28] S-VLAN PCP. */
  k_ra8_etha_eavtc_std_pos        = 31U, /**< EAVTC.STD[31] S-VLAN DEI.    */
  k_ra8_etha_eatasc_tase_pos      = 0U,  /**< EATASC.TASE.                 */
  k_ra8_etha_eatasc_tascc_pos     = 1U,  /**< EATASC.TASCC.                */
  k_ra8_etha_eatasc_tasci_pos     = 2U,  /**< EATASC.TASCI.                */
  k_ra8_etha_eatasc_tasts_pos     = 8U,  /**< EATASC.TASTS.                */
  k_ra8_etha_eatasc_tasca_pos     = 16U, /**< EATASC.TASCA[23:16].         */
  k_ra8_etha_pos_eaeis0_usmfses   = 5U,  /**< EAEIS0.USMFSES bit position. */
  k_ra8_etha_pos_eaeis0_tfes      = 6U,  /**< EAEIS0.TFES bit position.    */
  k_ra8_etha_pos_eaeis0_fses      = 8U,  /**< EAEIS0.FSESq base.           */
  k_ra8_etha_pos_eaeis0_tasgees   = 16U, /**< EAEIS0.TASGEESq base.        */
  k_ra8_etha_pos_eaeis0_tasctgees = 24U, /**< EAEIS0.TASCTGEES.            */
  k_ra8_etha_pos_eaeis2_dqoes     = 0U,  /**< EAEIS2.DQOESq base.          */
  k_ra8_etha_pos_eaeis2_ctdqoes   = 8U,  /**< EAEIS2.CTDQOES.              */
  k_ra8_etha_pos_eaeis2_dqses     = 16U, /**< EAEIS2.DQSESq base.          */
  k_ra8_etha_pos_eaeis1_cules     = 0U,  /**< EAEIS1.CULESq base.          */
  k_ra8_etha_pos_eaeis1_tasges    = 16U, /**< EAEIS1.TASGESq base.         */
  k_ra8_etha_pos_eaeis1_tasctges  = 24U, /**< EAEIS1.TASCTGES.             */
} ra8_etha_field_pos_t;

/**
 * @enum ra8_etha_irq_class_t
 * @brief Identifier for one of the three error-interrupt blocks.
 *
 * @details
 * Each ETHA port has three independent error-interrupt status/enable
 * triplets:
 *   - Block 0 (EAEIS0/E/D0): ECC, switch min frame size, tag filter,
 *     per-class frame size errors, TAS gate-event errors.
 *   - Block 1 (EAEIS1/E/D1): CBS upper-limit errors, TAS gate state.
 *   - Block 2 (EAEIS2/E/D2): TX descriptor queue overflow + security.
 */
typedef enum : uint8_t {
  k_ra8_etha_irq_class_0     = 0U, /**< Block 0: ECC + frame errors + TAS gate-events. */
  k_ra8_etha_irq_class_1     = 1U, /**< Block 1: CBS overflow + TAS gate-states.       */
  k_ra8_etha_irq_class_2     = 2U, /**< Block 2: descriptor queue overflow + security. */
  k_ra8_etha_irq_class_count = 3U, /**< RA8 etha IRQ class count.                      */
} ra8_etha_irq_class_t;

/**
 * @enum ra8_etha_eaeis0_bits_t
 * @brief Selected EAEIS0 status flags exposed for tests / IRQ dispatch.
 *
 * @details
 * From HUM Ch 32.3.x "EAEIS0 : Error Interrupt Status Register 0",
 * cross-referenced with the FSP ``EAEIS0_b`` bit-field layout. Bit
 * positions match the chip; full set lives in the register block.
 */
typedef enum : uint32_t {
  k_ra8_etha_eaeis0_decces       = (1UL << 0),   /**< Descriptor ECC error.      */
  k_ra8_etha_eaeis0_tecces       = (1UL << 1),   /**< Transmit data ECC error.   */
  k_ra8_etha_eaeis0_pecces       = (1UL << 2),   /**< Per-class ECC error.       */
  k_ra8_etha_eaeis0_dsecces      = (1UL << 3),   /**< Descriptor security ECC.   */
  k_ra8_etha_eaeis0_l23ueecces   = (1UL << 4),   /**< L2/3 unrecoverable ECC.    */
  k_ra8_etha_eaeis0_usmfses      = (1UL << 5),   /**< Switch min frame size err. */
  k_ra8_etha_eaeis0_tfes         = (1UL << 6),   /**< Tag-filter error.          */
  k_ra8_etha_eaeis0_fses_mask    = 0x0000FF00UL, /**< Per-class frame size err.  */
  k_ra8_etha_eaeis0_tasgees_mask = 0x00FF0000UL, /**< TAS gate-event error.      */
  k_ra8_etha_eaeis0_tasctgees    = (1UL << 24),  /**< TAS cut-through gate.      */
} ra8_etha_eaeis0_bits_t;

/**
 * @enum ra8_etha_eaeis2_bits_t
 * @brief Selected EAEIS2 (descriptor queue) status flags.
 */
typedef enum : uint32_t {
  k_ra8_etha_eaeis2_dqoes_mask = 0x000000FFUL, /**< TX queue overflow per-TC.   */
  k_ra8_etha_eaeis2_ctdqoes    = (1UL << 8),   /**< Cut-through queue overflow. */
  k_ra8_etha_eaeis2_dqses_mask = 0x00FF0000UL, /**< TX queue security per-TC.   */
} ra8_etha_eaeis2_bits_t;

/**
 * @enum ra8_etha_vem_t
 * @brief EAVCC.VEM[2:0] VLAN extraction mode.
 *
 * @details
 * From HUM Ch 32.3.x "EAVCC : Ethernet Agent VLAN Control Configuration".
 * Selects how the agent treats the inner / outer VLAN tag on RX.
 */
typedef enum : uint8_t {
  k_ra8_etha_vem_none        = 0U, /**< Pass through unchanged.         */
  k_ra8_etha_vem_strip_outer = 1U, /**< Strip outer (S-VLAN) tag on RX. */
  k_ra8_etha_vem_strip_inner = 2U, /**< Strip inner (C-VLAN) tag on RX. */
  k_ra8_etha_vem_strip_both  = 3U, /**< Strip both S- and C-VLAN tags.  */
  k_ra8_etha_vem_swap        = 4U, /**< Swap inner / outer tags on RX.  */
} ra8_etha_vem_t;

/**
 * @enum ra8_etha_vim_t
 * @brief EAVCC.VIM VLAN insertion mode for TX.
 */
typedef enum : uint8_t {
  k_ra8_etha_vim_disabled = 0U, /**< No VLAN insertion on TX.     */
  k_ra8_etha_vim_enabled  = 1U, /**< Insert tag from EAVTC on TX. */
} ra8_etha_vim_t;

/**
 * @enum ra8_etha_afs_t
 * @brief EATPEC.AFS[1:0] additional fragment size policy for 802.3br.
 */
typedef enum : uint8_t {
  k_ra8_etha_afs_64  = 0U, /**< Minimum fragment size 64 bytes.  */
  k_ra8_etha_afs_128 = 1U, /**< Minimum fragment size 128 bytes. */
  k_ra8_etha_afs_192 = 2U, /**< Minimum fragment size 192 bytes. */
  k_ra8_etha_afs_256 = 3U, /**< Minimum fragment size 256 bytes. */
} ra8_etha_afs_t;

/**
 * @enum ra8_etha_rsv_words_t
 * @brief Reserved-word array sizes (in 32-bit words) used inside the
 *        ETHA per-port register block.
 *
 * @details
 * Names each reserved gap so the struct definition does not depend
 * on raw integer literals. Cross-checked against HUM Ch 32 register
 * offset table.
 */
typedef enum : uint8_t {
  k_ra8_etha_rsv_words_eatpec_to_eatmfsc = 7U,  /**< +0x0024..0x003F gap. */
  k_ra8_etha_rsv_words_eartfc_to_eacaec  = 49U, /**< +0x013C..0x01FF gap. */
  k_ra8_etha_rsv_words_eacoem_to_eacoivm = 7U,  /**< +0x0264..0x027F gap. */
  k_ra8_etha_rsv_words_eacgsm_to_eatasc  = 15U, /**< +0x02C4..0x02FF gap. */
  k_ra8_etha_rsv_words_eatasctenc_gap    = 7U,  /**< +0x0344..0x035F gap. */
  k_ra8_etha_rsv_words_eatasctenm_gap    = 7U,  /**< +0x0384..0x039F gap. */
  k_ra8_etha_rsv_words_eatassm_to_eausm  = 5U,  /**< +0x03EC..0x03FF gap. */
  k_ra8_etha_rsv_words_eadqsecn_to_eaeis = 59U, /**< +0x0414..0x04FF gap. */
} ra8_etha_rsv_words_t;

/**
 * @struct r_etha_regs_t
 * @brief Full ETHA per-port register window (HUM Ch 32, FSP R_ETHA0_Type).
 *
 * @details
 * Sized to 0x584 bytes to match the FSP CMSIS layout exactly. Reserved
 * gaps are kept as ``reservedNN`` placeholders so structure offset
 * arithmetic matches the chip.
 *
 * cppcheck cannot see tests/ so it flags every field as unused;
 * each member is accessed via ``ra8_etha(port)`` in
 * ``libs/ra8_hal/src/ra8_etha.c``.
 */
typedef struct {
  volatile uint32_t EAMC;       /**< +0x0000 Mode command.                   */
  volatile uint32_t EAMS;       /**< +0x0004 Mode status.                    */
  volatile uint32_t reserved08; /**< +0x0008 Reserved.                       */
  volatile uint32_t reserved0C; /**< +0x000C Reserved.                       */
  volatile uint32_t EAIRC;      /**< +0x0010 IPV remap (8x 3-bit fields).    */
  volatile uint32_t EATDQSC;    /**< +0x0014 TX desc queue security cfg.     */
  volatile uint32_t EATDQC;     /**< +0x0018 TX desc queue control.          */
  volatile uint32_t EATDQAC;    /**< +0x001C TX desc queue arbitration.      */
  volatile uint32_t EATPEC;     /**< +0x0020 TX preemption config (802.3br). */
  volatile uint32_t reserved24
    [k_ra8_etha_rsv_words_eatpec_to_eatmfsc]; /**< +0x0024..0x003F Reserved.              */
  volatile uint32_t EATMFSC[8];               /**< +0x0040..0x005F Per-class max frame.   */
  volatile uint32_t EATDQDC[8];               /**< +0x0060..0x007F Per-class queue depth. */
  volatile uint32_t EATDQM[8];                /**< +0x0080..0x009F Per-class queue mon.   */
  volatile uint32_t EATDQMLM[8];              /**< +0x00A0..0x00BF Per-class peak level.  */
  volatile uint32_t reservedC0[16];           /**< +0x00C0..0x00FF Reserved.              */
  volatile uint32_t EACTQC;                   /**< +0x0100 Cut-through queue config.      */
  volatile uint32_t EACTDQDC;                 /**< +0x0104 Cut-through queue depth.       */
  volatile uint32_t EACTDQM;                  /**< +0x0108 Cut-through queue monitor.     */
  volatile uint32_t EACTDQMLM;                /**< +0x010C Cut-through max queue level.   */
  volatile uint32_t reserved110[8];           /**< +0x0110..0x012F Reserved.              */
  volatile uint32_t EAVCC;                    /**< +0x0130 VLAN control configuration.    */
  volatile uint32_t EAVTC;                    /**< +0x0134 VLAN tag (C+S) configuration.  */
  volatile uint32_t EARTFC;                   /**< +0x0138 RX tag filter configuration.   */
  volatile uint32_t
    reserved13C[k_ra8_etha_rsv_words_eartfc_to_eacaec]; /**< +0x013C..0x01FF Reserved.            */
  volatile uint32_t EACAEC;                             /**< +0x0200 CBS admin enable.            */
  volatile uint32_t EACC;                               /**< +0x0204 CBS configuration.           */
  volatile uint32_t reserved208[6];                     /**< +0x0208..0x021F Reserved.            */
  volatile uint32_t EACAIVC[8];                         /**< +0x0220..0x023F CBS admin increment. */
  volatile uint32_t EACAULC[8];                         /**< +0x0240..0x025F CBS admin upper lim. */
  volatile uint32_t EACOEM;                             /**< +0x0260 CBS oper enable monitor.     */
  volatile uint32_t reserved264
    [k_ra8_etha_rsv_words_eacoem_to_eacoivm]; /**< +0x0264..0x027F Reserved.               */
  volatile uint32_t EACOIVM[8];               /**< +0x0280..0x029F CBS oper increment mon. */
  volatile uint32_t EACOULM[8];               /**< +0x02A0..0x02BF CBS oper upper limit.   */
  volatile uint32_t EACGSM;                   /**< +0x02C0 CBS gate state monitor.         */
  volatile uint32_t
    reserved2C4[k_ra8_etha_rsv_words_eacgsm_to_eatasc]; /**< +0x02C4..0x02FF Reserved.           */
  volatile uint32_t EATASC;                             /**< +0x0300 TAS configuration.          */
  volatile uint32_t EATASIGSC;                          /**< +0x0304 TAS initial gate state cfg. */
  volatile uint32_t reserved308[6];                     /**< +0x0308..0x031F Reserved.           */
  volatile uint32_t EATASENC[8];                        /**< +0x0320..0x033F TAS entry-num cfg.  */
  volatile uint32_t EATASCTENC;                         /**< +0x0340 TAS cut-through entry cfg.  */
  volatile uint32_t
    reserved344[k_ra8_etha_rsv_words_eatasctenc_gap]; /**< +0x0344..0x035F Reserved.             */
  volatile uint32_t EATASENM[8];                      /**< +0x0360..0x037F TAS entry-number mon. */
  volatile uint32_t EATASCTENM;                       /**< +0x0380 TAS cut-through entry mon.    */
  volatile uint32_t
    reserved384[k_ra8_etha_rsv_words_eatasctenm_gap]; /**< +0x0384..0x039F Reserved.              */
  volatile uint32_t EATASCSTC0;                       /**< +0x03A0 TAS cycle start time cfg lo.   */
  volatile uint32_t EATASCSTC1;                       /**< +0x03A4 TAS cycle start time cfg hi.   */
  volatile uint32_t EATASCSTM0;                       /**< +0x03A8 TAS cycle start time mon lo.   */
  volatile uint32_t EATASCSTM1;                       /**< +0x03AC TAS cycle start time mon hi.   */
  volatile uint32_t EATASCTC;                         /**< +0x03B0 TAS cycle time configuration.  */
  volatile uint32_t EATASCTM;                         /**< +0x03B4 TAS cycle time monitor.        */
  volatile uint32_t reserved3B8[2];                   /**< +0x03B8..0x03BF Reserved.              */
  volatile uint32_t EATASGL0;                         /**< +0x03C0 TAS gate learn 0.              */
  volatile uint32_t EATASGL1;                         /**< +0x03C4 TAS gate learn 1.              */
  volatile uint32_t EATASGLR;                         /**< +0x03C8 TAS gate learn result.         */
  volatile uint32_t reserved3CC;                      /**< +0x03CC Reserved.                      */
  volatile uint32_t EATASGR;                          /**< +0x03D0 TAS gate read.                 */
  volatile uint32_t EATASGRR;                         /**< +0x03D4 TAS gate read result.          */
  volatile uint32_t reserved3D8[2];                   /**< +0x03D8..0x03DF Reserved.              */
  volatile uint32_t EATASHCC;                         /**< +0x03E0 TAS hardware calibration cfg.  */
  volatile uint32_t EATASRIRM;                        /**< +0x03E4 TAS RAM-init register monitor. */
  volatile uint32_t EATASSM;                          /**< +0x03E8 TAS status monitor.            */
  volatile uint32_t reserved3EC
    [k_ra8_etha_rsv_words_eatassm_to_eausm]; /**< +0x03EC..0x03FF Reserved.              */
  volatile uint32_t EAUSMFSECN;              /**< +0x0400 Switch min frame size err cnt. */
  volatile uint32_t EATFECN;                 /**< +0x0404 Tag filter error counter.      */
  volatile uint32_t EAFSECN;                 /**< +0x0408 Frame size error counter.      */
  volatile uint32_t EADQOECN;                /**< +0x040C Queue overflow error counter.  */
  volatile uint32_t EADQSECN;                /**< +0x0410 Queue security error counter.  */
  volatile uint32_t
    reserved414[k_ra8_etha_rsv_words_eadqsecn_to_eaeis]; /**< +0x0414..0x04FF Reserved.          */
  volatile uint32_t EAEIS0;                              /**< +0x0500 Error interrupt status 0.  */
  volatile uint32_t EAEIE0;                              /**< +0x0504 Error interrupt enable 0.  */
  volatile uint32_t EAEID0;                              /**< +0x0508 Error interrupt disable 0. */
  volatile uint32_t reserved50C;                         /**< +0x050C Reserved.                  */
  volatile uint32_t EAEIS1;                              /**< +0x0510 Error interrupt status 1.  */
  volatile uint32_t EAEIE1;                              /**< +0x0514 Error interrupt enable 1.  */
  volatile uint32_t EAEID1;                              /**< +0x0518 Error interrupt disable 1. */
  volatile uint32_t reserved51C;                         /**< +0x051C Reserved.                  */
  volatile uint32_t EAEIS2;                              /**< +0x0520 Error interrupt status 2.  */
  volatile uint32_t EAEIE2;                              /**< +0x0524 Error interrupt enable 2.  */
  volatile uint32_t EAEID2;                              /**< +0x0528 Error interrupt disable 2. */
} r_etha_regs_t;

/**
 * @enum ra8_etha_size_t
 * @brief ETHA register window total size, per HUM Table 32.3 (p 1628-1630).
 *
 * @details
 * The window ends at EAEID2 (+0x0528), the last row of Table 32.3, so the
 * block is 0x52C bytes. It was previously sized 0x584 to match the FSP
 * ``R_ETHA0_Type``, whose trailing ``EASCR`` at +0x0580 the manual does not
 * publish anywhere in Chapter 32 (#539).
 */
typedef enum : uint16_t {
  k_ra8_etha_window_bytes = 0x52CU, /**< Total MMIO window, EAMC..EAEID2. */
} ra8_etha_size_t;

/**
 * @brief Compile-time offset and size insurance for r_etha_regs_t.
 *
 * @details
 * Every offset below is pinned to the value HUM Table 32.3 (p 1628-1630)
 * prints for that register. If any field drifts (e.g. a reserved gap is
 * mis-sized), these assertions fire at compile time before any test
 * runs -- silent frame corruption on the wire is avoided.
 */
static_assert(sizeof(r_etha_regs_t) == (size_t)k_ra8_etha_window_bytes,
              "r_etha_regs_t must span EAMC..EAEID2 (HUM Table 32.3)");

static_assert(offsetof(r_etha_regs_t, EAMC) == (size_t)k_ra8_etha_off_eamc, "EAMC offset");
static_assert(offsetof(r_etha_regs_t, EAMS) == (size_t)k_ra8_etha_off_eams, "EAMS offset");
static_assert(offsetof(r_etha_regs_t, EAIRC) == (size_t)k_ra8_etha_off_eairc, "EAIRC offset");
static_assert(offsetof(r_etha_regs_t, EATDQSC) == (size_t)k_ra8_etha_off_eatdqsc, "EATDQSC offset");
static_assert(offsetof(r_etha_regs_t, EATDQC) == (size_t)k_ra8_etha_off_eatdqc, "EATDQC offset");
static_assert(offsetof(r_etha_regs_t, EATDQAC) == (size_t)k_ra8_etha_off_eatdqac, "EATDQAC offset");
static_assert(offsetof(r_etha_regs_t, EATPEC) == (size_t)k_ra8_etha_off_eatpec, "EATPEC offset");
static_assert(offsetof(r_etha_regs_t, EATMFSC) == (size_t)k_ra8_etha_off_eatmfsc0,
              "EATMFSC offset");
static_assert(offsetof(r_etha_regs_t, EATDQDC) == (size_t)k_ra8_etha_off_eatdqdc0,
              "EATDQDC offset");
static_assert(offsetof(r_etha_regs_t, EATDQM) == (size_t)k_ra8_etha_off_eatdqm0, "EATDQM offset");
static_assert(offsetof(r_etha_regs_t, EATDQMLM) == (size_t)k_ra8_etha_off_eatdqmlm0,
              "EATDQMLM offset");
static_assert(offsetof(r_etha_regs_t, EACTQC) == (size_t)k_ra8_etha_off_eactqc, "EACTQC offset");
static_assert(offsetof(r_etha_regs_t, EAVCC) == (size_t)k_ra8_etha_off_eavcc, "EAVCC offset");
static_assert(offsetof(r_etha_regs_t, EAVTC) == (size_t)k_ra8_etha_off_eavtc, "EAVTC offset");
static_assert(offsetof(r_etha_regs_t, EARTFC) == (size_t)k_ra8_etha_off_eartfc, "EARTFC offset");
static_assert(offsetof(r_etha_regs_t, EACAEC) == (size_t)k_ra8_etha_off_eacaec, "EACAEC offset");
static_assert(offsetof(r_etha_regs_t, EACC) == (size_t)k_ra8_etha_off_eacc, "EACC offset");
static_assert(offsetof(r_etha_regs_t, EACAIVC) == (size_t)k_ra8_etha_off_eacaivc0,
              "EACAIVC offset");
static_assert(offsetof(r_etha_regs_t, EACAULC) == (size_t)k_ra8_etha_off_eacaulc0,
              "EACAULC offset");
static_assert(offsetof(r_etha_regs_t, EACOEM) == (size_t)k_ra8_etha_off_eacoem, "EACOEM offset");
static_assert(offsetof(r_etha_regs_t, EACOIVM) == (size_t)k_ra8_etha_off_eacoivm0,
              "EACOIVM offset");
static_assert(offsetof(r_etha_regs_t, EACOULM) == (size_t)k_ra8_etha_off_eacoulm0,
              "EACOULM offset");
static_assert(offsetof(r_etha_regs_t, EACGSM) == (size_t)k_ra8_etha_off_eacgsm, "EACGSM offset");
static_assert(offsetof(r_etha_regs_t, EATASC) == (size_t)k_ra8_etha_off_eatasc, "EATASC offset");
static_assert(offsetof(r_etha_regs_t, EATASIGSC) == (size_t)k_ra8_etha_off_eatasigsc,
              "EATASIGSC offset");
static_assert(offsetof(r_etha_regs_t, EATASENC) == (size_t)k_ra8_etha_off_eatasenc0,
              "EATASENC offset");
static_assert(offsetof(r_etha_regs_t, EATASCTENC) == (size_t)k_ra8_etha_off_eatasctenc,
              "EATASCTENC offset");
static_assert(offsetof(r_etha_regs_t, EATASENM) == (size_t)k_ra8_etha_off_eatasenm0,
              "EATASENM offset");
static_assert(offsetof(r_etha_regs_t, EATASCTENM) == (size_t)k_ra8_etha_off_eatasctenm,
              "EATASCTENM offset");
static_assert(offsetof(r_etha_regs_t, EATASCSTC0) == (size_t)k_ra8_etha_off_eatascstc0,
              "EATASCSTC0 offset");
static_assert(offsetof(r_etha_regs_t, EATASCTC) == (size_t)k_ra8_etha_off_eatasctc,
              "EATASCTC offset");
static_assert(offsetof(r_etha_regs_t, EATASGL0) == (size_t)k_ra8_etha_off_eatasgl0,
              "EATASGL0 offset");
static_assert(offsetof(r_etha_regs_t, EATASGR) == (size_t)k_ra8_etha_off_eatasgr, "EATASGR offset");
static_assert(offsetof(r_etha_regs_t, EATASHCC) == (size_t)k_ra8_etha_off_eatashcc,
              "EATASHCC offset");
static_assert(offsetof(r_etha_regs_t, EATASSM) == (size_t)k_ra8_etha_off_eatassm, "EATASSM offset");
static_assert(offsetof(r_etha_regs_t, EAUSMFSECN) == (size_t)k_ra8_etha_off_eausmfsecn,
              "EAUSMFSECN offset");
static_assert(offsetof(r_etha_regs_t, EATFECN) == (size_t)k_ra8_etha_off_eatfecn, "EATFECN offset");
static_assert(offsetof(r_etha_regs_t, EAFSECN) == (size_t)k_ra8_etha_off_eafsecn, "EAFSECN offset");
static_assert(offsetof(r_etha_regs_t, EADQOECN) == (size_t)k_ra8_etha_off_eadqoecn,
              "EADQOECN offset");
static_assert(offsetof(r_etha_regs_t, EADQSECN) == (size_t)k_ra8_etha_off_eadqsecn,
              "EADQSECN offset");
static_assert(offsetof(r_etha_regs_t, EAEIS0) == (size_t)k_ra8_etha_off_eaeis0, "EAEIS0 offset");
static_assert(offsetof(r_etha_regs_t, EAEIE0) == (size_t)k_ra8_etha_off_eaeie0, "EAEIE0 offset");
static_assert(offsetof(r_etha_regs_t, EAEID0) == (size_t)k_ra8_etha_off_eaeid0, "EAEID0 offset");
static_assert(offsetof(r_etha_regs_t, EAEIS1) == (size_t)k_ra8_etha_off_eaeis1, "EAEIS1 offset");
static_assert(offsetof(r_etha_regs_t, EAEIE1) == (size_t)k_ra8_etha_off_eaeie1, "EAEIE1 offset");
static_assert(offsetof(r_etha_regs_t, EAEID1) == (size_t)k_ra8_etha_off_eaeid1, "EAEID1 offset");
static_assert(offsetof(r_etha_regs_t, EAEIS2) == (size_t)k_ra8_etha_off_eaeis2, "EAEIS2 offset");
static_assert(offsetof(r_etha_regs_t, EAEIE2) == (size_t)k_ra8_etha_off_eaeie2, "EAEIE2 offset");
static_assert(offsetof(r_etha_regs_t, EAEID2) == (size_t)k_ra8_etha_off_eaeid2, "EAEID2 offset");
static_assert(offsetof(r_etha_regs_t, EACTDQDC) == (size_t)k_ra8_etha_off_eactdqdc,
              "EACTDQDC offset");
static_assert(offsetof(r_etha_regs_t, EACTDQM) == (size_t)k_ra8_etha_off_eactdqm, "EACTDQM offset");
static_assert(offsetof(r_etha_regs_t, EACTDQMLM) == (size_t)k_ra8_etha_off_eactdqmlm,
              "EACTDQMLM offset");
static_assert(offsetof(r_etha_regs_t, EATASCSTC1) == (size_t)k_ra8_etha_off_eatascstc1,
              "EATASCSTC1 offset");
static_assert(offsetof(r_etha_regs_t, EATASCSTM0) == (size_t)k_ra8_etha_off_eatascstm0,
              "EATASCSTM0 offset");
static_assert(offsetof(r_etha_regs_t, EATASCSTM1) == (size_t)k_ra8_etha_off_eatascstm1,
              "EATASCSTM1 offset");
static_assert(offsetof(r_etha_regs_t, EATASCTM) == (size_t)k_ra8_etha_off_eatasctm,
              "EATASCTM offset");
static_assert(offsetof(r_etha_regs_t, EATASGL1) == (size_t)k_ra8_etha_off_eatasgl1,
              "EATASGL1 offset");
static_assert(offsetof(r_etha_regs_t, EATASGLR) == (size_t)k_ra8_etha_off_eatasglr,
              "EATASGLR offset");
static_assert(offsetof(r_etha_regs_t, EATASGRR) == (size_t)k_ra8_etha_off_eatasgrr,
              "EATASGRR offset");
static_assert(offsetof(r_etha_regs_t, EATASRIRM) == (size_t)k_ra8_etha_off_eatasrirm,
              "EATASRIRM offset");

/**
 * @brief Get pointer to a per-port ETHA register block.
 *
 * @param[in] port Port identifier (k_ra8_etha_port_0 or k_ra8_etha_port_1).
 *
 * @return Pointer to the requested ETHA register window. If the port
 *         index is out of range, returns the port-0 window so the
 *         caller still sees deterministic behaviour rather than a
 *         null-pointer dereference. The caller is expected to have
 *         range-checked the argument before calling.
 *
 * @pre Power to the Ethernet subsystem is on (k_ra8_mstp_eswm enabled).
 * @pre ``port`` is one of ::ra8_etha_port_t.
 * @post Returned pointer is non-null and aligned to 4 bytes.
 *
 * @note Not thread-safe; per-port serialisation is the caller's job.
 * @since 0.1.0
 */
RA8_HW_REGISTER_ACCESS
static inline volatile r_etha_regs_t* ra8_etha(ra8_etha_port_t port)
{
  uintptr_t base = k_ra8_etha0_base_addr;
  if (port == k_ra8_etha_port_1) {
    base = k_ra8_etha1_base_addr;
  }
  return (volatile r_etha_regs_t*)base;
}

#ifdef __cplusplus
}
#endif
