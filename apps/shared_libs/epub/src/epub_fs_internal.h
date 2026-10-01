/**
 * @file epub_fs_internal.h
 * @brief Test-access surface for the EPUB filesystem stream callback
 * @ingroup grp_ereader
 *
 * @details Not part of the public API. Focused host tests include this header
 * to drive defensive callback guards on the production source under MC/DC.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "epub_fs.h"
#include "ra8_attributes.h"

/**
 * @brief Read one bounded span from a live streamed-filesystem context.
 * @details Test-access form of the callback bound by
 * ::epub_open_streamed_fs. It seeks the open `ra8_fs` file to @p offset and
 * reads at most @p len bytes. Production callers keep using the public facade.
 * @param[in,out] ctx Bound ::epub_stream_fs_ctx_t descriptor.
 * @param[in] offset Absolute file offset.
 * @param[out] buf Destination for up to @p len bytes.
 * @param[in] len Requested byte count.
 * @param[out] out_read Receives the bytes read, on every return path.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The read succeeded; `*out_read` is @p len, or less at end of file.
 * @retval k_ra8_err_null_ptr An argument guard rejected the request.
 * @retval k_ra8_err_out_of_range The offset is past the 32-bit filesystem range.
 * @retval other The filesystem's own seek or read error.
 * @pre When non-null, @p ctx points to a live ::epub_stream_fs_ctx_t.
 * @pre When non-null, @p buf is writable for @p len bytes.
 * @pre @p out_read is non-null.
 * @post At most @p len bytes are written to @p buf.
 * @post A rejected argument guard performs no filesystem operation.
 * @note Test-access only and not thread-safe; mutates the bound file cursor.
 * @par MC/DC:
 * The context-null / file-null / buffer-null OR has one-null-at-a-time and
 * all-valid vectors in test_epub_fs.c.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t
priv_epub_fs_stream_read(void* ctx, uint64_t offset, void* buf, uint32_t len, uint32_t* out_read);

#ifdef __cplusplus
}
#endif
