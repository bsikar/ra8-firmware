/**
 * @file ra8_mipi_csi_regs.h
 * @brief MIPI CSI-2 Receiver register layout for the RA8D2 (HUM Ch 66)
 * @ingroup grp_hal_camera
 *
 * @details
 * The RA8D2 exposes a single MIPI CSI-2 receiver block at base
 * `0x40347000` (Secure) / `0x50347000` (Non-Secure). The peripheral
 * accepts up to 2 D-PHY data lanes at 80-720 Mbps/lane and forwards
 * decoded image data into the Video Input (VIN) module. It supports
 * MIPI CSI-2 V3.0 and MIPI D-PHY V2.5.
 *
 * The register window is 0x298 bytes long. The HUM specifies that
 * ALL register accesses must be in 32-bit units (HUM Ch 66.3
 * "Register Descriptions" p 3935: "Access the following registers
 * only in units of 32 bits.").
 *
 * ## Register summary (verified against HUM Ch 66 p 3935-3972)
 *
 * | Offset  | Name      | Width | Purpose                                         |
 * |--------:|-----------|------:|-------------------------------------------------|
 * |  0x000  | MCG       | 32    | Module configuration (RO IP info: VER/SDLN/GSNM)|
 * |  0x010  | MCT0      | 32    | Lanes, modes, ECC/LFSR knobs                    |
 * |  0x018  | MCT2      | 32    | Clock-rate adjust (FRRSKW / FRRCLK)             |
 * |  0x01C  | MCT3      | 32    | RXEN reception enable                           |
 * |  0x028  | RTCT      | 32    | Software reset control (VSRST W1)               |
 * |  0x02C  | RTST      | 32    | Software reset status (VSRSTS)                  |
 * |  0x040  | EPCT      | 32    | EPD Option Control (SLP/EPDOP/SSP/EPDEN)        |
 * |  0x044  | EMCT      | 32    | EPD Misc-Option (VLSIEN/EOTPEN)                 |
 * |  0x050  | MIST      | 32    | Module IRQ status (DL/PM/GST/RX/VC[15:0])       |
 * |  0x060  | DTEL      | 32    | Receive data-type enable low (DT 0x00-1F)       |
 * |  0x064  | DTEH      | 32    | Receive data-type enable high (DT 0x20-3F)      |
 * |  0x070  | RXST      | 32    | Receive status (FRMn[15:0], RACT, RACTDET)      |
 * |  0x074  | RXSC      | 32    | Receive status clear (RACTDETC)                 |
 * |  0x078  | RXIE      | 32    | Receive interrupt enable (RACTDETE)             |
 * |  0x080  | DLST0     | 32    | Data lane 0 status                              |
 * |  0x084  | DLSC0     | 32    | Data lane 0 status clear                        |
 * |  0x088  | DLIE0     | 32    | Data lane 0 interrupt enable                    |
 * |  0x090  | DLST1     | 32    | Data lane 1 status                              |
 * |  0x094  | DLSC1     | 32    | Data lane 1 status clear                        |
 * |  0x098  | DLIE1     | 32    | Data lane 1 interrupt enable                    |
 * |  0x100+ | VCST(M)   | 32    | Virtual channel M status (16 channels, +0x10)   |
 * |  0x104+ | VCSC(M)   | 32    | Virtual channel M status clear                  |
 * |  0x108+ | VCIE(M)   | 32    | Virtual channel M interrupt enable              |
 * |  0x200  | PMST      | 32    | Power-management status                         |
 * |  0x204  | PMSC      | 32    | Power-management status clear                   |
 * |  0x208  | PMIE      | 32    | Power-management interrupt enable               |
 * |  0x280  | GSCT      | 32    | Generic short packet control (SHTH/GFIF)        |
 * |  0x284  | GSST      | 32    | Generic short packet status (PNUM/GOV/...)      |
 * |  0x288  | GSSC      | 32    | Generic short packet status clear (GOVC)        |
 * |  0x28C  | GSIE      | 32    | Generic short packet interrupt enable           |
 * |  0x290  | GSHT      | 32    | Generic short packet header (FIFO read)         |
 * |  0x294  | GSIU      | 32    | Generic short packet info update (FINC/GFCLR/GFEN)|
 *
 * The MIPI CSI base address itself is declared in
 * ``ra8_glcdc_regs.h`` (alongside its display-block siblings
 * GLCDC, MIPI DSI, MIPI PHY, CEU) so all four display peripherals
 * share one address enum. We re-use ``k_ra8_mipi_csi_base_addr``
 * from there rather than redefining it.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_glcdc_regs.h" /* for k_ra8_mipi_csi_base_addr */

/**
 * @enum ra8_mipi_csi_off_t
 * @brief Absolute byte offsets into the MIPI CSI register window.
 *
 * @details
 * Names mirror the HUM Ch 66.3 register names (MCG, MCT0, MCT2,
 * ...). All offsets are 32-bit aligned per HUM Ch 66.3 "Register
 * Descriptions" p 3935.
 *
 * Only the first virtual channel (VC0) and data lane (DL0) are
 * enumerated explicitly; VC1..VC15 follow at +0x10 stride from
 * VCST0 / VCSC0 / VCIE0 and DL1 follows at +0x10 stride from
 * DLST0 / DLSC0 / DLIE0. Helpers ``ra8_mipi_csi_vc_off()`` and
 * ``ra8_mipi_csi_dl_off()`` below compute the offsets safely.
 */
