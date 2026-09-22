/**
 * @file test_ra8_decomp_limits.c
 * @brief Tests for the unified decompression-limits policy engine.
 *
 * @details
 * The policy record + budget tracker in `libs/ra8_core/src/ra8_decomp_limits.c`
 * is the one enforcement seam every archive decoder charges, so this suite
 * proves each axis independently and adversarially:
 *
 *   1. the default policy is fully populated and validates,
 *   2. binding rejects a policy with ANY zero field (six vectors),
 *   3. every charge axis fails closed exactly at its bound (output cap,
 *      ratio + grace, entries, iterations, depth),
 *   4. the saturating arithmetic cannot be wrapped by hostile 64-bit values
 *      (output-counter wrap, ratio-product wrap, grace-sum wrap),
 *   5. the header-level declared-size check rejects lying headers with the
 *      same bounds at O(1) cost.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_decomp_limits.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/**
 * @enum td_dim_t
 * @brief Tightened bounds used across the suite (tests are magic-number exempt).
 */
typedef enum : uint32_t {
  k_td_cap     = 1000U, /**< Small output cap.       */
  k_td_ratio   = 4U,    /**< Small ratio bound.      */
  k_td_grace   = 10U,   /**< Small additive grace.   */
  k_td_entries = 3U,    /**< Small entry cap.        */
  k_td_iters   = 2U,    /**< Small iteration budget. */
  k_td_depth   = 1U,    /**< Single decode layer.    */
} td_dim_t;

/**
 * @struct td_zip_source_t
 * @brief Immutable source and fault state for ZIP preflight vectors.
 */
typedef struct {
  const uint8_t* bytes;      /**< Immutable container bytes.            */
  size_t         size;       /**< Exact readable byte count.            */
  bool           short_read; /**< Return one fewer byte than requested. */
} td_zip_source_t;

/**
 * @brief Create the fully tightened policy used by breach vectors.
 * @details Populates every policy dimension with the suite's deliberately small bound.
 * @return Complete policy value.
 * @retval ra8_decomp_limits_t Policy populated with the six test dimensions.
 * @pre The compile-time test dimensions are nonzero.
 * @pre The policy representation accepts each enumerated test dimension.
 * @post Every returned policy field is nonzero.
 * @post No shared state is read or modified.
 * @note Returning by value keeps test mutations independent.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_decomp_limits_t internal_tight(void)
{
  ra8_decomp_limits_t lim = {};
  lim.max_output_bytes    = (uint64_t)k_td_cap;
  lim.max_ratio           = (uint32_t)k_td_ratio;
  lim.ratio_grace_bytes   = (uint32_t)k_td_grace;
  lim.max_entries         = (uint32_t)k_td_entries;
  lim.max_iterations      = (uint32_t)k_td_iters;
  lim.max_depth           = (uint8_t)k_td_depth;
  return lim;
}

/**
 * @test internal_test_decomp_default_policy
 * @brief The default policy is fully populated and binds cleanly.
 * @details Compares every published default and exercises explicit, implicit, and null binding.
 * @pre The production default policy constants are available to the test target.
 * @pre A writable budget record can receive both explicit and implicit policies.
 * @post Both successful bindings start with a zero entry counter.
 * @post A null budget is rejected with k_ra8_err_null_ptr.
 * @note This vector establishes the baseline used by the breach-specific cases.
 * @since Version 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions under test -- the validator is six independent
 * single-condition checks, each driven by internal_test_decomp_zero_field_rejected.)
 */
