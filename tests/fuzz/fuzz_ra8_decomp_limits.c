/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file fuzz_ra8_decomp_limits.c
 * @brief libFuzzer harness for the unified decompression-limits seam.
 *
 * @details
 * The policy engine is the single enforcement seam every archive decoder
 * charges, so its saturating 64-bit arithmetic must hold for ANY value an
 * attacker can steer into a header field. This harness carves the fuzz
 * input into raw little-endian words and drives them through the whole
 * seam: a fuzz-shaped policy bound via `ra8_decomp_budget_init` (invalid
 * policies must be refused, valid ones accepted), hostile declared sizes
 * through `ra8_decomp_check_declared`, and a charge sequence
 * (output / entry / iteration / depth) against the fuzz policy. UBSan
 * diagnoses any overflow the saturation guards were supposed to absorb.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "fuzz_entry.h"
#include "ra8_decomp_limits.h"
#include "ra8_err.h"

/**
 * @enum decomp_fuzz_field_t
 * @brief Which fuzz-input field supplies each decoder limit.
 */
typedef enum : uint8_t {
  k_fuzz_depth_field = 5U, /**< Which fuzz-input field supplies the recursion depth. */
} decomp_fuzz_field_t;

/**
 * @enum decomp_limits_fixture_t
 * @brief Buffer capacities and payload sizes.
 */
typedef enum : uint16_t {
  k_fuzz_input_cap =
    256U, /**< Largest input accepted; longer cases are dropped so a run stays bounded. */
} decomp_limits_fixture_t;

/** @brief Read one little-endian uint64 from the input (zero-padded). */
static uint64_t fz_u64(const uint8_t* data, size_t size, size_t idx)
{
  uint64_t     v   = 0U;
  const size_t off = idx * 8U;
  for (size_t i = 0U; i < 8U; ++i) {
    const size_t at = off + i;
    if (at >= size) {
      break;
    }
    v |= (uint64_t)data[at] << (8U * i);
  }
  return v;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
  if (size == 0U || size > k_fuzz_input_cap) {
    return 0;
  }
  ra8_decomp_limits_t lim = {};
  lim.max_output_bytes    = fz_u64(data, size, 0U);
  lim.max_ratio           = (uint32_t)fz_u64(data, size, 1U);
  lim.ratio_grace_bytes   = (uint32_t)fz_u64(data, size, 2U);
  lim.max_entries         = (uint32_t)fz_u64(data, size, 3U);
  lim.max_iterations      = (uint32_t)fz_u64(data, size, 4U);
  lim.max_depth           = (uint8_t)fz_u64(data, size, k_fuzz_depth_field);

  const uint64_t comp = fz_u64(data, size, 6U);
  const uint64_t outb = fz_u64(data, size, 7U);

  ra8_decomp_budget_t b = {};
  if (ra8_decomp_budget_init(&b, &lim) != k_ra8_ok) {
    /* Invalid (zero-field) policy refused; the default must still work. */
    if (ra8_decomp_budget_init(&b, nullptr) != k_ra8_ok) {
      __builtin_trap();
    }
  }
  (void)ra8_decomp_check_declared(&b.limits, comp, outb);
  (void)ra8_decomp_budget_charge_output(&b, comp, outb);
  (void)ra8_decomp_budget_charge_output(&b, comp, outb);
  (void)ra8_decomp_budget_charge_entry(&b);
  (void)ra8_decomp_budget_charge_iter(&b);
  if (ra8_decomp_budget_enter(&b) == k_ra8_ok) {
    ra8_decomp_budget_leave(&b);
  }
  ra8_decomp_budget_leave(&b); /* unbalanced leave must be a safe no-op */
  return 0;
}
