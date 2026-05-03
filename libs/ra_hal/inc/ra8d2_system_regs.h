/**
 * @file ra8d2_system_regs.h
 * @brief System Control (SYSC) register layout for the Renesas RA8D2
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
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
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
  k_ra_system_base_addr = 0x4001E000UL, /**< R_SYSTEM. */
} ra_system_addr_t;

/* =============================================================================
 * Register offsets (within the SYSTEM block)
 * =============================================================================
 */

typedef enum : uint16_t {
  k_ra_sys_off_vscr       = 0x014U, /**< VSCR voltage scaling control (32-bit). */
  k_ra_sys_off_sckdivcr   = 0x020U, /**< SCKDIVCR  (32-bit). */
  k_ra_sys_off_sckdivcr2  = 0x024U, /**< SCKDIVCR2 (16-bit). */
  k_ra_sys_off_sckscr     = 0x026U, /**< SCKSCR    (8-bit).  */
  k_ra_sys_off_pllcr      = 0x02AU, /**< PLLCR     (8-bit).  */
  k_ra_sys_off_moscr      = 0x032U, /**< MOSCCR    (8-bit).  */
  k_ra_sys_off_hococr     = 0x036U, /**< HOCOCR    (8-bit).  */
  k_ra_sys_off_oscsf      = 0x03CU, /**< OSCSF     (8-bit).  */
  k_ra_sys_off_pll2ccr    = 0x048U, /**< PLL2CCR  -- PLL2 input/mul/source (16-bit). */
  k_ra_sys_off_pll2cr     = 0x04AU, /**< PLL2CR   -- PLL2 stop control (8-bit).      */
  k_ra_sys_off_pllccr2    = 0x04CU, /**< PLLCCR2 -- PLL1 output dividers (16-bit). */
  k_ra_sys_off_pll2ccr2   = 0x04EU, /**< PLL2CCR2 -- PLL2 output dividers (16-bit). */
  k_ra_sys_off_scickdivcr = 0x054U, /**< SCICKDIVCR (8-bit). */
  k_ra_sys_off_scickcr    = 0x055U, /**< SCICKCR    (8-bit). */
  k_ra_sys_off_usbckdivcr = 0x06CU, /**< USBCKDIVCR (8-bit) -- USB clock divider. */
  k_ra_sys_off_usbckcr    = 0x074U, /**< USBCKCR    (8-bit) -- USB-FS clock src. */
  k_ra_sys_off_usb60ckcr  = 0x077U, /**< USB60CKCR  (8-bit) -- USBHS 60MHz src. */
  k_ra_sys_off_moscwtcr   = 0x0A2U, /**< MOSCWTCR  (8-bit).  */
  k_ra_sys_off_pllccr     = 0x0ACU, /**< PLLCCR    (32-bit). */
  k_ra_sys_off_rstsr1     = 0x0C0U, /**< RSTSR1    (32-bit). */
  k_ra_sys_off_prcr       = 0x3FAU, /**< PRCR      (16-bit). */
  k_ra_sys_off_rstsr0     = 0xA40U, /**< RSTSR0    (8-bit).  */
  k_ra_sys_off_rstsr2     = 0xA44U, /**< RSTSR2    (8-bit).  */
} ra_system_offset_t;

/**
 * @enum ra_vscr_bit_t
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
  k_ra_vscr_bit_vscm    = (1UL << 0U), /**< VSCM bit mask (write 1 = not-high-V). */
  k_ra_vscr_bit_vscmtsf = (1UL << 4U), /**< VSCMTSF transition-in-flight mask.    */
} ra_vscr_bit_t;

/**
 * @enum ra_moscr_bit_t
 * @brief Bit masks in MOSCCR (Main Clock Oscillator Control Register).
 *
 * @details
 * MOSTP at bit 0: write 0 to start the main oscillator, 1 to stop it
 * (HUM Ch 9.2.13).
 */
typedef enum : uint8_t {
  k_ra_moscr_mostp_mask = (1U << 0U), /**< MOSTP -- 0 starts XTAL, 1 stops it. */
} ra_moscr_bit_t;