typedef enum : uint16_t {
  /* ---- Module info / control ----------------------------------------- */
  k_ra8_mipi_csi_off_mcg  = 0x000U, /**< Module Configuration (RO).       */
  k_ra8_mipi_csi_off_mct0 = 0x010U, /**< Module Control 0 (lanes, modes). */
  k_ra8_mipi_csi_off_mct2 = 0x018U, /**< Module Control 2 (clock rates).  */
  k_ra8_mipi_csi_off_mct3 = 0x01CU, /**< Module Control 3 (RXEN).         */

  /* ---- Reset --------------------------------------------------------- */
  k_ra8_mipi_csi_off_rtct = 0x028U, /**< Reset Control (VSRST). */
  k_ra8_mipi_csi_off_rtst = 0x02CU, /**< Reset Status (VSRSTS). */

  /* ---- EPD options --------------------------------------------------- */
  k_ra8_mipi_csi_off_epct = 0x040U, /**< EPD Option Control.      */
  k_ra8_mipi_csi_off_emct = 0x044U, /**< EPD Misc-Option Control. */

  /* ---- Module-level interrupt status -------------------------------- */
  k_ra8_mipi_csi_off_mist = 0x050U, /**< Module Interrupt Status. */

  /* ---- Receive data-type enable ------------------------------------- */
  k_ra8_mipi_csi_off_dtel = 0x060U, /**< Receive Data-Type Enable Low.  */
  k_ra8_mipi_csi_off_dteh = 0x064U, /**< Receive Data-Type Enable High. */

  /* ---- Receive status / clear / IE ---------------------------------- */
  k_ra8_mipi_csi_off_rxst = 0x070U, /**< Receive Status.           */
  k_ra8_mipi_csi_off_rxsc = 0x074U, /**< Receive Status Clear.     */
  k_ra8_mipi_csi_off_rxie = 0x078U, /**< Receive Interrupt Enable. */

  /* ---- Data lane 0 status / clear / IE ------------------------------ */
  k_ra8_mipi_csi_off_dlst0 = 0x080U, /**< Data Lane 0 Status.           */
  k_ra8_mipi_csi_off_dlsc0 = 0x084U, /**< Data Lane 0 Status Clear.     */
  k_ra8_mipi_csi_off_dlie0 = 0x088U, /**< Data Lane 0 Interrupt Enable. */

  /* ---- Data lane 1 status / clear / IE ------------------------------ */
  k_ra8_mipi_csi_off_dlst1 = 0x090U, /**< Data Lane 1 Status.           */
  k_ra8_mipi_csi_off_dlsc1 = 0x094U, /**< Data Lane 1 Status Clear.     */
  k_ra8_mipi_csi_off_dlie1 = 0x098U, /**< Data Lane 1 Interrupt Enable. */

  /* ---- Virtual channel 0 (others at +0x10 stride) ------------------- */
  k_ra8_mipi_csi_off_vcst0 = 0x100U, /**< Virtual Channel 0 Status.       */
  k_ra8_mipi_csi_off_vcsc0 = 0x104U, /**< Virtual Channel 0 Status Clear. */
  k_ra8_mipi_csi_off_vcie0 = 0x108U, /**< Virtual Channel 0 IRQ Enable.   */

  /* ---- Power management --------------------------------------------- */
  k_ra8_mipi_csi_off_pmst = 0x200U, /**< Power-Management Status.       */
  k_ra8_mipi_csi_off_pmsc = 0x204U, /**< Power-Management Status Clear. */
  k_ra8_mipi_csi_off_pmie = 0x208U, /**< Power-Management IRQ Enable.   */

  /* ---- Generic short packet ----------------------------------------- */
  k_ra8_mipi_csi_off_gsct = 0x280U, /**< Generic Short Packet Control.     */
  k_ra8_mipi_csi_off_gsst = 0x284U, /**< Generic Short Packet Status.      */
  k_ra8_mipi_csi_off_gssc = 0x288U, /**< Generic Short Packet Stat Clear.  */
  k_ra8_mipi_csi_off_gsie = 0x28CU, /**< Generic Short Packet IRQ Enable.  */
  k_ra8_mipi_csi_off_gsht = 0x290U, /**< Generic Short Packet Header (RO). */
  k_ra8_mipi_csi_off_gsiu = 0x294U, /**< Generic Short Packet Info Update. */
} ra8_mipi_csi_off_t;

/**
 * @enum ra8_mipi_csi_limits_t
 * @brief Module-wide constants (channel counts, stride values).
 */
typedef enum : uint16_t {
  k_ra8_mipi_csi_vc_count    = 16U,    /**< Number of virtual channels.      */
  k_ra8_mipi_csi_vc_max      = 15U,    /**< Highest valid VC index.          */
  k_ra8_mipi_csi_vc_stride   = 0x10U,  /**< Bytes between VC blocks.         */
  k_ra8_mipi_csi_dl_count    = 2U,     /**< Number of data-lane blocks.      */
  k_ra8_mipi_csi_dl_max      = 1U,     /**< Highest valid DL index.          */
  k_ra8_mipi_csi_dl_stride   = 0x10U,  /**< Bytes between DL blocks.         */
  k_ra8_mipi_csi_data_lanes  = 2U,     /**< Max active D-PHY data lanes.     */
  k_ra8_mipi_csi_short_fifo  = 16U,    /**< Generic short-packet FIFO depth. */
  k_ra8_mipi_csi_window_size = 0x298U, /**< Total register window in bytes.  */
} ra8_mipi_csi_limits_t;

/**
 * @enum ra8_mipi_csi_mcg_bits_t
 * @brief MCG (Module Configuration, RO) field masks.
 *
 * @details Per HUM Ch 66.3.1 "MCG : Module Configuration Register" p 3936.
 * - VER  [3:0]   IP core version
 * - SDLN [11:8]  Number of supported data lanes (0x2 = 2 lanes)
 * - GSNM [23:16] Number of generic short packet FIFO stages (16)
 */
typedef enum : uint32_t {
  k_ra8_mipi_csi_mcg_ver_mask   = 0x0000000FUL, /**< [3:0] core version. */
  k_ra8_mipi_csi_mcg_sdln_mask  = 0x00000F00UL, /**< [11:8] sup. lanes.  */
  k_ra8_mipi_csi_mcg_sdln_shift = 8U,           /**< SDLN shift.         */
  k_ra8_mipi_csi_mcg_gsnm_mask  = 0x00FF0000UL, /**< [23:16] FIFO depth. */
  k_ra8_mipi_csi_mcg_gsnm_shift = 16U,          /**< GSNM shift.         */
} ra8_mipi_csi_mcg_bits_t;

