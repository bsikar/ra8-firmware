/**
 * @file test_rabook_import_crc.c
 * @brief Fault tests for bounded RABOOK import source-key streaming.
 *
 * @details
 * Drives the importer CRC seam through exact completion, preflight rejection,
 * short and oversized reads, appended data, callback failures, and exhausted
 * read budgets. Sentinel outputs prove that no partial source identity is
 * published when the stable size and EOF contract is violated.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "rabook_import_internal.h"
#include "unity_minimal.h"

/** @brief Deterministic sequential reader with length/error fault injection. */
typedef struct {
  const uint8_t* data;          /**< Source bytes.                          */
  uint32_t       len;           /**< Source length.                         */
  uint32_t       off;           /**< Current sequential offset.             */
  uint32_t       calls;         /**< Callback invocation count.             */
  uint32_t       fail_call;     /**< One-based failing call; zero disables. */
  bool           report_excess; /**< Report more bytes than requested.      */
} crc_reader_t;

/** @brief Serve one bounded sequential read or inject the configured fault. */
RA8_INTERNAL
static ra8_err_t internal_crc_read(void* ctx, uint8_t* buf, uint32_t requested, uint32_t* out_read)
{
  crc_reader_t* rd = (crc_reader_t*)ctx;
  rd->calls++;
  if ((rd->fail_call != 0U) && (rd->calls == rd->fail_call)) {
    return k_ra8_err_hw_timeout;
  }
  if (rd->report_excess) {
    *out_read = requested + 1U;
    return k_ra8_ok;
  }
  uint32_t available = rd->len - rd->off;
  if (available > requested) {
    available = requested;
  }
  if (available != 0U) {
    (void)memcpy(buf, &rd->data[rd->off], available);
    rd->off += available;
  }
  *out_read = available;
  return k_ra8_ok;
}

/** @test Exact input is consumed, EOF-probed, and published. */
RA8_INTERNAL
static void internal_test_exact_success(void)
{
  TEST_BEGIN("rabook import CRC exact bounded success");
  static const uint8_t data[] = {'a', 'b', 'c', 'd'};
  crc_reader_t         rd     = {.data = data, .len = (uint32_t)sizeof(data)};
  uint8_t              buf[3] = {};
  uint32_t             size   = UINT32_MAX;
  uint32_t             crc    = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 rabook_import_crc_stream_test(internal_crc_read,
                                               &rd,
                                               sizeof(data),
                                               buf,
                                               (uint32_t)sizeof(buf),
                                               2U,
                                               &size,
                                               &crc));
  TEST_ASSERT_EQ(sizeof(data), size);
  TEST_ASSERT_EQ(0xED82CD11U, crc);
  TEST_ASSERT_EQ(3U, rd.calls);
  crc_reader_t empty = {};
  size               = UINT32_MAX;
  crc                = UINT32_MAX;
  TEST_ASSERT_EQ(k_ra8_ok,
                 rabook_import_crc_stream_test(internal_crc_read,
                                               &empty,
                                               0U,
                                               buf,
                                               (uint32_t)sizeof(buf),
                                               1U,
                                               &size,
                                               &crc));
  TEST_ASSERT_EQ(0U, size);
  TEST_ASSERT_EQ(0U, crc);
  TEST_ASSERT_EQ(1U, empty.calls);
  TEST_END("rabook import CRC exact bounded success");
}

/** @test Budget/width errors fail before I/O and preserve outputs. */
RA8_INTERNAL
static void internal_test_preflight_rejection(void)
{
  TEST_BEGIN("rabook import CRC preflight rejection");
  static const uint8_t data[] = {1U, 2U, 3U};
  crc_reader_t         rd     = {.data = data, .len = (uint32_t)sizeof(data)};
  uint8_t              buf[1] = {};
  uint32_t             size   = 0x11223344U;
  uint32_t             crc    = 0x55667788U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 rabook_import_crc_stream_test(nullptr,
                                               &rd,
                                               sizeof(data),
                                               buf,
                                               (uint32_t)sizeof(buf),
                                               UINT32_MAX,
                                               &size,
                                               &crc));
  TEST_ASSERT_EQ(0U, rd.calls);
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 rabook_import_crc_stream_test(internal_crc_read,
                                               &rd,
                                               sizeof(data),
                                               buf,
                                               (uint32_t)sizeof(buf),
                                               2U,
                                               &size,
                                               &crc));
  TEST_ASSERT_EQ(0U, rd.calls);
  TEST_ASSERT_EQ(0x11223344U, size);
  TEST_ASSERT_EQ(0x55667788U, crc);
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 rabook_import_crc_stream_test(internal_crc_read,
                                               &rd,
                                               sizeof(data),
                                               buf,
                                               0U,
                                               UINT32_MAX,
                                               &size,
                                               &crc));
  TEST_ASSERT_EQ(0U, rd.calls);
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 rabook_import_crc_stream_test(internal_crc_read,
                                               &rd,
                                               (uint64_t)UINT32_MAX + 1U,
                                               buf,
                                               (uint32_t)sizeof(buf),
                                               UINT32_MAX,
                                               &size,
                                               &crc));
  TEST_ASSERT_EQ(0U, rd.calls);
  TEST_END("rabook import CRC preflight rejection");
}