/**
 * @enum ra_oscsf_bit_t
 * @brief Oscillation Stabilization Flag bit positions.
 *
 * @details
 * Each bit is set by hardware when the corresponding oscillator has
 * stabilised. The CGC driver polls these between starting an
 * oscillator and using it as the system-clock source.
 */
typedef enum : uint8_t {
  k_ra_oscsf_bit_hocosf = 0U, /**< HOCO stabilisation flag.    */
  k_ra_oscsf_bit_moscsf = 3U, /**< Main oscillator stab flag.  */
  k_ra_oscsf_bit_pll1sf = 5U, /**< PLL1 stabilisation flag.    */
  k_ra_oscsf_bit_pll2sf = 6U, /**< PLL2 stabilisation flag.    */
} ra_oscsf_bit_t;

/* =============================================================================
 * PRCR protection key and group masks
 * =============================================================================
 */

/**
 * @enum ra_prcr_t
 * @brief Values to write to PRCR to unlock / lock protection groups.
 *
 * @details
 * `k_ra_prcr_key` is the mandatory 0xA5 password in the upper byte.
 * OR with one of the group bits to *enable writes* to that group
 * (counter-intuitive -- bit set = unlocked), then write back with the
 * group bit cleared to relock.
 */
typedef enum : uint16_t {
  k_ra_prcr_key        = 0xA500U, /**< Password in upper byte.           */
  k_ra_prcr_grp0_cgc   = 0x0001U, /**< Group 0: CGC + LVD.               */
  k_ra_prcr_grp1_lpm   = 0x0002U, /**< Group 1: Low-power modes.         */
  k_ra_prcr_grp2_osc   = 0x0008U, /**< Group 2: LVD reset-detection.     */
  k_ra_prcr_unlock_cgc = (uint16_t)(k_ra_prcr_key | k_ra_prcr_grp0_cgc),
  k_ra_prcr_lock_all   = k_ra_prcr_key,
} ra_prcr_t;

/* =============================================================================
 * Reset-cause bits (subset -- add as needed)
 * =============================================================================
 */

/**
 * @enum ra_rstsr0_bit_t
 * @brief Reset Status Register 0 bit positions.
 *
 * @details
 * Set by hardware when a reset of the corresponding cause occurred.
 * Cleared by software (write 0 after unlocking PRCR group 0).
 */
typedef enum : uint8_t {
  k_ra_rstsr0_bit_porf = 0U, /**< Power-on reset.            */
  k_ra_rstsr0_bit_lvd0 = 1U, /**< Voltage-monitor 0 reset.   */
  k_ra_rstsr0_bit_lvd1 = 2U, /**< Voltage-monitor 1 reset.   */
  k_ra_rstsr0_bit_lvd2 = 3U, /**< Voltage-monitor 2 reset.   */
} ra_rstsr0_bit_t;

/**
 * @enum ra_rstsr2_bit_t
 * @brief Reset Status Register 2 bit positions.
 */
typedef enum : uint8_t {
  k_ra_rstsr2_bit_cwsf   = 0U, /**< Cold / warm start flag.     */
  k_ra_rstsr2_bit_iwdtrf = 1U, /**< IWDT reset.                 */
  k_ra_rstsr2_bit_wdtrf  = 2U, /**< WDT reset.                  */
  k_ra_rstsr2_bit_swrf   = 3U, /**< Software reset.             */
} ra_rstsr2_bit_t;

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
static inline volatile uint32_t* ra_sys_vscr(void)
{
  return (volatile uint32_t*)(k_ra_system_base_addr + k_ra_sys_off_vscr);
}

/** @brief Get pointer to the 32-bit SCKDIVCR register. */
static inline volatile uint32_t* ra_sys_sckdivcr(void)
{
  return (volatile uint32_t*)(k_ra_system_base_addr + k_ra_sys_off_sckdivcr);
}

/** @brief Get pointer to the 8-bit SCICKDIVCR register (SCICLK divider). */
static inline volatile uint8_t* ra_sys_scickdivcr(void)
{
  return (volatile uint8_t*)(k_ra_system_base_addr + k_ra_sys_off_scickdivcr);
}

/** @brief Get pointer to the 8-bit SCICKCR register (SCICLK source select). */
static inline volatile uint8_t* ra_sys_scickcr(void)
{
  return (volatile uint8_t*)(k_ra_system_base_addr + k_ra_sys_off_scickcr);
}

