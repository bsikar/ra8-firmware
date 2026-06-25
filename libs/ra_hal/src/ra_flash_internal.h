/**
 * @file ra_flash_internal.h
 * @brief Test-access surface for ra_flash internal helpers (MC/DC).
 *
 * @details
 * Not part of the public API. Tests under tests/ MAY include this
 * header to drive compound boolean decisions that sit in TU-private
 * helpers behind the public ra_flash facade. See CLAUDE.md
 * "Test access to internal symbols (MC/DC scope)".
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra_err.h"
#include "ra_flash.h"

/* =============================================================================
 * Cross-TU shared state + constants (split across ra_flash*.c)
 *
 * The ra_flash driver is split into several translation units that all
 * compile into libra_hal. The runtime state struct, its single
 * definition, the module log tag, and the constant blocks referenced by
 * more than one of those TUs live here so every TU sees one declaration.
 * =============================================================================
 */

/**
 * @struct ra_flash_runtime_t
 * @brief File-scope runtime state shared across the ra_flash TUs.
 *
 * @details
 * Cleared at init. Used by the IRQ dispatcher to find the registered
 * callback and to detect callers that try to use APIs before init. The
 * single definition lives in ``ra_flash.c``; the configuration and IRQ
 * TUs reference it via the @ref s_flash_rt extern below.
 *
 * @invariant ``win_low < win_high`` whenever a soft window is installed,
 *            else both are 0 (window disabled).
 *
 * @see s_flash_rt
 * @since 0.1.0
 */
typedef struct {
  ra_flash_callback_t cb;          /**< Registered IRQ callback (or NULL). */
  void*               user_ctx;    /**< Caller pointer passed to ``cb``.    */
  bool                initialized; /**< True after ``ra_flash_init``.       */
  bool                prefetch_on; /**< Last-known MRCPFB state.            */
  uintptr_t           win_low;     /**< Soft access-window low (incl).      */
  uintptr_t           win_high;    /**< Soft access-window high (excl).     */
} ra_flash_runtime_t;

/**
 * @var s_flash_rt
 * @brief Single shared ra_flash runtime-state instance.
 *
 * @details Defined exactly once in ``ra_flash.c``; the configuration and
 *          IRQ TUs reference this extern. Module-unique name keeps the
 *          symbol link-unique within libra_hal.
 *
 * @note Not thread-safe; mutated only from single-threaded init / ISR.
 * @warning Do not redefine; this is the sole owner of the state.
 * @since 0.1.0
 */
extern ra_flash_runtime_t s_flash_rt;

/**
 * @var s_flash_tag
 * @brief Shared log tag string for the ra_flash module.
 *
 * @details Defined once in ``ra_flash.c``. Referenced by every ra_flash
 *          TU's logging / validation macros.
 *
 * @note Read-only after init.
 * @since 0.1.0
 */
extern const char* s_flash_tag;

/**
 * @enum ra_flash_const_t
 * @brief Bounds on configuration values + spin limits (cross-TU).
 *
 * @details
 * HUM Ch 59.5.2 p 3551 limits MRCMHZ to 0x0FA (250 MHz). HUM
 * Ch 59.5.3 p 3552 limits MREMHZ to 0x07D (125 MHz). The MACI
 * commands take tens of microseconds to milliseconds; the spin
 * limit below is generous enough for the worst-case
 * configuration-set (~9 ms) at the slowest clock. Shared by the
 * lifecycle, configuration, and IRQ TUs.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra_flash_max_mrcfreq_mhz = 0x000000FAUL, /**< MRCMHZ <= 250.            */
  k_ra_flash_max_mrefreq_mhz = 0x0000007DUL, /**< MREMHZ <= 125.            */
  k_ra_flash_busy_spin_limit = 0x00010000UL, /**< Direct-write busy spin.   */
  k_ra_flash_maci_spin_limit = 0x00100000UL, /**< MACI command spin limit.  */
  k_ra_flash_pe_spin_limit   = 0x00010000UL, /**< P/E entry spin limit.     */
  k_ra_flash_zeroize_spin    = 0x00400000UL, /**< W-HUK zeroize spin.       */
  k_ra_flash_max_list_select = 0x0000000FUL, /**< MCTRLSR.LIST max value.   */
} ra_flash_const_t;

