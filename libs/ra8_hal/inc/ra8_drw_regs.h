/**
 * @file ra8_drw_regs.h
 * @brief 2D Drawing Engine (DRW / D/AVE 2D) register layout for the RA8D2
 * @ingroup grp_hal_display
 *
 * @details
 * The RA8D2 carries a Renesas D/AVE 2D vector / BitBLT drawing
 * engine at base ``0x40444000`` (Secure alias) / ``0x50444000``
 * (Non-Secure alias). The block size is 0x104 bytes (260 bytes).
 *
 * This header models the *full* register surface documented in
 * HUM Ch 62 "2D Drawing Engine (DRW)" pages 3685-3743:
 *
 *  - CONTROL (geometry)
 *  - CONTROL2 (surface, blend, texture, RLE, CLUT, color-key)
 *  - STATUS (engine + IRQ)
 *  - HWREVISION (feature ID register)
 *  - L1..L6 START / XADD / YADD (six edge limiters)
 *  - L1BAND / L2BAND (band post-process)
 *  - COLOR1, COLOR2, PATTERN
 *  - SIZE, PITCH, ORIGIN
 *  - LU* / LV* texture limiter set
 *  - TEXPITCH, TEXMASK, TEXORIGIN
 *  - IRQCTL, CACHECTL, DLISTSTART
 *  - PERFCOUNT1/2, PERFTRIGGER
 *  - TEXCLADDR, TEXCLDATA, TEXCLOFFSET, COLKEY
 *  - DBWER (bufferable write enable)
 *
 * ## Block structure (verified against FSP R_DRW_Type in
 * R7KA8D2KF_core0.h lines 5881-6571, register descriptions cross-
 * checked against the HUM 62.2 series)
 *
 *  | Offset  | Name (W)     | Name (R)     | Purpose                  |
 *  |--------:|--------------|--------------|--------------------------|
 *  | 0x00    | CONTROL      | STATUS       | Geometry ctl / engine st |
 *  | 0x04    | CONTROL2     | HWREVISION   | Surface ctl / hw rev     |
 *  | 0x10..  | L1..L6 START |              | Limiter start values     |
 *  | 0x28..  | L1..L6 XADD  |              | Limiter X-axis increment |
 *  | 0x40..  | L1..L6 YADD  |              | Limiter Y-axis increment |
 *  | 0x58    | L1BAND       |              | Limiter 1 band width     |
 *  | 0x5C    | L2BAND       |              | Limiter 2 band width     |
 *  | 0x64    | COLOR1       |              | Base RGBA color          |
 *  | 0x68    | COLOR2       |              | Secondary RGBA color     |
 *  | 0x74    | PATTERN      |              | Pattern bitmap           |
 *  | 0x78    | SIZE         |              | Bounding box (W,H)       |
 *  | 0x7C    | PITCH        |              | FB pitch + spanstore del |
 *  | 0x80    | ORIGIN       |              | Framebuffer base addr    |
 *  | 0x90    | LUSTART      |              | U limiter start          |
 *  | 0x94    | LUXADD       |              | U limiter X increment    |
 *  | 0x98    | LUYADD       |              | U limiter Y increment    |
 *  | 0x9C    | LVSTARTI     |              | V limiter start integer  |
 *  | 0xA0    | LVSTARTF     |              | V limiter start fraction |
 *  | 0xA4    | LVXADDI      |              | V limiter X int          |
 *  | 0xA8    | LVYADDI      |              | V limiter Y int          |
 *  | 0xAC    | LVYXADDF     |              | V limiter X/Y fraction   |
 *  | 0xB4    | TEXPITCH     |              | Texels per texture line  |
 *  | 0xB8    | TEXMASK      |              | Texture U/V mask         |
 *  | 0xBC    | TEXORIGIN    |              | Texture base addr        |
 *  | 0xC0    | IRQCTL       |              | Interrupt control        |
 *  | 0xC4    | CACHECTL     |              | Cache enable / flush     |
 *  | 0xC8    | DLISTSTART   |              | Display list start addr  |
 *  | 0xCC    | PERFCOUNT1   | PERFCOUNT1   | Performance counter 1    |
 *  | 0xD0    | PERFCOUNT2   | PERFCOUNT2   | Performance counter 2    |
 *  | 0xD4    | PERFTRIGGER  |              | Performance trigger sel  |
 *  | 0xDC    | TEXCLADDR    |              | CLUT start address       |
 *  | 0xE0    | TEXCLDATA    |              | CLUT entry data          |
 *  | 0xE4    | TEXCLOFFSET  |              | CLUT index offset        |
 *  | 0xE8    | COLKEY       |              | Color key value          |
 *  | 0x100   | DBWER        | DBWER        | Bufferable write enable  |
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
 * @enum ra8_drw_addr_t
 * @brief Memory-mapped base addresses for the DRW block.
 *
 * @details
 * The Secure alias (S = ``0x40444000``) is what the bare-metal HAL
 * uses today. The Non-Secure alias would be selected by the secure-
 * world arbiter once TrustZone partitioning lands.
 */
typedef enum : uintptr_t {
  k_ra8_drw_base_addr    = 0x40444000UL, /**< DRW Secure base.     */
  k_ra8_drw_ns_base_addr = 0x50444000UL, /**< DRW Non-Secure base. */
} ra8_drw_addr_t;

/**
 * @enum ra8_drw_off_t
 * @brief Absolute register offsets inside the DRW window.
 *
 * @details
 * Names mirror the FSP ``R_DRW_Type`` field names so a reader can
 * cross-reference directly against the CMSIS device header.
 */