/** @brief Get pointer to the 8-bit SCKSCR register. */
static inline volatile uint8_t* ra_sys_sckscr(void)
{
  return (volatile uint8_t*)(k_ra_system_base_addr + k_ra_sys_off_sckscr);
}

/** @brief Get pointer to the 8-bit HOCOCR register. */
static inline volatile uint8_t* ra_sys_hococr(void)
{
  return (volatile uint8_t*)(k_ra_system_base_addr + k_ra_sys_off_hococr);
}

/** @brief Get pointer to the 8-bit MOSCWTCR register. */
static inline volatile uint8_t* ra_sys_moscwtcr(void)
{
  return (volatile uint8_t*)(k_ra_system_base_addr + k_ra_sys_off_moscwtcr);
}

/** @brief Get pointer to the 32-bit PLLCCR register. */
static inline volatile uint32_t* ra_sys_pllccr(void)
{
  return (volatile uint32_t*)(k_ra_system_base_addr + k_ra_sys_off_pllccr);
}

/** @brief Get pointer to the 16-bit PLLCCR2 register (PLL1 output dividers). */
static inline volatile uint16_t* ra_sys_pllccr2(void)
{
  return (volatile uint16_t*)(k_ra_system_base_addr + k_ra_sys_off_pllccr2);
}

/**
 * @brief Get pointer to the 16-bit PLL2CCR register (PLL2 input/mul/source).
 *
 * @details
 * PLL2CCR layout (HUM Ch 9.2.10 "PLL2CCR : PLL2 Clock Control Register",
 * cross-reference FSP CMSIS device header `R7FA8M1AH.h` and
 * `R_SYSTEM_PLL2CCR_*_Pos`):
 *   - PL2IDIV   [1:0]  -- input divider (0=/1, 1=/2, 2=/3)
 *   - PL2SRCSEL [4]    -- 0 = Main XTAL, 1 = HOCO
 *   - PLL2MULNF [7:6]  -- fractional multiplier in 0.25 steps
 *   - PLL2MUL   [15:8] -- integer multiplier (8 bits, 1..255)
 *
 * @return Pointer to the live PLL2CCR register.
 * @pre  Single-threaded init context with PRCR group 0 unlocked.
 * @pre  PLL2 has been stopped (PLL2CR.PLL2STP = 1) before any write.
 * @post No mutation; pointer-only accessor.
 * @post Pointer aliases live MMIO -- caller must use volatile semantics.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static inline volatile uint16_t* ra_sys_pll2ccr(void)
{
  return (volatile uint16_t*)(k_ra_system_base_addr + k_ra_sys_off_pll2ccr);
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
static inline volatile uint8_t* ra_sys_pll2cr(void)
{
  return (volatile uint8_t*)(k_ra_system_base_addr + k_ra_sys_off_pll2cr);
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
 * Code-to-ratio map matches PLL1's `ra_plodiv_t` (ratio - 1 for /1../6,
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
static inline volatile uint16_t* ra_sys_pll2ccr2(void)
{
  return (volatile uint16_t*)(k_ra_system_base_addr + k_ra_sys_off_pll2ccr2);
}

/** @brief Get pointer to the 8-bit OSCSF (oscillation stabilisation) register. */
static inline volatile uint8_t* ra_sys_oscsf(void)
{
  return (volatile uint8_t*)(k_ra_system_base_addr + k_ra_sys_off_oscsf);
}

/** @brief Get pointer to the 8-bit PLLCR register (PLL enable/stop). */
static inline volatile uint8_t* ra_sys_pllcr(void)
{
  return (volatile uint8_t*)(k_ra_system_base_addr + k_ra_sys_off_pllcr);
}

/** @brief Get pointer to the 16-bit SCKDIVCR2 register. */
static inline volatile uint16_t* ra_sys_sckdivcr2(void)
{
  return (volatile uint16_t*)(k_ra_system_base_addr + k_ra_sys_off_sckdivcr2);
}

/** @brief Get pointer to the 32-bit RSTSR1 register. */
static inline volatile uint32_t* ra_sys_rstsr1(void)
{
  return (volatile uint32_t*)(k_ra_system_base_addr + k_ra_sys_off_rstsr1);
}

