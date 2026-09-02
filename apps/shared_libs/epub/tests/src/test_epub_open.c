/**
 * @file test_epub_open.c
 * @brief MC/DC unit tests for apps/shared_libs/epub/src/epub_open.c
 * @details Proves the public open entry point rejects each invalid media and
 *          destination combination without dereferencing either argument.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "epub.h"
#include "epub_internal.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "unity_minimal.h"

typedef enum : uint8_t {
  k_test_epub_dummy_byte = 0x42U, /**< Test EPUB dummy byte. */
} test_epub_byte_t;

typedef enum : size_t {
  k_test_epub_size_zero    = 0U,  /**< Test EPUB size zero.    */
  k_test_epub_size_nonzero = 16U, /**< Test EPUB size nonzero. */
} test_epub_size_t;

static uint8_t s_blob[16];

/**
 * @var s_stream_reads
 * @brief Count of ::internal_stream_fixture_read invocations.
 * @details Lets a test assert that ``priv_epub_stream_read`` short-circuited
 *          -- returning 0 WITHOUT reaching the media callback -- rather than
 *          calling through and having the callback happen to return 0.
 * @note Reset by the case that reads it; not thread-safe.
 * @since 0.1.0
 */
static uint32_t s_stream_reads = 0U;

/**
 * @brief Serve one bounded byte span to the streamed-read MC/DC fixture
 * @details Copies the requested range from the immutable fixture into the
 * destination and returns the exact requested length.
 * @param[in] ctx Fixture byte array.
 * @param[in] offset Starting byte offset in the fixture.
 * @param[out] buf Destination writable for @p len bytes.
 * @param[in] len Number of fixture bytes to copy.
 * @return The exact requested byte count.
 * @retval len The fixture supplied the complete request.
 * @retval 0 The opaque context does not identify the fixture byte array.
 * @pre @p ctx and @p buf are non-null.
 * @pre The range `offset + len` is inside the fixture.
 * @post When @p ctx identifies the fixture, exactly @p len bytes are written to @p buf.
 * @post A rejected context leaves @p buf unchanged; the fixture remains unchanged
 *       on every path.
 * @note Reentrant for callers that provide disjoint destination buffers.
 * @since 0.1.0
 */
RA8_INTERNAL static size_t
internal_stream_fixture_read(void* ctx, uint64_t offset, void* buf, size_t len)
{
  size_t result = 0U;
  ++s_stream_reads;
  if (ctx == s_blob) {
    (void)memcpy(buf, &s_blob[(size_t)offset], len);
    result = len;
  }
  return result;
}

