/**
 * @file test_ra8_fmt_stream_convert.c
 * @brief Allocation-free JOF converter workspace and transaction tests.
 * @details Drives the real firmware producer from a raw-fd fixture while the
 * output transaction is an injected bounded model, covering independent
 * workspaces, exact golden bytes, arena shortfall, append failure, commit
 * failure, and destination preservation.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_fmt_host_fd_internal.h"
#include "ra8_fmt_stream.h"

#ifndef RA8_FMT_TEST_PNG
#error "RA8_FMT_TEST_PNG must name the PNG golden fixture"
#endif

/** @brief Bounded test storage and golden constants. */
typedef enum : uint32_t {
  k_test_work_cap     = 2097152U,    /**< Per-instance producer arena. */
  k_test_artifact_cap = 4096U,       /**< Model stage/final capacity.  */
  k_test_report_cap   = 512U,        /**< Model report capacity.       */
  k_test_golden_len   = 428U,        /**< Legacy page1.png JOF bytes.  */
  k_test_golden_fnv   = 0xFBD49033U, /**< Legacy artifact FNV-1a.      */
  k_test_fnv_offset   = 2166136261U, /**< FNV-1a offset.               */
  k_test_fnv_prime    = 16777619U,   /**< FNV-1a prime.                */
} test_const_t;

/** @brief Bounded modeled durable transaction. */
typedef struct {
  uint8_t  stage[k_test_artifact_cap];     /**< Unpublished bytes.              */
  uint8_t  published[k_test_artifact_cap]; /**< Visible prior/final bytes.      */
  size_t   stage_len;                      /**< Staged byte count.              */
  size_t   published_len;                  /**< Visible byte count.             */
  uint32_t append_calls;                   /**< Append invocation count.        */
  uint32_t fail_append_call;               /**< Injected failing call, or zero. */
  bool     fail_commit;                    /**< Inject commit failure.          */
  bool     committed;                      /**< Publication completed.          */
  bool     aborted;                        /**< Abort callback observed.        */
} test_transaction_t;

/** @brief Bounded append-only report model. */
typedef struct {
  uint8_t bytes[k_test_report_cap]; /**< Captured report bytes. */
  size_t  len;                      /**< Captured byte count.   */
} test_report_t;

alignas(max_align_t) static uint8_t s_work_a[k_test_work_cap];
alignas(max_align_t) static uint8_t s_work_b[k_test_work_cap];
static int s_failures;

/** @brief Record one failed assertion while allowing cleanup. */
#define CHECK(expression)                                                                          \
  do {                                                                                             \
    if (!(expression)) {                                                                           \
      s_failures++;                                                                                \
    }                                                                                              \
  } while (false)

