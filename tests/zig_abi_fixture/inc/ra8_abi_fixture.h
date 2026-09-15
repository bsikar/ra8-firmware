/**
 * @file ra8_abi_fixture.h
 * @brief Public C23 contract used to prove a Zig-built library boundary.
 *
 * @details This host-only fixture is the reusable ABI-harness reference. It
 * exposes fixed-width values only; the implementation remains private Zig.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"

static_assert(sizeof(ra8_err_t) == 2U, "ABI fixture error width");
static_assert(k_ra8_ok == 0U, "ABI fixture success value");
static_assert(k_ra8_err_invalid_arg == 0x103U, "ABI fixture invalid arg value");
static_assert(k_ra8_err_invalid_size == 0x105U, "ABI fixture invalid size value");
static_assert(k_ra8_err_no_mem == 0x102U, "ABI fixture no-memory value");
static_assert(k_ra8_err_invalid_state == 0x104U, "ABI fixture invalid-state value");
static_assert(k_ra8_err_busy == 0x109U, "ABI fixture busy value");
static_assert(k_ra8_err_null_ptr == 0x504U, "ABI fixture null pointer value");

/**
 * @struct ra8_abi_fixture_t
 * @brief Opaque state owned by the ABI fixture.
 * @details Its representation, storage, and alignment remain private to Zig.
 * @invariant Only the fixture creates or dereferences a live handle.
 * @code
 * ra8_abi_fixture_t* handle = nullptr;
 * @endcode
 * @see ra8_abi_fixture_create
 * @see ra8_abi_fixture_destroy
 */
typedef struct ra8_abi_fixture ra8_abi_fixture_t;

/**
 * @enum ra8_abi_fixture_limit_t
 * @brief Fixed public limits used by the contract vectors.
 * @details Limits use an explicit fixed underlying type in the public ABI.
 * @invariant Values fit in `uint32_t` on host and target.
 * @code
 * uint8_t bytes[k_ra8_abi_fixture_max_bytes];
 * @endcode
 * @see ra8_abi_fixture_copy
 */
typedef enum : uint32_t {
  k_ra8_abi_fixture_max_bytes = 32U, /**< Maximum borrowed or owned byte span. */
} ra8_abi_fixture_limit_t;

static_assert(sizeof(ra8_abi_fixture_limit_t) == 4U, "ABI fixture limit width");
static_assert(k_ra8_abi_fixture_max_bytes == 32U, "ABI fixture maximum byte value");

/**
 * @struct ra8_abi_fixture_config_t
 * @brief Fixed-layout input accepted by `ra8_abi_fixture_apply()`.
 *
 * @details The explicit reserved byte makes the host and target layout
 * observable. `enabled` is a `uint8_t` boolean and accepts only zero or one.
 *
 * @invariant `enabled` is zero or one.
 * @invariant `reserved0` is zero.
 */
typedef struct {
  uint32_t value;     /**< Value to scale. */
  uint16_t factor;    /**< Unsigned scale factor. */
  uint8_t  enabled;   /**< Canonical boolean: zero disables scaling, one enables it. */
  uint8_t  reserved0; /**< Reserved byte; caller supplies zero. */
} ra8_abi_fixture_config_t;

static_assert(sizeof(ra8_abi_fixture_config_t) == 8U, "ABI fixture structure size");
static_assert(alignof(ra8_abi_fixture_config_t) == 4U, "ABI fixture structure alignment");
static_assert(offsetof(ra8_abi_fixture_config_t, value) == 0U, "ABI fixture value offset");
static_assert(offsetof(ra8_abi_fixture_config_t, factor) == 4U, "ABI fixture factor offset");
static_assert(offsetof(ra8_abi_fixture_config_t, enabled) == 6U, "ABI fixture enabled offset");
static_assert(offsetof(ra8_abi_fixture_config_t, reserved0) == 7U, "ABI fixture reserved offset");

