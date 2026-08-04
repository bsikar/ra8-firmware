/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_system_regs.h
 * @brief System Control (SYSC) register layout for the Renesas RA8D2
 * @ingroup grp_hal_system
 *
 * @details
 * Covers the System Control block at `0x4001E000`. The SYSC block
 * owns reset-cause reporting, software reset, protection-register
 * unlock, and (along with CGC) clock-source selection.
 *
 * ## Map (partial -- only the registers this firmware touches)
 *
 * | Offset | Reg       | Purpose                                   |
 * |-------:|-----------|-------------------------------------------|
 * | 0x020  | SCKDIVCR  | System clock divider control               |
 * | 0x026  | SCKSCR    | System clock source select                 |
 * | 0x036  | HOCOCR    | High-speed on-chip oscillator control      |
 * | 0x0A2  | MOSCWTCR  | Main clock oscillator wait control         |
 * | 0x0AC  | PLLCCR    | PLL1 clock control                         |
 * | 0x0C0  | RSTSR1    | Reset status register 1                    |
 * | 0x3FA  | PRCR      | Protection register (write with 0xA500 mask) |
 * | 0xA40  | RSTSR0    | Reset status register 0                    |
 * | 0xA44  | RSTSR2    | Reset status register 2                    |
 *
 * The full R_SYSTEM block spans 3456 bytes and contains dozens of
 * other registers (RAM wait-state control, LVD, temperature monitor,
 * VBAT, etc.). We expose only the subset the firmware actually uses
 * and add new fields as we need them. This keeps the header small
 * and makes every extension an explicit, reviewed change.
 *
 * ## PRCR unlock sequence
 *
 * Most CGC and LVD registers are protected. Write `0xA501` to unlock
 * group 0 (CGC + LVD) before changing them, and `0xA500` to re-lock
 * when done. The upper byte `0xA5` is the password; the low bit is the
 * protection-enable for that group.
 *
 * See the Hardware User's Manual section 12 ("Register Write
 * Protection") for the full table.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* =============================================================================
 * Base addresses
 * =============================================================================
 */

typedef enum : uintptr_t {
  k_ra8_system_base_addr = 0x4001E000UL, /**< R_SYSTEM. */
} ra8_system_addr_t;

/* =============================================================================
 * Register offsets (within the SYSTEM block)
 * =============================================================================
 */

typedef enum : uint16_t {
  k_ra8_sys_off_vscr         = 0x014U, /**< VSCR voltage scaling control (32-bit).              */
  k_ra8_sys_off_sckdivcr     = 0x020U, /**< SCKDIVCR  (32-bit).                                 */
  k_ra8_sys_off_sckdivcr2    = 0x024U, /**< SCKDIVCR2 (16-bit).                                 */
  k_ra8_sys_off_sckscr       = 0x026U, /**< SCKSCR    (8-bit).                                  */
  k_ra8_sys_off_pllcr        = 0x02AU, /**< PLLCR     (8-bit).                                  */
  k_ra8_sys_off_moscr        = 0x032U, /**< MOSCCR    (8-bit).                                  */
  k_ra8_sys_off_hococr       = 0x036U, /**< HOCOCR    (8-bit).                                  */
  k_ra8_sys_off_oscsf        = 0x03CU, /**< OSCSF     (8-bit).                                  */
  k_ra8_sys_off_pll2cr       = 0x04AU, /**< PLL2CR   -- PLL2 stop control (8-bit).              */
  k_ra8_sys_off_pllccr2      = 0x04CU, /**< PLLCCR2 -- PLL1 output dividers (16-bit).           */
  k_ra8_sys_off_pll2ccr2     = 0x04EU, /**< PLL2CCR2 -- PLL2 output dividers (16-bit).          */
  k_ra8_sys_off_scickdivcr   = 0x054U, /**< SCICKDIVCR (8-bit).                                 */
  k_ra8_sys_off_scickcr      = 0x055U, /**< SCICKCR    (8-bit).                                 */
  k_ra8_sys_off_eswckdivcr   = 0x0D5U, /**< ESWCKDIVCR  (8-bit) -- ESWM module clock div.       */
  k_ra8_sys_off_eswpckdivcr  = 0x0D6U, /**< ESWPCKDIVCR (8-bit) -- ESWM-PHY clock div.          */
  k_ra8_sys_off_eswckcr      = 0x0DBU, /**< ESWCKCR     (8-bit) -- ESWM clock source.           */
  k_ra8_sys_off_eswpckcr     = 0x0DCU, /**< ESWPCKCR    (8-bit) -- ESWM-PHY clock source.       */
  k_ra8_sys_off_usbckdivcr   = 0x06CU, /**< USBCKDIVCR  (8-bit) -- USB-FS clock divider.        */
  k_ra8_sys_off_octackdivcr  = 0x06DU, /**< OCTACKDIVCR (8-bit) -- HUM Ch 9.2.40 p 357.         */
  k_ra8_sys_off_canfdckdivcr = 0x06EU, /**< CANFDCKDIVCR (8-bit) -- HUM Ch 9.2.41 p 363.        */
  k_ra8_sys_off_usb60ckdivcr = 0x06FU, /**< USB60CKDIVCR (8-bit) -- USBHS 60MHz divider.        */
  k_ra8_sys_off_usbckcr      = 0x074U, /**< USBCKCR     (8-bit) -- USB-FS clock src.            */
  k_ra8_sys_off_octackcr     = 0x075U, /**< OCTACKCR    (8-bit) -- HUM Ch 9.2.45 p 360.         */
  k_ra8_sys_off_canfdckcr    = 0x076U, /**< CANFDCKCR   (8-bit) -- HUM Ch 9.2.46 p 366.         */
  k_ra8_sys_off_usb60ckcr    = 0x077U, /**< USB60CKCR   (8-bit) -- USBHS 60MHz src.             */
  k_ra8_sys_off_moscwtcr     = 0x0A2U, /**< MOSCWTCR  (8-bit).                                  */
  k_ra8_sys_off_pdctreswm    = 0x118U, /**< PDCTRESWM (8-bit) -- ESWM power-domain control.     */
  k_ra8_sys_off_pllccr       = 0x0ACU, /**< PLLCCR    (32-bit).                                 */
  k_ra8_sys_off_rstsr1       = 0x0C0U, /**< RSTSR1    (32-bit).                                 */
  k_ra8_sys_off_pll2ccr      = 0x0C8U, /**< PLL2CCR -- input/mul/source (32-bit, HUM Ch 9.2.9). */
  k_ra8_sys_off_prcr         = 0x3FAU, /**< PRCR      (16-bit).                                 */
  k_ra8_sys_off_rstsr0       = 0xA40U, /**< RSTSR0    (8-bit).                                  */
  k_ra8_sys_off_rstsr2       = 0xA44U, /**< RSTSR2    (8-bit).                                  */
  k_ra8_sys_off_lococr       = 0x400U, /**< LOCOCR    (8-bit) -- HUM Ch 9.2.15 p 339.           */
  k_ra8_sys_off_sosccr       = 0xC00U, /**< SOSCCR    (8-bit) -- HUM Ch 9.2.14 p 339.           */
  k_ra8_sys_off_somcr        = 0xC01U, /**< SOMCR     (8-bit) -- HUM Ch 9.2.29 p 351.           */
} ra8_system_offset_t;

