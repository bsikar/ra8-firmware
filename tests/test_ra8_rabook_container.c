/**
 * @file test_ra8_rabook_container.c
 * @brief Unit tests for the bounded streaming RBKC producer.
 *
 * @details Builds a multi-chunk container through memory callbacks, validates
 * every header/table field, independently inflates each RFC 1950 stream, and
 * covers workspace, short-I/O, backend-error, and compressor-exhaustion paths.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "miniz.h"
#include "ra8_attributes.h"
#include "ra8_book.h"
#include "ra8_err.h"
#include "ra8_io_compress.h"
#include "ra8_rabook_container.h"
#include "unity_minimal.h"

/** @brief Fixed fixture geometry and sentinel fault selectors. */
typedef enum : uint32_t {
  k_t_flat_bytes       = 5000U,                /**< Flat fixture length.           */
  k_t_chunk_bytes      = 1024U,                /**< Inflated bytes per chunk.      */
  k_t_chunk_count      = 5U,                   /**< Expected ceiling division.     */
  k_t_offset_count     = k_t_chunk_count + 1U, /**< Table entries including end.   */
  k_t_packed_bytes     = 16384U,               /**< RBKC destination capacity.     */
  k_t_compressed_bytes = 2048U,                /**< One zlib-stream capacity.      */
  k_t_no_fault         = UINT32_MAX,           /**< Disabled callback-fault index. */
  k_t_pattern_mul      = 37U,                  /**< Source-pattern multiplier.     */
  k_t_pattern_add      = 11U,                  /**< Source-pattern additive term.  */
} t_rbkc_limit_t;

/** @brief Byte shifts for the test-only little-endian decoder. */
typedef enum : uint8_t {
  k_t_bits_per_byte = 8U, /**< Binary bits represented by one fixture byte. */
} t_rbkc_bits_t;

/** @brief Memory-backed source/destination plus deterministic fault injection. */
typedef struct {
  const uint8_t* src;              /**< Flat source bytes.                     */
  uint8_t*       dst;              /**< RBKC destination bytes.                */
  uint32_t       src_len;          /**< Readable source length.                */
  uint32_t       dst_cap;          /**< Writable destination capacity.         */
  uint32_t       read_calls;       /**< Source callback invocation count.      */
  uint32_t       write_calls;      /**< Destination callback invocation count. */
  uint32_t       short_read_call;  /**< Call that reports one byte short.      */
  uint32_t       short_write_call; /**< Call that reports one byte short.      */
  uint32_t       fail_read_call;   /**< Call that returns a backend error.     */
  uint32_t       fail_write_call;  /**< Call that returns a backend error.     */
} t_io_t;

/** @brief Compressor scratch with at least max_align_t alignment. */
typedef union {
  /** @brief Forces portable scratch alignment. */
  max_align_t align;
  /** @brief tdefl storage. */
  uint8_t bytes[k_ra8_io_compress_scratch_bytes];
} t_compressor_t;

static uint8_t        s_flat[k_t_flat_bytes];
static uint8_t        s_packed[k_t_packed_bytes];
static uint8_t        s_input[k_t_chunk_bytes];
static uint8_t        s_compressed[k_t_compressed_bytes];
static uint8_t        s_restored[k_t_flat_bytes];
static uint64_t       s_offsets[k_t_offset_count];
static t_compressor_t s_compressor;

