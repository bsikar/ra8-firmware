/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file key_import.c
 * @brief Secure-side sealed-key import implementation
 *
 * @par Tag
 * [Ring 5 / SECAPP] {World: S}
 *
 * @details
 * Sits between the NSC veneer ``ra8_nsc_key_import`` and the
 * underlying ``ra8_key_vault_store``. The job here is twofold:
 *
 * - Authenticate the sealed blob with a real AES-CMAC (NIST
 *   SP 800-38B) over the 32 key bytes, keyed by the vault's
 *   key-authentication key (KAK). The CMAC is computed through the
 *   ``ra8_sec_cmac_*`` seam, so the firmware image runs the
 *   silicon-proven TF-PSA-Crypto ``psa_mac_*`` backend while host
 *   tests exercise a KAT-pinned in-tree reference. This replaces the
 *   forgeable length-tagged XOR fold that used to stand in for it;
 *   NS callers still see a binary go/no-go.
 *
 * - Hand back an opaque handle rather than the raw slot index.
 *   The handle is computed as ``slot ^ (s_salt rotated)``, so two
 *   different boots vend different handles for the same slot, and
 *   the slot index never leaks to NS.
 *
 * The KAK is provisioned once, secure-side, via
 * ``ra8_key_vault_set_mac_key`` and read back only through
 * ``ra8_key_vault_load_mac_key`` -- it lives in vault storage that
 * is separate from the NS-importable slot array, so nothing the
 * Non-Secure world can reach ever keys the MAC.
 */

#include <stdint.h>

#include "key_import_internal.h"
#include "key_vault.h"
#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_secure.h"
#include "sec_cmac_internal.h"

static const char* s_tag = "KEYIMP";

/** @brief 5-bit rotate-amount mask (mod 32). */
typedef enum : uint8_t {
  k_rotate_mask_5bit = 31U, /**< Rotate mask 5bit. */
} rotate_mask_t;

/**
 * @enum ra8_key_import_internal_t
 * @brief Handle-obfuscation shift constants.
 *
 * @details
 * Rotating the 32-bit salt before XORing into the 16-bit slot
 * decorrelates the low 16 bits of the salt from the high 16, so a
 * caller who can observe many handles cannot recover the salt by
 * differencing them.
 */
typedef enum : uint8_t {
  k_handle_rotate_bits = 13U, /**< Salt rotate amount before slot XOR. */
  k_salt_reroll_rot    = 7U,  /**< Salt reroll rotate amount.          */
} ra8_key_import_internal_t;

/** @brief 32-bit handle/salt mixing masks and seeds. */
typedef enum : uint32_t {
  k_initial_salt         = 0xA5A5A5A5U, /**< Boot salt seed.           */
  k_handle_high_bit_mask = 0x80000000U, /**< Forces a non-zero handle. */
  k_salt_reroll_xor      = 0xDEADBEEFU, /**< Salt reroll mixing const. */
} ra8_key_import_mask_t;

/**
 * @var s_slot_used
 * @brief One bit per vault slot: 1 if currently allocated.
 *
 * @details Bit ``i`` of ``s_slot_used`` is set when slot ``i`` has been
 * imported via ``ra8_key_import_seal``. ``ra8_key_import_reset`` clears
 * the entire mask.
 *
 * @note Direct modification from anywhere outside this TU is forbidden.
 * @since 0.1.0
 */
static uint16_t s_slot_used = 0U;

/**
 * @var s_salt
 * @brief Per-boot 32-bit salt used for handle obfuscation.
 *
 * @details Refreshed on every ``ra8_key_import_reset`` call. The chosen
 * value is intentionally non-zero so the handle for slot 0 never
 * collides with ``k_ra8_key_import_handle_zero``.
 *
 * @warning Do not write directly; call ``ra8_key_import_reset``.
 * @since 0.1.0
 */
static uint32_t s_salt = (uint32_t)k_initial_salt;

