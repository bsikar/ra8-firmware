/**
 * @file ra_dfu_antirollback.h
 * @brief DFU anti-rollback (downgrade protection) policy + storage seam.
 *
 * @par Tag
 * [Ring 4 / Service] {World: S}
 *
 * @details
 * Anti-rollback prevents an attacker re-flashing an OLDER but still
 * correctly-signed image -- one that may carry known, since-patched
 * vulnerabilities. It is the second half of the copy-to-run trust gate:
 * ``ra_rot_verify_image`` (``ra_rot.h``) proves an image is *authentic*; this
 * module proves the authentic image is not a *downgrade*.
 *
 * ## Policy
 *
 * Each signed image's ``ra_rot_trailer_t`` carries a monotonic ``img_version``.
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
 * ::ra_rot_antirollback_store_t vtable (NASA Rule 9 deviation: function
 * pointers enable Dependency Inversion and host mock injection). Host unit
 * tests inject a mock counter; production wires the real backing once it
 * exists. ::ra_rot_antirollback_default_store returns a non-faking stub whose
 * read/commit report "not provisioned" so that, until a real counter is wired,
 * the gate DEFAULT-DENIES rather than silently passes.
 *
 * ## Enabling (opt-in, default OFF)
 *
 * Gated behind ``RA_ENABLE_ROOT_OF_TRUST`` (default OFF), the same flag as the
 * root of trust. With the flag OFF the implementation in
 * ``ra_dfu_antirollback.c`` compiles to nothing (an empty translation unit,
 * mirroring ``ra_rot.c``) and the launch path does not check versions, so
 * existing apps are byte-for-byte unchanged. The declarations below are always
 * visible and reference no external symbol, so a flag-off translation unit
 * gains no link dependency.
 *
 * @warning The trailer ``img_version`` is NOT yet covered by the image
 *          signature (the ECDSA signature authenticates only the body digest).
 *          Until the signed-image tooling binds the version into the signed
 *          material, an attacker holding an older validly-signed image could
 *          raise the trailer version to defeat this check. See the TODO on
 *          ``ra_rot_trailer_t`` in ``ra_rot.h``.
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

/**
 * @typedef ra_rot_antirollback_read_fn_t
 * @brief Read the stored highest-accepted image version (DI seam).
 *
 * @details
 * Reads the device's non-volatile monotonic anti-rollback counter -- the
 * highest image version ever accepted. The production implementation reads an
 * OTP / data-flash counter; the host test implementation reads a mock variable.
 * Returning any non-``k_ra_ok`` code causes the verifier to DEFAULT-DENY.
 *
 * @param[out] out_min_version Receives the stored minimum version; non-NULL.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok           ``*out_min_version`` holds the stored counter.
 * @retval k_ra_err_null_ptr ``out_min_version`` is NULL.
 * @retval other             Backing-store read fault (verifier default-denies).
 *
 * @note Thread safety is the implementation's responsibility; the boot-path
 *       caller is single-threaded.
 * @since 0.1.0
 */
typedef ra_err_t (*ra_rot_antirollback_read_fn_t)(uint32_t* out_min_version);

/**
 * @typedef ra_rot_antirollback_commit_fn_t
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
 * @return ra_err_t Error code.
 * @retval k_ra_ok Counter advanced (or already at/above ``new_version``).
 * @retval other   Backing-store program fault (verifier default-denies).
 *
 * @note Thread safety is the implementation's responsibility; the boot-path
 *       caller is single-threaded.
 * @since 0.1.0
 */
typedef ra_err_t (*ra_rot_antirollback_commit_fn_t)(uint32_t new_version);

/**
 * @struct ra_rot_antirollback_store_t
 * @brief Dependency-injection vtable for the non-volatile version counter.
 *
 * @details
 * The single seam between the pure anti-rollback policy and the durable
 * storage that backs the monotonic counter. Production wires the real OTP /
 * data-flash accessors; host tests wire mocks. Both function pointers must be
 * non-NULL for ::ra_rot_antirollback_verify to proceed -- a NULL member is a
 * default-deny.
 *
 * @invariant ``read`` and ``commit`` are either both wired to a real backing
 *            or both report "not provisioned" (never one of each).
 *
 * @par Example:
 * @code
 * const ra_rot_antirollback_store_t* store = ra_rot_antirollback_default_store();
 * ra_err_t err = ra_rot_antirollback_verify(store, image_version);
 * @endcode
 *
 * @see ra_rot_antirollback_verify
 * @see ra_rot_antirollback_default_store
 */
