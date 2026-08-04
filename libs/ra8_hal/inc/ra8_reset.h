/**
 * @file ra8_reset.h
 * @brief Reset cause introspection + software reset trigger
 * @ingroup grp_hal_system
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * RA8D2 reset HAL driver. The chip exposes 21 distinct reset causes
 * (HUM Ch 6.1 p 244 Table 6.1) and three "Reset Status Registers"
 * (RSTSR0, RSTSR1, RSTSR2) plus an extended ``RSTSR3`` for the
 * core-voltage / overcurrent / temperature monitor flags. This driver
 * collapses every cause into a small set of tagged enums that the
 * application can switch on without having to know which physical
 * register or bit owns which cause.
 *
 * ## Driver surface (minimum-viable)
 *
 * | API                              | Purpose                            |
 * |----------------------------------|------------------------------------|
 * | ``ra8_reset_init``                | Snapshot reset cause at boot        |
 * | ``ra8_reset_get_cause``           | Decode RSTSR0/1/2/3 -> typed cause  |
 * | ``ra8_reset_get_raw``             | Return the raw 8/32-bit register words |
 * | ``ra8_reset_clear_cause``         | Software-clear (write 0 after read 1)  |
 * | ``ra8_reset_get_attribution``     | Read RSTSAR security attribution    |
 * | ``ra8_reset_software_reset``      | Trigger AIRCR.SYSRESETREQ (no return) |
 * | ``ra8_reset_set_source_mask``     | Disable/enable a reset source (SYRSTMSK0/1/2) |
 * | ``ra8_reset_get_source_mask``     | Read a reset source's mask state    |
 *
 * ## Not yet implemented (deferred TODO)
 *
 *   - ``TEMPRCR`` / ``TEMPRLR`` temperature reset control. The register
 *     definitions live in ``ra8_reset_regs.h`` (HUM Ch 6.2.9 / 6.2.10
 *     p 264-265) but the write-side bring-up is deferred: ``TEMPRLR``
 *     accepts only two writes total before requiring a real reset, so
 *     the enable sequence is call-once-at-boot and belongs in the
 *     secure-boot / temperature-sensor path, not in the general reset
 *     HAL.
 *   - Hot-pluggable per-cause callbacks (``ra8_reset_attach_handler``).
 *     Reset causes fire **before** the firmware runs, so the only
 *     "callback" is whatever code consumes ``ra8_reset_get_cause``
 *     during init. Adding a publish/subscribe layer with no consumer
 *     would be speculative; deferred until a caller needs it.
 *   - VBATT_POR. The HUM lists VBATT_POR as a separate (non-system)
 *     reset whose flag lives in the VBAT block, not in SYSC. The
 *     existing ``ra8_bkup`` driver owns that path.
 *
 * ## Locking
 *
 * Per HUM Ch 6.2.2 / 6.2.3 / 6.2.4 / 6.2.5 register descriptions, the
 * RSTSRn registers are R/W with the Note: "Only 0 can be written. To
 * clear a flag, read 1 from it and then write 0." They do **not** sit
 * behind PRCR. RSTSAR sits behind ``PRCR.PRC1`` (CGC + LVD group);
 * the driver does not touch RSTSAR write-side -- only read-side -- so
 * no PRCR unlock is required for ``ra8_reset_get_attribution``.
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
#include "ra8_reset_regs.h"

/* =============================================================================
 * Reset cause enumeration
 * =============================================================================
 */

/**
 * @enum ra8_reset_cause_t
 * @brief Decoded reset-cause taxonomy.
 *
 * @details
 * Values are non-overlapping so the driver can return exactly one
 * primary cause; callers that want to inspect all latched flags
 * (multiple resets can stack between two software reads) should use
 * ``ra8_reset_get_raw``.
 *
 * Ordering follows HUM Ch 6.1 Table 6.1 p 244. The first matching
 * flag (in register-index order RSTSR2 -> RSTSR0 -> RSTSR3 -> RSTSR1)
 * wins; ``k_ra8_reset_cause_unknown`` is returned when no flag is set
 * (e.g. the firmware itself cleared every cause earlier in boot).
 */
