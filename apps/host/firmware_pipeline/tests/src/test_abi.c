// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

/**
 * @file test_abi.c
 * @brief Always-active C-to-Zig-to-Rust runtime contract proof.
 * @details Exercises success and failures originating in Zig and Rust.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h>
#include <string.h>

#include "firmware_pipeline.h"

/**
 * @brief Abort when an always-active test requirement is false.
 * @param[in] condition Requirement result.
 * @pre None.
 * @post Returns only when the condition is true.
 * @note Calls abort on failure in every build mode.
 * @since 0.1.0
 */
static void require(bool condition)
{
  if (!condition) {
    abort();
  }
}

/**
 * @brief Exercise both ABI membranes and unchanged-output failures.
 * @return Zero after every requirement passes.
 * @retval 0 All boundary transitions preserve their contracts.
 * @pre Hosted abort semantics are available.
 * @post No ABI provider retains test storage.
 * @note Requirements remain active under `NDEBUG`.
 * @since 0.1.0
 */
int main(void)
{
  const uint8_t              input[] = {0U, 0xffU, 7U};
  firmware_pipeline_config_t config  = {
     .abi_version = k_firmware_pipeline_abi_version,
     .reserved0   = 0,
  };
  firmware_pipeline_result_t result;
  (void)memset(&result, 0xa5, sizeof(result));
  require(firmware_pipeline_analyze(&config, input, sizeof(input), &result) ==
          k_firmware_pipeline_ok);
  require(result.byte_count == 3U);
  require(result.zero_count == 1U);
  require(result.erased_count == 1U);
  require(result.zig_xor8 == 0xf8U);
  require(result.zig_stage_marker == 0x5aU);

  const firmware_pipeline_result_t saved = result;
  config.abi_version                     = 0U;
  require(firmware_pipeline_analyze(&config, input, sizeof(input), &result) ==
          k_firmware_pipeline_invalid_argument);
  require(memcmp(&saved, &result, sizeof(result)) == 0);
  config.abi_version = k_firmware_pipeline_abi_version;

  config.reserved0 = 1U;
  require(firmware_pipeline_analyze(&config, input, sizeof(input), &result) ==
          k_firmware_pipeline_invalid_argument);
  require(memcmp(&saved, &result, sizeof(result)) == 0);
  config.reserved0 = 0U;

  require(firmware_pipeline_analyze(nullptr, input, sizeof(input), &result) ==
          k_firmware_pipeline_invalid_argument);
  require(memcmp(&saved, &result, sizeof(result)) == 0);
  require(firmware_pipeline_analyze(&config, nullptr, sizeof(input), &result) ==
          k_firmware_pipeline_invalid_argument);
  require(memcmp(&saved, &result, sizeof(result)) == 0);
  require(firmware_pipeline_analyze(&config, input, sizeof(input), nullptr) ==
          k_firmware_pipeline_invalid_argument);
  require(memcmp(&saved, &result, sizeof(result)) == 0);

  require(firmware_pipeline_analyze(&config, input, (16U * 1024U * 1024U) + 1U, &result) ==
          k_firmware_pipeline_invalid_size);
  require(memcmp(&saved, &result, sizeof(result)) == 0);
  require(firmware_pipeline_analyze(&config, input, 0U, &result) ==
          k_firmware_pipeline_empty_image);
  require(memcmp(&saved, &result, sizeof(result)) == 0);
  return 0;
}