RA8_INTERNAL static void internal_test_decomp_default_policy(void)
{
  TEST_BEGIN("decomp: default policy populated + binds");
  const ra8_decomp_limits_t lim = ra8_decomp_limits_default();
  TEST_ASSERT(lim.max_output_bytes == (uint64_t)k_ra8_decomp_def_output_bytes);
  TEST_ASSERT_EQ(k_ra8_decomp_def_max_ratio, lim.max_ratio);
  TEST_ASSERT_EQ(k_ra8_decomp_def_ratio_grace, lim.ratio_grace_bytes);
  TEST_ASSERT_EQ(k_ra8_decomp_def_max_entries, lim.max_entries);
  TEST_ASSERT_EQ(k_ra8_decomp_def_max_iters, lim.max_iterations);
  TEST_ASSERT_EQ(k_ra8_decomp_def_max_depth, lim.max_depth);

  ra8_decomp_budget_t b = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_decomp_budget_init(&b, &lim));
  TEST_ASSERT_EQ(0U, b.entries);
  /* NULL limits selects the same default policy. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_decomp_budget_init(&b, nullptr));
  TEST_ASSERT(b.limits.max_output_bytes == (uint64_t)k_ra8_decomp_def_output_bytes);
  /* NULL budget is refused. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_decomp_budget_init(nullptr, &lim));
  TEST_END("decomp: default policy populated + binds");
}

/**
 * @test internal_test_decomp_zero_field_rejected
 * @brief Binding rejects a policy with any single zero field.
 * @details Clears each independent policy dimension once while keeping all others valid.
 * @pre internal_tight returns a policy whose six dimensions are nonzero.
 * @pre The destination budget remains writable for each independent attempt.
 * @post Every single-zero policy returns k_ra8_err_invalid_arg.
 * @post No rejected policy becomes an active budget.
 * @note The vectors isolate the validator's six early-return decisions.
 * @since Version 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions under test -- `internal_limits_usable` is six independent
 * single-condition early returns; each vector below drives exactly one.)
 */
RA8_INTERNAL static void internal_test_decomp_zero_field_rejected(void)
{
  TEST_BEGIN("decomp: zero-field policies rejected");
  ra8_decomp_budget_t b = {};

  ra8_decomp_limits_t lim = internal_tight();
  lim.max_output_bytes    = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_decomp_budget_init(&b, &lim));
  lim           = internal_tight();
  lim.max_ratio = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_decomp_budget_init(&b, &lim));
  lim                   = internal_tight();
  lim.ratio_grace_bytes = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_decomp_budget_init(&b, &lim));
  lim             = internal_tight();
  lim.max_entries = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_decomp_budget_init(&b, &lim));
  lim                = internal_tight();
  lim.max_iterations = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_decomp_budget_init(&b, &lim));
  lim           = internal_tight();
  lim.max_depth = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_decomp_budget_init(&b, &lim));
  TEST_END("decomp: zero-field policies rejected");
}

/**
 * @test internal_test_decomp_output_and_ratio_axes
 * @brief The output cap and the ratio + grace bound each fire exactly.
 * @details Charges fresh budgets at each threshold and then one byte beyond it.
 * @pre internal_tight supplies an output cap larger than the ratio threshold vector.
 * @pre Each breach direction starts from a freshly initialized budget.
 * @post The exact ratio and absolute limits are accepted.
 * @post The next byte returns the corresponding bounded-decompression error.
 * @note A null budget is also rejected before either arithmetic path runs.
 * @since Version 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions under test -- the cap and ratio checks are
 * independent single-condition guards exercised in both directions here.)
 */
