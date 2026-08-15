/**
 * @file ra8_fmt_host_spool.h
 * @brief Anonymous raw-fd scratch artifacts for portable format verification.
 * @details Defines composition-edge ownership for immediately unlinked scratch
 * descriptors while portable engines depend only on injected spool callbacks.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_fmt_stream.h"

/** @brief Caller-owned state for one unlinked scratch file. */
typedef struct {
  int      fd;       /**< Owned anonymous descriptor, or -1. */
  uint64_t position; /**< Bytes appended before sealing.     */
  bool     sealed;   /**< Positioned reads are now allowed.  */
} ra8_fmt_host_spool_t;

/**
 * @brief Create an anonymous scratch file beside an anchored input path.
 * @param[in] anchor_path Existing input spelling used only to select its parent.
 * @param[out] state Receives owned raw-fd state.
 * @param[out] out Receives portable append, seal, and positioned-read callbacks.
 * @return Host creation or bounded-path status.
 * @pre Output pointers are writable and @p anchor_path is NUL-terminated.
 * @post Success leaves no directory entry; only @p state owns the descriptor.
 * @post Failure owns no descriptor and creates no persistent filesystem object.
 * @note Host composition edge only; the portable verifier sees callbacks.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_fmt_host_spool_open(const char* anchor_path, ra8_fmt_host_spool_t* state, ra8_fmt_spool_t* out);

/**
 * @brief Close an anonymous scratch artifact.
 * @details Releases the only owner of an already-unlinked raw descriptor.
 * @param[in,out] state Possibly open spool state.
 * @pre @p state is null or was initialized by the spool-open operation.
 * @pre No callback is executing through @p state.
 * @post Any owned descriptor is closed and marked -1.
 * @post Repeated cleanup leaves the state closed.
 * @note Idempotent for sequential cleanup.
 * @since 0.1.0
 */
void ra8_fmt_host_spool_close(ra8_fmt_host_spool_t* state);