/**
 * @brief Rotate a 32-bit value left by ``amount`` bits (mod 32).
 *
 * @details
 * Used by both the salt rerolling step and the handle mixing so the
 * bit distribution is well spread for sparse inputs.
 *
 * @param[in] value  Source 32-bit word.
 * @param[in] amount Bit count; only the low 5 bits are used.
 *
 * @return ``value`` rotated left by ``amount mod 32`` bits.
 * @retval ``value`` when ``amount mod 32 == 0``.
 *
 * @pre ``amount`` may take any uint8_t value.
 * @pre Caller treats this as a pure expression (no side effects).
 * @post No state is mutated.
 * @post Return value depends only on the parameters.
 *
 * @note Pure helper; safe from any context.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_rotate_left_32(uint32_t value, uint8_t amount)
{
  const uint8_t bits = (uint8_t)(amount & (uint8_t)k_rotate_mask_5bit);
  if (bits == 0U) {
    return value;
  }
  return (value << bits) | (value >> (32U - bits));
}

/**
 * @brief Compute the opaque NS-side handle for a vault slot index.
 *
 * @details
 * Mixes the per-boot salt with the slot index so two boots vend
 * different handles for the same slot, then forces bit 31 high so
 * the value never collides with the reserved zero sentinel.
 *
 * @param[in] slot Slot index (0..k_ra8_key_vault_slots-1).
 *
 * @return Opaque handle suitable for return to NS callers.
 * @retval Always a value with bit 31 set, never ``0``.
 *
 * @pre ``slot`` was validated by the caller.
 * @pre ``s_salt`` has been initialized by ::ra8_key_import_reset or boot default.
 * @post No state is mutated.
 * @post Return value is deterministic for fixed (slot, s_salt).
 *
 * @note Pure helper; safe from any context.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_handle_for_slot(uint16_t slot)
{
  const uint32_t mixed = internal_rotate_left_32(s_salt, k_handle_rotate_bits);
  /* Force a non-zero handle: OR in bit 31 so we never collide with
   * the reserved zero sentinel even if the XOR happens to produce 0. */
  return ((uint32_t)slot ^ mixed) | (uint32_t)k_handle_high_bit_mask;
}

/**
 * @brief Verify the trailing AES-CMAC of a sealed key blob.
 *
 * @details
 * Loads the KAK from the vault, recomputes the AES-CMAC over the 32
 * key bytes via ``ra8_sec_cmac_verify``, and reports whether it
 * matches the trailing ``k_ra8_key_import_mac_bytes`` of the blob.
 * The KAK copy is wiped before return so no key material lingers on
 * the secure stack.
 *
 * @param[in] blob Sealed key blob; ``k_ra8_key_import_blob_bytes`` long.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               CMAC authentic under the vault KAK.
 * @retval k_ra8_err_invalid_arg  CMAC mismatch (blob tampered / wrong KAK).
 * @retval k_ra8_err_not_found    No KAK provisioned in the vault.
 *
 * @pre ``blob`` is non-NULL and spans ``k_ra8_key_import_blob_bytes``.
 * @pre A KAK was provisioned via ``ra8_key_vault_set_mac_key``.
 * @post No state is mutated; the KAK copy is wiped.
 * @post Return value is the authentication verdict only.
 *
 * @note Not thread-safe; secure-side serial dispatch only.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_verify_cmac(const uint8_t* blob)
{
  uint8_t         mac_key[k_ra8_key_vault_mac_key_bytes];
  uint16_t        mac_key_len = 0U;
  const ra8_err_t kerr =
    ra8_key_vault_load_mac_key(mac_key, (uint16_t)sizeof(mac_key), &mac_key_len);
  if (kerr != k_ra8_ok) {
    return kerr;
  }
  const ra8_err_t verr = ra8_sec_cmac_verify(mac_key,
                                             mac_key_len,
                                             blob,
                                             (uint32_t)k_ra8_key_import_key_bytes,
                                             &blob[k_ra8_key_import_key_bytes],
                                             (uint16_t)k_ra8_key_import_mac_bytes);
  ra8_secure_memzero(mac_key, sizeof(mac_key));
  return verr;
}

/**
 * @brief Reset the import allocator and reroll the per-boot salt.
 *
 * @details
 * Clears every ``s_slot_used`` bit and rotates the salt with a
 * fixed mixing constant so successive resets vend different
 * handles for the same slot index. Falls back to the boot seed
 * if the rerolled salt happens to be zero.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Always; the operation cannot fail.
 *
 * @pre Caller is in the secure-side init context.
 * @pre No NS-side handle issued before the call may be considered live afterwards.
 * @post All slots are marked free.
 * @post ``s_salt`` is non-zero.
 *
 * @note Not thread-safe; reset belongs to the boot/test path.
 * @since 0.1.0
 */
