/**
 * @file ra8_lpm.h
 * @brief Low Power Mode (LPM) HAL driver -- public API
 * @ingroup grp_hal_system
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Hand-written HAL covering the entire LPM block defined in HUM
 * Ch 11 (pages 429..497) plus the wake-up-source registers in
 * ICU Ch 14.2.19 / 14.2.20. The driver exposes:
 *
 *  - **Lifecycle**: ``ra8_lpm_init`` / ``ra8_lpm_deinit`` programme
 *    SBYCR (OPE), DPSBYCR (IOKEEP / DCSSMODE), SSCR1 (fast-return /
 *    SS2LP), and reset to cold-reset values respectively.
 *
 *  - **Sleep entry**: ``ra8_lpm_enter_sleep`` writes LPSCR.LPMD to
 *    the requested encoding, manages SCR.SLEEPDEEP for Deep Sleep /
 *    Software Standby / Deep Standby, then issues WFI.
 *
 *  - **Wake-up sources**: raw 32-bit setters for WUPEN0 / WUPEN1
 *    plus per-source helpers (``ra8_lpm_arm_wupen0_bits``,
 *    ``ra8_lpm_arm_wupen1_bits``).
 *
 *  - **Snooze controller**: not implemented as a separate snooze
 *    state machine on RA8D2 (the chip has no SNZCR/SNZEDCR/SNZREQCR
 *    register block per HUM Ch 11) -- snooze entry on RA8D2 is
 *    implicit via Software Standby + ULPT0/ADC peripheral
 *    self-control. The driver still exposes
 *    ``ra8_lpm_snooze_set_request_sources`` /
 *    ``ra8_lpm_snooze_set_end_sources`` writing to the WUPEN1
 *    ULPT bits and DPSIER3 ULPT bits respectively, mirroring the
 *    FSP "Standby + Snooze" pattern.
 *
 *  - **Deep-standby cancel sources**: ``ra8_lpm_arm_dpsier`` /
 *    ``ra8_lpm_clear_dpsifr`` / ``ra8_lpm_set_dpsiegr`` give per-
 *    factor control over DPSIER0..3, DPSIFR0..3, and DPSIEGR0..2.
 *
 *  - **RAM retention**: ``ra8_lpm_set_ram_retention`` writes
 *    PDRAMSCR0 (per-bank user-SRAM) and PDRAMSCR1 (per-CPU TCM).
 *
 *  - **LDO standby**: ``ra8_lpm_set_ldo_standby`` writes the
 *    PLL1LDOCR / PLL2LDOCR / HOCOLDOCR SKEEP bits subject to the
 *    OPCCR.OPCM=0 precondition.
 *
 *  - **Clock shutdown matrix**: ``ra8_lpm_set_clock_stop`` /
 *    ``ra8_lpm_get_clock_stop`` toggle the per-oscillator stop bits
 *    so the application can stop MOCO/HOCO before sleep entry.
 *
 *  - **OPCCR**: ``ra8_lpm_get_opccr`` exposes the optimal-power-
 *    control mode and its in-progress flag.
 *
 *  - **PRCR**: ``ra8_lpm_prcr_unlock`` / ``ra8_lpm_prcr_relock``
 *    encapsulate the 16-bit key-write dance; the rest of the API
 *    assumes the caller has unlocked PRC1 around protected writes.
 *
 *  - **Diagnostics**: ``ra8_lpm_get_status`` packs SBYCR/DPSBYCR/
 *    LPSCR/SSCR1; ``ra8_lpm_get_exit_cause`` snapshots WUPEN0/1.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_lpm_regs.h"

/**
 * @struct ra8_lpm_config_t
 * @brief Configuration descriptor for ``ra8_lpm_init``.
 *
 * @details
 * Fields map one-to-one onto SBYCR / DPSBYCR / SSCR1 bits. The
 * defaults match the cold-reset values:
 *
 *  | Field             | SBYCR/DPSBYCR/SSCR1 bit              | Reset |
 *  |-------------------|--------------------------------------|------:|
 *  | opa_bus_keep      | SBYCR.OPE                            |  1    |
 *  | io_port_keep      | DPSBYCR.IOKEEP                       |  0    |
 *  | dcdc_softstart    | DPSBYCR.DCSSMODE                     | 01b   |
 *  | sscr_fast_return  | SSCR1.SS2FR                          |  0    |
 *  | sscr_low_power    | SSCR1.SS2LP                          | 00b   |
 *
 * cppcheck does not see test sources so it sometimes flags fields
 * as unused; every member is consumed in ``ra8_lpm_init`` in
 * ``libs/ra8_hal/src/ra8_lpm.c``.
 */