/**
 * @enum ra8_vscr_bit_t
 * @brief Bit masks in VSCR (Voltage Scaling Control Register).
 *
 * @details
 * On RA8 Gen2 silicon, software must drop the core to the
 * "not high voltage" range (`VSCM = 1`) before lifting PLL1 above its
 * default rate, then poll `VSCMTSF` until the transition completes
 * (FSP `bsp_clocks.c`, FSP CMSIS device header
 * `R7KA8D2KF_core0.h`).
 *
 * Bit positions inside the 32-bit register:
 *   - VSCM     = bit 0 (write 1 to enter not-high-voltage mode)
 *   - VSCMTSF  = bit 4 (read-only, set while transition is in flight)
 */
typedef enum : uint32_t {
  k_ra8_vscr_bit_vscm    = (1UL << 0U), /**< VSCM bit mask (write 1 = not-high-V). */
  k_ra8_vscr_bit_vscmtsf = (1UL << 4U), /**< VSCMTSF transition-in-flight mask.    */
} ra8_vscr_bit_t;

/**
 * @enum ra8_moscr_bit_t
 * @brief Bit masks in MOSCCR (Main Clock Oscillator Control Register).
 *
 * @details
 * MOSTP at bit 0: write 0 to start the main oscillator, 1 to stop it
 * (HUM Ch 9.2.13).
 */
typedef enum : uint8_t {
  k_ra8_moscr_mostp_mask = (1U << 0U), /**< MOSTP -- 0 starts XTAL, 1 stops it. */
} ra8_moscr_bit_t;

/**
 * @enum ra8_sosccr_bit_t
 * @brief Bit mask in SOSCCR (Sub-Clock Oscillator Control Register).
 *
 * @details
 * SOSTP at bit 0: write 0 to operate the 32.768 kHz sub-clock crystal
 * oscillator, 1 to stop it. Reset value is 1 (stopped). Set SOMCR
 * before clearing SOSTP, then wait the sub-clock stabilization time
 * (t_SUBOSCWT) before using the clock (HUM Ch 9.2.14 "SOSCCR :
 * Sub-Clock Oscillator Control Register" p 339).
 */
typedef enum : uint8_t {
  k_ra8_sosccr_sostp_mask = (1U << 0U), /**< SOSTP -- 0 operates SOSC, 1 stops it. */
} ra8_sosccr_bit_t;

/**
 * @enum ra8_somcr_drv_t
 * @brief SOMCR.SODRV[1:0] sub-clock drive-capability codes.
 *
 * @details
 * Selects the drive strength feeding the 32.768 kHz crystal. Standard
 * (12.5 pF) is the cold-reset default and matches the EK-RA8D2 watch
 * crystal. SOMCR must be written while SOSCCR.SOSTP is 1 (SOSC stopped)
 * (HUM Ch 9.2.29 "SOMCR : Sub-Clock Oscillator Mode Control Register"
 * p 351).
 */