/**
 * @brief Validate and scale one fixed-layout ABI value.
 *
 * @details The output pointer is validated before the input pointer. On every
 * failure the function leaves `out_result` unchanged; recoverable native Zig
 * errors map to the existing `ra8_err_t` vocabulary.
 *
 * @param[in] config Input configuration; must not be NULL.
 * @param[out] out_result Scaled result written only on success; must not be NULL.
 *
 * @return `ra8_err_t` result.
 * @retval k_ra8_ok The result was written.
 * @retval k_ra8_err_null_ptr `out_result` or `config` was NULL.
 * @retval k_ra8_err_invalid_arg `enabled` or `reserved0` was invalid.
 * @retval k_ra8_err_invalid_size Scaling overflowed `uint32_t`.
 *
 * @pre `config` addresses a live `ra8_abi_fixture_config_t`.
 * @pre `out_result` addresses writable `uint32_t` storage.
 * @post Success writes exactly one `uint32_t` to `out_result`.
 * @post Failure leaves `out_result` unchanged.
 * @note Task-only and non-reentrant; this host fixture has no ISR path.
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_abi_fixture_apply(const ra8_abi_fixture_config_t* config,
                                              uint32_t*                       out_result);

/**
 * @brief Acquire the fixture's single bounded state object.
 *
 * @details The fixture models a fixed-capacity allocator with one opaque
 * handle slot. Exhaustion is recoverable and never publishes a partial handle.
 *
 * @param[out] out_handle Handle published only on success; must not be NULL.
 * @return `ra8_err_t` result.
 * @retval k_ra8_ok A handle was published.
 * @retval k_ra8_err_null_ptr `out_handle` was NULL.
 * @retval k_ra8_err_no_mem The bounded handle pool is already occupied.
 * @pre `out_handle` addresses writable pointer storage when non-NULL.
 * @pre The caller serializes access to the task-only handle pool.
 * @post Failure leaves `out_handle` unchanged.
 * @post Success transfers one handle reference to the caller.
 * @note Task-only and non-reentrant.
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_abi_fixture_create(ra8_abi_fixture_t** out_handle);

/**
 * @brief Fail the next bounded allocation point in this test fixture.
 *
 * @details This fixture-only control is part of the documented test ABI, not a
 * production-library pattern. It deterministically fails exactly one later
 * handle or owned-byte allocation and then clears itself.
 *
 * @pre Calls are serialized in the fixture's task-only context.
 * @pre The caller intends the next create operation to exercise allocation failure.
 * @post Exactly the next allocation point returns `k_ra8_err_no_mem`.
 * @note It exists so C acceptance tests arrange failure without reaching a
 * private Zig declaration.
 * @since Version 0.1.0
 */
void ra8_abi_fixture_test_fail_next_allocation(void);

/**
 * @brief Release a fixture state object and clear the caller's handle.
 *
 * @details Outstanding library-owned bytes or an actively executing callback
 * prevent destruction. Successful destruction releases any idle callback
 * registration, invalidates the handle reference, and restores pool capacity.
 *
 * @param[in,out] in_out_handle Handle slot to release; must not be NULL. A
 *                              NULL handle value is an idempotent success.
 * @return `ra8_err_t` result.
 * @retval k_ra8_ok The handle was absent or was released and cleared.
 * @retval k_ra8_err_null_ptr `in_out_handle` was NULL.
 * @retval k_ra8_err_invalid_arg The handle was not created by this fixture.
 * @retval k_ra8_err_busy Library-owned bytes remain outstanding or a callback is active.
 * @pre A non-NULL handle value came from `ra8_abi_fixture_create()` and has
 * not already been destroyed.
 * @pre `in_out_handle` addresses writable pointer storage when non-NULL.
 * @post Failure leaves `in_out_handle` unchanged.
 * @post Success clears a live handle or preserves an already-NULL value.
 * @note Task-only and non-reentrant.
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_abi_fixture_destroy(ra8_abi_fixture_t** in_out_handle);

/**
 * @brief Copy a borrowed byte span into caller-owned storage.
 *
 * @details All arguments are validated before either output changes. Empty
 * input may use a NULL input pointer, but the output pointer remains required.
 *
 * @param[in] handle Live fixture handle; must not be NULL.
 * @param[in] input Borrowed input. NULL is valid only when `input_len` is zero.
 * @param[in] input_len Number of input bytes.
 * @param[out] output Caller-owned destination; must not be NULL.
 * @param[in] capacity Writable bytes at `output`.
 * @param[out] out_len Bytes written on success; must not be NULL.
 * @return `ra8_err_t` result.
 * @retval k_ra8_ok The input was copied and `out_len` was written.
 * @retval k_ra8_err_null_ptr A required pointer was NULL.
 * @retval k_ra8_err_invalid_arg `handle` was not created by this fixture.
 * @retval k_ra8_err_invalid_size A length exceeds the fixture maximum or capacity.
 * @pre A non-NULL `handle` remains live for the duration of the call.
 * @pre Non-NULL span pointers address at least their declared byte counts.
 * @post Failure leaves `output` and `out_len` unchanged.
 * @post Success copies exactly `input_len` bytes and publishes that count.
 * @note Task-only and non-reentrant; the input is borrowed for this call only.
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_abi_fixture_copy(ra8_abi_fixture_t* handle,
                                             const uint8_t*     input,
                                             uint32_t           input_len,
                                             uint8_t*           output,
                                             uint32_t           capacity,
                                             uint32_t*          out_len);

/**
 * @brief Publish a library-owned copy of a borrowed byte span.
 *
 * @details The returned pointer remains owned by the fixture and valid until
 * its named release call. Only one owned-byte result may be live per handle.
 *
 * @param[in] handle Live fixture handle; must not be NULL.
 * @param[in] input Borrowed input. NULL is valid only when `input_len` is zero.
 * @param[in] input_len Number of input bytes.
 * @param[out] out_bytes Library-owned bytes published only on success.
 * @param[out] out_len Published byte count.
 * @return `ra8_err_t` result.
 * @retval k_ra8_ok Both outputs were published.
 * @retval k_ra8_err_null_ptr A required pointer was NULL.
 * @retval k_ra8_err_invalid_arg `handle` was invalid.
 * @retval k_ra8_err_invalid_size `input_len` exceeds the fixture maximum.
 * @retval k_ra8_err_no_mem The handle's bounded output slot is occupied.
 * @pre A non-NULL `handle` remains live for the duration of the call.
 * @pre A non-NULL input addresses at least `input_len` readable bytes.
 * @post Failure leaves both outputs unchanged.
 * @post Success publishes one pointer and its exact length atomically.
 * @note Release successful output with `ra8_abi_fixture_bytes_release()`.
 * @note Task-only and non-reentrant.
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_abi_fixture_bytes_create(ra8_abi_fixture_t* handle,
                                                     const uint8_t*     input,
                                                     uint32_t           input_len,
                                                     uint8_t**          out_bytes,
                                                     uint32_t*          out_len);

/**
 * @brief Release library-owned bytes and clear the caller's pointer.
 *
 * @details This follows the standard handle-independent release shape. The
 * fixture identifies its single outstanding allocation from the published
 * pointer and does not require callers to retain another release argument.
 *
 * @param[in,out] in_out_bytes Published byte pointer. A NULL value is an
 *                             idempotent success; the pointer slot must exist.
 * @return `ra8_err_t` result.
 * @retval k_ra8_ok The bytes were absent or were released and cleared.
 * @retval k_ra8_err_null_ptr `in_out_bytes` was NULL.
 * @retval k_ra8_err_invalid_arg The byte pointer was not published by the fixture.
 * @retval k_ra8_err_invalid_state No library-owned bytes were outstanding.
 * @pre A non-NULL byte value was returned by
 * `ra8_abi_fixture_bytes_create()` and has not been released.
 * @pre `in_out_bytes` addresses writable pointer storage when non-NULL.
 * @post Failure leaves `in_out_bytes` unchanged.
 * @post Success clears the pointer and restores owned-byte capacity.
 * @note Task-only and non-reentrant.
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_abi_fixture_bytes_release(uint8_t** in_out_bytes);

/**
 * @brief Task-context callback borrowed by the fixture until unregister or destroy.
 *
 * @param[in] context Caller-owned context supplied at registration; may be NULL.
 * @param[in] bytes Borrowed input bytes. NULL is valid only when `length` is zero.
 *                  The span is valid only for the duration of this callback.
 * @param[in] length Number of readable bytes at `bytes`.
 * @return A defined `ra8_err_t` value. Unrecognized values are mapped to
 *         `k_ra8_err_invalid_arg` by the fixture.
 */
