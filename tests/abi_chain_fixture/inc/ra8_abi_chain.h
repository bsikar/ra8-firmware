/**
 * @file ra8_abi_chain.h
 * @brief Public C23 contract for the C-to-Zig-to-Rust chain proof.
 * @details Defines the only interface exercised across the three-language fixture.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "ra8_rust_abi_fixture.h"

/**
 * @enum ra8_abi_chain_tag_t
 * @brief Required Zig-stage tag proving the upstream adapter was entered.
 * @details The fixed underlying width is part of the public C ABI.
 * @invariant The value fits exactly in `uint32_t`.
 * @code
 * uint32_t tag = k_ra8_abi_chain_tag;
 * @endcode
 * @see ra8_abi_chain_create
 */
typedef enum : uint32_t {
  k_ra8_abi_chain_tag = 0x5A494701U,
} ra8_abi_chain_tag_t;

/**
 * @struct ra8_abi_chain_config_t
 * @brief Inputs validated by Zig and then evaluated by Rust.
 * @details Zig validates `zig_tag`; Rust owns validation and scaling of `rust`.
 * @invariant `zig_tag` equals `k_ra8_abi_chain_tag`.
 * @invariant `reserved0` is zero.
 * @code
 * ra8_abi_chain_config_t config = {.zig_tag = k_ra8_abi_chain_tag};
 * @endcode
 * @see ra8_abi_chain_apply
 */
typedef struct {
  ra8_rust_abi_fixture_config_t rust;      /**< Downstream Rust input.  */
  uint32_t                      zig_tag;   /**< Zig validation tag.     */
  uint32_t                      reserved0; /**< Reserved; must be zero. */
} ra8_abi_chain_config_t;

static_assert(sizeof(ra8_abi_chain_config_t) == 16U, "chain config size");
static_assert(alignof(ra8_abi_chain_config_t) == 4U, "chain config alignment");
static_assert(offsetof(ra8_abi_chain_config_t, rust) == 0U, "chain Rust offset");
static_assert(offsetof(ra8_abi_chain_config_t, zig_tag) == 8U, "chain tag offset");
static_assert(offsetof(ra8_abi_chain_config_t, reserved0) == 12U, "chain reserved offset");

/**
 * @struct ra8_abi_chain_handle_t
 * @brief Opaque Rust allocation relayed through the Zig membrane.
 * @details C and Zig retain only the pointer; Rust owns representation and storage.
 * @invariant Only the Rust provider dereferences a live handle.
 * @invariant Successful destruction invalidates the handle.
 * @code
 * ra8_abi_chain_handle_t* handle = nullptr;
 * @endcode
 * @see ra8_abi_chain_create
 * @see ra8_abi_chain_destroy
 */
typedef struct ra8_abi_chain_handle ra8_abi_chain_handle_t;

/**
 * @brief Traverse Zig validation and Rust evaluation exactly once.
 * @details Rust scales the nested value; Zig adds one traceable stage marker.
 * @param[in] config Live chain configuration; must not be NULL.
 * @param[out] out_result Published only after both stages succeed.
 * @return Stable result from Zig validation or the Rust provider.
 * @retval k_ra8_ok Both stages completed and output was written.
 * @retval k_ra8_err_null_ptr A required pointer was NULL.
 * @retval k_ra8_err_invalid_arg Zig or Rust rejected an input field.
 * @retval k_ra8_err_invalid_size Rust scaling or Zig trace addition overflowed.
 * @pre Both pointers address their documented readable or writable storage.
 * @pre Calls are serialized in the fixture's task-only context.
 * @post Success writes the Rust result plus one Zig stage marker.
 * @post Failure leaves `out_result` unchanged.
 * @note Task-only and non-reentrant.
 * @see ra8_rust_abi_fixture_apply
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_abi_chain_apply(const ra8_abi_chain_config_t* config,
                                            uint32_t*                     out_result);

/**
 * @brief Validate in Zig and acquire one Rust-owned handle.
 * @param[in] zig_tag Required Zig-stage validation tag.
 * @param[out] out_handle Published only after Rust allocation succeeds.
 * @return Stable Zig validation or Rust allocation result.
 * @retval k_ra8_ok A Rust-owned handle was relayed to C.
 * @retval k_ra8_err_null_ptr `out_handle` was NULL.
 * @retval k_ra8_err_invalid_arg `zig_tag` was invalid.
 * @retval k_ra8_err_no_mem Rust rejected the bounded allocation.
 * @pre `out_handle` addresses writable pointer storage.
 * @pre Calls are serialized in the fixture's task-only context.
 * @post Success transfers exactly one Rust-owned handle.
 * @post Failure leaves `out_handle` unchanged.
 * @note Task-only and non-reentrant.
 * @see ra8_abi_chain_destroy
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_abi_chain_create(uint32_t zig_tag, ra8_abi_chain_handle_t** out_handle);

/**
 * @brief Release the Rust-owned handle through Zig.
 * @param[in,out] in_out_handle Handle slot relayed to Rust.
 * @return Stable Rust provider result.
 * @retval k_ra8_ok The handle was absent or released and cleared.
 * @retval k_ra8_err_null_ptr `in_out_handle` was NULL.
 * @retval k_ra8_err_invalid_arg The handle was not Rust-owned.
 * @pre A non-NULL handle came from `ra8_abi_chain_create` and remains live.
 * @pre `in_out_handle` addresses readable and writable pointer storage.
 * @post Success clears a live handle or preserves an already-NULL value.
 * @post Failure leaves the handle unchanged.
 * @note Task-only and non-reentrant.
 * @see ra8_abi_chain_create
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_abi_chain_destroy(ra8_abi_chain_handle_t** in_out_handle);

#ifdef __cplusplus
}
#endif