ra8_err_t ra8_key_import_reset(void)
{
  s_slot_used = 0U;
  /* Bump the salt with a fixed mixing constant so successive resets
   * also produce different handles. */
  s_salt =
    internal_rotate_left_32(s_salt, (uint8_t)k_salt_reroll_rot) ^ (uint32_t)k_salt_reroll_xor;
  if (s_salt == 0U) {
    s_salt = (uint32_t)k_initial_salt;
  }
  return k_ra8_ok;
}

/**
 * @brief Verify, store, and assign an opaque handle for a sealed key blob.
 *
 * @details
 * Validates the blob length, checks the AES-CMAC, allocates the
 * lowest free vault slot, copies the key into the vault, and returns
 * an opaque handle that the NS world can later present to the
 * SHA-256 challenge primitive without ever learning the slot index.
 *
 * @param[in]  blob       Sealed key blob.
 * @param[in]  blob_len   Length of ``blob``; must equal
 *                        ``k_ra8_key_import_blob_bytes``.
 * @param[out] out_handle Receives the opaque handle on success.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                 Key sealed and handle issued.
 * @retval k_ra8_err_null_ptr       ``blob`` or ``out_handle`` was NULL.
 * @retval k_ra8_err_invalid_size   ``blob_len`` did not match expected size.
 * @retval k_ra8_err_invalid_arg    CMAC verification failed.
 * @retval k_ra8_err_not_found      No KAK provisioned in the vault.
 * @retval k_ra8_err_no_mem         All vault slots are in use.
 *
 * @pre ``blob`` and ``out_handle`` are non-NULL.
 * @pre A KAK was provisioned via ``ra8_key_vault_set_mac_key``.
 * @post On success, the chosen slot bit is set in ``s_slot_used``.
 * @post On error, no vault slot is mutated.
 *
 * @note Not thread-safe; secure-side serial dispatch only.
 * @see ra8_key_import_resolve
 * @since 0.1.0
 */
ra8_err_t ra8_key_import_seal(const uint8_t* blob, uint32_t blob_len, uint32_t* out_handle)
{
  RA8_CHECK_NULL_PTR(blob, s_tag, "seal: blob");
  RA8_CHECK_NULL_PTR(out_handle, s_tag, "seal: out_handle");
  if (blob_len != (uint32_t)k_ra8_key_import_blob_bytes) {
    return k_ra8_err_invalid_size;
  }
  const ra8_err_t mac_err = internal_verify_cmac(blob);
  if (mac_err != k_ra8_ok) {
    return mac_err;
  }

  /* Pick the lowest free slot. Loop bound is the slot count -- NASA
   * Rule 2 compliant. */
  uint16_t chosen = (uint16_t)k_ra8_key_vault_slots;
  for (uint16_t i = 0U; i < (uint16_t)k_ra8_key_vault_slots; ++i) {
    const uint16_t bit = (uint16_t)((uint16_t)1U << i);
    if ((s_slot_used & bit) == 0U) {
      chosen = i;
      break;
    }
  }
  if (chosen == (uint16_t)k_ra8_key_vault_slots) {
    return k_ra8_err_no_mem;
  }

  const ra8_err_t store_err = ra8_key_vault_store(chosen, blob);
  if (store_err != k_ra8_ok) {
    return store_err;
  }
  s_slot_used = (uint16_t)(s_slot_used | (uint16_t)((uint16_t)1U << chosen));
  *out_handle = internal_handle_for_slot(chosen);
  return k_ra8_ok;
}

