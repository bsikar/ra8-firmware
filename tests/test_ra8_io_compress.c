/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_io_compress.c
 * @brief Unit tests for the ra8_io DEFLATE compress/decompress (issue #161).
 *
 * @details
 * Exercises the heap-free raw-DEFLATE wrappers: a compress -> decompress round
 * trip is byte-identical, highly-repetitive input compresses smaller than it
 * started, and every guard path (NULL args, undersized scratch, undersized
 * output on each direction) returns the documented error.
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_io_compress.h"
#include "unity_minimal.h"

/**
 * @enum t_compress_const_t
 * @brief Fixture sizes.
 */
typedef enum : uint32_t {
  k_t_payload_bytes = 4096, /**< Source payload size.                                  */
  k_t_out_bytes     = 8192, /**< Compressed/scratch output cap.                        */
  k_t_seed_mul      = 31,   /**< Pattern generator multiplier.                         */
  k_t_pattern_mod   = 7,    /**< Pattern alphabet size (so it is highly compressible). */
} t_compress_const_t;

static uint8_t s_payload[(size_t)k_t_payload_bytes];
static uint8_t s_packed[(size_t)k_t_out_bytes];
static uint8_t s_restored[(size_t)k_t_payload_bytes];
static uint8_t s_scratch[(size_t)k_ra8_io_compress_scratch_bytes];

/** @brief Fill the payload with a small-alphabet repetitive pattern. */
static void fill_payload(void)
{
  for (uint32_t i = 0; i < (uint32_t)k_t_payload_bytes; ++i) {
    s_payload[i] = (uint8_t)((i * (uint32_t)k_t_seed_mul) % (uint32_t)k_t_pattern_mod);
  }
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- a compress then decompress reproduces the
 * input exactly and the compressed form is strictly smaller than the source)
 */
static void test_round_trip(void)
{
  TEST_BEGIN("compress round-trip");
  fill_payload();

  uint32_t packed_len = 0;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_io_compress(s_payload,
                                 k_t_payload_bytes,
                                 s_packed,
                                 k_t_out_bytes,
                                 s_scratch,
                                 k_ra8_io_compress_scratch_bytes,
                                 &packed_len));
  TEST_ASSERT(packed_len > 0);
  TEST_ASSERT(packed_len < (uint32_t)k_t_payload_bytes); /* repetitive -> shrinks */

  uint32_t restored_len = 0;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_io_decompress(s_packed, packed_len, s_restored, k_t_payload_bytes, &restored_len));
  TEST_ASSERT_EQ(k_t_payload_bytes, restored_len);
  TEST_ASSERT_EQ(0, memcmp(s_payload, s_restored, (size_t)k_t_payload_bytes));
  TEST_END("compress round-trip");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- each guard is an independent
 * single-condition check: NULL src/out/scratch/out_len and undersized scratch)
 */
static void test_compress_validation(void)
{
  TEST_BEGIN("compress validation");
  uint32_t out_len = 0;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_io_compress(nullptr,
                                 k_t_payload_bytes,
                                 s_packed,
                                 k_t_out_bytes,
                                 s_scratch,
                                 k_ra8_io_compress_scratch_bytes,
                                 &out_len));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_io_compress(s_payload,
                                 k_t_payload_bytes,
                                 nullptr,
                                 k_t_out_bytes,
                                 s_scratch,
                                 k_ra8_io_compress_scratch_bytes,
                                 &out_len));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_io_compress(s_payload,
                                 k_t_payload_bytes,
                                 s_packed,
                                 k_t_out_bytes,
                                 nullptr,
                                 k_ra8_io_compress_scratch_bytes,
                                 &out_len));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_io_compress(s_payload,
                                 k_t_payload_bytes,
                                 s_packed,
                                 k_t_out_bytes,
                                 s_scratch,
                                 k_ra8_io_compress_scratch_bytes,
                                 nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_io_compress(s_payload,
                                 k_t_payload_bytes,
                                 s_packed,
                                 k_t_out_bytes,
                                 s_scratch,
                                 k_ra8_io_compress_scratch_bytes - 1,
                                 &out_len));
  TEST_END("compress validation");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- an output buffer too small to hold the
 * compressed stream trips the overflow path and returns no_mem)
 */
static void test_compress_overflow(void)
{
  TEST_BEGIN("compress overflow");
  fill_payload();
  uint32_t out_len = 0;
  uint8_t  tiny[4] = {};
  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 ra8_io_compress(s_payload,
                                 k_t_payload_bytes,
                                 tiny,
                                 (uint32_t)sizeof(tiny),
                                 s_scratch,
                                 k_ra8_io_compress_scratch_bytes,
                                 &out_len));
  TEST_END("compress overflow");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- decompress rejects NULL args and an
 * output buffer too small to hold the inflated result, each independently)
 */
static void test_decompress_validation(void)
{
  TEST_BEGIN("decompress validation");
  fill_payload();
  uint32_t packed_len = 0;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_io_compress(s_payload,
                                 k_t_payload_bytes,
                                 s_packed,
                                 k_t_out_bytes,
                                 s_scratch,
                                 k_ra8_io_compress_scratch_bytes,
                                 &packed_len));

  uint32_t out_len = 0;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_io_decompress(nullptr, packed_len, s_restored, k_t_payload_bytes, &out_len));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_io_decompress(s_packed, packed_len, nullptr, k_t_payload_bytes, &out_len));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_io_decompress(s_packed, packed_len, s_restored, k_t_payload_bytes, nullptr));

  uint8_t small[8] = {};
  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 ra8_io_decompress(s_packed, packed_len, small, (uint32_t)sizeof(small), &out_len));
  TEST_END("decompress validation");
}

int32_t main(void)
{
  test_round_trip();
  test_compress_validation();
  test_compress_overflow();
  test_decompress_validation();
  (void)fprintf(stderr, "[OK  ] test_ra8_io_compress.c\n");
  return 0;
}