/**
 * @enum ra8_mipi_csi_mct0_bits_t
 * @brief MCT0 (Module Control 0) bit masks.
 *
 * @details
 * Verified against HUM Ch 66.3.2 "MCT0 : Module Control Register 0"
 * p 3936-3937.
 */
typedef enum : uint32_t {
  k_ra8_mipi_csi_mct0_vdln_mask   = 0x0000000FUL, /**< [3:0] valid lane count.        */
  k_ra8_mipi_csi_mct0_zlmd_mask   = 0x00010000UL, /**< [16] zero-length-LP output.    */
  k_ra8_mipi_csi_mct0_edmd_mask   = 0x00020000UL, /**< [17] ErrFrameData notify.      */
  k_ra8_mipi_csi_mct0_rvmd_mask   = 0x00080000UL, /**< [19] Reserved-DT receive mode. */
  k_ra8_mipi_csi_mct0_grmd_mask   = 0x00100000UL, /**< [20] Generic CSI-2 rule mode.  */
  k_ra8_mipi_csi_mct0_eccv13_mask = 0x01000000UL, /**< [24] ECC v1.3 (24-bit) mode.   */
  k_ra8_mipi_csi_mct0_lfsren_mask = 0x02000000UL, /**< [25] LFSR descrambling enable. */
} ra8_mipi_csi_mct0_bits_t;

/**
 * @enum ra8_mipi_csi_mct2_bits_t
 * @brief MCT2 (Module Control 2) field masks/shifts.
 * @details Per HUM Ch 66.3.3 p 3937-3938.
 */
typedef enum : uint32_t {
  k_ra8_mipi_csi_mct2_frrclk_mask  = 0x000001FFUL, /**< [8:0] FRRCLK.   */
  k_ra8_mipi_csi_mct2_frrclk_shift = 0U,           /**< FRRCLK shift.   */
  k_ra8_mipi_csi_mct2_frrskw_mask  = 0x01FF0000UL, /**< [24:16] FRRSKW. */
  k_ra8_mipi_csi_mct2_frrskw_shift = 16U,          /**< FRRSKW shift.   */
} ra8_mipi_csi_mct2_bits_t;

/**
 * @enum ra8_mipi_csi_mct3_bits_t
 * @brief MCT3 (Module Control 3) bit masks. The only writable bit is
 *        RXEN, which gates D-PHY reception.
 *
 * @details Verified against HUM Ch 66.3.4 "MCT3 : Module Control
 * Register 3" p 3938.
 */
typedef enum : uint32_t {
  k_ra8_mipi_csi_mct3_rxen_mask = 0x00000001UL, /**< [0] RX enable. */
} ra8_mipi_csi_mct3_bits_t;

/**
 * @enum ra8_mipi_csi_rtct_bits_t
 * @brief RTCT (Reset Control) bits -- write 1 to VSRST to reset the
 *        Video-Pixel interface.
 *
 * @details Verified against HUM Ch 66.3.5 "RTCT : Reset Control
 * Register" p 3938.
 */
typedef enum : uint32_t {
  k_ra8_mipi_csi_rtct_vsrst_mask = 0x00000001UL, /**< [0] software reset. */
} ra8_mipi_csi_rtct_bits_t;

/**
 * @enum ra8_mipi_csi_rtst_bits_t
 * @brief RTST (Reset Status) bits -- VSRSTS reads 1 while a reset is
 *        in progress, 0 once it has completed.
 */
typedef enum : uint32_t {
  k_ra8_mipi_csi_rtst_vsrsts_mask = 0x00000001UL, /**< [0] reset busy. */
} ra8_mipi_csi_rtst_bits_t;

/**
 * @enum ra8_mipi_csi_epct_bits_t
 * @brief EPCT (EPD Option Control) field masks.
 * @details Per HUM Ch 66.3.7 "EPCT" p 3939-3940.
 *  - SLP   [14:0]  Long packet spacers
 *  - EPDOP [15]    EPD option select (must be 1 / Option 2)
 *  - SSP   [30:16] Short packet spacers
 *  - EPDEN [31]    EPD operation enable
 */
typedef enum : uint32_t {
  k_ra8_mipi_csi_epct_slp_mask   = 0x00007FFFUL, /**< [14:0] long spacers. */
  k_ra8_mipi_csi_epct_slp_shift  = 0U,           /**< SLP shift.           */
  k_ra8_mipi_csi_epct_epdop_mask = 0x00008000UL, /**< [15] option 2 sel.   */
  k_ra8_mipi_csi_epct_ssp_mask   = 0x7FFF0000UL, /**< [30:16] short spc.   */
  k_ra8_mipi_csi_epct_ssp_shift  = 16U,          /**< SSP shift.           */
  k_ra8_mipi_csi_epct_epden_mask = 0x80000000UL, /**< [31] EPD enable.     */
} ra8_mipi_csi_epct_bits_t;

/**
 * @enum ra8_mipi_csi_emct_bits_t
 * @brief EMCT (EPD Misc Option Control) field masks.
 * @details Per HUM Ch 66.3.8 "EMCT" p 3940-3941.
 *  - VLSIEN [5:4] Variable length spacer mode
 *  - EOTPEN [6]   End-of-Transmission Packet enable
 */
typedef enum : uint32_t {
  k_ra8_mipi_csi_emct_vlsien_mask  = 0x00000030UL, /**< [5:4] var spacer. */
  k_ra8_mipi_csi_emct_vlsien_shift = 4U,           /**< VLSIEN shift.     */
  k_ra8_mipi_csi_emct_eotpen_mask  = 0x00000040UL, /**< [6] EOTP enable.  */
} ra8_mipi_csi_emct_bits_t;

/**
 * @enum ra8_mipi_csi_vlsien_t
 * @brief Encodings for EMCT.VLSIEN[5:4] (HUM Ch 66.3.8 p 3941).
 */
typedef enum : uint8_t {
  k_ra8_mipi_csi_vlsien_fixed = 0U, /**< 00b fixed-length spacer.           */
  k_ra8_mipi_csi_vlsien_x1    = 1U, /**< 01b spacer = 1 x n/lane (1L proh.) */
  k_ra8_mipi_csi_vlsien_x2    = 2U, /**< 10b spacer = 2 x n/lane.           */
  k_ra8_mipi_csi_vlsien_x4    = 3U, /**< 11b spacer = 4 x n/lane.           */
} ra8_mipi_csi_vlsien_t;

