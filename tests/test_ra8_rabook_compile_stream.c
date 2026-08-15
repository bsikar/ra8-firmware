/**
 * @file test_ra8_rabook_compile_stream.c
 * @brief External-pool and callback-output tests for the RABOOK1 finalizer.
 *
 * @details
 * Qualifies byte parity between resident and external image pools, bounded
 * one-byte scratch transfers, short callback handling, destination exhaustion,
 * mixed pool-mode rejection, and 32-bit logical-layout overflow. The test uses
 * the same caller-owned canonical book fixture as the resident builder suite.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_book.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_rabook_compile.h"
#include "support/rabook_compile_test_fixture.h"
#include "unity_minimal.h"

/** @brief Caller-owned arenas shared by the stream tests in this executable. */
static ra8_test_rabook_fixture_t s_fixture;

/** @brief Bounded read/write fixture used by the streaming-finalizer tests. */
typedef struct {
  const uint8_t* read_data;   /**< External image-pool bytes.         */
  uint8_t*       write_data;  /**< Stream destination bytes.          */
  uint32_t       read_len;    /**< Readable external-pool byte count. */
  uint32_t       write_cap;   /**< Writable destination byte count.   */
  uint32_t       write_used;  /**< Bytes appended so far.             */
  uint32_t       read_calls;  /**< Read callback invocation count.    */
  uint32_t       write_calls; /**< Write callback invocation count.   */
  uint32_t       max_read;    /**< Largest one-call read request.     */
  bool           short_read;  /**< Return one byte short once.        */
  bool           short_write; /**< Return one byte short once.        */
} rabook_stream_io_t;

/**
 * @brief Exact external-pool reader with an opt-in short-read fault.
 * @details Copies from the fixture pool, tracks request high-water, and can
 *          return one deliberately short successful transfer.
 * @param[in,out] opaque Fixture I/O state.
 * @param[in] offset External-pool byte offset.
 * @param[out] dst Read destination.
 * @param[in] requested Requested bytes.
 * @param[out] out_read Actual copied bytes.
 * @return Fixture read status.
 * @retval k_ra8_ok Full or deliberately short fixture transfer.
 * @retval k_ra8_err_invalid_size Requested range exceeds the fixture pool.
 * @pre All pointer arguments are non-NULL.
 * @pre @p dst spans @p requested writable bytes.
 * @post The call count and maximum request are updated.
 * @post @p out_read reports exactly the initialized destination prefix.
 * @note Test-only and not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_stream_read(void*     opaque,
                                      uint32_t  offset,
                                      uint8_t*  dst,
                                      uint32_t  requested,
                                      uint32_t* out_read)
{
  rabook_stream_io_t* io = (rabook_stream_io_t*)opaque;
  io->read_calls++;
  if (requested > io->max_read) {
    io->max_read = requested;
  }
  if ((offset > io->read_len) || (requested > (io->read_len - offset))) {
    *out_read = 0U;
    return k_ra8_err_invalid_size;
  }
  uint32_t delivered = requested;
  if (io->short_read && (requested != 0U)) {
    delivered--;
    io->short_read = false;
  }
  (void)memcpy(dst, &io->read_data[offset], (size_t)delivered);
  *out_read = delivered;
  return k_ra8_ok;
}

/**
 * @brief Bounded stream writer with opt-in short-write/output-exhaustion faults.
 * @details Appends into the fixture destination and can return either a deliberate
 *          short success or a bounded-capacity error.
 * @param[in,out] opaque Fixture I/O state.
 * @param[in] src Source bytes.
 * @param[in] requested Requested bytes.
 * @param[out] out_written Actual copied bytes.
 * @return Fixture write status.
 * @retval k_ra8_ok Full or deliberately short fixture transfer.
 * @retval k_ra8_err_invalid_size Destination capacity is exhausted.
 * @pre All pointer arguments are non-NULL.
 * @pre @p src spans @p requested readable bytes.
 * @post The write call count is incremented.
 * @post @p out_written reports exactly the appended prefix.
 * @note Test-only and not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_stream_write(void* opaque, const uint8_t* src, uint32_t requested, uint32_t* out_written)
{
  rabook_stream_io_t* io = (rabook_stream_io_t*)opaque;
  io->write_calls++;
  if (requested > (io->write_cap - io->write_used)) {
    *out_written = 0U;
    return k_ra8_err_invalid_size;
  }
  uint32_t delivered = requested;
  if (io->short_write && (requested != 0U)) {
    delivered--;
    io->short_write = false;
  }
  (void)memcpy(&io->write_data[io->write_used], src, (size_t)delivered);
  io->write_used += delivered;
  *out_written = delivered;
  return k_ra8_ok;
}

/**
 * @test internal_test_rabook_stream_external_parity
 * @brief A one-byte-scratch external finalizer is byte-identical to memory finalize.
 * @details Builds the same complete fixture through each image-pool API, validates
 *          the streamed result, and compares every emitted byte including CRC.
 * @pre Shared fixture arenas are writable.
 * @pre The production validator is linked into this host test.
 * @post Internal and external stream outputs equal the memory-wrapper blob.
 * @post External reads never exceed the one-byte caller scratch.
 * @note Test-only; mutates shared fixture arenas.
 * @since 0.1.0
 * @par MC/DC:
 * Decision `external = (pool_size != 0) && (mode == external)` receives
 * `(T,F)->F` from the internal-pool finalization and `(T,T)->T` from the
 * external-pool finalization. Holding a nonempty pool true therefore isolates
 * mode condition. A zero-size pool cannot establish external mode because the
 * builder latches that mode only for a nonempty reservation, so this test does
 * not claim independence for the pool-size condition. External reader/scratch
 * guards receive their all-valid false control.
 */