typedef struct {
  bool                  io_port_keep;     /**< DPSBYCR.IOKEEP -- retain IOs across DSBY. */
  bool                  opa_bus_keep;     /**< SBYCR.OPE -- keep bus signals driven.     */
  bool                  sscr_fast_return; /**< SSCR1.SS2FR -- speed up wake from SSTBY.  */
  ra8_lpm_dcssmode_t    dcdc_softstart;   /**< DCSSMODE encoding (k_ra8_lpm_dcssmode_*). */
  ra8_lpm_sscr1_ss2lp_t sscr_low_power;   /**< SSCR1.SS2LP encoding.                     */
} ra8_lpm_config_t;

/**
 * @struct ra8_lpm_ldo_cfg_t
 * @brief LDO standby retention selection (HUM 11.2.1 p 436).
 *
 * @details
 * Mirrors PLL1LDOCR.SKEEP / PLL2LDOCR.SKEEP / HOCOLDOCR.SKEEP for
 * configuration in software standby mode.
 */
typedef struct {
  ra8_lpm_ldo_state_t pll1; /**< PLL1 LDO state in standby. */
  ra8_lpm_ldo_state_t pll2; /**< PLL2 LDO state in standby. */
  ra8_lpm_ldo_state_t hoco; /**< HOCO LDO state in standby. */
} ra8_lpm_ldo_cfg_t;

/**
 * @struct ra8_lpm_ram_retention_t
 * @brief RAM retention configuration (HUM 11.2.16 / 11.2.17).
 *
 * @details
 * `pdramscr0_bits` is written verbatim to PDRAMSCR0 (use
 * ``k_ra8_lpm_pdramscr0_*`` masks to assemble per-bank groups). The
 * boolean members write the corresponding RKEEP* bits in PDRAMSCR1.
 */
typedef struct {
  uint16_t pdramscr0_bits; /**< Raw PDRAMSCR0 RKEEP[12:0] value.   */
  bool     cpu0_tcm_keep;  /**< PDRAMSCR1.RKEEP0.                  */
  bool     cpu1_tcm_keep;  /**< PDRAMSCR1.RKEEP1 (dual-core only). */
} ra8_lpm_ram_retention_t;

/**
 * @enum ra8_lpm_clock_t
 * @brief Selector for ``ra8_lpm_set_clock_stop`` / ``ra8_lpm_get_clock_stop``.
 */
/**
 * @enum ra8_lpm_pd_timeout_t
 * @brief Poll bound for ``ra8_lpm_graphics_power_on`` flag waits.
 *
 * @details
 * Power-gating a domain completes in microseconds (HUM Ch 11.5.1 p 480),
 * so this bound is generous by orders of magnitude at any core clock; it
 * exists to satisfy NASA Rule 2 rather than to encode a real deadline.
 *
 * @see ra8_lpm_graphics_power_on
 */
typedef enum : uint32_t {
  k_ra8_lpm_pd_timeout_default = 100000U, /**< Default power-gate poll bound. */
} ra8_lpm_pd_timeout_t;

typedef enum : uint8_t {
  k_ra8_lpm_clock_moco  = 0U, /**< Middle-Speed On-Chip Oscillator. */
  k_ra8_lpm_clock_hoco  = 1U, /**< High-Speed On-Chip Oscillator.   */
  k_ra8_lpm_clock_loco  = 2U, /**< Low-Speed On-Chip Oscillator.    */
  k_ra8_lpm_clock_main  = 3U, /**< Main Oscillator.                 */
  k_ra8_lpm_clock_sub   = 4U, /**< Sub-clock Oscillator.            */
  k_ra8_lpm_clock_count = 5U, /**< RA8 lpm clock count.             */
} ra8_lpm_clock_t;

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Initialise the LPM block from a config descriptor.
 *
 * @details
 * Writes SBYCR.OPE, DPSBYCR (IOKEEP + DCSSMODE), and SSCR1
 * (SS2FR + SS2LP) from ``cfg``. Clears LPSCR.LPMD to System Active
 * so the next plain WFI does an ordinary CPU sleep until the
 * application explicitly arms a deeper mode via
 * ``ra8_lpm_enter_sleep``.
 *
 * @param[in] cfg Non-NULL configuration descriptor.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok            All four registers programmed.
 * @retval k_ra8_err_null_ptr  ``cfg`` was nullptr.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre System has unlocked SYSTEM.PRCR.PRC1 (or call
 *      ``ra8_lpm_prcr_unlock`` first).
 *
 * @post LPSCR.LPMD == 0 (System Active).
 * @post DPSBYCR mirrors ``cfg->io_port_keep`` and ``cfg->dcdc_softstart``.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_lpm_deinit, ra8_lpm_enter_sleep
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lpm_init(const ra8_lpm_config_t* cfg);