/**
 * @enum ra8_mipi_csi_mist_bits_t
 * @brief MIST (Module Interrupt Status, RO) bits.
 * @details Per HUM Ch 66.3.9 "MIST" p 3941-3942. Read-only summary
 * register; clear is by clearing the underlying source registers.
 */
typedef enum : uint32_t {
  k_ra8_mipi_csi_mist_dl0s_mask = 0x00000001UL, /**< [0] DL0 has IRQ src. */
  k_ra8_mipi_csi_mist_dl1s_mask = 0x00000002UL, /**< [1] DL1 has IRQ src. */
  k_ra8_mipi_csi_mist_pms_mask  = 0x00000100UL, /**< [8] PM has IRQ src.  */
  k_ra8_mipi_csi_mist_gsts_mask = 0x00000200UL, /**< [9] GST has IRQ src. */
  k_ra8_mipi_csi_mist_rxs_mask  = 0x00000400UL, /**< [10] RX has IRQ src. */
  k_ra8_mipi_csi_mist_vc_mask   = 0xFFFF0000UL, /**< [31:16] per-VC src.  */
  k_ra8_mipi_csi_mist_vc_shift  = 16U,          /**< VC mask shift.       */
} ra8_mipi_csi_mist_bits_t;

/**
 * @enum ra8_mipi_csi_dt_low_bits_t
 * @brief DTEL (Receive Data Type Enable Low) per-bit masks
 *        (HUM Ch 66.3.10 p 3943, Table 66.2).
 *
 * @details Bits [3:0] are reserved (fixed to 1 by hardware). Bits
 * [7:5] are reserved (fixed to 0). Bits [29:16] are reserved
 * (fixed to 0). The active bits are EOT, GSP1..GSP8, YUV422_8/10.
 */
typedef enum : uint32_t {
  k_ra8_mipi_csi_dtel_reserved_low_mask = 0x0000000FUL, /**< [3:0] fixed 1. */
  k_ra8_mipi_csi_dtel_eot_mask          = 0x00000010UL, /**< [4] EoT pkt.   */
  k_ra8_mipi_csi_dtel_gsp1_mask         = 0x00000100UL, /**< [8]  GSP 1.    */
  k_ra8_mipi_csi_dtel_gsp2_mask         = 0x00000200UL, /**< [9]  GSP 2.    */
  k_ra8_mipi_csi_dtel_gsp3_mask         = 0x00000400UL, /**< [10] GSP 3.    */
  k_ra8_mipi_csi_dtel_gsp4_mask         = 0x00000800UL, /**< [11] GSP 4.    */
  k_ra8_mipi_csi_dtel_gsp5_mask         = 0x00001000UL, /**< [12] GSP 5.    */
  k_ra8_mipi_csi_dtel_gsp6_mask         = 0x00002000UL, /**< [13] GSP 6.    */
  k_ra8_mipi_csi_dtel_gsp7_mask         = 0x00004000UL, /**< [14] GSP 7.    */
  k_ra8_mipi_csi_dtel_gsp8_mask         = 0x00008000UL, /**< [15] GSP 8.    */
  k_ra8_mipi_csi_dtel_gsp_all_mask      = 0x0000FF00UL, /**< [15:8] all GSP */
  k_ra8_mipi_csi_dtel_yuv422_8_mask     = 0x40000000UL, /**< [30] YUV 8b.   */
  k_ra8_mipi_csi_dtel_yuv422_10_mask    = 0x80000000UL, /**< [31] YUV 10b.  */
} ra8_mipi_csi_dt_low_bits_t;

/**
 * @enum ra8_mipi_csi_dt_high_bits_t
 * @brief DTEH (Receive Data Type Enable High) per-bit masks
 *        (HUM Ch 66.3.11 p 3944, Table 66.3).
 *
 * @details Active bits in DT 0x20-0x3F: RGB888 (bit 4), RAW8 (bit 10).
 * All other bits must be left at their reset value (0).
 */
typedef enum : uint32_t {
  k_ra8_mipi_csi_dteh_rgb888_mask = 0x00000010UL, /**< [4]  RGB888. */
  k_ra8_mipi_csi_dteh_raw8_mask   = 0x00000400UL, /**< [10] RAW8.   */
} ra8_mipi_csi_dt_high_bits_t;

/**
 * @enum ra8_mipi_csi_rxst_bits_t
 * @brief RXST (Receive Status) bits.
 *
 * @details
 * Bits [15:0] are FRMn (frame active for VC0..VC15). Bit 16 is RACT
 * (live reception), bit 17 is RACTDET (sticky reception detect).
 * HUM Ch 66.3.12 p 3944-3945.
 */
typedef enum : uint32_t {
  k_ra8_mipi_csi_rxst_frm_mask     = 0x0000FFFFUL, /**< [15:0] FRMn flags. */
  k_ra8_mipi_csi_rxst_ract_mask    = 0x00010000UL, /**< [16] RX active.    */
  k_ra8_mipi_csi_rxst_ractdet_mask = 0x00020000UL, /**< [17] RX detected.  */
} ra8_mipi_csi_rxst_bits_t;

/**
 * @enum ra8_mipi_csi_rxsc_bits_t
 * @brief RXSC (Receive Status Clear) -- write 1 to clear RACTDET.
 * @details HUM Ch 66.3.13 p 3945.
 */
typedef enum : uint32_t {
  k_ra8_mipi_csi_rxsc_ractdetc_mask = 0x00020000UL, /**< [17] clear RXDET. */
} ra8_mipi_csi_rxsc_bits_t;

/**
 * @enum ra8_mipi_csi_rxie_bits_t
 * @brief RXIE (Receive Interrupt Enable) -- enables RACTDET interrupt.
 * @details HUM Ch 66.3.14 p 3946.
 */
typedef enum : uint32_t {
  k_ra8_mipi_csi_rxie_ractdete_mask = 0x00020000UL, /**< [17] enable IRQ. */
} ra8_mipi_csi_rxie_bits_t;