/**
 * @brief Decode a little-endian uint32 from the fixture output.
 * @details Reconstructs the value byte by byte without host-endian assumptions.
 * @param[in] p Source spanning four readable bytes.
 * @return Decoded unsigned value.
 * @retval uint32_t Exact little-endian value at @p p.
 * @pre @p p is non-NULL and spans four readable bytes.
 * @pre The range contains a complete encoded field.
 * @post Source bytes remain unchanged.
 * @post The result is independent of host byte order.
 * @note Thread-safe; pure helper.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_rd_u32(const uint8_t* p)
{
  uint32_t value = 0U;
  for (uint8_t i = 0U; i < 4U; i++) {
    value |= (uint32_t)p[i] << ((uint32_t)i * (uint32_t)k_t_bits_per_byte);
  }
  return value;
}

/**
 * @brief Decode a little-endian uint64 from the fixture output.
 * @details Reconstructs the value byte by byte without host-endian assumptions.
 * @param[in] p Source spanning eight readable bytes.
 * @return Decoded unsigned value.
 * @retval uint64_t Exact little-endian value at @p p.
 * @pre @p p is non-NULL and spans eight readable bytes.
 * @pre The range contains a complete encoded field.
 * @post Source bytes remain unchanged.
 * @post The result is independent of host byte order.
 * @note Thread-safe; pure helper.
 * @since 0.1.0
 */
RA8_INTERNAL static uint64_t internal_rd_u64(const uint8_t* p)
{
  uint64_t value = 0U;
  for (uint8_t i = 0U; i < 8U; i++) {
    value |= (uint64_t)p[i] << ((uint32_t)i * (uint32_t)k_t_bits_per_byte);
  }
  return value;
}

/**
 * @brief Fill the flat source with a deterministic nontrivial pattern.
 * @details Uses a fixed affine byte sequence so every run has identical input.
 * @pre ::s_flat spans ::k_t_flat_bytes writable bytes.
 * @pre Pattern constants fit uint32_t arithmetic.
 * @post Every flat-source byte is initialized.
 * @post Repeated calls produce byte-identical source bytes.
 * @note Thread-safe only when the static fixture is exclusively owned.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_fill_flat(void)
{
  for (uint32_t i = 0U; i < (uint32_t)sizeof(s_flat); i++) {
    s_flat[i] = (uint8_t)((i * (uint32_t)k_t_pattern_mul) + (uint32_t)k_t_pattern_add);
  }
}

/**
 * @brief Return a fully-capable workspace over the static fixture buffers.
 * @details Binds each production workspace member to its matching fixed arena.
 * @return Populated workspace descriptor by value.
 * @retval ra8_rabook_container_workspace_t Descriptor with full test capacities.
 * @pre All static buffers have process lifetime.
 * @pre ::s_compressor satisfies max_align_t alignment.
 * @post No buffer bytes or counters are modified.
 * @post Every returned pointer is non-NULL and stable.
 * @note Thread-safe as a descriptor constructor; later shared use is not.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_rabook_container_workspace_t internal_full_workspace(void)
{
  return (ra8_rabook_container_workspace_t){.input          = s_input,
                                            .compressed     = s_compressed,
                                            .compressor     = s_compressor.bytes,
                                            .offsets        = s_offsets,
                                            .input_cap      = sizeof(s_input),
                                            .compressed_cap = sizeof(s_compressed),
                                            .compressor_cap = sizeof(s_compressor.bytes),
                                            .offset_cap     = k_t_offset_count};
}

/**
 * @brief Return a no-fault memory I/O context.
 * @details Binds the static source/destination and disables all injected faults.
 * @return Populated callback context by value.
 * @retval t_io_t Context ready for exact reads and random writes.
 * @pre Static source and destination buffers have process lifetime.
 * @pre ::k_t_no_fault cannot equal an initial callback count.
 * @post Callback counters begin at zero.
 * @post Every fault selector equals ::k_t_no_fault.
 * @note Thread-safe as a descriptor constructor; callbacks mutate the result.
 * @since 0.1.0
 */
RA8_INTERNAL static t_io_t internal_full_io(void)
{
  return (t_io_t){.src              = s_flat,
                  .dst              = s_packed,
                  .src_len          = sizeof(s_flat),
                  .dst_cap          = sizeof(s_packed),
                  .short_read_call  = k_t_no_fault,
                  .short_write_call = k_t_no_fault,
                  .fail_read_call   = k_t_no_fault,
                  .fail_write_call  = k_t_no_fault};
}

