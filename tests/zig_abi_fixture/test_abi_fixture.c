/**
 * @file test_abi_fixture.c
 * @brief C23 consumer acceptance test for the Zig ABI fixture.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_abi_fixture.h"

/** @enum abi_fixture_test_result_t @brief Process result values for this fixture. */
typedef enum : int {
  k_abi_fixture_test_success = 0,
  k_abi_fixture_test_failure = 1,
} abi_fixture_test_result_t;

static const uint32_t k_sentinel_u32      = 0xA5A5A5A5U;
static const uint8_t  k_sentinel_bytes[8] = {
  0xA5U,
  0xA5U,
  0xA5U,
  0xA5U,
  0xA5U,
  0xA5U,
  0xA5U,
  0xA5U,
};

/**
 * @brief Exercise the stateless scalar boundary.
 *
 * @return Test status.
 * @retval k_abi_fixture_test_success All scalar vectors passed.
 * @retval k_abi_fixture_test_failure A scalar vector failed.
 * @par MC/DC:
 * Each two-condition result/value decision uses one all-false success vector
 * and individual failure vectors that independently change the result and the
 * value comparison.
 */
static abi_fixture_test_result_t test_apply(void)
{
  ra8_abi_fixture_config_t config = {
    .value     = 7U,
    .factor    = 3U,
    .enabled   = 1U,
    .reserved0 = 0U,
  };
  uint32_t result = k_sentinel_u32;

  if (ra8_abi_fixture_apply(&config, &result) != k_ra8_ok || result != 21U) {
    return k_abi_fixture_test_failure;
  }
  result = k_sentinel_u32;
  if (ra8_abi_fixture_apply(nullptr, &result) != k_ra8_err_null_ptr || result != k_sentinel_u32) {
    return k_abi_fixture_test_failure;
  }
  config.enabled = 2U;
  if (ra8_abi_fixture_apply(&config, &result) != k_ra8_err_invalid_arg ||
      result != k_sentinel_u32) {
    return k_abi_fixture_test_failure;
  }
  config.enabled   = 1U;
  config.reserved0 = 1U;
  if (ra8_abi_fixture_apply(&config, &result) != k_ra8_err_invalid_arg ||
      result != k_sentinel_u32) {
    return k_abi_fixture_test_failure;
  }
  config.reserved0 = 0U;
  config.value     = UINT32_MAX;
  config.factor    = 2U;
  if (ra8_abi_fixture_apply(&config, &result) != k_ra8_err_invalid_size ||
      result != k_sentinel_u32) {
    return k_abi_fixture_test_failure;
  }
  if (ra8_abi_fixture_apply(&config, nullptr) != k_ra8_err_null_ptr) {
    return k_abi_fixture_test_failure;
  }
  return k_abi_fixture_test_success;
}

/**
 * @brief Exercise opaque-handle allocation and teardown symmetry.
 *
 * @param[out] out_handle Live handle retained for later test groups.
 * @return Test status.
 * @retval k_abi_fixture_test_success All lifecycle vectors passed.
 * @retval k_abi_fixture_test_failure A lifecycle vector failed.
 * @par MC/DC:
 * The create result/handle decision is exercised with success, non-success,
 * and unchanged-output vectors so each condition independently changes it.
 */
static abi_fixture_test_result_t test_create(ra8_abi_fixture_t** out_handle)
{
  ra8_abi_fixture_t* second  = (ra8_abi_fixture_t*)(uintptr_t)0x1U;
  ra8_abi_fixture_t* invalid = (ra8_abi_fixture_t*)(uintptr_t)0x2U;

  if (ra8_abi_fixture_create(nullptr) != k_ra8_err_null_ptr) {
    return k_abi_fixture_test_failure;
  }
  ra8_abi_fixture_test_fail_next_allocation();
  if (ra8_abi_fixture_create(&second) != k_ra8_err_no_mem ||
      second != (ra8_abi_fixture_t*)(uintptr_t)0x1U) {
    return k_abi_fixture_test_failure;
  }
  if (ra8_abi_fixture_create(out_handle) != k_ra8_ok || *out_handle == nullptr) {
    return k_abi_fixture_test_failure;
  }
  if (ra8_abi_fixture_create(&second) != k_ra8_err_no_mem ||
      second != (ra8_abi_fixture_t*)(uintptr_t)0x1U) {
    return k_abi_fixture_test_failure;
  }
  if (ra8_abi_fixture_destroy(&invalid) != k_ra8_err_invalid_arg ||
      invalid != (ra8_abi_fixture_t*)(uintptr_t)0x2U) {
    return k_abi_fixture_test_failure;
  }
  return k_abi_fixture_test_success;
}