/** @brief Get pointer to the 16-bit PRCR register. */
static inline volatile uint16_t* ra_sys_prcr(void)
{
  return (volatile uint16_t*)(k_ra_system_base_addr + k_ra_sys_off_prcr);
}

/** @brief Get pointer to the 8-bit RSTSR0 register. */
static inline volatile uint8_t* ra_sys_rstsr0(void)
{
  return (volatile uint8_t*)(k_ra_system_base_addr + k_ra_sys_off_rstsr0);
}

/** @brief Get pointer to the 8-bit RSTSR2 register. */
static inline volatile uint8_t* ra_sys_rstsr2(void)
{
  return (volatile uint8_t*)(k_ra_system_base_addr + k_ra_sys_off_rstsr2);
}

/**
 * @enum ra_usbckcr_bit_t
 * @brief USBCKCR / USB60CKCR bit positions (HUM Ch 9).
 *
 * @details Cited from FSP CMSIS device header
 * `R7KA8D2KF_core0.h`. USBCKSEL[3:0] selects the source
 * clock; USBCKSREQ requests a switch; USBCKSRDY is the read-only
 * handshake-done flag.
 */
typedef enum : uint8_t {
  k_ra_usbckcr_shift_sel = 0U,    /**< USBCKSEL field starts at bit 0.  */
  k_ra_usbckcr_mask_sel  = 0x0FU, /**< USBCKSEL field is 4 bits wide.   */
  k_ra_usbckcr_bit_sreq  = 6U,    /**< USBCKSREQ -- request switch.     */
  k_ra_usbckcr_bit_srdy  = 7U,    /**< USBCKSRDY -- handshake ready.    */
} ra_usbckcr_bit_t;

/**
 * @enum ra_usbcksel_t
 * @brief USBCKCR.USBCKSEL source codes (RA8D2 mapping).
 */
typedef enum : uint8_t {
  k_ra_usbcksel_hoco  = 0U,  /**< HOCO.     */
  k_ra_usbcksel_main  = 3U,  /**< Main XTAL.*/
  k_ra_usbcksel_pll1p = 5U,  /**< PLL1P.    */
  k_ra_usbcksel_pll2p = 6U,  /**< PLL2P.    */
  k_ra_usbcksel_pll1q = 7U,  /**< PLL1Q.    */
  k_ra_usbcksel_pll1r = 8U,  /**< PLL1R.    */
  k_ra_usbcksel_pll2q = 9U,  /**< PLL2Q.    */
  k_ra_usbcksel_pll2r = 10U, /**< PLL2R.    */
} ra_usbcksel_t;

/** @brief Get pointer to the 8-bit USBCKDIVCR (USB clock divider). */
static inline volatile uint8_t* ra_sys_usbckdivcr(void)
{
  return (volatile uint8_t*)(k_ra_system_base_addr + k_ra_sys_off_usbckdivcr);
}

/** @brief Get pointer to the 8-bit USBCKCR (USB-FS source select). */
static inline volatile uint8_t* ra_sys_usbckcr(void)
{
  return (volatile uint8_t*)(k_ra_system_base_addr + k_ra_sys_off_usbckcr);
}

/** @brief Get pointer to the 8-bit USB60CKCR (USBHS 60MHz source select). */
static inline volatile uint8_t* ra_sys_usb60ckcr(void)
{
  return (volatile uint8_t*)(k_ra_system_base_addr + k_ra_sys_off_usb60ckcr);
}

/* =============================================================================
 * PRCR unlock helpers
 * =============================================================================
 */

/**
 * @brief Unlock the CGC + LVD protection group so CGC registers can be written.
 *
 * @details
 * Writes `0xA501` to PRCR. Paired with `ra_sys_prcr_lock_all()` once
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
static inline void ra_sys_prcr_unlock_cgc(void)
{
  *ra_sys_prcr() = k_ra_prcr_unlock_cgc;
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
static inline void ra_sys_prcr_lock_all(void)
{
  *ra_sys_prcr() = k_ra_prcr_lock_all;
}

#ifdef __cplusplus
}
#endif
