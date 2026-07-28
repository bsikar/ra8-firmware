/**
 * @file ra8_dfu_antirollback.h
 * @brief DFU anti-rollback (downgrade protection) policy + storage seam.
 * @ingroup grp_security
 *
 * @par Tag
 * [Ring 4 / Service] {World: S}
 *
 * @details
 * Anti-rollback prevents an attacker re-flashing an OLDER but still
 * correctly-signed image -- one that may carry known, since-patched
 * vulnerabilities. It is the second half of the copy-to-run trust gate:
 * ``ra8_rot_verify_image`` (``ra8_rot.h``) proves an image is *authentic*; this
 * module proves the authentic image is not a *downgrade*.
 *
 * ## Policy
 *
 * Each signed image's ``ra8_rot_trailer_t`` carries a monotonic ``img_version``.
 * The device keeps the highest version it has ever accepted in a non-volatile
 * monotonic counter. The launch path:
 *
 *  1. reads the stored minimum version through the injected store;
 *  2. accepts iff ``img_version >= stored_min_version`` (equal is the
 *     re-flash-same-version case and is allowed); and
 *  3. on accept, advances the stored counter to the new version.
 *
 * Any failure -- a null store, a failed read, or (per the default-deny
 * contract) a failed commit -- denies the launch.
 *
 * ## Storage dependency-injection seam
 *
 * The non-volatile counter is reached only through a
 * ::ra8_rot_antirollback_store_t vtable (NASA Rule 9 deviation: function
 * pointers enable Dependency Inversion and host mock injection). Host unit
 * tests inject a mock counter; production wires the real backing once it
 * exists. ::ra8_rot_antirollback_default_store returns a non-faking stub whose
 * read/commit report "not provisioned" so that, until a real counter is wired,
 * the gate DEFAULT-DENIES rather than silently passes.
 *
 * ## Enabling (opt-in, default OFF)
 *
 * Gated behind ``RA8_ENABLE_ROOT_OF_TRUST`` (default OFF), the same flag as the
 * root of trust. With the flag OFF the implementation in
 * ``ra8_dfu_antirollback.c`` compiles to nothing (an empty translation unit,
 * mirroring ``ra8_rot.c``) and the launch path does not check versions, so
 * existing apps are byte-for-byte unchanged. The declarations below are always
 * visible and reference no external symbol, so a flag-off translation unit
 * gains no link dependency.
 *
 * @note The trailer ``img_version`` IS covered by the image signature: the ECDSA
 *       signature authenticates ``SHA-256`` of the little-endian ``img_version``
 *       concatenated with the body digest, rather than the bare body digest (see
 *       ``ra8_rot_verify_image`` /
 *       ``internal_bind_version`` in ``ra8_rot.c``, matched by the signer
 *       ``tools/rot_sign.py``). An attacker holding an older validly-signed image
 *       therefore cannot raise the trailer version to defeat this check -- the
 *       forged version invalidates the signature (T5-05).
 * @warning The downgrade check is only as strong as the durable counter store,
 *          which is provisioning-gated: on a fresh device the extra-MRAM counter
 *          word is blank and cannot be programmed at runtime on this silicon (no
 *          BlankCheck -- #194), so the first authentic image sets no floor and
 *          anti-rollback begins enforcing only once the counter is provisioned
 *          (see ``internal_default_store_commit`` in ``ra8_dfu_antirollback.c``).
 *          Provisioning that initial counter word is bench-gated.
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

/**
 * @typedef ra8_rot_antirollback_read_fn_t
 * @brief Read the stored highest-accepted image version (DI seam).
 *
 * @details
 * Reads the device's non-volatile monotonic anti-rollback counter -- the
 * highest image version ever accepted. The production implementation reads an
 * OTP / data-flash counter; the host test implementation reads a mock variable.
 * Returning any non-``k_ra8_ok`` code causes the verifier to DEFAULT-DENY.
 *
 * @param[out] out_min_version Receives the stored minimum version; non-NULL.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           ``*out_min_version`` holds the stored counter.
 * @retval k_ra8_err_null_ptr ``out_min_version`` is NULL.
 * @retval other             Backing-store read fault (verifier default-denies).
 *
 * @note Thread safety is the implementation's responsibility; the boot-path
 *       caller is single-threaded.
 * @since 0.1.0
 */
