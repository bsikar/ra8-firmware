/**
 * @file ra8_vin_regs.h
 * @brief Video Input Module (VIN) register layout for the RA8D2
 * @ingroup grp_hal_camera
 *
 * @details
 * The Video Input Module sits at the AXI/PCLKA boundary between the
 * MIPI CSI-2 receiver and SDRAM. It receives image data from the
 * camera, applies optional pre-clip / colour-space / scaling / dither
 * conversions, and writes the resulting frames into one of three
 * memory-base buffers (MB1, MB2, MB3) for consumption by GLCDC, DRW2D,
 * or DMA.
 *
 * On the RA8D2 there is a single VIN0 instance at base
 * ``0x40347400``. All registers are 32-bit-only access (HUM Ch 67.2
 * "Register Description" p 3974, "Access the following registers
 * only in units of 32 bits"). The driver exposes a typed-enum offset
 * table plus an inline accessor that returns a `volatile uint32_t*`
 * so the host-side `ra8_fake_mmap` shim can intercept every read/write.
 *
 * ## Register window summary
 *
 *  | Offset | Name             | Purpose                                  |
 *  |-------:|------------------|------------------------------------------|
 *  | 0x000  | MC               | Main control                             |
 *  | 0x004  | MS               | Module status (R)                        |
 *  | 0x008  | FC               | Frame capture                            |
 *  | 0x00C  | SLPRC            | Pre-clip start line                      |
 *  | 0x010  | ELPRC            | Pre-clip end line                        |
 *  | 0x014  | SPPRC            | Pre-clip start pixel                     |
 *  | 0x018  | EPPRC            | Pre-clip end pixel                       |
 *  | 0x020  | CSI_IFMD         | CSI-2 interface mode                     |
 *  | 0x024  | CSIFLD           | Field detect control                     |
 *  | 0x02C  | IS               | Image stride                             |
 *  | 0x030  | MB1              | Memory base 1 (frame buffer 1 address)   |
 *  | 0x034  | MB2              | Memory base 2 (frame buffer 2 address)   |
 *  | 0x038  | MB3              | Memory base 3 (frame buffer 3 address)   |
 *  | 0x03C  | LC               | Line count (R)                           |
 *  | 0x040  | IE               | Interrupt enable                         |
 *  | 0x044  | INTS             | Interrupt status (W1C subset)            |
 *  | 0x048  | SI               | Scanline interrupt compare               |
 *  | 0x054  | MTCSTOP          | AXI transfer stop control                |
 *  | 0x058  | DMR              | Data mode register                       |
 *  | 0x060  | UVAOF            | UV data address offset                   |
 *  | 0x080  | UDS_CTRL         | UDS scaling interpolation control        |
 *  | 0x084  | UDS_SCALE        | UDS scaling factors (V/H mantissa+frac)  |
 *  | 0x090  | UDS_PASS_BWIDTH  | UDS passband filter bandwidth            |
 *  | 0x0A4  | UDS_CLIP_SIZE    | UDS output clipping size                 |
 *  | 0x100  | LUTP             | LUT pointer (Y/Cb/Cr)                    |
 *  | 0x104  | LUTD             | LUT data (Y/Cb/Cr)                       |
 *  | 0x228  | YCCR1            | RGB-to-Y coefficient R                   |
 *  | 0x22C  | YCCR2            | RGB-to-Y coefficient G+B                 |
 *  | 0x230  | YCCR3            | RGB-to-Y normalisation/shift             |
 *  | 0x234  | CBCCR1           | RGB-to-Cb coefficient R                  |
 *  | 0x238  | CBCCR2           | RGB-to-Cb coefficient G+B                |
 *  | 0x23C  | CBCCR3           | RGB-to-Cb normalisation/shift            |
 *  | 0x240  | CRCCR1           | RGB-to-Cr coefficient R                  |
 *  | 0x244  | CRCCR2           | RGB-to-Cr coefficient G+B                |
 *  | 0x248  | CRCCR3           | RGB-to-Cr normalisation/shift            |
 *  | 0x300  | CSCE1            | YC-to-RGB Y multiplication               |
 *  | 0x304  | CSCE2            | YC-to-RGB Y/CbCr subtraction             |
 *  | 0x308  | CSCE3            | YC-to-RGB Cr multiplication              |
 *  | 0x30C  | CSCE4            | YC-to-RGB Cb multiplication              |
 *
 * Total window size: 0x310 bytes (HUM Ch 67.2 "Register Description"
 * p 3974, FSP `R_VIN_Type` size = 0x310).
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

/**
 * @enum ra8_vin_addr_t
 * @brief Memory-mapped base addresses for the VIN block.
 *
 * @details
 * VIN0 is the only instance on the RA8D2 package. The non-secure
 * alias VIN0_NS lives at ``0x50347400`` but the driver only uses
 * the secure base for now.
 */
typedef enum : uintptr_t {
  k_ra8_vin0_base_addr    = 0x40347400UL, /**< VIN0 secure base (HUM Ch 67.2 p 3974).      */
  k_ra8_vin0_ns_base_addr = 0x50347400UL, /**< VIN0 non-secure alias (HUM Ch 67.2 p 3974). */
} ra8_vin_addr_t;

/**
 * @enum ra8_vin_off_t
 * @brief 32-bit register offsets inside the VIN window.
 *
 * @details
 * Names mirror FSP's `R_VIN_Type` field names so a reader can
 * cross-reference directly. Offsets verified against FSP
 * `R_VIN_Type` in `R7KA8D2KF_core0.h` lines 18682-19248 and
 * against HUM Ch 67.2 register tables p 3974-4003.
 */
