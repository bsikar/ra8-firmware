/**
 * @file ra8_cnecc.h
 * @brief CANFD ECC (CNECC) HAL driver public API
 * @ingroup grp_hal_crypto
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Full driver for the RA8D2 CANFD message-buffer SRAM ECC block
 * (HUM Ch 42 "CANFD ECC (CNECC)", p 2868-2876). Two instances
 * (``ECCMB0`` for ``CAN0``, ``ECCMB1`` for ``CAN1``) -- each one
 * observes the 32-bit CANFD MBRAM behind its channel, attaches a
 * 7-bit ECC code, and reports
 *
 *  - 1-bit corrected errors (SEC),
 *  - 2-bit detected-but-uncorrected errors (DED),
 *  - and an overflow flag if a second fault is captured before the
 *    first is acknowledged.
 *
 * The driver covers every register field documented in HUM Ch 42:
 *
 *  - ``EC710CTL``  -- enable / status / IRQ / clear
 *  - ``EC710TMC``  -- decoder fault-injection control
 *  - ``EC710TED``  -- decoder fault-injection substitute data
 *  - ``EC710EAD0`` -- captured faulting RAM offset
 *
 * ## Public surface
 *
 * Lifecycle:
 *  - ``ra8_cnecc_init``                 -- bring up both instances
 *  - ``ra8_cnecc_deinit``               -- tear down both instances
 *  - ``ra8_cnecc_enable_instance``      -- per-instance enable
 *  - ``ra8_cnecc_disable_instance``     -- per-instance disable
 *  - ``ra8_cnecc_enter_standby``        -- HUM 42.5.1 standby prep
 *  - ``ra8_cnecc_exit_standby``         -- HUM 42.5.2 standby exit
 *
 * Configuration:
 *  - ``ra8_cnecc_set_irq_enables``      -- per-instance IRQ mask
 *  - ``ra8_cnecc_set_correction_permission`` -- per-instance EC1ECP
 *
 * Status / counters:
 *  - ``ra8_cnecc_get_status``           -- snapshot live + counters
 *  - ``ra8_cnecc_clear_status``         -- W0C the status flags
 *  - ``ra8_cnecc_get_counters``         -- read-only counter bundle
 *  - ``ra8_cnecc_reset_counters``       -- zero counters per instance
 *  - ``ra8_cnecc_set_counter_mirror``   -- attach BBR-mirrored counters
 *
 * Fault injection (HUM 42.3.2 Figure 42.2):
 *  - ``ra8_cnecc_inject_fault``         -- run the four-step procedure
 *  - ``ra8_cnecc_test_mode_disable``    -- force EC710TMC = 0x8000
 *  - ``ra8_cnecc_test_mode_active``     -- query ECTMCE bit
 *
 * IRQ wiring (HUM 42.4):
 *  - ``ra8_cnecc_attach_handler``       -- callback registration
 *  - ``ra8_cnecc_attach_isr``           -- auto-wire ICU vectors
 *  - ``ra8_cnecc_detach_isr``           -- unwire ICU vectors
 *  - ``ra8_cnecc_isr_handler``          -- generic ICU trampoline
 *  - ``ra8_cnecc_dispatch``             -- direct dispatch (test/manual)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_cnecc_regs.h"
#include "ra8_err.h"
#include "ra8_isr.h"

/* =============================================================================
 * Configuration types
 * =============================================================================
 */

/**
 * @struct ra8_cnecc_instance_cfg_t
 * @brief Per-instance ECC configuration descriptor.
 *
 * @details
 * Mirrors the HUM 42.3.1 procedure (figure 42.1 p 2874): set
 * ``correct_1bit`` / ``irq_1bit`` / ``irq_2bit``, then enable error
 * judgment (``ECERVF``). ``cppcheck`` is not aware of the cross-TU
 * read in ``ra8_cnecc.c`` and flags every member as unused -- the
 * suppression below silences it.
 */
typedef struct {
  bool correct_1bit; /**< true => HW corrects 1-bit faults (clears EC1ECP). */
  bool irq_1bit;     /**< true => raise IRQ on every 1-bit fault.           */
  bool irq_2bit;     /**< true => raise IRQ on every 2-bit fault.           */
  bool enable;       /**< true => leave ECERVF set after init.              */
} ra8_cnecc_instance_cfg_t;

/**
 * @struct ra8_cnecc_config_t
 * @brief Top-level driver configuration.
 *
 * @details
 * One sub-config per instance; ``instances[i]`` is applied to
 * ``ECCMBi``. Both instances are programmed identically in most
 * cases, but the HW supports per-instance settings so the descriptor
 * is split out.
 */
typedef struct {
  ra8_cnecc_instance_cfg_t instances[k_ra8_cnecc_instance_count]; /**< Per-instance settings. */
} ra8_cnecc_config_t;

/* =============================================================================
 * Status / event types
 * =============================================================================
 */

