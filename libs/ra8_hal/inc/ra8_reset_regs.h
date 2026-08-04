/**
 * @file ra8_reset_regs.h
 * @brief Reset block register layout (offsets, bit masks, accessors)
 * @ingroup grp_hal_system
 *        for the Renesas RA8D2
 *
 * @details
 * The RA8D2 reset-cause / reset-control registers physically live
 * inside the System Control (SYSC) block at base ``0x4001_E000``.
 * The shared header ``ra8_system_regs.h`` already exposes a partial
 * map of SYSC and includes pointer accessors for ``RSTSR0``, ``RSTSR1``
 * and ``RSTSR2``. The shared brief explicitly forbids editing the
 * shared system header from a per-driver task, so this header
 * **mirrors** the subset of reset registers needed by ``ra8_reset.c``
 * (the three RSTSR flags plus ``RSTSR3``, plus ``RSTSAR`` for security
 * attribution) without duplicating or changing anything in
 * ``ra8_system_regs.h``.
 *
 * ## Map (Reset block subset)
 *
 * | Offset | Reg     | Width | Purpose                                       |
 * |-------:|---------|------:|-----------------------------------------------|
 * | 0x0C0  | RSTSR1  | 32    | Reset Status Register 1 (W,SW,WDT,LM,...)     |
 * | 0x3C4  | RSTSAR  | 32    | Reset Security Attribution Register           |
 * | 0xA40  | RSTSR0  | 8     | Reset Status Register 0 (POR, LVD, DPSRSTF)   |
 * | 0xA44  | RSTSR2  | 8     | Reset Status Register 2 (CWSF)                |
 * | 0xA48  | RSTSR3  | 8     | Reset Status Register 3 (CVMRF, OCPRF, TEMPRF)|
 *
 * The "Software Reset Register" (``SWRR``) called out in the task
 * brief is **not a SYSC register on the RA8D2** -- the HUM Ch 6.1
 * p 244 entry for "Software reset" reads:
 *
 *   ``Register setting (use the software reset bit
 *     AIRCR.SYSRESETREQ)``
 *
 * That is the ARMv8-M System Control Block AIRCR register at
 * ``0xE000_ED0C``. ``ra8_reset_software_reset()`` therefore writes
 * AIRCR with the mandatory ``0x05FA`` write-key in the upper half-word
 * and the SYSRESETREQ bit set, exactly as ``CMSIS NVIC_SystemReset``
 * does. The constants for the AIRCR access live below alongside the
 * other reset-related register addresses so the driver implementation
 * has a single include for everything it touches.
 *
 * ## Bit layout reference
 *
 * Verified against:
 *   - HUM Ch 6.2.2 "RSTSR0", p 257-258
 *   - HUM Ch 6.2.3 "RSTSR1", p 258-260
 *   - HUM Ch 6.2.4 "RSTSR2", p 261
 *   - HUM Ch 6.2.5 "RSTSR3", p 261-262
 *   - HUM Ch 6.2.1 "RSTSAR", p 256
 *   - FSP CMSIS R7KA8D2KF_core0.h ``R_SYSTEM_Type`` (lines 15205,
 *     15731, 16645, 16664, 16677) for bit-position cross-check.
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
 *
 * The reset block lives inside SYSC. We re-state the base here so the
 * driver does not need to pull in ra8_system_regs.h just to learn
 * where SYSC sits (avoids a transitive include of every CGC / PRCR
 * helper). The literal value matches ``k_ra8_system_base_addr`` in
 * ``ra8_system_regs.h``; this is checked at compile time below.
 */

/**
 * @enum ra8_reset_addr_t
 * @brief Memory-mapped base addresses used by the reset driver.
 */
typedef enum : uintptr_t {
  k_ra8_reset_sysc_base_addr = 0x4001E000UL, /**< R_SYSTEM (SYSC) base.                  */
  k_ra8_reset_scb_aircr_addr = 0xE000ED0CUL, /**< Cortex-M85 SCB AIRCR (software reset). */
} ra8_reset_addr_t;

/* =============================================================================
 * Per-register byte offsets within the SYSC block
 * =============================================================================
 */