typedef enum : uint8_t {
  k_ra8_reset_cause_unknown            = 0U,  /**< No flag latched.  */
  k_ra8_reset_cause_power_on           = 1U,  /**< RSTSR0.PORF.      */
  k_ra8_reset_cause_lvd0               = 2U,  /**< RSTSR0.LVD0RF.    */
  k_ra8_reset_cause_lvd1               = 3U,  /**< RSTSR0.LVD1RF.    */
  k_ra8_reset_cause_lvd2               = 4U,  /**< RSTSR0.LVD2RF.    */
  k_ra8_reset_cause_lvd4               = 5U,  /**< RSTSR0.LVD4RF.    */
  k_ra8_reset_cause_lvd5               = 6U,  /**< RSTSR0.LVD5RF.    */
  k_ra8_reset_cause_deep_sw_standby    = 7U,  /**< RSTSR0.DPSRSTF.   */
  k_ra8_reset_cause_core_voltage       = 8U,  /**< RSTSR3.CVMRF.     */
  k_ra8_reset_cause_overcurrent        = 9U,  /**< RSTSR3.OCPRF.     */
  k_ra8_reset_cause_temperature        = 10U, /**< RSTSR3.TEMPRF.    */
  k_ra8_reset_cause_iwdt               = 11U, /**< RSTSR1.IWDTRF.    */
  k_ra8_reset_cause_wdt0               = 12U, /**< RSTSR1.WDTRF.     */
  k_ra8_reset_cause_software           = 13U, /**< RSTSR1.SWRF.      */
  k_ra8_reset_cause_lockup0            = 14U, /**< RSTSR1.CLURF.     */
  k_ra8_reset_cause_local_memory0      = 15U, /**< RSTSR1.LM0RF.     */
  k_ra8_reset_cause_bus_peripheral_mpu = 16U, /**< RSTSR1.BUSSRF.    */
  k_ra8_reset_cause_common_memory      = 17U, /**< RSTSR1.CMRF.      */
  k_ra8_reset_cause_wdt1               = 18U, /**< RSTSR1.WDT1RF.    */
  k_ra8_reset_cause_lockup1            = 19U, /**< RSTSR1.CLU1RF.    */
  k_ra8_reset_cause_local_memory1      = 20U, /**< RSTSR1.LM1RF.     */
  k_ra8_reset_cause_network            = 21U, /**< RSTSR1.NWRF.      */
  k_ra8_reset_cause_warm_start         = 22U, /**< RSTSR2.CWSF == 1. */
} ra8_reset_cause_t;

/**
 * @struct ra8_reset_raw_t
 * @brief Raw register snapshot returned by ``ra8_reset_get_raw``.
 *
 * @details
 * cppcheck cannot see tests/ so it flags every member as unused; each
 * field is read by the test suite in ``tests/test_ra8_reset.c`` and by
 * any caller that wants to see every latched flag at once.
 *
 * @invariant ``rstsr0`` and ``rstsr2`` are 8-bit reads; the upper bits
 *            of the wider integer storage are always zero.
 */
typedef struct {
  uint8_t  rstsr0; /**< Raw RSTSR0 byte. */
  uint32_t rstsr1; /**< Raw RSTSR1 word. */
  uint8_t  rstsr2; /**< Raw RSTSR2 byte. */
  uint8_t  rstsr3; /**< Raw RSTSR3 byte. */
} ra8_reset_raw_t;

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Snapshot the reset cause at boot.
 *
 * @details
 * Reads RSTSR0/1/2/3 once and caches the decoded cause + raw words in
 * a file-scope static. Subsequent calls to ``ra8_reset_get_cause`` /
 * ``ra8_reset_get_raw`` return the snapshot rather than re-reading the
 * registers. This is the only safe way to share the cause across the
 * firmware -- any other code path that calls
 * ``ra8_reset_clear_cause`` would otherwise destroy the information
 * before later modules could see it.
 *
 * Idempotent: calling ``ra8_reset_init`` more than once re-reads the
 * registers and overwrites the snapshot, which is what you want during
 * unit tests but you should avoid in production code.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Snapshot captured.
 *
 * @pre None -- can be called as the very first init step.
 * @pre PRCR does not need to be unlocked for RSTSRn reads.
 * @post ``ra8_reset_get_cause`` will return the cached value.
 * @post ``ra8_reset_get_raw`` will return the cached raw register snapshot.
 *
 * @note Thread safety: not thread-safe; intended for single-threaded
 *       boot context.
 * @see ra8_reset_get_cause
 * @see ra8_reset_get_raw
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_reset_init(void);

