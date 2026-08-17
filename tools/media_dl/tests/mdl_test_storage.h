/**
 * @file mdl_test_storage.h
 * @brief Process-local portable storage binding for hosted media tests.
 * @details Declares the bounded initialization, access, and teardown contract
 *          shared by media tests that exercise portable storage.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>

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
 * @brief Publish one byte span through the transaction seam.
 * @details Removes any prior node at @p path, then stages, validates and
 *          commits the payload exactly as a production writer would.
 * @param[in] path Canonical destination path.
 * @param[in] bytes Payload bytes.
 * @param[in] length Payload extent in bytes.
 * @return Canonical storage status.
 * @retval k_ra8_ok The fixture was published.
 * @retval other Namespace, write, validation or commit failure.
 * @pre ::mdl_test_storage_init returned ::k_ra8_ok and every pointer is valid.
 * @pre No concurrent fixture writer exists in this process.
 * @post Success exposes exactly @p length bytes at @p path.
 * @post Failure claims no publication and aborts any open transaction.
 * @note Test processes call this serially.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
mdl_test_storage_publish(const char* path, const uint8_t* bytes, uint32_t length);

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
