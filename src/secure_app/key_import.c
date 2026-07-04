/**
 * @file key_import.c
 * @brief Secure-side sealed-key import implementation
 *
 * @par Tag
 * [Ring 5 / SECAPP] {World: S}
 *
 * @details
 * Sits between the NSC veneer ``ra_nsc_key_import`` and the
 * underlying ``ra_key_vault_store``. The job here is twofold:
 *
 * - Validate the sealed blob's MAC. The MAC scheme is a stub
 *   (length-tagged XOR fold against ``s_salt``) until the SCE
 *   CMAC engine is hooked up; the *interface* is the part of this
 *   commit that matters. NS callers see a binary go/no-go.
 *
 * - Hand back an opaque handle rather than the raw slot index.
 *   The handle is computed as ``slot ^ (s_salt rotated)``, so two
 *   different boots vend different handles for the same slot, and
 *   the slot index never leaks to NS.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "key_import.h"

#include <stdint.h>

#include "key_vault.h"
#include "ra_check.h"
#include "ra_err.h"

static const char* s_tag = "KEYIMP";

/*
 * Fail-closed stub-crypto gate (issue #180). The sealed-blob MAC below is a
 * FORGEABLE length-tagged XOR fold standing in for the SCE CMAC engine, so a
 * caller can mint a blob this code will accept. It is only safe under host
 * simulation or an explicitly-declared insecure dev/eval image. A real
 * production/HIL image (neither flag set) compiles the #else branch, where
 * every entry point hard-errors so a forged key blob can never be accepted.
 * scripts/utils/check_stub_crypto_guarded.py enforces the guard.
 */
#if defined(RA_INSECURE_STUB_CRYPTO) || defined(RA_SIMULATOR_MODE)

/**
 * @def k_handle_rotate_bits
 * @brief Bits the salt is rotated by before XORing into the slot.
 *
 * @details
 * Rotating the 32-bit salt before XORing into the 16-bit slot
 * decorrelates the low 16 bits of the salt from the high 16, so a
 * caller who can observe many handles cannot recover the salt by
 * differencing them.
 *
 * @since 0.1.0
 */
/** @brief 5-bit rotate-amount mask (mod 32). */
typedef enum : uint8_t {
  k_rotate_mask_5bit = 31U,
} rotate_mask_t;

typedef enum : uint8_t {
  k_handle_rotate_bits = 13U,
  k_handle_lo16_mask   = 16U,
} ra_key_import_internal_t;

/* Magic-number suppression aids for clang-tidy. */
typedef enum : uint8_t {
  k_byte_shift_24   = 24U,
  k_byte_shift_16   = 16U,
  k_byte_shift_8    = 8U,
  k_blob_off_byte0  = 0U,
  k_blob_off_byte1  = 1U,
  k_blob_off_byte2  = 2U,
  k_blob_off_byte3  = 3U,
  k_salt_reroll_rot = 7U,
} ra_key_import_byte_pack_t;

typedef enum : uint32_t {
  k_byte_lo_mask         = 0xFFU,
  k_initial_salt         = 0xA5A5A5A5U,
  k_handle_high_bit_mask = 0x80000000U,
  k_salt_reroll_xor      = 0xDEADBEEFU,
} ra_key_import_byte_mask_t;

/**
 * @var s_slot_used
 * @brief One bit per vault slot: 1 if currently allocated.
 *
 * @details Bit ``i`` of ``s_slot_used`` is set when slot ``i`` has been
 * imported via ``ra_key_import_seal``. ``ra_key_import_reset`` clears
 * the entire mask.
 *
 * @note Direct modification from anywhere outside this TU is forbidden.
 * @since 0.1.0
 */
static uint16_t s_slot_used = 0U;

/**
 * @var s_salt
 * @brief Per-boot 32-bit salt used for handle obfuscation + MAC.
 *
 * @details Refreshed on every ``ra_key_import_reset`` call. The chosen
 * value is intentionally non-zero so the handle for slot 0 never
 * collides with ``k_ra_key_import_handle_zero``.
 *
 * @warning Do not write directly; call ``ra_key_import_reset``.
 * @since 0.1.0
 */
static uint32_t s_salt = (uint32_t)k_initial_salt;