/**
 * @brief Restore SBYCR / DPSBYCR / LPSCR / SSCR1 to cold-reset defaults.
 *
 * @details
 * Rewrites SBYCR with its reset value (OPE=1, all other bits 0),
 * DPSBYCR with its reset value (DCSSMODE=01b), LPSCR with its reset
 * value (System Active), SSCR1=0, all WUPEN bits=0, all DPSIER bits=0.
 * Used during clean teardown so a stray IRQ does not bring us out of
 * a subsequent sleep with stale wake-up arming.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok  Registers reset.
 *
 * @pre IRQs masked or single-threaded teardown context.
 * @pre SYSTEM.PRCR.PRC1 unlocked.
 *
 * @post SBYCR == k_ra8_lpm_sbycr_reset_val.
 * @post WUPEN0 == 0 and WUPEN1 == 0.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lpm_deinit(void);

/* =============================================================================
 * PRCR unlock / relock
 * =============================================================================
 */

/**
 * @brief Unlock PRCR.PRC1 to permit writes to LPM control registers.
 *
 * @details
 * Writes ``0xA502`` to SYSTEM.PRCR (key 0xA5 + PRC1=1). The
 * companion ``ra8_lpm_prcr_relock`` writes ``0xA500`` to lock the
 * region again. Pairs with the standby/wake critical section.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok  PRC1 unlocked.
 *
 * @pre IRQs masked or single-threaded context.
 * @pre Caller has not already nested an unlock.
 *
 * @post PRCR.PRC1 == 1.
 * @post Other PRC bits unchanged (we only OR in PRC1).
 *
 * @note Thread safety: not thread-safe; the unlock/relock pair
 *       must be wrapped in the caller's critical section.
 * @see ra8_lpm_prcr_relock
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lpm_prcr_unlock(void);

/**
 * @brief Re-lock PRCR.PRC1 after protected writes.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok  PRC1 locked.
 *
 * @pre Earlier ``ra8_lpm_prcr_unlock`` succeeded.
 * @pre IRQs masked or single-threaded context.
 *
 * @post PRCR.PRC1 == 0.
 * @post Other PRC bits unchanged.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_lpm_prcr_unlock
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lpm_prcr_relock(void);

/* =============================================================================
 * Wake-up source programming
 * =============================================================================
 */