typedef enum : uint8_t {
  k_ra8_somcr_drv_standard = 0x0U, /**< Standard drive (12.5 pF). */
  k_ra8_somcr_drv_lp1      = 0x1U, /**< Low-power mode 1 (9 pF).  */
  k_ra8_somcr_drv_lp2      = 0x2U, /**< Low-power mode 2 (7 pF).  */
  k_ra8_somcr_drv_lp3      = 0x3U, /**< Low-power mode 3 (4 pF).  */
} ra8_somcr_drv_t;

/**
 * @enum ra8_lococr_bit_t
 * @brief Bit mask in LOCOCR (Low-Speed On-Chip Oscillator Control Register).
 *
 * @details
 * LCSTP at bit 0: write 0 to operate the internal ~32.768 kHz LOCO,
 * 1 to stop it. Reset value is 0 (running), so the LOCO is normally
 * already available as a crystal-free RTC count source (HUM Ch 9.2.15
 * "LOCOCR : Low-Speed On-Chip Oscillator Control Register" p 339).
 */
typedef enum : uint8_t {
  k_ra8_lococr_lcstp_mask = (1U << 0U), /**< LCSTP -- 0 operates LOCO, 1 stops it. */
} ra8_lococr_bit_t;

/**
 * @enum ra8_oscsf_bit_t
 * @brief Oscillation Stabilization Flag bit positions.
 *
 * @details
 * Each bit is set by hardware when the corresponding oscillator has
 * stabilised. The CGC driver polls these between starting an
 * oscillator and using it as the system-clock source.
 */
typedef enum : uint8_t {
  k_ra8_oscsf_bit_hocosf = 0U, /**< HOCO stabilisation flag.   */
  k_ra8_oscsf_bit_moscsf = 3U, /**< Main oscillator stab flag. */
  k_ra8_oscsf_bit_pll1sf = 5U, /**< PLL1 stabilisation flag.   */
  k_ra8_oscsf_bit_pll2sf = 6U, /**< PLL2 stabilisation flag.   */
} ra8_oscsf_bit_t;

/* =============================================================================
 * PRCR protection key and group masks
 * =============================================================================
 */

/**
 * @enum ra8_prcr_t
 * @brief Values to write to PRCR to unlock / lock protection groups.
 *
 * @details
 * `k_ra8_prcr_key` is the mandatory 0xA5 password in the upper byte.
 * OR with one of the group bits to *enable writes* to that group
 * (counter-intuitive -- bit set = unlocked), then write back with the
 * group bit cleared to relock.
 *
 * Group membership is HUM Ch 13.1 Table 13.1 "Association between PRCR
 * bits and use of registers to be protected" p 520-521; the bit
 * positions are HUM Ch 13.2.1 "PRCR_S : Protect Register for Secure"
 * p 522. Bit 2 and bits 7:6 are reserved (read as 0, write 0).
 *
 * A write to a protected register while its group is locked is
 * **silently discarded** -- no fault, no status bit. Every driver that
 * touches a register named in Table 13.1 must therefore wrap the write
 * in ::RA8_PROTECTED_WRITE with the matching group, or it will appear
 * to work and change nothing.
 */
typedef enum : uint16_t {
  k_ra8_prcr_key      = 0xA500U, /**< Password in upper byte.                       */
  k_ra8_prcr_grp0_cgc = 0x0001U, /**< PRC0 @ bit 0: clock generation circuit.       */
  k_ra8_prcr_grp1_lpm = 0x0002U, /**< PRC1 @ bit 1: low-power modes + VBATT backup. */
  k_ra8_prcr_grp3_pvd = 0x0008U, /**< PRC3 @ bit 3: PVD + VBATTMNSELR.              */
  k_ra8_prcr_grp4_sar = 0x0010U, /**< PRC4 @ bit 4: security/privilege attribution. */
  k_ra8_prcr_grp5_rst = 0x0020U, /**< PRC5 @ bit 5: reset control.                  */
  k_ra8_prcr_unlock_cgc =
    (uint16_t)(k_ra8_prcr_key | k_ra8_prcr_grp0_cgc), /**< RA8 prcr unlock cgc. */
  k_ra8_prcr_unlock_lpm =
    (uint16_t)(k_ra8_prcr_key | k_ra8_prcr_grp1_lpm), /**< RA8 prcr unlock lpm. */
  k_ra8_prcr_unlock_pvd =
    (uint16_t)(k_ra8_prcr_key | k_ra8_prcr_grp3_pvd), /**< RA8 prcr unlock pvd. */
  k_ra8_prcr_unlock_sar =
    (uint16_t)(k_ra8_prcr_key | k_ra8_prcr_grp4_sar), /**< RA8 prcr unlock sar. */
  k_ra8_prcr_unlock_rst =
    (uint16_t)(k_ra8_prcr_key | k_ra8_prcr_grp5_rst), /**< RA8 prcr unlock rst. */
  k_ra8_prcr_lock_all = k_ra8_prcr_key,               /**< RA8 prcr lock all.   */
} ra8_prcr_t;