/**
 * @enum ra_flash_key_t
 * @brief KEY-byte shifts and codes that gate writes (cross-TU).
 *
 * @details
 * MRCFREQ requires KEY=0x1E in [31:24] (HUM Ch 59.5.2 p 3551 Note 1).
 * MREFREQ requires KEY=0xE1 in [31:24] (HUM Ch 59.5.3 p 3552 Note 1).
 * Shared by the lifecycle and configuration TUs.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra_flash_freq_key_shift = 24U, /**< KEY[7:0] @ [31:24] in MRCFREQ/MREFREQ. */
  k_ra_flash_mrcfreq_key    = 0x1EU,
  k_ra_flash_mrefreq_key    = 0xE1U,
} ra_flash_key_t;

/**
 * @brief Set MRCPFB.MPFBEN to enable/disable the prefetch buffer.
 *
 * @details Promoted from TU-private static linkage so the configuration
 *          TU (``ra_flash_config.c``) can drive prefetch around the
 *          clock-frequency-update sequence. Defined in ``ra_flash.c``.
 *
 * @param[in] enable ``true`` => prefetch on.
 *
 * @pre Module clock ungated.
 * @post MRCPFB.MPFBEN matches @p enable; ``s_flash_rt.prefetch_on`` updated.
 *
 * @note Internal helper, not thread-safe.
 * @since 0.1.0
 */
void ra_flash_internal_set_prefetch(bool enable);

/**
 * @brief Send a single byte through the MACI command-issuing area.
 *
 * @details Promoted from TU-private static linkage so the configuration
 *          TU can emit MACI command bytes. Defined in ``ra_flash.c``.
 *
 * @param[in] byte Command byte.
 *
 * @pre Controller is in P/E mode.
 * @post One byte was written to MACI_CMD8.
 *
 * @note Internal helper, not thread-safe.
 * @since 0.1.0
 */
void ra_flash_internal_maci_cmd8(uint8_t byte);

/**
 * @brief Send a halfword through the MACI command-issuing area.
 *
 * @details Promoted from TU-private static linkage so the configuration
 *          TU can emit MACI command halfwords. Defined in ``ra_flash.c``.
 *
 * @param[in] half 16-bit data.
 *
 * @pre Controller is in P/E mode.
 * @post One halfword was written to MACI_CMD16.
 *
 * @note Internal helper, not thread-safe.
 * @since 0.1.0
 */
void ra_flash_internal_maci_cmd16(uint16_t half);

/**
 * @brief Spin until MSTATR.MRDY rises or limit elapses.
 *
 * @details Promoted from TU-private static linkage so the configuration
 *          TU can wait for MACI completion. Defined in ``ra_flash.c``.
 *
 * @param[in] limit Maximum spin iterations.
 * @return ``k_ra_ok`` if MRDY observed, else ``k_ra_err_hw_timeout``.
 * @retval k_ra_ok            MRDY observed high within @p limit.
 * @retval k_ra_err_hw_timeout Limit exhausted without MRDY.
 *
 * @pre @p limit > 0.
 * @pre Controller is in P/E mode (MRDY only meaningful then).
 * @post MRDY observed high or function returns timeout.
 *
 * @note Internal helper, not thread-safe.
 * @since 0.1.0
 */
ra_err_t ra_flash_internal_wait_mrdy(uint32_t limit);

/**
 * @brief Test whether [addr, addr+len) lies inside the soft window.
 *
 * @details Promoted from TU-private static linkage so the FSP-parity /
 *          IRQ TU (``ra_flash_irq.c``) can run the same range-validation
 *          check used by the direct-programming path. Reads
 *          ``s_flash_rt`` and forwards to
 *          @ref ra_flash_internal_window_allows_pure. Defined in
 *          ``ra_flash.c``.
 *
 * @param[in] addr Start address of the candidate operation.
 * @param[in] len  Length in bytes.
 *
 * @return ``true`` if the operation is permitted, ``false`` if blocked.
 * @retval true  Region permitted (or no window installed).
 * @retval false Region overlaps outside the installed window.
 *
 * @pre None.
 * @post No side effects.
 *
 * @note Internal helper, not thread-safe.
 * @since 0.1.0
 */
bool ra_flash_internal_window_allows(uintptr_t addr, uint32_t len);