typedef enum : uint16_t {
  k_ra8_vin_off_mc              = 0x000U, /**< MC      Main control register.        */
  k_ra8_vin_off_ms              = 0x004U, /**< MS      Module status (R).            */
  k_ra8_vin_off_fc              = 0x008U, /**< FC      Frame capture register.       */
  k_ra8_vin_off_slprc           = 0x00CU, /**< SLPRC   Pre-clip start line.          */
  k_ra8_vin_off_elprc           = 0x010U, /**< ELPRC   Pre-clip end line.            */
  k_ra8_vin_off_spprc           = 0x014U, /**< SPPRC   Pre-clip start pixel.         */
  k_ra8_vin_off_epprc           = 0x018U, /**< EPPRC   Pre-clip end pixel.           */
  k_ra8_vin_off_csi_ifmd        = 0x020U, /**< CSI_IFMD CSI-2 interface mode.        */
  k_ra8_vin_off_csifld          = 0x024U, /**< CSIFLD  Field detect control.         */
  k_ra8_vin_off_is              = 0x02CU, /**< IS      Image stride.                 */
  k_ra8_vin_off_mb1             = 0x030U, /**< MB1     Memory base 1 (frame addr).   */
  k_ra8_vin_off_mb2             = 0x034U, /**< MB2     Memory base 2 (frame addr).   */
  k_ra8_vin_off_mb3             = 0x038U, /**< MB3     Memory base 3 (frame addr).   */
  k_ra8_vin_off_lc              = 0x03CU, /**< LC      Line count (R).               */
  k_ra8_vin_off_ie              = 0x040U, /**< IE      Interrupt enable.             */
  k_ra8_vin_off_ints            = 0x044U, /**< INTS    Interrupt status (W1C).       */
  k_ra8_vin_off_si              = 0x048U, /**< SI      Scanline interrupt compare.   */
  k_ra8_vin_off_mtcstop         = 0x054U, /**< MTCSTOP AXI transfer stop control.    */
  k_ra8_vin_off_dmr             = 0x058U, /**< DMR     Data mode register.           */
  k_ra8_vin_off_uvaof           = 0x060U, /**< UVAOF   UV data address offset.       */
  k_ra8_vin_off_uds_ctrl        = 0x080U, /**< UDS_CTRL Scaling interpolation ctrl.  */
  k_ra8_vin_off_uds_scale       = 0x084U, /**< UDS_SCALE Scaling factor V/H.         */
  k_ra8_vin_off_uds_pass_bwidth = 0x090U, /**< UDS_PASS_BWIDTH passband bandwidth.   */
  k_ra8_vin_off_uds_clip_size   = 0x0A4U, /**< UDS_CLIP_SIZE output clip size.       */
  k_ra8_vin_off_lutp            = 0x100U, /**< LUTP    LUT pointer (Y/Cb/Cr).        */
  k_ra8_vin_off_lutd            = 0x104U, /**< LUTD    LUT data (Y/Cb/Cr).           */
  k_ra8_vin_off_yccr1           = 0x228U, /**< YCCR1   RGB-to-Y coeff R.             */
  k_ra8_vin_off_yccr2           = 0x22CU, /**< YCCR2   RGB-to-Y coeff G+B.           */
  k_ra8_vin_off_yccr3           = 0x230U, /**< YCCR3   RGB-to-Y norm/shift.          */
  k_ra8_vin_off_cbccr1          = 0x234U, /**< CBCCR1  RGB-to-Cb coeff R.            */
  k_ra8_vin_off_cbccr2          = 0x238U, /**< CBCCR2  RGB-to-Cb coeff G+B.          */
  k_ra8_vin_off_cbccr3          = 0x23CU, /**< CBCCR3  RGB-to-Cb norm/shift.         */
  k_ra8_vin_off_crccr1          = 0x240U, /**< CRCCR1  RGB-to-Cr coeff R.            */
  k_ra8_vin_off_crccr2          = 0x244U, /**< CRCCR2  RGB-to-Cr coeff G+B.          */
  k_ra8_vin_off_crccr3          = 0x248U, /**< CRCCR3  RGB-to-Cr norm/shift.         */
  k_ra8_vin_off_csce1           = 0x300U, /**< CSCE1   YC-to-RGB Y multiplication.   */
  k_ra8_vin_off_csce2           = 0x304U, /**< CSCE2   YC-to-RGB Y/CbCr subtraction. */
  k_ra8_vin_off_csce3           = 0x308U, /**< CSCE3   YC-to-RGB Cr multiplication.  */
  k_ra8_vin_off_csce4           = 0x30CU, /**< CSCE4   YC-to-RGB Cb multiplication.  */
} ra8_vin_off_t;

/**
 * @enum ra8_vin_mc_mask_t
 * @brief Bit masks for the MC (Main Control) register.
 *
 * @details
 * Layout from HUM Ch 67.2.1 "MC : Main Control Register" p 3975. ME
 * is the main enable; ST is a write-only "init at startup" pulse;
 * SCLE turns on the UDS scaler.
 */
typedef enum : uint32_t {
  k_ra8_vin_mc_me     = 0x00000001UL, /**< ME    [0]  Module Enable.             */
  k_ra8_vin_mc_bps    = 0x00000002UL, /**< BPS   [1]  Colour-space bypass.       */
  k_ra8_vin_mc_im     = 0x00000018UL, /**< IM    [4:3] Interlace mode.           */
  k_ra8_vin_mc_en     = 0x00000040UL, /**< EN    [6]  Endian (1 = big).          */
  k_ra8_vin_mc_dc     = 0x0000C000UL, /**< DC    [15:14] Dithering mode.         */
  k_ra8_vin_mc_inf    = 0x00070000UL, /**< INF   [18:16] Input interface format. */
  k_ra8_vin_mc_lute   = 0x00100000UL, /**< LUTE  [20] LUT enable.                */
  k_ra8_vin_mc_st     = 0x00400000UL, /**< ST    [22] Init at startup (W).       */
  k_ra8_vin_mc_dc2    = 0x01000000UL, /**< DC2   [24] Dithering direction.       */
  k_ra8_vin_mc_yuv444 = 0x02000000UL, /**< YUV444[25] YUV444 conversion.         */
  k_ra8_vin_mc_scle   = 0x04000000UL, /**< SCLE  [26] UDS scaling enable.        */
  k_ra8_vin_mc_clp    = 0x30000000UL, /**< CLP   [29:28] Pixel data clipping.    */
} ra8_vin_mc_mask_t;

/**
 * @enum ra8_vin_mc_shift_t
 * @brief Field shift positions for multi-bit MC fields.
 *
 * @details
 * HUM Ch 67.2.1 p 3975. Used to compose multi-bit fields without
 * scattered magic numbers.
 */
typedef enum : uint8_t {
  k_ra8_vin_mc_shift_im  = 3U,  /**< IM[4:3]   shift.  */
  k_ra8_vin_mc_shift_dc  = 14U, /**< DC[15:14] shift.  */
  k_ra8_vin_mc_shift_inf = 16U, /**< INF[18:16] shift. */
  k_ra8_vin_mc_shift_clp = 28U, /**< CLP[29:28] shift. */
} ra8_vin_mc_shift_t;

/**
 * @enum ra8_vin_fc_mask_t
 * @brief Bit masks for the FC (Frame Capture) register.
 *
 * @details
 * HUM Ch 67.2.3 "FC : Frame Capture Register" p 3979. CC at bit 1
 * selects continuous (rolling MB1->MB2->MB3->...) capture mode.
 */
