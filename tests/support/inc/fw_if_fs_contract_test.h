/**
 * @file fw_if_fs_contract_test.h
 * @brief Shared filesystem facade contract-fault and VFS-init test vectors.
 *
 * @details
 * Declares the backend-independent negative vector that mutates copies of a
 * truthful filesystem binding and proves the facade rejects incoherent
 * metadata, transfer counts, capability claims, and publication results, and
 * the firmware VFS adapter's argument/lifecycle guard vector, moved here from
 * tests/misc/src/test_fw_if_fs.c to keep both files under the repository's per-file
 * line cap.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "fw_if_fs.h"
#include "ra8_attributes.h"
#include "ra8_fs.h"

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
 * @par MC/DC:
 * No compound decisions; each vector drives one single-condition guard.
 * @since 0.1.0
 */
RA8_TEST_HELPER void ra8_test_fw_if_fs_check_contract_guards(const fw_fs_t* fs);

/**
 * @brief Reject every unusable VFS binding request and honour media policy.
 * @details Runs the firmware VFS initializer vector through production
 *          filesystem seams and checks observable state.
 * @param[in] mount Live native ra8_fs mount handle the caller already opened.
 * @pre @p mount is non-NULL and mounted.
 * @pre No VFS mount already answers to the name "ram".
 * @post No access exceeds a caller-advertised capacity.
 * @post The return value or assertions describe the observed filesystem state.
 * @note Test helper; assertions report every rejection directly.
 * @par MC/DC:
 * No compound decisions; each vector drives one single-condition guard.
 * @since 0.1.0
 */
RA8_TEST_HELPER void ra8_test_fw_if_fs_vfs_init_guards(ra8_fs_mount_t* mount);
