/**
 * @file test_ra8_dfu_signed.c
 * @brief Host unit tests for signed USB-DFU image verification and tamper detection.
 *
 * @details
 * Validates the root-of-trust authenticity gate (ra8_rot_verify_image /
 * ra8_rot_trailer_after) on the USB-DFU download and flash-commit path. Exercises
 * valid signed image verification, payload tampering, signature corruption,
 * trailer format corruption, size mismatch, and NULL pointer rejection.
 *
 * [Ring 4 / Service]
 * {World: S}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#define RA8_ENABLE_ROOT_OF_TRUST

#include <stdint.h>
#include <string.h>

#include "ra8_dfu.h"
#include "ra8_dfu_internal.h"
#include "ra8_err.h"
#include "ra8_flash_regs.h"
#include "ra8_psa_crypto.h"
#include "ra8_rot.h"
#include "unity_minimal.h"

/**
 * @enum t_dfu_signed_fixture_t
 * @brief Test payload sizing, pattern strides, and tamper masks.
 */
typedef enum : uint8_t {
  k_t_dfu_stride       = 7U,    /**< Non-repeating stride for body payload.   */
  k_t_tamper_body_mask = 0xA5U, /**< Bit flip mask for image body corruption. */
  k_t_tamper_sig_mask  = 0x5AU, /**< Bit flip mask for signature corruption.  */
  k_t_octet_bits       = 8U,    /**< Bits per byte shift step.                */
} t_dfu_signed_fixture_t;

/**
 * @enum t_dfu_signed_const_t
 * @brief Test image geometry constants.
 */
typedef enum : uint32_t {
  k_test_signed_body_len = 64U,         /**< Representative 64-byte body.       */
  k_test_program_len     = 192U,        /**< 32-byte page aligned payload size. */
  k_test_signed_img_ver  = 3U,          /**< Anti-rollback version for testing. */
  k_test_bad_magic       = 0xDEADBEEFU, /**< Corrupted trailer magic sentinel.  */
  k_test_bad_version     = 0x000000FEU, /**< Unsupported format version.        */
  k_test_lied_body_len   = 32U,         /**< Inconsistent body length claim.    */
  k_test_dfu_seq         = 7U,          /**< DFU slot sequence number.          */
} t_dfu_signed_const_t;

/** @brief Mark fake MRAM controller program-ready for ra8_flash driver. */
static void fake_mark_program_ready(void)
{
  *ra8_mram_reg8((uint16_t)k_ra8_mram_off_mrcps) = (uint8_t)k_ra8_mrcps_mask_abufemp;
}

/** @brief Fill buffer with deterministic pattern. */
static void fill_body_pattern(uint8_t* buf, uint32_t len)
{
  for (uint32_t i = 0U; i < len; ++i) {
    buf[i] = (uint8_t)((i * k_t_dfu_stride) + 11U);
  }
}

/**
 * @brief Construct a valid signed trailer for a given body under RA8_OFF_TARGET.
 *
 * @details
 * In host builds, uses the provisioned test root public key and the deterministic
 * PSA software hash/signature facade to build a signed trailer identical to what
 * the on-target signing pipeline creates.
 *
 * @param[in]  body        Image body bytes.
 * @param[in]  body_len    Image body length in bytes.
 * @param[in]  img_version Anti-rollback version to bind into signature.
 * @param[out] out_trailer Destination trailer structure.
 */
static void build_test_signed_trailer(const uint8_t*     body,
                                      uint32_t           body_len,
                                      uint32_t           img_version,
                                      ra8_rot_trailer_t* out_trailer)
{
  uint8_t digest[k_ra8_rot_digest_bytes] = {};
  size_t  dlen                           = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_psa_hash_compute(k_ra8_psa_alg_sha_256,
                                      body,
                                      (size_t)body_len,
                                      digest,
                                      sizeof(digest),
                                      &dlen));

  const uint8_t* rootkey = nullptr;
  uint32_t       rootlen = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rot_root_public_key(&rootkey, &rootlen));

  const ra8_psa_key_attr_t sattr = {
    .type  = k_ra8_psa_key_type_ecc_p256_priv,
    .alg   = k_ra8_psa_alg_ecdsa_sha_256,
    .usage = k_ra8_psa_usage_sign,
  };
  ra8_psa_key_t signer = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_psa_key_import(&signer, &sattr, rootkey, (size_t)rootlen));

  uint8_t combined[sizeof(uint32_t) + k_ra8_rot_digest_bytes] = {};
  for (uint32_t i = 0U; i < (uint32_t)sizeof(uint32_t); ++i) {
    combined[i] = (uint8_t)(img_version >> (i * (uint32_t)k_t_octet_bits));
  }
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_rot_digest_bytes; ++i) {
    combined[sizeof(uint32_t) + i] = digest[i];
  }

  uint8_t signed_material[k_ra8_rot_digest_bytes] = {};
  size_t  bound_len                               = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_psa_hash_compute(k_ra8_psa_alg_sha_256,
                                      combined,
                                      sizeof(combined),
                                      signed_material,
                                      sizeof(signed_material),
                                      &bound_len));

  uint8_t sig[k_ra8_rot_sig_bytes] = {};
  size_t  siglen                   = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_psa_sign_hash(signer,
                                   k_ra8_psa_alg_ecdsa_sha_256,
                                   signed_material,
                                   sizeof(signed_material),
                                   sig,
                                   sizeof(sig),
                                   &siglen));
  (void)ra8_psa_key_destroy(signer);

  *out_trailer             = (ra8_rot_trailer_t){};
  out_trailer->magic       = (uint32_t)k_ra8_rot_trailer_magic;
  out_trailer->version     = (uint32_t)k_ra8_rot_version;
  out_trailer->img_version = img_version;
  out_trailer->body_len    = body_len;
  out_trailer->sig_len     = (uint32_t)siglen;
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_rot_digest_bytes; ++i) {
    out_trailer->digest[i] = digest[i];
  }
  for (uint32_t i = 0U; i < (uint32_t)siglen; ++i) {
    out_trailer->sig[i] = sig[i];
  }
}