/**
 * @brief Exercise borrowed spans and caller-owned output invariants.
 *
 * @param[in] handle Live fixture handle.
 * @return Test status.
 * @retval k_abi_fixture_test_success All span vectors passed.
 * @retval k_abi_fixture_test_failure A span vector failed.
 * @par MC/DC:
 * Success, null-pointer, oversize, insufficient-capacity, and invalid-handle
 * vectors independently toggle every adapter validation condition. Sentinels
 * show that each failing condition prevents both output publications.
 */
static abi_fixture_test_result_t test_copy(ra8_abi_fixture_t* handle)
{
  static const uint8_t input[] = {1U, 2U, 3U, 4U};
  uint8_t              output[sizeof(k_sentinel_bytes)];
  uint32_t             out_len = k_sentinel_u32;

  memcpy(output, k_sentinel_bytes, sizeof(output));
  if (ra8_abi_fixture_copy(handle, input, sizeof(input), output, sizeof(input), &out_len) !=
        k_ra8_ok ||
      out_len != sizeof(input) || memcmp(input, output, sizeof(input)) != 0) {
    return k_abi_fixture_test_failure;
  }
  memcpy(output, k_sentinel_bytes, sizeof(output));
  out_len = k_sentinel_u32;
  if (ra8_abi_fixture_copy(handle, input, sizeof(input), output, sizeof(output), &out_len) !=
        k_ra8_ok ||
      out_len != sizeof(input) || memcmp(input, output, sizeof(input)) != 0 ||
      memcmp(&output[sizeof(input)],
             &k_sentinel_bytes[sizeof(input)],
             sizeof(output) - sizeof(input)) != 0) {
    return k_abi_fixture_test_failure;
  }
  memcpy(output, k_sentinel_bytes, sizeof(output));
  out_len = k_sentinel_u32;
  if (ra8_abi_fixture_copy(handle, nullptr, 0U, output, sizeof(output), &out_len) != k_ra8_ok ||
      out_len != 0U || memcmp(output, k_sentinel_bytes, sizeof(output)) != 0) {
    return k_abi_fixture_test_failure;
  }
  memcpy(output, k_sentinel_bytes, sizeof(output));
  out_len = k_sentinel_u32;
  if (ra8_abi_fixture_copy(handle, input, 0U, output, sizeof(output), &out_len) != k_ra8_ok ||
      out_len != 0U || memcmp(output, k_sentinel_bytes, sizeof(output)) != 0) {
    return k_abi_fixture_test_failure;
  }
  memcpy(output, k_sentinel_bytes, sizeof(output));
  out_len = k_sentinel_u32;
  if (ra8_abi_fixture_copy(handle, nullptr, 1U, output, sizeof(output), &out_len) !=
        k_ra8_err_null_ptr ||
      out_len != k_sentinel_u32 || memcmp(output, k_sentinel_bytes, sizeof(output)) != 0) {
    return k_abi_fixture_test_failure;
  }
  memcpy(output, k_sentinel_bytes, sizeof(output));
  out_len = k_sentinel_u32;
  if (ra8_abi_fixture_copy(handle, input, sizeof(input), output, sizeof(input) - 1U, &out_len) !=
        k_ra8_err_invalid_size ||
      out_len != k_sentinel_u32 || memcmp(output, k_sentinel_bytes, sizeof(output)) != 0) {
    return k_abi_fixture_test_failure;
  }
  memcpy(output, k_sentinel_bytes, sizeof(output));
  if (ra8_abi_fixture_copy(handle,
                           input,
                           k_ra8_abi_fixture_max_bytes + 1U,
                           output,
                           sizeof(output),
                           &out_len) != k_ra8_err_invalid_size ||
      out_len != k_sentinel_u32 || memcmp(output, k_sentinel_bytes, sizeof(output)) != 0) {
    return k_abi_fixture_test_failure;
  }
  memcpy(output, k_sentinel_bytes, sizeof(output));
  if (ra8_abi_fixture_copy((ra8_abi_fixture_t*)(uintptr_t)0x1U,
                           input,
                           sizeof(input),
                           output,
                           sizeof(output),
                           &out_len) != k_ra8_err_invalid_arg ||
      out_len != k_sentinel_u32 || memcmp(output, k_sentinel_bytes, sizeof(output)) != 0) {
    return k_abi_fixture_test_failure;
  }
  memcpy(output, k_sentinel_bytes, sizeof(output));
  if (ra8_abi_fixture_copy(nullptr, input, sizeof(input), output, sizeof(output), &out_len) !=
        k_ra8_err_null_ptr ||
      out_len != k_sentinel_u32 || memcmp(output, k_sentinel_bytes, sizeof(output)) != 0) {
    return k_abi_fixture_test_failure;
  }
  memcpy(output, k_sentinel_bytes, sizeof(output));
  if (ra8_abi_fixture_copy(handle, input, sizeof(input), nullptr, sizeof(output), &out_len) !=
        k_ra8_err_null_ptr ||
      out_len != k_sentinel_u32 || memcmp(output, k_sentinel_bytes, sizeof(output)) != 0) {
    return k_abi_fixture_test_failure;
  }
  memcpy(output, k_sentinel_bytes, sizeof(output));
  if (ra8_abi_fixture_copy(handle, input, sizeof(input), output, sizeof(output), nullptr) !=
        k_ra8_err_null_ptr ||
      out_len != k_sentinel_u32 || memcmp(output, k_sentinel_bytes, sizeof(output)) != 0) {
    return k_abi_fixture_test_failure;
  }
  return k_abi_fixture_test_success;
}

