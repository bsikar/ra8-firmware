/**
 * @file ra8_rabook_import_internal.h
 * @brief Module-private bounded source-key streaming contracts.
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
 * @post Success publishes both outputs; failure leaves both unchanged.
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

/** @brief Test-only entry onto @ref priv_ra8_rabook_import_crc_stream. */
RA8_TEST_HELPER ra8_err_t ra8_rabook_import_crc_stream_test(ra8_rabook_import_read_fn read_fn,
                                                            void*                     read_ctx,
                                                            uint64_t                  expected_size,
                                                            uint8_t*                  buf,
                                                            uint32_t                  cap,
                                                            uint32_t                  max_reads,
                                                            uint32_t*                 out_size,
                                                            uint32_t*                 out_crc);
