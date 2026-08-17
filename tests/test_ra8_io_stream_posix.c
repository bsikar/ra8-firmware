/**
 * @file test_ra8_io_stream_posix.c
 * @brief Contract tests for the raw-descriptor POSIX stream backend.
 *
 * @details
 * Exercises a real pipe plus an injected syscall model covering short writes,
 * bounded interruption retries, impossible progress, errno mapping, partial
 * failure counts, independent state, and failure-atomic initialization.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_io_stream.h"
#include "ra8_io_stream_posix.h"
#include "ra8_io_stream_posix_internal.h"
#include "unity_minimal.h"

/** @brief Focused model dimensions and sentinels. */
typedef enum : uint32_t {
  k_test_sequence_cap = 20U, /**< Maximum modeled write attempts. */
  k_test_eintr_count  = 17U, /**< One more than the retry limit.  */
  k_test_fake_fd      = 37U, /**< Descriptor forwarded to model.  */
  k_test_byte_count   = 3U,  /**< Exact payload byte count.       */
} stream_posix_test_const_t;

/** @brief One injected raw-write result. */
typedef struct {
  int64_t result;       /**< Returned native byte count. */
  int     error_number; /**< Captured errno on failure.  */
} write_step_t;

/** @brief Caller-owned deterministic raw-write model. */
typedef struct {
  write_step_t steps[(size_t)k_test_sequence_cap]; /**< Scripted results.      */
  uint32_t     count;                              /**< Script length.         */
  uint32_t     cursor;                             /**< Next result index.     */
  uint32_t     last_length;                        /**< Last requested length. */
  int          last_fd;                            /**< Last descriptor value. */
} write_model_t;

/** @brief Expected canonical mapping for one raw failure. */
typedef struct {
  int       error_number; /**< Injected errno.           */
  ra8_err_t expected;     /**< Expected canonical error. */
} errno_vector_t;

/** @brief Canonical descriptor-write errno vectors. */
static const errno_vector_t s_errno_vectors[] = {
  {.error_number = EAGAIN, .expected = k_ra8_err_would_block},
#if defined(EWOULDBLOCK) && (EWOULDBLOCK != EAGAIN)
  {.error_number = EWOULDBLOCK, .expected = k_ra8_err_would_block},
#endif
  {.error_number = EBADF, .expected = k_ra8_err_invalid_state},
  {.error_number = ENOSPC, .expected = k_ra8_err_no_mem},
  {.error_number = EDQUOT, .expected = k_ra8_err_no_mem},
  {.error_number = EACCES, .expected = k_ra8_err_access_denied},
  {.error_number = EPERM, .expected = k_ra8_err_access_denied},
  {.error_number = EFBIG, .expected = k_ra8_err_invalid_size},
  {.error_number = EOVERFLOW, .expected = k_ra8_err_invalid_size},
  {.error_number = EINVAL, .expected = k_ra8_err_invalid_arg},
  {.error_number = EPIPE, .expected = k_ra8_err_comm_error},
  {.error_number = EIO, .expected = k_ra8_err_comm_error},
  {.error_number = 0, .expected = k_ra8_err_protocol_error},
  {.error_number = ENOENT, .expected = k_ra8_fail},
};