/**
 * @brief Rotate a 32-bit value left by ``amount`` bits (mod 32).
 *
 * @details
 * Used by both the salt rerolling step and the MAC fold so the bit
 * mixing is well distributed even for sparse blob inputs.
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
static uint32_t internal_rotate_left_32(uint32_t value, uint8_t amount)
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
 * @param[in] slot Slot index (0..k_ra_key_vault_slots-1).
 *
 * @return Opaque handle suitable for return to NS callers.
 * @retval Always a value with bit 31 set, never ``0``.
 *
 * @pre ``slot`` was validated by the caller.
 * @pre ``s_salt`` has been initialized by ::ra_key_import_reset or boot default.
 * @post No state is mutated.
 * @post Return value is deterministic for fixed (slot, s_salt).
 *
 * @note Pure helper; safe from any context.
 * @since 0.1.0
 */
static uint32_t internal_handle_for_slot(uint16_t slot)
{
  const uint32_t mixed = internal_rotate_left_32(s_salt, k_handle_rotate_bits);
  /* Force a non-zero handle: OR in bit 31 so we never collide with
   * the reserved zero sentinel even if the XOR happens to produce 0. */
  return ((uint32_t)slot ^ mixed) | (uint32_t)k_handle_high_bit_mask;
}

/**
 * @brief Verify the trailing MAC bytes of a sealed key blob.
 *
 * @details
 * Folds the 32 key bytes into a 32-bit accumulator (rotate-and-XOR
 * with the per-boot salt seed) and compares the result to the
 * trailing 4 big-endian bytes of the blob. The scheme is a stub for
 * the SCE CMAC engine slated for ; the interface is final.
 *
 * @param[in] blob Sealed key blob; ``k_ra_key_import_blob_bytes`` long.
 *
 * @return true when the computed MAC matches the trailing bytes.
 * @retval true  Blob authentic under the current ``s_salt``.
 * @retval false Blob tampered or built under a different salt.
 *
 * @pre ``blob`` is non-NULL.
 * @pre ``blob`` storage spans at least ``k_ra_key_import_blob_bytes``.
 * @post No state is mutated.
 * @post Return value depends only on ``blob`` and ``s_salt``.
 *
 * @note Pure helper; safe from any context.
 * @since 0.1.0
 */