/**
 * @brief Happy path: valid signed DFU download passes verification and commit.
 */
static void test_dfu_signed_image_valid(void)
{
  TEST_BEGIN("dfu_signed: valid signed image accepted");

  uint8_t body[k_test_signed_body_len];
  fill_body_pattern(body, (uint32_t)k_test_signed_body_len);

  ra8_rot_trailer_t trailer = {};
  build_test_signed_trailer(body, (uint32_t)k_test_signed_body_len, (uint32_t)k_test_signed_img_ver, &trailer);

  /* Prepare contiguous slot payload: [body][trailer] padded to 32-byte multiple */
  uint8_t payload[k_test_program_len] = {};
  memcpy(payload, body, sizeof(body));
  memcpy(payload + sizeof(body), &trailer, sizeof(trailer));

  /* Direct gate verification on the contiguous image buffer */
  const ra8_rot_trailer_t* found_tr =
    ra8_rot_trailer_after(payload, (uint32_t)k_test_signed_body_len);
  TEST_ASSERT(found_tr != nullptr);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rot_verify_image(payload, (uint32_t)k_test_signed_body_len, found_tr));
  TEST_ASSERT_EQ((uint32_t)k_test_signed_img_ver, ra8_rot_trailer_image_version(found_tr));

  /* Exercise flash stage through ra8_dfu programming primitives */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dfu_program_prepare(k_ra8_dfu_slot_a));
  fake_mark_program_ready();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_dfu_program_image(k_ra8_dfu_slot_a, 0U, payload, (uint32_t)k_test_program_len));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_dfu_program_commit(k_ra8_dfu_slot_a, (uint32_t)k_test_program_len, (uint32_t)k_test_dfu_seq));
  TEST_ASSERT(ra8_dfu_slot_valid(k_ra8_dfu_slot_a));

  /* Locate and authenticate the committed image directly from slot memory */
  const uintptr_t slot_base = ra8_dfu_slot_base(k_ra8_dfu_slot_a);
  const ra8_rot_trailer_t* slot_tr =
    ra8_rot_trailer_after((const void*)slot_base, (uint32_t)k_test_signed_body_len);
  TEST_ASSERT(slot_tr != nullptr);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rot_verify_image((const uint8_t*)slot_base, (uint32_t)k_test_signed_body_len, slot_tr));

  TEST_END("dfu_signed: valid signed image accepted");
}

/**
 * @brief Tamper check: body modification causes SHA-256 mismatch and denial.
 */
static void test_dfu_signed_tamper_body_rejected(void)
{
  TEST_BEGIN("dfu_signed: tampered body rejected");

  uint8_t body[k_test_signed_body_len];
  fill_body_pattern(body, (uint32_t)k_test_signed_body_len);

  ra8_rot_trailer_t trailer = {};
  build_test_signed_trailer(body, (uint32_t)k_test_signed_body_len, (uint32_t)k_test_signed_img_ver, &trailer);

  /* Tamper first body byte */
  body[0] ^= (uint8_t)k_t_tamper_body_mask;
  TEST_ASSERT_EQ(k_ra8_err_checksum_mismatch,
                 ra8_rot_verify_image(body, (uint32_t)k_test_signed_body_len, &trailer));

  /* Restore and tamper middle body byte */
  body[0] ^= (uint8_t)k_t_tamper_body_mask;
  body[sizeof(body) / 2U] ^= (uint8_t)k_t_tamper_body_mask;
  TEST_ASSERT_EQ(k_ra8_err_checksum_mismatch,
                 ra8_rot_verify_image(body, (uint32_t)k_test_signed_body_len, &trailer));

  TEST_END("dfu_signed: tampered body rejected");
}

/**
 * @brief Tamper check: corrupted signature fails cryptographic verification.
 */