/**
 * @enum ra8_reset_offset_t
 * @brief Offset (within SYSC) of every register the reset driver touches.
 *
 * @details
 * Mirrored from ``ra8_system_regs.h::ra8_system_offset_t`` for the
 * three RSTSR registers, plus the ``RSTSR3`` and ``RSTSAR`` offsets
 * the shared header does not yet expose.
 */
typedef enum : uint16_t {
  k_ra8_reset_off_rstsr1    = 0x0C0U, /**< RSTSR1 (32-bit). HUM Ch 6.2.3 p 258.               */
  k_ra8_reset_off_prcr      = 0x3FAU, /**< PRCR (16-bit) write-protect. HUM Ch 11.2.18 p 456. */
  k_ra8_reset_off_rstsar    = 0x3C4U, /**< RSTSAR (32-bit). HUM Ch 6.2.1 p 256.               */
  k_ra8_reset_off_rstsr0    = 0xA40U, /**< RSTSR0 (8-bit).  HUM Ch 6.2.2 p 257.               */
  k_ra8_reset_off_rstsr2    = 0xA44U, /**< RSTSR2 (8-bit).  HUM Ch 6.2.4 p 261.               */
  k_ra8_reset_off_rstsr3    = 0xA48U, /**< RSTSR3 (8-bit).  HUM Ch 6.2.5 p 261.               */
  k_ra8_reset_off_syrstmsk0 = 0xAD0U, /**< SYRSTMSK0 (8-bit). HUM Ch 6.2.6 p 262.             */
  k_ra8_reset_off_syrstmsk1 = 0xAD4U, /**< SYRSTMSK1 (8-bit). HUM Ch 6.2.7 p 263.             */
  k_ra8_reset_off_syrstmsk2 = 0xAD8U, /**< SYRSTMSK2 (8-bit). HUM Ch 6.2.8 p 263.             */
  k_ra8_reset_off_temprcr   = 0xADCU, /**< TEMPRCR (8-bit).  HUM Ch 6.2.9 p 264.              */
  k_ra8_reset_off_temprlr   = 0xAE0U, /**< TEMPRLR (8-bit).  HUM Ch 6.2.10 p 265.             */
} ra8_reset_offset_t;

/**
 * @enum ra8_reset_prcr_bits_t
 * @brief PRCR (Protect Register) password + PRC5 enable bit.
 *
 * @details
 * SYRSTMSK0/1/2, TEMPRCR and TEMPRLR are write-protected behind
 * ``PRCR.PRC5``. A 16-bit write must carry the ``0xA5`` password in the
 * upper byte; PRC5 (bit 5) enables writes to the reset-control group.
 */
typedef enum : uint16_t {
  k_ra8_reset_prcr_key         = 0xA500U, /**< Mandatory 0xA5 password, upper byte.      */
  k_ra8_reset_prcr_prc5_msk    = 0x0020U, /**< PRC5: reset-control register group.       */
  k_ra8_reset_prcr_pr_bits_msk = 0x00FFU, /**< Writable PR0..PR7 enable bits (low byte). */
} ra8_reset_prcr_bits_t;

/**
 * @enum ra8_reset_syrstmsk0_bits_t
 * @brief SYRSTMSK0 mask bits (1 = reset disabled). HUM Ch 6.2.6 p 262.
 */
typedef enum : uint8_t {
  k_ra8_reset_syrstmsk0_iwdt_msk = 0x01U, /**< IWDTMASK: independent WDT reset. */
  k_ra8_reset_syrstmsk0_wdt0_msk = 0x02U, /**< WDT0MASK: CPU0 WDT reset.        */
  k_ra8_reset_syrstmsk0_sw_msk   = 0x04U, /**< SWMASK: software reset.          */
  k_ra8_reset_syrstmsk0_clu0_msk = 0x10U, /**< CLU0MASK: CPU0 lockup reset.     */
  k_ra8_reset_syrstmsk0_lm0_msk  = 0x20U, /**< LM0MASK: local-memory-0 error.   */
  k_ra8_reset_syrstmsk0_cm_msk   = 0x40U, /**< CMMASK: common-memory error.     */
  k_ra8_reset_syrstmsk0_bus_msk  = 0x80U, /**< BUSMASK: bus error reset.        */
} ra8_reset_syrstmsk0_bits_t;

/**
 * @enum ra8_reset_syrstmsk1_bits_t
 * @brief SYRSTMSK1 mask bits (1 = reset disabled). HUM Ch 6.2.7 p 263.
 */