/**
 * @struct ra8_cnecc_status_t
 * @brief Snapshot of one CNECC instance, returned by ``ra8_cnecc_get_status``.
 *
 * @details
 * ``raw_ctl`` is the 32-bit ``EC710CTL`` value at sample time, so
 * future helpers can decode bits the driver does not yet name.
 * ``raw_tmc`` holds ``EC710TMC`` so callers can detect a stuck
 * fault-injection enable. ``one_bit_count`` / ``two_bit_count`` /
 * ``overflow_count`` are software counters maintained by
 * ``ra8_cnecc_dispatch`` -- the hardware itself only latches "was
 * there at least one error" booleans, not a count. HUM Ch 42.2.4
 * p 2872 caps ``last_addr`` at 10 bits.
 */
typedef struct {
  uint32_t raw_ctl;         /**< Live EC710CTL value.                          */
  uint16_t raw_tmc;         /**< Live EC710TMC value.                          */
  uint16_t reserved0;       /**< Padding for alignment.                        */
  uint32_t one_bit_count;   /**< Cumulative 1-bit fault count.                 */
  uint32_t two_bit_count;   /**< Cumulative 2-bit fault count.                 */
  uint32_t overflow_count;  /**< Cumulative ECOVFF count.                      */
  uint16_t last_addr;       /**< Last captured ECEAD[9:0] RAM offset.          */
  bool     err_present;     /**< Live ECEMF: present-cycle error.              */
  bool     err_1bit;        /**< Live ECER1F: 1-bit fault latched.             */
  bool     err_2bit;        /**< Live ECER2F: 2-bit fault latched.             */
  bool     overflow;        /**< Live ECOVFF: address-capture overflow.        */
  bool     addr_is_1bit;    /**< true => last_addr was a 1-bit fault.          */
  bool     addr_is_2bit;    /**< true => last_addr was a 2-bit fault.          */
  bool     judgment_active; /**< Live ECERVF: judgment currently enabled.      */
  bool     correct_enabled; /**< true => 1-bit correction enabled (EC1ECP=0).  */
  bool     irq1_enabled;    /**< Live EC1EDIC.                                 */
  bool     irq2_enabled;    /**< Live EC2EDIC.                                 */
  bool     test_mode;       /**< Live ECTMCE: fault-injection currently armed. */
  bool     reserved1;       /**< Padding.                                      */
} ra8_cnecc_status_t;

/**
 * @struct ra8_cnecc_counters_t
 * @brief Cumulative per-instance fault counters.
 *
 * @details
 * Returned by ``ra8_cnecc_get_counters`` for callers that only care
 * about the soft counters (without the live register snapshot
 * provided by ``ra8_cnecc_get_status``). When a BBR mirror is
 * attached via ``ra8_cnecc_set_counter_mirror`` the values returned
 * here also live in battery-backed RAM.
 */
typedef struct {
  uint32_t one_bit_count;  /**< Lifetime 1-bit fault count for this instance. */
  uint32_t two_bit_count;  /**< Lifetime 2-bit fault count for this instance. */
  uint32_t overflow_count; /**< Lifetime ECOVFF count for this instance.      */
} ra8_cnecc_counters_t;

/**
 * @typedef ra8_cnecc_error_fn_t
 * @brief CANFD ECC fault callback signature.
 *
 * @param[in] ctx       Caller-supplied opaque context pointer.
 * @param[in] instance  Instance index 0..1 that raised the error.
 * @param[in] is_2bit   ``true`` for 2-bit (uncorrectable),
 *                      ``false`` for 1-bit (corrected).
 * @param[in] err_addr  Faulting offset (ECEAD[9:0]) inside the
 *                      CANFD message buffer SRAM.
 */
typedef void (*ra8_cnecc_error_fn_t)(void* ctx, uint8_t instance, bool is_2bit, uint16_t err_addr);

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Initialise the CNECC driver and configure both instances.
 *
 * @details
 * Programs both ``EC710CTL`` instances per ``cfg``, walking the HUM
 * 42.3.1 procedure (figure 42.1, p 2874):
 *
 *   1. Ungate the parent CANFD module-stop bits so register
 *      accesses reach the CNECC block (the ECC sits behind the same
 *      clock as the CANFD it monitors -- HUM 42.1 overview p 2868).
 *   2. Clear any latched error state with ``ECER1C | ECER2C``.
 *   3. Optionally enable 1-bit-error correction and per-fault IRQs.
 *   4. Unlock ``ECERVF`` via ``EMCA = 01b`` and set ``ECERVF`` per
 *      ``cfg->instances[i].enable``.
 *
 * @param[in] cfg Non-NULL configuration descriptor.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Driver initialized, both instances live.
 * @retval k_ra8_err_null_ptr      ``cfg`` was NULL.
 * @retval k_ra8_err_hw_init_failed MSTP ungate failed for one of the
 *                                  CANFD instances.
 *
 * @pre  ``ra8_mstp_init`` has run.
 * @pre  Caller is in single-threaded init context (writes to the
 *       EMCA-protected ``ECERVF`` bit are not thread-safe).
 * @post Both instances' ``EC710CTL.ECERVF`` reflect cfg.
 * @post Both instances' SEC + DED + overflow counters are zeroed.
 *
 * @note Not thread-safe.
 *
 * @see ra8_cnecc_deinit
 * @see ra8_cnecc_get_status
 *
 * @since 0.1.0
 *
 * @par NASA Power of 10 Compliance:
 * - Rule 5: 2 preconditions, 2 postconditions
 * - Rule 7: returns ra8_err_t, marked [[nodiscard]]
 */
