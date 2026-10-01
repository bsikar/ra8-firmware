/**
 * @file test_ra8_appimg.c
 * @brief Unit tests for the `.ra8app` container header (RA8FW-293).
 *
 * @details
 * Exercises the three answers the loader needs before it trusts a byte of a
 * module image: identity (magic, format version, API generation, text-field
 * termination, undefined capability bits), declared sizes against both their
 * caps and the real file length, and the signed / payload spans the signer and
 * the RA8FW-291 verifier must agree on. Also covers the grant comparison, including
 * the case an app declares more than the host is willing to give.
 *
 * Pure format policy: nothing is hashed, nothing is loaded, no filesystem is
 * touched.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_appimg.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/**
 * @enum appimg_test_size_t
 * @brief Fixture magnitudes shared by the cases.
 */
typedef enum : uint32_t {
  k_appimg_test_code  = 256U,  /**< Instruction-area length of the fixture. */
  k_appimg_test_data  = 128U,  /**< Data-area length of the fixture.        */
  k_appimg_test_stack = 2048U, /**< Stack the fixture asks for.             */
  k_appimg_test_file  = (uint32_t)sizeof(ra8_appimg_header_t) + 256U + 128U,
                               /**< Exact file length the fixture implies. */
} appimg_test_size_t;

/**
 * @brief Build a header that parses, so each case can spoil one field.
 * @return A well-formed fixture header.
 */
static ra8_appimg_header_t internal_good_header(void)
{
  ra8_appimg_header_t hdr = {};
  hdr.magic           = (uint32_t)k_ra8_appimg_magic;
  hdr.format_version  = (uint32_t)k_ra8_appimg_format_version;
  hdr.entry_offset    = 0U;
  hdr.code_size       = (uint32_t)k_appimg_test_code;
  hdr.data_size       = (uint32_t)k_appimg_test_data;
  hdr.stack_size      = (uint32_t)k_appimg_test_stack;
  hdr.min_api_version = (uint32_t)k_ra8_appimg_api_version_current;
  hdr.capabilities    = (uint32_t)k_ra8_appimg_cap_display;
  (void)strcpy(hdr.app_id, "com.example.reader");
  (void)strcpy(hdr.display_name, "Reader");
  return hdr;
}

/**
 * @brief Serialise a fixture header into a file-sized byte buffer.
 * @param[in]  hdr   Header to place at the head of the image.
 * @param[out] bytes Destination image, at least ::k_appimg_test_file bytes.
 */
static void internal_serialise(const ra8_appimg_header_t* hdr, uint8_t* bytes)
{
  (void)memset(bytes, 0, (size_t)k_appimg_test_file);
  (void)memcpy(bytes, hdr, sizeof(*hdr));
}

/**
 * @test test_appimg_parses_well_formed
 * @brief A sound container parses and every field survives the round trip.
 * @pre None.
 * @post The parsed header equals the serialised one.
 * @note Single-threaded host test.
 * @since 0.1.0
 */
static void test_appimg_parses_well_formed(void)
{
  TEST_BEGIN("appimg: well-formed header parses");
  const ra8_appimg_header_t src = internal_good_header();
  uint8_t bytes[k_appimg_test_file];
  internal_serialise(&src, bytes);

  ra8_appimg_header_t out = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_appimg_parse(bytes, sizeof(bytes), &out));
  TEST_ASSERT_EQ((uint32_t)k_ra8_appimg_magic, out.magic);
  TEST_ASSERT_EQ((uint32_t)k_appimg_test_code, out.code_size);
  TEST_ASSERT_EQ((uint32_t)k_appimg_test_data, out.data_size);
  TEST_ASSERT(strcmp(out.app_id, "com.example.reader") == 0);
  TEST_ASSERT(strcmp(out.display_name, "Reader") == 0);
  TEST_END("appimg: well-formed header parses");
}

/**
 * @test test_appimg_refuses_bad_identity
 * @brief Wrong magic, wrong format version and a too-new API are all refused.
 * @pre None.
 * @post The caller's output is untouched on every refusal.
 * @note Single-threaded host test.
 * @since 0.1.0
 */
static void test_appimg_refuses_bad_identity(void)
{
  TEST_BEGIN("appimg: bad identity refused");
  uint8_t bytes[k_appimg_test_file];
  ra8_appimg_header_t out = {};

  ra8_appimg_header_t hdr = internal_good_header();
  hdr.magic = 0xDEADBEEFU;
  internal_serialise(&hdr, bytes);
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, ra8_appimg_parse(bytes, sizeof(bytes), &out));

  hdr = internal_good_header();
  hdr.format_version = (uint32_t)k_ra8_appimg_format_version + 1U;
  internal_serialise(&hdr, bytes);
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, ra8_appimg_parse(bytes, sizeof(bytes), &out));

  hdr = internal_good_header();
  hdr.min_api_version = (uint32_t)k_ra8_appimg_api_version_current + 1U;
  internal_serialise(&hdr, bytes);
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_appimg_parse(bytes, sizeof(bytes), &out));
  TEST_ASSERT_EQ(0U, out.magic);
  TEST_END("appimg: bad identity refused");
}

