/**
 * @file ra8_rabook_xml_shim_test_internal.h
 * @brief Test-only seams for post-validation XML reader faults.
 * @details Declares the bounded selection and emission entry points used to
 *          force second-pass reader failures after the public first-pass
 *          validator would otherwise reject the source.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since Version 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_rabook_xml_shim.h"
#include "rabook_compile.h"

/**
 * @brief Run the XML subtree-selection pass without public validation.
 * @details Initialises a fresh pull reader over the supplied bytes and drives
 *          the post-validation selection logic. The selected coordinates are
 *          intentionally discarded; the seam exists only to verify defensive
 *          reader-failure propagation in a `UNIT_TEST` build.
 * @param[in] source Immutable XML bytes, or nullptr for reader validation.
 * @param[in] source_len Exact readable extent of @p source in bytes.
 * @param[in,out] workspace Exclusive XML reader workspace.
 * @return Repository status from reader initialisation, traversal, or selection.
 * @retval k_ra8_ok Selection reached a clean end or selected a direct body.
 * @retval k_ra8_err_null_ptr @p source was nullptr.
 * @retval k_ra8_err_invalid_size @p source_len was zero or exceeded `UINT32_MAX`.
 * @retval k_ra8_err_validation_failed XML traversal or the bounded direct-child
 *         search failed.
 * @pre @p workspace is non-null and not shared with another live reader.
 * @pre A non-null @p source spans exactly @p source_len readable bytes.
 * @post @p source is unchanged on every result.
 * @post @p workspace contains only scratch state from the completed or failed pass.
 * @note Test-only and not thread-safe for a shared @p workspace.
 * @par MC/DC:
 * This seam bypasses the successful public validation prerequisite so tests
 * can force the second-pass reader-error legs in `internal_select()`.
 * @since Version 0.1.0
 */
RA8_TEST_HELPER RA8_NODISCARD ra8_err_t
priv_ra8_rabook_xml_select_unvalidated_test(const uint8_t*              source,
                                            size_t                      source_len,
                                            ra8_rabook_xml_workspace_t* workspace);

/**
 * @brief Run XML subtree emission without public validation or rollback.
 * @details Drives emission from source offset zero at depth zero with a fresh
 *          pull reader. It deliberately omits the public validation and
 *          checkpoint restoration so a `UNIT_TEST` build can observe a
 *          failure that occurs after the selected root has begun emission.
 * @param[in] source Immutable XML bytes, or nullptr for reader validation.
 * @param[in] source_len Exact readable extent of @p source in bytes.
 * @param[in,out] ctx Active bounded RABOOK builder receiving emitted nodes.
 * @param[in,out] workspace Exclusive XML reader and emission workspace.
 * @return Repository status from reader initialisation, traversal, or emission.
 * @retval k_ra8_ok The selected root was consumed without a reader error.
 * @retval k_ra8_err_null_ptr @p source was nullptr.
 * @retval k_ra8_err_invalid_size @p source_len was zero or exceeded `UINT32_MAX`.
 * @retval k_ra8_err_no_mem Bounded builder capacity was exhausted.
 * @retval k_ra8_err_validation_failed XML traversal failed.
 * @pre @p ctx and @p workspace are non-null, initialized, and exclusive.
 * @pre A non-null @p source spans exactly @p source_len readable bytes.
 * @post @p source is unchanged on every result.
 * @post @p ctx may retain partial emission and @p workspace remains scratch;
 *       this narrow seam intentionally performs no public rollback.
 * @note Test-only and not thread-safe for shared builder or workspace state.
 * @par MC/DC:
 * This seam bypasses the successful public validation prerequisite so tests
 * can force the second-pass reader-error legs in `internal_emit()`.
 * @since Version 0.1.0
 */
RA8_TEST_HELPER RA8_NODISCARD ra8_err_t
priv_ra8_rabook_xml_emit_unvalidated_test(const uint8_t*              source,
                                          size_t                      source_len,
                                          ra8_rabook_ctx_t*           ctx,
                                          ra8_rabook_xml_workspace_t* workspace);

#ifdef __cplusplus
}
#endif