[[nodiscard]] ra8_err_t ra8_cnecc_init(const ra8_cnecc_config_t* cfg);

/**
 * @brief Tear down both CNECC instances and gate the CANFD MSTP bits.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Always.
 *
 * @pre  Caller has stopped any CANFD traffic that depends on MBRAM.
 * @pre  ``ra8_cnecc_init`` was previously called (deinit is idempotent
 *       even if not).
 * @post Both ``EC710CTL.ECERVF`` are cleared (judgment disabled).
 * @post Both CANFDn MSTP bits are released.
 *
 * @note Not thread-safe.
 * @see ra8_cnecc_init
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_cnecc_deinit(void);

/**
 * @brief Enable error judgment for one instance only.
 *
 * @details
 * Performs the ``ECERVF = 1`` step of the HUM 42.3.1 procedure for
 * one instance; the other instance is left untouched. The write goes
 * through the EMCA = 01b unlock pattern in the same store (HUM 42.2.1
 * p 2870 EMCA notes).
 *
 * @param[in] instance Instance index 0..1.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Judgment enabled.
 * @retval k_ra8_err_invalid_arg ``instance`` >= ``k_ra8_cnecc_instance_count``.
 *
 * @pre  ``ra8_cnecc_init`` has run.
 * @pre  Caller has masked IRQs or runs single-threaded.
 * @post Live ``EC710CTL.ECERVF`` reads back as 1.
 * @post Other configuration bits in ``EC710CTL`` are preserved.
 *
 * @note Not thread-safe.
 * @see ra8_cnecc_disable_instance
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_cnecc_enable_instance(uint8_t instance);

/**
 * @brief Disable error judgment for one instance only.
 *
 * @details
 * Clears ``ECERVF`` for ``instance`` (with the EMCA unlock pattern).
 * Leaves the IRQ enable / correction-permission bits alone so the
 * caller can re-enable later via ``ra8_cnecc_enable_instance``.
 *
 * @param[in] instance Instance index 0..1.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Judgment disabled.
 * @retval k_ra8_err_invalid_arg ``instance`` >= ``k_ra8_cnecc_instance_count``.
 *
 * @pre  ``ra8_cnecc_init`` has run.
 * @pre  Caller has masked IRQs or runs single-threaded.
 * @post Live ``EC710CTL.ECERVF`` reads back as 0.
 * @post All other ``EC710CTL`` bits are preserved.
 *
 * @note Not thread-safe.
 * @see ra8_cnecc_enable_instance
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_cnecc_disable_instance(uint8_t instance);

/**
 * @brief Software-standby preparation (HUM 42.5.1 p 2876).
 *
 * @details
 * Per HUM Ch 42.5.1 the firmware MUST clear the ECC error flags and
 * disable judgment before entering Software Standby. This helper:
 *
 *   1. Clears ECER1F / ECER2F / ECOVFF / ECSEDF0 / ECDEDF0 via the
 *      ECER1C | ECER2C bundle.
 *   2. Clears ECERVF via the EMCA = 01b unlock pattern.
 *
 * The driver-local fault counters are preserved (they survive any
 * standby trip).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Always succeeds.
 *
 * @pre  ``ra8_cnecc_init`` has run.
 * @pre  All CANFD traffic on the parent channels is stopped.
 * @post Both instances' ECERVF read 0.
 * @post Both instances' status flags read 0.
 *
 * @note Not thread-safe.
 * @see ra8_cnecc_exit_standby
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_cnecc_enter_standby(void);

/**
 * @brief Software-standby exit (HUM 42.5.2 p 2876).
 *
 * @details
 * Restores ``ECERVF = 1`` for both instances after a wakeup from
 * Software Standby. Re-uses the cached configuration from the last
 * ``ra8_cnecc_init`` call so per-instance ``correct_1bit`` /
 * ``irq_1bit`` / ``irq_2bit`` survive across the standby trip.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                  Both instances re-armed.
 * @retval k_ra8_err_not_initialized ``ra8_cnecc_init`` was never called.
 *
 * @pre  ``ra8_cnecc_init`` has run at least once before standby.
 * @pre  CPU has just returned from Software Standby.
 * @post Both instances reflect the original cfg (ECERVF=1 if so
 *       configured).
 * @post Driver-local counters are preserved.
 *
 * @note Not thread-safe.
 * @see ra8_cnecc_enter_standby
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_cnecc_exit_standby(void);

/* =============================================================================
 * Per-instance configuration tweaks
 * =============================================================================
 */