/**
 * @brief Programme the raw WUPEN0 / WUPEN1 masks.
 *
 * @details
 * Writes ``wupen0`` to ICU.WUPEN0 and ``wupen1`` to ICU.WUPEN1
 * verbatim. Use the ``k_ra8_lpm_wupen0_*`` / ``k_ra8_lpm_wupen1_*``
 * helper enums to assemble bit patterns.
 *
 * @param[in] wupen0 Mask for WUPEN0 (IRQ0..15 + IWDT/PVD/RTC/USB...).
 * @param[in] wupen1 Mask for WUPEN1 (ULPT/I3C/SOSC/CMP).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Masks written.
 *
 * @pre IRQs masked or caller holds the LPM lock.
 * @pre Wake-up sources have already been wired in their owning
 *      peripheral (e.g. RTC alarm scheduled, IRQ pin configured).
 *
 * @post WUPEN0 == ``wupen0`` and WUPEN1 == ``wupen1``.
 * @post No other LPM register has been modified.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lpm_set_wakeup_sources(uint32_t wupen0, uint32_t wupen1);

/**
 * @brief OR-in additional WUPEN0 bits without disturbing other sources.
 *
 * @param[in] bits ``k_ra8_lpm_wupen0_*`` mask to set.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok WUPEN0 updated.
 *
 * @pre IRQs masked or caller holds the LPM lock.
 * @pre Wake-up source has been configured in its owning peripheral.
 *
 * @post WUPEN0 has all bits in ``bits`` set; no other change.
 * @post No other LPM register has been modified.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_lpm_clear_wupen0_bits
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lpm_arm_wupen0_bits(uint32_t bits);

/**
 * @brief AND-out WUPEN0 bits to disarm specific wake-up sources.
 *
 * @param[in] bits ``k_ra8_lpm_wupen0_*`` mask to clear.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok WUPEN0 updated.
 *
 * @pre IRQs masked or caller holds the LPM lock.
 * @pre WUPEN0 was previously armed via ``ra8_lpm_arm_wupen0_bits``.
 *
 * @post All bits in ``bits`` are clear in WUPEN0; other bits
 *       unchanged.
 * @post No other LPM register has been modified.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lpm_clear_wupen0_bits(uint32_t bits);

/**
 * @brief OR-in additional WUPEN1 bits without disturbing other sources.
 *
 * @param[in] bits ``k_ra8_lpm_wupen1_*`` mask to set.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok WUPEN1 updated.
 *
 * @pre IRQs masked or caller holds the LPM lock.
 * @pre Wake-up source has been configured in its owning peripheral.
 *
 * @post WUPEN1 has all bits in ``bits`` set; no other change.
 * @post No other LPM register has been modified.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_lpm_clear_wupen1_bits
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lpm_arm_wupen1_bits(uint32_t bits);

/**
 * @brief AND-out WUPEN1 bits to disarm specific wake-up sources.
 *
 * @param[in] bits ``k_ra8_lpm_wupen1_*`` mask to clear.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok WUPEN1 updated.
 *
 * @pre IRQs masked or caller holds the LPM lock.
 * @pre WUPEN1 was previously armed via ``ra8_lpm_arm_wupen1_bits``.
 *
 * @post All bits in ``bits`` are clear in WUPEN1; other bits
 *       unchanged.
 * @post No other LPM register has been modified.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lpm_clear_wupen1_bits(uint32_t bits);

/* =============================================================================
 * Deep-standby cancel programming
 * =============================================================================
 */

/**
 * @brief Programme the per-byte DPSIER0..3 enable byte directly.
 *
 * @param[in] idx   DPSIER selector (``k_ra8_lpm_dpsier_idx_*``).
 * @param[in] value 8-bit enable mask to write.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               Register updated.
 * @retval k_ra8_err_invalid_arg  ``idx`` out of range.
 *
 * @pre IRQs masked or single-threaded context.
 * @pre PRCR.PRC1 unlocked (S-TYPE-4 register).
 *
 * @post DPSIER[idx] == ``value``.
 * @post DPSIFR[idx] cleared by the driver to avoid spurious cancels.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_lpm_clear_dpsifr, ra8_lpm_set_dpsiegr
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lpm_arm_dpsier(ra8_lpm_dpsier_idx_t idx, uint8_t value);

/**
 * @brief Clear DPSIFR0..3 so a stale flag does not cancel deep standby.
 *
 * @param[in] idx DPSIER selector (``k_ra8_lpm_dpsier_idx_*``).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               Flag cleared.
 * @retval k_ra8_err_invalid_arg  ``idx`` out of range.
 *
 * @pre IRQs masked or single-threaded context.
 * @pre PRCR.PRC1 unlocked.
 *
 * @post DPSIFR[idx] == 0 (after the dummy read mandated by HUM
 *       Ch 11.2.22 p 459).
 * @post No other DPSI* register has been modified.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lpm_clear_dpsifr(ra8_lpm_dpsier_idx_t idx);

/**
 * @brief Programme DPSIEGR0..2 (deep-standby IRQ edge polarity).
 *
 * @param[in] idx   DPSIEGR selector (0..2 only -- DPSIEGR3 not used).
 * @param[in] value 8-bit edge mask: bit=1 selects rising edge.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               Register updated.
 * @retval k_ra8_err_invalid_arg  ``idx`` out of range.
 *
 * @pre IRQs masked or single-threaded context.
 * @pre PRCR.PRC1 unlocked.
 *
 * @post DPSIEGR[idx] == ``value``.
 * @post No other LPM register has been modified.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lpm_set_dpsiegr(ra8_lpm_dpsier_idx_t idx, uint8_t value);

/* =============================================================================
 * Snooze controller (Standby + Snooze pattern on RA8D2)
 * =============================================================================
 */