typedef enum : uint16_t {
  k_ra8_drw_off_control     = 0x000U, /**< W: Geometry control.           */
  k_ra8_drw_off_status      = 0x000U, /**< R: Engine status (alias).      */
  k_ra8_drw_off_control2    = 0x004U, /**< W: Surface control.            */
  k_ra8_drw_off_hwrevision  = 0x004U, /**< R: HW revision (alias).        */
  k_ra8_drw_off_l1start     = 0x010U, /**< W: Limiter 1 start.            */
  k_ra8_drw_off_l2start     = 0x014U, /**< W: Limiter 2 start.            */
  k_ra8_drw_off_l3start     = 0x018U, /**< W: Limiter 3 start.            */
  k_ra8_drw_off_l4start     = 0x01CU, /**< W: Limiter 4 start.            */
  k_ra8_drw_off_l5start     = 0x020U, /**< W: Limiter 5 start.            */
  k_ra8_drw_off_l6start     = 0x024U, /**< W: Limiter 6 start.            */
  k_ra8_drw_off_l1xadd      = 0x028U, /**< W: Limiter 1 X-axis increment. */
  k_ra8_drw_off_l2xadd      = 0x02CU, /**< W: Limiter 2 X-axis increment. */
  k_ra8_drw_off_l3xadd      = 0x030U, /**< W: Limiter 3 X-axis increment. */
  k_ra8_drw_off_l4xadd      = 0x034U, /**< W: Limiter 4 X-axis increment. */
  k_ra8_drw_off_l5xadd      = 0x038U, /**< W: Limiter 5 X-axis increment. */
  k_ra8_drw_off_l6xadd      = 0x03CU, /**< W: Limiter 6 X-axis increment. */
  k_ra8_drw_off_l1yadd      = 0x040U, /**< W: Limiter 1 Y-axis increment. */
  k_ra8_drw_off_l2yadd      = 0x044U, /**< W: Limiter 2 Y-axis increment. */
  k_ra8_drw_off_l3yadd      = 0x048U, /**< W: Limiter 3 Y-axis increment. */
  k_ra8_drw_off_l4yadd      = 0x04CU, /**< W: Limiter 4 Y-axis increment. */
  k_ra8_drw_off_l5yadd      = 0x050U, /**< W: Limiter 5 Y-axis increment. */
  k_ra8_drw_off_l6yadd      = 0x054U, /**< W: Limiter 6 Y-axis increment. */
  k_ra8_drw_off_l1band      = 0x058U, /**< W: Limiter 1 band width.       */
  k_ra8_drw_off_l2band      = 0x05CU, /**< W: Limiter 2 band width.       */
  k_ra8_drw_off_color1      = 0x064U, /**< W: Base color RGBA.            */
  k_ra8_drw_off_color2      = 0x068U, /**< W: Secondary color RGBA.       */
  k_ra8_drw_off_pattern     = 0x074U, /**< W: Pattern bitmap.             */
  k_ra8_drw_off_size        = 0x078U, /**< W: Bounding box (W,H).         */
  k_ra8_drw_off_pitch       = 0x07CU, /**< W: FB pitch + spanstore delay. */
  k_ra8_drw_off_origin      = 0x080U, /**< W: Framebuffer base address.   */
  k_ra8_drw_off_lustart     = 0x090U, /**< W: U limiter start.            */
  k_ra8_drw_off_luxadd      = 0x094U, /**< W: U limiter X increment.      */
  k_ra8_drw_off_luyadd      = 0x098U, /**< W: U limiter Y increment.      */
  k_ra8_drw_off_lvstarti    = 0x09CU, /**< W: V limiter start integer.    */
  k_ra8_drw_off_lvstartf    = 0x0A0U, /**< W: V limiter start fraction.   */
  k_ra8_drw_off_lvxaddi     = 0x0A4U, /**< W: V limiter X integer.        */
  k_ra8_drw_off_lvyaddi     = 0x0A8U, /**< W: V limiter Y integer.        */
  k_ra8_drw_off_lvyxaddf    = 0x0ACU, /**< W: V limiter X/Y fraction.     */
  k_ra8_drw_off_texpitch    = 0x0B4U, /**< W: Texels per texture line.    */
  k_ra8_drw_off_texmask     = 0x0B8U, /**< W: Texture U/V mask.           */
  k_ra8_drw_off_texorigin   = 0x0BCU, /**< W: Texture base address.       */
  k_ra8_drw_off_irqctl      = 0x0C0U, /**< W: Interrupt control.          */
  k_ra8_drw_off_cachectl    = 0x0C4U, /**< W: Cache enable / flush.       */
  k_ra8_drw_off_dliststart  = 0x0C8U, /**< W: Display list start address. */
  k_ra8_drw_off_perfcount1  = 0x0CCU, /**< RW: Performance counter 1.     */
  k_ra8_drw_off_perfcount2  = 0x0D0U, /**< RW: Performance counter 2.     */
  k_ra8_drw_off_perftrigger = 0x0D4U, /**< W: Performance trigger select. */
  k_ra8_drw_off_texcladdr   = 0x0DCU, /**< W: CLUT start address.         */
  k_ra8_drw_off_texcldata   = 0x0E0U, /**< W: CLUT entry data.            */
  k_ra8_drw_off_texcloffset = 0x0E4U, /**< W: CLUT index offset.          */
  k_ra8_drw_off_colkey      = 0x0E8U, /**< W: Color key value.            */
  k_ra8_drw_off_dbwer       = 0x100U, /**< RW: DRW bufferable write en.   */
} ra8_drw_off_t;

/**
 * @enum ra8_drw_blocksize_t
 * @brief Total DRW window size, used by static-assert clients.
 */
typedef enum : uint16_t {
  k_ra8_drw_block_bytes = 0x104U, /**< 260 bytes -- matches FSP R_DRW_Type. */
} ra8_drw_blocksize_t;

/* =============================================================================
 * STATUS register (R only, alias of CONTROL @ +0x00)
 *
 * HUM Ch 62.2.5 "STATUS : Status Control Register" p 3695.
 * =============================================================================
 */