/**
 * @brief Update the IRQ enable bits for one instance without touching
 *        any other CTL field.
 *
 * @details
 * Read-modify-writes ``EC710CTL`` so the EC1EDIC / EC2EDIC bits
 * reflect the new flags while ECERVF, EC1ECP, latched status, and
 * EMCA are preserved. The write re-asserts the EMCA = 01b unlock
 * pattern so the same store does not accidentally clear ECERVF (HUM
 * 42.2.1 p 2870 EMCA notes).
 *
 * @param[in] instance Instance index 0..1.
 * @param[in] irq_1bit ``true`` => enable EC1EDIC.
 * @param[in] irq_2bit ``true`` => enable EC2EDIC.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Mask updated.
 * @retval k_ra8_err_invalid_arg ``instance`` out of range.
 *
 * @pre  ``ra8_cnecc_init`` has run.
 * @pre  Caller has masked IRQs or runs single-threaded.
 * @post Live ``EC710CTL.EC1EDIC`` matches ``irq_1bit``.
 * @post Live ``EC710CTL.EC2EDIC`` matches ``irq_2bit``.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_cnecc_set_irq_enables(uint8_t instance, bool irq_1bit, bool irq_2bit);

/**
 * @brief Update the 1-bit correction permission for one instance.
 *
 * @details
 * Per HUM Ch 42.2.1 p 2870, EC1ECP = 0 means "correction executed",
 * EC1ECP = 1 means "correction NOT executed". The argument follows
 * the natural semantic (``correct_1bit = true`` => correction
 * enabled, EC1ECP cleared).
 *
 * @param[in] instance     Instance index 0..1.
 * @param[in] correct_1bit ``true`` => clear EC1ECP (HW corrects).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              EC1ECP updated.
 * @retval k_ra8_err_invalid_arg ``instance`` out of range.
 *
 * @pre  ``ra8_cnecc_init`` has run.
 * @pre  Caller has masked IRQs or runs single-threaded.
 * @post Live ``EC710CTL.EC1ECP`` reads back per ``!correct_1bit``.
 * @post Other ``EC710CTL`` bits are preserved.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_cnecc_set_correction_permission(uint8_t instance, bool correct_1bit);

/* =============================================================================
 * Status / event API
 * =============================================================================
 */