/**
 * @brief Reset the cached reset-cause snapshot for host tests.
 *
 * @details
 * Zeroes the cached boot-cause snapshot so each host test case starts from the
 * driver's just-loaded state. Compiled unconditionally (like the other
 * ``*_test_*`` helpers) but only ever called from ``tests/``; on the target it
 * is an unreferenced symbol dropped by ``--gc-sections``.
 *
 * @note Host-test only; not part of the target firmware API.
 * @pre The driver is otherwise idle (single-threaded test context).
 * @pre No caller depends on the current cached snapshot.
 * @post ``ra8_reset_get_cause`` re-reads the registers on the next call.
 * @post The cached cause is ``k_ra8_reset_cause_unknown``.
 * @since 0.1.0
 */
void ra8_reset_test_only_reset_state(void);

/* =============================================================================
 * Cause introspection
 * =============================================================================
 */

/**
 * @brief Decode the latched RSTSRn flags into a single primary cause.
 *
 * @details
 * If ``ra8_reset_init`` was called this returns the cached snapshot;
 * otherwise it reads the registers fresh. The returned value is the
 * highest-priority cause currently latched. Priority order matches
 * HUM Ch 6.1 Table 6.1 p 244 (POR -> LVD -> Deep SW Standby ->
 * core/temp/overcurrent -> IWDT/WDT/SW/Lockup/LM/BUS/CM -> Cold/Warm).
 *
 * To examine every latched flag at once (multiple causes can stack
 * between two reads), use ``ra8_reset_get_raw`` instead.
 *
 * @param[out] out  Decoded cause (non-NULL).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok            Cause written to ``*out``.
 * @retval k_ra8_err_null_ptr  ``out`` was nullptr.
 *
 * @pre ``out != nullptr``.
 * @pre The host or target has the SYSC block mapped (always true on RA8D2
 *      and via ``ra8_fake_mmap`` in host tests).
 * @post ``*out`` holds the decoded cause.
 * @post No hardware state is modified.
 *
 * @note Thread safety: read-only; safe to call from any context.
 * @see ra8_reset_get_raw
 * @see ra8_reset_clear_cause
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_reset_get_cause(ra8_reset_cause_t* out);

/**
 * @brief Read the raw RSTSR0 / RSTSR1 / RSTSR2 / RSTSR3 register words.
 *
 * @details
 * If ``ra8_reset_init`` has run this returns the cached snapshot;
 * otherwise the registers are read fresh.
 *
 * @param[out] out  Destination struct (non-NULL).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok            Raw values written.
 * @retval k_ra8_err_null_ptr  ``out`` was nullptr.
 *
 * @pre ``out != nullptr``.
 * @pre The reset block is mapped (true on target and host-test).
 * @post ``*out`` populated with the latest snapshot.
 * @post No hardware state is modified.
 *
 * @note Thread safety: read-only.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_reset_get_raw(ra8_reset_raw_t* out);

/**
 * @brief Clear (write 0 to) the requested set of latched RSTSRn flags.
 *
 * @details
 * Per HUM Ch 6.2.2 / 6.2.3 / 6.2.4 register descriptions, every RSTSRn
 * flag uses the "read 1 then write 0" idiom: software clears a bit by
 * writing 0 after observing it as 1. This function performs the write
 * step for every bit selected by ``mask`` across all three RSTSR
 * registers.
 *
 * The ``mask`` argument is encoded as follows so a single call can
 * clear flags from RSTSR0 (bits 0..7), RSTSR1 (bits 8..30) and
 * RSTSR2/RSTSR3 (bits 31..) without forcing the caller to issue
 * three separate calls:
 *
 *   - bits 0..7   -- RSTSR0 mask (PORF, LVDxRF, DPSRSTF).
 *   - bits 8..30  -- RSTSR1 mask shifted left by 8.
 *   - bit 31      -- RSTSR2.CWSF (write-1-to-set per HUM Ch 6.2.4
 *                    Note 2 p 261).
 *
 * RSTSR3 has no software-clear path -- its flags are owned by the
 * core-voltage / temperature monitor secure code; this function leaves
 * RSTSR3 alone.
 *
 * @param[in] mask Encoded clear mask (see above).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Mask applied.
 *
 * @pre None.
 * @pre Caller has already inspected the cause via ``ra8_reset_get_cause``
 *      if the cause needs to survive the clear.
 * @post Every flag selected by ``mask`` reads 0 (after a 2-PCLKB
 *       settling window per HUM Ch 6.2.2 p 258 Note).
 * @post Other RSTSRn bits are untouched.
 *
 * @note Thread safety: not thread-safe.
 * @warning After this call ``ra8_reset_get_cause`` will reflect the
 *          remaining (unclearned) flags or ``k_ra8_reset_cause_unknown``.
 * @see ra8_reset_init  Snapshot the cause **before** calling clear.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_reset_clear_cause(uint32_t mask);

/* =============================================================================
 * Security attribution
 * =============================================================================
 */