typedef enum : uint32_t {
  k_ra8_vin_fc_cc = 0x00000002UL, /**< CC [1] Continuous frame capture mode. */
} ra8_vin_fc_mask_t;

/**
 * @enum ra8_vin_ms_mask_t
 * @brief Bit masks for the MS (Module Status) register.
 *
 * @details
 * HUM Ch 67.2.2 "MS : Module Status Register" p 3978. CA / MA
 * indicate active capture; FBS / FMS report which MBn buffer the
 * latest valid frame landed in.
 */
typedef enum : uint32_t {
  k_ra8_vin_ms_ca  = 0x00000001UL, /**< CA  [0]    Capture active.              */
  k_ra8_vin_ms_av  = 0x00000002UL, /**< AV  [1]    Active video.                */
  k_ra8_vin_ms_fs  = 0x00000004UL, /**< FS  [2]    Field status (0=odd,1=even). */
  k_ra8_vin_ms_fbs = 0x00000018UL, /**< FBS [4:3]  Latest frame buffer id.      */
  k_ra8_vin_ms_ma  = 0x00010000UL, /**< MA  [16]   Memory capture active.       */
  k_ra8_vin_ms_fms = 0x00180000UL, /**< FMS [20:19] External MB buffer status.  */
} ra8_vin_ms_mask_t;

/**
 * @enum ra8_vin_ms_shift_t
 * @brief Field shift positions for multi-bit MS fields.
 *
 * @details
 * HUM Ch 67.2.2 p 3978.
 */
typedef enum : uint8_t {
  k_ra8_vin_ms_shift_fbs = 3U,  /**< FBS[4:3] shift.   */
  k_ra8_vin_ms_shift_fms = 19U, /**< FMS[20:19] shift. */
} ra8_vin_ms_shift_t;

/**
 * @enum ra8_vin_ms_fbs_t
 * @brief Decoded values of MS.FBS / MS.FMS.
 *
 * @details
 * HUM Ch 67.2.2 p 3978: 0..2 select the corresponding MBn buffer,
 * 3 means "no valid frame buffer / invalid".
 */
typedef enum : uint8_t {
  k_ra8_vin_fbs_mb1     = 0U, /**< Latest frame in MB1.   */
  k_ra8_vin_fbs_mb2     = 1U, /**< Latest frame in MB2.   */
  k_ra8_vin_fbs_mb3     = 2U, /**< Latest frame in MB3.   */
  k_ra8_vin_fbs_invalid = 3U, /**< No valid frame buffer. */
} ra8_vin_ms_fbs_t;

/**
 * @enum ra8_vin_int_mask_t
 * @brief Bit masks for the IE (Interrupt Enable) and INTS
 *        (Interrupt Status) registers.
 *
 * @details
 * Both registers share an identical bit layout. Status bits FOS,
 * ARES and FIS2 are W1C; the remaining bits are state and are
 * cleared by the hardware when their condition resolves. See HUM
 * Ch 67.2.15 "IE" p 3987 and HUM Ch 67.2.16 "INTS" p 3989.
 *
 * Naming uses INTS-side names for the status bits and the same
 * positions for IE so a single mask works for both registers.
 */
typedef enum : uint32_t {
  k_ra8_vin_int_fos      = 0x00000001UL, /**< [0]  FIFO overflow (W1C).           */
  k_ra8_vin_int_eof      = 0x00000002UL, /**< [1]  End of frame (EFS / EFE).      */
  k_ra8_vin_int_scanline = 0x00000004UL, /**< [2]  Scanline match (SIS / SIE).    */
  k_ra8_vin_int_field    = 0x00000010UL, /**< [4]  Field switch (FIS / FIE).      */
  k_ra8_vin_int_fme      = 0x00000020UL, /**< [5]  Frame memory write complete.   */
  k_ra8_vin_int_prcliph  = 0x00000100UL, /**< [8]  Horizontal preclip error.      */
  k_ra8_vin_int_prclipv  = 0x00000200UL, /**< [9]  Vertical preclip error.        */
  k_ra8_vin_int_resp_ovf = 0x00004000UL, /**< [14] Response overflow (ROS / ROE). */
  k_ra8_vin_int_axi_err  = 0x00008000UL, /**< [15] AXI response error (W1C).      */
  k_ra8_vin_int_vsync_d  = 0x00010000UL, /**< [16] VSYNC deassert (VRS / VRE).    */
  k_ra8_vin_int_vsync_a  = 0x00020000UL, /**< [17] VSYNC assert (VFS / VFE).      */
  k_ra8_vin_int_field2   = 0x80000000UL, /**< [31] Field detect 2 (W1C).          */
} ra8_vin_int_mask_t;

/**
 * @enum ra8_vin_int_w1c_mask_t
 * @brief Subset of INTS bits that are write-1-to-clear.
 *
 * @details
 * From HUM Ch 67.2.16 "INTS" p 3989 the bits FOS [0], ARES [15]
 * and FIS2 [31] are described as "After being set to 1, this bit
 * is cleared to 0 by writing 1." Other bits track hardware state
 * and ignore writes. The driver writes this mask back to INTS at
 * the end of an interrupt to clear the latched conditions.
 */
typedef enum : uint32_t {
  k_ra8_vin_int_w1c_mask = 0x80008001UL, /**< FOS | ARES | FIS2. */
} ra8_vin_int_w1c_mask_t;

/**
 * @enum ra8_vin_int_all_mask_t
 * @brief Union of every documented IE/INTS bit (FOS through FIS2).
 *
 * @details
 * Used by `ra8_vin_get_status` and the deinit path to reason about
 * the full surface without hard-coding repeated OR expressions.
 */
typedef enum : uint32_t {
  k_ra8_vin_int_all_mask = 0x8003C737UL, /**< All defined IE/INTS bits. */
} ra8_vin_int_all_mask_t;

/**
 * @enum ra8_vin_mtcstop_mask_t
 * @brief Bit masks for the MTCSTOP (AXI transfer stop) register.
 *
 * @details
 * HUM Ch 67.2.18 "MTCSTOP : AXI Transfer Stop Control Register"
 * p 3991. Setting STOPREQ blocks new AXI requests; STOPACK reads
 * back 1 once outstanding responses have drained. OUTSTAND[5:0]
 * reports the in-flight request count.
 */
