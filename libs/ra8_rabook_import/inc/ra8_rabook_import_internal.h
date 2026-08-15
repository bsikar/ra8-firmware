/**
 * @file ra8_rabook_import_internal.h
 * @brief Module-private bounded source-key streaming contracts.
 *
 * @details
 * Declares the exact sequential-read seam shared by the RABOOK importer and
 * its focused tests. The contract consumes a stable source-size snapshot,
 * confirms EOF within a bounded callback budget, and publishes CRC and size
 * only after the complete stream has been accepted.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since Version 0.1.0
 */
#pragma once

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"

/** @brief Exact sequential-reader seam used by the source-key CRC pass. */
typedef ra8_err_t (*ra8_rabook_import_read_fn)(void*     ctx,
                                               uint8_t*  buf,
                                               uint32_t  requested,
                                               uint32_t* out_read);

/**
 * @brief Compute one exact bounded source CRC through an injected reader.
 * @details Consumes the immutable size in fixed-capacity chunks, rejects short
 *          or oversized callback results, and requires one final exact EOF read
 *          before publishing either source-key output.
 * @param[in] read_fn Sequential read callback.
 * @param[in,out] read_ctx Opaque callback context.
 * @param[in] expected_size Immutable source-size snapshot.
 * @param[out] buf Caller-owned transfer buffer.
 * @param[in] cap Transfer-buffer capacity.
 * @param[in] max_reads Maximum data-bearing callback calls.
 * @param[out] out_size Published exact size on success only.
 * @param[out] out_crc Published CRC-32/ISO-HDLC on success only.
 * @return Exact-stream validation status.
 * @retval k_ra8_ok The size snapshot was consumed and EOF confirmed.
 * @retval k_ra8_err_invalid_size Geometry, callback count, or read length was
 * invalid.
 * @retval k_ra8_err_* Callback failure, returned verbatim.
 * @pre Pointer arguments are non-NULL; @p buf holds @p cap bytes.
 * @pre @p expected_size is stable and representable by the published uint32_t size.
 * @post Success publishes both outputs; failure leaves both unchanged.
 * @post Success confirms one additional callback returned exact EOF.
 * @note Not thread-safe unless the injected reader serializes its context.
 * @since Version 0.1.0
 */
RA8_PRIV ra8_err_t priv_ra8_rabook_import_crc_stream(ra8_rabook_import_read_fn read_fn,
                                                     void*                     read_ctx,
                                                     uint64_t                  expected_size,
                                                     uint8_t*                  buf,
                                                     uint32_t                  cap,
                                                     uint32_t                  max_reads,
                                                     uint32_t*                 out_size,
                                                     uint32_t*                 out_crc);

/**
 * @brief Exercise the bounded source-key CRC contract from focused tests.
 * @details Forwards every argument unchanged to the module-private production
 *          implementation so fault-vector tests cover the exact shipped loop.
 * @param[in] read_fn Sequential read callback.
 * @param[in,out] read_ctx Opaque callback context.
 * @param[in] expected_size Immutable source-size snapshot.
 * @param[out] buf Caller-owned transfer buffer.
 * @param[in] cap Transfer-buffer capacity.
 * @param[in] max_reads Maximum data-bearing callback calls.
 * @param[out] out_size Published exact size on success only.
 * @param[out] out_crc Published CRC-32/ISO-HDLC on success only.
 * @return Exact-stream validation status from the production helper.
 * @retval k_ra8_ok The size snapshot was consumed and EOF confirmed.
 * @retval k_ra8_err_invalid_size Geometry, count, or callback length was invalid.
 * @retval k_ra8_err_* An injected callback error, returned verbatim.
 * @pre Pointer arguments are non-NULL and @p buf holds @p cap bytes.
 * @pre @p max_reads bounds every possible data-bearing callback.
 * @post Success publishes both result objects.
 * @post Failure leaves both output objects unchanged.
 * @note Test helper; thread safety follows the injected reader context.
 * @since Version 0.1.0
 */
RA8_TEST_HELPER ra8_err_t ra8_rabook_import_crc_stream_test(ra8_rabook_import_read_fn read_fn,
                                                            void*                     read_ctx,
                                                            uint64_t                  expected_size,
                                                            uint8_t*                  buf,
                                                            uint32_t                  cap,
                                                            uint32_t                  max_reads,
                                                            uint32_t*                 out_size,
                                                            uint32_t*                 out_crc);
