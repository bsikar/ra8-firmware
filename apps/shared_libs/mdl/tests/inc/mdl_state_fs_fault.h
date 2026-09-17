/**
 * @file mdl_state_fs_fault.h
 * @brief Delegating portable-filesystem fault wrapper for state tests.
 * @details Defines independently selectable failure points for portable state qualification.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "fw_if_fs.h"

/** @brief Independently combinable filesystem fault controls. */
typedef enum : uint32_t {
  k_mdl_state_fault_none              = 0U,         /**< No injected fault.           */
  k_mdl_state_fault_short_read        = 1UL << 0U,  /**< Limit each read to one byte. */
  k_mdl_state_fault_short_write       = 1UL << 1U,  /**< Limit writes to one byte.    */
  k_mdl_state_fault_read              = 1UL << 2U,  /**< Fail stream reads.           */
  k_mdl_state_fault_close             = 1UL << 3U,  /**< Fail after closing.          */
  k_mdl_state_fault_unlink            = 1UL << 4U,  /**< Fail namespace unlink.       */
  k_mdl_state_fault_begin             = 1UL << 5U,  /**< Fail transaction begin.      */
  k_mdl_state_fault_transaction_write = 1UL << 6U,  /**< Fail transaction writes.     */
  k_mdl_state_fault_seek              = 1UL << 7U,  /**< Fail transaction seek.       */
  k_mdl_state_fault_validate          = 1UL << 8U,  /**< Fail stage validation.       */
  k_mdl_state_fault_commit_before     = 1UL << 9U,  /**< Fail before publication.     */
  k_mdl_state_fault_commit_after      = 1UL << 10U, /**< Fail after publication.      */
  k_mdl_state_fault_abort             = 1UL << 11U, /**< Fail after stage cleanup.    */
  k_mdl_state_fault_stat              = 1UL << 12U, /**< Fail namespace stat.         */
  k_mdl_state_fault_list              = 1UL << 13U, /**< Fail directory enumeration.  */
  k_mdl_state_fault_rmdir             = 1UL << 14U, /**< Fail directory removal.      */
  k_mdl_state_fault_media             = 1UL << 15U, /**< Report media unavailable.    */
  k_mdl_state_fault_dir_open          = 1UL << 16U, /**< Fail directory-cursor open.  */
  k_mdl_state_fault_dir_next          = 1UL << 17U, /**< Fail directory-cursor next.  */
  k_mdl_state_fault_stream_seek       = 1UL << 18U, /**< Fail open-stream seeks.      */
} mdl_state_fault_flag_t;

/** @brief Wrapper context and bound facade. */
typedef struct {
  const fw_fs_t* inner; /**< Real backend receiving delegated calls. */
  fw_fs_t        fs;    /**< Fault-capable facade used by the test.  */
  uint32_t       flags; /**< OR of ::mdl_state_fault_flag_t.         */
} mdl_state_fault_fs_t;

/**
 * @brief Bind a no-allocation fault wrapper around one real filesystem.
 * @details Delegates every operation while exposing independently selectable deterministic faults.
 * @param[out] fault Wrapper to initialize.
 * @param[in] inner Complete real filesystem binding.
 * @return Canonical binding/capability status.
 * @retval k_ra8_err_no_mem Nested backend workspace or alignment exceeds the fixed wrapper.
 * @pre Both pointers are non-NULL and @p inner remains live while used.
 * @pre @p inner exposes complete namespace, stream, and transaction contracts.
 * @post Success initializes `fault->fs` with all faults disabled.
 * @post Failure leaves no live delegated operation.
 * @note The wrapper owns no backend resource.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t mdl_state_fault_fs_init(mdl_state_fault_fs_t* fault, const fw_fs_t* inner);
