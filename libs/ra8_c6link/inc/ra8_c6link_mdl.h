/**
 * @file ra8_c6link_mdl.h
 * @brief Pull-based media download client over ESP-hosted CustomRpc.
 * @ingroup grp_ereader
 *
 * @details The C6 owns HTTP fetching. The RA8 owns the destination path and
 * file lifecycle: callers pull bounded chunks and write them through the RA8
 * filesystem. This keeps POSIX paths and storage handles off the co-processor
 * wire and gives the existing single-outstanding-call c6link natural
 * backpressure.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#pragma once

#include <stdint.h>

#include "ra8_c6link.h"
#include "ra8_err.h"
#include "ra8_mdl_protocol.h"

/** @brief RA8-local state for one accepted remote job. */
typedef struct {
  uint32_t job_id;
  uint32_t next_sequence;
  uint64_t next_offset;
  uint32_t max_chunk_bytes;
  bool     active;
} ra8_mdl_session_t;

/** @brief One correlated, bounded download response. */
typedef struct {
  uint32_t        job_id;
  uint32_t        sequence;
  uint64_t        offset;
  uint64_t        total_bytes;
  ra8_mdl_state_t state;
  ra8_err_t       status;
  uint16_t        data_len;
  uint8_t         data[RA8_MDL_CHUNK_DATA_MAX];
  bool            has_sha256;
  uint8_t         sha256[RA8_MDL_SHA256_BYTES];
} ra8_mdl_chunk_t;

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Start an asynchronous C6 download and receive its job identifier. */
[[nodiscard]] ra8_err_t ra8_c6link_mdl_start(ra8_c6link_t*      link,
                                             const char*        url,
                                             ra8_mdl_format_t   format,
                                             ra8_mdl_session_t* session);

/** @brief Pull the next bounded chunk, acknowledging the prior offset. */
[[nodiscard]] ra8_err_t ra8_c6link_mdl_next(ra8_c6link_t*      link,
                                            ra8_mdl_session_t* session,
                                            uint16_t           max_bytes,
                                            ra8_mdl_chunk_t*   chunk);

/** @brief Cancel a remote job and invalidate the local session. */
[[nodiscard]] ra8_err_t ra8_c6link_mdl_cancel(ra8_c6link_t* link, ra8_mdl_session_t* session);

#ifdef __cplusplus
}
#endif
