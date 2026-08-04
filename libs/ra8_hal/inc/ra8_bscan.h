/**
 * @file ra8_bscan.h
 * @brief JTAG / IEEE-1149.1 Boundary Scan TAP HAL surface
 * @ingroup grp_hal_system
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Thin software wrapper over the RA8D2 boundary-scan TAP described in
 * HUM Ch 50 "Boundary Scan", p 3257-3262. The actual scan vectors
 * (EXTEST / SAMPLE-PRELOAD / CLAMP / HIGHZ / BYPASS / IDCODE) are
 * driven over the four JTAG pins (TCK / TMS / TDI / TDO) by an
 * external manufacturing-test fixture while the device is held in
 * reset. None of the four TAP registers (JTIR, JTIDR, JTBPR, JTBSR)
 * is reachable through the AHB / system bus, so the firmware-side
 * "driver" is a small bookkeeping object rather than a normal
 * register-poking peripheral.
 *
 * What this module does:
 *  - ``ra8_bscan_init()`` -- record the expected JTIDR value and mark
 *    the TAP-state object as initialized so the host-side test rig
 *    has a place to stash its "current instruction" hint.
 *  - ``ra8_bscan_deinit()`` -- forget all state.
 *  - ``ra8_bscan_get_idcode()`` -- return the manufacturer ID code
 *    (``0x085D_A447`` per HUM Ch 50.2.2, p 3258) so a host-side
 *    sanity check can confirm it matches what was scanned out over
 *    TDO. This is read from a constant; there is no hardware register
 *    to fetch.
 *  - ``ra8_bscan_get_status()`` -- report the recorded TAP instruction
 *    plus initialisation state.
 *  - ``ra8_bscan_set_instruction()`` -- record (NOT actually load) the
 *    JTIR opcode the external fixture is currently driving so other
 *    firmware modules (e.g. a logging task) can reflect it.
 *
 * @warning Boundary scan can only be exercised while the RES pin is
 *          held low (HUM Ch 50.1 Table 50.1, p 3257). Issuing the
 *          EXTEST / CLAMP / HIGHZ instructions on a live system
 *          disconnects every general-purpose I/O pin from its normal
 *          peripheral function and drives it from the JTBSR shift
 *          register. Do not call this driver from a running
 *          application path -- it is intended for power-on
 *          self-test / production-test contexts only.
 *
 * @warning HUM Ch 50.1 (p 3257) also notes the device does not
 *          implement the optional TRST pin, so the TAP can only be
 *          reset by holding TMS=1 for at least 5 TCK clocks. The
 *          firmware cannot do this itself; the external test fixture
 *          must.
 *
 * ## Not yet implemented
 *
 * - Loading or reading the JTBSR scan chain (cannot be done from the
 *   CPU; HUM Ch 50.2 p 3258).
 * - Driving the TAP state machine (Test-Logic-Reset -> Run-Test/Idle
 *   -> Select-DR -> ... per HUM Figure 50.2 p 3260) -- this is the
 *   external fixture's responsibility.
 * - BSDL parsing -- handled in host tooling, not on the MCU.
 *
 * ## Validation
 *
 * The CPU-side contract (IDCODE constant cross-check, opcode validation,
 * lifecycle, NULL guards) is exercised by the host unit tests in
 * ``tests/test_ra8_bscan.c`` and by the on-silicon self-test example
 * ``examples/.../hil/bscan_selftest`` (prints
 * ``bscan: idcode=085DA447 checks=17 PASS``). The actual boundary-scan
 * vectors are validated externally by an IEEE-1149.1 JTAG fixture using the
 * device's BSDL file -- there is no firmware example for the scan chain
 * itself because it is not CPU-reachable (HUM Ch 50.2.3 p 3259).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_bscan_regs.h"
#include "ra8_err.h"

/**
 * @struct ra8_bscan_status_t
 * @brief Snapshot of the firmware-side TAP bookkeeping object.
 *
 * @details
 * Returned by ``ra8_bscan_get_status``. Reflects only what firmware
 * has been told -- the actual hardware TAP state is not observable
 * from the CPU.
 */
typedef struct {
  bool              initialized;      /**< True once ``ra8_bscan_init`` ran.         */
  ra8_bscan_instr_t last_instruction; /**< Last JTIR opcode reported by the fixture. */
  uint32_t          expected_idcode;  /**< JTIDR value the fixture should observe.   */
} ra8_bscan_status_t;

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Initialise the firmware-side TAP bookkeeping object.
 *
 * @details
 * Marks the driver as initialized and seeds the expected JTIDR with
 * the chip's hardwired device ID code (``0x085D_A447``, HUM Ch 50.2.2
 * Table 50.3 entry, p 3258). Touches no hardware: the boundary-scan
 * TAP is reachable only over the external JTAG pins.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok Always succeeds.
 *
 * @pre Single-threaded init context.
 * @pre Caller has not already called ``ra8_bscan_init`` since the last
 *      ``ra8_bscan_deinit`` (re-init is allowed and is a no-op).
 * @post ``ra8_bscan_get_status``->initialized == true.
 * @post ``ra8_bscan_get_status``->expected_idcode == 0x085DA447.
 *
 * @note Not thread-safe.
 *
 * @see ra8_bscan_deinit
 * @see ra8_bscan_get_idcode
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_bscan_init(void);