/**
 * @test test_appimg_refuses_unknown_capability_bit
 * @brief A capability bit this revision does not define invalidates the header.
 * @pre None.
 * @post Nothing is loaded and no permission is silently dropped.
 * @note Single-threaded host test.
 * @since 0.1.0
 */
static void test_appimg_refuses_unknown_capability_bit(void)
{
  TEST_BEGIN("appimg: undefined capability bit refused");
  ra8_appimg_header_t hdr = internal_good_header();
  hdr.capabilities = (uint32_t)k_ra8_appimg_cap_known | 0x80000000U;
  uint8_t bytes[k_appimg_test_file];
  internal_serialise(&hdr, bytes);

  ra8_appimg_header_t out = {};
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, ra8_appimg_parse(bytes, sizeof(bytes), &out));
  TEST_END("appimg: undefined capability bit refused");
}

/**
 * @test test_appimg_refuses_unterminated_text
 * @brief A text field with no NUL inside its width is malformed.
 * @pre None.
 * @post No unterminated field ever reaches a caller's string handling.
 * @note Single-threaded host test.
 * @since 0.1.0
 */
static void test_appimg_refuses_unterminated_text(void)
{
  TEST_BEGIN("appimg: unterminated text field refused");
  ra8_appimg_header_t hdr = internal_good_header();
  (void)memset(hdr.app_id, 'a', sizeof(hdr.app_id));
  uint8_t bytes[k_appimg_test_file];
  internal_serialise(&hdr, bytes);

  ra8_appimg_header_t out = {};
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, ra8_appimg_parse(bytes, sizeof(bytes), &out));

  hdr = internal_good_header();
  hdr.app_id[0] = '\0';
  internal_serialise(&hdr, bytes);
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, ra8_appimg_parse(bytes, sizeof(bytes), &out));
  TEST_END("appimg: unterminated text field refused");
}

/**
 * @test test_appimg_refuses_bad_sizes
 * @brief Zero code, an over-cap segment, a tiny stack and a stray entry all fail.
 * @pre None.
 * @post No length the loader would map survives unchecked.
 * @note Single-threaded host test.
 * @since 0.1.0
 */
static void test_appimg_refuses_bad_sizes(void)
{
  TEST_BEGIN("appimg: bad declared sizes refused");
  uint8_t bytes[k_appimg_test_file];
  ra8_appimg_header_t out = {};

  ra8_appimg_header_t hdr = internal_good_header();
  hdr.code_size = 0U;
  internal_serialise(&hdr, bytes);
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_appimg_parse(bytes, sizeof(bytes), &out));

  hdr = internal_good_header();
  hdr.data_size = (uint32_t)k_ra8_appimg_segment_max + 1U;
  internal_serialise(&hdr, bytes);
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_appimg_parse(bytes, sizeof(bytes), &out));

  hdr = internal_good_header();
  hdr.stack_size = (uint32_t)k_ra8_appimg_stack_min - 1U;
  internal_serialise(&hdr, bytes);
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_appimg_parse(bytes, sizeof(bytes), &out));

  hdr = internal_good_header();
  hdr.entry_offset = hdr.code_size;
  internal_serialise(&hdr, bytes);
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_appimg_parse(bytes, sizeof(bytes), &out));
  TEST_END("appimg: bad declared sizes refused");
}

/**
 * @test test_appimg_refuses_short_file
 * @brief A file shorter than the header, or than the payload it declares, fails.
 * @pre None.
 * @post A truncated download can never be mapped.
 * @note Single-threaded host test.
 * @since 0.1.0
 */
static void test_appimg_refuses_short_file(void)
{
  TEST_BEGIN("appimg: short file refused");
  const ra8_appimg_header_t hdr = internal_good_header();
  uint8_t bytes[k_appimg_test_file];
  internal_serialise(&hdr, bytes);

  ra8_appimg_header_t out = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_appimg_parse(bytes, sizeof(ra8_appimg_header_t) - 1U, &out));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_appimg_parse(bytes, sizeof(ra8_appimg_header_t), &out));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_appimg_parse(nullptr, sizeof(bytes), &out));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_appimg_parse(bytes, sizeof(bytes), nullptr));
  TEST_END("appimg: short file refused");
}

