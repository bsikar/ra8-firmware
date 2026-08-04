/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_io_stream_blockdev.h
 * @brief ra8_io byte-stream sink that buffers into a raw block device.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Block devices are sector-granular, so a byte stream must accumulate a 512-byte
 * sector and flush it. This sink writes consecutive logical blocks of an
 * ::ra8_io_blockdev_t starting at a chosen LBA: bytes fill an internal sector
 * buffer; a full sector is committed automatically; ::ra8_io_stream_flush commits
 * any partial trailing sector (zero-padded). Useful for streaming a log or a
 * blob straight onto SD / OSPI / MRAM via the fabric without a filesystem.
 *
 *
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_io_blockdev.h"
#include "ra8_io_stream.h"

/**
 * @struct ra8_io_stream_blockdev_state_t
 * @brief Caller-owned private state for a block-device stream sink.
 *
 * @details
 * Zero-initialise and pass to ::ra8_io_stream_blockdev_init. Treat the fields as
 * private. The bound block device and this state must out-live the stream.
 *
 * @invariant `fill < k_ra8_io_block_size_bytes`.
 *
 * @since 0.1.0
 */
typedef struct {
  const ra8_io_blockdev_t* bd;   /**< Target device (private). */
  uint32_t                 lba;  /**< Next LBA to write.       */
  uint32_t                 fill; /**< Bytes in the sector buf. */
  /** Sector buffer. */
  uint8_t sector[(uint32_t)k_ra8_io_block_size_bytes];
} ra8_io_stream_blockdev_state_t;

/**
 * @brief Bind a block-device stream sink into a caller-owned stream handle.
 *
 * @param[out] s         Stream handle to bind (zero-initialised by the caller).
 * @param[out] state     Caller-owned sink state to populate.
 * @param[in]  bd        Bound block device to stream into.
 * @param[in]  start_lba First logical block address to write.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Sink bound; `s` is usable.
 * @retval k_ra8_err_null_ptr `s`, `state`, or `bd` was NULL.
 *
 * @pre `bd` is a bound block device that out-lives the stream.
 * @pre `s` and `state` out-live every call made through the stream.
 * @post On success `s` streams bytes onto `bd` from `start_lba`.
 * @post On any non-ok return `s` and `state` are left unbound/untouched.
 *
 * @note Not thread-safe with respect to the same stream. Call
 *       ::ra8_io_stream_flush to commit a partial trailing sector.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_io_stream_blockdev_init(ra8_io_stream_t*                s,
                                                    ra8_io_stream_blockdev_state_t* state,
                                                    const ra8_io_blockdev_t*        bd,
                                                    uint32_t                        start_lba);

#ifdef __cplusplus
}
#endif
