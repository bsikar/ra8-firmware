/**
 * @file ra_dfu_antirollback.c
 * @brief DFU anti-rollback (downgrade protection) -- policy + storage seam.
 *
 * @par Tag
 * [Ring 4 / Service] {World: S}
 *
 * @details
 * See ``ra_dfu_antirollback.h`` for the full contract. This file owns the pure
 * downgrade policy, the verify-and-commit flow over the injected storage
 * vtable, and the non-faking default store. No raw MMIO is performed here: the
 * non-volatile counter is reached only through the
 * ::ra_rot_antirollback_store_t function pointers.
 *
 * The whole module is opt-in behind ``RA_ENABLE_ROOT_OF_TRUST`` (default OFF),
 * mirroring ``ra_rot.c``. With the flag OFF this file compiles to nothing --
 * only the link-free declarations from ``ra_dfu_antirollback.h`` -- so the
 * default build is byte-for-byte unchanged and pulls in no extra link
 * dependency.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_dfu_antirollback.h"

#ifdef RA_ENABLE_ROOT_OF_TRUST

#include "ra_check.h"

/**
 * @var s_tag
 * @brief Logger tag for the anti-rollback module.
 * @details Component identifier used by ``ra_log_*`` / ``RA_CHECK_*`` so
 *          anti-rollback log lines are easy to grep for in RTT output.
 * @note    File-private; never accessed from outside.
 * @warning Do not modify.
 * @since   0.1.0
 */
static const char* s_tag = "ROLLBACK";

ra_err_t ra_rot_antirollback_check(uint32_t image_version, uint32_t stored_min_version)
{
  /* Downgrade: a strictly older image carries since-patched defects -- deny. */
  if (image_version < stored_min_version) {
    ra_log_error(s_tag, "anti-rollback: image version below stored minimum");
    return k_ra_err_validation_failed;
  }
  /* Newer or equal: not a downgrade -- accept. */
  return k_ra_ok;
}

/**
 * @brief Default-store read stub: report "not provisioned" (no NV backing).
 *
 * @details
 * Validates the output pointer, then reports that no durable counter exists.
 * It deliberately does NOT write ``*out_min_version`` or fabricate a value, so
 * ::ra_rot_antirollback_verify default-denies until a real backing is wired.
 *
 * @param[out] out_min_version Output pointer (validated, never written here).
 *
 * @return ra_err_t Error code.
 * @retval k_ra_err_null_ptr      ``out_min_version`` is NULL.
 * @retval k_ra_err_not_supported No NV monotonic counter is wired.
 *
 * @pre ``out_min_version`` is the caller's stored-version sink.
 * @pre The caller treats a non-``k_ra_ok`` return as DEFAULT-DENY.
 * @post No state is mutated; ``*out_min_version`` is left untouched.
 * @post The return is never ``k_ra_ok`` (the counter is not provisioned).
 *
 * @note Thread-safe (pure; reports a constant status).
 * @since 0.1.0
 */
static ra_err_t internal_default_store_read(uint32_t* out_min_version)
{
  RA_CHECK_NULL_PTR(out_min_version, s_tag, "out_min_version");
  /* TODO(no NV monotonic-counter backing wired -- needs MRAM extra-area or
   * data-flash): no durable anti-rollback counter exists yet. Report
   * not-provisioned rather than fabricate a value so the gate default-denies. */
  ra_log_error(s_tag, "anti-rollback: NV counter not provisioned (read)");
  return k_ra_err_not_supported;
}

/**
 * @brief Default-store commit stub: report "not provisioned" (no NV backing).
 *
 * @details
 * Accepts the new version argument but performs no durable write, reporting
 * that no counter backing exists. It never fakes success, so an accept that
 * reaches commit through the default store still default-denies.
 *
 * @param[in] new_version The version a real backing would persist (ignored).
 *
 * @return ra_err_t Error code.
 * @retval k_ra_err_not_supported No NV monotonic counter is wired.
 *
 * @pre The caller has already accepted the image via the pure policy.
 * @pre The caller treats a non-``k_ra_ok`` return as DEFAULT-DENY.
 * @post No state is mutated; nothing is persisted.
 * @post The return is never ``k_ra_ok`` (the counter is not provisioned).
 *
 * @note Thread-safe (pure; reports a constant status).
 * @since 0.1.0
 */
static ra_err_t internal_default_store_commit(uint32_t new_version)
{
  (void)new_version;
  /* TODO(no NV monotonic-counter backing wired -- needs MRAM extra-area or
   * data-flash): no durable anti-rollback counter exists yet. Report
   * not-provisioned rather than fake a passing commit. */
  ra_log_error(s_tag, "anti-rollback: NV counter not provisioned (commit)");
  return k_ra_err_not_supported;
}

/**
 * @var s_default_store
 * @brief The process-lifetime not-provisioned default store.
 * @details Wires the two non-faking stubs above. Returned by
 *          ::ra_rot_antirollback_default_store so the launch path can
 *          default-deny until a real NV counter is provisioned.
 * @note    File-private; exposed only through the accessor.
 * @warning Do not modify; both members must report not-provisioned.
 * @since   0.1.0
 */
static const ra_rot_antirollback_store_t s_default_store = {
  .read   = internal_default_store_read,
  .commit = internal_default_store_commit,
};

ra_err_t ra_rot_antirollback_verify(const ra_rot_antirollback_store_t* store,
                                    uint32_t                           image_version)
{
  RA_CHECK_NULL_PTR(store, s_tag, "store");
  RA_CHECK_NULL_PTR(store->read, s_tag, "store->read");
  RA_CHECK_NULL_PTR(store->commit, s_tag, "store->commit");

  /* Read the stored highest-accepted version. A read fault is DEFAULT-DENY. */
  uint32_t       stored_min = 0U;
  const ra_err_t read_err   = store->read(&stored_min);
  RA_RETURN_ON_ERROR(read_err, s_tag, "anti-rollback: stored-version read failed");

  /* Apply the pure downgrade policy. A downgrade returns validation_failed. */
  const ra_err_t policy_err = ra_rot_antirollback_check(image_version, stored_min);
  RA_RETURN_ON_ERROR(policy_err, s_tag, "anti-rollback: downgrade rejected");

  /* Accepted: advance the durable counter. A commit fault is DEFAULT-DENY. */
  const ra_err_t commit_err = store->commit(image_version);
  RA_RETURN_ON_ERROR(commit_err, s_tag, "anti-rollback: counter commit failed");

  return k_ra_ok;
}

const ra_rot_antirollback_store_t* ra_rot_antirollback_default_store(void)
{
  return &s_default_store;
}

#endif /* RA_ENABLE_ROOT_OF_TRUST */
