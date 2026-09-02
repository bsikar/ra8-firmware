/**
 * @file test_ra8_compress.c
 * @brief Unit tests for app-domain DEFLATE compress/decompress (issue #161).
 *
 * @details
 * Exercises the heap-free raw-DEFLATE and zlib wrappers: both round trips are
 * byte-identical, highly-repetitive input compresses smaller than it started,
 * and every guard path (NULL args, undersized scratch, undersized output on
 * each direction) returns the documented error.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_compress.h"
#include "ra8_err.h"
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

/** @brief RFC 1950 CMF byte for DEFLATE with a 32 KiB window. */
typedef enum : uint8_t {
  k_t_zlib_cmf = 0x78U, /**< Deflate plus 32 KiB LZ77 window declaration. */
} t_zlib_const_t;

static uint8_t s_payload[(size_t)k_t_payload_bytes];
static uint8_t s_packed[(size_t)k_t_out_bytes];
static uint8_t s_restored[(size_t)k_t_payload_bytes];
static uint8_t s_scratch[(size_t)k_ra8_compress_scratch_bytes];

/**
 * @brief Fill the payload with a small-alphabet repetitive pattern.
 * @details Uses deterministic integer arithmetic so compression assertions are repeatable.
 * @pre ::s_payload spans ::k_t_payload_bytes writable bytes.
 * @pre Pattern constants are nonzero and fit uint32_t arithmetic.
 * @post Every payload byte is initialized.
 * @post Repeated calls produce byte-identical payloads.
 * @note Thread-safe only when the static fixture is exclusively owned.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_fill_payload(void)
{
  for (uint32_t i = 0; i < (uint32_t)k_t_payload_bytes; ++i) {
    s_payload[i] = (uint8_t)((i * (uint32_t)k_t_seed_mul) % (uint32_t)k_t_pattern_mod);
  }
}

/**
 * @test internal_test_round_trip
 * @brief Verify raw-DEFLATE compression and decompression are byte-identical.
 * @details Compresses the deterministic payload, proves it shrinks, inflates it,
 *          and compares the complete restored range.
 * @pre Static payload, packed, restored, and scratch buffers are exclusively owned.
 * @pre The vendored miniz implementation is linked.
 * @post The restored payload equals the source byte for byte.
 * @post The emitted raw stream is nonempty and smaller than this fixture.
 * @note No compound decisions are under test.
 * @since 0.1.0
 * @par MC/DC:
 * (no compound decisions under test -- a compress then decompress reproduces the
 * input exactly and the compressed form is strictly smaller than the source)
 */
RA8_INTERNAL static void internal_test_round_trip(void)
{
  TEST_BEGIN("compress round-trip");
  internal_fill_payload();

  uint32_t packed_len = 0;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_compress(s_payload,
                              k_t_payload_bytes,
                              s_packed,
                              k_t_out_bytes,
                              s_scratch,
                              k_ra8_compress_scratch_bytes,
                              &packed_len));
  TEST_ASSERT(packed_len > 0U);
  TEST_ASSERT(packed_len < (uint32_t)k_t_payload_bytes); /* repetitive -> shrinks */

  uint32_t restored_len = 0;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_decompress(s_packed, packed_len, s_restored, k_t_payload_bytes, &restored_len));
  TEST_ASSERT_EQ(k_t_payload_bytes, restored_len);
  TEST_ASSERT_EQ(0, memcmp(s_payload, s_restored, (size_t)k_t_payload_bytes));
  TEST_END("compress round-trip");
}

/**
 * @test Emit an RFC 1950 stream and inflate it with miniz's zlib parser.
 * @brief Verify the explicit zlib wrapper emits an independently decodable stream.
 * @details Proves the public zlib arm is not the raw-DEFLATE arm under a second
 *          name: the CMF byte is present, the zlib-header parser accepts it,
 *          and every restored byte matches the source.
 * @pre The fixed payload, output, restore, and compressor arenas are writable.
 * @pre The vendored inflater accepts the RFC 1950 parse flag.
 * @post The zlib stream round-trips exactly without allocating memory.
 * @post The first stream byte equals the expected CMF declaration.
 * @note Uses the vendored low-level inflater directly to select RFC 1950 mode.
 * @since 0.1.0
 * @par MC/DC:
 * (no compound decisions in this test -- `ra8_compress_zlib` uses the same
 * single pointer, scratch-size, compressor-status, overflow, and completion
 * guards as the raw wrapper. This all-valid vector takes their success arms;
 * the assertion checks one nonzero length rather than combining conditions.)
 */
RA8_INTERNAL static void internal_test_zlib_round_trip(void)
{
  TEST_BEGIN("zlib compress round-trip");
  internal_fill_payload();

  uint32_t packed_len = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_compress_zlib(s_payload,
                                   k_t_payload_bytes,
                                   s_packed,
                                   k_t_out_bytes,
                                   s_scratch,
                                   k_ra8_compress_scratch_bytes,
                                   &packed_len));
  TEST_ASSERT(packed_len > 0U);
  TEST_ASSERT_EQ(k_t_zlib_cmf, s_packed[0]);
  const size_t restored = tinfl_decompress_mem_to_mem(s_restored,
                                                      (size_t)k_t_payload_bytes,
                                                      s_packed,
                                                      (size_t)packed_len,
                                                      TINFL_FLAG_PARSE_ZLIB_HEADER);
  TEST_ASSERT_EQ(k_t_payload_bytes, restored);
  TEST_ASSERT_EQ(0, memcmp(s_payload, s_restored, (size_t)k_t_payload_bytes));
  TEST_END("zlib compress round-trip");
}