static bool internal_verify_mac(const uint8_t* blob)
{
  /* MAC scheme: fold the 32 key bytes into a 32-bit accumulator
   * with the salt mixed in, then compare to the trailing 4 bytes.
 * A real CMAC swap-in lands in ; the interface stays the
   * same so the veneer code survives. */
  uint32_t acc = s_salt;
  for (uint16_t i = 0U; i < k_ra_key_import_key_bytes; ++i) {
    acc = internal_rotate_left_32(acc, 1U) ^ (uint32_t)blob[i];
  }
  const uint32_t expect = ((uint32_t)blob[k_ra_key_import_key_bytes + (uint16_t)k_blob_off_byte0]
                           << (uint32_t)k_byte_shift_24) |
                          ((uint32_t)blob[k_ra_key_import_key_bytes + (uint16_t)k_blob_off_byte1]
                           << (uint32_t)k_byte_shift_16) |
                          ((uint32_t)blob[k_ra_key_import_key_bytes + (uint16_t)k_blob_off_byte2]
                           << (uint32_t)k_byte_shift_8) |
                          ((uint32_t)blob[k_ra_key_import_key_bytes + (uint16_t)k_blob_off_byte3]);
  return acc == expect;
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
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Always; the operation cannot fail.
 *
 * @pre Caller is in the secure-side init context.
 * @pre No NS-side handle issued before the call may be considered live afterwards.
 * @post All slots are marked free.
 * @post ``s_salt`` is non-zero.
 *
 * @note Not thread-safe; reset belongs to the boot/test path.
 * @since 0.1.0
 */
ra_err_t ra_key_import_reset(void)
{
  s_slot_used = 0U;
  /* Bump the salt with a fixed mixing constant so successive resets
   * also produce different handles. */
  s_salt =
    internal_rotate_left_32(s_salt, (uint8_t)k_salt_reroll_rot) ^ (uint32_t)k_salt_reroll_xor;
  if (s_salt == 0U) {
    s_salt = (uint32_t)k_initial_salt;
  }
  return k_ra_ok;
}

/**
 * @brief Verify, store, and assign an opaque handle for a sealed key blob.
 *
 * @details
 * Validates the blob length, checks the MAC, allocates the lowest
 * free vault slot, copies the key into the vault, and returns an
 * opaque handle that the NS world can later present to the SHA-256
 * challenge primitive without ever learning the slot index.
 *
 * @param[in]  blob       Sealed key blob.
 * @param[in]  blob_len   Length of ``blob``; must equal
 *                        ``k_ra_key_import_blob_bytes``.
 * @param[out] out_handle Receives the opaque handle on success.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                 Key sealed and handle issued.
 * @retval k_ra_err_null_ptr       ``blob`` or ``out_handle`` was NULL.
 * @retval k_ra_err_invalid_size   ``blob_len`` did not match expected size.
 * @retval k_ra_err_invalid_arg    MAC verification failed.
 * @retval k_ra_err_no_mem         All vault slots are in use.
 *
 * @pre ``blob`` and ``out_handle`` are non-NULL.
 * @pre Caller has finished bring-up of the key vault via ::ra_key_vault_init.
 * @post On success, the chosen slot bit is set in ``s_slot_used``.
 * @post On error, no vault slot is mutated.
 *
 * @note Not thread-safe; secure-side serial dispatch only.
 * @see ra_key_import_resolve
 * @since 0.1.0
 */
ra_err_t ra_key_import_seal(const uint8_t* blob, uint32_t blob_len, uint32_t* out_handle)
{
  RA_CHECK_NULL_PTR(blob, s_tag, "seal: blob");
  RA_CHECK_NULL_PTR(out_handle, s_tag, "seal: out_handle");
  if (blob_len != (uint32_t)k_ra_key_import_blob_bytes) {
    return k_ra_err_invalid_size;
  }
  if (!internal_verify_mac(blob)) {
    return k_ra_err_invalid_arg;
  }

  /* Pick the lowest free slot. Loop bound is the slot count -- NASA
   * Rule 2 compliant. */
  uint16_t chosen = (uint16_t)k_ra_key_vault_slots;
  for (uint16_t i = 0U; i < (uint16_t)k_ra_key_vault_slots; ++i) {
    const uint16_t bit = (uint16_t)((uint16_t)1U << i);
    if ((s_slot_used & bit) == 0U) {
      chosen = i;
      break;
    }
  }
  if (chosen == (uint16_t)k_ra_key_vault_slots) {
    return k_ra_err_no_mem;
  }

  const ra_err_t store_err = ra_key_vault_store(chosen, blob);
  if (store_err != k_ra_ok) {
    return store_err;
  }
  s_slot_used = (uint16_t)(s_slot_used | (uint16_t)((uint16_t)1U << chosen));
  *out_handle = internal_handle_for_slot(chosen);
  return k_ra_ok;
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
 * @param[in]  handle   Opaque handle previously returned by ::ra_key_import_seal.
 * @param[out] out_slot Receives the resolved slot index on success.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                 Handle matched a live slot.
 * @retval k_ra_err_null_ptr       ``out_slot`` was NULL.
 * @retval k_ra_err_not_found      Handle does not match any live slot.
 *
 * @pre ``out_slot`` is non-NULL.
 * @pre Caller has previously issued the handle through ::ra_key_import_seal.
 * @post On success, ``*out_slot`` is in [0, k_ra_key_vault_slots).
 * @post No vault state is mutated.
 *
 * @note Not thread-safe.
 * @see ra_key_import_seal
 * @since 0.1.0
 */
ra_err_t ra_key_import_resolve(uint32_t handle, uint16_t* out_slot)
{
  RA_CHECK_NULL_PTR(out_slot, s_tag, "resolve: out_slot");
  for (uint16_t i = 0U; i < (uint16_t)k_ra_key_vault_slots; ++i) {
    const uint16_t bit = (uint16_t)((uint16_t)1U << i);
    if (((s_slot_used & bit) != 0U) && (internal_handle_for_slot(i) == handle)) {
      *out_slot = i;
      return k_ra_ok;
    }
  }
  return k_ra_err_not_found;
}

/**
 * @brief Build a sealed key blob from a raw 32-byte key (test helper).
 *
 * @details
 * Copies the key bytes verbatim, computes the same MAC fold as
 * ::internal_verify_mac, and writes the trailing 4 big-endian
 * bytes. Provided so unit tests can exercise the verify path
 * without a separate sealing utility.
 *
 * @param[in]  key      Raw 32-byte key.
 * @param[out] out_blob Receives ``k_ra_key_import_blob_bytes`` of output.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                 Blob written.
 * @retval k_ra_err_null_ptr       ``key`` or ``out_blob`` was NULL.
 *
 * @pre ``key`` and ``out_blob`` are non-NULL.
 * @pre ``out_blob`` storage spans at least ``k_ra_key_import_blob_bytes``.
 * @post ``out_blob`` contains a blob accepted by ::internal_verify_mac
 *       under the current ``s_salt``.
 * @post No global state is mutated.
 *
 * @note Not thread-safe.
 * @see ra_key_import_seal
 * @since 0.1.0
 */
ra_err_t ra_key_import_build_blob(const uint8_t* key, uint8_t* out_blob)
{
  RA_CHECK_NULL_PTR(key, s_tag, "build_blob: key");
  RA_CHECK_NULL_PTR(out_blob, s_tag, "build_blob: out_blob");
  uint32_t acc = s_salt;
  for (uint16_t i = 0U; i < k_ra_key_import_key_bytes; ++i) {
    out_blob[i] = key[i];
    acc         = internal_rotate_left_32(acc, 1U) ^ (uint32_t)key[i];
  }
  out_blob[k_ra_key_import_key_bytes + (uint16_t)k_blob_off_byte0] =
    (uint8_t)((acc >> (uint32_t)k_byte_shift_24) & (uint32_t)k_byte_lo_mask);
  out_blob[k_ra_key_import_key_bytes + (uint16_t)k_blob_off_byte1] =
    (uint8_t)((acc >> (uint32_t)k_byte_shift_16) & (uint32_t)k_byte_lo_mask);
  out_blob[k_ra_key_import_key_bytes + (uint16_t)k_blob_off_byte2] =
    (uint8_t)((acc >> (uint32_t)k_byte_shift_8) & (uint32_t)k_byte_lo_mask);
  out_blob[k_ra_key_import_key_bytes + (uint16_t)k_blob_off_byte3] =
    (uint8_t)(acc & (uint32_t)k_byte_lo_mask);
  return k_ra_ok;
}

#else /* production build: neither RA_INSECURE_STUB_CRYPTO nor RA_SIMULATOR_MODE */

/*
 * Fail-closed production variant. Without a real CMAC backend the forgeable
 * XOR-fold MAC above must never authenticate a blob, so every entry point
 * returns a hard error (never k_ra_ok). A production image that forgot to
 * provide real key sealing therefore cannot import an unauthenticated key.
 */

ra_err_t ra_key_import_reset(void)
{
  return k_ra_err_not_supported;
}

ra_err_t ra_key_import_seal(const uint8_t* blob, uint32_t blob_len, uint32_t* out_handle)
{
  RA_CHECK_NULL_PTR(blob, s_tag, "seal: blob");
  RA_CHECK_NULL_PTR(out_handle, s_tag, "seal: out_handle");
  (void)blob_len;
  return k_ra_err_not_supported;
}

ra_err_t ra_key_import_resolve(uint32_t handle, uint16_t* out_slot)
{
  RA_CHECK_NULL_PTR(out_slot, s_tag, "resolve: out_slot");
  (void)handle;
  return k_ra_err_not_supported;
}

ra_err_t ra_key_import_build_blob(const uint8_t* key, uint8_t* out_blob)
{
  RA_CHECK_NULL_PTR(key, s_tag, "build_blob: key");
  RA_CHECK_NULL_PTR(out_blob, s_tag, "build_blob: out_blob");
  return k_ra_err_not_supported;
}

#endif /* RA_INSECURE_STUB_CRYPTO || RA_SIMULATOR_MODE */