typedef enum : uint32_t {
  k_ra8_vin_mtcstop_req      = 0x00000001UL, /**< STOPREQ [0].         */
  k_ra8_vin_mtcstop_ack      = 0x00000002UL, /**< STOPACK [1] (R).     */
  k_ra8_vin_mtcstop_outstand = 0x003F0000UL, /**< OUTSTAND[21:16] (R). */
} ra8_vin_mtcstop_mask_t;

/**
 * @enum ra8_vin_mtcstop_shift_t
 * @brief Field shift positions for MTCSTOP.
 */
typedef enum : uint8_t {
  k_ra8_vin_mtcstop_shift_outstand = 16U, /**< OUTSTAND[21:16] shift. */
} ra8_vin_mtcstop_shift_t;

/**
 * @enum ra8_vin_dmr_mask_t
 * @brief Bit masks for the DMR (Data Mode) register.
 *
 * @details
 * HUM Ch 67.2.19 "DMR : Data Mode Register" p 3992. Selects YC vs
 * RGB output, alpha-bit value, byte-swap, RGB extension to 32 bits,
 * YC through-mode, and the 8-bit alpha for ARGB-8888.
 */
typedef enum : uint32_t {
  k_ra8_vin_dmr_dtmd   = 0x00000003UL, /**< DTMD  [1:0]   Data conversion mode. */
  k_ra8_vin_dmr_abit   = 0x00000004UL, /**< ABIT  [2]     ARGB-1555 alpha bit.  */
  k_ra8_vin_dmr_bpsm   = 0x00000010UL, /**< BPSM  [4]     Byte-swap mode.       */
  k_ra8_vin_dmr_exrgb  = 0x00000100UL, /**< EXRGB [8]     Extend RGB to 32-bit. */
  k_ra8_vin_dmr_yc_thr = 0x00000800UL, /**< YC_THR [11]   YC data through mode. */
  k_ra8_vin_dmr_ymode  = 0x00007000UL, /**< YMODE [14:12] YC transfer mode.     */
  k_ra8_vin_dmr_a8bit  = 0xFF000000UL, /**< A8BIT [31:24] ARGB-8888 alpha.      */
} ra8_vin_dmr_mask_t;

/**
 * @enum ra8_vin_dmr_shift_t
 * @brief Field shift positions for DMR.
 */
typedef enum : uint8_t {
  k_ra8_vin_dmr_shift_dtmd  = 0U,  /**< DTMD[1:0]   shift.  */
  k_ra8_vin_dmr_shift_ymode = 12U, /**< YMODE[14:12] shift. */
  k_ra8_vin_dmr_shift_a8bit = 24U, /**< A8BIT[31:24] shift. */
} ra8_vin_dmr_shift_t;

/**
 * @enum ra8_vin_csi_ifmd_mask_t
 * @brief Bit masks for the CSI_IFMD (CSI-2 Interface Mode) register.
 *
 * @details
 * HUM Ch 67.2.8 "CSI_IFMD : CSI2 Interface Mode Register" p 3982.
 * VC_SEL picks the virtual channel; DT picks the data type; DES0
 * controls sign-extend (0) vs zero-extend (1).
 */
typedef enum : uint32_t {
  k_ra8_vin_csi_ifmd_vc_sel = 0x0000000FUL, /**< VC_SEL [3:0]  Virtual channel. */
  k_ra8_vin_csi_ifmd_dt     = 0x00003F00UL, /**< DT     [13:8] Data type.       */
  k_ra8_vin_csi_ifmd_des0   = 0x02000000UL, /**< DES0   [25]   Data extension.  */
} ra8_vin_csi_ifmd_mask_t;

/**
 * @enum ra8_vin_csi_ifmd_shift_t
 * @brief Field shift positions for CSI_IFMD.
 */
typedef enum : uint8_t {
  k_ra8_vin_csi_ifmd_shift_vc_sel = 0U, /**< VC_SEL[3:0] shift. */
  k_ra8_vin_csi_ifmd_shift_dt     = 8U, /**< DT[13:8]    shift. */
} ra8_vin_csi_ifmd_shift_t;

/**
 * @enum ra8_vin_csifld_mask_t
 * @brief Bit masks for the CSIFLD (Field Detect Control) register.
 *
 * @details
 * HUM Ch 67.2.9 "CSIFLD : Field Detection Control Register" p 3983.
 */
typedef enum : uint32_t {
  k_ra8_vin_csifld_fld_en  = 0x00000001UL, /**< FLD_EN  [0]    Field detect.      */
  k_ra8_vin_csifld_fld_sel = 0x00000030UL, /**< FLD_SEL [5:4]  Even-field select. */
  k_ra8_vin_csifld_fld_num = 0x00010000UL, /**< FLD_NUM [16]   Even-field number. */
} ra8_vin_csifld_mask_t;

/**
 * @enum ra8_vin_csifld_shift_t
 * @brief Field shift positions for CSIFLD.
 */
typedef enum : uint8_t {
  k_ra8_vin_csifld_shift_fld_sel = 4U, /**< FLD_SEL[5:4] shift. */
} ra8_vin_csifld_shift_t;

/**
 * @enum ra8_vin_field_mask_t
 * @brief Bit masks for SLPRC / ELPRC / SPPRC / EPPRC pre-clip registers.
 *
 * @details
 * HUM Ch 67.2.4..67.2.7 p 3980-3981. All four registers carry one
 * 12-bit pixel/line index (max 4095).
 */
typedef enum : uint32_t {
  k_ra8_vin_preclip_mask = 0x00000FFFUL, /**< 12-bit pre-clip count. */
} ra8_vin_field_mask_t;

/**
 * @enum ra8_vin_is_mask_t
 * @brief Bit masks for the IS (Image Stride) register.
 *
 * @details
 * HUM Ch 67.2.13 "IS : Image Stride Register" p 3984. 13-bit field
 * (max 8191 pixels per line).
 */
typedef enum : uint32_t {
  k_ra8_vin_is_mask = 0x00001FFFUL, /**< IS[12:0] image stride in pixels. */
} ra8_vin_is_mask_t;

/**
 * @enum ra8_vin_lc_mask_t
 * @brief Bit masks for the LC (Line Count) register.
 *
 * @details
 * HUM Ch 67.2.14 "LC : Line Count Register" p 3987. Read-only count
 * of received scanlines (0..4095), updated every line.
 */
typedef enum : uint32_t {
  k_ra8_vin_lc_mask = 0x00000FFFUL, /**< LC[11:0] line count. */
} ra8_vin_lc_mask_t;

/**
 * @enum ra8_vin_si_mask_t
 * @brief Bit masks for the SI (Scanline Interrupt) register.
 *
 * @details
 * HUM Ch 67.2.17 "SI : Scanline Interrupt Register" p 3990. 12-bit
 * compare value; 0 disables the scanline interrupt.
 */
