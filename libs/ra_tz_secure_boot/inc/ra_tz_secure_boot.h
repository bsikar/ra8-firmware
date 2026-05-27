/**
 * @file ra_tz_secure_boot.h
 * @brief FSP-style TrustZone secure-boot for the Cortex-M85 (CPU0)
 *
 * @par Tag
 * [Ring 1 / Boot] {World: S}
 *
 * @details
 * Mirrors the FSP ``R_BSP_SAUInit`` + ``R_BSP_SecurityInit`` +
 * ``R_BSP_NonSecureEnter`` triplet without copying any FSP code. The
 * entry point is ``ra_tz_secure_boot_run``, which is called from the
 * per-app ``trustzone_init.c`` when ``RA_TRUSTZONE_ENABLE`` is on.
 *
 * The boot sequence is:
 *   1. Programme the SAU with the canonical RA8D2 partition (see
 *      ``ra_tz_sau_region_t`` for the layout).
 *   2. Enable the SAU with ALLNS = 0 (default-deny).
 *   3. Unlock CPSCU writes via ``PRCR_S.PRC4`` (HUM Ch 9.2.4 -- write
 *      key 0xA510 to address 0x4001E3FA).
 *   4. Write IPCSAR so the channels CPU1 (always-NS on this chip) must
 *      reach are tagged NS in the per-channel SAIPCIRn bits.
 *   5. Re-lock PRCR_S.
 *   6. Configure the NS main stack pointer via MSP_NS and jump to the
 *      NS reset vector with ``BLXNS``.
 *
 * On a host (``RA_SIMULATOR_MODE`` defined) the module exposes a pure-C
 * implementation that writes to a mocked CPSCU window, so the unit
 * tests under ``tests/`` can drive the IPCSAR-unlock sequence without
 * touching real hardware.
 *
 * @par TrustZone Safety:
 *  - **Validates:** SAU_TYPE.SREGION reports >= 5 regions before
 *    programming the partition (the FSP-style layout needs 5).
 *  - **Validates:** ``ns_vector_table`` is non-NULL and word-aligned.
 *  - **Trusts:** the boot ROM left the SAU disabled and the IDAU in
 *    its documented reset state (bit-28 split).
 *  - **Denies:** any return path from ``ra_tz_secure_boot_run`` -- the
 *    function is ``[[noreturn]]`` once the BLXNS branches out.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra_attributes.h"
#include "ra_err.h"

/**
 * @enum ra_tz_sau_region_t
 * @brief Canonical SAU region indices for the RA8D2 secure-boot layout.
 *
 * @details
 * The layout deliberately mirrors FSP's ``R_BSP_SAUInit`` -- five
 * regions are programmed and the SAU is enabled with ALLNS = 0. Region
 * 3 (NSC veneer) points at the unused 0x10000000 IDAU alias rather
 * than at the actual ``.gnu.sgstubs`` placement inside MRAM, because
 * the latter has been bench-verified to brick the chip (see
 * project memory ``project_sau_sgstubs_brick``). With ALLNS = 0 and
 * Region 3 pointing at an unused alias, callers must not emit any
 * Non-Secure entries (``cmse_nonsecure_entry``) until a follow-up
 * change moves the veneers into their own non-S-executable region.
 *
 * @code
 * Region 0  Code-Flash NSC alias  0x10000000..0x100FFFE0  NSC
 * Region 1  NS upper MRAM         0x02080000..0x020FFFE0  NS
 * Region 2  NSC SRAM alias        0x12000000..0x1200FFE0  NSC
 * Region 3  NS upper SRAM         0x22100000..0x221FFFE0  NS
 * Region 4  NS peripheral window  0x50000000..0x5FFFFFE0  NS
 * @endcode
 */
typedef enum : uint8_t {
  k_ra_tz_sau_region_code_nsc  = 0U, /**< NSC alias for code-flash veneers.  */
  k_ra_tz_sau_region_ns_mram   = 1U, /**< Non-Secure upper MRAM (NS image).  */
  k_ra_tz_sau_region_sram_nsc  = 2U, /**< NSC alias for SRAM veneers.        */
  k_ra_tz_sau_region_ns_sram   = 3U, /**< Non-Secure upper SRAM (NS data).   */
  k_ra_tz_sau_region_ns_periph = 4U, /**< Non-Secure peripheral window.     */
  k_ra_tz_sau_region_count     = 5U, /**< Number of programmed regions.      */
} ra_tz_sau_region_t;

/**
 * @enum ra_tz_secure_boot_step_t
 * @brief Progress markers stamped into ``g_ra_tz_secure_boot_step``.
 *
 * @details
 * Mirrors the diagnostic counters in the existing cpu1_pingpong app --
 * each step is set in-order as the secure boot makes forward progress,
 * so a J-Link memprobe can pinpoint exactly where bring-up wedged if
 * the BLXNS never fires.
 */
