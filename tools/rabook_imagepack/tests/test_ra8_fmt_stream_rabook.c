/**
* @file test_ra8_fmt_stream_rabook.c
* @brief Strict streamed RBKC inspector tests over a production-built book.
 * @details Builds real bounded RBKC fixtures and verifies strict streamed inspection, corruption rejection, reporting, and workspace limits.
* @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fmt_stream.h"
#include "ra8_io_compress.h"
#include "ra8_log.h"
#include "ra8_rabook_compile.h"
#include "ra8_rabook_container.h"
#include "support/rabook_compile_test_fixture.h"
#include "unity_minimal.h"

/** @brief Fixed multi-chunk and capture capacities. */
typedef enum : uint32_t {
  k_test_chunk_bytes    = 128U,   /**< Inflated bytes in each test chunk. */
  k_test_rbkc_cap       = 16384U, /**< Generated container capacity.      */
  k_test_compressed_cap = 512U,   /**< One compressed-stream capacity.    */
  k_test_table_cap      = 64U,    /**< Container offset entries.          */
  k_test_scratch_cap    = 1024U,  /**< Strict validator scratch bytes.    */
  k_test_report_cap     = 8192U,  /**< Captured report byte capacity.     */
} test_limit_t;

/** @brief Aligned compressor state. */
typedef union {
  max_align_t align;                                  /**< Forces maximum platform alignment. */
  uint8_t     bytes[k_ra8_io_compress_scratch_bytes]; /**< tdefl state bytes.                 */
} test_compressor_t;

/** @brief Memory object used by both production and inspect callbacks. */
typedef struct {
  uint8_t* bytes;    /**< Object storage.                      */
  uint32_t len;      /**< Readable byte length.                */
  uint32_t cap;      /**< Writable byte capacity.              */
  uint32_t max_step; /**< Maximum read result; zero means all. */
} test_object_t;

/** @brief Bounded report capture with one injected failure switch. */
typedef struct {
  char     bytes[k_test_report_cap]; /**< Captured bytes plus terminator. */
  uint32_t used;                     /**< Captured payload length.        */
  bool     reject;                   /**< Reject every append when set.   */
} test_report_t;

static ra8_test_rabook_fixture_t s_fixture;
static uint8_t                   s_rbkc[k_test_rbkc_cap];
static uint8_t                   s_input[k_test_chunk_bytes];
static uint8_t                   s_compressed[k_test_compressed_cap];
static uint64_t                  s_offsets[k_test_table_cap];
static test_compressor_t         s_compressor;
static uint64_t                  s_inspect_table[k_test_table_cap];
static uint8_t                   s_inspect_compressed[k_test_compressed_cap];
static uint8_t                   s_inspect_chunk[k_test_chunk_bytes];
static uint8_t                   s_inspect_scratch[k_test_scratch_cap];

