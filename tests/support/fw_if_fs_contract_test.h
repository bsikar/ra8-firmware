/**
 * @file fw_if_fs_contract_test.h
 * @brief Shared filesystem facade contract-fault test vector.
 *
 * @details
 * Declares the backend-independent negative vector that mutates copies of a
 * truthful filesystem binding and proves the facade rejects incoherent
 * metadata, transfer counts, capability claims, and publication results.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "fw_if_fs.h"
#include "ra8_attributes.h"

/**
 * @brief Exercise every filesystem facade output-contract guard.
 * @details Mutates local copies of a real backend's vtables so invalid outputs
 *          are contained without invoking or changing the real adapter state.
 * @param[in] fs Fully bound filesystem used as a truthful baseline.
 * @pre @p fs is non-null and remains live for the complete vector.
 * @pre All three baseline vtables and their advertised capabilities agree.
 * @post Contradictory capability claims and impossible outputs are rejected.
 * @post The supplied filesystem binding and its backend state are unchanged.
 * @note Test helper; assertions report every contract violation directly.
 * @since 0.1.0
 */
RA8_TEST_HELPER void ra8_test_fw_if_fs_check_contract_guards(const fw_fs_t* fs);