typedef struct {
  ra_rot_antirollback_read_fn_t   read;   /**< Read stored highest-accepted version. */
  ra_rot_antirollback_commit_fn_t commit; /**< Persist the newly-accepted version.   */
} ra_rot_antirollback_store_t;

/**
 * @brief Pure downgrade policy: accept iff the image is not older than stored.
 *
 * @details
 * The side-effect-free core of anti-rollback. An image is accepted when its
 * version is greater than or equal to the stored minimum (equal allows a
 * same-version re-flash); a strictly lower version is a downgrade and is
 * rejected. No storage is touched -- callers read the stored minimum first
 * (see ::ra_rot_antirollback_verify) and default-deny on any read failure
 * before reaching this comparison.
 *
 * @param[in] image_version       Version recorded in the candidate image trailer.
 * @param[in] stored_min_version  Highest version the device has ever accepted.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                    ``image_version >= stored_min_version``
 *                                    (newer or equal -- launch permitted).
 * @retval k_ra_err_validation_failed ``image_version < stored_min_version``
 *                                    (downgrade -- launch refused).
 *
 * @pre ``stored_min_version`` was produced by a successful store read.
 * @pre ``image_version`` came from an already-authenticated image trailer.
 * @post No state is mutated; the result depends only on the inputs.
 * @post On any non-``k_ra_ok`` return the caller must NOT launch the image.
 *
 * @note Thread-safe (pure; no statics).
 * @see ra_rot_antirollback_verify
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rot_antirollback_check(uint32_t image_version,
                                                 uint32_t stored_min_version);

/**
 * @brief Read the stored minimum, apply the policy, and commit on accept.
 *
 * @details
 * The full anti-rollback gate used by the copy-to-run launch path. Reads the
 * stored highest-accepted version through ``store``, runs
 * ::ra_rot_antirollback_check, and -- only on accept -- advances the stored
 * counter to ``image_version``. Enforces DEFAULT-DENY: a NULL store, a NULL
 * accessor, a failed read, a downgrade verdict, or a failed commit all return a
 * non-``k_ra_ok`` code and the caller must NOT launch.
 *
 * @param[in] store         Storage DI vtable; non-NULL with non-NULL members.
 * @param[in] image_version Version from the authenticated image trailer.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                    Accepted and the counter was advanced.
 * @retval k_ra_err_null_ptr          ``store`` (or a member) is NULL.
 * @retval k_ra_err_validation_failed The image is a downgrade.
 * @retval other                      Store read or commit reported a fault.
 *
 * @pre ``image_version`` belongs to an image that already passed
 *      ``ra_rot_verify_image`` (authenticity before freshness).
 * @pre The boot path is single-threaded (the store is not re-entrant).
 * @post On ``k_ra_ok`` the store's persisted version is ``>= image_version``.
 * @post On any non-``k_ra_ok`` return the caller must NOT launch the image.
 *
 * @note Not thread-safe: mutates the device's non-volatile counter via
 *       ``store->commit``. Call from the single-threaded boot path.
 * @see ra_rot_antirollback_check
 * @see ra_rot_antirollback_default_store
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_rot_antirollback_verify(const ra_rot_antirollback_store_t* store,
                                                  uint32_t                           image_version);

/**
 * @brief Return the non-faking default store (reports "not provisioned").
 *
 * @details
 * The real OTP / data-flash monotonic counter is not yet wired (see the TODO
 * in ``ra_dfu_antirollback.c``). Rather than fake a passing counter, this
 * default store's read and commit both report a "not provisioned" status, so
 * ::ra_rot_antirollback_verify DEFAULT-DENIES until a real backing is wired.
 * The returned pointer is to storage with static lifetime; the caller must not
 * free it.
 *
 * @return Pointer to the process-lifetime default store; never NULL.
 * @retval non-NULL The default (not-provisioned) store.
 *
 * @pre ``RA_ENABLE_ROOT_OF_TRUST`` is defined (otherwise this symbol is absent).
 * @pre The caller treats a verify failure against this store as DEFAULT-DENY.
 * @post No state is mutated; the same pointer is returned on every call.
 * @post The returned store's read and commit never report ``k_ra_ok``.
 *
 * @note Thread-safe (returns a pointer to immutable static data).
 * @see ra_rot_antirollback_verify
 * @since 0.1.0
 */
const ra_rot_antirollback_store_t* ra_rot_antirollback_default_store(void);

#ifdef __cplusplus
}
#endif