/* =============================================================================
 * Reset-cause bits (subset -- add as needed)
 * =============================================================================
 */

/**
 * @enum ra8_rstsr0_bit_t
 * @brief Reset Status Register 0 bit positions.
 *
 * @details
 * Set by hardware when a reset of the corresponding cause occurred.
 * Cleared by software (write 0 after unlocking PRCR group 0).
 */
typedef enum : uint8_t {
  k_ra8_rstsr0_bit_porf = 0U, /**< Power-on reset.          */
  k_ra8_rstsr0_bit_lvd0 = 1U, /**< Voltage-monitor 0 reset. */
  k_ra8_rstsr0_bit_lvd1 = 2U, /**< Voltage-monitor 1 reset. */
  k_ra8_rstsr0_bit_lvd2 = 3U, /**< Voltage-monitor 2 reset. */
} ra8_rstsr0_bit_t;

/**
 * @enum ra8_rstsr2_bit_t
 * @brief Reset Status Register 2 bit positions.
 */
typedef enum : uint8_t {
  k_ra8_rstsr2_bit_cwsf   = 0U, /**< Cold / warm start flag. */
  k_ra8_rstsr2_bit_iwdtrf = 1U, /**< IWDT reset.             */
  k_ra8_rstsr2_bit_wdtrf  = 2U, /**< WDT reset.              */
  k_ra8_rstsr2_bit_swrf   = 3U, /**< Software reset.         */
} ra8_rstsr2_bit_t;

/* =============================================================================
 * Direct register accessors
 * =============================================================================
 *
 * Rather than define a single bloated R_SYSTEM struct (3456 bytes of
 * bitfields), we expose a tiny set of typed accessors that each return
 * a pointer to one register. The caller reads / writes whole 8/16/32-bit
 * values and masks bits using the named constants above. This is
 * exactly how the rx72n project handled its SYSTEM block and keeps the
 * header under 200 lines.
 */

/** @brief Get pointer to the 32-bit VSCR register (voltage scaling control). */
static inline volatile uint32_t* ra8_sys_vscr(void)
{
  return (volatile uint32_t*)((uintptr_t)k_ra8_system_base_addr + (uintptr_t)k_ra8_sys_off_vscr);
}

/** @brief Get pointer to the 32-bit SCKDIVCR register. */
static inline volatile uint32_t* ra8_sys_sckdivcr(void)
{
  return (volatile uint32_t*)((uintptr_t)k_ra8_system_base_addr +
                              (uintptr_t)k_ra8_sys_off_sckdivcr);
}

/** @brief Get pointer to the 8-bit SCICKDIVCR register (SCICLK divider). */
static inline volatile uint8_t* ra8_sys_scickdivcr(void)
{
  return (volatile uint8_t*)((uintptr_t)k_ra8_system_base_addr +
                             (uintptr_t)k_ra8_sys_off_scickdivcr);
}

/** @brief Get pointer to the 8-bit SCICKCR register (SCICLK source select). */
static inline volatile uint8_t* ra8_sys_scickcr(void)
{
  return (volatile uint8_t*)((uintptr_t)k_ra8_system_base_addr + (uintptr_t)k_ra8_sys_off_scickcr);
}

/** @brief Get pointer to the 8-bit SCKSCR register. */
static inline volatile uint8_t* ra8_sys_sckscr(void)
{
  return (volatile uint8_t*)((uintptr_t)k_ra8_system_base_addr + (uintptr_t)k_ra8_sys_off_sckscr);
}

/** @brief Get pointer to the 8-bit HOCOCR register. */
static inline volatile uint8_t* ra8_sys_hococr(void)
{
  return (volatile uint8_t*)((uintptr_t)k_ra8_system_base_addr + (uintptr_t)k_ra8_sys_off_hococr);
}

/** @brief Get pointer to the 8-bit SOSCCR register (sub-clock oscillator stop). */
static inline volatile uint8_t* ra8_sys_sosccr(void)
{
  return (volatile uint8_t*)((uintptr_t)k_ra8_system_base_addr + (uintptr_t)k_ra8_sys_off_sosccr);
}

/** @brief Get pointer to the 8-bit SOMCR register (sub-clock oscillator mode). */
static inline volatile uint8_t* ra8_sys_somcr(void)
{
  return (volatile uint8_t*)((uintptr_t)k_ra8_system_base_addr + (uintptr_t)k_ra8_sys_off_somcr);
}

/** @brief Get pointer to the 8-bit LOCOCR register (LOCO stop control). */
static inline volatile uint8_t* ra8_sys_lococr(void)
{
  return (volatile uint8_t*)((uintptr_t)k_ra8_system_base_addr + (uintptr_t)k_ra8_sys_off_lococr);
}