/**
 * @brief Exercise library-owned output publication and release.
 *
 * @param[in] handle Live fixture handle.
 * @return Test status.
 * @retval k_abi_fixture_test_success All ownership vectors passed.
 * @retval k_abi_fixture_test_failure An ownership vector failed.
 * @par MC/DC:
 * Live/free slot, valid/invalid pointer, and present/absent output conditions
 * are varied independently. Every failure vector checks both output sentinels.
 */
static abi_fixture_test_result_t test_owned_bytes(ra8_abi_fixture_t* handle)
{
  static const uint8_t input[]    = {9U, 8U, 7U};
  uint8_t*             bytes      = (uint8_t*)(uintptr_t)0x1U;
  uint8_t*             second     = (uint8_t*)(uintptr_t)0x2U;
  uint32_t             out_len    = k_sentinel_u32;
  uint32_t             second_len = k_sentinel_u32;

  ra8_abi_fixture_test_fail_next_allocation();
  if (ra8_abi_fixture_bytes_create(handle, input, sizeof(input), &bytes, &out_len) !=
        k_ra8_err_no_mem ||
      bytes != (uint8_t*)(uintptr_t)0x1U || out_len != k_sentinel_u32) {
    return k_abi_fixture_test_failure;
  }
  if (ra8_abi_fixture_bytes_create(handle, input, sizeof(input), &bytes, &out_len) != k_ra8_ok ||
      bytes == nullptr || out_len != sizeof(input) || memcmp(bytes, input, sizeof(input)) != 0) {
    return k_abi_fixture_test_failure;
  }
  if (ra8_abi_fixture_bytes_create(handle, input, sizeof(input), &second, &second_len) !=
        k_ra8_err_no_mem ||
      second != (uint8_t*)(uintptr_t)0x2U || second_len != k_sentinel_u32) {
    return k_abi_fixture_test_failure;
  }
  if (ra8_abi_fixture_destroy(&handle) != k_ra8_err_busy || handle == nullptr) {
    return k_abi_fixture_test_failure;
  }
  second = (uint8_t*)(uintptr_t)0x2U;
  if (ra8_abi_fixture_bytes_release(&second) != k_ra8_err_invalid_arg ||
      second != (uint8_t*)(uintptr_t)0x2U) {
    return k_abi_fixture_test_failure;
  }
  if (ra8_abi_fixture_bytes_release(&bytes) != k_ra8_ok || bytes != nullptr ||
      ra8_abi_fixture_bytes_release(&bytes) != k_ra8_ok) {
    return k_abi_fixture_test_failure;
  }
  second = (uint8_t*)(uintptr_t)0x2U;
  if (ra8_abi_fixture_bytes_release(&second) != k_ra8_err_invalid_state ||
      second != (uint8_t*)(uintptr_t)0x2U) {
    return k_abi_fixture_test_failure;
  }
  if (ra8_abi_fixture_bytes_release(nullptr) != k_ra8_err_null_ptr) {
    return k_abi_fixture_test_failure;
  }
  bytes   = (uint8_t*)(uintptr_t)0x1U;
  out_len = k_sentinel_u32;
  if (ra8_abi_fixture_bytes_create(handle, nullptr, 1U, &bytes, &out_len) != k_ra8_err_null_ptr ||
      bytes != (uint8_t*)(uintptr_t)0x1U || out_len != k_sentinel_u32) {
    return k_abi_fixture_test_failure;
  }
  if (ra8_abi_fixture_bytes_create(handle,
                                   input,
                                   k_ra8_abi_fixture_max_bytes + 1U,
                                   &bytes,
                                   &out_len) != k_ra8_err_invalid_size ||
      bytes != (uint8_t*)(uintptr_t)0x1U || out_len != k_sentinel_u32) {
    return k_abi_fixture_test_failure;
  }
  if (ra8_abi_fixture_bytes_create((ra8_abi_fixture_t*)(uintptr_t)0x1U,
                                   input,
                                   sizeof(input),
                                   &bytes,
                                   &out_len) != k_ra8_err_invalid_arg ||
      bytes != (uint8_t*)(uintptr_t)0x1U || out_len != k_sentinel_u32) {
    return k_abi_fixture_test_failure;
  }
  if (ra8_abi_fixture_bytes_create(handle, input, sizeof(input), nullptr, &out_len) !=
        k_ra8_err_null_ptr ||
      out_len != k_sentinel_u32) {
    return k_abi_fixture_test_failure;
  }
  if (ra8_abi_fixture_bytes_create(handle, input, sizeof(input), &bytes, nullptr) !=
        k_ra8_err_null_ptr ||
      bytes != (uint8_t*)(uintptr_t)0x1U) {
    return k_abi_fixture_test_failure;
  }
  if (ra8_abi_fixture_bytes_create(handle, nullptr, 0U, &bytes, &out_len) != k_ra8_ok ||
      out_len != 0U || ra8_abi_fixture_bytes_release(&bytes) != k_ra8_ok) {
    return k_abi_fixture_test_failure;
  }
  if (ra8_abi_fixture_bytes_create(handle, input, 0U, &bytes, &out_len) != k_ra8_ok ||
      out_len != 0U || ra8_abi_fixture_bytes_release(&bytes) != k_ra8_ok) {
    return k_abi_fixture_test_failure;
  }
  if (ra8_abi_fixture_bytes_create(handle, input, sizeof(input), &bytes, &out_len) != k_ra8_ok ||
      ra8_abi_fixture_bytes_release(&bytes) != k_ra8_ok) {
    return k_abi_fixture_test_failure;
  }
  return k_abi_fixture_test_success;
}