/**
 * @test internal_test_mcdc_epub_open_media_or_book_null
 *
 * @par MC/DC:
 * Decision: ``if (media == NULL || out_book == NULL)``
 * (2 conditions, apps/shared_libs/epub/src/epub_open.c around line 326)
 * Per DO-178C 6.4.4.3 N+1 = 3 vectors. @brief Verify mcdc epub open media or book null behavior. @details Executes the mcdc epub open media or book null scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_mcdc_epub_open_media_or_book_null(void)
{
  TEST_BEGIN("epub_open MC/DC: (media==NULL || out_book==NULL)");
  epub_book_t            book  = {};
  const epub_mem_media_t media = {.data = s_blob, .size = (size_t)k_test_epub_size_nonzero};
  s_blob[0]                    = (uint8_t)k_test_epub_dummy_byte;
  TEST_ASSERT(epub_open(&media, nullptr, &book) != k_ra8_err_null_ptr);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, epub_open(nullptr, nullptr, &book));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, epub_open(&media, nullptr, nullptr));
  TEST_END("epub_open MC/DC: (media==NULL || out_book==NULL)");
}

/**
 * @test internal_test_mcdc_epub_open_mem_data_or_size
 *
 * @par MC/DC:
 * Decision: ``if (mem->data == NULL || mem->size == 0U)``
 * (2 conditions, apps/shared_libs/epub/src/epub_open.c around line 330)
 * Per DO-178C 6.4.4.3 N+1 = 3 vectors. @brief Verify mcdc epub open mem data or size behavior. @details Executes the mcdc epub open mem data or size scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_mcdc_epub_open_mem_data_or_size(void)
{
  TEST_BEGIN("epub_open MC/DC: (mem->data==NULL || mem->size==0)");
  epub_book_t            book = {};
  const epub_mem_media_t v1m  = {.data = s_blob, .size = (size_t)k_test_epub_size_nonzero};
  TEST_ASSERT(epub_open(&v1m, nullptr, &book) != k_ra8_err_invalid_arg);
  const epub_mem_media_t v2m = {.data = nullptr, .size = (size_t)k_test_epub_size_nonzero};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, epub_open(&v2m, nullptr, &book));
  const epub_mem_media_t v3m = {.data = s_blob, .size = (size_t)k_test_epub_size_zero};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, epub_open(&v3m, nullptr, &book));
  TEST_END("epub_open MC/DC: (mem->data==NULL || mem->size==0)");
}

/**
 * @test internal_test_mcdc_epub_open_private_guards
 * @brief Drive the three defensive OR guards behind the public open paths.
 *
 * @par MC/DC:
 * Decision owners and vectors:
 * - apps/shared_libs/epub/src/epub_open.c@priv_epub_dirname
 *   `(dst == nullptr) || (cap == 0U)`: all-valid, null-dst, and zero-capacity.
 * - apps/shared_libs/epub/src/epub_open.c@priv_epub_dirname
 *   `path == nullptr` (1 condition): V1 path="OPS/book.opf" -> false, the
 *   scan runs (the first call below); V2 path=NULL -> true, the function
 *   returns with only dst[0] cleared. N+1 = 2.
 * - apps/shared_libs/epub/src/epub_open.c@priv_epub_dirname
 *   `len >= cap` (1 condition): V1 cap=16 with a 4-char "OPS/" prefix ->
 *   false, the whole prefix is copied; V2 cap=4 with the same path -> true,
 *   the copy is clamped to cap-1 and yields "OPS". N+1 = 2.
 * - apps/shared_libs/epub/src/epub_open.c@priv_epub_stream_read
 *   `(sm == nullptr) || (sm->read == nullptr)`: all-valid, null descriptor,
 *   and null callback.
 * - apps/shared_libs/epub/src/epub_open.c@priv_epub_stream_read
 *   `file_ofs >= sm->size` (1 condition): V1 file_ofs=0 with size=1 ->
 *   false, the media callback runs and delivers a byte; V2 file_ofs=1 with
 *   size=1 -> true, the call returns 0 and the callback is NOT reached (the
 *   invocation counter is asserted unchanged). N+1 = 2.
 * - apps/shared_libs/epub/src/epub_open.c@priv_epub_finish_open
 *   `(zip == nullptr) || (out_book == nullptr)`: the two one-null vectors are
 *   below; valid EPUB opens in test_epub_open_cov.c supply the all-valid control.
 * @pre Fixture buffers remain live through every callback invocation.
 * @post Each defensive guard rejects only its independently varied operand.
 * @note Uses the private test-access surface; the public EPUB ABI is unchanged.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_epub_open_private_guards(void)
{
  TEST_BEGIN("epub_open MC/DC: private defensive OR guards");
  char directory[16] = {[0] = '\0'};
  priv_epub_dirname("OPS/book.opf", directory, sizeof(directory));
  TEST_ASSERT_EQ(0, strcmp(directory, "OPS/"));
  priv_epub_dirname("OPS/book.opf", nullptr, sizeof(directory));
  directory[0] = 'x';
  priv_epub_dirname("OPS/book.opf", directory, 0U);
  TEST_ASSERT_EQ('x', directory[0]);

  /* `path == nullptr`, true arm: dst[0] is cleared and nothing else happens. */
  directory[0] = 'y';
  priv_epub_dirname(nullptr, directory, sizeof(directory));
  TEST_ASSERT_EQ('\0', directory[0]);

  /* `len >= cap`, true arm: "OPS/" needs 4 bytes but only cap-1 = 3 fit, so
     the prefix is clamped rather than written past the end of the buffer. */
  char narrow[4] = {[0] = 'z'};
  priv_epub_dirname("OPS/book.opf", narrow, sizeof(narrow));
  TEST_ASSERT_EQ(0, strcmp(narrow, "OPS"));

  s_blob[0]                  = (uint8_t)k_test_epub_dummy_byte;
  uint8_t             output = 0U;
  epub_stream_media_t media  = {.read = internal_stream_fixture_read,
                                .ctx  = s_blob,
                                .size = sizeof(s_blob[0])};
  TEST_ASSERT_EQ(1U, priv_epub_stream_read(&media, 0U, &output, 1U));
  TEST_ASSERT_EQ(s_blob[0], output);

  /* `file_ofs >= sm->size`, true arm: the request starts at the one-past-end
     offset, so the call must short-circuit to 0 without touching the media --
     the callback counter proves it was never reached. */
  const uint32_t reads_before = s_stream_reads;
  TEST_ASSERT_EQ(0U, priv_epub_stream_read(&media, 1U, &output, 1U));
  TEST_ASSERT_EQ(reads_before, s_stream_reads);

  TEST_ASSERT_EQ(0U, priv_epub_stream_read(nullptr, 0U, &output, 1U));
  media.read = nullptr;
  TEST_ASSERT_EQ(0U, priv_epub_stream_read(&media, 0U, &output, 1U));

  mz_zip_archive zip  = {};
  epub_book_t    book = {.in_use = 0U};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, priv_epub_finish_open(nullptr, &book));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, priv_epub_finish_open(&zip, nullptr));
  TEST_END("epub_open MC/DC: private defensive OR guards");
}

int main(void)
{
  internal_test_mcdc_epub_open_media_or_book_null();
  internal_test_mcdc_epub_open_mem_data_or_size();
  internal_test_mcdc_epub_open_private_guards();
  return 0;
}