typedef enum : uint32_t {
  k_ra8_vin_si_mask = 0x00000FFFUL, /**< SI[11:0] scanline match value. */
} ra8_vin_si_mask_t;

/**
 * @enum ra8_vin_mb_addr_mask_t
 * @brief Bit masks for the MB1/MB2/MB3 frame-buffer base registers.
 *
 * @details
 * HUM Ch 67.2.10..67.2.12 p 3985-3987. Lower 7 bits are reserved
 * (zero) which forces 64-byte alignment for the frame buffer.
 */
typedef enum : uint32_t {
  k_ra8_vin_mb_addr_mask  = 0xFFFFFF80UL, /**< MBn[31:7] base address.           */
  k_ra8_vin_mb_align_mask = 0x0000007FUL, /**< Reserved low bits (must be zero). */
} ra8_vin_mb_addr_mask_t;

/**
 * @enum ra8_vin_uvaof_mask_t
 * @brief Bit masks for the UVAOF (UV Address Offset) register.
 *
 * @details
 * HUM Ch 67.2.20 "UVAOF : UV Address Offset Register" p 3994. Same
 * 64-byte-aligned 25-bit field as the MBn registers.
 */
typedef enum : uint32_t {
  k_ra8_vin_uvaof_mask = 0xFFFFFF80UL, /**< UVAOF[31:7] UV data offset. */
} ra8_vin_uvaof_mask_t;

/**
 * @enum ra8_vin_uds_ctrl_mask_t
 * @brief Bit masks for the UDS_CTRL register.
 *
 * @details
 * HUM Ch 67.2.21 "UDS_CTRL : UDS Scaling Control Register" p 3996.
 * NE_BCB, NE_GY, NE_RCR pick bilinear (0) vs nearest neighbour (1)
 * per channel; BC selects multi-tap mode; BLADV / AMD enable two
 * advanced-mode tweaks for the bilinear/nearest filter.
 */
typedef enum : uint32_t {
  k_ra8_vin_uds_ctrl_ne_bcb = 0x00010000UL, /**< NE_BCB [16] B/Cb interp method. */
  k_ra8_vin_uds_ctrl_ne_gy  = 0x00020000UL, /**< NE_GY  [17] G/Y interp method.  */
  k_ra8_vin_uds_ctrl_ne_rcr = 0x00040000UL, /**< NE_RCR [18] R/Cr interp method. */
  k_ra8_vin_uds_ctrl_bc     = 0x00100000UL, /**< BC     [20] Bilinear vs multi.  */
  k_ra8_vin_uds_ctrl_bladv  = 0x10000000UL, /**< BLADV  [28] Advanced bilinear.  */
  k_ra8_vin_uds_ctrl_amd    = 0x40000000UL, /**< AMD    [30] Advanced pixel cnt. */
} ra8_vin_uds_ctrl_mask_t;

/**
 * @enum ra8_vin_uds_scale_mask_t
 * @brief Bit masks for the UDS_SCALE register.
 *
 * @details
 * HUM Ch 67.2.22 "UDS_SCALE : Scaling Factor Register" p 3997.
 * Each axis carries a 4-bit integer + 12-bit fractional multiplier
 * (Q4.12 fixed-point scaling factor).
 */
typedef enum : uint32_t {
  k_ra8_vin_uds_scale_vfrac = 0x00000FFFUL, /**< VFRAC [11:0]  V scale frac. */
  k_ra8_vin_uds_scale_vmant = 0x0000F000UL, /**< VMANT [15:12] V scale int.  */
  k_ra8_vin_uds_scale_hfrac = 0x0FFF0000UL, /**< HFRAC [27:16] H scale frac. */
  k_ra8_vin_uds_scale_hmant = 0xF0000000UL, /**< HMANT [31:28] H scale int.  */
} ra8_vin_uds_scale_mask_t;

/**
 * @enum ra8_vin_uds_scale_shift_t
 * @brief Field shift positions for UDS_SCALE.
 */
typedef enum : uint8_t {
  k_ra8_vin_uds_scale_shift_vfrac = 0U,  /**< VFRAC[11:0]  shift. */
  k_ra8_vin_uds_scale_shift_vmant = 12U, /**< VMANT[15:12] shift. */
  k_ra8_vin_uds_scale_shift_hfrac = 16U, /**< HFRAC[27:16] shift. */
  k_ra8_vin_uds_scale_shift_hmant = 28U, /**< HMANT[31:28] shift. */
} ra8_vin_uds_scale_shift_t;

/**
 * @enum ra8_vin_uds_bwidth_mask_t
 * @brief Bit masks for the UDS_PASS_BWIDTH register.
 *
 * @details
 * HUM Ch 67.2.23 "UDS_PASS_BWIDTH : Passband Register" p 3998.
 * 7-bit V and H passband bandwidths used by the UDS filter.
 */
typedef enum : uint32_t {
  k_ra8_vin_uds_bwidth_v = 0x0000007FUL, /**< BWIDTH_V [6:0].   */
  k_ra8_vin_uds_bwidth_h = 0x007F0000UL, /**< BWIDTH_H [22:16]. */
} ra8_vin_uds_bwidth_mask_t;

/**
 * @enum ra8_vin_uds_bwidth_shift_t
 * @brief Field shift positions for UDS_PASS_BWIDTH.
 */
typedef enum : uint8_t {
  k_ra8_vin_uds_bwidth_shift_v = 0U,  /**< BWIDTH_V[6:0]  shift.  */
  k_ra8_vin_uds_bwidth_shift_h = 16U, /**< BWIDTH_H[22:16] shift. */
} ra8_vin_uds_bwidth_shift_t;

/**
 * @enum ra8_vin_uds_clip_size_mask_t
 * @brief Bit masks for the UDS_CLIP_SIZE register.
 *
 * @details
 * HUM Ch 67.2.24 "UDS_CLIP_SIZE : UDS Output Size Clipping Register"
 * p 3999. 12-bit V and H clip sizes after scaling.
 */
typedef enum : uint32_t {
  k_ra8_vin_uds_clip_v = 0x00000FFFUL, /**< CL_VSIZE [11:0]   . */
  k_ra8_vin_uds_clip_h = 0x0FFF0000UL, /**< CL_HSIZE [27:16].   */
} ra8_vin_uds_clip_size_mask_t;

/**
 * @enum ra8_vin_uds_clip_shift_t
 * @brief Field shift positions for UDS_CLIP_SIZE.
 */