/**
 * @brief Exercise the C header, linked Zig export, and failure-output contract.
 *
 * @details The test uses only the fixture's public header and linked library.
 * It verifies success plus null, validation, and overflow mappings.
 *
 * @return Process status for CTest.
 * @retval k_abi_fixture_test_success Every ABI assertion held.
 * @retval k_abi_fixture_test_failure One ABI assertion failed.
 * @pre The Zig fixture library was linked by CMake.
 * @pre The fixture header declarations match the library exports.
 * @post A nonzero result identifies a C ABI contract regression.
 * @post No private Zig declaration is referenced.
 * @note Host-only task context; no ISR behavior is exercised.
 * @since Version 0.1.0
 */
int main(void)
{
  ra8_abi_fixture_t* handle = nullptr;

  if (test_apply() != k_abi_fixture_test_success) {
    return k_abi_fixture_test_failure;
  }
  if (test_create(&handle) != k_abi_fixture_test_success) {
    return k_abi_fixture_test_failure;
  }
  if (test_copy(handle) != k_abi_fixture_test_success) {
    return k_abi_fixture_test_failure;
  }
  if (test_owned_bytes(handle) != k_abi_fixture_test_success) {
    return k_abi_fixture_test_failure;
  }
  if (ra8_abi_fixture_destroy(&handle) != k_ra8_ok || handle != nullptr ||
      ra8_abi_fixture_destroy(&handle) != k_ra8_ok ||
      ra8_abi_fixture_destroy(nullptr) != k_ra8_err_null_ptr) {
    return k_abi_fixture_test_failure;
  }
  return test_create(&handle) == k_abi_fixture_test_success &&
             ra8_abi_fixture_destroy(&handle) == k_ra8_ok
           ? k_abi_fixture_test_success
           : k_abi_fixture_test_failure;
}