typedef enum : uint8_t {
  k_ra_tz_secure_boot_step_idle           = 0U, /**< Pre-run sentinel.       */
  k_ra_tz_secure_boot_step_sau_done       = 1U, /**< SAU regions programmed. */
  k_ra_tz_secure_boot_step_prcr_unlocked  = 2U, /**< PRCR_S.PRC4 set.        */
  k_ra_tz_secure_boot_step_ipcsar_written = 3U, /**< IPCSAR write landed.    */
  k_ra_tz_secure_boot_step_prcr_relocked  = 4U, /**< PRCR_S.PRC4 cleared.    */
  k_ra_tz_secure_boot_step_blxns_armed    = 5U, /**< MSP_NS / VTOR set up.   */
  k_ra_tz_secure_boot_step_branched       = 6U, /**< BLXNS executed (host).  */
} ra_tz_secure_boot_step_t;

/**
 * @brief Programme the SAU regions documented in ``ra_tz_sau_region_t``.
 *
 * @details
 * Equivalent to the FSP ``R_BSP_SAUInit`` function: writes each
 * region's RBAR / RLAR, then enables the SAU with ALLNS = 0. Does NOT
 * touch CPSCU or jump to the NS image -- callers issue
 * ``ra_tz_secure_boot_security_init`` and ``ra_tz_secure_boot_jump_ns``
 * separately so unit tests can exercise each phase in isolation.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                 SAU programmed and enabled.
 * @retval k_ra_err_not_supported  SAU_TYPE.SREGION < 5.
 *
 * @pre Caller is in Secure state (the SAU window lives at 0xE000EDD0
 *      which is secure-only).
 * @pre This function has not been called previously in the current
 *      boot (SAU programming is one-shot per reset).
 *
 * @post On success, SAU_CTRL.ENABLE = 1 and the five regions cover
 *       the canonical layout.
 * @post On failure, the SAU stays disabled (default-allow Secure).
 *
 * @note Not thread-safe; runs once at boot.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_tz_secure_boot_sau_init(void);

/**
 * @brief Unlock PRCR_S.PRC4 and write IPCSAR + IPCPAR.
 *
 * @details
 * Writes ``ipcsar_value`` to CPSCU.IPCSAR and ``ipcpar_value`` to
 * CPSCU.IPCPAR after first opening the PRCR_S.PRC4 write-protect gate
 * (HUM Ch 9.2.4 "PRCR_S" -- key 0xA510 at address 0x4001E3FA). The
 * gate is closed again before the function returns.
 *
 * For the cpu1_pingpong_ipc app the canonical value is
 * ``ipcsar_value = 0x00050000``, which sets SAIPCIR0 (IPC0 channel 0,
 * CPU1 -> CPU0) and SAIPCIR2 (IPC1 channel 0, CPU0 -> CPU1) -- the
 * two channels CPU1 (always-NS) must reach. ``ipcpar_value`` stays at
 * 0 so the channels remain Privileged-only.
 *
 * @param[in] ipcsar_value Value to write into ``CPSCU.IPCSAR``.
 * @param[in] ipcpar_value Value to write into ``CPSCU.IPCPAR``.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok Always (the unlock + write + relock sequence cannot
 *                 fail in software; bench bring-up may still observe
 *                 read-back mismatches if the chip's CPSCU is in an
 *                 unexpected state).
 *
 * @pre Caller is in Secure state (CPSCU and PRCR_S are secure-only).
 * @pre Caller has already programmed the SAU partition.
 *
 * @post IPCSAR contains ``ipcsar_value`` (verifiable via JTAG read).
 * @post PRCR_S.PRC4 is cleared (write-protect restored).
 *
 * @note Not thread-safe; runs once at boot.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_tz_secure_boot_security_init(uint32_t ipcsar_value,
                                                       uint32_t ipcpar_value);

/**
 * @brief Switch to NS state and jump to the NS image's reset vector.
 *
 * @details
 * Reads the initial-SP slot of the NS vector table into MSP_NS, sets
 * VTOR_NS to ``ns_vector_table``, then performs a ``BLXNS`` to the
 * reset-vector slot. The function is declared ``[[noreturn]]`` and
 * does not return on hardware.
 *
 * On the host (``RA_SIMULATOR_MODE`` defined) the function stamps the
 * progress counter, sets ``s_ra_tz_secure_boot_blxns_target`` to the
 * supplied reset vector, and returns ``k_ra_ok`` so unit tests can
 * assert the documented transition state.
 *
 * @param[in] ns_vector_table Pointer to the NS image's vector table.
 *
 * @return ra_err_t On host only -- never reached on target.
 * @retval k_ra_ok                On host: BLXNS state captured.
 * @retval k_ra_err_null_ptr      ``ns_vector_table`` is NULL.
 * @retval k_ra_err_invalid_arg   ``ns_vector_table`` is not 4-byte
 *                                aligned, or the reset-vector slot
 *                                holds an obviously bogus value
 *                                (``0`` or ``0xFFFFFFFF``).
 *
 * @pre ``ns_vector_table`` is non-NULL and 4-byte aligned.
 * @pre ``ns_vector_table[0]`` (initial SP) and ``ns_vector_table[1]``
 *      (reset vector) hold valid values for the NS image.
 *
 * @post On hardware: control transfers to the NS image and never
 *       returns to the caller.
 * @post On host: ``s_ra_tz_secure_boot_blxns_target`` captures the NS
 *       reset vector for test inspection.
 *
 * @note Not thread-safe; runs once at boot from a single-threaded
 *       context.
 * @since 0.1.0
 */
