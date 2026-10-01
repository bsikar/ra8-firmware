/**
 * @file test_ra8_appimg_verify.c
 * @brief Unit tests for the `.ra8app` admission gate (RA8FW-291).
 *
 * @details
 * Proves the policy, not the primitive. A recording stand-in stands in for the
 * Ed25519 backend, so each case can assert both the verdict the gate reached
 * and whether the backend was consulted at all -- which is how the ordering
 * claims ("a refused manifest never spends a signature", "an unsigned image is
 * refused without a backend") are actually tested rather than asserted in prose.
 *
 * Covers the three acceptance criteria of RA8FW-291 directly: an unsigned module is
 * rejected, a module modified after signing is rejected, and a validly signed
 * module is admitted.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_appimg.h"
#include "ra8_appimg_verify.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/**
 * @enum verify_test_size_t
 * @brief Fixture magnitudes shared by the cases.
 */
typedef enum : uint32_t {
  k_verify_test_code  = 256U,  /**< Instruction-area length of the fixture. */
  k_verify_test_data  = 128U,  /**< Data-area length of the fixture.        */
  k_verify_test_stack = 2048U, /**< Stack the fixture asks for.             */
  k_verify_test_file  = (uint32_t)sizeof(ra8_appimg_header_t) + 256U + 128U,
                               /**< Exact file length the fixture implies. */
} verify_test_size_t;

/**
 * @struct verify_spy_t
 * @brief What the stand-in backend saw, and what it should answer.
 */
typedef struct {
  uint32_t  calls;       /**< How many times the backend was consulted.  */
  uint32_t  head_len;    /**< `head_len` of the last message seen.       */
  uint32_t  tail_len;    /**< `tail_len` of the last message seen.       */
  uint8_t   first_byte;  /**< First byte of the last `head` run seen.    */
  uint8_t   key_byte;    /**< First byte of the pinned key it was given. */
  ra8_err_t answer;      /**< Verdict the stand-in returns.              */
} verify_spy_t;

/**
 * @brief Recording Ed25519 stand-in: answers what the fixture told it to.
 * @param[in] ctx        The ::verify_spy_t under test.
 * @param[in] msg        Signed material handed over by the gate.
 * @param[in] signature  Signature field of the image.
 * @param[in] public_key Pinned key the gate was configured with.
 * @return The verdict recorded in the spy.
 */
static ra8_err_t
internal_spy_verify(void*                   ctx,
                    const ra8_appimg_msg_t* msg,
                    const uint8_t*          signature,
                    const uint8_t*          public_key)
{
  verify_spy_t* spy = (verify_spy_t*)ctx;
  spy->calls += 1U;
  spy->head_len   = msg->head_len;
  spy->tail_len   = msg->tail_len;
  spy->first_byte = msg->head[0];
  spy->key_byte   = public_key[0];
  (void)signature;
  return spy->answer;
}

/**
 * @brief Build a header that parses and carries a non-zero signature.
 * @return A well-formed, "signed" fixture header.
 */
static ra8_appimg_header_t
internal_good_header(void)
{
  ra8_appimg_header_t hdr = {};
  hdr.magic               = (uint32_t)k_ra8_appimg_magic;
  hdr.format_version      = (uint32_t)k_ra8_appimg_format_version;
  hdr.entry_offset        = 0U;
  hdr.code_size           = (uint32_t)k_verify_test_code;
  hdr.data_size           = (uint32_t)k_verify_test_data;
  hdr.stack_size          = (uint32_t)k_verify_test_stack;
  hdr.min_api_version     = (uint32_t)k_ra8_appimg_api_version_current;
  hdr.capabilities        = (uint32_t)k_ra8_appimg_cap_display;
  (void)memcpy(hdr.app_id, "com.ra8.reader", 15U);
  (void)memcpy(hdr.display_name, "Reader", 7U);
  for (size_t i = 0U; i < (size_t)k_ra8_appimg_sig_bytes; ++i) {
    hdr.signature[i] = (uint8_t)(i + 1U);
  }
  return hdr;
}

/**
 * @brief Serialize a header plus a deterministic payload into a file image.
 * @param[in]  hdr   Header to place at offset zero.
 * @param[out] image Buffer of at least ::k_verify_test_file bytes.
 */
static void
internal_build_image(const ra8_appimg_header_t* hdr, uint8_t* image)
{
  (void)memcpy(image, hdr, sizeof(*hdr));
  for (size_t i = sizeof(*hdr); i < (size_t)k_verify_test_file; ++i) {
    image[i] = (uint8_t)(i & 0xFFU);
  }
}