typedef enum : uint8_t {
  k_ra8_vin_uds_clip_shift_v = 0U,  /**< CL_VSIZE shift. */
  k_ra8_vin_uds_clip_shift_h = 16U, /**< CL_HSIZE shift. */
} ra8_vin_uds_clip_shift_t;

/**
 * @enum ra8_vin_lutp_mask_t
 * @brief Bit masks for the LUTP (LUT Pointer) register.
 *
 * @details
 * HUM Ch 67.2.25 "LUTP : Look Up Table Pointer Register" p 4000.
 * Three 10-bit pointers (Y / Cb / Cr) into the 256-entry LUT used
 * to convert 10-bit input to 8-bit output samples.
 */
typedef enum : uint32_t {
  k_ra8_vin_lutp_ltcrpr = 0x000003FFUL, /**< LTCRPR [9:0]   Cr pointer. */
  k_ra8_vin_lutp_ltcbpr = 0x000FFC00UL, /**< LTCBPR [19:10] Cb pointer. */
  k_ra8_vin_lutp_ltypr  = 0x3FF00000UL, /**< LTYPR  [29:20] Y  pointer. */
} ra8_vin_lutp_mask_t;

/**
 * @enum ra8_vin_lutp_shift_t
 * @brief Field shift positions for LUTP.
 */
typedef enum : uint8_t {
  k_ra8_vin_lutp_shift_ltcrpr = 0U,  /**< Cr pointer shift. */
  k_ra8_vin_lutp_shift_ltcbpr = 10U, /**< Cb pointer shift. */
  k_ra8_vin_lutp_shift_ltypr  = 20U, /**< Y  pointer shift. */
} ra8_vin_lutp_shift_t;

/**
 * @enum ra8_vin_lutd_mask_t
 * @brief Bit masks for the LUTD (LUT Data) register.
 *
 * @details
 * HUM Ch 67.2.26 "LUTD : Look Up Table Data Register" p 4001. Three
 * 8-bit values written into the LUT entry pointed to by LUTP. The
 * driver writes 256 entries by stepping LTYPR / LTCBPR / LTCRPR.
 */
typedef enum : uint32_t {
  k_ra8_vin_lutd_ltcrdt = 0x000000FFUL, /**< LTCRDT [7:0]   Cr data byte. */
  k_ra8_vin_lutd_ltcbdt = 0x0000FF00UL, /**< LTCBDT [15:8]  Cb data byte. */
  k_ra8_vin_lutd_ltydt  = 0x00FF0000UL, /**< LTYDT  [23:16] Y  data byte. */
} ra8_vin_lutd_mask_t;

/**
 * @enum ra8_vin_lutd_shift_t
 * @brief Field shift positions for LUTD.
 */
typedef enum : uint8_t {
  k_ra8_vin_lutd_shift_ltcrdt = 0U,  /**< Cr data byte shift. */
  k_ra8_vin_lutd_shift_ltcbdt = 8U,  /**< Cb data byte shift. */
  k_ra8_vin_lutd_shift_ltydt  = 16U, /**< Y  data byte shift. */
} ra8_vin_lutd_shift_t;

/**
 * @enum ra8_vin_yccr_mask_t
 * @brief Bit masks for the RGB-to-YUV coefficient registers.
 *
 * @details
 * HUM Ch 67.2.27..67.2.35 p 4002-4011. Layout is identical for the
 * three colour-component triples (YCCRn / CBCCRn / CRCCRn). Settings
 * 1 carries the R coefficient; 2 carries G+B; 3 carries the additive
 * offset, the rounding-enable, and the shift-down volume.
 */
typedef enum : uint32_t {
  k_ra8_vin_yc1_rp  = 0x00001FFFUL, /**< Setting 1: R coeff [12:0].            */
  k_ra8_vin_yc2_gp  = 0x00001FFFUL, /**< Setting 2: G coeff [12:0].            */
  k_ra8_vin_yc2_bp  = 0x1FFF0000UL, /**< Setting 2: B coeff [28:16].           */
  k_ra8_vin_yc3_ap  = 0x00000FFFUL, /**< Setting 3: additive offset [11:0].    */
  k_ra8_vin_yc3_hen = 0x00800000UL, /**< Setting 3: round-off enable [23].     */
  k_ra8_vin_yc3_sft = 0x1F000000UL, /**< Setting 3: shift-down volume [28:24]. */
} ra8_vin_yccr_mask_t;

/**
 * @enum ra8_vin_yccr_shift_t
 * @brief Field shift positions for the RGB-to-YUV coefficient registers.
 */
typedef enum : uint8_t {
  k_ra8_vin_yccr_shift_rp  = 0U,  /**< Setting 1 R coeff shift.    */
  k_ra8_vin_yccr_shift_gp  = 0U,  /**< Setting 2 G coeff shift.    */
  k_ra8_vin_yccr_shift_bp  = 16U, /**< Setting 2 B coeff shift.    */
  k_ra8_vin_yccr_shift_ap  = 0U,  /**< Setting 3 offset shift.     */
  k_ra8_vin_yccr_shift_sft = 24U, /**< Setting 3 shift-down shift. */
} ra8_vin_yccr_shift_t;

/**
 * @enum ra8_vin_csce1_mask_t
 * @brief Bit masks for the CSCE1 (YC-to-RGB Y multiply) register.
 *
 * @details
 * HUM Ch 67.2.36 "CSCE1" p 3995 (numbering note: CSCE bank lives at
 * offset 0x300 even though section number is 67.2.36). YMUL2 is a
 * 14-bit unsigned multiplier (initial value 1.164 = 4767 / 4096);
 * ROUND enables rounding of the result.
 */
typedef enum : uint32_t {
  k_ra8_vin_csce1_ymul2 = 0x00003FFFUL, /**< YMUL2 [13:0]. */
  k_ra8_vin_csce1_round = 0x00010000UL, /**< ROUND [16].   */
} ra8_vin_csce1_mask_t;

/**
 * @enum ra8_vin_csce2_mask_t
 * @brief Bit masks for the CSCE2 (YC-to-RGB subtract) register.
 *
 * @details
 * HUM Ch 67.2.37 "CSCE2" p 3995. CSUB2 must remain at the reset
 * value (2048 = 0x800); YSUB2 carries the Y subtraction term
 * (initial value 256 = 0x100).
 */
typedef enum : uint32_t {
  k_ra8_vin_csce2_csub2 = 0x00000FFFUL, /**< CSUB2 [11:0].  */
  k_ra8_vin_csce2_ysub2 = 0x0FFF0000UL, /**< YSUB2 [27:16]. */
} ra8_vin_csce2_mask_t;

