/**
 * @file test_ra8_fmt_stream_verify.c
 * @brief Golden and fault tests for bounded two-spool JOF verification.
 * @details Exercises independent short-reading sources, anonymous-spool models,
 * seal/read/write faults, truncation, corruption, stability failure, durable
 * dump preservation, and two independent workspace bindings.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "ra8_attributes.h"
#include "ra8_fmt_stream.h"
#include "ra8_log.h"

#ifndef RA8_FMT_TEST_PNG
#error "RA8_FMT_TEST_PNG must name the PNG golden fixture"
#endif

/** @brief Fixed test storage capacities. */
typedef enum : uint32_t {
  k_test_image_cap  = 256U,     /**< Encoded fixture storage.       */
  k_test_spool_cap  = 8192U,    /**< One modeled scratch atlas.     */
  k_test_work_cap   = 1048576U, /**< Per-instance verifier arena.   */
  k_test_report_cap = 4096U,    /**< Captured diagnostic bytes.     */
  k_test_dump_cap   = 2048U,    /**< Captured PPM bytes.            */
  k_test_short_read = 5U,       /**< Maximum source bytes per read. */
  k_test_scratch_at = 4096U,    /**< Comparison scratch offset.     */
  k_test_row_at     = 16384U,   /**< Reference row offset.          */
} test_const_t;

/** @brief Short-reading immutable source model with injected mutation evidence. */
typedef struct {
  const uint8_t* bytes;              /**< Encoded fixture bytes.           */
  size_t         len;                /**< Declared source size.            */
  size_t         extent;             /**< Actually readable prefix.        */
  size_t         max_chunk;          /**< Maximum bytes returned per call. */
  size_t         xor_offset;         /**< Read-time corruption offset.     */
  uint8_t        xor_mask;           /**< Read-time corruption mask.       */
  uint32_t       validate_calls;     /**< Stability callback count.        */
  uint32_t       fail_validate_call; /**< Injected mutation observation.   */
} test_source_t;

/** @brief Append-seal-positioned-read scratch model. */
typedef struct {
  uint8_t  bytes[k_test_spool_cap]; /**< Scratch bytes.                  */
  size_t   len;                     /**< Appended byte count.            */
  uint32_t append_calls;            /**< Append callback count.          */
  uint32_t read_calls;              /**< Positioned-read callback count. */
  uint32_t fail_append_call;        /**< Injected append failure.        */
  uint32_t fail_read_call;          /**< Injected seek/read failure.     */
  bool     fail_seal;               /**< Injected seal failure.          */
  bool     sealed;                  /**< Immutable read phase entered.   */
} test_spool_t;

/** @brief Durable diagnostic PPM transaction model. */
typedef struct {
  uint8_t  stage[k_test_dump_cap];     /**< Unpublished PPM bytes.       */
  uint8_t  published[k_test_dump_cap]; /**< Visible committed PPM bytes. */
  size_t   stage_len;                  /**< Staged byte count.           */
  size_t   published_len;              /**< Visible byte count.          */
  uint32_t append_calls;               /**< Transaction append count.    */
  uint32_t fail_append_call;           /**< Injected dump write fault.   */
  bool     committed;                  /**< Commit callback observed.    */
  bool     aborted;                    /**< Abort callback observed.     */
} test_dump_t;

/** @brief Bounded report capture. */
typedef struct {
  uint8_t bytes[k_test_report_cap]; /**< Report bytes plus reserved NUL. */
  size_t  len;                      /**< Bytes captured before NUL.      */
} test_report_t;

alignas(max_align_t) static uint8_t s_work_a[k_test_work_cap];
alignas(max_align_t) static uint8_t s_work_b[k_test_work_cap];
static uint8_t s_image_a[k_test_image_cap];
static uint8_t s_image_b[k_test_image_cap];
static int     s_failures;

/** @brief Record a failed assertion while continuing fault cleanup. */
#define CHECK(expression)                                                                          \
  do {                                                                                             \
    if (!(expression)) {                                                                           \
      s_failures++;                                                                                \
    }                                                                                              \
  } while (false)