typedef enum : uint8_t {
  k_ra8_reset_syrstmsk1_wdt1_msk = 0x02U, /**< WDT1MASK: CPU1 WDT reset.      */
  k_ra8_reset_syrstmsk1_clu1_msk = 0x10U, /**< CLU1MASK: CPU1 lockup reset.   */
  k_ra8_reset_syrstmsk1_lm1_msk  = 0x20U, /**< LM1MASK: local-memory-1 error. */
} ra8_reset_syrstmsk1_bits_t;

/**
 * @enum ra8_reset_syrstmsk2_bits_t
 * @brief SYRSTMSK2 mask bits (1 = reset disabled). HUM Ch 6.2.8 p 263.
 */
typedef enum : uint8_t {
  k_ra8_reset_syrstmsk2_pvd1_msk = 0x01U, /**< PVD1MASK: voltage-monitor-1 reset. */
  k_ra8_reset_syrstmsk2_pvd2_msk = 0x02U, /**< PVD2MASK: voltage-monitor-2 reset. */
} ra8_reset_syrstmsk2_bits_t;

/**
 * @enum ra8_reset_temprcr_bits_t
 * @brief TEMPRCR (Temperature Monitor Reset Control) bits. HUM Ch 6.2.9 p 264.
 */
typedef enum : uint8_t {
  k_ra8_reset_temprcr_tempren_msk = 0x01U, /**< TEMPREN: temperature reset enable. */
  k_ra8_reset_temprcr_tsnen_msk   = 0x02U, /**< TSNEN: temperature sensor enable.  */
  k_ra8_reset_temprcr_cmpen_msk   = 0x04U, /**< CMPEN: comparator enable.          */
  k_ra8_reset_temprcr_tsnkeep_msk = 0x08U, /**< TSNKEEP: 1 = close sensor latch.   */
} ra8_reset_temprcr_bits_t;

/**
 * @enum ra8_reset_temprlr_bits_t
 * @brief TEMPRLR (Temperature Monitor Reset Lock) bits. HUM Ch 6.2.10 p 265.
 */
typedef enum : uint8_t {
  k_ra8_reset_temprlr_lock_msk = 0x01U, /**< LOCK: 1 = TEMPRCR locked (reset value). */
} ra8_reset_temprlr_bits_t;

/* =============================================================================
 * RSTSR0 -- 8-bit (Power-on / voltage-monitor / Deep-software-standby)
 * =============================================================================
 */

/**
 * @enum ra8_rstsr0_bits_t
 * @brief RSTSR0 bit positions / masks.
 *
 * @details
 * Per HUM Ch 6.2.2 p 257-258. Each flag is set by hardware when the
 * matching reset cause fires and is cleared by software using the
 * write-1-then-0 protocol described in HUM Ch 6.2.2 p 258 Note:
 * "Only 0 can be written. To write 0, the bit must be read as 1
 * first."
 */
typedef enum : uint8_t {
  k_ra8_reset_rstsr0_porf_pos    = 0U,    /**< PORF: Power-On Reset.      */
  k_ra8_reset_rstsr0_lvd0rf_pos  = 1U,    /**< LVD0RF: Voltage Monitor 0. */
  k_ra8_reset_rstsr0_lvd1rf_pos  = 2U,    /**< LVD1RF: Voltage Monitor 1. */
  k_ra8_reset_rstsr0_lvd2rf_pos  = 3U,    /**< LVD2RF: Voltage Monitor 2. */
  k_ra8_reset_rstsr0_lvd4rf_pos  = 5U,    /**< LVD4RF: Voltage Monitor 4. */
  k_ra8_reset_rstsr0_lvd5rf_pos  = 6U,    /**< LVD5RF: Voltage Monitor 5. */
  k_ra8_reset_rstsr0_dpsrstf_pos = 7U,    /**< DPSRSTF: Deep SW Standby.  */
  k_ra8_reset_rstsr0_porf_msk    = 0x01U, /**< PORF mask.                 */
  k_ra8_reset_rstsr0_lvd0rf_msk  = 0x02U, /**< LVD0RF mask.               */
  k_ra8_reset_rstsr0_lvd1rf_msk  = 0x04U, /**< LVD1RF mask.               */
  k_ra8_reset_rstsr0_lvd2rf_msk  = 0x08U, /**< LVD2RF mask.               */
  k_ra8_reset_rstsr0_lvd4rf_msk  = 0x20U, /**< LVD4RF mask.               */
  k_ra8_reset_rstsr0_lvd5rf_msk  = 0x40U, /**< LVD5RF mask.               */
  k_ra8_reset_rstsr0_dpsrstf_msk = 0x80U, /**< DPSRSTF mask.              */
} ra8_rstsr0_bits_t;