/** @brief A validly signed image is admitted and the header is published. */
static void
internal_test_valid_image_admitted(void)
{
  TEST_BEGIN("valid image admitted");

  uint8_t                   image[k_verify_test_file] = {};
  const ra8_appimg_header_t hdr                       = internal_good_header();
  internal_build_image(&hdr, image);

  uint8_t      key[k_ra8_appimg_pubkey_bytes] = {0xA5U};
  verify_spy_t spy                            = {.answer = k_ra8_ok};
  const ra8_appimg_verifier_t gate            = {.verify     = internal_spy_verify,
                                                 .verify_ctx = &spy,
                                                 .public_key = key,
                                                 .granted = (uint32_t)k_ra8_appimg_cap_display};

  ra8_appimg_header_t out = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_appimg_verify(&gate, image, sizeof(image), &out));
  TEST_ASSERT_EQ((uint32_t)k_ra8_appimg_magic, out.magic);
  TEST_ASSERT_EQ((uint32_t)k_verify_test_code, out.code_size);
  TEST_ASSERT_EQ(1U, spy.calls);
  TEST_ASSERT_EQ(0xA5U, spy.key_byte);

  TEST_END("valid image admitted");
}

/** @brief The backend sees exactly the two signed runs, signature excised. */
static void
internal_test_message_is_two_runs(void)
{
  TEST_BEGIN("message is two runs");

  uint8_t                   image[k_verify_test_file] = {};
  const ra8_appimg_header_t hdr                       = internal_good_header();
  internal_build_image(&hdr, image);

  uint8_t      key[k_ra8_appimg_pubkey_bytes] = {0x11U};
  verify_spy_t spy                            = {.answer = k_ra8_ok};
  const ra8_appimg_verifier_t gate            = {.verify     = internal_spy_verify,
                                                 .verify_ctx = &spy,
                                                 .public_key = key,
                                                 .granted = (uint32_t)k_ra8_appimg_cap_display};

  ra8_appimg_header_t out = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_appimg_verify(&gate, image, sizeof(image), &out));
  TEST_ASSERT_EQ((uint32_t)offsetof(ra8_appimg_header_t, signature), spy.head_len);
  TEST_ASSERT_EQ((uint32_t)(k_verify_test_code + k_verify_test_data), spy.tail_len);
  TEST_ASSERT(spy.head_len + spy.tail_len + (uint32_t)k_ra8_appimg_sig_bytes ==
              (uint32_t)k_verify_test_file);

  TEST_END("message is two runs");
}

/** @brief An all-zero signature is refused without consulting the backend. */
static void
internal_test_unsigned_refused(void)
{
  TEST_BEGIN("unsigned image refused");

  uint8_t             image[k_verify_test_file] = {};
  ra8_appimg_header_t hdr                       = internal_good_header();
  (void)memset(hdr.signature, 0, sizeof(hdr.signature));
  internal_build_image(&hdr, image);

  uint8_t      key[k_ra8_appimg_pubkey_bytes] = {0x22U};
  verify_spy_t spy                            = {.answer = k_ra8_ok};
  const ra8_appimg_verifier_t gate            = {.verify     = internal_spy_verify,
                                                 .verify_ctx = &spy,
                                                 .public_key = key,
                                                 .granted = (uint32_t)k_ra8_appimg_cap_display};

  ra8_appimg_header_t out = {};
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 ra8_appimg_verify(&gate, image, sizeof(image), &out));
  TEST_ASSERT_EQ(0U, spy.calls);
  TEST_ASSERT_EQ(0U, out.magic);

  TEST_END("unsigned image refused");
}

/** @brief A backend rejection becomes a tamper verdict, header withheld. */
static void
internal_test_tampered_refused(void)
{
  TEST_BEGIN("tampered image refused");

  uint8_t                   image[k_verify_test_file] = {};
  const ra8_appimg_header_t hdr                       = internal_good_header();
  internal_build_image(&hdr, image);
  image[sizeof(ra8_appimg_header_t) + 4U] ^= 0xFFU;

  uint8_t      key[k_ra8_appimg_pubkey_bytes] = {0x33U};
  verify_spy_t spy                            = {.answer = k_ra8_err_validation_failed};
  const ra8_appimg_verifier_t gate            = {.verify     = internal_spy_verify,
                                                 .verify_ctx = &spy,
                                                 .public_key = key,
                                                 .granted = (uint32_t)k_ra8_appimg_cap_display};

  ra8_appimg_header_t out = {};
  TEST_ASSERT_EQ(k_ra8_err_crc_mismatch, ra8_appimg_verify(&gate, image, sizeof(image), &out));
  TEST_ASSERT_EQ(1U, spy.calls);
  TEST_ASSERT_EQ(0U, out.magic);

  TEST_END("tampered image refused");
}

/** @brief A withheld capability refuses before the signature is spent. */
static void
internal_test_grant_refused_before_backend(void)
{
  TEST_BEGIN("withheld capability refused first");

  uint8_t             image[k_verify_test_file] = {};
  ra8_appimg_header_t hdr                       = internal_good_header();
  hdr.capabilities = (uint32_t)k_ra8_appimg_cap_display | (uint32_t)k_ra8_appimg_cap_network;
  internal_build_image(&hdr, image);

  uint8_t      key[k_ra8_appimg_pubkey_bytes] = {0x44U};
  verify_spy_t spy                            = {.answer = k_ra8_ok};
  const ra8_appimg_verifier_t gate            = {.verify     = internal_spy_verify,
                                                 .verify_ctx = &spy,
                                                 .public_key = key,
                                                 .granted = (uint32_t)k_ra8_appimg_cap_display};

  ra8_appimg_header_t out = {};
  TEST_ASSERT_EQ(k_ra8_err_access_denied, ra8_appimg_verify(&gate, image, sizeof(image), &out));
  TEST_ASSERT_EQ(0U, spy.calls);

  TEST_END("withheld capability refused first");
}