/** @copydoc ra8_log_byte_sink_fn_t */
RA8_INTERNAL
static void internal_log(void* ctx, uint8_t byte)
{
  (void)ctx;
  (void)byte;
}

/**
 * @brief Read one tiny fixture through raw descriptors.
 * @details Loops until EOF or fixed test capacity without stdio ownership.
 * @param[in] path Fixture path.
 * @param[out] bytes Fixed destination.
 * @return Complete fixture byte count, or zero on failure.
 * @retval 0 Open failed or no fixture byte was read.
 * @pre @p path is NUL-terminated.
 * @pre @p bytes spans ::k_test_image_cap writable bytes.
 * @post Success closes the descriptor and fills the returned prefix.
 * @post Failure owns no descriptor.
 * @note Test composition edge only.
 * @since 0.1.0
 */
RA8_INTERNAL
static size_t internal_load(const char* path, uint8_t bytes[k_test_image_cap])
{
  const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    return 0U;
  }
  size_t len = 0U;
  while (len < k_test_image_cap) {
    const ssize_t rc = read(fd, &bytes[len], k_test_image_cap - len);
    if (rc > 0) {
      len += (size_t)rc;
    } else {
      break;
    }
  }
  (void)close(fd);
  return len;
}

/**
 * @brief Read a bounded span from the verifier test source.
 * @details Applies the configured extent and short-read behavior while
 * publishing only a count that fits the caller's destination.
 * @param[in,out] ctx Test-owned ::test_source_t callback context.
 * @param[in] offset Absolute source byte offset.
 * @param[out] bytes Destination for returned source bytes.
 * @param[in] len Writable capacity of @p bytes.
 * @param[out] got Number of bytes returned.
 * @return Canonical callback status.
 * @retval k_ra8_ok A bounded span or clean end-of-source was returned.
 * @retval other Configured test-source failure.
 * @pre @p ctx and @p got point to valid test storage.
 * @pre Nonzero @p len requires writable @p bytes storage.
 * @post Success reports no more than @p len bytes.
 * @post Reads never extend beyond the configured source extent.
 * @note Host-only deterministic fault-injection seam.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_source_read(void* ctx, uint64_t offset, uint8_t* bytes, size_t len, size_t* got)
{
  test_source_t* source = (test_source_t*)ctx;
  *got                  = 0U;
  if (offset >= source->extent) {
    return k_ra8_ok;
  }
  size_t take = source->extent - (size_t)offset;
  if (take > len) {
    take = len;
  }
  if ((source->max_chunk != 0U) && (take > source->max_chunk)) {
    take = source->max_chunk;
  }
  (void)memcpy(bytes, &source->bytes[offset], take);
  if ((source->xor_mask != 0U) && (source->xor_offset >= offset) &&
      (source->xor_offset < (offset + take))) {
    bytes[source->xor_offset - (size_t)offset] ^= source->xor_mask;
  }
  *got = take;
  return k_ra8_ok;
}

/** @copydoc ra8_fmt_source_validate_fn */
RA8_INTERNAL
static ra8_err_t internal_source_validate(void* ctx, uint64_t expected_size)
{
  test_source_t* source = (test_source_t*)ctx;
  source->validate_calls++;
  if ((expected_size != source->len) || ((source->fail_validate_call != 0U) &&
                                         (source->validate_calls == source->fail_validate_call))) {
    return k_ra8_err_validation_failed;
  }
  return k_ra8_ok;
}