/**
 * @enum ra8_mipi_csi_dlst_bits_t
 * @brief DLST(N) (Data Lane N Status) bits.
 * @details HUM Ch 66.3.15 p 3946-3947.
 */
typedef enum : uint32_t {
  k_ra8_mipi_csi_dlst_esh_mask = 0x00000001UL, /**< [0]  ErrSotHs.          */
  k_ra8_mipi_csi_dlst_ess_mask = 0x00000002UL, /**< [1]  ErrSotSyncHs.      */
  k_ra8_mipi_csi_dlst_ect_mask = 0x00000004UL, /**< [2]  ErrControl.        */
  k_ra8_mipi_csi_dlst_ees_mask = 0x00000008UL, /**< [3]  ErrEsc.            */
  k_ra8_mipi_csi_dlst_eul_mask = 0x00010000UL, /**< [16] Exit ULPS.         */
  k_ra8_mipi_csi_dlst_rul_mask = 0x00020000UL, /**< [17] Enter ULPS.        */
  k_ra8_mipi_csi_dlst_ulp_mask = 0x01000000UL, /**< [24] RxUlpsEsc state.   */
  k_ra8_mipi_csi_dlst_w1c_mask = 0x0003000FUL, /**< Bits clearable by DLSC. */
} ra8_mipi_csi_dlst_bits_t;

/**
 * @enum ra8_mipi_csi_dlsc_bits_t
 * @brief DLSC(N) (Data Lane N Status Clear) bits (W1C).
 * @details HUM Ch 66.3.16 p 3947-3948.
 */
typedef enum : uint32_t {
  k_ra8_mipi_csi_dlsc_eshc_mask = 0x00000001UL, /**< [0]  clear ESH. */
  k_ra8_mipi_csi_dlsc_essc_mask = 0x00000002UL, /**< [1]  clear ESS. */
  k_ra8_mipi_csi_dlsc_ectc_mask = 0x00000004UL, /**< [2]  clear ECT. */
  k_ra8_mipi_csi_dlsc_eesc_mask = 0x00000008UL, /**< [3]  clear EES. */
  k_ra8_mipi_csi_dlsc_eulc_mask = 0x00010000UL, /**< [16] clear EUL. */
  k_ra8_mipi_csi_dlsc_rulc_mask = 0x00020000UL, /**< [17] clear RUL. */
  k_ra8_mipi_csi_dlsc_all_mask  = 0x0003000FUL, /**< All clearable.  */
} ra8_mipi_csi_dlsc_bits_t;

/**
 * @enum ra8_mipi_csi_dlie_bits_t
 * @brief DLIE(N) (Data Lane N Interrupt Enable) bits.
 * @details HUM Ch 66.3.17 p 3948.
 */
typedef enum : uint32_t {
  k_ra8_mipi_csi_dlie_eshe_mask = 0x00000001UL, /**< [0]  ESH IRQ enable. */
  k_ra8_mipi_csi_dlie_esse_mask = 0x00000002UL, /**< [1]  ESS IRQ enable. */
  k_ra8_mipi_csi_dlie_ecte_mask = 0x00000004UL, /**< [2]  ECT IRQ enable. */
  k_ra8_mipi_csi_dlie_eese_mask = 0x00000008UL, /**< [3]  EES IRQ enable. */
  k_ra8_mipi_csi_dlie_eule_mask = 0x00010000UL, /**< [16] EUL IRQ enable. */
  k_ra8_mipi_csi_dlie_rule_mask = 0x00020000UL, /**< [17] RUL IRQ enable. */
  k_ra8_mipi_csi_dlie_all_mask  = 0x0003000FUL, /**< Enable everything.   */
} ra8_mipi_csi_dlie_bits_t;

/**
 * @enum ra8_mipi_csi_vcst_bits_t
 * @brief VCST(M) (Virtual Channel M Status) bit masks.
 * @details HUM Ch 66.3.18 p 3949-3950.
 */
typedef enum : uint32_t {
  k_ra8_mipi_csi_vcst_mlf_mask = 0x00000001UL, /**< [0]  Malformed packet. */
  k_ra8_mipi_csi_vcst_ecd_mask = 0x00000002UL, /**< [1]  ECC 2-bit error.  */
  k_ra8_mipi_csi_vcst_crc_mask = 0x00000004UL, /**< [2]  CRC error.        */
  k_ra8_mipi_csi_vcst_ide_mask = 0x00000008UL, /**< [3]  ErrID.            */
  k_ra8_mipi_csi_vcst_wce_mask = 0x00000010UL, /**< [4]  Word count err.   */
  k_ra8_mipi_csi_vcst_ecc_mask = 0x00000020UL, /**< [5]  ECC 1-bit corr.   */
  k_ra8_mipi_csi_vcst_ecn_mask = 0x00000040UL, /**< [6]  ECC no-error.     */
  k_ra8_mipi_csi_vcst_frs_mask = 0x00000100UL, /**< [8]  ErrFrameSync.     */
  k_ra8_mipi_csi_vcst_frd_mask = 0x00000200UL, /**< [9]  ErrFrameData.     */
  k_ra8_mipi_csi_vcst_ovf_mask = 0x00010000UL, /**< [16] GSFIFO ovf disc.  */
  k_ra8_mipi_csi_vcst_fsr_mask = 0x01000000UL, /**< [24] Frame Start.      */
  k_ra8_mipi_csi_vcst_fer_mask = 0x02000000UL, /**< [25] Frame End.        */
  k_ra8_mipi_csi_vcst_lsr_mask = 0x04000000UL, /**< [26] Line Start.       */
  k_ra8_mipi_csi_vcst_ler_mask = 0x08000000UL, /**< [27] Line End.         */
  k_ra8_mipi_csi_vcst_etr_mask = 0x10000000UL, /**< [28] EoTp received.    */
  /** Errors that affect every VC because the VC of the offending
   *  packet cannot be determined (MLF + ECD). */
  k_ra8_mipi_csi_vcst_generic_err_mask = 0x00000003UL,
} ra8_mipi_csi_vcst_bits_t;