RA8_INTERNAL
static void internal_test_rabook_stream_external_parity(void)
{
  TEST_BEGIN("ra8_rabook_compile: external stream == memory blob");
  ra8_rabook_ctx_t            ctx = {};
  ra8_test_rabook_roundtrip_t rt  = {};
  ra8_test_rabook_init(&s_fixture, &ctx);
  ra8_test_rabook_populate(&s_fixture, &ctx, &rt, false);
  const void* mem_blob = nullptr;
  uint32_t    mem_len  = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_finalize(&ctx, &mem_blob, &mem_len));
  (void)memcpy(s_fixture.expected, mem_blob, (size_t)mem_len);

  rabook_stream_io_t io         = {.write_data = s_fixture.stream_out,
                                   .write_cap  = (uint32_t)sizeof(s_fixture.stream_out)};
  uint32_t           stream_len = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rabook_finalize_stream(&ctx,
                                            nullptr,
                                            nullptr,
                                            internal_stream_write,
                                            &io,
                                            nullptr,
                                            0U,
                                            &stream_len));
  TEST_ASSERT_EQ(mem_len, stream_len);
  TEST_ASSERT_EQ(0, memcmp(s_fixture.expected, s_fixture.stream_out, (size_t)stream_len));

  ra8_test_rabook_init(&s_fixture, &ctx);
  rt = (ra8_test_rabook_roundtrip_t){};
  ra8_test_rabook_populate(&s_fixture, &ctx, &rt, true);
  io                 = (rabook_stream_io_t){.read_data  = s_fixture.external_pool,
                                            .write_data = s_fixture.stream_out,
                                            .read_len   = 2U,
                                            .write_cap  = (uint32_t)sizeof(s_fixture.stream_out)};
  uint8_t scratch[1] = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rabook_finalize_stream(&ctx,
                                            internal_stream_read,
                                            &io,
                                            internal_stream_write,
                                            &io,
                                            scratch,
                                            (uint32_t)sizeof(scratch),
                                            &stream_len));
  TEST_ASSERT_EQ(mem_len, stream_len);
  TEST_ASSERT_EQ(0, memcmp(s_fixture.expected, s_fixture.stream_out, (size_t)stream_len));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_validate(s_fixture.stream_out, (size_t)stream_len));
  TEST_ASSERT_EQ(1U, io.max_read);
  TEST_ASSERT_EQ(4U, io.read_calls); /* two bytes, once for CRC and once for emit */
  TEST_ASSERT(io.write_calls > 1U);
  TEST_END("ra8_rabook_compile: external stream == memory blob");
}

/**
 * @test internal_test_rabook_stream_short_io
 * @brief Successful-but-short callbacks are rejected and leave out_len untouched.
 * @details Injects one short read during the CRC pass and one short header write,
 *          proving neither is accepted as a complete stream.
 * @pre Shared fixture arenas are writable.
 * @pre The external cover fixture contains two readable bytes.
 * @post Both short transfers return @ref k_ra8_err_invalid_size.
 * @post The sentinel output length remains unchanged.
 * @note Test-only; mutates shared fixture arenas.
 * @since 0.1.0
 * @par MC/DC:
 * Fault gates `(short_read && requested != 0)` and
 * `(short_write && requested != 0)` each receive `(T,T)->T` here and the
 * companion parity test supplies `(F,T)->F`; with a nonzero transfer held true,
 * each short flag independently changes the delivered count. No zero-length
 * transfer is injected, so independence of `requested != 0` is not claimed.
 * Production `got == len` and `wrote == len` are single-condition decisions,
 * flipped by the short and full-success vectors respectively.
 */