/**
 * @brief Memory read callback with bounds and selected call faults.
 * @details Counts calls, optionally returns a backend error or one-byte-short
 *          success, and otherwise copies the exact in-range source slice.
 * @param[in,out] ctx Bound ::t_io_t context.
 * @param[in] offset Absolute flat-source offset.
 * @param[out] dst Destination spanning @p requested bytes.
 * @param[in] requested Exact requested byte count.
 * @param[out] out_read Actual copied byte count.
 * @return Scripted memory-backend status.
 * @retval k_ra8_ok Copy completed, possibly short when selected.
 * @retval k_ra8_err_hw_error Selected backend-failure vector.
 * @pre All pointers are non-NULL and @p ctx addresses ::t_io_t.
 * @pre The destination capacity matches @p requested.
 * @post The call counter advances exactly once.
 * @post On ordinary success @p out_read equals @p requested.
 * @note Not thread-safe; mutates the bound counter.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_mem_read(void* ctx, uint32_t offset, uint8_t* dst, uint32_t requested, uint32_t* out_read)
{
  t_io_t*        io   = (t_io_t*)ctx;
  const uint32_t call = io->read_calls++;
  if (call == io->fail_read_call) {
    return k_ra8_err_hw_error;
  }
  if ((offset > io->src_len) || (requested > (io->src_len - offset))) {
    return k_ra8_err_invalid_size;
  }
  uint32_t copied = requested;
  if ((call == io->short_read_call) && (copied != 0U)) {
    copied--;
  }
  (void)memcpy(dst, &io->src[offset], copied);
  *out_read = copied;
  return k_ra8_ok;
}

/**
 * @brief Memory random-write callback with bounds and selected call faults.
 * @details Counts calls, optionally returns a backend error or one-byte-short
 *          success, and otherwise copies into the exact destination range.
 * @param[in,out] ctx Bound ::t_io_t context.
 * @param[in] offset Absolute RBKC destination offset.
 * @param[in] src Source spanning @p requested bytes.
 * @param[in] requested Exact requested byte count.
 * @param[out] out_written Actual copied byte count.
 * @return Scripted memory-backend status.
 * @retval k_ra8_ok Copy completed, possibly short when selected.
 * @retval k_ra8_fail Selected backend-failure vector.
 * @pre All pointers are non-NULL and @p ctx addresses ::t_io_t.
 * @pre The source range holds @p requested readable bytes.
 * @post The call counter advances exactly once.
 * @post On ordinary success @p out_written equals @p requested.
 * @note Not thread-safe; mutates the bound destination and counter.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mem_write(void*          ctx,
                                                 uint64_t       offset,
                                                 const uint8_t* src,
                                                 uint32_t       requested,
                                                 uint32_t*      out_written)
{
  t_io_t*        io   = (t_io_t*)ctx;
  const uint32_t call = io->write_calls++;
  if (call == io->fail_write_call) {
    return k_ra8_fail;
  }
  if ((offset > io->dst_cap) || ((uint64_t)requested > ((uint64_t)io->dst_cap - offset))) {
    return k_ra8_err_invalid_size;
  }
  uint32_t copied = requested;
  if ((call == io->short_write_call) && (copied != 0U)) {
    copied--;
  }
  (void)memcpy(&io->dst[(size_t)offset], src, copied);
  *out_written = copied;
  return k_ra8_ok;
}

/**
 * @test internal_test_multi_chunk_round_trip
 * @brief Verify a multi-chunk RBKC output restores every source byte independently.
 * @details Checks header geometry, strictly increasing table ranges, per-stream
 *          RFC 1950 inflation, the reconstructed flat blob, and final length.
 * @pre All static fixture buffers are exclusively owned and writable.
 * @pre The low-level zlib inflater is linked.
 * @post Every independently inflated chunk matches its source range.
 * @post Header, table, terminal offset, and reported output length agree.
 * @note Exercises production code only through public callbacks and API.
 * @since 0.1.0
 * @par MC/DC:
 * No compound production decision is independently varied by this nominal
 * vector: validation OR chains are all false, the payload-overflow OR is
 * `(F,F)->F`, and callback bounds ORs are `(F,F)->F`. The multi-chunk geometry
 * does exercise the single `remaining < chunk_bytes` choice both ways: false
 * for four full chunks and true for the final 904-byte chunk. These compound
 * observations are controls only and are not claimed as independence sets.
 */
