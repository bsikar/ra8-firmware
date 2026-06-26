/**
 * @file ra_ble_patch.h
 * @brief Renesas BLE controller firmware patch loader (stub-with-warning).
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * The RA8D2 BLE controller boots from on-chip ROM and then expects
 * the host to load an encrypted firmware patch image before it will
 * accept HCI traffic. The patch image itself is distributed by
 * Renesas under NDA and CANNOT be redistributed in this repository.
 *
 * This translation unit therefore intentionally ships as a runtime
 * stub:
 *
 *   - ``ra_ble_patch_load(NULL, 0)`` is the integration point our
 *     ``ra_ble_open`` boot path always calls. With no patch
 *     configured it logs a warning, sets a "no patch present" flag
 *     and returns ``k_ra_err_not_supported`` so the caller can
 *     decide whether to halt or proceed.
 *   - ``ra_ble_patch_load(image, len)`` with a non-NULL pointer
 *     returns ``k_ra_err_not_supported`` until a downstream user
 *     replaces this TU with a real loader.
 *
 * The error mapping is documented per the API spec:
 *   - NULL pointer with len > 0  ==> ``k_ra_err_null_ptr``
 *   - Both NULL/zero arguments   ==> ``k_ra_err_not_supported``
 *     (no-patch indicator path; not actually a fatal error).
 *   - Non-NULL image             ==> ``k_ra_err_not_supported``
 *     (real loader not implemented in this build).
 *
 * @warning Do not deploy production firmware that depends on radio
 *          traffic without replacing this stub. The controller will
 *          NACK every HCI command without a valid patch image.
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

#include "ra_err.h"

/* ============================================================ */
/* Limits */
/* ============================================================ */

/**
 * @enum ra_ble_patch_limits_t
 * @brief Loader-side hard limits.
 */
typedef enum : uint32_t {
  k_ra_ble_patch_max_bytes = 65536U, /**< Largest patch image we will accept. */
  k_ra_ble_patch_min_bytes = 16U,    /**< Smallest plausible image size.      */
} ra_ble_patch_limits_t;

/* ============================================================ */
/* API */
/* ============================================================ */

/**
 * @brief Load the BLE controller firmware patch image.
 *
 * @details
 * Production-mode behaviour (when a real loader is linked in):
 *   1. Validate ``image`` and ``len`` fall in the documented bounds.
 *   2. Stream the image into PATCHADDR / PATCHDATA in 32-bit words
 *      (HUM Ch 50 "Bluetooth Low Energy" controller patch loop).
 *   3. Wait for STATUS.patch_done.
 *   4. Return ``k_ra_ok``.
 *
 * Stub-mode behaviour (this TU):
 *   - Logs a warning that the patch image is missing and returns
 *     ``k_ra_err_not_supported``. The caller (``ra_ble_open``) is
 *     responsible for deciding whether to continue with a degraded
 *     controller or to halt.
 *
 * Error mapping (matches the public spec used by callers):
 *   - ``image == NULL`` with ``len == 0`` -- "no patch configured" path.
 *     Returns ``k_ra_err_not_supported`` after logging.
 *   - ``image == NULL`` with ``len > 0``  -- ``k_ra_err_null_ptr``.
 *   - Non-NULL ``image``                  -- ``k_ra_err_not_supported``
 *     (real loader unavailable in this build).
 *   - Out-of-range ``len``                -- ``k_ra_err_invalid_arg``.
 *
 * @param[in] image Pointer to the patch image bytes, or NULL to
 *                  request the "no patch configured" probe.
 * @param[in] len   Image byte count. Must be 0 if ``image == NULL``,
 *                  otherwise within
 *                  ``[k_ra_ble_patch_min_bytes, k_ra_ble_patch_max_bytes]``.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_err_null_ptr      ``image == NULL && len > 0``.
 * @retval k_ra_err_invalid_arg   ``len`` out of range.
 * @retval k_ra_err_not_supported No patch configured (stub) or non-NULL
 *                                image with no production loader linked.
 *
 * @pre BLE controller block is powered (MSTPCRC.MSTPC bit cleared).
 * @pre Reset has been deasserted.
 * @post On success ``ra_ble_patch_is_loaded()`` returns 1.
 * @post On stub return the loader has logged the "no patch" warning
 *       and set the "no patch present" flag.
 *
 * @note The stub is safe to call from a single-threaded init context.
 *
 * @see ra_ble_patch_is_loaded()
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_ble_patch_load(const uint8_t* image, size_t len);

/**
 * @brief Report whether a patch image was loaded successfully.
 *
 * @return uint8_t 0 = no patch loaded (or stub path), 1 = patch live.
 *
 * @since 0.1.0
 *
 * @details See implementation.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 */
uint8_t ra_ble_patch_is_loaded(void);

#ifdef __cplusplus
}
#endif