/**
 * @enum ra8_drw_status_mask_t
 * @brief STATUS register bit masks (read-only).
 *
 * @details
 * Verified against HUM Ch 62.2.5 p 3695 and FSP
 * ``R_DRW_Type::STATUS_b`` in ``R7KA8D2KF_core0.h`` lines 5925-5939.
 * The status register shares its offset with CONTROL: a write at
 * offset 0 hits CONTROL, a read returns STATUS.
 */
typedef enum : uint32_t {
  k_ra8_drw_status_busyenum     = (1UL << 0U),  /**< Enumeration unit busy.     */
  k_ra8_drw_status_busywrite    = (1UL << 1U),  /**< FB writeback busy.         */
  k_ra8_drw_status_cachedirty   = (1UL << 2U),  /**< FB cache dirty.            */
  k_ra8_drw_status_dlistactive  = (1UL << 3U),  /**< Display list reader busy.  */
  k_ra8_drw_status_enumirq      = (1UL << 4U),  /**< Enumeration finished IRQ.  */
  k_ra8_drw_status_dlistirq     = (1UL << 5U),  /**< Display list finished IRQ. */
  k_ra8_drw_status_busirq       = (1UL << 6U),  /**< Bus error IRQ.             */
  k_ra8_drw_status_buserrmfb    = (1UL << 8U),  /**< FB bus error.              */
  k_ra8_drw_status_buserrmtxmrl = (1UL << 9U),  /**< Texture/RLE bus error.     */
  k_ra8_drw_status_buserrmdl    = (1UL << 10U), /**< Display list bus error.    */
  k_ra8_drw_status_busy_mask    = (k_ra8_drw_status_busyenum | k_ra8_drw_status_busywrite |
                                   k_ra8_drw_status_dlistactive), /**< RA8 DRW status busy mask. */
  k_ra8_drw_status_irq_mask     = (k_ra8_drw_status_enumirq | k_ra8_drw_status_dlistirq |
                                   k_ra8_drw_status_busirq), /**< RA8 DRW status IRQ mask. */
  k_ra8_drw_status_buserr_mask  = (k_ra8_drw_status_buserrmfb | k_ra8_drw_status_buserrmtxmrl |
                                   k_ra8_drw_status_buserrmdl), /**< RA8 DRW status buserr mask. */
} ra8_drw_status_mask_t;

/* =============================================================================
 * IRQCTL register (W only)
 *
 * HUM Ch 62.2.3 "IRQCTL : Interrupt Control Register" p 3693.
 * =============================================================================
 */

/**
 * @enum ra8_drw_irqctl_mask_t
 * @brief IRQCTL register bit masks (write-only).
 *
 * @details
 * Verified against HUM Ch 62.2.3 p 3693 and FSP
 * ``R_DRW_Type::IRQCTL_b`` in ``R7KA8D2KF_core0.h`` lines 5443-5450.
 * Note that ``ENUMIRQEN`` (bit 0) is the *enable* for ENUMIRQ; the
 * ``*CLR`` bits are write-1-to-acknowledge.
 */
typedef enum : uint32_t {
  k_ra8_drw_irqctl_enumirqen   = (1UL << 0U), /**< ENUMIRQ unmask.  */
  k_ra8_drw_irqctl_dlistirqen  = (1UL << 1U), /**< DLISTIRQ unmask. */
  k_ra8_drw_irqctl_enumirqclr  = (1UL << 2U), /**< Clear ENUMIRQ.   */
  k_ra8_drw_irqctl_dlistirqclr = (1UL << 3U), /**< Clear DLISTIRQ.  */
  k_ra8_drw_irqctl_busirqen    = (1UL << 4U), /**< BUSIRQ unmask.   */
  k_ra8_drw_irqctl_busirqclr   = (1UL << 5U), /**< Clear BUSIRQ.    */
  k_ra8_drw_irqctl_all_clr     = (k_ra8_drw_irqctl_enumirqclr | k_ra8_drw_irqctl_dlistirqclr |
                                  k_ra8_drw_irqctl_busirqclr), /**< RA8 DRW irqctl all clr. */
  k_ra8_drw_irqctl_all_en      = (k_ra8_drw_irqctl_enumirqen | k_ra8_drw_irqctl_dlistirqen |
                                  k_ra8_drw_irqctl_busirqen), /**< RA8 DRW irqctl all en. */
} ra8_drw_irqctl_mask_t;

/* =============================================================================
 * CACHECTL register (W only)
 *
 * HUM Ch 62.2.4 "CACHECTL : Cache Control Register" p 3694.
 * =============================================================================
 */

/**
 * @enum ra8_drw_cachectl_mask_t
 * @brief CACHECTL register bit masks (write-only).
 *
 * @details
 * HUM Ch 62.2.4 p 3694 documents four bits:
 *   - bit 0 CENABLEFX -- framebuffer cache enable
 *   - bit 1 CFLUSHFX  -- framebuffer cache flush (W1)
 *   - bit 2 CENABLETX -- texture cache enable
 *   - bit 3 CFLUSHTX  -- texture cache flush (W1)
 */
typedef enum : uint32_t {
  k_ra8_drw_cachectl_cenablefx = (1UL << 0U), /**< FB cache enable.      */
  k_ra8_drw_cachectl_cflushfx  = (1UL << 1U), /**< FB cache flush.       */
  k_ra8_drw_cachectl_cenabletx = (1UL << 2U), /**< Texture cache enable. */
  k_ra8_drw_cachectl_cflushtx  = (1UL << 3U), /**< Texture cache flush.  */
  k_ra8_drw_cachectl_all_en =
    (k_ra8_drw_cachectl_cenablefx | k_ra8_drw_cachectl_cenabletx), /**< RA8 DRW cachectl all en. */
  k_ra8_drw_cachectl_all_flush =
    (k_ra8_drw_cachectl_cflushfx | k_ra8_drw_cachectl_cflushtx), /**< RA8 DRW cachectl all flush. */
} ra8_drw_cachectl_mask_t;

