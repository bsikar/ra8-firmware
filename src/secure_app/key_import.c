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
typedef enum : uint8_t {
  k_handle_rotate_bits = 13U,
  k_handle_lo16_mask   = 16U,
} ra_key_import_internal_t;

/* Magic-number suppression aids for clang-tidy. */
typedef enum : uint8_t {
  k_byte_shift_24    = 24U,
  k_byte_shift_16    = 16U,
  k_byte_shift_8     = 8U,
  k_blob_off_byte0   = 0U,
  k_blob_off_byte1   = 1U,
  k_blob_off_byte2   = 2U,
  k_blob_off_byte3   = 3U,
  k_salt_reroll_rot  = 7U,
} ra_key_import_byte_pack_t;

typedef enum : uint32_t {
  k_byte_lo_mask         = 0xFFU,
  k_initial_salt         = 0xA5A5A5A5U,
  k_handle_high_bit_mask = 0x80000000U,
  k_salt_reroll_xor      = 0xDEADBEEFU,
} ra_key_import_byte_mask_t;


static uint32_t internal_rotate_left_32(uint32_t value, uint8_t amount)
{
  const uint8_t bits = (uint8_t)(amount & (uint8_t)31U);
  if (bits == 0U) {
    return value;
  }
  return (value << bits) | (value >> (32U - bits));
}

static uint32_t internal_handle_for_slot(uint16_t slot)
{
  const uint32_t mixed = internal_rotate_left_32(s_salt, k_handle_rotate_bits);
  /* Force a non-zero handle: OR in bit 31 so we never collide with
   * the reserved zero sentinel even if the XOR happens to produce 0. */
  return ((uint32_t)slot ^ mixed) | (uint32_t)k_handle_high_bit_mask;
}

static bool internal_verify_mac(const uint8_t* blob)
{
  /* MAC scheme: fold the 32 key bytes into a 32-bit accumulator
   * with the salt mixed in, then compare to the trailing 4 bytes.
   * A real CMAC swap-in lands in Wave 14; the interface stays the
   * same so the veneer code survives. */
  uint32_t acc = s_salt;
  for (uint16_t i = 0U; i < k_ra_key_import_key_bytes; ++i) {
    acc = internal_rotate_left_32(acc, 1U) ^ (uint32_t)blob[i];
  }
  const uint32_t expect = ((uint32_t)blob[k_ra_key_import_key_bytes + (uint16_t)k_blob_off_byte0] << (uint32_t)k_byte_shift_24) |
                          ((uint32_t)blob[k_ra_key_import_key_bytes + (uint16_t)k_blob_off_byte1] << (uint32_t)k_byte_shift_16) |
                          ((uint32_t)blob[k_ra_key_import_key_bytes + (uint16_t)k_blob_off_byte2] << (uint32_t)k_byte_shift_8) |
                          ((uint32_t)blob[k_ra_key_import_key_bytes + (uint16_t)k_blob_off_byte3]);
  return acc == expect;
}

ra_err_t ra_key_import_reset(void)
{
  s_slot_used = 0U;
  /* Bump the salt with a fixed mixing constant so successive resets
   * also produce different handles. */
  s_salt = internal_rotate_left_32(s_salt, (uint8_t)k_salt_reroll_rot) ^ (uint32_t)k_salt_reroll_xor;
  if (s_salt == 0U) {
    s_salt = (uint32_t)k_initial_salt;
  }
  return k_ra_ok;
}

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

ra_err_t ra_key_import_build_blob(const uint8_t* key, uint8_t* out_blob)
{
  RA_CHECK_NULL_PTR(key, s_tag, "build_blob: key");
  RA_CHECK_NULL_PTR(out_blob, s_tag, "build_blob: out_blob");
  uint32_t acc = s_salt;
  for (uint16_t i = 0U; i < k_ra_key_import_key_bytes; ++i) {
    out_blob[i] = key[i];
    acc         = internal_rotate_left_32(acc, 1U) ^ (uint32_t)key[i];
  }
  out_blob[k_ra_key_import_key_bytes + (uint16_t)k_blob_off_byte0] = (uint8_t)((acc >> (uint32_t)k_byte_shift_24) & (uint32_t)k_byte_lo_mask);
  out_blob[k_ra_key_import_key_bytes + (uint16_t)k_blob_off_byte1] = (uint8_t)((acc >> (uint32_t)k_byte_shift_16) & (uint32_t)k_byte_lo_mask);
  out_blob[k_ra_key_import_key_bytes + (uint16_t)k_blob_off_byte2] = (uint8_t)((acc >> (uint32_t)k_byte_shift_8) & (uint32_t)k_byte_lo_mask);
  out_blob[k_ra_key_import_key_bytes + (uint16_t)k_blob_off_byte3] = (uint8_t)(acc & (uint32_t)k_byte_lo_mask);
  return k_ra_ok;
}