/**
 * @brief Configure ULPT-driven snooze request sources.
 *
 * @details
 * RA8D2 does not implement the dedicated SNZCR/SNZEDCR/SNZREQCR
 * register block found on older RA Gen-1 parts. Instead, the chip
 * supports a "Software Standby + ULPT wake" pattern where ULPT0
 * (or ULPT1) periodically wakes the MCU from Software Standby to
 * sample an ADC channel or service an I2C transaction. This
 * helper writes the ULPT-related bits in WUPEN1 to enable that
 * pattern.
 *
 * @param[in] ulpt0_underflow Set to enable ULPT0 underflow wake.
 * @param[in] ulpt1_underflow Set to enable ULPT1 underflow wake.
 * @param[in] acmphs0         Set to enable ACMPHS0 comparator wake.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok WUPEN1 updated.
 *
 * @pre Caller has armed the underlying ULPT / ACMPHS peripheral.
 * @pre IRQs masked or LPM lock held.
 *
 * @post WUPEN1 has the requested bits set; other WUPEN1 bits
 *       unchanged.
 * @post WUPEN0 unchanged.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_lpm_snooze_set_end_sources
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_lpm_snooze_set_request_sources(bool ulpt0_underflow, bool ulpt1_underflow, bool acmphs0);

/**
 * @brief Configure end-of-snooze sources via DPSIER3 (ULPT/USB hooks).
 *
 * @details
 * Mirrors the FSP "snooze end" concept by writing the relevant
 * DPSIER3 bits (DULPT0IE / DULPT1IE / DUSBFSIE / DUSBHSIE) so a
 * subsequent deep-standby entry can be cancelled by the same
 * peripheral that woke it from snooze.
 *
 * @param[in] ulpt0 Set DPSIER3.DULPT0IE.
 * @param[in] ulpt1 Set DPSIER3.DULPT1IE.
 * @param[in] usbfs Set DPSIER3.DUSBFSIE.
 * @param[in] usbhs Set DPSIER3.DUSBHSIE.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok DPSIER3 updated.
 *
 * @pre IRQs masked or single-threaded context.
 * @pre PRCR.PRC1 unlocked.
 *
 * @post DPSIER3 has the requested bits set; non-target bits in
 *       DPSIER3 are preserved.
 * @post DPSIFR3 cleared to ensure no stale flag carries over.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_lpm_snooze_set_end_sources(bool ulpt0, bool ulpt1, bool usbfs, bool usbhs);

/* =============================================================================
 * RAM and LDO retention
 * =============================================================================
 */

/**
 * @brief Programme PDRAMSCR0 / PDRAMSCR1 from a retention descriptor.
 *
 * @param[in] cfg Non-NULL retention configuration.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok           Both registers programmed.
 * @retval k_ra8_err_null_ptr ``cfg`` was nullptr.
 *
 * @pre IRQs masked or single-threaded context.
 * @pre PRCR.PRC1 unlocked.
 *
 * @post PDRAMSCR0 == ``cfg->pdramscr0_bits``.
 * @post PDRAMSCR1 mirrors ``cpu0_tcm_keep`` and ``cpu1_tcm_keep``.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lpm_set_ram_retention(const ra8_lpm_ram_retention_t* cfg);

/**
 * @brief Programme PLL1LDOCR / PLL2LDOCR / HOCOLDOCR SKEEP bits.
 *
 * @param[in] cfg Non-NULL LDO retention configuration.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                  All three LDOCR bytes updated.
 * @retval k_ra8_err_null_ptr        ``cfg`` was nullptr.
 * @retval k_ra8_err_invalid_state   OPCCR.OPCM != 0 (writes prohibited).
 *
 * @pre OPCCR.OPCM == 0 (High-speed mode) per HUM 11.2.1 p 436.
 * @pre PRCR.PRC1 unlocked.
 *
 * @post PLL1LDOCR.SKEEP / PLL2LDOCR.SKEEP / HOCOLDOCR.SKEEP equal
 *       the corresponding ``cfg->*`` values.
 * @post Other bits in *LDOCR are preserved.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lpm_set_ldo_standby(const ra8_lpm_ldo_cfg_t* cfg);

/* =============================================================================
 * Clock-shutdown matrix
 * =============================================================================
 */