/**
 * @brief Bind one model source to the portable contract.
 * @details Exposes independent read and mutation-validation callbacks.
 * @param[in,out] model Source model.
 * @return Complete portable source view.
 * @retval other A view bound to @p model and its declared extent.
 * @pre @p model is non-null and initialized.
 * @pre Model bytes span the declared readable extent.
 * @post Returned callbacks retain only @p model.
 * @post Model state is not changed.
 * @note Separate models prove independent source contexts.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_fmt_source_t internal_source(test_source_t* model)
{
  return (ra8_fmt_source_t){
    .read_at  = internal_source_read,
    .validate = internal_source_validate,
    .ctx      = model,
    .size     = model->len,
  };
}

/** @copydoc ra8_fmt_sink_write_fn */
RA8_INTERNAL
static ra8_err_t internal_spool_append(void* ctx, const uint8_t* bytes, size_t len)
{
  test_spool_t* spool = (test_spool_t*)ctx;
  spool->append_calls++;
  if (spool->sealed ||
      ((spool->fail_append_call != 0U) && (spool->append_calls == spool->fail_append_call))) {
    return k_ra8_fail;
  }
  if (len > (sizeof(spool->bytes) - spool->len)) {
    return k_ra8_err_no_mem;
  }
  (void)memcpy(&spool->bytes[spool->len], bytes, len);
  spool->len += len;
  return k_ra8_ok;
}

/** @copydoc ra8_fmt_spool_seal_fn */
RA8_INTERNAL
static ra8_err_t internal_spool_seal(void* ctx, uint64_t expected_size)
{
  test_spool_t* spool = (test_spool_t*)ctx;
  if (spool->fail_seal || (expected_size != spool->len)) {
    return k_ra8_fail;
  }
  spool->sealed = true;
  return k_ra8_ok;
}

/**
 * @brief Read a bounded span from the sealed verifier test spool.
 * @details Rejects unsealed or fault-injected reads, then copies only the
 * available suffix within the caller's requested capacity.
 * @param[in,out] ctx Test-owned ::test_spool_t callback context.
 * @param[in] offset Absolute spool byte offset.
 * @param[out] bytes Destination for returned spool bytes.
 * @param[in] len Writable capacity of @p bytes.
 * @param[out] got Number of bytes returned.
 * @return Canonical callback status.
 * @retval k_ra8_ok A bounded span or clean end-of-spool was returned.
 * @retval k_ra8_fail The spool is unsealed or the read fault fired.
 * @pre @p ctx and @p got point to valid test storage.
 * @pre Nonzero @p len requires writable @p bytes storage.
 * @post Success reports no more than @p len bytes.
 * @post Failure reports zero accepted bytes.
 * @note Every invocation increments the deterministic read-call counter.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_spool_read(void* ctx, uint64_t offset, uint8_t* bytes, size_t len, size_t* got)
{
  test_spool_t* spool = (test_spool_t*)ctx;
  spool->read_calls++;
  *got = 0U;
  if (!spool->sealed ||
      ((spool->fail_read_call != 0U) && (spool->read_calls == spool->fail_read_call))) {
    return k_ra8_fail;
  }
  if (offset >= spool->len) {
    return k_ra8_ok;
  }
  size_t take = spool->len - (size_t)offset;
  if (take > len) {
    take = len;
  }
  (void)memcpy(bytes, &spool->bytes[offset], take);
  *got = take;
  return k_ra8_ok;
}

/**
 * @brief Bind one modeled spool contract.
 * @details Exposes append, exact seal, and positioned-read fault seams.
 * @param[in,out] model Spool model.
 * @return Complete portable spool view.
 * @retval other A view bound to @p model.
 * @pre @p model is non-null and initialized.
 * @pre The model begins unsealed for producer use.
 * @post Returned callbacks retain only @p model.
 * @post Model storage is not changed.
 * @note Test-only deterministic spool backend.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_fmt_spool_t internal_spool(test_spool_t* model)
{
  return (ra8_fmt_spool_t){
    .read_at = internal_spool_read,
    .append  = internal_spool_append,
    .seal    = internal_spool_seal,
    .ctx     = model,
  };
}

/** @copydoc ra8_fmt_sink_write_fn */
RA8_INTERNAL
static ra8_err_t internal_dump_append(void* ctx, const uint8_t* bytes, size_t len)
{
  test_dump_t* dump = (test_dump_t*)ctx;
  dump->append_calls++;
  if ((dump->fail_append_call != 0U) && (dump->append_calls == dump->fail_append_call)) {
    return k_ra8_fail;
  }
  if (len > (sizeof(dump->stage) - dump->stage_len)) {
    return k_ra8_err_no_mem;
  }
  (void)memcpy(&dump->stage[dump->stage_len], bytes, len);
  dump->stage_len += len;
  return k_ra8_ok;
}