/**
 * @brief Snapshot the current ECC state of one CNECC instance.
 *
 * @details
 * Reads ``EC710CTL``, ``EC710TMC`` and ``EC710EAD0`` for ``instance``
 * and returns a decoded view through ``out``. The cumulative SEC /
 * DED / overflow counts are driver-local (incremented by
 * ``ra8_cnecc_dispatch``) -- the hardware only carries the "at least
 * one happened" booleans.
 *
 * @param[in]  instance Instance index 0..1.
 * @param[out] out      Non-NULL status receiver.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Status copied to ``*out``.
 * @retval k_ra8_err_null_ptr    ``out`` was NULL.
 * @retval k_ra8_err_invalid_arg ``instance`` >= ``k_ra8_cnecc_instance_count``.
 *
 * @pre  ``out`` points to writable memory of at least
 *       ``sizeof(ra8_cnecc_status_t)`` bytes.
 * @pre  ``ra8_cnecc_init`` has run.
 * @post ``out->raw_ctl`` matches the live ``EC710CTL`` value.
 * @post ``out->last_addr`` <= ``k_ra8_cnecc_ead_max`` (10-bit field,
 *       HUM 42.2.4 p 2872).
 *
 * @note Read-only path; safe to call from any context whose memory
 *       barriers cover the underlying MMIO.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_cnecc_get_status(uint8_t instance, ra8_cnecc_status_t* out);

/**
 * @brief Read the cumulative fault counters for one instance.
 *
 * @details
 * Lightweight accessor for callers that don't need the full
 * ``ra8_cnecc_get_status`` snapshot. Returns the same counters that
 * ``ra8_cnecc_dispatch`` increments and that the optional BBR mirror
 * tracks.
 *
 * @param[in]  instance Instance index 0..1.
 * @param[out] out      Non-NULL counter receiver.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Counters copied.
 * @retval k_ra8_err_null_ptr    ``out`` was NULL.
 * @retval k_ra8_err_invalid_arg ``instance`` out of range.
 *
 * @pre  ``out`` points to writable storage.
 * @pre  ``instance`` is a valid index.
 * @post ``*out`` matches the driver-local counter triple for
 *       ``instance``.
 * @post Hardware state is unchanged.
 *
 * @note Read-only; safe to call concurrently with the dispatcher
 *       provided the host's 32-bit aligned writes are atomic (true
 *       on Cortex-M85).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_cnecc_get_counters(uint8_t instance, ra8_cnecc_counters_t* out);

/**
 * @brief Zero the cumulative fault counters for one instance.
 *
 * @param[in] instance Instance index 0..1.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Counters zeroed (in driver and BBR mirror).
 * @retval k_ra8_err_invalid_arg ``instance`` out of range.
 *
 * @pre  ``ra8_cnecc_init`` has run.
 * @pre  IRQs masked or single-threaded context.
 * @post One-bit / two-bit / overflow counters for ``instance`` read
 *       back as 0.
 * @post Hardware status flags are NOT cleared (use
 *       ``ra8_cnecc_clear_status`` for that).
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_cnecc_reset_counters(uint8_t instance);

/**
 * @brief Attach a BBR-mirrored counter triple for one instance.
 *
 * @details
 * The driver keeps SEC / DED / overflow counters in plain SRAM that
 * does not survive a reset. Production health-monitor pipelines
 * often want the counters in battery-backed RAM (e.g. one of the
 * 32 ``VBTBKRn`` words exposed by ``ra8_bkup``) so the lifetime
 * counts persist across cold boots. Callers allocate a
 * ``ra8_cnecc_counters_t`` in BBR memory, hand the pointer to this
 * function, and the driver mirrors every ``ra8_cnecc_dispatch`` /
 * ``ra8_cnecc_reset_counters`` write into it.
 *
 * Pass ``NULL`` to detach the mirror without resetting.
 *
 * @param[in] instance Instance index 0..1.
 * @param[in] mirror   Pointer to caller-managed BBR storage, or
 *                     ``nullptr`` to detach.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Mirror attached / detached.
 * @retval k_ra8_err_invalid_arg ``instance`` out of range.
 *
 * @pre  ``ra8_cnecc_init`` has run.
 * @pre  Caller manages the lifetime of ``*mirror``; the driver
 *       does not copy it.
 * @post Subsequent ``ra8_cnecc_dispatch`` calls update both the
 *       driver-local counter and ``*mirror``.
 * @post Subsequent ``ra8_cnecc_reset_counters`` calls zero both.
 *
 * @note Not thread-safe at install time; the runtime mirror update
 *       is plain word writes (atomic on Cortex-M85).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_cnecc_set_counter_mirror(uint8_t               instance,
                                                     ra8_cnecc_counters_t* mirror);

/**
 * @brief Clear the latched ECC fault state for one instance.
 *
 * @details
 * Writes ``ECER1C | ECER2C = 1`` to the instance's ``EC710CTL``,
 * which per HUM Ch 42.2.1 p 2870 clears ``ECER1F``, ``ECER2F``,
 * ``ECOVFF``, ``ECSEDF0`` and ``ECDEDF0`` in one shot. The driver's
 * cumulative counters are NOT zeroed (use ``ra8_cnecc_reset_counters``
 * for that).
 *
 * @param[in] instance Instance index 0..1.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Cleared.
 * @retval k_ra8_err_invalid_arg ``instance`` >= ``k_ra8_cnecc_instance_count``.
 *
 * @pre  ``ra8_cnecc_init`` has run.
 * @pre  ``instance`` is a valid instance index.
 * @post Instance's ECER1F / ECER2F / ECOVFF / ECSEDF0 / ECDEDF0 read
 *       back as 0.
 * @post Instance's ``EC710EAD0`` reads back as 0 (HUM 42.2.4 p 2873
 *       "reset by clearing the status flag").
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_cnecc_clear_status(uint8_t instance);

/* =============================================================================
 * Fault injection (HUM 42.3.2 ECC Decoder Testing)
 * =============================================================================
 */

/**
 * @struct ra8_cnecc_inject_t
 * @brief Fault-injection request descriptor.
 *
 * @details
 * Wraps the four-step procedure on HUM page 2875 (Figure 42.2):
 *   1. Disable ECC test mode (EC710TMC = 0x8000).
 *   2. Drive substitute decoder data (EC710TED = ``substitute``).
 *   3. Enable ECC test mode (EC710TMC = 0x8080).
 *   4. Select EC710TED as decoder input (EC710TMC = 0x8082).
 *
 * The caller is responsible for the actual MBRAM read that triggers
 * the decoder pass and surfaces the fake fault on the next
 * ``ra8_cnecc_dispatch`` -- this helper only programs the registers.
 *
 * Pass ``one_bit_flip = true`` to flip exactly one bit of the
 * configured decoder input from the previous correct value (a 1-bit
 * SEC scenario), or ``false`` to flip two bits (a 2-bit DED scenario).
 * The substitute value to write is fully application-defined; this
 * descriptor stores it as ``substitute`` for traceability and the
 * driver writes it verbatim into EC710TED.
 *
 * cppcheck cannot see tests/ so it flags every member as unused.
 */
typedef struct {
  uint32_t substitute;   /**< 32-bit value stored in EC710TED.             */
  bool     one_bit_flip; /**< true => 1-bit error variant; false => 2-bit. */
} ra8_cnecc_inject_t;

