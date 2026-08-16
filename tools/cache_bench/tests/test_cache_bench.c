/**
 * @file test_cache_bench.c
 * @brief Capacity, isolation, mutation, and publication tests for cache_bench.
 * @details Exercises bounded short I/O, exact caller workspace, source
 *          mutation/fault rejection, and failure-atomic host publication.
 *
 * [Ring 7 / Tooling] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cache_bench.h"
#include "cache_bench_host.h"
#include "cache_bench_io.h"
#include "ra8_attributes.h"
#include "trace.h"

typedef struct {
  uint8_t* bytes;     /**< Borrowed source backing.         */
  size_t   length;    /**< Valid source extent.             */
  size_t   max_read;  /**< Per-call short-read ceiling.     */
  uint32_t calls;     /**< Read callback invocation count.  */
  uint32_t fail_call; /**< One-based injected failure call. */
} test_source_t;

typedef struct {
  uint8_t bytes[64];  /**< Fixed captured output.           */
  size_t  length;     /**< Bytes accepted so far.           */
  size_t  max_write;  /**< Per-call short-write ceiling.    */
  size_t  fail_after; /**< Accepted-byte failure threshold. */
} test_sink_t;

static int s_failures;

/**
 * @brief Record one contract assertion without aborting later checks.
 * @details Increments the file-local failure count only when @p condition is false.
 * @param[in] condition Assertion result.
 * @pre The test process is single-threaded.
 * @pre `s_failures` has been initialized by static storage duration.
 * @post A true condition leaves `s_failures` unchanged.
 * @post A false condition increments `s_failures` exactly once.
 * @note Continuing after failures exposes independent contract regressions.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_expect(bool condition)
{
  if (!condition) {
    s_failures++;
  }
}

/**
 * @brief Serve a bounded, fault-injectable test source read.
 * @details Applies call-failure and short-read controls before copying from the
 *          caller-owned byte array.
 * @param[in,out] ctx Bound ::test_source_t.
 * @param[in] offset Source byte offset.
 * @param[out] destination Read destination.
 * @param[in] capacity Maximum requested bytes.
 * @param[out] out_read Receives the copied prefix length.
 * @return Tool-local I/O status.
 * @retval k_cb_io_ok A short/full read or EOF completed.
 * @retval k_cb_io_fault The configured failure call was reached.
 * @pre All pointers are non-NULL and the source bytes cover `source->length`.
 * @pre @p destination is writable for @p capacity bytes.
 * @post On success, @p out_read does not exceed @p capacity.
 * @post The source call count advances exactly once.
 * @note The callback deliberately supports one-byte reads.
 * @since 0.1.0
 */
RA8_INTERNAL
static cb_io_status_t internal_test_source_read(void*    ctx,
                                                uint64_t offset,
                                                uint8_t* destination,
                                                size_t   capacity,
                                                size_t*  out_read)
{
  test_source_t* source = (test_source_t*)ctx;
  source->calls++;
  if ((source->fail_call != 0U) && (source->calls == source->fail_call)) {
    return k_cb_io_fault;
  }
  if (offset >= source->length) {
    *out_read = 0U;
    return k_cb_io_ok;
  }
  size_t span = source->length - (size_t)offset;
  if (span > capacity) {
    span = capacity;
  }
  if ((source->max_read != 0U) && (span > source->max_read)) {
    span = source->max_read;
  }
  memcpy(destination, &source->bytes[offset], span);
  *out_read = span;
  return k_cb_io_ok;
}

/**
 * @brief Capture a bounded, fault-injectable test sink write.
 * @details Applies failure and short-write controls before appending to the
 *          fixed capture buffer.
 * @param[in,out] ctx Bound ::test_sink_t.
 * @param[in] data Fragment bytes.
 * @param[in] length Requested byte count.
 * @param[out] out_written Receives the appended prefix length.
 * @return Tool-local I/O status.
 * @retval k_cb_io_ok A short or full prefix was appended.
 * @retval k_cb_io_fault The configured failure threshold was reached.
 * @retval k_cb_io_capacity The fixed capture buffer is too small.
 * @pre All pointers are non-NULL and @p data covers @p length bytes.
 * @pre Existing sink length does not exceed its fixed buffer.
 * @post On success, length advances by exactly @p out_written.
 * @post On failure, captured bytes and length are unchanged.
 * @note Short writes exercise ::cb_sink_write_all retry behavior.
 * @since 0.1.0
 */