/**
 * @brief Pure (state-free) reimplementation of @c internal_window_allows.
 *
 * @details Returns true iff @p addr / @p len falls within the
 *          inclusive lower bound @p win_low and exclusive upper bound
 *          @p win_high. The "no window installed" sentinel is
 *          @c (win_low == 0 && win_high == 0).
 *
 *          Promoted as a pure helper so tests can drive the line-722
 *          ``win_low == 0 && win_high == 0`` AND-decision under
 *          -fcoverage-mcdc on the production source. The
 *          state-reading wrapper @c internal_window_allows simply
 *          forwards to this function.
 *
 * @param[in] addr     Start address of the candidate region.
 * @param[in] len      Length in bytes of the candidate region.
 * @param[in] win_low  Inclusive lower bound of the allow window.
 * @param[in] win_high Exclusive upper bound of the allow window.
 *
 * @return Boolean window predicate.
 * @retval true  Region is permitted (or no window installed).
 * @retval false Region overlaps outside the installed window.
 *
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return value depends solely on the four inputs.
 *
 * @note Test-access only. Pure function.
 *
 * @par MC/DC:
 * Drives line 722 ``s_rt.win_low == 0U && s_rt.win_high == 0U``
 * (2 conditions, AND; N+1 = 3 vectors).
 *
 * @since 0.1.0
 */
bool ra_flash_internal_window_allows_pure(uintptr_t addr,
                                          uint32_t  len,
                                          uintptr_t win_low,
                                          uintptr_t win_high);

#include "ra_err.h"

/**
 * @brief Direct-call test access to @c internal_wait_buffer_ready.
 *
 * @details Promoted from TU-private static linkage so tests can drive
 *          the line-150 ``(s & PRGBSYC == 0) && (s & ABUFFULL == 0)``
 *          AND-decision under @c -fcoverage-mcdc on the production
 *          source. Tests poke the simulator-backed MRCPS register to
 *          present each pair of bit values across calls.
 *
 * @param[in] limit Maximum spin iterations.
 * @return @c k_ra_ok if both bits cleared within @p limit iterations,
 *         else @c k_ra_err_hw_timeout.
 * @retval k_ra_ok            Both bits observed clear within @p limit.
 * @retval k_ra_err_hw_timeout Limit exhausted without both bits clear.
 *
 * @pre Simulator mmap window for MRCPS is mapped (host-test only).
 * @pre Caller has staged the desired MRCPS bit pattern.
 * @post No persistent state mutated outside of MRCPS read.
 * @post Return value reflects the observed register sequence.
 *
 * @note Test-access only. Not part of the public ra_flash API.
 *
 * @par MC/DC:
 * Drives line 150 ``(s & PRGBSYC) == 0 && (s & ABUFFULL) == 0``
 * (2 conditions, AND; N+1 = 3 vectors).
 *
 * @since 0.1.0
 */
ra_err_t ra_flash_internal_wait_buffer_ready_call(uint32_t limit);

/**
 * @brief Direct-call test access to @c internal_wait_commit_done.
 *
 * @details Promoted from TU-private static linkage so tests can drive
 *          the line-181 ``(s & ABUFEMP) != 0 && (s & PRGBSYC) == 0``
 *          AND-decision under @c -fcoverage-mcdc on the production
 *          source. Tests poke the simulator-backed MRCPS register to
 *          present each pair of bit values across calls.
 *
 * @param[in] limit Maximum spin iterations.
 * @return @c k_ra_ok if commit observed within @p limit iterations,
 *         else @c k_ra_err_hw_timeout.
 * @retval k_ra_ok            Commit observed within @p limit iterations.
 * @retval k_ra_err_hw_timeout Limit exhausted without commit observed.
 *
 * @pre Simulator mmap window for MRCPS is mapped (host-test only).
 * @pre Caller has staged the desired MRCPS bit pattern.
 * @post No persistent state mutated outside of MRCPS read.
 * @post Return value reflects the observed register sequence.
 *
 * @note Test-access only. Not part of the public ra_flash API.
 *
 * @par MC/DC:
 * Drives line 181 ``(s & ABUFEMP) != 0 && (s & PRGBSYC) == 0``
 * (2 conditions, AND; N+1 = 3 vectors).
 *
 * @since 0.1.0
 */
ra_err_t ra_flash_internal_wait_commit_done_call(uint32_t limit);

#ifdef __cplusplus
}
#endif