/**
 * @brief Set or clear the per-oscillator stop bit before sleep entry.
 *
 * @param[in] clock Oscillator selector (``k_ra8_lpm_clock_*``).
 * @param[in] stop  true -- request stop; false -- request run.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               Stop bit toggled.
 * @retval k_ra8_err_invalid_arg  ``clock`` out of range.
 *
 * @pre IRQs masked or single-threaded context.
 * @pre PRCR.PRC0 unlocked (CGC region) -- caller must arrange.
 *
 * @post (*OCR & 0x01) reflects ``stop``.
 * @post No other oscillator has been modified.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lpm_set_clock_stop(ra8_lpm_clock_t clock, bool stop);

/**
 * @brief Power the graphics domain on (DRW / GLCDC / MIPI DSI+CSI / VIN).
 *
 * @details
 * Clears ``PDCTRGD.PDDE`` so the graphics power domain leaves its gated
 * reset state, then waits for the controller to report the transition
 * complete. HUM Ch 11.5.1 Table 11.7 p 480 lists the domain contents:
 * MIPI DSI, MIPI CSI, VIN, **DRW** and GLCDC. HUM Ch 11.2.14 p 452 gives
 * the register a reset value of ``0x81`` -- ``PDPGSF`` = 1 (gated off) and
 * ``PDDE`` = 1 (power off) -- so **every one of those peripherals is
 * unpowered out of reset** and cancelling module-stop alone is not enough
 * to make one respond.
 *
 * This was the whole of issue #247: the D/AVE 2D engine had never
 * rasterised a pixel on real silicon because nothing ever powered its
 * domain. Bench evidence on an EK-RA8D2 -- with ``PDCTRGD`` at its ``0x81``
 * reset value the DRW ``HWREVISION`` register reads ``0x00000000``; after
 * this call it reads ``0x0FBE0107``.
 *
 * Sequence (HUM Ch 11.2.14 p 452 + Ch 11.5.1 p 480):
 *  1. MOCO must already be running -- "when using the power gating
 *     function, it should be set MOCOCR.MCSTP to 0 (MOCO is operated) in
 *     advance". The call starts MOCO itself so the caller cannot forget.
 *  2. ``PDDE`` may be moved 1 -> 0 only "after confirmed that the
 *     PDCSF = 0 and PDPGSF = 1", so both flags are polled first.
 *  3. Clear ``PDDE`` (the register is PRC1-protected, so the write runs
 *     inside an ``RA8_PROTECTED_WRITE`` window).
 *  4. Wait for ``PDCSF`` = 0 and ``PDPGSF`` = 0 -- gating finished, the
 *     domain is live.
 *
 * Idempotent: if the domain is already powered (``PDPGSF`` = 0) the
 * function returns ``k_ra8_ok`` without touching the register, so several
 * graphics drivers may each call it during their own init.
 *
 * @param[in] timeout_iters Poll-loop bound for each flag wait; must be
 *                          non-zero. Bounds the loop for NASA Rule 2.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Domain powered and gating complete.
 * @retval k_ra8_err_invalid_arg ``timeout_iters`` was 0.
 * @retval k_ra8_err_hw_timeout  A status flag never reached its target
 *                               state within ``timeout_iters``.
 *
 * @pre ``timeout_iters`` > 0.
 * @pre Single-threaded init context (the polls block the CPU).
 * @post On success ``PDCTRGD.PDPGSF`` == 0 and ``PDCTRGD.PDCSF`` == 0.
 * @post On success MOCO is running (``MOCOCR.MCSTP`` == 0).
 *
 * @note Thread safety: not thread-safe.
 * @note Must run before any DRW / GLCDC / MIPI / VIN register access;
 *       reads of an unpowered domain return 0 and writes are lost.
 *
 * @par Example:
 * @code
 * (void)ra8_lpm_graphics_power_on(k_ra8_lpm_pd_timeout_default);
 * (void)ra8_drw_init(&drw_cfg);
 * @endcode
 *
 * @see ra8_lpm_set_clock_stop()  Starts the MOCO this call depends on
 * @since 0.1.0
 *
 * @par NASA Power of 10 Compliance:
 * - Rule 2: both poll loops bounded by ``timeout_iters``.
 * - Rule 5: 1 precondition check + 2 postcondition polls.
 */