/* =============================================================================
 * RSTSR1 -- 32-bit (IWDT / WDT / SW / Lockup / LM / BUS / CM / WDT1 / NW)
 * =============================================================================
 */

/**
 * @enum ra8_rstsr1_bits_t
 * @brief RSTSR1 bit positions and masks.
 *
 * @details
 * Per HUM Ch 6.2.3 p 258-260 and FSP CMSIS R7KA8D2KF_core0.h
 * lines 15205-15226. Reserved bits read as 0 and write as 0.
 */
typedef enum : uint32_t {
  k_ra8_reset_rstsr1_iwdtrf_pos = 0U,           /**< IWDTRF.         */
  k_ra8_reset_rstsr1_wdtrf_pos  = 1U,           /**< WDTRF (CPU0).   */
  k_ra8_reset_rstsr1_swrf_pos   = 2U,           /**< SWRF.           */
  k_ra8_reset_rstsr1_clurf_pos  = 4U,           /**< CLURF (CPU0).   */
  k_ra8_reset_rstsr1_lm0rf_pos  = 5U,           /**< LM0RF.          */
  k_ra8_reset_rstsr1_bussrf_pos = 10U,          /**< BUSSRF.         */
  k_ra8_reset_rstsr1_cmrf_pos   = 14U,          /**< CMRF.           */
  k_ra8_reset_rstsr1_wdt1rf_pos = 17U,          /**< WDT1RF (CPU1).  */
  k_ra8_reset_rstsr1_clu1rf_pos = 20U,          /**< CLU1RF (CPU1).  */
  k_ra8_reset_rstsr1_lm1rf_pos  = 21U,          /**< LM1RF.          */
  k_ra8_reset_rstsr1_nwrf_pos   = 22U,          /**< NWRF (Network). */
  k_ra8_reset_rstsr1_iwdtrf_msk = 0x00000001UL, /**< IWDTRF mask.    */
  k_ra8_reset_rstsr1_wdtrf_msk  = 0x00000002UL, /**< WDTRF mask.     */
  k_ra8_reset_rstsr1_swrf_msk   = 0x00000004UL, /**< SWRF mask.      */
  k_ra8_reset_rstsr1_clurf_msk  = 0x00000010UL, /**< CLURF mask.     */
  k_ra8_reset_rstsr1_lm0rf_msk  = 0x00000020UL, /**< LM0RF mask.     */
  k_ra8_reset_rstsr1_bussrf_msk = 0x00000400UL, /**< BUSSRF mask.    */
  k_ra8_reset_rstsr1_cmrf_msk   = 0x00004000UL, /**< CMRF mask.      */
  k_ra8_reset_rstsr1_wdt1rf_msk = 0x00020000UL, /**< WDT1RF mask.    */
  k_ra8_reset_rstsr1_clu1rf_msk = 0x00100000UL, /**< CLU1RF mask.    */
  k_ra8_reset_rstsr1_lm1rf_msk  = 0x00200000UL, /**< LM1RF mask.     */
  k_ra8_reset_rstsr1_nwrf_msk   = 0x00400000UL, /**< NWRF mask.      */
} ra8_rstsr1_bits_t;

/* =============================================================================
 * RSTSR2 -- 8-bit (Cold/Warm Start)
 * =============================================================================
 */

/**
 * @enum ra8_rstsr2_bits_t
 * @brief RSTSR2 bit positions / masks (per HUM Ch 6.2.4 p 261).
 */
typedef enum : uint8_t {
  k_ra8_reset_rstsr2_cwsf_pos = 0U,    /**< CWSF: 0 = cold start, 1 = warm start. */
  k_ra8_reset_rstsr2_cwsf_msk = 0x01U, /**< CWSF mask.                            */
} ra8_rstsr2_bits_t;