RA8_INTERNAL
static cb_io_status_t
internal_test_sink_write(void* ctx, const uint8_t* data, size_t length, size_t* out_written)
{
  test_sink_t* sink = (test_sink_t*)ctx;
  if ((sink->fail_after != 0U) && (sink->length >= sink->fail_after)) {
    return k_cb_io_fault;
  }
  size_t span = length;
  if ((sink->max_write != 0U) && (span > sink->max_write)) {
    span = sink->max_write;
  }
  if (span > (sizeof(sink->bytes) - sink->length)) {
    return k_cb_io_capacity;
  }
  memcpy(&sink->bytes[sink->length], data, span);
  sink->length += span;
  *out_written = span;
  return k_cb_io_ok;
}

/**
 * @brief Verify complete publication across short writes and sink faults.
 * @details Exercises one-byte progress to completion and a deterministic
 *          mid-publication failure.
 * @pre Test sink callbacks satisfy their local binding contracts.
 * @pre `s_failures` is available to record assertions.
 * @post Successful capture equals the six expected bytes.
 * @post Fault injection is reported as ::k_cb_io_fault.
 * @note This test acquires no external resources.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_sink_contract(void)
{
  test_sink_t short_binding = {.max_write = 1U};
  cb_sink_t   short_sink    = {.write = internal_test_sink_write, .ctx = &short_binding};
  internal_test_expect(cb_sink_write_all(&short_sink, "abcdef", 6U) == k_cb_io_ok);
  internal_test_expect((short_binding.length == 6U) &&
                       (memcmp(short_binding.bytes, "abcdef", 6U) == 0));

  test_sink_t fault_binding = {.max_write = 2U, .fail_after = 3U};
  cb_sink_t   fault_sink    = {.write = internal_test_sink_write, .ctx = &fault_binding};
  internal_test_expect(cb_sink_write_all(&fault_sink, "abcdef", 6U) == k_cb_io_fault);
}

/**
 * @brief Verify trace binding, workspace isolation, mutation, and source faults.
 * @details Uses one-byte injected reads and two distinct workspaces to prove
 *          exact sizing and full workspace re-initialisation before exercising
 *          failure paths.
 * @pre The registered policy table contains at least one policy.
 * @pre Local aligned arrays cover the expected small replay demand.
 * @post Equivalent workspaces produce identical three-access results.
 * @post A workspace dirtied between replays yields the same result.
 * @post Mutated and faulting sources are rejected.
 * @note All test storage has automatic duration.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_trace_and_workspace(void)
{
  uint8_t       bytes[]        = "1 2\n3 4\n5 6\n";
  test_source_t source_binding = {.bytes = bytes, .length = sizeof(bytes) - 1U, .max_read = 1U};
  cb_source_t   source         = {.read = internal_test_source_read,
                                  .ctx  = &source_binding,
                                  .size = sizeof(bytes) - 1U};
  cb_trace_t    trace          = {};
  internal_test_expect(cb_trace_bind(&source, "tiny", 4U, &trace) == k_cb_io_ok);
  internal_test_expect(trace.n == 3U);

  const cache_policy_t* policy   = g_cb_policies[0];
  const size_t          required = cb_replay_workspace_required(policy, 4U);
  internal_test_expect((required > 0U) && (required < 4096U));
  alignas(max_align_t) uint8_t first[4096]  = {};
  alignas(max_align_t) uint8_t second[4096] = {};
  cb_workspace_t               too_small    = {.data = first, .capacity = required - 1U};
  cb_result_t                  result       = {.accesses = 99U};
  internal_test_expect(cb_replay(policy, &trace, 4U, &too_small, &result) != 0);
  internal_test_expect((too_small.required == required) && (result.accesses == 0U));

  cb_workspace_t one = {.data = first, .capacity = required};
  cb_workspace_t two = {.data = second, .capacity = required};
  cb_result_t    a   = {};
  cb_result_t    b   = {};
  internal_test_expect(cb_replay(policy, &trace, 4U, &one, &a) == 0);
  internal_test_expect(cb_replay(policy, &trace, 4U, &two, &b) == 0);
  internal_test_expect((memcmp(&a, &b, sizeof(a)) == 0) && (a.accesses == 3U));

  /* A workspace holding stale bytes must be fully re-initialised on reuse. */
  one.data[0] ^= 0x5AU;
  cb_result_t reused = {};
  internal_test_expect(cb_replay(policy, &trace, 4U, &one, &reused) == 0);
  internal_test_expect(memcmp(&a, &reused, sizeof(a)) == 0);

  bytes[0] = (uint8_t)'9';
  internal_test_expect(cb_replay(policy, &trace, 4U, &one, &a) != 0);

  bytes[0]                 = (uint8_t)'1';
  source_binding.calls     = 0U;
  source_binding.fail_call = 1U;
  internal_test_expect(cb_replay(policy, &trace, 4U, &one, &a) != 0);
}

