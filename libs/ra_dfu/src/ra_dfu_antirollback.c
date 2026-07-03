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

#include <string.h>

#include "ra8d2_flash_regs.h"
#include "ra_check.h"
#include "ra_flash_core.h"

/**
 * @brief NV anti-rollback backing constants.
 * @details The durable highest-accepted version is a single little-endian
 *          ``uint32_t`` at the base of the extra-MRAM (data-flash) window
 *          (::k_ra_flash_extra_start, 0x27000000). Extra-MRAM is bit-alterable,
 *          so a commit is a plain write -- no erase cycle and no power-loss
 *          window where the counter reads back as reset.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra_rot_ar_erased = 0xFFFFFFFFU, /**< Erased word: no version stored yet. */
} ra_rot_ar_nv_t;

#ifdef RA_SIMULATOR_MODE
/**
 * @var s_sim_ar_counter
 * @brief Host-test RAM shadow of the extra-MRAM anti-rollback counter.
 * @details The host unit-test simulator models the flash MACI *registers* but
 *          not the extra-MRAM *data* side, so writes never round-trip through
 *          ``0x27000000``. Under RA_SIMULATOR_MODE the durable read/commit use
 *          this word instead; silicon and board_sim exercise the real
 *          extra-MRAM path in the ``#else`` branch.
 * @note File-private; the anti-rollback host test seeds it directly.
 * @warning Test seam only -- never compiled into a silicon image.
 * @since 0.1.0
 */
static uint32_t s_sim_ar_counter = (uint32_t)k_ra_rot_ar_erased;
#endif

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
 * @brief Default-store read: load the durable highest-accepted version.
 *
 * @details
 * Reads the little-endian ``uint32_t`` counter at the base of the extra-MRAM
 * (data-flash) window (::k_ra_flash_extra_start). A never-programmed word reads
 * as ::k_ra_rot_ar_erased and maps to version 0 -- a fresh device has accepted
 * nothing, so any image version is ``>= 0`` and passes the pure policy.
 *
 * @param[out] out_min_version Receives the stored highest-accepted version, or 0
 *             when the counter has never been programmed.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok           ``*out_min_version`` holds the durable version.
 * @retval k_ra_err_null_ptr ``out_min_version`` is NULL.
 *
 * @pre ``out_min_version`` is the caller's stored-version sink.
 * @pre Extra-MRAM is readable (always true after reset).
 * @post ``*out_min_version`` holds the durable counter (0 when erased).
 * @post No flash or RSIP state is mutated.
 *
 * @note Thread-safe: no (shares the extra-MRAM counter with the commit path).
 * @since 0.1.0
 */
static ra_err_t internal_default_store_read(uint32_t* out_min_version)
{
  RA_CHECK_NULL_PTR(out_min_version, s_tag, "out_min_version");
#ifdef RA_SIMULATOR_MODE
  const uint32_t raw = s_sim_ar_counter;
#else
  const uint32_t raw = *(const volatile uint32_t*)(uintptr_t)k_ra_flash_extra_start;
#endif
  *out_min_version = (raw == (uint32_t)k_ra_rot_ar_erased) ? 0U : raw;
  return k_ra_ok;
}

/**
 * @brief Default-store commit: durably advance the highest-accepted version.
 *
 * @details
 * The caller reaches this only after the pure policy accepted ``new_version``
 * (``new_version >= stored``). Re-reads the durable counter and writes
 * ``new_version`` to extra-MRAM only when it is strictly newer, so a same- or
 * lower-version reboot performs no write (extra-MRAM is bit-alterable, so the
 * write is a plain program with no erase cycle). A program fault propagates so
 * ::ra_rot_antirollback_verify default-denies the launch.
 *
 * @param[in] new_version The accepted version to persist as the new floor.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                 Counter is at or above ``new_version``.
 * @retval k_ra_err_invalid_arg    Flash write rejected the request.
 * @retval k_ra_err_hw_error       Flash reported an error after the program.
 * @retval k_ra_err_hw_timeout     Flash MACI never returned ready.
 *
 * @pre The caller has already accepted the image via the pure policy.
 * @pre Extra-MRAM is programmable (module powered, not locked).
 * @post On success the durable counter is ``>= new_version``.
 * @post On failure the durable counter is unchanged.
 *
 * @note Thread-safe: no (shares the extra-MRAM counter with the read path).
 * @since 0.1.0
 */
static ra_err_t internal_default_store_commit(uint32_t new_version)
{
#ifdef RA_SIMULATOR_MODE
  const uint32_t raw = s_sim_ar_counter;
#else
  const uint32_t raw = *(const volatile uint32_t*)(uintptr_t)k_ra_flash_extra_start;
#endif
  const uint32_t stored = (raw == (uint32_t)k_ra_rot_ar_erased) ? 0U : raw;
  if (new_version <= stored) {
    return k_ra_ok; /* already at or above the floor -- nothing to persist */
  }
#ifdef RA_SIMULATOR_MODE
  s_sim_ar_counter = new_version;
  return k_ra_ok;
#else
  uint8_t le[sizeof(uint32_t)] = {};
  (void)memcpy(le, &new_version, sizeof(le)); /* both toolchains little-endian */
  return ra_flash_extra_mram_write((uint32_t)k_ra_flash_extra_start, le, (uint32_t)sizeof(le));
#endif
}

/**
 * @var s_default_store
 * @brief The process-lifetime extra-MRAM-backed default store.
 * @details Wires the durable read/commit above (extra-MRAM counter at
 *          ::k_ra_flash_extra_start). Returned by
 *          ::ra_rot_antirollback_default_store so the launch path enforces a
 *          real, reboot-persistent anti-rollback floor.
 * @note    File-private; exposed only through the accessor.
 * @warning Do not modify; both members must reference the durable NV backing.
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