/* =============================================================================
 * CONTROL2 (Surface Control) register
 *
 * HUM Ch 62.2.2 p 3690-3693.
 * =============================================================================
 */

/**
 * @enum ra8_drw_control2_mask_t
 * @brief CONTROL2 (Surface Control) field masks for every documented bit.
 *
 * @details
 * HUM Ch 62.2.2 p 3690-3693. The 32-bit register packs every blend
 * mode, texture/CLUT/RLE/color-key knob, plus the 3-bit framebuffer
 * format and 4-bit texture format selectors.
 */
typedef enum : uint32_t {
  k_ra8_drw_control2_patternenable    = (1UL << 0U),  /**< Pattern source.        */
  k_ra8_drw_control2_textureenable    = (1UL << 1U),  /**< Texture source.        */
  k_ra8_drw_control2_patternsourcel5  = (1UL << 2U),  /**< Use L5 as pat index.   */
  k_ra8_drw_control2_useacb           = (1UL << 3U),  /**< Alpha channel blend.   */
  k_ra8_drw_control2_readformath_pos  = 4U,           /**< READFORMAT_H[3:2] @4.  */
  k_ra8_drw_control2_readformath_mask = (3UL << 4U),  /**< READFORMAT_H mask.     */
  k_ra8_drw_control2_bsfa             = (1UL << 6U),  /**< Blend src fac alpha.   */
  k_ra8_drw_control2_bdfa             = (1UL << 7U),  /**< Blend dst fac alpha.   */
  k_ra8_drw_control2_writeformat2_bit = (1UL << 8U),  /**< WRITEFORMAT[2].        */
  k_ra8_drw_control2_bsf              = (1UL << 9U),  /**< Blend src factor.      */
  k_ra8_drw_control2_bdf              = (1UL << 10U), /**< Blend dst factor.      */
  k_ra8_drw_control2_bsi              = (1UL << 11U), /**< Blend src inverted.    */
  k_ra8_drw_control2_bdi              = (1UL << 12U), /**< Blend dst inverted.    */
  k_ra8_drw_control2_bc2              = (1UL << 13U), /**< Use COLOR2 as DST.     */
  k_ra8_drw_control2_textureclampx    = (1UL << 14U), /**< Texture clamp X.       */
  k_ra8_drw_control2_textureclampy    = (1UL << 15U), /**< Texture clamp Y.       */
  k_ra8_drw_control2_texturefilterx   = (1UL << 16U), /**< Bilinear X.            */
  k_ra8_drw_control2_texturefiltery   = (1UL << 17U), /**< Bilinear Y.            */
  k_ra8_drw_control2_readformatl_pos  = 18U,          /**< READFORMAT_L[1:0] @18. */
  k_ra8_drw_control2_readformatl_mask = (3UL << 18U), /**< READFORMAT_L mask.     */
  k_ra8_drw_control2_writeformat_pos  = 20U,          /**< WRITEFORMAT[1:0] @20.  */
  k_ra8_drw_control2_writeformat_mask = (3UL << 20U), /**< WRITEFORMAT mask.      */
  k_ra8_drw_control2_writealpha_pos   = 22U,          /**< WRITEALPHA[1:0] @22.   */
  k_ra8_drw_control2_writealpha_mask  = (3UL << 22U), /**< WRITEALPHA mask.       */
  k_ra8_drw_control2_rleenable        = (1UL << 24U), /**< RLE source.            */
  k_ra8_drw_control2_clutenable       = (1UL << 25U), /**< CLUT enable.           */
  k_ra8_drw_control2_colkeyenable     = (1UL << 26U), /**< Color key enable.      */
  k_ra8_drw_control2_clutformat_565   = (1UL << 27U), /**< CLUT in RGB565 mode.   */
  k_ra8_drw_control2_bsia             = (1UL << 28U), /**< Blend src alpha inv.   */
  k_ra8_drw_control2_bdia             = (1UL << 29U), /**< Blend dst alpha inv.   */
  k_ra8_drw_control2_rlepixel_pos     = 30U,          /**< RLEPIXELWIDTH @30.     */
  k_ra8_drw_control2_rlepixel_mask    = (3UL << 30U), /**< RLEPIXELWIDTH mask.    */
} ra8_drw_control2_mask_t;

/* =============================================================================
 * CONTROL (Geometry Control) register
 *
 * HUM Ch 62.2.1 p 3689-3690.
 * =============================================================================
 */

/**
 * @enum ra8_drw_control_mask_t
 * @brief CONTROL (Geometry Control) field masks for every documented bit.
 *
 * @details
 * HUM Ch 62.2.1 p 3689-3690. Six edge limiters (LIM1..LIM6), three
 * quadratic-coupling pairs (QUAD1=L1+L2, QUAD2=L3+L4, QUAD3=L5+L6),
 * six per-limiter "threshold" enables, two band post-process enables,
 * five union/intersect select bits (UNION12 -> A, UNION34 -> B,
 * UNION56 -> D, UNIONAB -> C, UNIONCD -> final), and the SPANABORT /
 * SPANSTORE optimisations.
 *
 * Solid-fill rectangle uses LIM1..LIM4 with the default INTERSECT for
 * (1,2) and (3,4) and INTERSECT for (A,B), giving a filled axis-aligned
 * quad. Lines use UNION12. Triangles use LIM1..LIM3 with all
 * intersects.
 */