typedef ra8_err_t (*ra8_rot_antirollback_read_fn_t)(uint32_t* out_min_version);

/**
 * @typedef ra8_rot_antirollback_commit_fn_t
 * @brief Durably advance the stored highest-accepted image version (DI seam).
 *
 * @details
 * Writes ``new_version`` to the device's non-volatile monotonic anti-rollback
 * counter after an image has been accepted, so future downgrades below this
 * version are rejected. The production implementation programs an OTP /
 * data-flash counter; the host test implementation records a mock variable.
 *
 * @param[in] new_version The just-accepted image version to persist.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Counter advanced (or already at/above ``new_version``).
 * @retval other   Backing-store program fault (verifier default-denies).
 *
 * @note Thread safety is the implementation's responsibility; the boot-path
 *       caller is single-threaded.
 * @since 0.1.0
 */
typedef ra8_err_t (*ra8_rot_antirollback_commit_fn_t)(uint32_t new_version);

/**
 * @struct ra8_rot_antirollback_store_t
 * @brief Dependency-injection vtable for the non-volatile version counter.
 *
 * @details
 * The single seam between the pure anti-rollback policy and the durable
 * storage that backs the monotonic counter. Production wires the real OTP /
 * data-flash accessors; host tests wire mocks. Both function pointers must be
 * non-NULL for ::ra8_rot_antirollback_verify to proceed -- a NULL member is a
 * default-deny.
 *
 * @invariant ``read`` and ``commit`` are either both wired to a real backing
 *            or both report "not provisioned" (never one of each).
 *
 * @par Example:
 * @code
 * const ra8_rot_antirollback_store_t* store = ra8_rot_antirollback_default_store();
 * ra8_err_t err = ra8_rot_antirollback_verify(store, image_version);
 * @endcode
 *
 * @see ra8_rot_antirollback_verify
 * @see ra8_rot_antirollback_default_store
 */
typedef struct {
  ra8_rot_antirollback_read_fn_t   read;   /**< Read stored highest-accepted version. */
  ra8_rot_antirollback_commit_fn_t commit; /**< Persist the newly-accepted version.   */
} ra8_rot_antirollback_store_t;