/**
 * @enum ra8_vin_csce2_shift_t
 * @brief Field shift positions for CSCE2.
 */
typedef enum : uint8_t {
  k_ra8_vin_csce2_shift_ysub2 = 16U, /**< YSUB2[27:16] shift. */
} ra8_vin_csce2_shift_t;

/**
 * @enum ra8_vin_csce34_mask_t
 * @brief Bit masks for the CSCE3 / CSCE4 multiply-pair registers.
 *
 * @details
 * HUM Ch 67.2.38 "CSCE3" and 67.2.39 "CSCE4". Each carries two
 * unsigned 14-bit multiplier coefficients packed low/high.
 */
typedef enum : uint32_t {
  k_ra8_vin_csce_mul_lo = 0x00003FFFUL, /**< Low  multiplier [13:0].  */
  k_ra8_vin_csce_mul_hi = 0x3FFF0000UL, /**< High multiplier [29:16]. */
} ra8_vin_csce34_mask_t;

/**
 * @enum ra8_vin_csce_shift_t
 * @brief Field shift positions for CSCE3 / CSCE4.
 */
typedef enum : uint8_t {
  k_ra8_vin_csce_shift_mul_hi = 16U, /**< High multiplier shift. */
} ra8_vin_csce_shift_t;

/**
 * @enum ra8_vin_lut_t
 * @brief LUT geometry constants.
 *
 * @details
 * HUM Ch 67.2.25 "LUTP" p 4000. The LUT has 256 entries (0..255);
 * each LUTD write requires a matching LUTP pointer update.
 */
typedef enum : uint16_t {
  k_ra8_vin_lut_entries = 256U, /**< LUT entry count for Y, Cb, and Cr. */
} ra8_vin_lut_t;

/**
 * @enum ra8_vin_st_settle_t
 * @brief Number of MC.ST read-back cycles required before MC.ME = 1.
 *
 * @details
 * FSP `R_VIN_CaptureStart` reads `MC.ST` ten times immediately
 * after writing it, to absorb the "wait at least 10 cycles of aclk
 * (ICLK)" requirement called out in HUM Ch 67.3.1 "Initialization
 * Procedure" p 4015. The driver replays the same loop count.
 */
typedef enum : uint8_t {
  k_ra8_vin_st_settle_reads = 10U, /**< MC.ST settle read-backs. */
} ra8_vin_st_settle_t;

/**
 * @enum ra8_vin_stop_drain_t
 * @brief Number of MTCSTOP poll iterations before declaring timeout.
 *
 * @details
 * HUM Ch 67.3.6 "Stopping the capture operation" p 4022. There is no
 * documented worst-case latency, but the FSP reference simply checks
 * MTCSTOP.OUTSTAND once. The driver polls a bounded number of times
 * (NASA Rule 2: every loop has a fixed upper bound) and reports
 * `k_ra8_err_hw_timeout` if AXI traffic does not drain.
 */
typedef enum : uint16_t {
  k_ra8_vin_stop_drain_max = 1024U, /**< Max OUTSTAND poll iterations. */
} ra8_vin_stop_drain_t;

/**
 * @enum ra8_vin_im_value_t
 * @brief Decoded values of MC.IM (Interlace Mode field).
 *
 * @details
 * HUM Ch 67.2.1 p 3975. The IM field [4:3] selects which incoming
 * field(s) are written to memory. Encodings match Table 67.4
 * "Interlace Mode" in the manual.
 */
typedef enum : uint8_t {
  k_ra8_vin_im_odd_only  = 0U, /**< 00 -- odd-field (field 1) capture.       */
  k_ra8_vin_im_odd_even  = 1U, /**< 01 -- both odd and even fields captured. */
  k_ra8_vin_im_even_only = 2U, /**< 10 -- even-field only capture.           */
} ra8_vin_im_value_t;

/**
 * @enum ra8_vin_dc_value_t
 * @brief Decoded values of MC.DC (Dithering Mode field).
 *
 * @details
 * HUM Ch 67.2.1 p 3975. DC[15:14] selects between cumulative addition
 * and one of two ordered-dither patterns; the DC2 bit (separate, see
 * `k_ra8_vin_mc_dc2`) flips the direction of the ordered patterns.
 */
typedef enum : uint8_t {
  k_ra8_vin_dc_addition  = 0U, /**< 00 -- dithering with cumulative addition. */
  k_ra8_vin_dc_ordered_1 = 1U, /**< 01 -- ordered dithering pattern 1.        */
  k_ra8_vin_dc_ordered_2 = 2U, /**< 10 -- ordered dithering pattern 2.        */
} ra8_vin_dc_value_t;

/**
 * @enum ra8_vin_clp_value_t
 * @brief Decoded values of MC.CLP (Pixel data clipping field).
 *
 * @details
 * HUM Ch 67.2.1 p 3975. CLP[29:28] truncates the input pixel sample
 * before downstream processing.
 */
typedef enum : uint8_t {
  k_ra8_vin_clp_default  = 0U, /**< 00 -- default clipping mode.            */
  k_ra8_vin_clp_nibble   = 1U, /**< 01 -- lower 4-bit clipping mode.        */
  k_ra8_vin_clp_disabled = 3U, /**< 11 -- clipping disabled (pass through). */
} ra8_vin_clp_value_t;

/**
 * @enum ra8_vin_dtmd_value_t
 * @brief Decoded values of DMR.DTMD (Data conversion mode).
 *
 * @details
 * HUM Ch 67.2.19 p 3992. Selects how the YC / RGB pipeline emits
 * pixels into memory.
 */
typedef enum : uint8_t {
  k_ra8_vin_dtmd_none         = 0U, /**< Pass-through (no conversion).   */
  k_ra8_vin_dtmd_rgb_to_argb  = 1U, /**< RGB input rendered as ARGB.     */
  k_ra8_vin_dtmd_yc_separated = 2U, /**< Y/CbCr separated to two planes. */
} ra8_vin_dtmd_value_t;

/**
 * @enum ra8_vin_ymode_value_t
 * @brief Decoded values of DMR.YMODE (YC transfer mode).
 *
 * @details
 * HUM Ch 67.2.19 p 3992. Picks how the Y / CbCr planes are layered
 * into memory in YC-separated mode.
 */