typedef enum : uint32_t {
  k_ra8_drw_control_lim1en        = (1UL << 0U),  /**< Enable limiter 1.   */
  k_ra8_drw_control_lim2en        = (1UL << 1U),  /**< Enable limiter 2.   */
  k_ra8_drw_control_lim3en        = (1UL << 2U),  /**< Enable limiter 3.   */
  k_ra8_drw_control_lim4en        = (1UL << 3U),  /**< Enable limiter 4.   */
  k_ra8_drw_control_lim5en        = (1UL << 4U),  /**< Enable limiter 5.   */
  k_ra8_drw_control_lim6en        = (1UL << 5U),  /**< Enable limiter 6.   */
  k_ra8_drw_control_quad1en       = (1UL << 6U),  /**< QUAD1 (L1,L2).      */
  k_ra8_drw_control_quad2en       = (1UL << 7U),  /**< QUAD2 (L3,L4).      */
  k_ra8_drw_control_quad3en       = (1UL << 8U),  /**< QUAD3 (L5,L6).      */
  k_ra8_drw_control_lim1threshold = (1UL << 9U),  /**< Threshold mode L1.  */
  k_ra8_drw_control_lim2threshold = (1UL << 10U), /**< Threshold mode L2.  */
  k_ra8_drw_control_lim3threshold = (1UL << 11U), /**< Threshold mode L3.  */
  k_ra8_drw_control_lim4threshold = (1UL << 12U), /**< Threshold mode L4.  */
  k_ra8_drw_control_lim5threshold = (1UL << 13U), /**< Threshold mode L5.  */
  k_ra8_drw_control_lim6threshold = (1UL << 14U), /**< Threshold mode L6.  */
  k_ra8_drw_control_band1en       = (1UL << 15U), /**< Band post-proc L1.  */
  k_ra8_drw_control_band2en       = (1UL << 16U), /**< Band post-proc L2.  */
  k_ra8_drw_control_union12       = (1UL << 17U), /**< Union L1,L2 -> A.   */
  k_ra8_drw_control_union34       = (1UL << 18U), /**< Union L3,L4 -> B.   */
  k_ra8_drw_control_union56       = (1UL << 19U), /**< Union L5,L6 -> D.   */
  k_ra8_drw_control_unionab       = (1UL << 20U), /**< Union A,B -> C.     */
  k_ra8_drw_control_unioncd       = (1UL << 21U), /**< Union C,D -> final. */
  k_ra8_drw_control_spanabort     = (1UL << 22U), /**< Convex-shape opt.   */
  k_ra8_drw_control_spanstore     = (1UL << 23U), /**< Span-store opt.     */
  k_ra8_drw_control_quad_box =
    (k_ra8_drw_control_lim1en | k_ra8_drw_control_lim2en | k_ra8_drw_control_lim3en |
     k_ra8_drw_control_lim4en), /**< RA8 DRW control quad box. */
  k_ra8_drw_control_triangle = (k_ra8_drw_control_lim1en | k_ra8_drw_control_lim2en |
                                k_ra8_drw_control_lim3en), /**< RA8 DRW control triangle. */
  k_ra8_drw_control_line_quad =
    (k_ra8_drw_control_lim1en | k_ra8_drw_control_lim2en | k_ra8_drw_control_lim3en |
     k_ra8_drw_control_lim4en | k_ra8_drw_control_union12), /**< RA8 DRW control line quad. */
} ra8_drw_control_mask_t;

/* =============================================================================
 * HWREVISION (R only, alias of CONTROL2)
 *
 * HUM Ch 62.2.6 p 3696.
 * =============================================================================
 */

/**
 * @enum ra8_drw_hwrev_mask_t
 * @brief HWREVISION feature-set bit masks (read-only).
 *
 * @details
 * HUM Ch 62.2.6 p 3696. Reset value on the RA8D2 is documented as
 * 0x1FBE_0017 (DLR=1, FBCACHE=1, TXCACHE=1, PERFCOUNT=1, TEXCLUT=1,
 * RLEUNIT=1, TEXCLUT256=1, COLORKEY=1, ACBLEND=1, REV=0x017).
 */
typedef enum : uint32_t {
  k_ra8_drw_hwrev_rev_mask    = 0x00000FFFUL, /**< REV[11:0].             */
  k_ra8_drw_hwrev_dlr         = (1UL << 17U), /**< Display list reader.   */
  k_ra8_drw_hwrev_fbcache     = (1UL << 18U), /**< FB cache present.      */
  k_ra8_drw_hwrev_txcache     = (1UL << 19U), /**< Texture cache present. */
  k_ra8_drw_hwrev_perfcount   = (1UL << 20U), /**< 2 perf counters.       */
  k_ra8_drw_hwrev_texclut     = (1UL << 21U), /**< Texture CLUT present.  */
  k_ra8_drw_hwrev_rleunit     = (1UL << 23U), /**< RLE unit present.      */
  k_ra8_drw_hwrev_texclut256  = (1UL << 24U), /**< CLUT is 256 entries.   */
  k_ra8_drw_hwrev_colorkey    = (1UL << 25U), /**< Color key unit.        */
  k_ra8_drw_hwrev_acblend     = (1UL << 27U), /**< Full alpha blend.      */
  k_ra8_drw_hwrev_reset_value = 0x1FBE0017UL, /**< Power-on value.        */
} ra8_drw_hwrev_mask_t;

/* =============================================================================
 * Pixel formats
 *
 * HUM Ch 62.2.2 p 3692 (WRITEFORMAT[2:0] table) and Ch 62.3.1.1 p 3707.
 * =============================================================================
 */

/**
 * @enum ra8_drw_writeformat_t
 * @brief Framebuffer pixel format codes (3-bit field).
 *
 * @details
 * HUM Ch 62.2.2 p 3692. The 3-bit code spans CONTROL2.WRITEFORMAT2
 * (bit 8) and CONTROL2.WRITEFORMAT[1:0] (bits 21:20). Codes 4..7 are
 * marked "Setting prohibited".
 *
 * | Code | Format                   |
 * |-----:|--------------------------|
 * | 0    | A (8) -- 8 bpp           |
 * | 1    | RGB (565) -- 16 bpp      |
 * | 2    | ARGB (8888) -- 32 bpp    |
 * | 3    | ARGB (4444) -- 16 bpp    |
 */