/**
 * @test Zero-progress and exhausted-budget reads never publish a key.
 * @par MC/DC:
 * Decision identity:
 * `apps/shared_libs/rabook_import/src/rabook_import.c@internal_crc_stream_validate_args`
 * (capacity/width OR, factored out of the entry point's argument checks),
 * `apps/shared_libs/rabook_import/src/rabook_import.c@priv_rabook_import_crc_stream`
 * (loop AND), and
 * `apps/shared_libs/rabook_import/src/rabook_import.c@internal_crc_stream_read_chunk`
 * (progress OR, factored out of the read loop body).
 * The capacity/width OR uses the exact-success all-false control plus the
 * zero-capacity and UINT32_MAX+1 vectors in the preflight test. The loop AND
 * uses non-empty entry, empty-stream T,F exit, and exhausted-budget F,T exit.
 * The progress OR uses an ordinary bounded read plus zero-progress and
 * report-excess vectors. Each operand therefore independently determines.
 * @details Output sentinels prove every rejected stream remains unpublished.
 * @pre The injected reader owns stable source bytes for each vector.
 * @pre The local buffer is writable for its advertised nonzero capacity.
 * @post Every compound operand has an independently determining vector.
 * @post Rejection preserves both output sentinels.
 * @note The exact and preflight companion tests supply named control vectors.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void internal_test_progress_faults(void)
{
  TEST_BEGIN("rabook import CRC progress faults preserve outputs");
  static const uint8_t data[] = {1U, 2U, 3U, 4U};
  uint8_t              buf[4] = {};
  uint32_t             size   = 0x11223344U;
  uint32_t             crc    = 0x55667788U;
  crc_reader_t         rd     = {.data = data, .len = 0U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 rabook_import_crc_stream_test(internal_crc_read,
                                               &rd,
                                               1U,
                                               buf,
                                               (uint32_t)sizeof(buf),
                                               1U,
                                               &size,
                                               &crc));
  TEST_ASSERT_EQ(0x11223344U, size);
  TEST_ASSERT_EQ(0x55667788U, crc);
  rd = (crc_reader_t){.data = data, .len = 2U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 rabook_import_crc_stream_test(internal_crc_read,
                                               &rd,
                                               3U,
                                               buf,
                                               (uint32_t)sizeof(buf),
                                               1U,
                                               &size,
                                               &crc));
  TEST_ASSERT_EQ(0x11223344U, size);
  TEST_ASSERT_EQ(0x55667788U, crc);
  TEST_END("rabook import CRC progress faults preserve outputs");
}

/**
 * @test Oversized, appended, and failed reads never publish a source key.
 * @details Drives the remaining reader-contract faults after progress/budget
 * coverage is established by internal_test_progress_faults().
 * @pre The injected reader owns stable source bytes for every vector.
 * @pre The local output sentinels begin unpublished.
 * @post Every vector returns its exact failure status.
 * @post Both output sentinels remain unchanged.
 * @note Test-only fault-injection helper.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_test_read_faults(void)
{
  TEST_BEGIN("rabook import CRC read faults preserve outputs");
  static const uint8_t data[] = {1U, 2U, 3U, 4U};
  uint8_t              buf[4] = {};
  uint32_t             size   = 0x11223344U;
  uint32_t             crc    = 0x55667788U;
  crc_reader_t         rd = {.data = data, .len = (uint32_t)sizeof(data), .report_excess = true};
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 rabook_import_crc_stream_test(internal_crc_read,
                                               &rd,
                                               3U,
                                               buf,
                                               (uint32_t)sizeof(buf),
                                               1U,
                                               &size,
                                               &crc));
  rd = (crc_reader_t){.data = data, .len = (uint32_t)sizeof(data)};
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 rabook_import_crc_stream_test(internal_crc_read,
                                               &rd,
                                               3U,
                                               buf,
                                               (uint32_t)sizeof(buf),
                                               1U,
                                               &size,
                                               &crc));
  rd = (crc_reader_t){.data = data, .len = (uint32_t)sizeof(data), .fail_call = 1U};
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 rabook_import_crc_stream_test(internal_crc_read,
                                               &rd,
                                               3U,
                                               buf,
                                               (uint32_t)sizeof(buf),
                                               1U,
                                               &size,
                                               &crc));
  TEST_ASSERT_EQ(0x11223344U, size);
  TEST_ASSERT_EQ(0x55667788U, crc);
  TEST_END("rabook import CRC read faults preserve outputs");
}

int main(void)
{
  internal_test_exact_success();
  internal_test_preflight_rejection();
  internal_test_progress_faults();
  internal_test_read_faults();
  return 0;
}