/**
 * @brief Run the HUM Ch 42.3.2 fault-injection sequence for one instance.
 *
 * @details
 * Executes the four-step procedure from Figure 42.2 (p 2875). After
 * this returns, the caller should perform an MBRAM read at the
 * target address; the read will route through the test substitute
 * data and trigger the decoder fault path. The captured offset will
 * appear in ``EC710EAD0`` and the matching ``ECER1F`` / ``ECER2F``
 * flag will latch as if a real ECC fault had occurred.
 *
 * Call ``ra8_cnecc_test_mode_disable`` after the test to restore
 * normal operation.
 *
 * @param[in] instance Instance index 0..1.
 * @param[in] req      Non-NULL injection descriptor.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Test-mode programmed.
 * @retval k_ra8_err_null_ptr    ``req`` was NULL.
 * @retval k_ra8_err_invalid_arg ``instance`` out of range.
 *
 * @pre  ``ra8_cnecc_init`` has run with the matching instance enabled.
 * @pre  Caller has stopped CANFD traffic on the parent channel.
 * @post Live ``EC710TMC`` reads back as ``0x8082``.
 * @post Live ``EC710TED`` reads back as ``req->substitute``.
 *
 * @note Not thread-safe. Fault injection is destructive to live
 *       traffic and must only run on a quiesced channel.
 * @see ra8_cnecc_test_mode_disable
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_cnecc_inject_fault(uint8_t instance, const ra8_cnecc_inject_t* req);

/**
 * @brief Force EC710TMC = 0x8000 to leave fault-injection mode.
 *
 * @details
 * Direct wrapper for the "Disable ECC test mode" step in Figure
 * 42.2 (HUM p 2875). Restores the decoder to use the live MBRAM
 * data instead of EC710TED. Per HUM 42.2.2 p 2872 ECDCS is
 * automatically cleared when ECTMCE is cleared, so the single
 * 16-bit write is enough to fully exit test mode.
 *
 * @param[in] instance Instance index 0..1.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Test mode disabled.
 * @retval k_ra8_err_invalid_arg ``instance`` out of range.
 *
 * @pre  ``ra8_cnecc_init`` has run.
 * @pre  Caller has masked IRQs or runs single-threaded.
 * @post Live ``EC710TMC.ECTMCE`` reads back as 0.
 * @post Live ``EC710TMC.ECDCS``  reads back as 0.
 *
 * @note Not thread-safe.
 * @see ra8_cnecc_inject_fault
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_cnecc_test_mode_disable(uint8_t instance);

/**
 * @brief Query whether fault injection is currently armed.
 *
 * @param[in]  instance Instance index 0..1.
 * @param[out] out      Non-NULL receiver -- set to ``true`` if
 *                      ``EC710TMC.ECTMCE`` is asserted.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Query succeeded.
 * @retval k_ra8_err_null_ptr    ``out`` was NULL.
 * @retval k_ra8_err_invalid_arg ``instance`` out of range.
 *
 * @pre  ``out`` points to writable storage.
 * @pre  ``instance`` is a valid index.
 * @post ``*out`` reflects the live ECTMCE bit.
 * @post Hardware state is unchanged.
 *
 * @note Read-only; safe to call from any context.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_cnecc_test_mode_active(uint8_t instance, bool* out);

/* =============================================================================
 * Async fault notification
 * =============================================================================
 */