static void test_dfu_signed_tamper_signature_rejected(void)
{
  TEST_BEGIN("dfu_signed: corrupted signature rejected");

  uint8_t body[k_test_signed_body_len];
  fill_body_pattern(body, (uint32_t)k_test_signed_body_len);

  ra8_rot_trailer_t trailer = {};
  build_test_signed_trailer(body, (uint32_t)k_test_signed_body_len, (uint32_t)k_test_signed_img_ver, &trailer);

  /* Corrupt signature field */
  trailer.sig[0] ^= (uint8_t)k_t_tamper_sig_mask;
  const ra8_err_t err =
    ra8_rot_verify_image(body, (uint32_t)k_test_signed_body_len, &trailer);
  TEST_ASSERT(err == k_ra8_err_crc_mismatch);

  TEST_END("dfu_signed: corrupted signature rejected");
}

/**
 * @brief Format check: corrupted trailer magic or version is rejected immediately.
 */
static void test_dfu_signed_tamper_trailer_header_rejected(void)
{
  TEST_BEGIN("dfu_signed: bad magic and version rejected");

  uint8_t body[k_test_signed_body_len];
  fill_body_pattern(body, (uint32_t)k_test_signed_body_len);

  ra8_rot_trailer_t trailer = {};
  build_test_signed_trailer(body, (uint32_t)k_test_signed_body_len, (uint32_t)k_test_signed_img_ver, &trailer);

  /* Corrupt trailer magic */
  trailer.magic = (uint32_t)k_test_bad_magic;
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 ra8_rot_verify_image(body, (uint32_t)k_test_signed_body_len, &trailer));

  /* Restore magic, corrupt format version */
  trailer.magic   = (uint32_t)k_ra8_rot_trailer_magic;
  trailer.version = (uint32_t)k_test_bad_version;
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 ra8_rot_verify_image(body, (uint32_t)k_test_signed_body_len, &trailer));

  TEST_END("dfu_signed: bad magic and version rejected");
}

/**
 * @brief Length check: mismatched body_len or invalid sig_len is rejected.
 */
static void test_dfu_signed_tamper_length_rejected(void)
{
  TEST_BEGIN("dfu_signed: mismatched lengths rejected");

  uint8_t body[k_test_signed_body_len];
  fill_body_pattern(body, (uint32_t)k_test_signed_body_len);

  ra8_rot_trailer_t trailer = {};
  build_test_signed_trailer(body, (uint32_t)k_test_signed_body_len, (uint32_t)k_test_signed_img_ver, &trailer);

  /* Body len mismatch against caller parameter */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_rot_verify_image(body, (uint32_t)k_test_lied_body_len, &trailer));

  /* Zero body len */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_rot_verify_image(body, 0U, &trailer));

  /* Oversized body len exceeding 1 MiB cap */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_rot_verify_image(body, (uint32_t)k_ra8_rot_body_max + 1U, &trailer));

  /* Zero sig_len */
  trailer.sig_len = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_rot_verify_image(body, (uint32_t)k_test_signed_body_len, &trailer));

  /* Oversized sig_len past 64 bytes */
  trailer.sig_len = (uint32_t)k_ra8_rot_sig_bytes + 1U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_rot_verify_image(body, (uint32_t)k_test_signed_body_len, &trailer));

  TEST_END("dfu_signed: mismatched lengths rejected");
}

/**
 * @brief Guard check: NULL pointer arguments fail closed.
 */
static void test_dfu_signed_null_guards(void)
{
  TEST_BEGIN("dfu_signed: NULL guards fail closed");

  uint8_t body[k_test_signed_body_len];
  fill_body_pattern(body, (uint32_t)k_test_signed_body_len);

  ra8_rot_trailer_t trailer = {};
  build_test_signed_trailer(body, (uint32_t)k_test_signed_body_len, (uint32_t)k_test_signed_img_ver, &trailer);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rot_verify_image(nullptr, (uint32_t)k_test_signed_body_len, &trailer));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rot_verify_image(body, (uint32_t)k_test_signed_body_len, nullptr));

  TEST_ASSERT(ra8_rot_trailer_after(nullptr, (uint32_t)k_test_signed_body_len) == nullptr);
  TEST_ASSERT(ra8_rot_trailer_after(body, 0U) == nullptr);
  TEST_ASSERT(ra8_rot_trailer_after(body, (uint32_t)k_ra8_rot_body_max + 1U) == nullptr);
  TEST_ASSERT_EQ(0U, ra8_rot_trailer_image_version(nullptr));

  TEST_END("dfu_signed: NULL guards fail closed");
}

int main(void)
{
  (void)ra8_psa_crypto_init();

  test_dfu_signed_image_valid();
  test_dfu_signed_tamper_body_rejected();
  test_dfu_signed_tamper_signature_rejected();
  test_dfu_signed_tamper_trailer_header_rejected();
  test_dfu_signed_tamper_length_rejected();
  test_dfu_signed_null_guards();

  return 0;
}