/** @brief A malformed container refuses before the backend is consulted. */
static void
internal_test_malformed_refused_before_backend(void)
{
  TEST_BEGIN("malformed container refused first");

  uint8_t             image[k_verify_test_file] = {};
  ra8_appimg_header_t hdr                       = internal_good_header();
  hdr.magic                                     = 0xDEADBEEFU;
  internal_build_image(&hdr, image);

  uint8_t      key[k_ra8_appimg_pubkey_bytes] = {0x55U};
  verify_spy_t spy                            = {.answer = k_ra8_ok};
  const ra8_appimg_verifier_t gate            = {.verify     = internal_spy_verify,
                                                 .verify_ctx = &spy,
                                                 .public_key = key,
                                                 .granted = (uint32_t)k_ra8_appimg_cap_display};

  ra8_appimg_header_t out = {};
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 ra8_appimg_verify(&gate, image, sizeof(image), &out));
  TEST_ASSERT_EQ(0U, spy.calls);

  TEST_END("malformed container refused first");
}

/** @brief A backend without Ed25519 reports that, not a tamper verdict. */
static void
internal_test_backend_absent_reported(void)
{
  TEST_BEGIN("absent backend reported");

  uint8_t                   image[k_verify_test_file] = {};
  const ra8_appimg_header_t hdr                       = internal_good_header();
  internal_build_image(&hdr, image);

  uint8_t      key[k_ra8_appimg_pubkey_bytes] = {0x66U};
  verify_spy_t spy                            = {.answer = k_ra8_err_not_supported};
  const ra8_appimg_verifier_t gate            = {.verify     = internal_spy_verify,
                                                 .verify_ctx = &spy,
                                                 .public_key = key,
                                                 .granted = (uint32_t)k_ra8_appimg_cap_display};

  ra8_appimg_header_t out = {};
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_appimg_verify(&gate, image, sizeof(image), &out));
  TEST_ASSERT_EQ(0U, out.magic);

  TEST_END("absent backend reported");
}

/** @brief A verifier missing its backend or its pinned key is refused. */
static void
internal_test_verifier_incomplete(void)
{
  TEST_BEGIN("incomplete verifier refused");

  uint8_t                   image[k_verify_test_file] = {};
  const ra8_appimg_header_t hdr                       = internal_good_header();
  internal_build_image(&hdr, image);

  uint8_t             key[k_ra8_appimg_pubkey_bytes] = {0x77U};
  verify_spy_t        spy                            = {.answer = k_ra8_ok};
  ra8_appimg_header_t out                            = {};

  const ra8_appimg_verifier_t no_backend = {
    .verify = nullptr, .verify_ctx = &spy, .public_key = key, .granted = 0U};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_appimg_verify(&no_backend, image, sizeof(image), &out));

  const ra8_appimg_verifier_t no_key = {
    .verify = internal_spy_verify, .verify_ctx = &spy, .public_key = nullptr, .granted = 0U};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_appimg_verify(&no_key, image, sizeof(image), &out));
  TEST_ASSERT_EQ(0U, spy.calls);

  TEST_END("incomplete verifier refused");
}

/** @brief The signed-message view refuses a truncated file and zeroes out. */
static void
internal_test_signed_message_truncated(void)
{
  TEST_BEGIN("signed message truncated");

  uint8_t                   image[k_verify_test_file] = {};
  const ra8_appimg_header_t hdr                       = internal_good_header();
  internal_build_image(&hdr, image);

  ra8_appimg_msg_t msg = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_appimg_signed_message(&hdr, image, sizeof(ra8_appimg_header_t), &msg));
  TEST_ASSERT_NULL(msg.head);
  TEST_ASSERT_EQ(0U, msg.tail_len);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_appimg_signed_message(&hdr, nullptr, sizeof(image), &msg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_appimg_signed_message(&hdr, image, sizeof(image), &msg));
  TEST_ASSERT_NOT_NULL(msg.head);
  TEST_ASSERT_NOT_NULL(msg.tail);

  TEST_END("signed message truncated");
}

/**
 * @brief Run every case in order.
 * @return 0 always; a failed assertion aborts before the return.
 */
int
main(void)
{
  internal_test_valid_image_admitted();
  internal_test_message_is_two_runs();
  internal_test_unsigned_refused();
  internal_test_tampered_refused();
  internal_test_grant_refused_before_backend();
  internal_test_malformed_refused_before_backend();
  internal_test_backend_absent_reported();
  internal_test_verifier_incomplete();
  internal_test_signed_message_truncated();
  return 0;
}