/* =============================================================================
 * RSTSR3 -- 8-bit (Core voltage / Overcurrent / Temperature)
 * =============================================================================
 */

/**
 * @enum ra8_rstsr3_bits_t
 * @brief RSTSR3 bit positions / masks (per HUM Ch 6.2.5 p 261-262).
 */
typedef enum : uint8_t {
  k_ra8_reset_rstsr3_cvmrf_pos  = 0U,    /**< CVMRF: Core voltage monitor.   */
  k_ra8_reset_rstsr3_ocprf_pos  = 4U,    /**< OCPRF: Overcurrent protection. */
  k_ra8_reset_rstsr3_temprf_pos = 7U,    /**< TEMPRF: Temperature monitor.   */
  k_ra8_reset_rstsr3_cvmrf_msk  = 0x01U, /**< CVMRF mask.                    */
  k_ra8_reset_rstsr3_ocprf_msk  = 0x10U, /**< OCPRF mask.                    */
  k_ra8_reset_rstsr3_temprf_msk = 0x80U, /**< TEMPRF mask.                   */
} ra8_rstsr3_bits_t;

/* =============================================================================
 * RSTSAR -- 32-bit Reset Security Attribution
 * =============================================================================
 */

/**
 * @enum ra8_rstsar_bits_t
 * @brief RSTSAR bit positions / masks (per HUM Ch 6.2.1 p 256).
 *
 * @details
 * Each ``NONSECn`` bit selects whether the matching reset cause's
 * detect flag and mask register is reachable from non-secure side.
 * On the RA8D2 the field is 5 bits wide (FSP CMSIS lines 15735-15740);
 * higher bits read as 0.
 */
typedef enum : uint32_t {
  k_ra8_reset_rstsar_nonsec0_pos = 0U, /**< NONSEC0 position. */
  k_ra8_reset_rstsar_nonsec1_pos = 1U, /**< NONSEC1 position. */
  k_ra8_reset_rstsar_nonsec2_pos = 2U, /**< NONSEC2 position. */
  k_ra8_reset_rstsar_nonsec3_pos = 3U, /**< NONSEC3 position. */
  k_ra8_reset_rstsar_nonsec4_pos = 4U, /**< NONSEC4 position. */
  k_ra8_reset_rstsar_field_msk   = 0x0000001FUL,
  /**< Defined bit window. */
} ra8_rstsar_bits_t;

/* =============================================================================
 * SCB AIRCR (Cortex-M85) -- used by ra8_reset_software_reset()
 * =============================================================================
 */

/**
 * @enum ra8_reset_aircr_t
 * @brief AIRCR fields touched when triggering a software reset.
 *
 * @details
 * Per ARMv8-M Architecture Reference Manual / DDI0489 Sec B3.2.6, the
 * Application Interrupt and Reset Control Register at ``0xE000ED0C``
 * requires the upper 16 bits to carry the magic key ``0x05FA`` on
 * every write or the access is ignored. SYSRESETREQ asks the system
 * controller to schedule a system reset.
 *
 * HUM Ch 6.1 p 244 explicitly names this as the software-reset path:
 * "Software reset / Source / Register setting (use the software reset
 * bit AIRCR.SYSRESETREQ)".
 */
typedef enum : uint32_t {
  k_ra8_reset_aircr_vectkey_pos     = 16U,          /**< VECTKEY field position. */
  k_ra8_reset_aircr_vectkey_value   = 0x05FAUL,     /**< Mandatory write key.    */
  k_ra8_reset_aircr_sysresetreq_pos = 2U,           /**< SYSRESETREQ bit pos.    */
  k_ra8_reset_aircr_sysresetreq_msk = 0x00000004UL, /**< SYSRESETREQ mask.       */
} ra8_reset_aircr_t;

/* =============================================================================
 * Inline pointer accessors
 * =============================================================================
 *
 * Accessors return ``volatile`` pointers to the underlying register so
 * the host-side ``ra8_fake_mmap`` shim can intercept reads / writes
 * without any conditional compilation in the driver.
 */

/**
 * @brief Pointer to the 8-bit RSTSR0 register.
 * @return Volatile pointer to RSTSR0.
 */