RA8_INTERNAL
static void internal_test_rabook_stream_short_io(void)
{
  TEST_BEGIN("ra8_rabook_compile: short stream read/write rejected");
  ra8_rabook_ctx_t            ctx = {};
  ra8_test_rabook_roundtrip_t rt  = {};
  ra8_test_rabook_init(&s_fixture, &ctx);
  ra8_test_rabook_populate(&s_fixture, &ctx, &rt, true);
  uint8_t  scratch[2] = {};
  uint32_t out_len    = UINT32_C(0xA5A5A5A5);

  rabook_stream_io_t io = {.read_data  = s_fixture.external_pool,
                           .write_data = s_fixture.stream_out,
                           .read_len   = 2U,
                           .write_cap  = (uint32_t)sizeof(s_fixture.stream_out),
                           .short_read = true};
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_rabook_finalize_stream(&ctx,
                                            internal_stream_read,
                                            &io,
                                            internal_stream_write,
                                            &io,
                                            scratch,
                                            (uint32_t)sizeof(scratch),
                                            &out_len));
  TEST_ASSERT_EQ(UINT32_C(0xA5A5A5A5), out_len);
  TEST_ASSERT_EQ(0U, io.write_calls); /* CRC pass fails before header publication. */

  io = (rabook_stream_io_t){.read_data   = s_fixture.external_pool,
                            .write_data  = s_fixture.stream_out,
                            .read_len    = 2U,
                            .write_cap   = (uint32_t)sizeof(s_fixture.stream_out),
                            .short_write = true};
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_rabook_finalize_stream(&ctx,
                                            internal_stream_read,
                                            &io,
                                            internal_stream_write,
                                            &io,
                                            scratch,
                                            (uint32_t)sizeof(scratch),
                                            &out_len));
  TEST_ASSERT_EQ(UINT32_C(0xA5A5A5A5), out_len);
  TEST_ASSERT_EQ(1U, io.read_calls); /* complete CRC pass precedes the short header write */
  TEST_ASSERT_EQ(1U, io.write_calls);
  TEST_END("ra8_rabook_compile: short stream read/write rejected");
}

/**
 * @test internal_test_rabook_stream_write_exhaustion
 * @brief Destination exhaustion propagates without publishing a final length.
 * @details Gives the bounded test writer less than one header of capacity after
 *          a successful external-pool CRC pass.
 * @pre Shared fixture and external-pool arenas are writable.
 * @pre The bounded writer reports invalid size without modifying its cursor.
 * @post Finalization reports invalid size after one CRC read and one write call.
 * @post The caller's output length remains unchanged.
 * @note Test-only; exercises writer error propagation, not a short success.
 * @since 0.1.0
 * @par MC/DC:
 * No compound production decision is independently varied. The external-mode
 * selector is fixed at `(pool nonempty=T, mode external=T)->T`; after the CRC
 * read succeeds, the writer's single `requested > remaining` capacity guard
 * flips true with one byte available. The full-capacity parity vector supplies
 * that guard's false arm, while this vector proves the error leaves `out_len`
 * unpublished.
 */
RA8_INTERNAL
static void internal_test_rabook_stream_write_exhaustion(void)
{
  TEST_BEGIN("ra8_rabook_compile: stream destination exhaustion");
  ra8_rabook_ctx_t            ctx = {};
  ra8_test_rabook_roundtrip_t rt  = {};
  ra8_test_rabook_init(&s_fixture, &ctx);
  ra8_test_rabook_populate(&s_fixture, &ctx, &rt, true);

  rabook_stream_io_t io         = {.read_data  = s_fixture.external_pool,
                                   .write_data = s_fixture.stream_out,
                                   .read_len   = 2U,
                                   .write_cap  = 1U};
  uint8_t            scratch[2] = {};
  uint32_t           out_len    = UINT32_C(0xA5A5A5A5);
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_rabook_finalize_stream(&ctx,
                                            internal_stream_read,
                                            &io,
                                            internal_stream_write,
                                            &io,
                                            scratch,
                                            (uint32_t)sizeof(scratch),
                                            &out_len));
  TEST_ASSERT_EQ(1U, io.read_calls);
  TEST_ASSERT_EQ(1U, io.write_calls);
  TEST_ASSERT_EQ(0U, io.write_used);
  TEST_ASSERT_EQ(UINT32_C(0xA5A5A5A5), out_len);
  TEST_END("ra8_rabook_compile: stream destination exhaustion");
}