/**
 * @brief Resolve a previously issued handle back to its vault slot.
 *
 * @details
 * Walks the live slot bitmap and recomputes the per-slot handle
 * until a match is found. The slot index never leaves the secure
 * world via the handle itself; this function is the only place
 * that performs the inverse mapping.
 *
 * @param[in]  handle   Opaque handle previously returned by ::ra8_key_import_seal.
 * @param[out] out_slot Receives the resolved slot index on success.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                 Handle matched a live slot.
 * @retval k_ra8_err_null_ptr       ``out_slot`` was NULL.
 * @retval k_ra8_err_not_found      Handle does not match any live slot.
 *
 * @pre ``out_slot`` is non-NULL.
 * @pre Caller has previously issued the handle through ::ra8_key_import_seal.
 * @post On success, ``*out_slot`` is in [0, k_ra8_key_vault_slots).
 * @post No vault state is mutated.
 *
 * @note Not thread-safe.
 * @see ra8_key_import_seal
 * @since 0.1.0
 */
ra8_err_t ra8_key_import_resolve(uint32_t handle, uint16_t* out_slot)
{
  RA8_CHECK_NULL_PTR(out_slot, s_tag, "resolve: out_slot");
  for (uint16_t i = 0U; i < (uint16_t)k_ra8_key_vault_slots; ++i) {
    const uint16_t bit = (uint16_t)((uint16_t)1U << i);
    if (((s_slot_used & bit) != 0U) && (internal_handle_for_slot(i) == handle)) {
      *out_slot = i;
      return k_ra8_ok;
    }
  }
  return k_ra8_err_not_found;
}

/**
 * @brief Build a sealed key blob from a raw 32-byte key (provisioning + test).
 *
 * @details
 * Copies the key bytes verbatim, computes the AES-CMAC over them with
 * the vault KAK via ``ra8_sec_cmac_compute``, and writes the trailing
 * ``k_ra8_key_import_mac_bytes`` of the blob. Provided so provisioning
 * and unit tests can package a key the import path will accept.
 *
 * @param[in]  material Raw 32-byte key material.
 * @param[out] out_blob Receives ``k_ra8_key_import_blob_bytes`` of output.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                 Blob written.
 * @retval k_ra8_err_null_ptr       ``material`` or ``out_blob`` was NULL.
 * @retval k_ra8_err_not_found      No KAK provisioned in the vault.
 *
 * @pre ``material`` and ``out_blob`` are non-NULL.
 * @pre A KAK was provisioned via ``ra8_key_vault_set_mac_key``.
 * @post On success ``out_blob`` is accepted by ::internal_verify_cmac
 *       under the current KAK.
 * @post No global state is mutated.
 *
 * @note Not thread-safe.
 * @see ra8_key_import_seal
 * @since 0.1.0
 */
ra8_err_t ra8_key_import_build_blob(const uint8_t* material, uint8_t* out_blob)
{
  RA8_CHECK_NULL_PTR(material, s_tag, "build_blob: material");
  RA8_CHECK_NULL_PTR(out_blob, s_tag, "build_blob: out_blob");
  uint8_t         mac_key[k_ra8_key_vault_mac_key_bytes];
  uint16_t        mac_key_len = 0U;
  const ra8_err_t kerr =
    ra8_key_vault_load_mac_key(mac_key, (uint16_t)sizeof(mac_key), &mac_key_len);
  if (kerr != k_ra8_ok) {
    return kerr;
  }
  for (uint16_t i = 0U; i < (uint16_t)k_ra8_key_import_key_bytes; ++i) {
    out_blob[i] = material[i];
  }
  const ra8_err_t cerr = ra8_sec_cmac_compute(mac_key,
                                              mac_key_len,
                                              material,
                                              (uint32_t)k_ra8_key_import_key_bytes,
                                              &out_blob[k_ra8_key_import_key_bytes]);
  ra8_secure_memzero(mac_key, sizeof(mac_key));
  return cerr;
}