/**
 * @brief Attach a callback for CNECC fault events.
 *
 * @param[in] fn  Non-NULL handler invoked from the ECC ISR (or test code).
 * @param[in] ctx Opaque context pointer forwarded to the handler.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok           Handler installed.
 * @retval k_ra8_err_null_ptr ``fn`` was NULL.
 *
 * @pre  ``ra8_cnecc_init`` has run.
 * @pre  Caller is in single-threaded install context (the install
 *       store is not interrupt-atomic; the dispatch read is).
 * @post Subsequent ``ra8_cnecc_dispatch`` calls fire ``fn``.
 * @post Calling with the same ``fn`` twice is idempotent.
 *
 * @note Install-time is not thread-safe; the dispatch read is.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_cnecc_attach_handler(ra8_cnecc_error_fn_t fn, void* ctx);

/**
 * @brief Wire the CANn_MRAM_ERI ICU vectors to the driver dispatcher.
 *
 * @details
 * Walks both ``CAN0_MRAM_ERI`` and ``CAN1_MRAM_ERI`` ELC events
 * (HUM 42.4 p 2875), allocates an IELSR slot for each via
 * ``ra8_isr_register``, and points them at ``ra8_cnecc_isr_handler``.
 * The handler reads the offending instance's ``EC710CTL`` to
 * decide whether the fault was 1-bit or 2-bit, samples ``EC710EAD0``
 * for the offset, calls the registered ``ra8_cnecc_error_fn_t``
 * callback, and then W0Cs the latched flags.
 *
 * If only one CANFD channel is in use the caller can set its
 * priority differently from the other; both vectors are wired in
 * one shot to keep the API simple. Detach via ``ra8_cnecc_detach_isr``.
 *
 * @param[in] priority NVIC priority shared by both vectors
 *                     (0..k_ra8_isr_prio_max).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Both vectors wired.
 * @retval k_ra8_err_invalid_arg   ``priority`` out of range.
 * @retval k_ra8_err_no_mem        No free IELSR slot.
 * @retval k_ra8_err_exists        One of the events was already routed.
 *
 * @pre  ``ra8_isr_init`` and ``ra8_cnecc_init`` have run.
 * @pre  Caller has registered a fault callback via
 *       ``ra8_cnecc_attach_handler`` (the dispatcher is harmless
 *       without one but the callback is the actual reason to wire).
 * @post Both ICU vectors enabled at the requested priority.
 * @post Subsequent ECC faults route automatically through
 *       ``ra8_cnecc_dispatch`` to the registered callback.
 *
 * @note Not thread-safe.
 * @see ra8_cnecc_detach_isr
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_cnecc_attach_isr(uint8_t priority);

/**
 * @brief Tear down the ICU vector wiring established by
 *        ``ra8_cnecc_attach_isr``.
 *
 * @details
 * Releases both ``CAN0_MRAM_ERI`` and ``CAN1_MRAM_ERI`` IELSR
 * slots. After this returns, MBRAM ECC faults still latch in
 * hardware but no callback fires until the next attach.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok            Both vectors released (or already gone).
 *
 * @pre  ``ra8_cnecc_attach_isr`` was previously called.
 * @pre  Caller has masked IRQs or runs single-threaded.
 * @post Neither CAN0/CAN1_MRAM_ERI ELC event maps to an IELSR slot.
 * @post Driver-local callback storage is preserved.
 *
 * @note Not thread-safe.
 * @see ra8_cnecc_attach_isr
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_cnecc_detach_isr(void);

/**
 * @brief Generic ICU trampoline for both CANn_MRAM_ERI vectors.
 *
 * @details
 * Shared between the two ICU slots set up by
 * ``ra8_cnecc_attach_isr``; ``ctx`` carries the instance index
 * cast to a ``void*``. Reads the instance's ``EC710CTL`` /
 * ``EC710EAD0``, decides between 1-bit, 2-bit, or pure-overflow,
 * forwards to ``ra8_cnecc_dispatch``, then W0Cs the latched flags
 * (HUM 42.3.1 figure 42.1 p 2874 closing steps "Clear ECC Error
 * flag" and "Clear Interrupt Request flag").
 *
 * @param[in] ctx Instance index packed in the low byte of the
 *                ``void*``. Out-of-range values are silently dropped.
 *
 * @pre  Called from Cortex-M85 handler mode (the ICU dispatcher
 *       invokes it).
 * @pre  ``ra8_cnecc_init`` has run.
 * @post Latched ECER1F / ECER2F / ECOVFF / ECSEDF0 / ECDEDF0 are
 *       cleared for the instance.
 * @post The ICU IELSR.IR bit is cleared by the surrounding
 *       ``ra8_isr_dispatch``.
 *
 * @note Re-entrant.
 * @see ra8_cnecc_attach_isr
 * @since 0.1.0
 *
 */
void ra8_cnecc_isr_handler(void* ctx);

/**
 * @brief Dispatch a CNECC fault event to the installed handler.
 *
 * @details
 * Drives the registered ``ra8_cnecc_error_fn_t`` callback and bumps
 * the matching cumulative SEC / DED counter for the instance.
 * Direct entry point for unit tests and integrators that want to
 * drive the dispatcher manually instead of going through
 * ``ra8_cnecc_isr_handler``.
 *
 * @param[in] instance  Instance index 0..1 (out-of-range silently ignored).
 * @param[in] is_2bit   ``true`` for uncorrectable, ``false`` for 1-bit.
 * @param[in] err_addr  Faulting RAM offset (``ECEAD[9:0]``).
 *
 * @pre  ``instance < k_ra8_cnecc_instance_count`` OR the call is dropped.
 * @pre  Caller is the ISR or a test harness mimicking it.
 * @post If a handler is registered AND ``instance`` is valid, the
 *       handler runs to completion before this returns.
 * @post The matching cumulative counter for ``instance`` is bumped
 *       by 1 (also in the BBR mirror if attached).
 *
 * @note Safe to call from NMI/ISR context provided the registered
 *       handler is itself NMI-safe.
 * @since 0.1.0
 *
 */
void ra8_cnecc_dispatch(uint8_t instance, bool is_2bit, uint16_t err_addr);