/**
 * @brief Read the RSTSAR (Reset Security Attribution) register.
 *
 * @details
 * RSTSAR at SYSC offset ``0x3C4`` holds five ``NONSECn`` bits that
 * select whether each of the five reset-status group registers
 * (RSTSR0..RSTSR3 + future) is reachable from the non-secure side.
 * The driver returns the raw 32-bit value; callers can mask against
 * ``k_ra8_reset_rstsar_field_msk`` to drop reserved upper bits.
 *
 * @param[out] out Receives the RSTSAR value (non-NULL).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok            Value written.
 * @retval k_ra8_err_null_ptr  ``out`` was nullptr.
 *
 * @pre ``out != nullptr``.
 * @pre Caller is on the secure side or RSTSAR has already been made
 *      non-secure-readable.
 * @post ``*out`` holds the RSTSAR snapshot.
 * @post No hardware state is modified.
 *
 * @note Thread safety: read-only.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_reset_get_attribution(uint32_t* out);

/* =============================================================================
 * Software reset
 * =============================================================================
 */

/**
 * @brief Trigger a software-initiated system reset.
 *
 * @details
 * Writes the Cortex-M85 ``SCB->AIRCR`` register with the mandatory
 * ``0x05FA`` write-key in the upper half-word and the ``SYSRESETREQ``
 * bit set, exactly as ``CMSIS NVIC_SystemReset`` does. The reset
 * **does not return** -- on real hardware control is given to the
 * reset exception handler within a few cycles. The function ends with
 * a ``__DSB`` (or its off-target equivalent) so the AIRCR write is
 * observable before the CPU stalls waiting for reset.
 *
 * After the reset completes ``RSTSR1.SWRF`` will read 1 and
 * ``ra8_reset_get_cause`` will return ``k_ra8_reset_cause_software``.
 *
 * @pre None.
 * @pre Caller has flushed any data that must survive the reset to
 *      non-volatile storage (MRAM, BKUP regs).
 * @post On target: control transferred to the reset vector.
 * @post On host:   ``*ra8_reset_aircr() & SYSRESETREQ_msk`` reads 1.
 *
 * @note Thread safety: not thread-safe; should be the last code that
 *       runs on the CPU before the reset.
 * @warning This call **does not return on target** -- treat it as
 *          equivalent to ``[[noreturn]]`` for control-flow analysis.
 *          The function is **not** declared ``[[noreturn]]`` because
 *          host unit tests need to observe the AIRCR write and then
 *          continue executing; on the host build the function does
 *          return after writing AIRCR. Use it only as the last
 *          statement of a function on target firmware.
 * @warning Pending DMA, USB, and watchdog state are wiped. Make sure
 *          you have already disabled any external bus controller that
 *          might continue driving lines after the CPU resets.
 * @see HUM Ch 6.1 Table 6.1 p 244 ("Software reset / Source / Register
 *      setting (use the software reset bit AIRCR.SYSRESETREQ)")
 * @since 0.1.0
 */
void ra8_reset_software_reset(void);

/* =============================================================================
 * Reset-source masking (SYRSTMSK0/1/2)
 * =============================================================================
 */

/**
 * @enum ra8_reset_source_t
 * @brief Reset sources whose occurrence can be individually disabled.
 *
 * @details
 * Each value maps to one mask bit across SYRSTMSK0/1/2 (HUM Ch 6.2.6-6.2.8
 * p 262-264). Setting a source's mask disables the corresponding reset;
 * clearing it re-enables the reset (the reset-value of every mask is 0 =
 * enabled). ``k_ra8_reset_source_count`` is a non-source sentinel used for
 * range validation, never a maskable bit.
 *
 * @invariant ``0 <= source < k_ra8_reset_source_count`` for every API call.
 */