/**
 * @brief Pure downgrade policy: accept iff the image is not older than stored.
 *
 * @details
 * The side-effect-free core of anti-rollback. An image is accepted when its
 * version is greater than or equal to the stored minimum (equal allows a
 * same-version re-flash); a strictly lower version is a downgrade and is
 * rejected. No storage is touched -- callers read the stored minimum first
 * (see ::ra8_rot_antirollback_verify) and default-deny on any read failure
 * before reaching this comparison.
 *
 * @param[in] image_version       Version recorded in the candidate image trailer.
 * @param[in] stored_min_version  Highest version the device has ever accepted.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                    ``image_version >= stored_min_version``
 *                                    (newer or equal -- launch permitted).
 * @retval k_ra8_err_validation_failed ``image_version < stored_min_version``
 *                                    (downgrade -- launch refused).
 *
 * @pre ``stored_min_version`` was produced by a successful store read.
 * @pre ``image_version`` came from an already-authenticated image trailer.
 * @post No state is mutated; the result depends only on the inputs.
 * @post On any non-``k_ra8_ok`` return the caller must NOT launch the image.
 *
 * @note Thread-safe (pure; no statics).
 * @see ra8_rot_antirollback_verify
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rot_antirollback_check(uint32_t image_version,
                                                   uint32_t stored_min_version);

/**
 * @brief Read the stored minimum, apply the policy, and commit on accept.
 *
 * @details
 * The full anti-rollback gate used by the copy-to-run launch path. Reads the
 * stored highest-accepted version through ``store``, runs
 * ::ra8_rot_antirollback_check, and -- only on accept -- advances the stored
 * counter to ``image_version``. Enforces DEFAULT-DENY: a NULL store, a NULL
 * accessor, a failed read, a downgrade verdict, or a failed commit all return a
 * non-``k_ra8_ok`` code and the caller must NOT launch.
 *
 * @param[in] store         Storage DI vtable; non-NULL with non-NULL members.
 * @param[in] image_version Version from the authenticated image trailer.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                    Accepted and the counter was advanced.
 * @retval k_ra8_err_null_ptr          ``store`` (or a member) is NULL.
 * @retval k_ra8_err_validation_failed The image is a downgrade.
 * @retval other                      Store read or commit reported a fault.
 *
 * @pre ``image_version`` belongs to an image that already passed
 *      ``ra8_rot_verify_image`` (authenticity before freshness).
 * @pre The boot path is single-threaded (the store is not re-entrant).
 * @post On ``k_ra8_ok`` the store's persisted version is ``>= image_version``.
 * @post On any non-``k_ra8_ok`` return the caller must NOT launch the image.
 *
 * @note Not thread-safe: mutates the device's non-volatile counter via
 *       ``store->commit``. Call from the single-threaded boot path.
 * @see ra8_rot_antirollback_check
 * @see ra8_rot_antirollback_default_store
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rot_antirollback_verify(const ra8_rot_antirollback_store_t* store,
                                                    uint32_t image_version);

/**
 * @brief Return the non-faking default store (reports "not provisioned").
 *
 * @details
 * The real OTP / data-flash monotonic counter is not yet wired (see the TODO
 * in ``ra8_dfu_antirollback.c``). Rather than fake a passing counter, this
 * default store's read and commit both report a "not provisioned" status, so
 * ::ra8_rot_antirollback_verify DEFAULT-DENIES until a real backing is wired.
 * The returned pointer is to storage with static lifetime; the caller must not
 * free it.
 *
 * @return Pointer to the process-lifetime default store; never NULL.
 * @retval non-NULL The default (not-provisioned) store.
 *
 * @pre ``RA8_ENABLE_ROOT_OF_TRUST`` is defined (otherwise this symbol is absent).
 * @pre The caller treats a verify failure against this store as DEFAULT-DENY.
 * @post No state is mutated; the same pointer is returned on every call.
 * @post The returned store's read and commit never report ``k_ra8_ok``.
 *
 * @note Thread-safe (returns a pointer to immutable static data).
 * @see ra8_rot_antirollback_verify
 * @since 0.1.0
 */
const ra8_rot_antirollback_store_t* ra8_rot_antirollback_default_store(void);

/**
 * @brief Recover a fault-tolerant counter probe from the app fault handler.
 *
 * @details
 * The default store reads the durable counter (extra-MRAM at
 * ::k_ra8_flash_extra_start) under a transient fault-catch, because a never-written
 * (blank) ECC-protected extra-MRAM word bus-faults on read and the RA8D2 has no
 * MRAM BlankCheck command (#194). The app's BusFault/HardFault handler must call
 * this first with a pointer to the exception stack frame
 * ([R0 R1 R2 R3 R12 LR PC xPSR]): when a probe is in progress it records the fault,
 * advances the stacked PC past the faulting load, clears the sticky fault status,
 * and returns true so the handler does a plain exception return (the read then maps
 * the blank word to version 0). Outside a probe it returns false and the handler
 * proceeds with its normal fault reporting. No-op under ``RA8_OFF_TARGET``.
 *
 * @param[in,out] exc_frame Exception stack frame captured at handler entry (MSP or
 *                          PSP per EXC_RETURN); its stacked PC (index 6) is advanced.
 *
 * @return bool True if a probe fault was recovered; false otherwise.
 * @retval true  Probe fault recovered (handler should exception-return).
 * @retval false Not a probe fault (or ``exc_frame`` is NULL) -- handle as real.
 *
 * @pre Called only from a fault exception handler with a valid frame pointer.
 * @pre ``exc_frame`` points at an 8-word basic exception frame.
 * @post On true the frame's stacked PC skips the faulting load and CFSR is cleared.
 * @post On false no state is changed.
 *
 * @note Not thread-safe; runs in handler context on the boot path.
 * @see ra8_rot_antirollback_verify
 * @since 0.1.0
 */
bool ra8_rot_antirollback_on_probe_fault(uint32_t* exc_frame);

#ifdef __cplusplus
}
#endif