/**
 * @brief Force-bump the overflow counter (used by the ISR trampoline).
 *
 * @details
 * Called by ``ra8_cnecc_isr_handler`` when ``ECOVFF`` is set so the
 * software counters remain in sync with the hardware overflow flag
 * even when the underlying SEC / DED bit was already latched.
 *
 * @param[in] instance Instance index 0..1 (out-of-range silently ignored).
 *
 * @pre  ``instance < k_ra8_cnecc_instance_count`` OR the call is dropped.
 * @pre  Caller is the ISR or a test harness mimicking it.
 * @post The overflow counter for ``instance`` (and BBR mirror) is
 *       bumped by 1.
 * @post No callback is invoked; only the counter changes.
 *
 * @note Re-entrant.
 * @since 0.1.0
 *
 */
void ra8_cnecc_dispatch_overflow(uint8_t instance);

/* =============================================================================
 * Code-flash ECC compute / verify (software fallback)
 * =============================================================================
 */

/**
 * @brief One-shot CNECC bring-up using the driver's default config.
 *
 * @details
 * Convenience wrapper that calls ``ra8_cnecc_init`` with both instances
 * configured to enable correction, irq_1bit, irq_2bit and judgment.
 * Used by the boot-time fault monitor that just wants the ECC active
 * without having to spell out a config struct.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                  Both instances live with defaults.
 * @retval k_ra8_err_hw_init_failed  MSTP enable failed.
 *
 * @pre  ``ra8_mstp_init`` has run.
 * @pre  Caller is in single-threaded init context.
 * @post Both instances' ECERVF / EC1EDIC / EC2EDIC are set.
 * @post Both instances' EC1ECP is cleared (correction enabled).
 *
 * @note Not thread-safe.
 * @see ra8_cnecc_init
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_cnecc_open(void);

/**
 * @brief Compute a 32-bit ECC code over a (addr, len) memory region.
 *
 * @details
 * Software fall-back used by the bootloader's anti-rollback path to
 * derive an ECC tag for an MRAM / flash range when the hardware ECC
 * does not expose a "compute" engine for arbitrary addresses (HUM
 * Ch 42 only describes runtime read-side checking). The algorithm is
 * a deterministic word-wise CRC32-style accumulator (polynomial
 * ``0xEDB88320``) seeded with ``0xFFFFFFFF`` and finalised by XOR
 * with ``0xFFFFFFFF``. ``len`` is rounded down to the nearest 4-byte
 * boundary; trailing bytes (0..3) are NOT included to keep the
 * computation deterministic and word-aligned.
 *
 * The result is suitable for "did this region change since boot"
 * style checks; it is NOT a Hamming-style ECC and cannot correct
 * single-bit faults. The hardware ECC behind ``EC710CTL`` remains
 * the single point of truth for live read traffic.
 *
 * @param[in]  addr    Base address (must be 4-byte aligned).
 * @param[in]  len     Region length in bytes (rounded down to /4).
 * @param[out] out_ecc Receives the 32-bit ECC tag.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              ECC computed.
 * @retval k_ra8_err_null_ptr    ``addr`` was 0 OR ``out_ecc`` was NULL.
 * @retval k_ra8_err_invalid_arg ``addr`` not 4-byte aligned OR ``len < 4``.
 *
 * @pre  ``addr`` is non-zero and 4-byte aligned.
 * @pre  ``out_ecc`` points to writable storage.
 * @post On success, ``*out_ecc`` is the deterministic CRC32 of the
 *       word-aligned portion of [addr, addr + len).
 * @post Hardware state is unchanged.
 *
 * @note Read-only path; safe to call from any context provided the
 *       memory at ``addr`` is mapped.
 * @see ra8_cnecc_verify
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_cnecc_compute(uint32_t addr, uint32_t len, uint32_t* out_ecc);

/**
 * @brief Verify a region's ECC tag matches an expected value.
 *
 * @details
 * Computes the CRC32 over ``[addr, addr + len)`` via ``ra8_cnecc_compute``
 * and compares against ``expected_ecc``. Returns ``k_ra8_err_crc_mismatch`` on
 * mismatch so callers can route the failure into the same telemetry
 * path as a hardware ECC fault.
 *
 * @param[in] addr         Base address (must be 4-byte aligned).
 * @param[in] len          Region length in bytes (rounded down to /4).
 * @param[in] expected_ecc Expected ECC tag.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Computed value matched.
 * @retval k_ra8_err_crc_mismatch Computed value did NOT match expected.
 * @retval k_ra8_err_invalid_arg ``addr`` not 4-byte aligned OR ``len < 4``.
 *
 * @pre  ``addr`` is non-zero and 4-byte aligned.
 * @pre  ``len`` is at least 4 bytes.
 * @post Hardware state is unchanged.
 * @post On success the verification result is logged at INFO level;
 *       on mismatch it is logged at ERROR level.
 *
 * @note Read-only path.
 * @see ra8_cnecc_compute
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_cnecc_verify(uint32_t addr, uint32_t len, uint32_t expected_ecc);

#ifdef __cplusplus
}
#endif