typedef enum : uint8_t {
  k_ra8_reset_source_iwdt  = 0U,  /**< Independent watchdog reset (SYRSTMSK0). */
  k_ra8_reset_source_wdt0  = 1U,  /**< CPU0 watchdog reset (SYRSTMSK0).        */
  k_ra8_reset_source_sw    = 2U,  /**< Software reset (SYRSTMSK0).             */
  k_ra8_reset_source_clu0  = 3U,  /**< CPU0 lockup reset (SYRSTMSK0).          */
  k_ra8_reset_source_lm0   = 4U,  /**< Local-memory-0 error reset (SYRSTMSK0). */
  k_ra8_reset_source_cm    = 5U,  /**< Common-memory error reset (SYRSTMSK0).  */
  k_ra8_reset_source_bus   = 6U,  /**< Bus error reset (SYRSTMSK0).            */
  k_ra8_reset_source_wdt1  = 7U,  /**< CPU1 watchdog reset (SYRSTMSK1).        */
  k_ra8_reset_source_clu1  = 8U,  /**< CPU1 lockup reset (SYRSTMSK1).          */
  k_ra8_reset_source_lm1   = 9U,  /**< Local-memory-1 error reset (SYRSTMSK1). */
  k_ra8_reset_source_pvd1  = 10U, /**< Voltage-monitor-1 reset (SYRSTMSK2).    */
  k_ra8_reset_source_pvd2  = 11U, /**< Voltage-monitor-2 reset (SYRSTMSK2).    */
  k_ra8_reset_source_count = 12U, /**< Sentinel: number of maskable sources.   */
} ra8_reset_source_t;

/**
 * @brief Enable or disable the occurrence of a specific reset source.
 *
 * @details
 * Sets (``disable == true``) or clears (``disable == false``) the mask bit
 * for @p source in SYRSTMSK0/1/2. The register group is write-protected by
 * ``PRCR.PRC5`` (HUM Ch 6.2.6 Note p 262): this function unlocks PRC5,
 * performs the read-modify-write of the relevant SYRSTMSKn byte, then
 * relocks PRC5 -- leaving the other PRC bits untouched.
 *
 * @param[in] source  Reset source to mask/unmask (< k_ra8_reset_source_count).
 * @param[in] disable ``true`` disables the reset; ``false`` re-enables it.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Mask bit updated.
 * @retval k_ra8_err_invalid_arg   @p source >= k_ra8_reset_source_count.
 *
 * @pre @p source is a valid ``ra8_reset_source_t`` (< count).
 * @pre Caller is on the secure side (the SYSC reset-control group is
 *      secure-attributed by default).
 * @post The SYRSTMSKn mask bit for @p source reads @p disable.
 * @post PRCR.PRC5 is left re-locked (write-protected) on return.
 *
 * @note Thread safety: not thread-safe; serialize SYSC PRCR-gated writes.
 * @warning Per HUM Ch 6.2.6 p 263, ``IWDTMASK`` cannot be rewritten while the
 *          independent watchdog is running and ``WDT0MASK`` cannot be
 *          rewritten while the CPU0 watchdog is running -- the hardware
 *          silently ignores such writes. Mask those sources before starting
 *          their watchdogs.
 * @see ra8_reset_get_source_mask
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_reset_set_source_mask(ra8_reset_source_t source, bool disable);

/**
 * @brief Read whether a specific reset source is currently masked (disabled).
 *
 * @details
 * Reads the relevant SYRSTMSK0/1/2 byte and returns the mask state for
 * @p source. Reads do not require a PRCR unlock.
 *
 * @param[in]  source   Reset source to query (< k_ra8_reset_source_count).
 * @param[out] disabled Receives ``true`` if the reset is masked/disabled.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Result written to ``*disabled``.
 * @retval k_ra8_err_null_ptr      ``disabled`` was nullptr.
 * @retval k_ra8_err_invalid_arg   @p source >= k_ra8_reset_source_count.
 *
 * @pre ``disabled != nullptr``.
 * @pre @p source is a valid ``ra8_reset_source_t`` (< count).
 * @post ``*disabled`` holds the current mask state.
 * @post No hardware state is modified.
 *
 * @note Thread safety: read-only; safe from any context.
 * @see ra8_reset_set_source_mask
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_reset_get_source_mask(ra8_reset_source_t source, bool* disabled);

#ifdef __cplusplus
}
#endif