typedef enum : uint8_t {
  k_ra8_drw_writefmt_a8       = 0U, /**< 8 bpp A(8).      */
  k_ra8_drw_writefmt_rgb565   = 1U, /**< 16 bpp RGB565.   */
  k_ra8_drw_writefmt_argb8888 = 2U, /**< 32 bpp ARGB8888. */
  k_ra8_drw_writefmt_argb4444 = 3U, /**< 16 bpp ARGB4444. */
} ra8_drw_writeformat_t;

/**
 * @enum ra8_drw_readformat_t
 * @brief Texture pixel format codes (4-bit field).
 *
 * @details
 * HUM Ch 62.2.2 p 3692. Spans CONTROL2.READFORMAT_H[3:2] (bits 5:4)
 * and CONTROL2.READFORMAT_L[1:0] (bits 19:18). Encoded as one
 * contiguous 4-bit value here; the driver splits it on write.
 *
 * | Code | Bits | Format                         |
 * |-----:|-----:|--------------------------------|
 * | 0x0  |  8   | A (8)                          |
 * | 0x1  | 16   | RGB565                         |
 * | 0x2  | 32   | ARGB8888                       |
 * | 0x3  | 16   | ARGB4444                       |
 * | 0x4  | 16   | ARGB1555                       |
 * | 0x5  |  8   | ACLUT44 (4-bit alpha + 4-bit i)|
 * | 0x9  |  8   | CLUT8/I8                       |
 * | 0xA  |  4   | CLUT4/I4                       |
 * | 0xB  |  2   | CLUT2/I2                       |
 * | 0xC  |  1   | CLUT1/I1                       |
 */
typedef enum : uint8_t {
  k_ra8_drw_readfmt_a8       = 0x0U, /**< 8 bpp A(8) texture.    */
  k_ra8_drw_readfmt_rgb565   = 0x1U, /**< 16 bpp RGB565 texture. */
  k_ra8_drw_readfmt_argb8888 = 0x2U, /**< 32 bpp ARGB8888.       */
  k_ra8_drw_readfmt_argb4444 = 0x3U, /**< 16 bpp ARGB4444.       */
  k_ra8_drw_readfmt_argb1555 = 0x4U, /**< 16 bpp ARGB1555.       */
  k_ra8_drw_readfmt_aclut44  = 0x5U, /**< 8 bpp 4+4 ACLUT.       */
  k_ra8_drw_readfmt_clut8    = 0x9U, /**< 8 bpp CLUT8/I8.        */
  k_ra8_drw_readfmt_clut4    = 0xAU, /**< 4 bpp CLUT4/I4.        */
  k_ra8_drw_readfmt_clut2    = 0xBU, /**< 2 bpp CLUT2/I2.        */
  k_ra8_drw_readfmt_clut1    = 0xCU, /**< 1 bpp CLUT1/I1.        */
} ra8_drw_readformat_t;

/**
 * @enum ra8_drw_writealpha_t
 * @brief WRITEALPHA[1:0] writeback alpha source select.
 *
 * @details
 * HUM Ch 62.2.2 p 3693. Two bits select where the alpha channel of
 * each writeback pixel comes from.
 */
typedef enum : uint8_t {
  k_ra8_drw_writealpha_color2    = 0U, /**< Use COLOR2 alpha.      */
  k_ra8_drw_writealpha_pixel_cov = 1U, /**< Use source coverage.   */
  k_ra8_drw_writealpha_zero      = 2U, /**< Force 0.0.             */
  k_ra8_drw_writealpha_fb        = 3U, /**< Use framebuffer alpha. */
} ra8_drw_writealpha_t;

/**
 * @enum ra8_drw_rlepixel_t
 * @brief RLEPIXELWIDTH[1:0] -- bytes per RLE-encoded texel.
 *
 * @details
 * HUM Ch 62.2.2 p 3693 + HUM Ch 62.5.2.1 p 3712 "Texel formats
 * supported by the RLE unit". RLE source width 1..4 bytes per texel.
 */
typedef enum : uint8_t {
  k_ra8_drw_rle_1byte = 0U, /**< 1 byte per texel  (8-bit CLUT/I/A/ACLUT44). */
  k_ra8_drw_rle_2byte = 1U, /**< 2 bytes per texel (16-bit RGB565/ARGB).     */
  k_ra8_drw_rle_3byte = 2U, /**< 3 bytes per texel (24-bit RGB888).          */
  k_ra8_drw_rle_4byte = 3U, /**< 4 bytes per texel (32-bit ARGB8888).        */
} ra8_drw_rlepixel_t;

/* =============================================================================
 * Performance counters
 *
 * HUM Ch 62.2.33 p 3706.
 * =============================================================================
 */

/**
 * @enum ra8_drw_perftrigger_t
 * @brief Internal events that increment a PERFCOUNT register.
 *
 * @details
 * HUM Ch 62.2.33 p 3706. 14 documented event sources plus "every
 * clock" (0x1F). Two counters are independently programmed via
 * PERFTRIGGER[15:0] (counter 1) and PERFTRIGGER[31:16] (counter 2).
 */
