/**
 * @file ra8_c6link_mdl_transfer_internal.h
 * @brief Private commit-stage seam for the transactional media coordinator
 * @details Exposes the terminal verify/validate/publish stage to focused host
 * tests so its metadata guard can be driven with terminal shapes the wire
 * validator rejects upstream. Production sequencing, the pull budget, and every
 * cleanup path stay private to `ra8_c6link_mdl_transfer.c`.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_c6link_mdl_transfer.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Run the terminal commit stage against a caller-supplied chunk
 * @details Forwards every argument unchanged to the module-private production
 * commit helper with an active storage transaction, so a focused test drives
 * the exact shipped digest-comparison, artifact-validation, and publication
 * sequence rather than a copy of it.
 * @param[in] config Complete transfer configuration supplying storage and hash.
 * @param[in] chunk Terminal chunk whose metadata the stage must verify.
 * @param[in] bytes_stored Durable byte count the injected digest describes.
 * @param[in] chunks_received Number of remote responses consumed.
 * @param[out] result Result written only after commit succeeds.
 * @return Verification or commit status from the production helper.
 * @retval k_ra8_ok Digest matched and storage committed atomically.
 * @retval k_ra8_err_invalid_size The digest is absent or the counts disagree.
 * @retval k_ra8_err_checksum_mismatch SHA-256 digests disagree.
 * @retval k_ra8_fail Hash finalisation or storage commit failed.
 * @pre @p config, @p chunk, and @p result are non-null and caller-owned.
 * @pre The injected hash stream has already consumed @p bytes_stored bytes.
 * @post Success fills @p result with the committed identity.
 * @post Failure leaves @p result unmodified by this stage.
 * @note Test helper; not thread-safe because it finalises injected contexts.
 * @par MC/DC:
 * The only seam that can drive `(!chunk->has_sha256)` true. A COMPLETE response
 * without the mandatory 32-byte digest is rejected by the chunk-semantics
 * validator in `ra8_c6link_mdl.c` before `ra8_c6link_mdl_transfer()` ever sees
 * it, so that operand of the terminal-metadata guard has no public-API vector.
 * @since 0.1.0
 */
RA8_TEST_HELPER ra8_err_t
ra8_c6link_mdl_transfer_commit_test(const ra8_mdl_transfer_config_t* config,
                                    const ra8_mdl_chunk_t*           chunk,
                                    uint64_t                         bytes_stored,
                                    uint32_t                         chunks_received,
                                    ra8_mdl_transfer_result_t*       result);

#ifdef __cplusplus
}
#endif