/**
 * @brief Model an exact append and an optional injected sink fault.
 * @details Bounds every stage write and fails on one configured invocation.
 * @param[in,out] ctx Bound ::test_transaction_t.
 * @param[in] bytes Bytes offered by the producer.
 * @param[in] len Exact append length.
 * @return Modeled transaction status.
 * @retval k_ra8_ok Complete span entered staging storage.
 * @retval k_ra8_fail Configured call fault fired.
 * @pre @p ctx is non-null and names initialized model state.
 * @pre @p bytes spans @p len readable bytes when non-empty.
 * @post Success advances stage length by exactly @p len.
 * @post A fault never changes visible published bytes.
 * @note Test-only and single-threaded through one model.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_append(void* ctx, const uint8_t* bytes, size_t len)
{
  test_transaction_t* transaction = (test_transaction_t*)ctx;
  transaction->append_calls++;
  if ((transaction->fail_append_call != 0U) &&
      (transaction->append_calls == transaction->fail_append_call)) {
    return k_ra8_fail;
  }
  if ((len > (sizeof(transaction->stage) - transaction->stage_len)) ||
      ((bytes == nullptr) && (len != 0U))) {
    return k_ra8_err_invalid_size;
  }
  if (len != 0U) {
    (void)memcpy(&transaction->stage[transaction->stage_len], bytes, len);
  }
  transaction->stage_len += len;
  return k_ra8_ok;
}

/**
 * @brief Publish the complete modeled stage unless commit failure is injected.
 * @details Copies staging to visible storage only on the successful commit arm.
 * @param[in,out] ctx Bound ::test_transaction_t.
 * @return Modeled commit status.
 * @retval k_ra8_ok Staged bytes became visible atomically in the model.
 * @retval k_ra8_fail Configured commit fault preserved prior bytes.
 * @pre @p ctx names initialized transaction state.
 * @pre Stage length does not exceed fixed artifact capacity.
 * @post Success sets committed and replaces visible bytes exactly.
 * @post Failure leaves visible bytes and committed state unchanged.
 * @note Test-only and deterministic.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_commit(void* ctx)
{
  test_transaction_t* transaction = (test_transaction_t*)ctx;
  if (transaction->fail_commit) {
    return k_ra8_fail;
  }
  (void)memcpy(transaction->published, transaction->stage, transaction->stage_len);
  transaction->published_len = transaction->stage_len;
  transaction->committed     = true;
  return k_ra8_ok;
}

/**
 * @brief Discard modeled staging bytes without changing visible bytes.
 * @details Records cleanup evidence while resetting only unpublished state.
 * @param[in,out] ctx Bound ::test_transaction_t.
 * @pre @p ctx names initialized transaction state.
 * @pre No concurrent model callback uses the state.
 * @post Stage length becomes zero and aborted becomes true.
 * @post Visible bytes and visible length remain unchanged.
 * @note Test-only and idempotent for sequential calls.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_abort(void* ctx)
{
  test_transaction_t* transaction = (test_transaction_t*)ctx;
  transaction->stage_len          = 0U;
  transaction->aborted            = true;
}

static const ra8_fmt_transaction_ops_t s_transaction_ops = {
  .append = internal_append,
  .commit = internal_commit,
  .abort  = internal_abort,
};

/**
 * @brief Capture one exact report span.
 * @details Appends into bounded model storage and rejects any overflow.
 * @param[in,out] ctx Bound ::test_report_t.
 * @param[in] bytes Report bytes.
 * @param[in] len Exact byte count.
 * @return Capture status.
 * @retval k_ra8_ok Complete span was captured.
 * @retval k_ra8_err_invalid_size Span exceeds remaining capacity.
 * @pre @p ctx is non-null and initialized.
 * @pre @p bytes spans @p len readable bytes when non-empty.
 * @post Success advances captured length by exactly @p len.
 * @post Failure does not claim successful capture.
 * @note Test-only and single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_report(void* ctx, const uint8_t* bytes, size_t len)
{
  test_report_t* report = (test_report_t*)ctx;
  if ((len > (sizeof(report->bytes) - report->len)) || ((bytes == nullptr) && (len != 0U))) {
    return k_ra8_err_invalid_size;
  }
  if (len != 0U) {
    (void)memcpy(&report->bytes[report->len], bytes, len);
  }
  report->len += len;
  return k_ra8_ok;
}

/**
 * @brief Initialize one transaction with stable prior visible content.
 * @details Seeds `old` as the preservation sentinel and binds model operations.
 * @param[out] state Transaction model to initialize.
 * @param[out] view Portable transaction view to bind.
 * @pre Both output pointers are non-null and writable.
 * @pre No live transaction still references @p state.
 * @post Visible bytes equal the three-byte preservation sentinel.
 * @post @p view references @p state and the complete model operation table.
 * @note Test-only deterministic setup.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_init_transaction(test_transaction_t* state, ra8_fmt_transaction_t* view)
{
  static const uint8_t prior[] = {'o', 'l', 'd'};
  *state                       = (test_transaction_t){};
  (void)memcpy(state->published, prior, sizeof(prior));
  state->published_len = sizeof(prior);
  *view                = (ra8_fmt_transaction_t){.ops = &s_transaction_ops, .ctx = state};
}

/**
 * @brief Compute the stable FNV-1a evidence for one artifact.
 * @details Applies the fixed 32-bit recurrence to all bytes in order.
 * @param[in] bytes Artifact byte span.
 * @param[in] len Byte count.
 * @return Stable 32-bit digest.
 * @retval k_test_fnv_offset Empty-span digest.
 * @pre @p bytes spans @p len readable bytes.
 * @pre Golden constants match the FNV-1a 32-bit definition.
 * @post Input bytes remain unchanged.
 * @post Result depends on every byte and its position.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_fnv(const uint8_t* bytes, size_t len)
{
  uint32_t hash = k_test_fnv_offset;
  for (size_t i = 0U; i < len; ++i) {
    hash ^= bytes[i];
    hash *= k_test_fnv_prime;
  }
  return hash;
}

/**
 * @brief Bind one test workspace to a distinct aligned backing array.
 * @details Creates the PNG-only view used to prove caller ownership.
 * @param[in,out] bytes Aligned producer arena.
 * @param[in] cap Supplied capacity.
 * @return Complete workspace binding.
 * @retval non-NULL The returned work pointer is @p bytes.
 * @pre @p bytes spans ::k_test_work_cap aligned bytes.
 * @pre @p cap does not exceed that backing capacity.
 * @post Returned WebP members are null and zero.
 * @post Backing bytes are not modified during binding.
 * @note Pure apart from the returned view.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_fmt_jof_convert_workspace_t internal_workspace(uint8_t  bytes[k_test_work_cap],
                                                          uint32_t cap)
{
  return (ra8_fmt_jof_convert_workspace_t){.work          = bytes,
                                           .work_cap      = cap,
                                           .webp_work     = nullptr,
                                           .webp_work_cap = 0U};
}

/**
 * @brief Convert once through one supplied workspace and modeled transaction.
 * @details Seeds preservation state, binds a bounded report, and runs the real producer.
 * @param[in] source Open PNG source.
 * @param[in] requirements Exact conversion requirements.
 * @param[in,out] workspace Workspace binding under test.
 * @param[in,out] state Transaction state.
 * @return Converter status.
 * @retval k_ra8_ok Artifact was committed into model-visible storage.
 * @retval other Portable converter rejected input, workspace, or transaction work.
 * @pre Every pointer argument is non-null and remains live.
 * @pre @p requirements were derived from @p source.
 * @post @p state contains complete success or abort evidence.
 * @post Workspace ownership remains with the caller.
 * @note Test-only composition helper.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_convert(const ra8_fmt_source_t*                   source,
                                  const ra8_fmt_jof_convert_requirements_t* requirements,
                                  ra8_fmt_jof_convert_workspace_t*          workspace,
                                  test_transaction_t*                       state)
{
  ra8_fmt_transaction_t transaction;
  internal_init_transaction(state, &transaction);
  test_report_t        report_state = {};
  const ra8_fmt_sink_t report       = {.write = internal_report, .ctx = &report_state};
  return ra8_fmt_jof_convert_stream(source,
                                    requirements,
                                    workspace,
                                    &transaction,
                                    &report,
                                    "golden.jof");
}

/**
 * @brief Run the real producer twice into two independent workspaces.
 * @details Binds each of the two static arenas to its own workspace and
 * converts @p source into each, checking that both commit cleanly.
 * @param[in] source Open PNG source.
 * @param[in] requirements Exact requirements for @p source.
 * @param[out] first Receives the first transaction's model state.
 * @param[out] second Receives the second transaction's model state.
 * @pre Both pointers are non-null and the source remains open.
 * @pre Both static arenas are distinct and sufficiently aligned.
 * @post Both model transactions are committed without abort.
 * @note Test-only and sequential because the producer is documented non-reentrant.
 * @since 0.1.0
 */
