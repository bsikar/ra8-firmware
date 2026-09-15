// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

/**
 * @file main.c
 * @brief C host shell for the Zig-to-Rust firmware pipeline.
 * @details Owns bounded input, diagnostics, output, and cleanup around both ABI stages.
 */

#include <stdio.h>

#include "firmware_pipeline.h"
#include "firmware_pipeline_cli.h"
#include "firmware_pipeline_io.h"

/**
 * @brief Emit a stable diagnostic and return failure.
 * @details Prefixes a static message on standard error.
 * @param[in] message Non-null null-terminated message.
 * @return Process status two.
 * @retval 2 Always.
 * @pre `message` is readable and terminated.
 * @pre Standard error is initialized.
 * @post A best-effort diagnostic was attempted.
 * @post No caller storage changed.
 * @note Thread-compatible through the C runtime stream lock.
 * @since 0.1.0
 */
static int fail(const char* message)
{
  (void)fprintf(stderr, "firmware_pipeline: %s\n", message);
  return 2;
}

/**
 * @brief Run the C-to-Zig-to-Rust pipeline and print its result.
 * @details Supplies a non-null placeholder for empty input so Rust owns the empty-image error.
 * @param[in] image Valid C-owned image representation.
 * @return Process-compatible status.
 * @retval 0 Both language stages and output succeeded.
 * @retval 2 Either ABI stage or output failed.
 * @pre `image` is readable and its pointer/length pair is valid.
 * @pre Both static provider libraries are initialized by the runtime.
 * @post Neither stage retains the input pointer.
 * @post No cross-language resource remains owned.
 * @note Thread-safe; both providers are allocation-free.
 * @since 0.1.0
 */
static int run_pipeline(const firmware_pipeline_image_t* image)
{
  const firmware_pipeline_config_t config = {
    .abi_version = k_firmware_pipeline_abi_version,
    .reserved0   = 0,
  };
  const uint8_t                    empty_placeholder = 0U;
  const uint8_t*                   data   = image->size == 0U ? &empty_placeholder : image->bytes;
  firmware_pipeline_result_t       result = {};
  const firmware_pipeline_status_t status =
    firmware_pipeline_analyze(&config, data, image->size, &result);
  if (status != k_firmware_pipeline_ok) {
    return fail(status == k_firmware_pipeline_empty_image ? "Rust rejected empty image"
                                                          : "language pipeline failed");
  }
  if (printf("bytes=%llu\nzero=%llu\nerased=%llu\nfnv1a64=%016llx\nzig_xor8=%02x\nzig_stage=%02x\n",
             (unsigned long long)result.byte_count,
             (unsigned long long)result.zero_count,
             (unsigned long long)result.erased_count,
             (unsigned long long)result.fnv1a64,
             (unsigned int)result.zig_xor8,
             (unsigned int)result.zig_stage_marker) < 0) {
    return fail("cannot write output");
  }
  return 0;
}

/**
 * @brief Run the bounded three-language host application.
 * @details Validates arguments, owns input cleanup, and invokes both language stages.
 * @param[in] argc Argument count.
 * @param[in] argv Hosted argument vector.
 * @return Process status.
 * @retval 0 Complete report emitted.
 * @retval 2 C, Zig, Rust, or output failure.
 * @pre Hosted entry-point arguments are valid.
 * @pre Hosted file and stream services are initialized.
 * @post Every C allocation and file is released exactly once.
 * @post No ABI stage retains caller storage.
 * @note Not thread-safe; this is the process entry point.
 * @since 0.1.0
 */
int main(int argc, char** argv)
{
  const char* path = nullptr;
  if (firmware_pipeline_parse_args(argc, argv, &path) != k_firmware_pipeline_cli_ok) {
    (void)fprintf(stderr, "usage: firmware_pipeline <firmware-image>\n");
    return 2;
  }
  const firmware_pipeline_io_ops_t* io    = firmware_pipeline_host_io();
  firmware_pipeline_image_t         image = {};
  if (firmware_pipeline_read_image(io, path, &image) != k_firmware_pipeline_io_ok) {
    return fail("cannot read bounded input");
  }
  const int pipeline_status = run_pipeline(&image);
  firmware_pipeline_release_image(io, &image);
  return pipeline_status;
}