typedef enum : uint8_t {
  k_ra8_vin_ymode_y_cbcr   = 0U, /**< Y + CbCr 8-bit interleaved planes. */
  k_ra8_vin_ymode_y_only   = 1U, /**< Y plane only, 8-bit.               */
  k_ra8_vin_ymode_y10_cbcr = 2U, /**< Y plane 10-bit + CbCr 8-bit.       */
  k_ra8_vin_ymode_y10_only = 3U, /**< Y plane 10-bit only.               */
} ra8_vin_ymode_value_t;

/**
 * @enum ra8_vin_yuv444_value_t
 * @brief Decoded values of MC.YUV444 (YUV-444 conversion).
 *
 * @details
 * HUM Ch 67.2.1 p 3975. Selects how the missing CbCr samples in
 * 4:2:2 input are interpolated up to 4:4:4.
 */
typedef enum : uint8_t {
  k_ra8_vin_yuv444_data_extend = 0U, /**< Repeat the previous CbCr sample.   */
  k_ra8_vin_yuv444_interpolate = 1U, /**< Linearly interpolate CbCr samples. */
} ra8_vin_yuv444_value_t;

/**
 * @enum ra8_vin_uds_interp_value_t
 * @brief Decoded values for the UDS_CTRL NE_* / BC interpolation flags.
 *
 * @details
 * HUM Ch 67.2.21 p 3996. Each NE_* bit picks bilinear (0) vs nearest
 * neighbour (1) for one of the colour channels. BC picks bilinear vs
 * multi-tap mode.
 */
typedef enum : uint8_t {
  k_ra8_vin_uds_bilinear = 0U, /**< Bilinear interpolation.          */
  k_ra8_vin_uds_nearest  = 1U, /**< Nearest-neighbour interpolation. */
} ra8_vin_uds_interp_value_t;

/**
 * @enum ra8_vin_csi_dt_value_t
 * @brief Common values for the CSI_IFMD.DT field.
 *
 * @details
 * HUM Ch 67.2.8 p 3982. Mirrors the MIPI CSI-2 short-packet data
 * type values used by the camera; the driver does not validate that
 * the chosen DT matches MC.INF (the user is expected to keep them
 * coherent).
 */
typedef enum : uint8_t {
  k_ra8_vin_csi_dt_yuv422_8  = 0x1EU, /**< YUV 4:2:2 8-bit.  */
  k_ra8_vin_csi_dt_yuv422_10 = 0x1FU, /**< YUV 4:2:2 10-bit. */
  k_ra8_vin_csi_dt_rgb888    = 0x24U, /**< RGB-888.          */
  k_ra8_vin_csi_dt_raw8      = 0x2AU, /**< RAW-8.            */
} ra8_vin_csi_dt_value_t;

/**
 * @enum ra8_vin_field_polarity_t
 * @brief Decoded values for the CSIFLD.FLD_NUM bit.
 *
 * @details
 * HUM Ch 67.2.9 p 3983. Picks which field-number ordering counts
 * as the "even" field for interlaced capture.
 */
typedef enum : uint8_t {
  k_ra8_vin_field_even_is_zero = 0U, /**< field-number 0 -> even. */
  k_ra8_vin_field_even_is_one  = 1U, /**< field-number 1 -> even. */
} ra8_vin_field_polarity_t;

/**
 * @enum ra8_vin_lut_geometry_t
 * @brief Geometric / range constants for the LUT plane.
 *
 * @details
 * HUM Ch 67.2.25 p 4000 / 67.2.26 p 4001. Each LUT byte is 8-bit
 * unsigned and the pointer field is 10-bit (0..1023) but only
 * indices 0..255 select real entries.
 */
typedef enum : uint16_t {
  k_ra8_vin_lut_max_pointer = 0x3FFU, /**< LUTP pointer field width. */
  k_ra8_vin_lut_max_byte    = 0xFFU,  /**< LUTD byte field width.    */
} ra8_vin_lut_geometry_t;

/**
 * @enum ra8_vin_uds_scale_geometry_t
 * @brief Range / step limits for UDS scaling factor and clipping.
 *
 * @details
 * HUM Ch 67.2.22 p 3997 / 67.2.24 p 3999. Mantissa is 4 bits, fraction
 * is 12 bits, clip output is 12-bit pixel count.
 */
typedef enum : uint16_t {
  k_ra8_vin_uds_max_mant = 0xFU,   /**< 4-bit mantissa max value.    */
  k_ra8_vin_uds_max_frac = 0xFFFU, /**< 12-bit fractional max value. */
  k_ra8_vin_uds_max_clip = 0xFFFU, /**< 12-bit clip-size max.        */
  k_ra8_vin_uds_max_bw   = 0x7FU,  /**< 7-bit pass-band width max.   */
} ra8_vin_uds_scale_geometry_t;

/**
 * @enum ra8_vin_yccr_init_t
 * @brief Reset / suggested-initial values for YCCRn / CBCCRn / CRCCRn.
 *
 * @details
 * HUM Ch 67.2.27..67.2.35 p 4002-4011. The driver does not force
 * these values on init; they are exposed so the test harness can
 * assert on the stock identity matrix.
 */
typedef enum : uint32_t {
  k_ra8_vin_yccr_y_setting1_init = 0x00000000UL, /**< YCCR1 reset.               */
  k_ra8_vin_yccr_y_setting2_init = 0x00000000UL, /**< YCCR2 reset.               */
  k_ra8_vin_yccr_y_setting3_init = 0x00800000UL, /**< YCCR3 reset (HEN = 1).     */
  k_ra8_vin_csce1_init           = 0x0001129FUL, /**< CSCE1 reset (YMUL2/ROUND). */
  k_ra8_vin_csce2_init           = 0x01000800UL, /**< CSCE2 reset (YSUB2/CSUB2). */
} ra8_vin_yccr_init_t;

/**
 * @brief Pointer to a 32-bit register inside the VIN block.
 *
 * @param[in] off Register offset (one of `k_ra8_vin_off_*`).
 * @return Volatile pointer to the register.
 *
 * @pre `off` is one of the documented `k_ra8_vin_off_*` values.
 * @pre Caller's task has read access to the secure peripheral window.
 * @post Returned pointer aliases real MMIO on target / mmap'd RAM
 *       on the host test build.
 *
 * @note Stateless inline accessor; safe from any context.
 * @see k_ra8_vin0_base_addr
 * @since 0.1.0
 */
RA8_HW_REGISTER_ACCESS
static inline volatile uint32_t* ra8_vin_reg32(ra8_vin_off_t off)
{
  return (volatile uint32_t*)(k_ra8_vin0_base_addr + (uintptr_t)(uint16_t)off);
}

#ifdef __cplusplus
}
#endif