RA8_INTERNAL static void internal_test_decomp_output_and_ratio_axes(void)
{
  TEST_BEGIN("decomp: output cap + ratio bound fire exactly");
  const ra8_decomp_limits_t lim = internal_tight();
  ra8_decomp_budget_t       b   = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_decomp_budget_init(&b, &lim));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_decomp_budget_charge_output(nullptr, 0U, 0U));

  /* in=100: ratio bound admits 100*4+10 = 410 output bytes. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_decomp_budget_charge_output(&b, 100U, 410U));
  /* One more byte breaches the ratio, well under the absolute cap. */
  TEST_ASSERT_EQ(k_ra8_err_decomp_ratio, ra8_decomp_budget_charge_output(&b, 100U, 1U));

  /* Fresh budget: the absolute cap fires first when input is plentiful. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_decomp_budget_init(&b, &lim));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_decomp_budget_charge_output(&b, 500U, (uint64_t)k_td_cap));
  TEST_ASSERT_EQ(k_ra8_err_decomp_output_cap, ra8_decomp_budget_charge_output(&b, 500U, 1U));
  TEST_END("decomp: output cap + ratio bound fire exactly");
}

/**
 * @test internal_test_decomp_saturating_arithmetic
 * @brief Hostile 64-bit values saturate instead of wrapping any bound.
 * @details Drives output accumulation, ratio multiplication, and grace addition near UINT64_MAX.
 * @pre The tightened policy has a finite absolute output cap.
 * @pre Each arithmetic scenario starts with an independently initialized budget.
 * @post Counter overflow attempts report the output-cap error instead of wrapping.
 * @post Ratio-bound overflow saturates while the absolute cap remains authoritative.
 * @note The vectors intentionally use values far above realistic archive sizes.
 * @since Version 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions under test -- each overflow guard is a
 * single-condition check driven in both directions across this suite.)
 */
RA8_INTERNAL static void internal_test_decomp_saturating_arithmetic(void)
{
  TEST_BEGIN("decomp: hostile 64-bit values saturate, never wrap");
  const ra8_decomp_limits_t lim = internal_tight();
  ra8_decomp_budget_t       b   = {};

  /* Output-counter wrap attempt: a near-UINT64_MAX delta saturates and is
   * reported as an output-cap breach, not wrapped back under the cap. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_decomp_budget_init(&b, &lim));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_decomp_budget_charge_output(&b, 8U, 8U));
  TEST_ASSERT_EQ(k_ra8_err_decomp_output_cap,
                 ra8_decomp_budget_charge_output(&b, 8U, UINT64_MAX - 2U));

  /* Ratio-product wrap attempt: in * ratio would exceed UINT64_MAX, so the
   * ratio test saturates (disabled) and only the absolute cap governs. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_decomp_budget_init(&b, &lim));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_decomp_budget_charge_output(&b, UINT64_MAX / 2U, (uint64_t)k_td_cap));

  /* Grace-sum wrap attempt: ratio 1 with in near UINT64_MAX makes
   * product + grace overflow; the bound saturates and the cap governs. */
  ra8_decomp_limits_t one = internal_tight();
  one.max_ratio           = 1U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_decomp_budget_init(&b, &one));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_decomp_budget_charge_output(&b, UINT64_MAX - 2U, (uint64_t)k_td_cap));
  TEST_END("decomp: hostile 64-bit values saturate, never wrap");
}

/**
 * @test internal_test_decomp_entry_iter_depth_axes
 * @brief Entry, iteration, and depth budgets fire exactly at their bounds.
 * @details Charges all three discrete budget axes through their exact limit and first breach.
 * @pre The tightened entry, iteration, and depth limits are nonzero.
 * @pre A single budget remains live for the ordered independent axis checks.
 * @post Each exact bound is accepted and the next charge returns its specific error.
 * @post Balanced and extra depth leaves finish at depth zero without wrapping.
 * @note Null charge and teardown calls are included in the same lifecycle vector.
 * @since Version 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions under test -- each axis is one single-condition
 * threshold check exercised in both directions.)
 */