/**
 * @brief Discard one shared-library diagnostic byte in the host test.
 * @details Provides a harmless logger endpoint for expected fault vectors.
 * @param[in] ctx Unused logger context.
 * @param[in] byte Diagnostic byte to discard.
 * @pre The callback is installed only for this single-threaded test.
 * @pre No caller relies on retaining @p byte.
 * @post No output byte or fixture state changes.
 * @post The caller may immediately offer another byte.
 * @note Test-only and intentionally silent.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_log(void* ctx, uint8_t byte)
{
  (void)ctx;
  (void)byte;
}

/**
 * @brief Read an exact flat-builder range for the container producer.
 * @details Bounds and copies one requested range from the finalized fixture.
 * @param[in,out] opaque Bound ::test_object_t source.
 * @param[in] offset Flat-blob byte offset.
 * @param[out] dst Destination for @p requested bytes.
 * @param[in] requested Exact requested byte count.
 * @param[out] out_read Receives the copied byte count.
 * @return Callback status.
 * @retval k_ra8_ok The complete range was copied.
 * @retval k_ra8_err_invalid_size The range exceeded the fixture.
 * @pre Callback pointers are non-null and writable where applicable.
 * @pre @p dst spans @p requested bytes.
 * @post @p out_read is initialized on every path.
 * @post Success copies exactly @p requested bytes.
 * @note Test-only memory source.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_flat_read(void*     opaque,
                                    uint32_t  offset,
                                    uint8_t*  dst,
                                    uint32_t  requested,
                                    uint32_t* out_read)
{
  test_object_t* object = (test_object_t*)opaque;
  *out_read             = 0U;
  if ((offset > object->len) || (requested > (object->len - offset))) {
    return k_ra8_err_invalid_size;
  }
  (void)memcpy(dst, &object->bytes[offset], requested);
  *out_read = requested;
  return k_ra8_ok;
}

/**
 * @brief Write one exact generated container range into bounded memory.
 * @details Bounds and stores one positioned output range from the producer.
 * @param[in,out] opaque Bound ::test_object_t destination.
 * @param[in] offset Container byte offset.
 * @param[in] src Source bytes to copy.
 * @param[in] requested Exact requested byte count.
 * @param[out] out_written Receives the stored byte count.
 * @return Callback status.
 * @retval k_ra8_ok The complete range was stored.
 * @retval k_ra8_err_invalid_size The range exceeded destination capacity.
 * @pre Callback pointers are non-null and writable where applicable.
 * @pre @p src spans @p requested readable bytes.
 * @post @p out_written is initialized on every path.
 * @post Success stores exactly @p requested bytes.
 * @note Test-only memory destination.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_write_at(void*          opaque,
                                   uint64_t       offset,
                                   const uint8_t* src,
                                   uint32_t       requested,
                                   uint32_t*      out_written)
{
  test_object_t* object = (test_object_t*)opaque;
  *out_written          = 0U;
  if ((offset > object->cap) || ((uint64_t)requested > ((uint64_t)object->cap - offset))) {
    return k_ra8_err_invalid_size;
  }
  (void)memcpy(&object->bytes[offset], src, requested);
  *out_written = requested;
  return k_ra8_ok;
}

/**
 * @brief Serve legal short positioned reads to exercise exact adaptation.
 * @details Clips at object end and at the configured positive-read ceiling.
 * @param[in,out] opaque Bound ::test_object_t source.
 * @param[in] offset Container byte offset.
 * @param[out] dst Destination for the returned prefix.
 * @param[in] requested Maximum requested byte count.
 * @param[out] out_read Receives the copied prefix length.
 * @return Callback status.
 * @retval k_ra8_ok A legal full, short, or end read was produced.
 * @pre Callback pointers are non-null and writable where applicable.
 * @pre @p dst spans @p requested bytes when nonzero.
 * @post @p out_read never exceeds @p requested.
 * @post Source position and object bytes remain unchanged.
 * @note Test-only short-read injector.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_read_at(void* opaque, uint64_t offset, uint8_t* dst, size_t requested, size_t* out_read)
{
  test_object_t* object = (test_object_t*)opaque;
  *out_read             = 0U;
  if (offset >= object->len) {
    return k_ra8_ok;
  }
  size_t       count  = requested;
  const size_t remain = (size_t)object->len - (size_t)offset;
  if (count > remain) {
    count = remain;
  }
  if ((object->max_step != 0U) && (count > object->max_step)) {
    count = object->max_step;
  }
  (void)memcpy(dst, &object->bytes[offset], count);
  *out_read = count;
  return k_ra8_ok;
}

/**
 * @brief Confirm the memory object still has the captured extent.
 * @details Compares the live fixture length with the inspector's captured view.
 * @param[in] opaque Bound ::test_object_t source.
 * @param[in] expected_size Captured source extent.
 * @return Stability status.
 * @retval k_ra8_ok The extents match.
 * @retval k_ra8_err_validation_failed The object extent changed.
 * @pre @p opaque resolves to a live test object.
 * @pre The expected extent came from the same object.
 * @post No object byte or member changes.
 * @post The verdict depends only on the two extents.
 * @note Test-only stability seam.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_validate(void* opaque, uint64_t expected_size)
{
  const test_object_t* object = (const test_object_t*)opaque;
  return (object->len == expected_size) ? k_ra8_ok : k_ra8_err_validation_failed;
}

/**
 * @brief Capture one exact report span or inject a sink error.
 * @details Appends to bounded test text and maintains a trailing NUL.
 * @param[in,out] opaque Bound ::test_report_t capture.
 * @param[in] bytes Source report bytes.
 * @param[in] len Exact source byte count.
 * @return Capture status.
 * @retval k_ra8_ok The complete span and terminator fit.
 * @retval k_ra8_fail The configured rejection fault fired.
 * @pre @p opaque and @p bytes are non-null for nonzero @p len.
 * @pre Existing capture length is within its fixed capacity.
 * @post Success advances `used` by exactly @p len.
 * @post Success preserves a NUL immediately after captured payload.
 * @note Test-only sink and deterministic fault injector.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_report(void* opaque, const uint8_t* bytes, size_t len)
{
  test_report_t* report = (test_report_t*)opaque;
  if (report->reject) {
    return k_ra8_fail;
  }
  if (len >= ((size_t)k_test_report_cap - report->used)) {
    return k_ra8_err_no_mem;
  }
  (void)memcpy(&report->bytes[report->used], bytes, len);
  report->used += (uint32_t)len;
  report->bytes[report->used] = '\0';
  return k_ra8_ok;
}

/**
 * @brief Build a real compiler blob and wrap it with the production RBKC
 * writer.
 * @details Populates the canonical fixture, optionally changes one image byte
 * after CRC finalization, then compresses real independent chunks.
 * @param[in] corrupt_inner Whether to create a validly compressed CRC mismatch.
 * @return Generated RBKC byte length.
 * @retval nonzero Complete container length within `s_rbkc`.
 * @pre Static fixture and producer workspaces are exclusively owned.
 * @pre The production fixture capacities cover the canonical test book.
 * @post `s_rbkc` contains one complete production-generated container.
 * @post A true fault selector changes only the inner CRC relationship.
 * @note Test assertions terminate the process on unexpected producer failure.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_build(bool corrupt_inner)
{
  (void)memset(&s_fixture, 0, sizeof(s_fixture));
  (void)memset(s_rbkc, 0, sizeof(s_rbkc));
  ra8_rabook_ctx_t ctx = {};
  ra8_test_rabook_init(&s_fixture, &ctx);
  ra8_test_rabook_roundtrip_t roundtrip = {};
  ra8_test_rabook_populate(&s_fixture, &ctx, &roundtrip, false);
  const void* blob     = nullptr;
  uint32_t    flat_len = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_finalize(&ctx, &blob, &flat_len));
  TEST_ASSERT_EQ((intptr_t)s_fixture.out, (intptr_t)blob);
  if (corrupt_inner) {
    s_fixture.out[flat_len - 1U] ^= 0x01U;
  }
  test_object_t flat   = {.bytes = s_fixture.out, .len = flat_len, .cap = flat_len, .max_step = 0U};
  test_object_t packed = {.bytes = s_rbkc, .len = 0U, .cap = sizeof(s_rbkc), .max_step = 0U};
  ra8_rabook_container_workspace_t ws       = {.input          = s_input,
                                               .compressed     = s_compressed,
                                               .compressor     = s_compressor.bytes,
                                               .offsets        = s_offsets,
                                               .input_cap      = sizeof(s_input),
                                               .compressed_cap = sizeof(s_compressed),
                                               .compressor_cap = sizeof(s_compressor.bytes),
                                               .offset_cap     = k_test_table_cap};
  uint64_t                         rbkc_len = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rabook_container_write(internal_flat_read,
                                            &flat,
                                            flat_len,
                                            k_test_chunk_bytes,
                                            internal_write_at,
                                            &packed,
                                            &ws,
                                            &rbkc_len));
  TEST_ASSERT(rbkc_len <= sizeof(s_rbkc));
  return (uint32_t)rbkc_len;
}

/**
 * @brief Inspect one prepared object through the production stream API.
 * @details Binds positioned input, stability, report, and disjoint workspace
 * callbacks before delegating complete inspection.
 * @param[in,out] object Prepared container memory object.
 * @param[in] verbose Whether to request the chunk inventory.
 * @param[in] table_cap Advertised inspector table capacity.
 * @param[in,out] capture Bounded report capture.
 * @return Production inspector status.
 * @retval k_ra8_ok Strict validation and reporting succeeded.
 * @retval other Injected fault or production validation status.
 * @pre @p object and @p capture are non-null and live.
 * @pre Static inspector workspaces are exclusively owned.
 * @post Production status is returned without translation.
 * @post Object bytes and extent remain unchanged.
 * @note Test-only composition helper.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_inspect(test_object_t* object, bool verbose, uint32_t table_cap, test_report_t* capture)
{
  const ra8_fmt_source_t             source    = {.read_at  = internal_read_at,
                                                  .validate = internal_validate,
                                                  .ctx      = object,
                                                  .size     = object->len};
  ra8_fmt_rabook_inspect_workspace_t workspace = {.table          = s_inspect_table,
                                                  .table_cap      = table_cap,
                                                  .compressed     = s_inspect_compressed,
                                                  .compressed_cap = sizeof(s_inspect_compressed),
                                                  .chunk          = s_inspect_chunk,
                                                  .chunk_cap      = sizeof(s_inspect_chunk),
                                                  .scratch        = s_inspect_scratch,
                                                  .scratch_cap    = sizeof(s_inspect_scratch)};
  const ra8_fmt_sink_t               report    = {.write = internal_report, .ctx = capture};
  return ra8_fmt_rabook_inspect_stream(&source, verbose, &workspace, &report);
}

/**
 * @brief Verify real multi-chunk validation and the exact stable report.
 * @details Exercises legal seven-byte reads and compares every report byte.
 * @pre Static test workspaces are exclusively owned.
 * @pre The production compiler fixture is available.
 * @post The strict inspector accepted the real container.
 * @post The successful report exactly matched its golden text.
 * @note Terminates through the test harness on failure.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_valid(void)
{
  TEST_BEGIN("imagepack strict RBKC inspect golden");
  const uint32_t rbkc_len = internal_build(false);
  test_object_t  object   = {.bytes = s_rbkc, .len = rbkc_len, .cap = rbkc_len, .max_step = 7U};
  test_report_t  capture  = {};
  TEST_ASSERT_EQ(k_ra8_ok, internal_inspect(&object, false, k_test_table_cap, &capture));
  char           expected[512U];
  const uint32_t flat_len = ((const ra8_book_header_t*)s_fixture.out)->total_size;
  const uint32_t chunks   = (flat_len + k_test_chunk_bytes - 1U) / k_test_chunk_bytes;
  (void)snprintf(expected,
                 sizeof(expected),
                 "RBKC rabook container: %u bytes\n"
                 "  chunk_bytes    : %u\n"
                 "  inflated_total : %u\n"
                 "  chunk_count    : %u\n"
                 "  reserved       : 0\n"
                 "verdict: VALID (chunk table monotonic and complete)\n",
                 rbkc_len,
                 (uint32_t)k_test_chunk_bytes,
                 flat_len,
                 chunks);
  TEST_ASSERT_EQ(0, strcmp(expected, capture.bytes));
  TEST_END("imagepack strict RBKC inspect golden");
}

/**
 * @brief Reject outer corruption, bad inner CRC, and short tables.
 * @details Covers bad magic, payload-only corruption after finalization, and an
 * inspector table too small for otherwise valid geometry.
 * @pre Static test workspaces are exclusively owned.
 * @pre The production compiler fixture is available.
 * @post Every malformed vector returned a non-success status.
 * @post The payload-only fault reached the inner CRC verdict.
 * @note Terminates through the test harness on failure.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_faults(void)
{
  TEST_BEGIN("imagepack strict RBKC inspect faults");
  uint32_t      rbkc_len = internal_build(false);
  test_object_t object   = {.bytes = s_rbkc, .len = rbkc_len, .cap = rbkc_len, .max_step = 0U};
  test_report_t capture  = {};
  s_rbkc[0]              = 'X';
  TEST_ASSERT(internal_inspect(&object, false, k_test_table_cap, &capture) != k_ra8_ok);
  TEST_ASSERT_EQ(
    0,
    strcmp("verdict: INVALID (strict RBKC/RABOOK1 validation failed)\n", capture.bytes));
  rbkc_len = internal_build(true);
  object   = (test_object_t){.bytes = s_rbkc, .len = rbkc_len, .cap = rbkc_len, .max_step = 0U};
  capture  = (test_report_t){};
  TEST_ASSERT_EQ(k_ra8_err_range_check_failed,
                 internal_inspect(&object, false, k_test_table_cap, &capture));
  capture = (test_report_t){};
  TEST_ASSERT(internal_inspect(&object, false, 1U, &capture) != k_ra8_ok);
  TEST_END("imagepack strict RBKC inspect faults");
}

/**
 * @brief Propagate an injected report-sink failure after complete validation.
 * @details Uses a valid real container and a report sink that rejects every
 * span.
 * @pre Static test workspaces are exclusively owned.
 * @pre The production compiler fixture is available.
 * @post The exact injected sink status was returned.
 * @post The valid container bytes remained unchanged.
 * @note Terminates through the test harness on failure.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_sink_fault(void)
{
  TEST_BEGIN("imagepack strict RBKC inspect sink fault");
  const uint32_t rbkc_len = internal_build(false);
  test_object_t  object   = {.bytes = s_rbkc, .len = rbkc_len, .cap = rbkc_len, .max_step = 0U};
  test_report_t  capture  = {.reject = true};
  TEST_ASSERT_EQ(k_ra8_fail, internal_inspect(&object, true, k_test_table_cap, &capture));
  TEST_END("imagepack strict RBKC inspect sink fault");
}

/**
 * @brief Emit one production-built RBKC artifact for CLI integration.
 * @details Builds the canonical fixture and writes all bytes through a raw
 * descriptor, retrying short positive writes.
 * @param[in] path NUL-terminated destination path.
 * @return Process-style emission status.
 * @retval 0 The complete artifact was written and closed.
 * @retval 1 Open, write, or close failed.
 * @pre @p path is non-null and NUL-terminated.
 * @pre The caller provides a disposable integration destination.
 * @post Success leaves one complete RBKC artifact at @p path.
 * @post The descriptor is closed on every post-open path.
 * @note Test-only fixture producer.
 * @since 0.1.0
 */
RA8_INTERNAL
static int internal_emit(const char* path)
{
  const uint32_t len = internal_build(false);
  const int      fd  = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    return 1;
  }
  uint32_t done = 0U;
  while (done < len) {
    const ssize_t wrote = write(fd, &s_rbkc[done], len - done);
    if (wrote <= 0) {
      (void)close(fd);
      return 1;
    }
    done += (uint32_t)wrote;
  }
  return (close(fd) == 0) ? 0 : 1;
}

int main(int argc, char** argv)
{
  ra8_log_set_byte_sink(internal_log, nullptr);
  if ((argc == 3) && (strcmp(argv[1], "--emit") == 0)) {
    return internal_emit(argv[2]);
  }
  internal_test_valid();
  internal_test_faults();
  internal_test_sink_fault();
  return 0;
}