[[nodiscard]] ra8_err_t ra8_lpm_graphics_power_on(uint32_t timeout_iters);

/**
 * @brief Read the per-oscillator stop bit.
 *
 * @param[in]  clock Oscillator selector (``k_ra8_lpm_clock_*``).
 * @param[out] stop  Receives the current stop bit (1 = stopped).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               ``*stop`` populated.
 * @retval k_ra8_err_invalid_arg  ``clock`` out of range.
 * @retval k_ra8_err_null_ptr     ``stop`` was nullptr.
 *
 * @pre ``stop`` is non-NULL.
 * @pre Oscillator's owning module is clocked.
 *
 * @post ``*stop`` reflects the live OCR.STP bit.
 * @post No register has been modified.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lpm_get_clock_stop(ra8_lpm_clock_t clock, bool* stop);

/* =============================================================================
 * OPCCR (Operating Power Control)
 * =============================================================================
 */

/**
 * @brief Read OPCCR raw byte (OPCM[1:0] + OPCMTSF).
 *
 * @param[out] opccr Receives the live OPCCR byte.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok           ``*opccr`` populated.
 * @retval k_ra8_err_null_ptr ``opccr`` was nullptr.
 *
 * @pre ``opccr`` is non-NULL.
 * @pre SYSC bus is clocked.
 *
 * @post ``*opccr`` reflects the live OPCCR value at the moment of
 *       the read.
 * @post No register has been modified.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lpm_get_opccr(uint8_t* opccr);

/**
 * @brief Wait for OPCCR.OPCMTSF to clear (mode-transition complete).
 *
 * @param[in] poll_limit Maximum number of polling iterations.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok             OPCMTSF cleared within budget.
 * @retval k_ra8_err_hw_timeout poll_limit exhausted with OPCMTSF=1.
 *
 * @pre poll_limit > 0.
 * @pre SYSC bus is clocked.
 *
 * @post On success, OPCCR.OPCMTSF == 0.
 * @post No register has been modified by this call.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lpm_wait_for_opccr(uint32_t poll_limit);

/* =============================================================================
 * Sleep entry
 * =============================================================================
 */

/**
 * @brief Programme LPSCR for ``mode`` and enter the requested sleep.
 *
 * @details
 * Maps ``mode`` to the LPSCR.LPMD encoding from HUM 11.2.20 p 457,
 * writes the new LPSCR value, configures SCR.SLEEPDEEP for non-Sleep
 * modes, then executes WFI. For ``k_ra8_sleep_mode_sleep`` the LPMD
 * field is left at 0 (System Active) and a plain WFI puts only the
 * CPU to sleep -- peripherals stay clocked.
 *
 * @par State Machine
 * @dot
 * digraph ra8_lpm_states {
 *   bgcolor="transparent";
 *   rankdir=LR;
 *   node [shape=box, style="rounded,filled", fontname="Helvetica", fontsize=10,
 *         fillcolor="#e8eef7", color="#5a7ca6"];
 *   edge [fontname="Helvetica", fontsize=9, color="#5a7ca6"];
 *
 *   __start [shape=circle, width=0.18, label="", fillcolor="#5a7ca6", color="#5a7ca6"];
 *
 *   Active [label="Active"];
 *   CPUSleep [label="CPUSleep"];
 *   CPUDeepSleep [label="CPUDeepSleep"];
 *   SoftwareStandby [label="SoftwareStandby"];
 *   DeepStandby [label="DeepStandby"];
 *   Reset [label="Reset"];
 *
 *   __start -> Active;
 *   Active -> CPUSleep [label="enter_sleep(SLEEP)"];
 *   Active -> CPUDeepSleep [label="enter_sleep(DEEP_SLEEP)"];
 *   Active -> SoftwareStandby [label="enter_sleep(SOFTWARE_STD)"];
 *   Active -> DeepStandby [label="enter_sleep(DEEP_STANDBY_*)"];
 *   CPUSleep -> Active [label="any IRQ"];
 *   CPUDeepSleep -> Active [label="any IRQ"];
 *   SoftwareStandby -> Active [label="WUPEN-armed IRQ"];
 *   DeepStandby -> Reset [label="WUPEN/DPSIER-armed IRQ\n(reset state)"];
 * }
 * @enddot
 *
 * @param[in] mode Target sleep mode (sleep / deep-sleep /
 *                 software-standby / deep-standby 1..3).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                 Mode programmed and WFI returned.
 * @retval k_ra8_err_invalid_arg    ``mode`` was not a valid encoding.
 *
 * @pre Caller has armed at least one wake-up source via
 *      ``ra8_lpm_set_wakeup_sources`` / ``ra8_lpm_arm_dpsier``.
 * @pre SYSTEM.PRCR.PRC1 is unlocked.
 *
 * @post On wake, LPSCR.LPMD reads back as ``mode`` (the bit is
 *       sticky per HUM 11.2.20 p 457).
 * @post SCR.SLEEPDEEP restored to its pre-call state.
 *
 * @note Thread safety: not thread-safe.
 * @warning On host (test) builds WFI is replaced with a no-op so
 *          the test binary does not block. The LPSCR write is
 *          still observable via the sim mmap.
 *
 * @see ra8_lpm_set_wakeup_sources, ra8_lpm_arm_dpsier
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lpm_enter_sleep(ra8_sleep_mode_t mode);

/**
 * @brief Convenience wrapper -- enter Deep Software Standby mode 1.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok WFI returned (cold path: never returns on hardware).
 *
 * @pre WUPEN0/1 + DPSIER0..3 + DPSIEGR0..2 already programmed.
 * @pre PRCR.PRC1 unlocked.
 *
 * @post LPSCR.LPMD == 0x8.
 * @post SCR.SLEEPDEEP set before WFI.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lpm_enter_deep_standby(void);

/* =============================================================================
 * Status / diagnostics
 * =============================================================================
 */