/**
 * @test internal_test_rabook_external_offsets
 * @brief External spans are contiguous and cannot be mixed with internal bytes.
 * @details Checks sequential offsets and mixed-mode rejection after the logical
 *          external pool becomes non-empty.
 * @pre Shared fixture arenas are writable.
 * @pre The image table admits at least two descriptors.
 * @post Reserved offsets exactly tile the logical image pool.
 * @post A later non-empty internal append latches builder failure.
 * @note Test-only; mutates shared fixture arenas.
 * @since 0.1.0
 * @par MC/DC:
 * External reservation guard `(data_size != 0) && (mode == internal)` sees
 * `(T,F)->F` for both reservations. The later internal append evaluates its
 * reciprocal guard `(data_size != 0) && (mode == external)` as `(T,T)->T`;
 * successful nonempty internal appends elsewhere in this file provide
 * `(T,F)->F`, isolating the mode condition. This test never appends zero bytes
 * while external mode is latched, so it does not claim independence for the
 * data-size condition. The output-pointer/failed OR guard stays `(F,F)->F`.
 */
RA8_INTERNAL
static void internal_test_rabook_external_offsets(void)
{
  TEST_BEGIN("ra8_rabook_compile: external pool offsets and mode");
  ra8_rabook_buffers_t buf = ra8_test_rabook_buffers(&s_fixture);
  ra8_rabook_ctx_t     ctx = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_compile_init(&ctx, &buf));
  const uint32_t href = ra8_rabook_intern(&ctx, "spooled.bin");
  uint32_t       off  = UINT32_MAX;
  TEST_ASSERT_EQ(0U,
                 ra8_rabook_add_image_external(&ctx,
                                               href,
                                               2U,
                                               2U,
                                               (uint8_t)k_ra8_book_image_gray4,
                                               (uint8_t)k_ra8_book_pixfmt_gray4,
                                               2U,
                                               &off));
  TEST_ASSERT_EQ(0U, off);
  TEST_ASSERT_EQ(1U,
                 ra8_rabook_add_image_external(&ctx,
                                               href,
                                               2U,
                                               3U,
                                               (uint8_t)k_ra8_book_image_gray4,
                                               (uint8_t)k_ra8_book_pixfmt_gray4,
                                               3U,
                                               &off));
  TEST_ASSERT_EQ(2U, off);
  TEST_ASSERT_EQ(5U, ctx.image_pool_size);

  static const uint8_t local[1] = {0U};
  TEST_ASSERT_EQ(k_ra8_book_nil,
                 ra8_rabook_add_image(&ctx,
                                      href,
                                      1U,
                                      1U,
                                      (uint8_t)k_ra8_book_image_gray4,
                                      (uint8_t)k_ra8_book_pixfmt_gray4,
                                      local,
                                      1U));
  TEST_ASSERT(ctx.failed);
  TEST_END("ra8_rabook_compile: external pool offsets and mode");
}

/**
 * @test internal_test_rabook_external_pool_overflow
 * @brief Logical external-pool uint32 addition overflow fails closed.
 * @details Places the logical cursor at UINT32_MAX and verifies that reserving
 *          one additional byte is rejected without descriptor publication.
 * @pre Shared fixture arenas are writable.
 * @pre The image table admits one descriptor.
 * @post The append returns nil and latches builder failure.
 * @post The logical pool size remains UINT32_MAX.
 * @note Test-only; directly establishes the overflow boundary state.
 * @since 0.1.0
 * @par MC/DC:
 * The targeted decision `data_size > UINT32_MAX - image_pool_size` is a single
 * condition: `(1 > 0)->T` here, versus the false arm in the normal external
 * offset vectors. Compound guards before it remain on `(out pointer null=F,
 * failed=F)->F` and `(data nonzero=T, internal mode=F)->F`; no compound
 * independence is claimed by this boundary vector.
 */