/**
 * @test test_appimg_spans_cover_everything_but_the_signature
 * @brief The two spans together are the whole file minus the signature field.
 * @pre None.
 * @post Signer and verifier can derive the same signed material.
 * @note Single-threaded host test.
 * @since 0.1.0
 */
static void test_appimg_spans_cover_everything_but_the_signature(void)
{
  TEST_BEGIN("appimg: spans cover all but the signature");
  const ra8_appimg_header_t hdr = internal_good_header();

  ra8_appimg_span_t signed_run = {};
  ra8_appimg_span_t payload    = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_appimg_signed_span(&hdr, (size_t)k_appimg_test_file, &signed_run));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_appimg_payload_span(&hdr, (size_t)k_appimg_test_file, &payload));

  TEST_ASSERT_EQ(0U, signed_run.offset);
  TEST_ASSERT_EQ((uint32_t)offsetof(ra8_appimg_header_t, signature), signed_run.length);
  TEST_ASSERT_EQ((uint32_t)sizeof(ra8_appimg_header_t), payload.offset);
  TEST_ASSERT_EQ((uint32_t)k_appimg_test_code + (uint32_t)k_appimg_test_data, payload.length);

  const uint32_t covered = signed_run.length + payload.length;
  TEST_ASSERT_EQ((uint32_t)k_appimg_test_file - (uint32_t)k_ra8_appimg_sig_bytes, covered);
  TEST_END("appimg: spans cover all but the signature");
}

/**
 * @test test_appimg_spans_refuse_bad_input
 * @brief Null arguments and a too-short file zero the span rather than guess.
 * @pre None.
 * @post A refused span can never be mistaken for a zero-length signed region.
 * @note Single-threaded host test.
 * @since 0.1.0
 */
static void test_appimg_spans_refuse_bad_input(void)
{
  TEST_BEGIN("appimg: spans refuse bad input");
  const ra8_appimg_header_t hdr = internal_good_header();
  ra8_appimg_span_t span        = {.offset = 7U, .length = 9U};

  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_appimg_signed_span(&hdr, sizeof(ra8_appimg_header_t) - 1U, &span));
  TEST_ASSERT_EQ(0U, span.offset);
  TEST_ASSERT_EQ(0U, span.length);

  span.length = 9U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_appimg_payload_span(&hdr, sizeof(ra8_appimg_header_t), &span));
  TEST_ASSERT_EQ(0U, span.length);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_appimg_signed_span(nullptr, (size_t)k_appimg_test_file, &span));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_appimg_payload_span(&hdr, (size_t)k_appimg_test_file, nullptr));
  TEST_END("appimg: spans refuse bad input");
}

/**
 * @test test_appimg_capability_grant
 * @brief A grant must cover every declared bit; a stray grant bit is malformed.
 * @pre None.
 * @post An app can never run with a capability the host withheld.
 * @note Single-threaded host test.
 * @since 0.1.0
 */
static void test_appimg_capability_grant(void)
{
  TEST_BEGIN("appimg: capability grant compared");
  ra8_appimg_header_t hdr = internal_good_header();
  hdr.capabilities        = (uint32_t)k_ra8_appimg_cap_display |
                            (uint32_t)k_ra8_appimg_cap_storage;

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_appimg_capabilities_permitted(&hdr, (uint32_t)k_ra8_appimg_cap_known));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_appimg_capabilities_permitted(&hdr, hdr.capabilities));
  TEST_ASSERT_EQ(k_ra8_err_access_denied,
                 ra8_appimg_capabilities_permitted(&hdr, (uint32_t)k_ra8_appimg_cap_display));
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 ra8_appimg_capabilities_permitted(&hdr, 0x80000000U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_appimg_capabilities_permitted(nullptr, (uint32_t)k_ra8_appimg_cap_none));

  hdr.capabilities = (uint32_t)k_ra8_appimg_cap_none;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_appimg_capabilities_permitted(&hdr, (uint32_t)k_ra8_appimg_cap_none));
  TEST_END("appimg: capability grant compared");
}

/**
 * @brief Run every `.ra8app` header case in order.
 * @return 0 when all cases pass; an assertion exits non-zero on first failure.
 */
int main(void)
{
  test_appimg_parses_well_formed();
  test_appimg_refuses_bad_identity();
  test_appimg_refuses_unknown_capability_bit();
  test_appimg_refuses_unterminated_text();
  test_appimg_refuses_bad_sizes();
  test_appimg_refuses_short_file();
  test_appimg_spans_cover_everything_but_the_signature();
  test_appimg_spans_refuse_bad_input();
  test_appimg_capability_grant();
  return 0;
}