/**
 * @enum ra8_mipi_csi_vcsc_bits_t
 * @brief VCSC(M) (Virtual Channel M Status Clear) bits (W1C).
 * @details HUM Ch 66.3.19 p 3951-3952.
 */
typedef enum : uint32_t {
  k_ra8_mipi_csi_vcsc_mlfc_mask   = 0x00000001UL, /**< [0]  clear MLF.      */
  k_ra8_mipi_csi_vcsc_ecdc_mask   = 0x00000002UL, /**< [1]  clear ECD.      */
  k_ra8_mipi_csi_vcsc_crcc_mask   = 0x00000004UL, /**< [2]  clear CRC.      */
  k_ra8_mipi_csi_vcsc_idec_mask   = 0x00000008UL, /**< [3]  clear IDE.      */
  k_ra8_mipi_csi_vcsc_wcec_mask   = 0x00000010UL, /**< [4]  clear WCE.      */
  k_ra8_mipi_csi_vcsc_eccc_mask   = 0x00000020UL, /**< [5]  clear ECC.      */
  k_ra8_mipi_csi_vcsc_ecnc_mask   = 0x00000040UL, /**< [6]  clear ECN.      */
  k_ra8_mipi_csi_vcsc_frsc_mask   = 0x00000100UL, /**< [8]  clear FRS.      */
  k_ra8_mipi_csi_vcsc_frdc_mask   = 0x00000200UL, /**< [9]  clear FRD.      */
  k_ra8_mipi_csi_vcsc_amlfc_mask  = 0x00004000UL, /**< [14] clear MLF all.  */
  k_ra8_mipi_csi_vcsc_aecdc_mask  = 0x00008000UL, /**< [15] clear ECD all.  */
  k_ra8_mipi_csi_vcsc_ovfc_mask   = 0x00010000UL, /**< [16] clear OVF.      */
  k_ra8_mipi_csi_vcsc_fsrc_mask   = 0x01000000UL, /**< [24] clear FSR.      */
  k_ra8_mipi_csi_vcsc_ferc_mask   = 0x02000000UL, /**< [25] clear FER.      */
  k_ra8_mipi_csi_vcsc_lsrc_mask   = 0x04000000UL, /**< [26] clear LSR.      */
  k_ra8_mipi_csi_vcsc_lerc_mask   = 0x08000000UL, /**< [27] clear LER.      */
  k_ra8_mipi_csi_vcsc_etrc_mask   = 0x10000000UL, /**< [28] clear ETR.      */
  k_ra8_mipi_csi_vcsc_per_vc_mask = 0x1F01037FUL, /**< All per-VC W1C bits. */
} ra8_mipi_csi_vcsc_bits_t;

/**
 * @enum ra8_mipi_csi_vcie_bits_t
 * @brief VCIE(M) (Virtual Channel M Interrupt Enable) bits.
 * @details HUM Ch 66.3.20 p 3952-3953. Bit positions match VCST.
 */
typedef enum : uint32_t {
  k_ra8_mipi_csi_vcie_mlfe_mask = 0x00000001UL, /**< [0]  MLF IRQ enable. */
  k_ra8_mipi_csi_vcie_ecde_mask = 0x00000002UL, /**< [1]  ECD IRQ enable. */
  k_ra8_mipi_csi_vcie_crce_mask = 0x00000004UL, /**< [2]  CRC IRQ enable. */
  k_ra8_mipi_csi_vcie_idee_mask = 0x00000008UL, /**< [3]  IDE IRQ enable. */
  k_ra8_mipi_csi_vcie_wcee_mask = 0x00000010UL, /**< [4]  WCE IRQ enable. */
  k_ra8_mipi_csi_vcie_ecce_mask = 0x00000020UL, /**< [5]  ECC IRQ enable. */
  k_ra8_mipi_csi_vcie_ecne_mask = 0x00000040UL, /**< [6]  ECN IRQ enable. */
  k_ra8_mipi_csi_vcie_frse_mask = 0x00000100UL, /**< [8]  FRS IRQ enable. */
  k_ra8_mipi_csi_vcie_frde_mask = 0x00000200UL, /**< [9]  FRD IRQ enable. */
  k_ra8_mipi_csi_vcie_ovfe_mask = 0x00010000UL, /**< [16] OVF IRQ enable. */
  k_ra8_mipi_csi_vcie_fsre_mask = 0x01000000UL, /**< [24] FSR IRQ enable. */
  k_ra8_mipi_csi_vcie_fere_mask = 0x02000000UL, /**< [25] FER IRQ enable. */
  k_ra8_mipi_csi_vcie_lsre_mask = 0x04000000UL, /**< [26] LSR IRQ enable. */
  k_ra8_mipi_csi_vcie_lere_mask = 0x08000000UL, /**< [27] LER IRQ enable. */
  k_ra8_mipi_csi_vcie_etre_mask = 0x10000000UL, /**< [28] ETR IRQ enable. */
  k_ra8_mipi_csi_vcie_all_mask  = 0x1F01037FUL, /**< Enable everything.   */
} ra8_mipi_csi_vcie_bits_t;

/**
 * @enum ra8_mipi_csi_pmst_bits_t
 * @brief PMST (Power Management Status) bit masks.
 * @details HUM Ch 66.3.21 p 3954-3955.
 */