RA8_INTERNAL
static void internal_test_rabook_external_pool_overflow(void)
{
  TEST_BEGIN("ra8_rabook_compile: external pool uint32 overflow");
  ra8_rabook_buffers_t buf = ra8_test_rabook_buffers(&s_fixture);
  ra8_rabook_ctx_t     ctx = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_compile_init(&ctx, &buf));
  const uint32_t href = ra8_rabook_intern(&ctx, "spooled.bin");
  uint32_t       off  = 0U;
  ctx.image_pool_size = UINT32_MAX;
  TEST_ASSERT_EQ(k_ra8_book_nil,
                 ra8_rabook_add_image_external(&ctx,
                                               href,
                                               1U,
                                               1U,
                                               (uint8_t)k_ra8_book_image_gray4,
                                               (uint8_t)k_ra8_book_pixfmt_gray4,
                                               1U,
                                               &off));
  TEST_ASSERT(ctx.failed);
  TEST_ASSERT_EQ(UINT32_MAX, ctx.image_pool_size);
  TEST_END("ra8_rabook_compile: external pool uint32 overflow");
}

/**
 * @test internal_test_rabook_external_layout_overflow
 * @brief A representable external span that breaks final layout is rejected early.
 * @details Reserves UINT32_MAX logical pool bytes and verifies layout arithmetic
 *          fails before the finalizer invokes the spool reader or writer.
 * @pre Shared fixture arenas are writable.
 * @pre The image table admits one descriptor.
 * @post Finalization reports invalid size and performs no callback I/O.
 * @post The caller's output length remains unchanged.
 * @note Test-only; no UINT32_MAX backing allocation is required.
 * @since 0.1.0
 * @par MC/DC:
 * The targeted layout decision `total > UINT32_MAX` is a single condition and
 * is true for this UINT32_MAX pool; ordinary parity finalization supplies its
 * false arm. The external selector is fixed at `(pool nonempty=T, mode
 * external=T)->T`, and the external-reservation mode guard is `(T,F)->F`.
 * Those compound decisions are controls only; the layout error occurs before
 * any callback can add a further vector.
 */
RA8_INTERNAL
static void internal_test_rabook_external_layout_overflow(void)
{
  TEST_BEGIN("ra8_rabook_compile: external pool layout overflow");
  ra8_rabook_buffers_t buf = ra8_test_rabook_buffers(&s_fixture);
  ra8_rabook_ctx_t     ctx = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_compile_init(&ctx, &buf));
  const uint32_t href = ra8_rabook_intern(&ctx, "spooled.bin");
  uint32_t       off  = 0U;
  TEST_ASSERT_EQ(0U,
                 ra8_rabook_add_image_external(&ctx,
                                               href,
                                               1U,
                                               1U,
                                               (uint8_t)k_ra8_book_image_gray4,
                                               (uint8_t)k_ra8_book_pixfmt_gray4,
                                               UINT32_MAX,
                                               &off));
  rabook_stream_io_t io         = {.read_data  = s_fixture.external_pool,
                                   .write_data = s_fixture.stream_out,
                                   .read_len   = (uint32_t)sizeof(s_fixture.external_pool),
                                   .write_cap  = (uint32_t)sizeof(s_fixture.stream_out)};
  uint8_t            scratch[1] = {};
  uint32_t           out_len    = UINT32_C(0xA5A5A5A5);
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_rabook_finalize_stream(&ctx,
                                            internal_stream_read,
                                            &io,
                                            internal_stream_write,
                                            &io,
                                            scratch,
                                            (uint32_t)sizeof(scratch),
                                            &out_len));
  TEST_ASSERT_EQ(0U, io.read_calls); /* layout overflow precedes any spool access */
  TEST_ASSERT_EQ(0U, io.write_calls);
  TEST_ASSERT_EQ(UINT32_C(0xA5A5A5A5), out_len);
  TEST_END("ra8_rabook_compile: external pool layout overflow");
}

/**
 * @brief Discard host-test log bytes so failure-arm diagnostics avoid ITM MMIO.
 * @details Installed before any test invokes a production error path.
 * @param[in] ctx Unused sink context.
 * @param[in] byte Unused log byte.
 * @return Nothing.
 * @pre Installed from main before the first test.
 * @pre The host test executes single-threaded.
 * @post The byte is discarded.
 * @post No fixture state is modified.
 * @note Test-only and not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_log_sink(void* ctx, uint8_t byte)
{
  (void)ctx;
  (void)byte;
}

int32_t main(void)
{
  ra8_log_set_byte_sink(internal_log_sink, nullptr);
  internal_test_rabook_stream_external_parity();
  internal_test_rabook_stream_short_io();
  internal_test_rabook_stream_write_exhaustion();
  internal_test_rabook_external_offsets();
  internal_test_rabook_external_pool_overflow();
  internal_test_rabook_external_layout_overflow();
  return 0;
}