ra_err_t ra_tz_secure_boot_jump_ns(const uint32_t* ns_vector_table);

/**
 * @brief Run the full secure-boot sequence and (on target) BLXNS.
 *
 * @details
 * Convenience wrapper: ``ra_tz_secure_boot_sau_init`` ->
 * ``ra_tz_secure_boot_security_init`` -> ``ra_tz_secure_boot_jump_ns``,
 * returning the first non-OK status.
 *
 * @param[in] ipcsar_value     Forwarded to security_init.
 * @param[in] ipcpar_value     Forwarded to security_init.
 * @param[in] ns_vector_table  Forwarded to jump_ns.
 *
 * @return ra_err_t First failing step's status; never returns on target
 *                  in the happy path.
 * @retval k_ra_ok                On host happy path (BLXNS captured).
 * @retval k_ra_err_null_ptr      ``ns_vector_table`` is NULL.
 * @retval k_ra_err_not_supported SAU_TYPE.SREGION < 5.
 * @retval k_ra_err_invalid_arg   NS vector table mis-aligned / invalid.
 *
 * @pre Secure state, single-threaded boot context.
 * @pre ``ns_vector_table`` non-NULL.
 *
 * @post On hardware happy path: NS image running, no return.
 * @post On any failure step: SAU left in whatever partial state the
 *       failing call established; caller logs and halts.
 *
 * @note Not thread-safe; runs once at boot.
 * @since 0.1.0
 */
ra_err_t ra_tz_secure_boot_run(uint32_t        ipcsar_value,
                               uint32_t        ipcpar_value,
                               const uint32_t* ns_vector_table);

/**
 * @brief Reset the host-side state for unit-test fixtures.
 *
 * @details
 * Host-only helper that clears the simulated PRCR_S / IPCSAR / IPCPAR
 * captures and the BLXNS target so each test starts from a known
 * baseline. Cross-compiled (target) builds expose the symbol but the
 * body is empty.
 *
 * @pre None.
 * @pre None.
 * @post Host state reset to ``k_ra_tz_secure_boot_step_idle``.
 * @post BLXNS target capture cleared.
 *
 * @note Test-only on host; no-op on target.
 * @since 0.1.0
 */
RA_TEST_HELPER void ra_tz_secure_boot_host_reset(void);

/**
 * @brief Read the boot progress counter (debug / test).
 *
 * @details
 * Returns the most-recent ``ra_tz_secure_boot_step_t`` value stored in
 * the internal progress counter. Useful for J-Link memprobe scripts on
 * the bench and for unit-test assertions on the host.
 *
 * @return Current progress step.
 * @retval k_ra_tz_secure_boot_step_idle           Pre-run.
 * @retval k_ra_tz_secure_boot_step_sau_done       SAU init complete.
 * @retval k_ra_tz_secure_boot_step_ipcsar_written IPCSAR landed.
 * @retval k_ra_tz_secure_boot_step_branched       BLXNS executed.
 *
 * @pre None.
 * @pre None.
 * @post No state change.
 * @post Caller-visible state matches the most recent boot step.
 *
 * @note Thread-safe (single 32-bit load).
 * @since 0.1.0
 */
ra_tz_secure_boot_step_t ra_tz_secure_boot_get_step(void);

/**
 * @brief Read the captured BLXNS target (host / test).
 *
 * @details
 * Host-only accessor: returns the NS reset vector that
 * ``ra_tz_secure_boot_jump_ns`` was last asked to branch to. On target
 * builds this returns 0 because BLXNS does not return.
 *
 * @return Captured reset-vector address, or 0 if not yet armed.
 * @retval 0          BLXNS not yet armed (or running on target).
 * @retval non-zero   NS reset vector that was loaded for BLXNS.
 *
 * @pre None.
 * @pre None.
 * @post No state change.
 * @post Caller-visible state matches the most recent BLXNS arm.
 *
 * @note Test-only on host; returns 0 on target.
 * @since 0.1.0
 */
RA_TEST_HELPER uint32_t ra_tz_secure_boot_host_blxns_target(void);

#ifdef __cplusplus
}
#endif
