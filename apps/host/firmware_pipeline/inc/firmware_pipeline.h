// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

/**
 * @file firmware_pipeline.h
 * @brief Public C23 contract for the C-to-Zig-to-Rust firmware pipeline.
 * @details Zig validates requests and computes XOR; Rust computes counts and FNV-1a.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * @enum firmware_pipeline_status_t
 * @brief Stable result shared across both C ABI membranes.
 * @details Values have an exact signed 32-bit representation in C, Zig, and Rust.
 * @invariant Every returned value is declared below.
 * @code
 * firmware_pipeline_status_t status = k_firmware_pipeline_ok;
 * @endcode
 * @see firmware_pipeline_analyze
 */
typedef enum : int32_t {
  k_firmware_pipeline_ok               = 0, /**< Both language stages succeeded.        */
  k_firmware_pipeline_invalid_argument = 1, /**< A pointer or configuration is invalid. */
  k_firmware_pipeline_invalid_size     = 2, /**< Input exceeded 16 MiB.                 */
  k_firmware_pipeline_empty_image      = 3, /**< Rust rejected an empty image.          */
} firmware_pipeline_status_t;

/**
 * @enum firmware_pipeline_version_t
 * @brief Required Zig-stage ABI version.
 * @details The distinctive value proves Zig configuration validation executed.
 * @invariant The value fits exactly in 32 bits.
 * @code
 * uint32_t version = k_firmware_pipeline_abi_version;
 * @endcode
 * @see firmware_pipeline_config_t
 */
typedef enum : uint32_t {
  k_firmware_pipeline_abi_version = 0x5A495001U, /**< Version `ZIP` plus revision one. */
} firmware_pipeline_version_t;

/**
 * @struct firmware_pipeline_config_t
 * @brief Configuration validated exclusively by Zig.
 * @details Reserved space makes future compatible extension explicit.
 * @invariant `abi_version` equals `k_firmware_pipeline_abi_version`.
 * @code
 * firmware_pipeline_config_t config = {.abi_version = k_firmware_pipeline_abi_version};
 * @endcode
 * @see firmware_pipeline_analyze
 */
typedef struct {
  uint32_t abi_version; /**< Required Zig-stage ABI version. */
  uint32_t reserved0;   /**< Reserved; must be zero.         */
} firmware_pipeline_config_t;

/**
 * @struct firmware_pipeline_result_t
 * @brief Complete output proving both Zig and Rust stages executed.
 * @details The four 64-bit fields originate in Rust; XOR and marker originate in Zig.
 * @invariant `zig_stage_marker` equals `0x5a` after success.
 * @code
 * firmware_pipeline_result_t result = {};
 * @endcode
 * @see firmware_pipeline_analyze
 */
typedef struct {
  uint64_t byte_count;       /**< Rust-reported complete input length.          */
  uint64_t zero_count;       /**< Rust-reported zero byte count.                */
  uint64_t erased_count;     /**< Rust-reported `0xff` byte count.              */
  uint64_t fnv1a64;          /**< Rust-reported FNV-1a digest.                  */
  uint8_t  zig_xor8;         /**< Zig-computed XOR over every input byte.       */
  uint8_t  zig_stage_marker; /**< Fixed `0x5a` proof of Zig publication.        */
  uint8_t  reserved[6];      /**< Zeroed output padding reserved for extension. */
} firmware_pipeline_result_t;

static_assert(sizeof(firmware_pipeline_status_t) == 4U, "pipeline status ABI size");
static_assert(sizeof(firmware_pipeline_config_t) == 8U, "pipeline config ABI size");
static_assert(sizeof(firmware_pipeline_result_t) == 40U, "pipeline result ABI size");
static_assert(alignof(firmware_pipeline_result_t) == 8U, "pipeline result ABI alignment");
static_assert(offsetof(firmware_pipeline_result_t, zig_xor8) == 32U, "Zig XOR offset");

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Analyze one image through Zig and Rust.
 * @details Zig validates configuration and pointers, computes XOR, calls Rust through the private
 * C ABI, and publishes the complete combined result only after Rust succeeds.
 * @param[in] config Non-null Zig-stage configuration.
 * @param[in] data Non-null pointer to `size` readable image bytes.
 * @param[in] size Input length from zero through 16 MiB.
 * @param[in,out] out_result Writable destination changed only on complete success.
 * @return Stable status from Zig validation or Rust analysis.
 * @retval k_firmware_pipeline_ok Both stages succeeded.
 * @retval k_firmware_pipeline_invalid_argument A pointer, version, or reserved field is invalid.
 * @retval k_firmware_pipeline_invalid_size Zig or Rust rejected the input size.
 * @retval k_firmware_pipeline_empty_image Rust rejected an empty image.
 * @pre `config` and `out_result` point to readable/writable aligned storage.
 * @pre `data` names `size` readable bytes when `size` is nonzero.
 * @post Success initializes every result byte and proves both stages executed.
 * @post Failure leaves every result byte unchanged and retains no caller pointer.
 * @note Thread-safe, allocation-free, and initialization-order independent.
 * @since 0.1.0
 */
firmware_pipeline_status_t firmware_pipeline_analyze(const firmware_pipeline_config_t* config,
                                                     const uint8_t*                    data,
                                                     size_t                            size,
                                                     firmware_pipeline_result_t*       out_result);

#ifdef __cplusplus
}
#endif