/**
 * @brief Tear down the bookkeeping object.
 *
 * @details
 * Clears the recorded instruction and idcode and marks the driver as
 * uninitialized. Touches no hardware.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok Always succeeds.
 *
 * @pre Driver may be in any state (deinit is idempotent).
 * @pre Single-threaded shutdown context.
 * @post ``ra8_bscan_get_status``->initialized == false.
 * @post ``ra8_bscan_get_status``->last_instruction == BYPASS.
 *
 * @note Not thread-safe.
 *
 * @see ra8_bscan_init
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_bscan_deinit(void);

/* =============================================================================
 * Read-out API
 * =============================================================================
 */

/**
 * @brief Get the chip's hardwired JTIDR device ID code.
 *
 * @details
 * Returns the constant value the external fixture should observe when
 * it shifts JTIDR out over TDO after issuing the IDCODE instruction
 * (per HUM Ch 50.3.2 (4) IDCODE description, p 3261). The value is
 * sourced from the regs header and does NOT require hardware access.
 *
 * @param[out] out Receives the 32-bit JTIDR value.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok          Success, ``*out`` is valid.
 * @retval k_ra8_err_null_ptr ``out`` is NULL.
 * @retval k_ra8_err_not_initialized ``ra8_bscan_init`` has not been called.
 *
 * @pre ``ra8_bscan_init`` has been called.
 * @pre ``out`` is a writable ``uint32_t``.
 * @post On success ``*out == 0x085DA447``.
 * @post Driver state unchanged.
 *
 * @note Thread safety: re-entrant (read-only access to a constant).
 * @see ra8_bscan_init
 * @see k_ra8_bscan_jtidr_reset
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_bscan_get_idcode(uint32_t* out);

/**
 * @brief Get a snapshot of the driver's TAP bookkeeping state.
 *
 * @details
 * Returns the firmware-side view (initialized flag, last reported
 * JTIR opcode, expected JTIDR). Does not query hardware -- the four
 * TAP registers (JTIR / JTIDR / JTBPR / JTBSR) are not CPU-readable
 * (HUM Ch 50.2.3 explicit note, p 3259).
 *
 * @param[out] out Receives the status snapshot.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok           Success.
 * @retval k_ra8_err_null_ptr ``out`` is NULL.
 *
 * @pre ``out`` is a writable ``ra8_bscan_status_t`` object.
 * @pre Single-threaded read or caller serialises access.
 * @post Driver state unchanged.
 * @post ``*out`` reflects the most recent calls to ``ra8_bscan_init``,
 *       ``ra8_bscan_deinit``, and ``ra8_bscan_set_instruction``.
 *
 * @note Thread safety: not thread-safe relative to ``set_instruction``.
 * @see ra8_bscan_set_instruction
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_bscan_get_status(ra8_bscan_status_t* out);

/**
 * @brief Clear the recorded last-instruction state (no hardware effect).
 *
 * @details
 * Resets the bookkeeping ``last_instruction`` field back to BYPASS
 * (which is the TAP's reset-state instruction per HUM Ch 50.2.1,
 * p 3258 -- the JTIR reset value 0xE selects the BYPASS path). Used
 * by the host-side fixture when it has finished a scan run and wants
 * the firmware-side log to stop reporting the previous opcode.
 *
 * @param[in] mask Reserved -- must be 0. Present to match the
 *                 ``ra8_<short>_clear_status(uint32_t)`` shape used by
 *                 sibling HAL drivers.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok                Success.
 * @retval k_ra8_err_invalid_arg   ``mask`` is non-zero.
 * @retval k_ra8_err_not_initialized ``ra8_bscan_init`` not called.
 *
 * @pre ``ra8_bscan_init`` has been called.
 * @pre ``mask`` is 0.
 * @post Recorded instruction == BYPASS.
 * @post Initialized flag unchanged.
 *
 * @note Not thread-safe.
 * @see ra8_bscan_set_instruction
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_bscan_clear_status(uint32_t mask);

/* =============================================================================
 * Bookkeeping API
 * =============================================================================
 */

/**
 * @brief Record the JTIR opcode the external fixture is currently driving.
 *
 * @details
 * Pure firmware-side bookkeeping. The opcode is validated against the
 * named instruction set in HUM Ch 50.2.1 Table (p 3258); reserved
 * 4-bit codes are rejected. The TAP itself does not see this call --
 * the actual JTIR load happens via Shift-IR on the JTAG pins from the
 * external fixture.
 *
 * @param[in] instr One of the named opcodes in ``ra8_bscan_instr_t``.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok                Recorded.
 * @retval k_ra8_err_invalid_arg   ``instr`` is a reserved opcode.
 * @retval k_ra8_err_not_initialized ``ra8_bscan_init`` not called.
 *
 * @pre ``ra8_bscan_init`` has been called.
 * @pre ``instr`` is one of EXTEST / SAMPLE_PRELOAD / IDCODE / CLAMP /
 *      HIGHZ / BYPASS.
 * @post ``ra8_bscan_get_status``->last_instruction == ``instr``.
 * @post Initialized flag unchanged.
 *
 * @note Not thread-safe.
 * @warning EXTEST / CLAMP / HIGHZ disconnect every general-purpose
 *          I/O from its normal peripheral function. Recording one of
 *          these opcodes here does not cause that effect, but the
 *          fixture loading the same opcode over TDI will.
 *
 * @see ra8_bscan_get_status
 * @see ra8_bscan_clear_status
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_bscan_set_instruction(ra8_bscan_instr_t instr);

#ifdef __cplusplus
}
#endif