/**
 * @brief Compare a host file prefix against expected bytes.
 * @details Opens without following a final symlink, reads into a fixed local
 *          buffer, closes, and requires exact length and content.
 * @param[in] path NUL-terminated host path.
 * @param[in] expected Expected bytes.
 * @param[in] length Exact expected length.
 * @return Whether open, read, close, length, and content all match.
 * @retval true Every comparison condition succeeded.
 * @retval false A host operation or byte comparison failed.
 * @pre @p path and @p expected are non-NULL.
 * @pre @p length does not exceed the fixed local buffer.
 * @post Any successfully opened descriptor is closed.
 * @post File bytes are not modified.
 * @note This helper is limited to the short preservation fixture.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_test_fd_bytes(const char* path, const char* expected, size_t length)
{
  const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    return false;
  }
  uint8_t       bytes[16] = {};
  const ssize_t count     = read(fd, bytes, sizeof(bytes));
  const bool    close_ok  = close(fd) == 0;
  return close_ok && (count == (ssize_t)length) && (memcmp(bytes, expected, length) == 0);
}

/**
 * @brief Verify abort preserves and commit replaces an output destination.
 * @details Creates a temporary destination with old bytes, exercises both
 *          transaction outcomes, then removes the fixture.
 * @pre The host provides writable `/tmp` and POSIX descriptor operations.
 * @pre `s_failures` is available to record assertions.
 * @post Abort leaves `old`; commit publishes `new`.
 * @post The fixture path is unlinked after the successful sequence.
 * @note An early `mkstemp` failure records a failure and returns safely.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_output_preservation(void)
{
  char      path[] = "/tmp/cache-bench-test.XXXXXX";
  const int fd     = mkstemp(path);
  internal_test_expect(fd >= 0);
  if (fd < 0) {
    return;
  }
  internal_test_expect(write(fd, "old", 3U) == 3);
  internal_test_expect(close(fd) == 0);

  cb_host_output_t binding = {};
  cb_sink_t        sink    = {};
  internal_test_expect(cb_host_output_open(path, &binding, &sink) == k_cb_io_ok);
  internal_test_expect(cb_sink_write_all(&sink, "new", 3U) == k_cb_io_ok);
  cb_host_output_abort(&binding);
  internal_test_expect(internal_test_fd_bytes(path, "old", 3U));

  internal_test_expect(cb_host_output_open(path, &binding, &sink) == k_cb_io_ok);
  internal_test_expect(cb_sink_write_all(&sink, "new", 3U) == k_cb_io_ok);
  internal_test_expect(cb_host_output_commit(&binding) == k_cb_io_ok);
  internal_test_expect(internal_test_fd_bytes(path, "new", 3U));
  internal_test_expect(unlink(path) == 0);
}

int main(void)
{
  internal_test_sink_contract();
  internal_test_trace_and_workspace();
  internal_test_output_preservation();
  return (s_failures == 0) ? 0 : 1;
}