/** @brief Get pointer to the 8-bit MOSCWTCR register. */
static inline volatile uint8_t* ra8_sys_moscwtcr(void)
{
  return (volatile uint8_t*)((uintptr_t)k_ra8_system_base_addr + (uintptr_t)k_ra8_sys_off_moscwtcr);
}

/** @brief Get pointer to the 32-bit PLLCCR register. */
static inline volatile uint32_t* ra8_sys_pllccr(void)
{
  return (volatile uint32_t*)((uintptr_t)k_ra8_system_base_addr + (uintptr_t)k_ra8_sys_off_pllccr);
}

/** @brief Get pointer to the 16-bit PLLCCR2 register (PLL1 output dividers). */
static inline volatile uint16_t* ra8_sys_pllccr2(void)
{
  return (volatile uint16_t*)((uintptr_t)k_ra8_system_base_addr + (uintptr_t)k_ra8_sys_off_pllccr2);
}

/**
 * @brief Get pointer to the 32-bit PLL2CCR register (PLL2 input/mul/source).
 *
 * @details
 * PLL2CCR layout per RA8D2 HUM Ch 9.2.9 "PLL2CCR : PLL2 Clock Control
 * Register" (offset 0x0C8, 32-bit access):
 *   - PL2IDIV   [1:0]   -- input divider (0=/1, 1=/2, 2=/3)
 *   - PL2SRCSEL [4]     -- 0 = Main XTAL, 1 = HOCO
 *   - PLL2MULNF [7:6]   -- fractional multiplier in 0.25 steps
 *   - PLL2MUL   [16:8]  -- integer multiplier (9 bits, 0x27..0x12B = x40..x300)
 *   - bits 31:17, 5, 3:2 reserved (read 0, write 0)
 *
 * Note: this register on RA8D2 is 9-bit-MUL wide and located at offset
 * 0x0C8 (NOT 0x048 as in older RA8M1 family parts). The previous
 * accessor used the RA8M1 layout and silently dropped writes on real
 * RA8D2 silicon.
 *
 * @return Pointer to the live PLL2CCR register.
 * @pre  Single-threaded init context with PRCR group 0 unlocked.
 * @pre  PLL2 has been stopped (PLL2CR.PLL2STP = 1) before any write.
 * @post No mutation; pointer-only accessor.
 * @post Pointer aliases live MMIO -- caller must use volatile semantics.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static inline volatile uint32_t* ra8_sys_pll2ccr(void)
{
  return (volatile uint32_t*)((uintptr_t)k_ra8_system_base_addr + (uintptr_t)k_ra8_sys_off_pll2ccr);
}

/**
 * @brief Get pointer to the 8-bit PLL2CR register (PLL2 stop control).
 *
 * @details
 * PLL2CR.PLL2STP at bit 0: write 1 to stop PLL2, 0 to run it
 * (HUM Ch 9.2.11 "PLL2CR : PLL2 Control Register"; FSP
 * `R_SYSTEM_PLL2CR_PLL2STP_Pos`).
 *
 * @return Pointer to the live PLL2CR register.
 * @pre  Single-threaded init context with PRCR group 0 unlocked.
 * @pre  CPU is not currently sourced from PLL2 (would self-stall).
 * @post No mutation; pointer-only accessor.
 * @post Pointer aliases live MMIO.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static inline volatile uint8_t* ra8_sys_pll2cr(void)
{
  return (volatile uint8_t*)((uintptr_t)k_ra8_system_base_addr + (uintptr_t)k_ra8_sys_off_pll2cr);
}

/**
 * @brief Get pointer to the 16-bit PLL2CCR2 register (PLL2 output dividers).
 *
 * @details
 * PLL2CCR2 layout (HUM Ch 9.2.12 "PLL2CCR2 : PLL2 Clock Control Register 2",
 * FSP `R_SYSTEM_PLL2CCR2_PL2ODIVx_Pos`):
 *   - PL2ODIVP [3:0]   -- PLL2P output divider code
 *   - PL2ODIVQ [7:4]   -- PLL2Q output divider code
 *   - PL2ODIVR [11:8]  -- PLL2R output divider code
 *
 * Code-to-ratio map matches PLL1's `ra8_plodiv_t` (ratio - 1 for /1../6,
 * /8 = 7, /9 = 8, /16 = 15).
 *
 * @return Pointer to the live PLL2CCR2 register.
 * @pre  Single-threaded init context with PRCR group 0 unlocked.
 * @pre  PLL2 has been stopped before any write.
 * @post No mutation; pointer-only accessor.
 * @post Pointer aliases live MMIO.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static inline volatile uint16_t* ra8_sys_pll2ccr2(void)
{
  return (volatile uint16_t*)((uintptr_t)k_ra8_system_base_addr +
                              (uintptr_t)k_ra8_sys_off_pll2ccr2);
}

/** @brief Get pointer to the 8-bit OSCSF (oscillation stabilisation) register. */
static inline volatile uint8_t* ra8_sys_oscsf(void)
{
  return (volatile uint8_t*)((uintptr_t)k_ra8_system_base_addr + (uintptr_t)k_ra8_sys_off_oscsf);
}