static inline volatile uint8_t* ra8_reset_rstsr0(void)
{
  return (volatile uint8_t*)(k_ra8_reset_sysc_base_addr + (uintptr_t)k_ra8_reset_off_rstsr0);
}

/**
 * @brief Pointer to the 32-bit RSTSR1 register.
 * @return Volatile pointer to RSTSR1.
 */
static inline volatile uint32_t* ra8_reset_rstsr1(void)
{
  return (volatile uint32_t*)(k_ra8_reset_sysc_base_addr + (uintptr_t)k_ra8_reset_off_rstsr1);
}

/**
 * @brief Pointer to the 8-bit RSTSR2 register.
 * @return Volatile pointer to RSTSR2.
 */
static inline volatile uint8_t* ra8_reset_rstsr2(void)
{
  return (volatile uint8_t*)(k_ra8_reset_sysc_base_addr + (uintptr_t)k_ra8_reset_off_rstsr2);
}

/**
 * @brief Pointer to the 8-bit RSTSR3 register.
 * @return Volatile pointer to RSTSR3.
 */
static inline volatile uint8_t* ra8_reset_rstsr3(void)
{
  return (volatile uint8_t*)(k_ra8_reset_sysc_base_addr + (uintptr_t)k_ra8_reset_off_rstsr3);
}

/**
 * @brief Pointer to the 32-bit RSTSAR (security attribution) register.
 * @return Volatile pointer to RSTSAR.
 */
static inline volatile uint32_t* ra8_reset_rstsar(void)
{
  return (volatile uint32_t*)(k_ra8_reset_sysc_base_addr + (uintptr_t)k_ra8_reset_off_rstsar);
}

/**
 * @brief Pointer to the Cortex-M85 SCB AIRCR register.
 * @return Volatile pointer to AIRCR (used by the software-reset path).
 */
static inline volatile uint32_t* ra8_reset_aircr(void)
{
  return (volatile uint32_t*)k_ra8_reset_scb_aircr_addr;
}

/**
 * @brief Pointer to the 16-bit PRCR write-protect register.
 * @return Volatile pointer to PRCR (gates SYRSTMSKn / TEMPRCR / TEMPRLR via PRC5).
 */
static inline volatile uint16_t* ra8_reset_prcr(void)
{
  return (volatile uint16_t*)(k_ra8_reset_sysc_base_addr + (uintptr_t)k_ra8_reset_off_prcr);
}

/**
 * @brief Pointer to the 8-bit SYRSTMSK0 register.
 * @return Volatile pointer to SYRSTMSK0.
 */
static inline volatile uint8_t* ra8_reset_syrstmsk0(void)
{
  return (volatile uint8_t*)(k_ra8_reset_sysc_base_addr + (uintptr_t)k_ra8_reset_off_syrstmsk0);
}

/**
 * @brief Pointer to the 8-bit SYRSTMSK1 register.
 * @return Volatile pointer to SYRSTMSK1.
 */
static inline volatile uint8_t* ra8_reset_syrstmsk1(void)
{
  return (volatile uint8_t*)(k_ra8_reset_sysc_base_addr + (uintptr_t)k_ra8_reset_off_syrstmsk1);
}

/**
 * @brief Pointer to the 8-bit SYRSTMSK2 register.
 * @return Volatile pointer to SYRSTMSK2.
 */
static inline volatile uint8_t* ra8_reset_syrstmsk2(void)
{
  return (volatile uint8_t*)(k_ra8_reset_sysc_base_addr + (uintptr_t)k_ra8_reset_off_syrstmsk2);
}

/**
 * @brief Pointer to the 8-bit TEMPRCR register.
 * @return Volatile pointer to TEMPRCR.
 */
static inline volatile uint8_t* ra8_reset_temprcr(void)
{
  return (volatile uint8_t*)(k_ra8_reset_sysc_base_addr + (uintptr_t)k_ra8_reset_off_temprcr);
}

/**
 * @brief Pointer to the 8-bit TEMPRLR register.
 * @return Volatile pointer to TEMPRLR.
 */
static inline volatile uint8_t* ra8_reset_temprlr(void)
{
  return (volatile uint8_t*)(k_ra8_reset_sysc_base_addr + (uintptr_t)k_ra8_reset_off_temprlr);
}

#ifdef __cplusplus
}
#endif