/**
 * @test internal_test_compress_validation
 * @brief Verify every required compression argument and scratch bound.
 * @details Varies each NULL pointer independently, then supplies undersized scratch.
 * @pre Static fixture buffers exist and @p out_len is writable in non-NULL vectors.
 * @pre The production validator runs before compressor mutation.
 * @post Each invalid vector returns its documented canonical error.
 * @post No invalid vector is reported as successful output.
 * @note Each tested guard is a single-condition decision.
 * @since 0.1.0
 * @par MC/DC:
 * (no compound decisions under test -- each guard is an independent
 * single-condition check: NULL src/out/scratch/out_len and undersized scratch)
 */
RA8_INTERNAL static void internal_test_compress_validation(void)
{
  TEST_BEGIN("compress validation");
  uint32_t out_len = 0;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_compress(nullptr,
                              k_t_payload_bytes,
                              s_packed,
                              k_t_out_bytes,
                              s_scratch,
                              k_ra8_compress_scratch_bytes,
                              &out_len));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_compress(s_payload,
                              k_t_payload_bytes,
                              nullptr,
                              k_t_out_bytes,
                              s_scratch,
                              k_ra8_compress_scratch_bytes,
                              &out_len));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_compress(s_payload,
                              k_t_payload_bytes,
                              s_packed,
                              k_t_out_bytes,
                              nullptr,
                              k_ra8_compress_scratch_bytes,
                              &out_len));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_compress(s_payload,
                              k_t_payload_bytes,
                              s_packed,
                              k_t_out_bytes,
                              s_scratch,
                              k_ra8_compress_scratch_bytes,
                              nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_compress(s_payload,
                              k_t_payload_bytes,
                              s_packed,
                              k_t_out_bytes,
                              s_scratch,
                              k_ra8_compress_scratch_bytes - 1,
                              &out_len));
  TEST_END("compress validation");
}

/**
 * @test internal_test_compress_overflow
 * @brief Verify a bounded output overflow returns no-memory.
 * @details Compresses a nonempty source into an intentionally four-byte destination.
 * @pre The payload and compressor scratch are valid.
 * @pre The tiny output is smaller than the emitted stream.
 * @post The operation returns ::k_ra8_err_no_mem.
 * @post No success length is accepted from the truncated stream.
 * @note The overflow callback leg is the single decision under test.
 * @since 0.1.0
 * @par MC/DC:
 * (no compound decisions under test -- an output buffer too small to hold the
 * compressed stream trips the overflow path and returns no_mem)
 */
RA8_INTERNAL static void internal_test_compress_overflow(void)
{
  TEST_BEGIN("compress overflow");
  internal_fill_payload();
  uint32_t out_len = 0;
  uint8_t  tiny[4] = {[0] = 0U};
  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 ra8_compress(s_payload,
                              k_t_payload_bytes,
                              tiny,
                              (uint32_t)sizeof(tiny),
                              s_scratch,
                              k_ra8_compress_scratch_bytes,
                              &out_len));
  TEST_END("compress overflow");
}

/**
 * @test internal_test_decompress_validation
 * @brief Verify decompression pointer guards and output-capacity failure.
 * @details Creates one valid raw stream, then varies each required pointer and
 *          supplies an output buffer too small for the declared payload.
 * @pre Compression of the shared deterministic fixture succeeds.
 * @pre The packed stream remains unchanged across validation vectors.
 * @post NULL vectors return ::k_ra8_err_null_ptr.
 * @post The undersized output returns ::k_ra8_err_no_mem.
 * @note Each guard is exercised independently.
 * @since 0.1.0
 * @par MC/DC:
 * (no compound decisions under test -- decompress rejects NULL args and an
 * output buffer too small to hold the inflated result, each independently)
 */
RA8_INTERNAL static void internal_test_decompress_validation(void)
{
  TEST_BEGIN("decompress validation");
  internal_fill_payload();
  uint32_t packed_len = 0;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_compress(s_payload,
                              k_t_payload_bytes,
                              s_packed,
                              k_t_out_bytes,
                              s_scratch,
                              k_ra8_compress_scratch_bytes,
                              &packed_len));

  uint32_t out_len = 0;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_decompress(nullptr, packed_len, s_restored, k_t_payload_bytes, &out_len));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_decompress(s_packed, packed_len, nullptr, k_t_payload_bytes, &out_len));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_decompress(s_packed, packed_len, s_restored, k_t_payload_bytes, nullptr));

  uint8_t small[8] = {[0] = 0U};
  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 ra8_decompress(s_packed, packed_len, small, (uint32_t)sizeof(small), &out_len));
  TEST_END("decompress validation");
}

int main(void)
{
  internal_test_round_trip();
  internal_test_zlib_round_trip();
  internal_test_compress_validation();
  internal_test_compress_overflow();
  internal_test_decompress_validation();
  return 0;
}