/** @brief Get pointer to the 8-bit PLLCR register (PLL enable/stop). */
static inline volatile uint8_t* ra8_sys_pllcr(void)
{
  return (volatile uint8_t*)((uintptr_t)k_ra8_system_base_addr + (uintptr_t)k_ra8_sys_off_pllcr);
}

/** @brief Get pointer to the 16-bit SCKDIVCR2 register. */
static inline volatile uint16_t* ra8_sys_sckdivcr2(void)
{
  return (volatile uint16_t*)((uintptr_t)k_ra8_system_base_addr +
                              (uintptr_t)k_ra8_sys_off_sckdivcr2);
}

/** @brief Get pointer to the 32-bit RSTSR1 register. */
static inline volatile uint32_t* ra8_sys_rstsr1(void)
{
  return (volatile uint32_t*)((uintptr_t)k_ra8_system_base_addr + (uintptr_t)k_ra8_sys_off_rstsr1);
}

/** @brief Get pointer to the 16-bit PRCR register. */
static inline volatile uint16_t* ra8_sys_prcr(void)
{
  return (volatile uint16_t*)((uintptr_t)k_ra8_system_base_addr + (uintptr_t)k_ra8_sys_off_prcr);
}

/** @brief Get pointer to the 8-bit RSTSR0 register. */
static inline volatile uint8_t* ra8_sys_rstsr0(void)
{
  return (volatile uint8_t*)((uintptr_t)k_ra8_system_base_addr + (uintptr_t)k_ra8_sys_off_rstsr0);
}

/** @brief Get pointer to the 8-bit RSTSR2 register. */
static inline volatile uint8_t* ra8_sys_rstsr2(void)
{
  return (volatile uint8_t*)((uintptr_t)k_ra8_system_base_addr + (uintptr_t)k_ra8_sys_off_rstsr2);
}

/**
 * @enum ra8_usbckcr_bit_t
 * @brief USBCKCR / USB60CKCR bit positions (HUM Ch 9).
 *
 * @details Cited from FSP CMSIS device header
 * `R7KA8D2KF_core0.h`. USBCKSEL[3:0] selects the source
 * clock; USBCKSREQ requests a switch; USBCKSRDY is the read-only
 * handshake-done flag.
 */
typedef enum : uint8_t {
  k_ra8_usbckcr_shift_sel = 0U,    /**< USBCKSEL field starts at bit 0. */
  k_ra8_usbckcr_mask_sel  = 0x0FU, /**< USBCKSEL field is 4 bits wide.  */
  k_ra8_usbckcr_bit_sreq  = 6U,    /**< USBCKSREQ -- request switch.    */
  k_ra8_usbckcr_bit_srdy  = 7U,    /**< USBCKSRDY -- handshake ready.   */
} ra8_usbckcr_bit_t;

/**
 * @enum ra8_usbcksel_t
 * @brief USBCKCR.USBCKSEL source codes (RA8D2 mapping).
 */
typedef enum : uint8_t {
  k_ra8_usbcksel_hoco  = 0U,  /**< HOCO.      */
  k_ra8_usbcksel_main  = 3U,  /**< Main XTAL. */
  k_ra8_usbcksel_pll1p = 5U,  /**< PLL1P.     */
  k_ra8_usbcksel_pll2p = 6U,  /**< PLL2P.     */
  k_ra8_usbcksel_pll1q = 7U,  /**< PLL1Q.     */
  k_ra8_usbcksel_pll1r = 8U,  /**< PLL1R.     */
  k_ra8_usbcksel_pll2q = 9U,  /**< PLL2Q.     */
  k_ra8_usbcksel_pll2r = 10U, /**< PLL2R.     */
} ra8_usbcksel_t;

/** @brief Get pointer to the 8-bit USBCKDIVCR (USB clock divider). */
static inline volatile uint8_t* ra8_sys_usbckdivcr(void)
{
  return (volatile uint8_t*)((uintptr_t)k_ra8_system_base_addr +
                             (uintptr_t)k_ra8_sys_off_usbckdivcr);
}

/** @brief Get pointer to the 8-bit USBCKCR (USB-FS source select). */
static inline volatile uint8_t* ra8_sys_usbckcr(void)
{
  return (volatile uint8_t*)((uintptr_t)k_ra8_system_base_addr + (uintptr_t)k_ra8_sys_off_usbckcr);
}

/** @brief Get pointer to the 8-bit USB60CKCR (USBHS 60MHz source select). */
static inline volatile uint8_t* ra8_sys_usb60ckcr(void)
{
  return (volatile uint8_t*)((uintptr_t)k_ra8_system_base_addr +
                             (uintptr_t)k_ra8_sys_off_usb60ckcr);
}

/** @brief Get pointer to the 8-bit USB60CKDIVCR (USBHS 60MHz divider). */
static inline volatile uint8_t* ra8_sys_usb60ckdivcr(void)
{
  return (volatile uint8_t*)((uintptr_t)k_ra8_system_base_addr +
                             (uintptr_t)k_ra8_sys_off_usb60ckdivcr);
}