typedef enum : uint16_t {
  k_ra8_drw_perfev_disabled       = 0x0000U, /**< Disable counter.            */
  k_ra8_drw_perfev_active_cycles  = 0x0001U, /**< 2D engine active cycles.    */
  k_ra8_drw_perfev_fb_read        = 0x0002U, /**< FB read access count.       */
  k_ra8_drw_perfev_fb_write       = 0x0003U, /**< FB write access count.      */
  k_ra8_drw_perfev_tex_read       = 0x0004U, /**< Texture read accesses.      */
  k_ra8_drw_perfev_invisible      = 0x0005U, /**< Invisible (alpha 0) pixels. */
  k_ra8_drw_perfev_invisible_idle = 0x0006U, /**< Invisible while FIFO empty. */
  k_ra8_drw_perfev_dlist_active   = 0x0007U, /**< Display list reader active. */
  k_ra8_drw_perfev_fb_read_hit    = 0x0008U, /**< FB cache read hits.         */
  k_ra8_drw_perfev_fb_read_miss   = 0x0009U, /**< FB cache read misses.       */
  k_ra8_drw_perfev_fb_write_hit   = 0x000AU, /**< FB cache write hits.        */
  k_ra8_drw_perfev_fb_write_miss  = 0x000BU, /**< FB cache write misses.      */
  k_ra8_drw_perfev_tex_read_hit   = 0x000CU, /**< Texture cache read hits.    */
  k_ra8_drw_perfev_tex_read_miss  = 0x000DU, /**< Texture cache read misses.  */
  k_ra8_drw_perfev_every_cycle    = 0x001FU, /**< Free-running timer mode.    */
} ra8_drw_perftrigger_t;

/**
 * @enum ra8_drw_perfcounter_id_t
 * @brief Selector for the two PERFCOUNTk registers.
 */
typedef enum : uint8_t {
  k_ra8_drw_perfctr_1 = 0U, /**< PERFCOUNT1 @ +0xCC. */
  k_ra8_drw_perfctr_2 = 1U, /**< PERFCOUNT2 @ +0xD0. */
} ra8_drw_perfcounter_id_t;

/* =============================================================================
 * Display list encoding
 *
 * A display list is a stream of 32-bit words the DRW's display-list reader
 * (DLR) fetches from RAM after DLISTSTART is written (HUM Ch 62.6 "Display
 * List Mode" p 3724). The HUM documents the concept but not the word layout;
 * the layout below is the one the DLR consumes, decoded from the vendored TES
 * D/AVE 2D reference (its software DLR model d2_executedlist_intern and
 * register map: register index == byte offset / 4) and bench-verified on an
 * EK-RA8D2 (issue #247).
 *
 * Each entry is a TAG word followed by one value word per register it writes.
 * The tag packs up to four register indices, one per byte. The first byte
 * position >= 1 whose bit 7 is set marks the entry's boundary: byte 1 -> one
 * register, byte 2 -> two, byte 3 -> three, none set -> four. A word whose low
 * byte is 0xFF is an end-of-list marker with an argument in byte 1 (2 = wait
 * for the pipeline and cache then continue; 3 = terminate).
 * =============================================================================
 */

/**
 * @enum ra8_drw_dlr_index_t
 * @brief Register indices a display-list entry addresses (byte offset / 4).
 *
 * @details
 * The DLR names registers by word index, not byte offset. Each value equals
 * the matching ``k_ra8_drw_off_*`` divided by four (e.g. ORIGIN 0x80 -> 32).
 * Only the registers the solid-fill display-list path programs are listed;
 * add more here as further primitives gain a display-list encoding.
 *
 * @see ra8_drw_dlist_add_fill
 */
typedef enum : uint8_t {
  k_ra8_drw_dlr_idx_control  = 0U,  /**< CONTROL  (HUM Ch 62.2.1, 0x00/4).  */
  k_ra8_drw_dlr_idx_control2 = 1U,  /**< CONTROL2 (HUM Ch 62.2.2, 0x04/4).  */
  k_ra8_drw_dlr_idx_color1   = 25U, /**< COLOR1   (HUM Ch 62.2.7, 0x64/4).  */
  k_ra8_drw_dlr_idx_color2   = 26U, /**< COLOR2   (HUM Ch 62.2.8, 0x68/4).  */
  k_ra8_drw_dlr_idx_size     = 30U, /**< SIZE     (HUM Ch 62.2.29, 0x78/4). */
  k_ra8_drw_dlr_idx_pitch    = 31U, /**< PITCH    (HUM Ch 62.2.30, 0x7C/4). */
  k_ra8_drw_dlr_idx_origin   = 32U, /**< ORIGIN   (HUM Ch 62.2.31, 0x80/4). */
  k_ra8_drw_dlr_idx_cachectl = 49U, /**< CACHECTL (HUM Ch 62.2.4, 0xC4/4).  */
} ra8_drw_dlr_index_t;

/**
 * @enum ra8_drw_dlr_word_t
 * @brief Display-list tag and end-of-list word constants.
 *
 * @details
 * ::k_ra8_drw_dlr_tag_one_index OR-ed with a ::ra8_drw_dlr_index_t forms a tag
 * for a single-register entry (byte 1 carries bit 7, the boundary marker, so
 * the DLR reads exactly one value word after it). An end-of-list word is
 * ::k_ra8_drw_dlr_eol_byte OR-ed with an argument shifted by
 * ::k_ra8_drw_dlr_eol_arg_pos.
 *
 * @see ra8_drw_dlr_index_t
 */
typedef enum : uint32_t {
  k_ra8_drw_dlr_tag_one_index = 0x80808000UL, /**< 1-register entry tag base.   */
  k_ra8_drw_dlr_eol_byte      = 0x000000FFUL, /**< End-of-list low byte marker. */
  k_ra8_drw_dlr_eol_arg_pos   = 8UL,          /**< End-of-list argument shift.  */
  k_ra8_drw_dlr_arg_wait      = 2UL,          /**< Arg: wait pipe+cache, go on. */
  k_ra8_drw_dlr_arg_terminate = 3UL,          /**< Arg: terminate the list.     */
} ra8_drw_dlr_word_t;