/**
 * @brief Commit one complete modeled PPM.
 * @details Copies staged bytes into the visible model and clears the stage.
 * @param[in,out] ctx Bound ::test_dump_t.
 * @return Commit status.
 * @retval k_ra8_ok The complete stage became visible.
 * @pre @p ctx points to an initialized dump model.
 * @pre Staged length fits visible storage.
 * @post Published bytes exactly equal the prior stage.
 * @post Stage length becomes zero and committed is true.
 * @note Test model performs no filesystem I/O.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_dump_commit(void* ctx)
{
  test_dump_t* dump = (test_dump_t*)ctx;
  (void)memcpy(dump->published, dump->stage, dump->stage_len);
  dump->published_len = dump->stage_len;
  dump->committed     = true;
  dump->stage_len     = 0U;
  return k_ra8_ok;
}

/**
 * @brief Abort one modeled PPM stage without touching visible bytes.
 * @details Clears unpublished length and records abort evidence.
 * @param[in,out] ctx Bound ::test_dump_t.
 * @pre @p ctx points to an initialized dump model.
 * @pre No model callback executes concurrently.
 * @post Stage length becomes zero.
 * @post Published bytes and length remain unchanged.
 * @note Test model is idempotent for sequential cleanup.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_dump_abort(void* ctx)
{
  test_dump_t* dump = (test_dump_t*)ctx;
  dump->stage_len   = 0U;
  dump->aborted     = true;
}

static const ra8_fmt_transaction_ops_t s_dump_ops = {
  .append = internal_dump_append,
  .commit = internal_dump_commit,
  .abort  = internal_dump_abort,
};

/** @copydoc ra8_fmt_sink_write_fn */
RA8_INTERNAL
static ra8_err_t internal_report(void* ctx, const uint8_t* bytes, size_t len)
{
  test_report_t* report = (test_report_t*)ctx;
  if (len >= (sizeof(report->bytes) - report->len)) {
    return k_ra8_err_no_mem;
  }
  (void)memcpy(&report->bytes[report->len], bytes, len);
  report->len += len;
  report->bytes[report->len] = '\0';
  return k_ra8_ok;
}

/**
 * @brief Bind phase-overlaid verifier views into one test arena.
 * @details Uses fixed offsets sized for the tiny golden fixture.
 * @param[in,out] arena Caller test workspace.
 * @param[in] need Exact verifier requirements.
 * @return Complete verifier workspace view.
 * @retval other A view whose every span lies in @p arena.
 * @pre @p arena spans ::k_test_work_cap bytes.
 * @pre @p need describes the 24-by-32 golden fixture.
 * @post Returned spans retain only @p arena.
 * @post No arena byte is initialized.
 * @note Tests bind this independently to two arenas.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_fmt_jof_verify_workspace_t
internal_workspace(uint8_t arena[k_test_work_cap], const ra8_fmt_jof_verify_requirements_t* need)
{
  return (ra8_fmt_jof_verify_workspace_t){
    .work          = arena,
    .work_cap      = k_test_work_cap,
    .webp_work     = nullptr,
    .webp_work_cap = 0U,
    .band_tile     = arena,
    .band_tile_cap = need->band_tile_bytes,
    .scratch       = &arena[k_test_scratch_at],
    .scratch_cap   = need->scratch_bytes,
    .row           = &arena[k_test_row_at],
    .row_cap       = need->row_bytes,
  };
}

/**
 * @brief Initialize one source model with legal five-byte short reads.
 * @details Binds the complete encoded extent and disables corruption injection.
 * @param[in] bytes Encoded fixture storage.
 * @param[in] len Exact fixture length.
 * @return Initialized source model.
 * @retval other A model bound to @p bytes with short-read limiting enabled.
 * @pre @p bytes spans @p len readable bytes.
 * @pre @p len fits ::k_test_image_cap.
 * @post Returned model owns no fixture storage.
 * @post Input bytes remain unchanged.
 * @note Each call yields an independent callback context.
 * @since 0.1.0
 */