/** @brief Get pointer to the 8-bit CANFDCKCR (CANFD core clock source select).
 *
 * @details HUM Ch 9.2.46 "CANFDCKCR : CANFD Core Clock Control Register"
 * p 366. Layout matches USBCKCR: CANFDCKSEL[3:0] at [3:0], CANFDCKSREQ
 * at bit 6, CANFDCKSRDY at bit 7. Reset value 0x01 selects MOCO.
 */
static inline volatile uint8_t* ra8_sys_canfdckcr(void)
{
  return (volatile uint8_t*)((uintptr_t)k_ra8_system_base_addr +
                             (uintptr_t)k_ra8_sys_off_canfdckcr);
}

/** @brief Get pointer to the 8-bit CANFDCKDIVCR (CANFD core clock divider).
 *
 * @details HUM Ch 9.2.41 "CANFDCKDIVCR : CANFD Core Clock Division Control
 * Register" p 363. CANFDCKDIV[3:0] in bits [3:0].
 */
static inline volatile uint8_t* ra8_sys_canfdckdivcr(void)
{
  return (volatile uint8_t*)((uintptr_t)k_ra8_system_base_addr +
                             (uintptr_t)k_ra8_sys_off_canfdckdivcr);
}

/** @brief Get pointer to the 8-bit OCTACKCR (Octal-SPI clock source select).
 *
 * @details HUM Ch 9.2.45 "OCTACKCR : Octal-SPI Clock Control Register"
 * p 360. Layout matches USBCKCR / CANFDCKCR: OCTACKSEL[3:0] at [3:0],
 * OCTACKSREQ at bit 6, OCTACKSRDY at bit 7. Reset value 0x01 selects
 * MOCO. Per HUM Ch 11.2.7 MSTPCRB Note 3 (p 444) the OSPI MSTPB16 /
 * MSTPB17 bits must be written AFTER OCTACLK is stable; the SREQ ->
 * SRDY -> SREQ-clear handshake is the documented way to confirm
 * stability.
 */
static inline volatile uint8_t* ra8_sys_octackcr(void)
{
  return (volatile uint8_t*)((uintptr_t)k_ra8_system_base_addr + (uintptr_t)k_ra8_sys_off_octackcr);
}

/** @brief Get pointer to the 8-bit OCTACKDIVCR (Octal-SPI clock divider).
 *
 * @details HUM Ch 9.2.40 "OCTACKDIVCR : Octal-SPI Clock Division Control
 * Register" p 357. OCTACKDIV[3:0] in bits [3:0]; reset value 0 (/1).
 */
static inline volatile uint8_t* ra8_sys_octackdivcr(void)
{
  return (volatile uint8_t*)((uintptr_t)k_ra8_system_base_addr +
                             (uintptr_t)k_ra8_sys_off_octackdivcr);
}

/**
 * @enum ra8_eswcksel_t
 * @brief ESWCKCR.CKSEL source codes (RA8D2 mapping).
 *
 * @details
 * Layout matches USBCKCR / USB60CKCR -- the CGC uses the same
 * peripheral-clock-source enum for every CKSEL field. The Renesas
 * FSP confirms this in `bsp_clocks.c` (single `BSP_CLOCKS_SOURCE_CLOCK_*`
 * enum drives every peripheral clock select).
 */
typedef enum : uint8_t {
  k_ra8_eswcksel_hoco  = 0U,  /**< HOCO.                                 */
  k_ra8_eswcksel_moco  = 1U,  /**< MOCO.                                 */
  k_ra8_eswcksel_loco  = 2U,  /**< LOCO.                                 */
  k_ra8_eswcksel_main  = 3U,  /**< Main XTAL.                            */
  k_ra8_eswcksel_pll1p = 5U,  /**< PLL1P (preferred for 125 MHz ESWCLK). */
  k_ra8_eswcksel_pll2p = 6U,  /**< PLL2P.                                */
  k_ra8_eswcksel_pll1q = 7U,  /**< PLL1Q.                                */
  k_ra8_eswcksel_pll1r = 8U,  /**< PLL1R.                                */
  k_ra8_eswcksel_pll2q = 9U,  /**< PLL2Q.                                */
  k_ra8_eswcksel_pll2r = 10U, /**< PLL2R.                                */
} ra8_eswcksel_t;

/**
 * @enum ra8_eswckcr_bit_t
 * @brief Bit positions in ESWCKCR.
 */
typedef enum : uint8_t {
  k_ra8_eswckcr_shift_sel = 0U,    /**< CKSEL field starts at bit 0. */
  k_ra8_eswckcr_mask_sel  = 0x0FU, /**< CKSEL field is 4 bits wide.  */
  k_ra8_eswckcr_bit_sreq  = 6U,    /**< CKSREQ -- request switch.    */
  k_ra8_eswckcr_bit_srdy  = 7U,    /**< CKSRDY -- handshake ready.   */
} ra8_eswckcr_bit_t;