RA8_INTERNAL
static void
internal_test_success_convert_both(const ra8_fmt_source_t*                   source,
                                   const ra8_fmt_jof_convert_requirements_t* requirements,
                                   test_transaction_t*                       first,
                                   test_transaction_t*                       second)
{
  ra8_fmt_jof_convert_workspace_t workspace_a = internal_workspace(s_work_a, k_test_work_cap);
  ra8_fmt_jof_convert_workspace_t workspace_b = internal_workspace(s_work_b, k_test_work_cap);
  CHECK(internal_convert(source, requirements, &workspace_a, first) == k_ra8_ok);
  CHECK(internal_convert(source, requirements, &workspace_b, second) == k_ra8_ok);
  CHECK(first->committed && second->committed && !first->aborted && !second->aborted);
}

/**
 * @brief Prove exact golden bytes and two independent workspace bindings.
 * @details Runs the real producer twice and checks legacy digest plus byte identity.
 * @param[in] source Open PNG source.
 * @param[in] requirements Exact requirements for @p source.
 * @pre Both pointers are non-null and the source remains open.
 * @pre Both static arenas are distinct and sufficiently aligned.
 * @post Both model transactions are committed without abort.
 * @post Any failed expectation increments only the shared failure counter.
 * @note Test-only and sequential because the producer is documented non-reentrant.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_success(const ra8_fmt_source_t*                   source,
                                  const ra8_fmt_jof_convert_requirements_t* requirements)
{
  CHECK(requirements->work_bytes <= k_test_work_cap);
  CHECK(&s_work_a[0] != &s_work_b[0]);
  test_transaction_t first;
  test_transaction_t second;
  internal_test_success_convert_both(source, requirements, &first, &second);
  CHECK(first.published_len == k_test_golden_len);
  CHECK(internal_fnv(first.published, first.published_len) == k_test_golden_fnv);
  CHECK((first.published_len == second.published_len) &&
        (memcmp(first.published, second.published, first.published_len) == 0));
}

/**
 * @brief Prove insufficient capacity aborts before any output append.
 * @details Supplies exactly one byte below the derived work requirement.
 * @param[in] source Open PNG source.
 * @param[in] requirements Exact requirements for @p source.
 * @pre Requirements report a non-zero work size within test backing storage.
 * @pre Source and backing remain live for the call.
 * @post Append count remains zero and abort is observed.
 * @post Prior visible `old` bytes remain exact.
 * @note Test-only capacity-bound vector.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_short_workspace(const ra8_fmt_source_t*                   source,
                                          const ra8_fmt_jof_convert_requirements_t* requirements)
{
  ra8_fmt_jof_convert_workspace_t workspace =
    internal_workspace(s_work_a, requirements->work_bytes - 1U);
  test_transaction_t    state;
  ra8_fmt_transaction_t transaction;
  internal_init_transaction(&state, &transaction);
  test_report_t        report_state = {};
  const ra8_fmt_sink_t report       = {.write = internal_report, .ctx = &report_state};
  CHECK(ra8_fmt_jof_convert_stream(source,
                                   requirements,
                                   &workspace,
                                   &transaction,
                                   &report,
                                   "short.jof") == k_ra8_err_invalid_size);
  CHECK(state.aborted && !state.committed && (state.append_calls == 0U));
  CHECK((state.published_len == 3U) && (memcmp(state.published, "old", 3U) == 0));
}

/**
 * @brief Prove append and commit failures preserve prior visible content.
 * @details Injects a second-append failure and a post-produce commit failure independently.
 * @param[in] source Open PNG source.
 * @param[in] requirements Exact requirements for @p source.
 * @pre Requirements fit the aligned test workspace.
 * @pre Source remains stable across both sequential conversions.
 * @post Both faults observe abort and no commit.
 * @post Prior visible `old` bytes survive both vectors exactly.
 * @note Test-only transaction fault matrix.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_faults(const ra8_fmt_source_t*                   source,
                                 const ra8_fmt_jof_convert_requirements_t* requirements)
{
  ra8_fmt_jof_convert_workspace_t workspace    = internal_workspace(s_work_a, k_test_work_cap);
  test_report_t                   report_state = {};
  const ra8_fmt_sink_t            report       = {.write = internal_report, .ctx = &report_state};
  test_transaction_t              state;
  ra8_fmt_transaction_t           transaction;
  internal_init_transaction(&state, &transaction);
  state.fail_append_call = 2U;
  CHECK(ra8_fmt_jof_convert_stream(source,
                                   requirements,
                                   &workspace,
                                   &transaction,
                                   &report,
                                   "append.jof") == k_ra8_fail);
  CHECK(state.aborted && !state.committed && (state.append_calls == 2U));
  CHECK((state.published_len == 3U) && (memcmp(state.published, "old", 3U) == 0));
  internal_init_transaction(&state, &transaction);
  state.fail_commit = true;
  CHECK(ra8_fmt_jof_convert_stream(source,
                                   requirements,
                                   &workspace,
                                   &transaction,
                                   &report,
                                   "commit.jof") == k_ra8_fail);
  CHECK(state.aborted && !state.committed && (state.stage_len == 0U));
  CHECK((state.published_len == 3U) && (memcmp(state.published, "old", 3U) == 0));
}

int main(void)
{
  ra8_fmt_host_source_t source = {.fd = -1};
  CHECK(priv_fmt_host_source_open(RA8_FMT_TEST_PNG, 4096U, &source) == k_ra8_ok);
  ra8_fmt_jof_convert_requirements_t requirements = {};
  CHECK(ra8_fmt_jof_convert_requirements(&source.source, &requirements) == k_ra8_ok);
  if (requirements.work_bytes != 0U) {
    internal_test_success(&source.source, &requirements);
    internal_test_short_workspace(&source.source, &requirements);
    internal_test_faults(&source.source, &requirements);
  }
  priv_fmt_host_source_close(&source);
  return s_failures;
}