RA8_INTERNAL static void internal_test_decomp_entry_iter_depth_axes(void)
{
  TEST_BEGIN("decomp: entry / iteration / depth budgets fire");
  const ra8_decomp_limits_t lim = internal_tight();
  ra8_decomp_budget_t       b   = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_decomp_budget_init(&b, &lim));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_decomp_budget_charge_entry(nullptr));
  for (uint32_t i = 0U; i < (uint32_t)k_td_entries; ++i) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_decomp_budget_charge_entry(&b));
  }
  TEST_ASSERT_EQ(k_ra8_err_decomp_entries, ra8_decomp_budget_charge_entry(&b));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_decomp_budget_charge_iter(nullptr));
  for (uint32_t i = 0U; i < (uint32_t)k_td_iters; ++i) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_decomp_budget_charge_iter(&b));
  }
  TEST_ASSERT_EQ(k_ra8_err_decomp_iterations, ra8_decomp_budget_charge_iter(&b));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_decomp_budget_enter(nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_decomp_budget_enter(&b));
  TEST_ASSERT_EQ(k_ra8_err_decomp_depth, ra8_decomp_budget_enter(&b));
  ra8_decomp_budget_leave(&b);
  TEST_ASSERT_EQ(0U, b.depth);
  ra8_decomp_budget_leave(&b); /* unbalanced leave: ignored, no wrap */
  TEST_ASSERT_EQ(0U, b.depth);
  ra8_decomp_budget_leave(nullptr); /* NULL-safe teardown */
  TEST_END("decomp: entry / iteration / depth budgets fire");
}

/**
 * @test internal_test_decomp_check_declared
 * @brief Lying header-declared sizes are rejected at O(1), honest ones pass.
 * @details Compares one honest size pair with absolute-cap and ratio-cap declarations.
 * @pre internal_tight returns the small deterministic policy used by expected values.
 * @pre Compressed and output sizes are supplied without any decoder state.
 * @post The honest pair returns k_ra8_ok.
 * @post Each lying declaration returns its precise bounded-decompression error.
 * @note A null policy is rejected before size arithmetic.
 * @since Version 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions under test -- the declared-size check reuses the
 * same two single-condition bounds as the running charge.)
 */
RA8_INTERNAL static void internal_test_decomp_check_declared(void)
{
  TEST_BEGIN("decomp: declared-size header check");
  const ra8_decomp_limits_t lim = internal_tight();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_decomp_check_declared(nullptr, 1U, 1U));
  /* Honest pair: 100 packed -> 300 unpacked (ratio 3 < 4). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_decomp_check_declared(&lim, 100U, 300U));
  /* Declared output over the absolute cap. */
  TEST_ASSERT_EQ(k_ra8_err_decomp_output_cap,
                 ra8_decomp_check_declared(&lim, 100U, (uint64_t)k_td_cap + 1U));
  /* Declared output over the ratio bound (needs in*4+10 < out <= cap). */
  TEST_ASSERT_EQ(k_ra8_err_decomp_ratio, ra8_decomp_check_declared(&lim, 10U, 60U));
  TEST_END("decomp: declared-size header check");
}

/**
 * @brief Read an exact positioned span from the in-memory ZIP fixture.
 * @details Models both a complete immutable source and a positive short-read fault.
 * @param[in] ctx Pointer to a td_zip_source_t fixture.
 * @param[in] offset Absolute source byte offset.
 * @param[out] buf Destination for the requested span.
 * @param[in] len Requested byte count.
 * @return Bytes transferred, or zero when the request is outside the fixture.
 * @retval 0 The request is invalid, out of range, empty, or forced fully short.
 * @retval 1..len The number of fixture bytes copied into @p buf.
 * @pre @p ctx and @p buf are non-NULL for nonzero requests.
 * @pre @p offset and @p len describe values representable by size_t.
 * @post Source bytes and fixture metadata remain unchanged.
 * @post A configured short-read transfers at most @p len - 1 bytes.
 * @note The production preflight intentionally defers short reads to the ZIP decoder.
 * @since Version 0.1.0
 */
RA8_INTERNAL static size_t internal_zip_read(void* ctx, uint64_t offset, void* buf, size_t len)
{
  const td_zip_source_t* const source = (const td_zip_source_t*)ctx;
  if ((source == nullptr) || (buf == nullptr) || (offset > (uint64_t)SIZE_MAX)) {
    return 0U;
  }
  const size_t at = (size_t)offset;
  if ((at > source->size) || (len > (source->size - at))) {
    return 0U;
  }
  const size_t transferred = (source->short_read && (len > 0U)) ? (len - 1U) : len;
  (void)memcpy(buf, &source->bytes[at], transferred);
  return transferred;
}

