/**
 * @file mdl_test_storage.h
 * @brief Process-local portable storage binding for hosted media tests.
 * @details Declares the bounded initialization, access, and teardown contract
 *          shared by media tests that exercise portable storage.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "mdl_storage.h"

/**
 * @brief Initialize the test process's root-bound POSIX storage facade.
 * @return Canonical adapter or downloader-binding status.
 * @retval k_ra8_ok The process-local binding is ready.
 * @retval other POSIX adapter or workspace initialization failed.
 * @pre The binding is not already initialized.
 * @post Success makes ::mdl_test_storage_get return a live binding.
 * @post Failure leaves no live POSIX adapter resource.
 * @note Test processes call this once and execute serially.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t mdl_test_storage_init(void);

/**
 * @brief Return the initialized process-local test storage binding.
 * @return Borrowed storage pointer.
 * @retval non-NULL Live process-local binding.
 * @pre ::mdl_test_storage_init returned ::k_ra8_ok.
 * @post Ownership remains with the test process.
 * @note Not thread-safe for concurrent file operations.
 * @since 0.1.0
 */
mdl_storage_t* mdl_test_storage_get(void);

/**
 * @brief Release the test process's POSIX adapter resource.
 * @return Canonical adapter shutdown status.
 * @retval k_ra8_ok The root descriptor was closed.
 * @retval other The adapter reported a shutdown failure.
 * @pre Initialization previously succeeded and no file is open.
 * @post The borrowed storage binding must no longer be used.
 * @note Test processes call this once after all vectors.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t mdl_test_storage_deinit(void);