/**
 * @brief Get pointer to the 8-bit ESWCKCR (Ethernet Switch / RMAC clock source).
 *
 * @details
 * HUM Ch 9.2 "ESWCKCR" -- 8-bit register, layout identical to USB60CKCR:
 *   - CKSEL[3:0] : clock source code (same enum as ra8_usbcksel_t).
 *   - CKSREQ bit 6 : write 1 to request a switch.
 *   - CKSRDY bit 7 : read-only handshake-done flag.
 *
 * The ESWCLK feeds the Ethernet Switch Module (ESWM) and the per-port
 * RMAC PHY-management clock divider MPIC.PSMCS. Without a valid ESWCLK
 * the MDC line never toggles at the rate the PHY expects (max 2.5 MHz
 * for Clause-22).
 */
static inline volatile uint8_t* ra8_sys_eswckcr(void)
{
  return (volatile uint8_t*)((uintptr_t)k_ra8_system_base_addr + (uintptr_t)k_ra8_sys_off_eswckcr);
}

/** @brief Get pointer to the 8-bit ESWCKDIVCR (ESWCLK divider; 4-bit CKDIV field). */
static inline volatile uint8_t* ra8_sys_eswckdivcr(void)
{
  return (volatile uint8_t*)((uintptr_t)k_ra8_system_base_addr +
                             (uintptr_t)k_ra8_sys_off_eswckdivcr);
}

/**
 * @brief Get pointer to the 8-bit ESWPCKCR (Ethernet-PHY clock source).
 *
 * @details
 * ESWPHYCLK is the high-speed clock fed into the per-port RMAC pin pad
 * timing (in addition to ESWCLK feeding the MDC divider). Without this
 * clock running, the entire RMAC / ETHA register block reads back as
 * 0 and writes are silently dropped -- exactly the failure mode seen
 * when only ESWCKCR is programmed but ESWPCKCR is left at its reset
 * default of MOCO/1 ~= 8 MHz. Layout identical to ESWCKCR (same
 * ::ra8_eswckcr_bit_t shifts and ::ra8_eswcksel_t source enum).
 */
static inline volatile uint8_t* ra8_sys_eswpckcr(void)
{
  return (volatile uint8_t*)((uintptr_t)k_ra8_system_base_addr + (uintptr_t)k_ra8_sys_off_eswpckcr);
}

/** @brief Get pointer to the 8-bit ESWPCKDIVCR (ESWPHYCLK divider; 4-bit CKDIV field). */
static inline volatile uint8_t* ra8_sys_eswpckdivcr(void)
{
  return (volatile uint8_t*)((uintptr_t)k_ra8_system_base_addr +
                             (uintptr_t)k_ra8_sys_off_eswpckdivcr);
}

/**
 * @enum ra8_pdctreswm_bit_t
 * @brief Bit positions in PDCTRESWM (ESWM power-domain control).
 *
 * @details
 * HUM Ch 9.2 "General Power Domain ESWM Control Register" + FSP
 * R_SYSTEM_PDCTRESWM_PDDE_Pos. The PDDE bit is a write-1-to-DISABLE
 * (counter-intuitive but matches the FSP comment "Turn on the domain
 * power" by writing PDDE = 0). PDCSF / PDPGSF are read-only status
 * flags that go to 0 once the requested transition completes.
 *
 * Without ``PDDE = 0`` the ESWM peripheral domain stays power-gated
 * and the per-port RMAC / ETHA register windows read back 0 and
 * silently drop every write -- observed on EK-RA8D2 hardware.
 */
typedef enum : uint8_t {
  k_ra8_pdctr_bit_pdde   = 0U, /**< 1 = power disabled, 0 = power enabled. */
  k_ra8_pdctr_bit_pdcsf  = 6U, /**< Read-only: power-control status flag.  */
  k_ra8_pdctr_bit_pdpgsf = 7U, /**< Read-only: power-gating status flag.   */
} ra8_pdctreswm_bit_t;

/** @brief Get pointer to the 8-bit PDCTRESWM (ESWM power-domain control). */
static inline volatile uint8_t* ra8_sys_pdctreswm(void)
{
  return (volatile uint8_t*)((uintptr_t)k_ra8_system_base_addr +
                             (uintptr_t)k_ra8_sys_off_pdctreswm);
}

/* =============================================================================
 * PRCR unlock helpers
 * =============================================================================
 */

/**
 * @brief Unlock the CGC + LVD protection group so CGC registers can be written.
 *
 * @details
 * Writes `0xA501` to PRCR. Paired with `ra8_sys_prcr_lock_all()` once
 * the CGC programming sequence is complete.
 *
 * @note Not thread-safe. Call only from single-threaded init or with
 *       interrupts masked.
 *
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @since 0.1.0
 */
static inline void ra8_sys_prcr_unlock_cgc(void)
{
  *ra8_sys_prcr() = k_ra8_prcr_unlock_cgc;
}

/**
 * @brief Re-lock every protection group (write password with all group bits clear).
 *
 * @details See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static inline void ra8_sys_prcr_lock_all(void)
{
  *ra8_sys_prcr() = k_ra8_prcr_lock_all;
}

#ifdef __cplusplus
}
#endif