RA8_INTERNAL static void internal_test_multi_chunk_round_trip(void)
{
  TEST_BEGIN("rabook container multi-chunk round-trip");
  internal_fill_flat();
  memset(s_packed, 0, sizeof(s_packed));
  memset(s_restored, 0, sizeof(s_restored));
  t_io_t                           io      = internal_full_io();
  ra8_rabook_container_workspace_t ws      = internal_full_workspace();
  uint64_t                         out_len = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rabook_container_write(internal_mem_read,
                                            &io,
                                            sizeof(s_flat),
                                            k_t_chunk_bytes,
                                            internal_mem_write,
                                            &io,
                                            &ws,
                                            &out_len));
  TEST_ASSERT_EQ(0, memcmp(s_packed, "RBKC", k_ra8_book_container_magic_len));
  TEST_ASSERT_EQ(k_t_chunk_bytes, internal_rd_u32(&s_packed[4]));
  TEST_ASSERT_EQ(k_t_flat_bytes, internal_rd_u64(&s_packed[8]));
  TEST_ASSERT_EQ(k_t_chunk_count, internal_rd_u32(&s_packed[16]));
  TEST_ASSERT_EQ(0, internal_rd_u32(&s_packed[20]));

  const uint64_t payload_off =
    (uint64_t)k_ra8_book_container_header_len +
    ((uint64_t)k_t_offset_count * (uint64_t)k_ra8_book_container_entry_len);
  TEST_ASSERT_EQ(0, internal_rd_u64(&s_packed[k_ra8_book_container_header_len]));
  for (uint32_t i = 0U; i < k_t_chunk_count; i++) {
    const uint64_t begin = internal_rd_u64(
      &s_packed[k_ra8_book_container_header_len + (i * k_ra8_book_container_entry_len)]);
    const uint64_t end = internal_rd_u64(
      &s_packed[k_ra8_book_container_header_len + ((i + 1U) * k_ra8_book_container_entry_len)]);
    TEST_ASSERT(end > begin);
    const uint32_t flat_off  = i * k_t_chunk_bytes;
    const uint32_t remaining = k_t_flat_bytes - flat_off;
    const uint32_t expected  = (remaining < k_t_chunk_bytes) ? remaining : k_t_chunk_bytes;
    const size_t   got       = tinfl_decompress_mem_to_mem(&s_restored[flat_off],
                                                           expected,
                                                           &s_packed[payload_off + begin],
                                                           (size_t)(end - begin),
                                                           TINFL_FLAG_PARSE_ZLIB_HEADER);
    TEST_ASSERT_EQ((int64_t)expected, (int64_t)got);
  }
  TEST_ASSERT_EQ(0, memcmp(s_flat, s_restored, sizeof(s_flat)));
  TEST_ASSERT_EQ(
    (int64_t)out_len,
    (int64_t)(payload_off +
              internal_rd_u64(&s_packed[k_ra8_book_container_header_len +
                                        (k_t_chunk_count * k_ra8_book_container_entry_len)])));
  TEST_END("rabook container multi-chunk round-trip");
}