typedef ra8_err_t (*ra8_abi_fixture_callback_t)(void*          context,
                                                const uint8_t* bytes,
                                                uint32_t       length);

/**
 * @brief Borrow a callback and context until the named release boundary.
 * @param[in] handle Live fixture handle.
 * @param[in] callback C callback; must not be NULL.
 * @param[in] context Caller-owned context retained without dereferencing by Zig; may be NULL.
 * @return `k_ra8_ok`, `k_ra8_err_null_ptr`, `k_ra8_err_invalid_arg`, or `k_ra8_err_busy`.
 * @post Success retains callback and context until unregister, cancel, or destroy.
 * @pre The caller keeps the callback code and any non-NULL context alive until
 * successful unregister, cancel, or destruction.
 * @note Task-only and non-reentrant.
 */
[[nodiscard]] ra8_err_t ra8_abi_fixture_callback_register(ra8_abi_fixture_t*         handle,
                                                          ra8_abi_fixture_callback_t callback,
                                                          void*                      context);

/**
 * @brief Invoke the registered callback synchronously in caller task context.
 * @param[in] handle Live fixture handle.
 * @param[in] bytes Borrowed bytes. NULL is valid only when `length` is zero;
 * valid storage remains borrowed through the synchronous callback only.
 * @param[in] length Number of bytes.
 * @return A recognized callback result, or a fixture validation/state result.
 * Unrecognized callback values map to `k_ra8_err_invalid_arg`.
 * @post No callback remains active after this function returns.
 * @note Reentrant calls on the same handle return `k_ra8_err_busy`.
 */
[[nodiscard]] ra8_err_t
ra8_abi_fixture_callback_invoke(ra8_abi_fixture_t* handle, const uint8_t* bytes, uint32_t length);

/**
 * @brief Release the borrowed callback and context while idle.
 * @param[in] handle Live fixture handle.
 * @return `k_ra8_ok`, or a validation/state result; active callbacks return `k_ra8_err_busy`.
 * @post Success guarantees no later callback begins or continues.
 */
[[nodiscard]] ra8_err_t ra8_abi_fixture_callback_unregister(ra8_abi_fixture_t* handle);

/**
 * @brief Cancel callback registration through the same release boundary.
 * @param[in] handle Live fixture handle.
 * @return The unregister result.
 * @post Success releases the retained callback and context.
 */
[[nodiscard]] ra8_err_t ra8_abi_fixture_callback_cancel(ra8_abi_fixture_t* handle);

#ifdef __cplusplus
}
#endif