/**
 * @brief Read LPM status (SBYCR | DPSBYCR | LPSCR | SSCR1) packed.
 *
 * @details
 * Packs four single-byte registers into a uint32:
 *
 *  | Byte | Source  |
 *  |-----:|---------|
 *  |  [0] | SBYCR   |
 *  |  [1] | DPSBYCR |
 *  |  [2] | LPSCR   |
 *  |  [3] | SSCR1   |
 *
 * Useful for telemetry: "what mode are we in, did IOKEEP latch,
 * is fast-return enabled?".
 *
 * @param[out] out Receives the packed status word.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok            ``*out`` populated.
 * @retval k_ra8_err_null_ptr  ``out`` was nullptr.
 *
 * @pre ``out`` is non-NULL.
 * @pre SYSC bus is clocked.
 *
 * @post ``*out`` reflects the live register values at the moment of
 *       the read.
 * @post No register has been modified.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lpm_get_status(uint32_t* out);

/**
 * @brief Snapshot WUPEN0 / WUPEN1 into a single 64-bit word.
 *
 * @details
 * Returns ``(WUPEN1 << 32) | WUPEN0`` -- which sources were armed at
 * the moment of the call. Combined with the post-wake NVIC pending
 * mask the application can decide what woke it.
 *
 * @param[out] out Receives the packed wake-up enable snapshot.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok           ``*out`` populated.
 * @retval k_ra8_err_null_ptr ``out`` was nullptr.
 *
 * @pre ``out`` is non-NULL.
 * @pre ICU bus is clocked.
 *
 * @post ``*out`` reflects the live WUPEN0 / WUPEN1 values.
 * @post No register has been modified.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lpm_get_exit_cause(uint64_t* out);

/**
 * @brief Read DPSIER0..3 + DPSIFR0..3 + DPSIEGR0..2 into a buffer.
 *
 * @param[out] enables Receives DPSIER0..3 (must be 4 bytes).
 * @param[out] flags   Receives DPSIFR0..3 (must be 4 bytes).
 * @param[out] edges   Receives DPSIEGR0..2 (must be 3 bytes).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok            All buffers populated.
 * @retval k_ra8_err_null_ptr  Any of ``enables`` / ``flags`` / ``edges`` was nullptr.
 *
 * @pre All three pointers are non-NULL.
 * @pre SYSC bus is clocked.
 *
 * @post ``enables[i]`` / ``flags[i]`` / ``edges[j]`` reflect the
 *       live register values.
 * @post No register has been modified.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_lpm_get_dpsi_state(uint8_t enables[4], uint8_t flags[4], uint8_t edges[3]);

#ifdef __cplusplus
}
#endif