/**
 * @test internal_test_validation
 * @brief Verify argument and workspace bounds fail before a success length.
 * @details Varies a NULL callback, zero flat length, undersized input, offset
 *          table, and compressor scratch while retaining valid peer inputs.
 * @pre The full workspace and memory context constructors return valid objects.
 * @pre Each vector resets the field it does not intend to vary.
 * @post Every invalid vector returns its documented canonical error.
 * @post The public output length is zeroed on entry before failure.
 * @note Validation is required to precede destination mutation.
 * @since 0.1.0
 * @par MC/DC:
 * Required-pointer OR: null read gives `(T,-,-,-,-)->T`; valid calls provide
 * `(F,F,F,F,F)->F`, so the other pointer conditions are not isolated here.
 * The workspace-member OR is observed only as its all-present false control.
 * Size OR `(flat_len == 0) || (chunk_bytes == 0)` gets `(T,-)->T` here and
 * `(F,F)->F` from valid vectors; zero `chunk_bytes` is not varied.
 * Capacity OR `(chunk_bytes > input_cap) || (compressor_cap < minimum)` has
 * the complete N+1 set: `(F,F)->F`, `(T,-)->T` for short input, and
 * `(F,T)->T` for short compressor scratch.
 * Count/table OR `(count == UINT32_MAX) || (offset_cap < count + 1)` gets
 * `(F,T)->T` for the short table and `(F,F)->F` when full; its first condition
 * is not independently exercised.
 */
RA8_INTERNAL static void internal_test_validation(void)
{
  TEST_BEGIN("rabook container validation");
  t_io_t                           io      = internal_full_io();
  ra8_rabook_container_workspace_t ws      = internal_full_workspace();
  uint64_t                         out_len = 99U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rabook_container_write(nullptr,
                                            &io,
                                            k_t_flat_bytes,
                                            k_t_chunk_bytes,
                                            internal_mem_write,
                                            &io,
                                            &ws,
                                            &out_len));
  TEST_ASSERT_EQ(0, out_len);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rabook_container_write(internal_mem_read,
                                            &io,
                                            0U,
                                            k_t_chunk_bytes,
                                            internal_mem_write,
                                            &io,
                                            &ws,
                                            &out_len));
  ws.input_cap = k_t_chunk_bytes - 1U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_rabook_container_write(internal_mem_read,
                                            &io,
                                            k_t_flat_bytes,
                                            k_t_chunk_bytes,
                                            internal_mem_write,
                                            &io,
                                            &ws,
                                            &out_len));
  ws            = internal_full_workspace();
  ws.offset_cap = k_t_offset_count - 1U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_rabook_container_write(internal_mem_read,
                                            &io,
                                            k_t_flat_bytes,
                                            k_t_chunk_bytes,
                                            internal_mem_write,
                                            &io,
                                            &ws,
                                            &out_len));
  ws                = internal_full_workspace();
  ws.compressor_cap = k_ra8_io_compress_scratch_bytes - 1U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_rabook_container_write(internal_mem_read,
                                            &io,
                                            k_t_flat_bytes,
                                            k_t_chunk_bytes,
                                            internal_mem_write,
                                            &io,
                                            &ws,
                                            &out_len));
  TEST_END("rabook container validation");
}

/**
 * @test internal_test_write_faults
 * @brief Verify short and failed destination callbacks propagate exactly.
 * @details Selects the first prefix write so no later payload behavior obscures
 *          the short-success conversion or backend error propagation.
 * @pre The source and workspace are otherwise valid.
 * @pre Each vector starts with fresh callback counters.
 * @post Short success returns ::k_ra8_err_invalid_size.
 * @post Backend failure returns ::k_ra8_fail unchanged.
 * @note The caller owns cleanup of the intentionally partial stage.
 * @since 0.1.0
 * @par MC/DC:
 * Callback gate `(call == short_write_call) && (copied != 0)` receives
 * `(T,T)->T` in the short vector and `(F,T)->F` in the round-trip control,
 * isolating the selected-call condition. No zero-byte write is issued, so the
 * copied-size condition is not claimed independently. Production exact-write
 * decisions are single conditions: short success flips `written == len`, and
 * the backend-failure vector flips `err != ok`; callback bounds stay `(F,F)`.
 */