typedef enum : uint32_t {
  k_ra8_mipi_csi_pmst_dsx_mask  = 0x00000001UL, /**< [0]  Data exit STOP.   */
  k_ra8_mipi_csi_pmst_dsn_mask  = 0x00000002UL, /**< [1]  Data enter STOP.  */
  k_ra8_mipi_csi_pmst_csx_mask  = 0x00000004UL, /**< [2]  Clk exit STOP.    */
  k_ra8_mipi_csi_pmst_csn_mask  = 0x00000008UL, /**< [3]  Clk enter STOP.   */
  k_ra8_mipi_csi_pmst_dux_mask  = 0x00000010UL, /**< [4]  Data exit ULPS.   */
  k_ra8_mipi_csi_pmst_dun_mask  = 0x00000020UL, /**< [5]  Data enter ULPS.  */
  k_ra8_mipi_csi_pmst_cux_mask  = 0x00000040UL, /**< [6]  Clk exit ULPS.    */
  k_ra8_mipi_csi_pmst_cun_mask  = 0x00000080UL, /**< [7]  Clk enter ULPS.   */
  k_ra8_mipi_csi_pmst_clss_mask = 0x00004000UL, /**< [14] Clk STOP state.   */
  k_ra8_mipi_csi_pmst_clul_mask = 0x00008000UL, /**< [15] Clk ULPS state.   */
  k_ra8_mipi_csi_pmst_dlss_mask = 0x00030000UL, /**< [17:16] DL stop state. */
  k_ra8_mipi_csi_pmst_dlul_mask = 0x03000000UL, /**< [25:24] DL ULPS state. */
  k_ra8_mipi_csi_pmst_w1c_mask  = 0x000000FFUL, /**< Lower 8 bits W1C-clr.  */
} ra8_mipi_csi_pmst_bits_t;

/**
 * @enum ra8_mipi_csi_pmsc_bits_t
 * @brief PMSC (Power Management Status Clear) bits (W1C).
 * @details HUM Ch 66.3.22 p 3955.
 */
typedef enum : uint32_t {
  k_ra8_mipi_csi_pmsc_dsxc_mask = 0x00000001UL, /**< [0] clear DSX.    */
  k_ra8_mipi_csi_pmsc_dsnc_mask = 0x00000002UL, /**< [1] clear DSN.    */
  k_ra8_mipi_csi_pmsc_csxc_mask = 0x00000004UL, /**< [2] clear CSX.    */
  k_ra8_mipi_csi_pmsc_csnc_mask = 0x00000008UL, /**< [3] clear CSN.    */
  k_ra8_mipi_csi_pmsc_duxc_mask = 0x00000010UL, /**< [4] clear DUX.    */
  k_ra8_mipi_csi_pmsc_dunc_mask = 0x00000020UL, /**< [5] clear DUN.    */
  k_ra8_mipi_csi_pmsc_cuxc_mask = 0x00000040UL, /**< [6] clear CUX.    */
  k_ra8_mipi_csi_pmsc_cunc_mask = 0x00000080UL, /**< [7] clear CUN.    */
  k_ra8_mipi_csi_pmsc_all_mask  = 0x000000FFUL, /**< Clear everything. */
} ra8_mipi_csi_pmsc_bits_t;

/**
 * @enum ra8_mipi_csi_pmie_bits_t
 * @brief PMIE (Power Management Interrupt Enable) bits.
 * @details HUM Ch 66.3.23 p 3956.
 */
typedef enum : uint32_t {
  k_ra8_mipi_csi_pmie_dsxe_mask = 0x00000001UL, /**< [0] DSX IRQ enable. */
  k_ra8_mipi_csi_pmie_dsne_mask = 0x00000002UL, /**< [1] DSN IRQ enable. */
  k_ra8_mipi_csi_pmie_csxe_mask = 0x00000004UL, /**< [2] CSX IRQ enable. */
  k_ra8_mipi_csi_pmie_csne_mask = 0x00000008UL, /**< [3] CSN IRQ enable. */
  k_ra8_mipi_csi_pmie_duxe_mask = 0x00000010UL, /**< [4] DUX IRQ enable. */
  k_ra8_mipi_csi_pmie_dune_mask = 0x00000020UL, /**< [5] DUN IRQ enable. */
  k_ra8_mipi_csi_pmie_cuxe_mask = 0x00000040UL, /**< [6] CUX IRQ enable. */
  k_ra8_mipi_csi_pmie_cune_mask = 0x00000080UL, /**< [7] CUN IRQ enable. */
  k_ra8_mipi_csi_pmie_all_mask  = 0x000000FFUL, /**< Enable everything.  */
} ra8_mipi_csi_pmie_bits_t;

/**
 * @enum ra8_mipi_csi_gsct_bits_t
 * @brief GSCT (Generic Short Packet Control) field masks.
 * @details HUM Ch 66.3.24 p 3957.
 *  - SHTH [6:0]  Stored short-packet threshold (FIFO depth - 1)
 *  - GFIF [16]   Store received short packets in FIFO (reset = 1)
 */
typedef enum : uint32_t {
  k_ra8_mipi_csi_gsct_shth_mask  = 0x0000007FUL, /**< [6:0] threshold.     */
  k_ra8_mipi_csi_gsct_shth_shift = 0U,           /**< SHTH shift.          */
  k_ra8_mipi_csi_gsct_gfif_mask  = 0x00010000UL, /**< [16] FIFO store en.  */
  k_ra8_mipi_csi_gsct_shth_max   = 0x0FUL,       /**< Max meaningful SHTH. */
} ra8_mipi_csi_gsct_bits_t;

/**
 * @enum ra8_mipi_csi_gsst_bits_t
 * @brief GSST (Generic Short Packet Status) bit layout.
 *
 * @details HUM Ch 66.3.25 p 3957-3958. PNUM is at [15:8]; GOV at
 * bit [4]; GCD at bit [16]; STRDS at bit [17]. The lower flag bits
 * (GNE bit 0, GTH bit 1) are level-sensitive while data is queued.
 */
typedef enum : uint32_t {
  k_ra8_mipi_csi_gsst_gne_mask   = 0x00000001UL, /**< [0]  FIFO not empty.  */
  k_ra8_mipi_csi_gsst_gth_mask   = 0x00000002UL, /**< [1]  Threshold met.   */
  k_ra8_mipi_csi_gsst_gov_mask   = 0x00000010UL, /**< [4]  FIFO overflow.   */
  k_ra8_mipi_csi_gsst_pnum_mask  = 0x0000FF00UL, /**< [15:8] FIFO depth.    */
  k_ra8_mipi_csi_gsst_pnum_shift = 8U,           /**< PNUM shift.           */
  k_ra8_mipi_csi_gsst_gcd_mask   = 0x00010000UL, /**< [16] FIFO clear stat. */
  k_ra8_mipi_csi_gsst_strds_mask = 0x00020000UL, /**< [17] Store disabled.  */
} ra8_mipi_csi_gsst_bits_t;