/* =============================================================================
 * SIZE / PITCH / DBWER misc bit fields
 * =============================================================================
 */

/**
 * @enum ra8_drw_geom_layout_t
 * @brief Bit positions / masks shared by SIZE and PITCH.
 *
 * @details
 *   - SIZE[15:0]   = SIZEX width  (HUM Ch 62.2.29 p 3704).
 *   - SIZE[31:16]  = SIZEY height (HUM Ch 62.2.29 p 3704).
 *   - PITCH[15:0]  = PITCH        (HUM Ch 62.2.30 p 3705).
 *   - PITCH[31:16] = SSD          (HUM Ch 62.2.30 p 3705).
 *   - PERFTRIGGER[15:0]  = trigger 1 (HUM Ch 62.2.33 p 3706).
 *   - PERFTRIGGER[31:16] = trigger 2 (HUM Ch 62.2.33 p 3706).
 */
typedef enum : uint8_t {
  k_ra8_drw_size_height_pos      = 16U, /**< SIZEY in upper half-word.    */
  k_ra8_drw_pitch_ssd_pos        = 16U, /**< SSD in upper half-word.      */
  k_ra8_drw_perftrigger2_pos     = 16U, /**< Counter-2 selector pos.      */
  k_ra8_drw_color_b_pos          = 0U,  /**< COLORnB[7:0].                */
  k_ra8_drw_color_g_pos          = 8U,  /**< COLORnG[7:0].                */
  k_ra8_drw_color_r_pos          = 16U, /**< COLORnR[7:0].                */
  k_ra8_drw_color_a_pos          = 24U, /**< COLORnA[7:0].                */
  k_ra8_drw_lvyxaddf_lvyaddf_pos = 16U, /**< LVYXADDF.LVYADDF in [31:16]. */
} ra8_drw_geom_layout_t;

/**
 * @enum ra8_drw_dim_limits_t
 * @brief Hardware bounds documented by HUM Ch 62.
 */
typedef enum : uint16_t {
  k_ra8_drw_max_width_px     = 1024U, /**< HUM 62.2.29 SIZEX max.      */
  k_ra8_drw_max_height_px    = 1024U, /**< HUM 62.2.29 SIZEY max.      */
  k_ra8_drw_max_texpitch_tx  = 2048U, /**< HUM 62.2.15 TEXPITCH max.   */
  k_ra8_drw_max_texumask     = 2048U, /**< HUM 62.2.16 TEXUMASK clamp. */
  k_ra8_drw_max_texvmask     = 1024U, /**< HUM 62.2.16 TEXVMASK clamp. */
  k_ra8_drw_clut_entries_max = 256U,  /**< HUM 62.2.6 TEXCLUT256 = 1.  */
  k_ra8_drw_min_dim_px       = 1U,    /**< Zero-sized rect rejected.   */
} ra8_drw_dim_limits_t;

/**
 * @enum ra8_drw_pixel_stride_t
 * @brief Framebuffer pixel stride in bytes per WRITEFORMAT class.
 *
 * @details
 * HUM Ch 62.3.1.1 "Color Formats" p 3707. Needed to advance ORIGIN to a
 * primitive's top-left pixel, because the engine scans the bounding box
 * anchored at ORIGIN (HUM Ch 62.6.2 p 3716).
 *
 * @invariant Every member is a power of two in [1, 4].
 *
 * @see ra8_drw_writeformat_t
 */
typedef enum : uint8_t {
  k_ra8_drw_bytes_px_8bpp  = 1U, /**< A8.                */
  k_ra8_drw_bytes_px_16bpp = 2U, /**< RGB565 / ARGB4444. */
  k_ra8_drw_bytes_px_32bpp = 4U, /**< ARGB8888.          */
} ra8_drw_pixel_stride_t;

/**
 * @enum ra8_drw_dbwer_mask_t
 * @brief DBWER (DRW Bufferable Write Enable) field masks.
 *
 * @details
 * HUM Ch 62.2.35 p 3707. Only one usable bit (BWE @ bit 2). Bits 1:0
 * and 31:3 are RAZ/WI.
 */
typedef enum : uint32_t {
  k_ra8_drw_dbwer_bwe = (1UL << 2U), /**< Enable bufferable writes. */
} ra8_drw_dbwer_mask_t;

/**
 * @enum ra8_drw_subpixel_t
 * @brief DRW limiter sub-pixel constants (Q12.4 fixed-point space).
 *
 * @details
 * The DRW limiters work in 4-bit sub-pixel coordinates -- shift pixel
 * coordinates left by 4 before writing LnSTART / LnXADD / LnYADD.
 * HUM Ch 62.4.2 "Limiters" p 3724 documents the fixed-point grid.
 */
typedef enum : uint8_t {
  k_ra8_drw_subpixel_shift = 4U,  /**< 1 pixel == 16 sub-pixels. */
  k_ra8_drw_subpixel_unit  = 16U, /**< 1 pixel value (1 << 4).   */
} ra8_drw_subpixel_t;

/**
 * @brief Pointer to a 32-bit register inside the DRW block.
 *
 * @param[in] off One of the ``k_ra8_drw_off_*`` values (low 12 bits
 *                of the absolute address).
 * @return Volatile pointer to the register.
 *
 * @pre Caller passes a well-formed offset from ``ra8_drw_off_t``.
 * @post Returned pointer is non-null and 4-byte aligned.
 *
 * @note Access width is always 32 bits; sub-word writes are not
 *       supported by the DRW block.
 */
RA8_HW_REGISTER_ACCESS
static inline volatile uint32_t* ra8_drw_reg32(ra8_drw_off_t off)
{
  return (volatile uint32_t*)(k_ra8_drw_base_addr + (uintptr_t)(uint16_t)off);
}

#ifdef __cplusplus
}
#endif