/**
 * @brief Encode a minimal classic-ZIP EOCD with a caller-selected entry count.
 * @details Produces the exact 22-byte record consumed by the bounded preflight.
 * @param[out] eocd Destination record.
 * @param[in] entries Total-entry field to encode.
 * @param[in] comment_bytes EOCD comment length to encode.
 * @pre @p eocd points to at least 22 writable bytes.
 * @pre The caller appends exactly @p comment_bytes bytes after the record.
 * @post All 22 record bytes are initialized.
 * @post The entry and comment fields use ZIP little-endian representation.
 * @note Other EOCD fields remain zero because the preflight does not consume them.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void
internal_build_eocd(uint8_t eocd[22], uint16_t entries, uint16_t comment_bytes)
{
  (void)memset(eocd, 0, 22U);
  eocd[0]  = 0x50U;
  eocd[1]  = 0x4BU;
  eocd[2]  = 0x05U;
  eocd[3]  = 0x06U;
  eocd[10] = (uint8_t)(entries & 0xFFU);
  eocd[11] = (uint8_t)(entries >> 8U);
  eocd[20] = (uint8_t)(comment_bytes & 0xFFU);
  eocd[21] = (uint8_t)(comment_bytes >> 8U);
}

/**
 * @test internal_test_zip_entry_preflight
 * @brief ZIP entry preflight distinguishes exact-cap, flood, and fallback paths.
 * @details Exercises a bare and commented EOCD, saturated ZIP64 count, malformed
 *          placement, short source reads, undersized input, and null callback.
 * @pre The shared default entry cap remains representable by the classic EOCD field.
 * @pre internal_zip_read obeys the positioned-reader contract.
 * @post Exact-cap archives remain eligible for authoritative ZIP decoding.
 * @post Over-cap records return k_ra8_err_decomp_entries without allocation.
 * @note Malformed or unreadable inputs deliberately fall through to the format decoder.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_test_zip_entry_preflight(void)
{
  TEST_BEGIN("decomp: ZIP entry preflight boundaries + fallback");
  uint8_t bytes[25] = {};
  internal_build_eocd(bytes, (uint16_t)k_ra8_decomp_def_max_entries, 0U);
  td_zip_source_t source = {.bytes = bytes, .size = 22U, .short_read = false};

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_decomp_zip_entry_preflight(nullptr, &source, 22U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_decomp_zip_entry_preflight(internal_zip_read, &source, 21U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_decomp_zip_entry_preflight(internal_zip_read, &source, 22U));

  internal_build_eocd(bytes, (uint16_t)(k_ra8_decomp_def_max_entries + 1U), 0U);
  TEST_ASSERT_EQ(k_ra8_err_decomp_entries,
                 ra8_decomp_zip_entry_preflight(internal_zip_read, &source, 22U));
  internal_build_eocd(bytes, UINT16_MAX, 3U);
  source.size = sizeof(bytes);
  TEST_ASSERT_EQ(k_ra8_err_decomp_entries,
                 ra8_decomp_zip_entry_preflight(internal_zip_read, &source, sizeof(bytes)));

  bytes[20] = 2U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_decomp_zip_entry_preflight(internal_zip_read, &source, sizeof(bytes)));
  bytes[20]         = 3U;
  source.short_read = true;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_decomp_zip_entry_preflight(internal_zip_read, &source, sizeof(bytes)));
  TEST_END("decomp: ZIP entry preflight boundaries + fallback");
}

/**
 * @brief Test entry point -- runs the decompression-limits suite in order.
 * @return 0 on success; unity_minimal.h exits non-zero on the first failure.
 */
int main(void)
{
  internal_test_decomp_default_policy();
  internal_test_decomp_zero_field_rejected();
  internal_test_decomp_output_and_ratio_axes();
  internal_test_decomp_saturating_arithmetic();
  internal_test_decomp_entry_iter_depth_axes();
  internal_test_decomp_check_declared();
  internal_test_zip_entry_preflight();
  return 0;
}