/**
 * @brief Return the next deterministic raw-write result.
 * @param[in,out] context ::write_model_t model.
 * @param[in] fd Forwarded descriptor.
 * @param[in] bytes Source bytes, unused by the model.
 * @param[in] length Requested byte count.
 * @param[out] out_errno Scripted errno value.
 * @return Scripted native byte count.
 * @pre The model has an unconsumed step and all pointers are non-null.
 * @post The model cursor advances and records @p fd and @p length. @details Exercises the model write path with bounded caller-owned fixture state and verifies its documented result. @retval value The computed fixture value for the supplied inputs. @pre Fixed-capacity fixture storage required by this operation is available. @post Documented outputs contain the exercised result when the operation succeeds. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static int64_t
internal_model_write(void* context, int fd, const uint8_t* bytes, uint32_t length, int* out_errno)
{
  write_model_t* model = (write_model_t*)context;
  (void)bytes;
  TEST_ASSERT(model->cursor < model->count);
  const write_step_t step = model->steps[model->cursor];
  ++model->cursor;
  model->last_fd     = fd;
  model->last_length = length;
  *out_errno         = step.error_number;
  return step.result;
}

/**
 * @brief Reset one model to a single raw failure.
 * @param[out] model Model to reset.
 * @param[in] error_number Failure errno.
 * @post The next modeled call returns negative one with @p error_number. @details Exercises the set error path with bounded caller-owned fixture state and verifies its documented result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_set_error(write_model_t* model, int error_number)
{
  *model                       = (write_model_t){};
  model->steps[0].result       = -1;
  model->steps[0].error_number = error_number;
  model->count                 = 1U;
}

/**
 * @brief Prove actual pipe writes and borrowed-descriptor ownership.
 * @post The payload crosses the pipe exactly and both descriptors are closed by
 *       the test rather than the adapter. @details Executes the native pipe scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_native_pipe(void)
{
  TEST_BEGIN("POSIX stream native pipe");
  int descriptors[2] = {-1, -1};
  TEST_ASSERT_EQ(0, pipe(descriptors));
  ra8_io_stream_t             stream = {};
  ra8_io_stream_posix_state_t state  = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_posix_init(&stream, &state, descriptors[1]));
  const uint8_t payload[(size_t)k_test_byte_count] = {'a', 'b', 'c'};
  uint32_t      written                            = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_write(&stream, payload, sizeof(payload), &written));
  TEST_ASSERT_EQ(sizeof(payload), written);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_flush(&stream));
  uint8_t captured[(size_t)k_test_byte_count] = {};
  TEST_ASSERT_EQ(sizeof(captured), read(descriptors[0], captured, sizeof(captured)));
  TEST_ASSERT(memcmp(payload, captured, sizeof(payload)) == 0);
  TEST_ASSERT_EQ(0, close(descriptors[0]));
  TEST_ASSERT_EQ(0, close(descriptors[1]));
  TEST_END("POSIX stream native pipe");
}

/**
 * @brief Prove two descriptor bindings retain independent caller state.
 * @post Each pipe receives only its own byte and all descriptors are closed by
 *       the test. @details Executes the two bindings scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_two_bindings(void)
{
  TEST_BEGIN("POSIX stream two bindings");
  int first[2]  = {-1, -1};
  int second[2] = {-1, -1};
  TEST_ASSERT_EQ(0, pipe(first));
  TEST_ASSERT_EQ(0, pipe(second));
  ra8_io_stream_t             streams[2] = {};
  ra8_io_stream_posix_state_t states[2]  = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_posix_init(&streams[0], &states[0], first[1]));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_posix_init(&streams[1], &states[1], second[1]));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_putc(&streams[0], 'a'));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_putc(&streams[1], 'b'));
  uint8_t bytes[2] = {};
  TEST_ASSERT_EQ(1, read(first[0], &bytes[0], 1U));
  TEST_ASSERT_EQ(1, read(second[0], &bytes[1], 1U));
  TEST_ASSERT_EQ('a', bytes[0]);
  TEST_ASSERT_EQ('b', bytes[1]);
  TEST_ASSERT_EQ(0, close(first[0]));
  TEST_ASSERT_EQ(0, close(first[1]));
  TEST_ASSERT_EQ(0, close(second[0]));
  TEST_ASSERT_EQ(0, close(second[1]));
  TEST_END("POSIX stream two bindings");
}

/**
 * @brief Prove initialization validation and failure-atomic output handling.
 *
 * @par MC/DC:
 * `port/posix/src/ra8_io_stream_posix.c@ra8_io_stream_posix_init` validates
 * `stream == NULL || state == NULL`. The first two calls isolate C1 and C2;
 * the valid descriptor bind makes both operands false. These are the three
 * required vectors for the two-condition OR.
 * @post Invalid calls preserve both caller objects; valid calls bind the fd. @details Executes the init contract scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_init_contract(void)
{
  TEST_BEGIN("POSIX stream init");
  ra8_io_stream_t             stream = {};
  ra8_io_stream_posix_state_t state  = {.fd = -2};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_stream_posix_init(nullptr, &state, 1));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_stream_posix_init(&stream, nullptr, 1));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_io_stream_posix_init(&stream, &state, -1));
  TEST_ASSERT(stream.iface == nullptr);
  TEST_ASSERT_EQ(-2, state.fd);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_posix_init(&stream, &state, 1));
  TEST_ASSERT(stream.iface != nullptr);
  TEST_ASSERT_EQ(1, state.fd);
  TEST_END("POSIX stream init");
}

/**
 * @brief Prove short progress, interruption retry, and partial-error counts.
 * @post Exact writes succeed; partial failure returns the proven byte prefix. @details Executes the write progress scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_write_progress(void)
{
  TEST_BEGIN("POSIX stream progress");
  const uint8_t bytes[(size_t)k_test_byte_count] = {1U, 2U, 3U};
  write_model_t model                            = {
    .steps = {{.result = -1, .error_number = EINTR},
              {.result = 1, .error_number = 0},
              {.result = 2, .error_number = 0}},
    .count = 3U,
  };
  uint32_t written = 99U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 priv_ra8_io_stream_posix_write_loop((int)k_test_fake_fd,
                                                     bytes,
                                                     sizeof(bytes),
                                                     &written,
                                                     internal_model_write,
                                                     &model));
  TEST_ASSERT_EQ(sizeof(bytes), written);
  TEST_ASSERT_EQ(3U, model.cursor);
  TEST_ASSERT_EQ(k_test_fake_fd, model.last_fd);
  TEST_ASSERT_EQ(2U, model.last_length);

  model = (write_model_t){
    .steps = {{.result = 1, .error_number = 0}, {.result = -1, .error_number = EPIPE}},
    .count = 2U,
  };
  TEST_ASSERT_EQ(k_ra8_err_comm_error,
                 priv_ra8_io_stream_posix_write_loop((int)k_test_fake_fd,
                                                     bytes,
                                                     sizeof(bytes),
                                                     &written,
                                                     internal_model_write,
                                                     &model));
  TEST_ASSERT_EQ(1U, written);
  TEST_END("POSIX stream progress");
}

/**
 * @brief Prove null arguments and a zero-length transfer fail or succeed exactly.
 * @details Exercises each required pointer independently, then confirms the
 *          injected writer is not called for an empty valid transfer.
 * @param[in] bytes Non-null source fixture.
 * @param[in] length Fixture byte count.
 * @return Nothing.
 * @pre @p bytes spans @p length readable bytes.
 * @pre @p length is non-zero for the null-argument vectors.
 * @post Null calls preserve the sentinel; the empty call publishes zero.
 * @post The injected model cursor remains at zero.
 * @note Mutates only stack-local fixture state and the test counters.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_write_arguments(const uint8_t* bytes, uint32_t length)
{
  write_model_t null_model = {};
  uint32_t      written    = 99U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 priv_ra8_io_stream_posix_write_loop(1,
                                                     nullptr,
                                                     length,
                                                     &written,
                                                     internal_model_write,
                                                     &null_model));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 priv_ra8_io_stream_posix_write_loop(1,
                                                     bytes,
                                                     length,
                                                     nullptr,
                                                     internal_model_write,
                                                     &null_model));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    priv_ra8_io_stream_posix_write_loop(1, bytes, length, &written, nullptr, &null_model));
  TEST_ASSERT_EQ(99U, written);
  TEST_ASSERT_EQ(
    k_ra8_ok,
    priv_ra8_io_stream_posix_write_loop(1, bytes, 0U, &written, internal_model_write, &null_model));
  TEST_ASSERT_EQ(0U, written);
  TEST_ASSERT_EQ(0U, null_model.cursor);
}

/**
 * @brief Prove zero and over-reported successful progress are rejected.
 * @details Injects a successful zero-byte result and a result larger than the
 *          requested extent through the same exact-write seam.
 * @param[in] bytes Source fixture.
 * @param[in] length Fixture byte count.
 * @return Nothing.
 * @pre @p bytes spans @p length readable bytes and @p length is non-zero.
 * @pre The injected model accepts one scripted result per vector.
 * @post Both impossible results publish zero accepted bytes.
 * @post Both vectors return ::k_ra8_err_protocol_error.
 * @note Mutates only stack-local fixture state and the test counters.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_write_impossible(const uint8_t* bytes, uint32_t length)
{
  uint32_t      written = 99U;
  write_model_t model   = {.steps = {{.result = 0, .error_number = 0}}, .count = 1U};
  TEST_ASSERT_EQ(
    k_ra8_err_protocol_error,
    priv_ra8_io_stream_posix_write_loop(1, bytes, length, &written, internal_model_write, &model));
  TEST_ASSERT_EQ(0U, written);
  model = (write_model_t){.steps = {{.result = 4, .error_number = 0}}, .count = 1U};
  TEST_ASSERT_EQ(
    k_ra8_err_protocol_error,
    priv_ra8_io_stream_posix_write_loop(1, bytes, length, &written, internal_model_write, &model));
  TEST_ASSERT_EQ(0U, written);
}

/**
 * @brief Prove exhausted interruption retries publish no progress.
 * @details Fills the complete bounded model with interrupted attempts so the
 *          adapter must stop at its documented ceiling.
 * @param[in] bytes Source fixture.
 * @param[in] length Fixture byte count.
 * @return Nothing.
 * @pre @p bytes spans @p length readable bytes and @p length is non-zero.
 * @pre ::k_test_eintr_count matches the adapter's exhausted-attempt vector.
 * @post The exact retry ceiling is consumed with zero accepted bytes.
 * @post The adapter returns ::k_ra8_err_retry_limit.
 * @note Mutates only stack-local fixture state and the test counters.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_write_retry_limit(const uint8_t* bytes, uint32_t length)
{
  write_model_t model = {};
  model.count         = (uint32_t)k_test_eintr_count;
  for (uint32_t index = 0U; index < model.count; ++index) {
    model.steps[index] = (write_step_t){.result = -1, .error_number = EINTR};
  }
  uint32_t written = 99U;
  TEST_ASSERT_EQ(
    k_ra8_err_retry_limit,
    priv_ra8_io_stream_posix_write_loop(1, bytes, length, &written, internal_model_write, &model));
  TEST_ASSERT_EQ(0U, written);
  TEST_ASSERT_EQ(k_test_eintr_count, model.cursor);
}

/**
 * @brief Prove impossible progress and exhausted interruptions fail closed.
 *
 * @par MC/DC:
 * `port/posix/src/ra8_io_stream_posix.c@priv_ra8_io_stream_posix_write_loop`
 * (pointer guard) and
 * `port/posix/src/ra8_io_stream_posix.c@internal_write_loop_step`
 * (progress guard, factored out of the retry loop body) together contain the
 * two compound guards. The pointer guard's three one-null calls plus
 * the valid zero-length call isolate C1, C2, C3, and all-false. The progress
 * guard's zero-result and over-report calls isolate its two true operands;
 * internal_test_write_progress() supplies a positive in-range result with both
 * operands false. These are the N+1 sets for the three-condition and
 * two-condition ORs.
 * @post No vector publishes bytes that were not accepted. @details Executes the write protocol scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_write_protocol(void)
{
  TEST_BEGIN("POSIX stream protocol");
  const uint8_t bytes[(size_t)k_test_byte_count] = {1U, 2U, 3U};
  internal_test_write_arguments(bytes, sizeof(bytes));
  internal_test_write_impossible(bytes, sizeof(bytes));
  internal_test_write_retry_limit(bytes, sizeof(bytes));
  TEST_END("POSIX stream protocol");
}

/**
 * @brief Prove every adapter-specific errno mapping.
 *
 * @par MC/DC:
 * `port/posix/src/ra8_io_stream_posix.c@internal_map_errno` contains five
 * two-condition errno ORs. The table supplies each left errno, each right
 * errno, and ENOENT as the shared all-false result. EWOULDBLOCK is included
 * separately exactly on hosts where it differs from EAGAIN; where they are
 * equal the preprocessor removes that second condition. Each compiled OR
 * therefore has its required three vectors.
 * @post Each vector returns its canonical status with zero accepted bytes. @details Executes the errno map scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_errno_map(void)
{
  TEST_BEGIN("POSIX stream errno");
  const uint8_t byte = 1U;
  for (uint32_t index = 0U;
       index < (uint32_t)(sizeof(s_errno_vectors) / sizeof(s_errno_vectors[0]));
       ++index) {
    write_model_t model = {};
    internal_set_error(&model, s_errno_vectors[index].error_number);
    uint32_t written = 99U;
    TEST_ASSERT_EQ(
      s_errno_vectors[index].expected,
      priv_ra8_io_stream_posix_write_loop(1, &byte, 1U, &written, internal_model_write, &model));
    TEST_ASSERT_EQ(0U, written);
  }
  TEST_END("POSIX stream errno");
}

int main(void)
{
  internal_test_native_pipe();
  internal_test_two_bindings();
  internal_test_init_contract();
  internal_test_write_progress();
  internal_test_write_protocol();
  internal_test_errno_map();
  return 0;
}