/**
 * @enum ra8_mipi_csi_gssc_bits_t
 * @brief GSSC (Generic Short Packet Status Clear) bits (W1C).
 * @details HUM Ch 66.3.26 p 3958.
 */
typedef enum : uint32_t {
  k_ra8_mipi_csi_gssc_govc_mask = 0x00000010UL, /**< [4] clear GOV. */
} ra8_mipi_csi_gssc_bits_t;

/**
 * @enum ra8_mipi_csi_gsie_bits_t
 * @brief GSIE (Generic Short Packet Interrupt Enable) bits.
 * @details HUM Ch 66.3.27 p 3958-3959.
 */
typedef enum : uint32_t {
  k_ra8_mipi_csi_gsie_gnee_mask = 0x00000001UL, /**< [0] GNE IRQ enable. */
  k_ra8_mipi_csi_gsie_gthe_mask = 0x00000002UL, /**< [1] GTH IRQ enable. */
  k_ra8_mipi_csi_gsie_gove_mask = 0x00000010UL, /**< [4] GOV IRQ enable. */
  k_ra8_mipi_csi_gsie_all_mask  = 0x00000013UL, /**< Enable everything.  */
} ra8_mipi_csi_gsie_bits_t;

/**
 * @enum ra8_mipi_csi_gsht_bits_t
 * @brief GSHT (Generic Short Packet Header, RO) field masks.
 * @details HUM Ch 66.3.28 p 3959.
 *  - SPDT [15:0]  Stored 16-bit user data
 *  - DTYP [21:16] Data type (0x08-0x0F generic short packet codes)
 *  - SPVC [27:24] Virtual channel of the stored packet
 */
typedef enum : uint32_t {
  k_ra8_mipi_csi_gsht_spdt_mask  = 0x0000FFFFUL, /**< [15:0] payload.       */
  k_ra8_mipi_csi_gsht_spdt_shift = 0U,           /**< SPDT shift.           */
  k_ra8_mipi_csi_gsht_dtyp_mask  = 0x003F0000UL, /**< [21:16] data type.    */
  k_ra8_mipi_csi_gsht_dtyp_shift = 16U,          /**< DTYP shift.           */
  k_ra8_mipi_csi_gsht_spvc_mask  = 0x0F000000UL, /**< [27:24] VC of packet. */
  k_ra8_mipi_csi_gsht_spvc_shift = 24U,          /**< SPVC shift.           */
} ra8_mipi_csi_gsht_bits_t;

/**
 * @enum ra8_mipi_csi_gsiu_bits_t
 * @brief GSIU (Generic Short Packet Information Update) bits.
 * @details HUM Ch 66.3.29 p 3960.
 *  - FINC  [0]   Increment FIFO read pointer (W1)
 *  - GFCLR [8]   Clear the GS FIFO (R/W; write 1 to request, 0 to cancel)
 *  - GFEN  [16]  Re-enable storing after overflow (W1)
 */
typedef enum : uint32_t {
  k_ra8_mipi_csi_gsiu_finc_mask  = 0x00000001UL, /**< [0]  FIFO increment. */
  k_ra8_mipi_csi_gsiu_gfclr_mask = 0x00000100UL, /**< [8]  FIFO clear req. */
  k_ra8_mipi_csi_gsiu_gfen_mask  = 0x00010000UL, /**< [16] FIFO re-enable. */
} ra8_mipi_csi_gsiu_bits_t;

/**
 * @brief Get a 32-bit volatile pointer to a register inside the MIPI
 *        CSI window.
 *
 * @details
 * All MIPI CSI register accesses are 32 bits wide per HUM Ch 66.3
 * "Register Descriptions" p 3935. Going through this accessor (rather
 * than raw `(*(volatile uint32_t*)addr)` casts) lets the host-side
 * unit-test fake mmap intercept the access and back it with ordinary
 * heap memory.
 *
 * @param[in] off One of the ``k_ra8_mipi_csi_off_*`` enum values.
 * @return Volatile 32-bit pointer to the register.
 */
RA8_HW_REGISTER_ACCESS
static inline volatile uint32_t* ra8_mipi_csi_reg32(ra8_mipi_csi_off_t off)
{
  return (volatile uint32_t*)(k_ra8_mipi_csi_base_addr + (uint16_t)off);
}

/**
 * @brief Compute the byte offset of a per-virtual-channel register.
 *
 * @param[in] base One of ``k_ra8_mipi_csi_off_vcst0`` /
 *                 ``k_ra8_mipi_csi_off_vcsc0`` /
 *                 ``k_ra8_mipi_csi_off_vcie0`` (caller chooses status,
 *                 clear, or interrupt-enable).
 * @param[in] vc   Virtual channel index 0..15. Caller is expected to
 *                 validate.
 * @return The combined offset for the chosen VC.
 *
 * @since 0.1.0
 *
 * @details See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 */
static inline ra8_mipi_csi_off_t ra8_mipi_csi_vc_off(ra8_mipi_csi_off_t base, uint8_t vc)
{
  /* Computed stride offset is a valid HUM-defined VC register location, not a
   * literal enumerator -- the analyzer can't see that. */
  return (ra8_mipi_csi_off_t)((uint16_t)base + ((uint16_t)vc * k_ra8_mipi_csi_vc_stride));
}

/**
 * @brief Compute the byte offset of a per-data-lane register.
 *
 * @param[in] base One of ``k_ra8_mipi_csi_off_dlst0`` /
 *                 ``k_ra8_mipi_csi_off_dlsc0`` /
 *                 ``k_ra8_mipi_csi_off_dlie0``.
 * @param[in] lane Data lane index 0..1.
 * @return The combined offset for the chosen lane.
 *
 * @since 0.1.0
 *
 * @details See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 */
static inline ra8_mipi_csi_off_t ra8_mipi_csi_dl_off(ra8_mipi_csi_off_t base, uint8_t lane)
{
  return (ra8_mipi_csi_off_t)((uint16_t)base + ((uint16_t)lane * k_ra8_mipi_csi_dl_stride));
}

#ifdef __cplusplus
}
#endif