RA8_INTERNAL static void internal_test_write_faults(void)
{
  TEST_BEGIN("rabook container write faults");
  ra8_rabook_container_workspace_t ws      = internal_full_workspace();
  uint64_t                         out_len = 0U;
  t_io_t                           io      = internal_full_io();
  io.short_write_call                      = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_rabook_container_write(internal_mem_read,
                                            &io,
                                            k_t_flat_bytes,
                                            k_t_chunk_bytes,
                                            internal_mem_write,
                                            &io,
                                            &ws,
                                            &out_len));
  io                 = internal_full_io();
  io.fail_write_call = 0U;
  TEST_ASSERT_EQ(k_ra8_fail,
                 ra8_rabook_container_write(internal_mem_read,
                                            &io,
                                            k_t_flat_bytes,
                                            k_t_chunk_bytes,
                                            internal_mem_write,
                                            &io,
                                            &ws,
                                            &out_len));
  TEST_END("rabook container write faults");
}

/**
 * @test internal_test_read_and_capacity_faults
 * @brief Verify short/failed reads and compressed-cap exhaustion propagate.
 * @details Drives the first payload read short, then failed, and finally leaves
 *          one output byte for a nonempty zlib stream.
 * @pre Prefix writes can fit the memory destination.
 * @pre Each vector starts from full valid source/workspace state.
 * @post Read faults map to their documented exact result.
 * @post Compression exhaustion returns ::k_ra8_err_no_mem.
 * @note No vector publishes or validates the partial destination.
 * @since 0.1.0
 * @par MC/DC:
 * Callback gate `(call == short_read_call) && (copied != 0)` receives
 * `(T,T)->T` here and `(F,T)->F` in the round-trip control, isolating the
 * selected-call condition; no zero-byte request isolates the second condition.
 * The backend-error selector and production `got != want` checks are separate
 * single decisions, flipped by failed and short reads. The one-byte compressed
 * capacity flips the compressor's single overflow guard. Callback range ORs
 * and the production payload-offset overflow OR remain `(F,F)->F` controls.
 */
RA8_INTERNAL static void internal_test_read_and_capacity_faults(void)
{
  TEST_BEGIN("rabook container read and capacity faults");
  ra8_rabook_container_workspace_t ws      = internal_full_workspace();
  uint64_t                         out_len = 0U;
  t_io_t                           io      = internal_full_io();
  io                                       = internal_full_io();
  io.short_read_call                       = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_rabook_container_write(internal_mem_read,
                                            &io,
                                            k_t_flat_bytes,
                                            k_t_chunk_bytes,
                                            internal_mem_write,
                                            &io,
                                            &ws,
                                            &out_len));
  io                = internal_full_io();
  io.fail_read_call = 0U;
  TEST_ASSERT_EQ(k_ra8_err_hw_error,
                 ra8_rabook_container_write(internal_mem_read,
                                            &io,
                                            k_t_flat_bytes,
                                            k_t_chunk_bytes,
                                            internal_mem_write,
                                            &io,
                                            &ws,
                                            &out_len));
  io                = internal_full_io();
  ws                = internal_full_workspace();
  ws.compressed_cap = 1U;
  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 ra8_rabook_container_write(internal_mem_read,
                                            &io,
                                            k_t_flat_bytes,
                                            k_t_chunk_bytes,
                                            internal_mem_write,
                                            &io,
                                            &ws,
                                            &out_len));
  TEST_END("rabook container read and capacity faults");
}

int main(void)
{
  internal_test_multi_chunk_round_trip();
  internal_test_validation();
  internal_test_write_faults();
  internal_test_read_and_capacity_faults();
  return 0;
}