RA8_INTERNAL
static test_source_t internal_source_model(const uint8_t* bytes, size_t len)
{
  return (test_source_t){
    .bytes      = bytes,
    .len        = len,
    .extent     = len,
    .max_chunk  = k_test_short_read,
    .xor_offset = SIZE_MAX,
  };
}

/**
 * @brief Invoke one complete model verification.
 * @details Binds independent sources, spools, workspace, dump, and report seams.
 * @param[in,out] ref_source Reference source model.
 * @param[in,out] got_source Subject source model.
 * @param[in] need Exact requirements.
 * @param[in,out] arena Caller workspace.
 * @param[in,out] ref_spool Reference spool model.
 * @param[in,out] got_spool Subject spool model.
 * @param[in,out] dump Optional dump model.
 * @param[in,out] report Report model.
 * @return Verifier status.
 * @retval k_ra8_ok The modeled round trip was exact.
 * @retval other Injected source, spool, output, or decoder status.
 * @pre Every required model and @p need are initialized.
 * @pre @p arena spans ::k_test_work_cap bytes.
 * @post All model evidence remains inspectable by the caller.
 * @post No model ownership escapes the call.
 * @note The optional dump is absent only when @p dump is null.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_invoke(test_source_t*                           ref_source,
                                 test_source_t*                           got_source,
                                 const ra8_fmt_jof_verify_requirements_t* need,
                                 uint8_t                                  arena[k_test_work_cap],
                                 test_spool_t*                            ref_spool,
                                 test_spool_t*                            got_spool,
                                 test_dump_t*                             dump,
                                 test_report_t*                           report)
{
  ra8_fmt_source_t               ref         = internal_source(ref_source);
  ra8_fmt_source_t               got         = internal_source(got_source);
  ra8_fmt_spool_t                ref_view    = internal_spool(ref_spool);
  ra8_fmt_spool_t                got_view    = internal_spool(got_spool);
  ra8_fmt_jof_verify_workspace_t workspace   = internal_workspace(arena, need);
  const ra8_fmt_sink_t           report_view = {.write = internal_report, .ctx = report};
  ra8_fmt_transaction_t          transaction = {.ops = &s_dump_ops, .ctx = dump};
  return ra8_fmt_jof_verify_stream(&ref,
                                   &got,
                                   need,
                                   &workspace,
                                   &ref_view,
                                   &got_view,
                                   (dump == nullptr) ? nullptr : &transaction,
                                   (dump == nullptr) ? nullptr : "golden.ppm",
                                   &report_view);
}

/**
 * @brief Prove exact legacy report and PPM output through two workspaces.
 * @details Runs the same PNG fixture through independent caller arenas.
 * @param[in] len Encoded fixture length.
 * @param[in] need Exact verifier requirements.
 * @pre The two source fixture arrays contain identical @p len bytes.
 * @pre Both fixed workspaces satisfy @p need.
 * @post Both instances commit the exact 781-byte P5 output.
 * @post Both reports exactly match the legacy golden text.
 * @note The final assertion proves the arena bindings do not alias.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_golden(size_t len, const ra8_fmt_jof_verify_requirements_t* need)
{
  static const char expected[] =
    "verify: 24x32 bpp=1 | reference 1 tile | banded 1 tiles of 32 rows\n"
    "  wrote reassembled raster to golden.ppm (ok)\n"
    "verdict: ROUND-TRIP EXACT -- the produced file is correct (0 differing bytes)\n";
  for (uint32_t instance = 0U; instance < 2U; ++instance) {
    test_source_t  ref       = internal_source_model(s_image_a, len);
    test_source_t  got       = internal_source_model(s_image_b, len);
    test_spool_t   ref_spool = {};
    test_spool_t   got_spool = {};
    test_dump_t    dump      = {};
    test_report_t  report    = {};
    uint8_t* const arena     = (instance == 0U) ? s_work_a : s_work_b;
    CHECK(internal_invoke(&ref, &got, need, arena, &ref_spool, &got_spool, &dump, &report) ==
          k_ra8_ok);
    CHECK(ref.max_chunk == k_test_short_read);
    CHECK(got.max_chunk == k_test_short_read);
    CHECK(dump.committed && !dump.aborted && (dump.published_len == 781U));
    CHECK(memcmp(dump.published, "P5\n24 32\n255\n", 13U) == 0);
    CHECK(strcmp((const char*)report.bytes, expected) == 0);
  }
  CHECK(&s_work_a[0] != &s_work_b[0]);
}

/**
 * @brief Exercise source truncation, corruption, and concurrent-mutation guards.
 * @details Reinitializes every model between independent fault vectors.
 * @param[in] len Encoded fixture length.
 * @param[in] need Exact verifier requirements.
 * @pre Golden source arrays contain @p len bytes.
 * @pre Caller workspace satisfies @p need.
 * @post Every injected fault prevents dump commit.
 * @post Final stability failure returns validation status.
 * @note Assertions accumulate without skipping cleanup evidence.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_sources(size_t len, const ra8_fmt_jof_verify_requirements_t* need)
{
  test_source_t ref       = internal_source_model(s_image_a, len);
  test_source_t got       = internal_source_model(s_image_b, len);
  test_spool_t  ref_spool = {};
  test_spool_t  got_spool = {};
  test_dump_t   dump      = {};
  test_report_t report    = {};
  ref.extent              = len / 2U;
  CHECK(internal_invoke(&ref, &got, need, s_work_a, &ref_spool, &got_spool, &dump, &report) !=
        k_ra8_ok);
  CHECK(dump.aborted && !dump.committed);
  ref            = internal_source_model(s_image_a, len);
  got            = internal_source_model(s_image_b, len);
  ref_spool      = (test_spool_t){};
  got_spool      = (test_spool_t){};
  dump           = (test_dump_t){};
  report         = (test_report_t){};
  got.xor_offset = 16U;
  got.xor_mask   = 0x40U;
  CHECK(internal_invoke(&ref, &got, need, s_work_a, &ref_spool, &got_spool, &dump, &report) !=
        k_ra8_ok);
  CHECK(dump.aborted && !dump.committed);
  ref                    = internal_source_model(s_image_a, len);
  got                    = internal_source_model(s_image_b, len);
  ref.fail_validate_call = 2U;
  ref_spool              = (test_spool_t){};
  got_spool              = (test_spool_t){};
  dump                   = (test_dump_t){};
  report                 = (test_report_t){};
  CHECK(internal_invoke(&ref, &got, need, s_work_a, &ref_spool, &got_spool, &dump, &report) ==
        k_ra8_err_validation_failed);
  CHECK(dump.aborted && !dump.committed);
}

/**
 * @brief Exercise spool append, seal, and positioned-read failures.
 * @details Runs one fresh model set for each anonymous-spool protocol seam.
 * @param[in] len Encoded fixture length.
 * @param[in] need Exact verifier requirements.
 * @pre Golden source arrays contain @p len bytes.
 * @pre Caller workspace satisfies @p need.
 * @post Every injected spool fault fails verification.
 * @post No failed vector commits the dump.
 * @note Model storage makes fault calls deterministic.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_spools(size_t len, const ra8_fmt_jof_verify_requirements_t* need)
{
  for (uint32_t vector = 0U; vector < 3U; ++vector) {
    test_source_t ref       = internal_source_model(s_image_a, len);
    test_source_t got       = internal_source_model(s_image_b, len);
    test_spool_t  ref_spool = {};
    test_spool_t  got_spool = {};
    test_dump_t   dump      = {};
    test_report_t report    = {};
    if (vector == 0U) {
      ref_spool.fail_append_call = 2U;
    } else if (vector == 1U) {
      ref_spool.fail_seal = true;
    } else {
      ref_spool.fail_read_call = 1U;
    }
    CHECK(internal_invoke(&ref, &got, need, s_work_a, &ref_spool, &got_spool, &dump, &report) !=
          k_ra8_ok);
    CHECK(dump.aborted && !dump.committed);
  }
}

/**
 * @brief Prove dump failure preserves exact verdict and prior visible bytes.
 * @details Injects the first stage append failure after sources fully validate.
 * @param[in] len Encoded fixture length.
 * @param[in] need Exact verifier requirements.
 * @pre Golden source arrays contain @p len bytes.
 * @pre Caller workspace satisfies @p need.
 * @post Verification remains exact despite diagnostic-output failure.
 * @post Prior visible bytes remain exactly unchanged.
 * @note Mirrors legacy output-failure exit semantics.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_dump_failure(size_t len, const ra8_fmt_jof_verify_requirements_t* need)
{
  test_source_t ref       = internal_source_model(s_image_a, len);
  test_source_t got       = internal_source_model(s_image_b, len);
  test_spool_t  ref_spool = {};
  test_spool_t  got_spool = {};
  test_dump_t   dump = {.published = {'o', 'l', 'd'}, .published_len = 3U, .fail_append_call = 1U};
  test_report_t report = {};
  CHECK(internal_invoke(&ref, &got, need, s_work_a, &ref_spool, &got_spool, &dump, &report) ==
        k_ra8_ok);
  CHECK(dump.aborted && !dump.committed && (dump.published_len == 3U));
  CHECK(memcmp(dump.published, "old", 3U) == 0);
  CHECK(strstr((const char*)report.bytes, "golden.ppm (FAILED)\n") != nullptr);
}

/**
 * @brief Reject an aliased source context before producer or dump mutation.
 * @details Binds one source model into both nominally independent source views.
 * @param[in] len Encoded fixture length.
 * @param[in] need Exact verifier requirements.
 * @pre Golden source storage contains @p len bytes.
 * @pre Caller workspace and both spool models are initialized.
 * @post Validation returns before either spool append callback runs.
 * @post Report and source model remain unmodified.
 * @note This proves context identity, not source-byte equality, is rejected.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_alias(size_t len, const ra8_fmt_jof_verify_requirements_t* need)
{
  test_source_t                  source_model = internal_source_model(s_image_a, len);
  ra8_fmt_source_t               source       = internal_source(&source_model);
  test_spool_t                   ref_model    = {};
  test_spool_t                   got_model    = {};
  ra8_fmt_spool_t                ref_spool    = internal_spool(&ref_model);
  ra8_fmt_spool_t                got_spool    = internal_spool(&got_model);
  test_report_t                  report_model = {};
  const ra8_fmt_sink_t           report       = {.write = internal_report, .ctx = &report_model};
  ra8_fmt_jof_verify_workspace_t work         = internal_workspace(s_work_a, need);
  CHECK(ra8_fmt_jof_verify_stream(&source,
                                  &source,
                                  need,
                                  &work,
                                  &ref_spool,
                                  &got_spool,
                                  nullptr,
                                  nullptr,
                                  &report) == k_ra8_err_null_ptr);
  CHECK((ref_model.append_calls == 0U) && (got_model.append_calls == 0U));
}

int main(void)
{
  ra8_log_set_byte_sink(internal_log, nullptr);
  const size_t len = internal_load(RA8_FMT_TEST_PNG, s_image_a);
  CHECK(len != 0U);
  (void)memcpy(s_image_b, s_image_a, len);
  test_source_t                     requirements_model  = internal_source_model(s_image_a, len);
  ra8_fmt_source_t                  requirements_source = internal_source(&requirements_model);
  ra8_fmt_jof_verify_requirements_t need                = {};
  CHECK(ra8_fmt_jof_verify_requirements(&requirements_source, &need) == k_ra8_ok);
  CHECK((need.width == 24U) && (need.height == 32U) && (need.bpp == 1U));
  internal_test_golden(len, &need);
  internal_test_sources(len, &need);
  internal_test_spools(len, &need);
  internal_test_dump_failure(len, &need);
  internal_test_alias(len, &need);
  return s_failures;
}
