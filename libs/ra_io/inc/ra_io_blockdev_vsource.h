/**
 * @file ra_io_blockdev_vsource.h
 * @brief Thin adapter exposing an ra_io block device as an ra_vsource reader.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * The #147 page cache (Layer 2, `ra_vmem`) pages objects in through a Layer-1
 * object-source registry (`ra_vsource`, Ring 2 / Core). That registry binds each
 * paged object to a generic byte-offset read callback of type
 * ::ra_vsource_read_fn so the cache stays free of any storage dependency. A
 * block device (::ra_io_blockdev_t, Ring 4 / PAL) speaks 512-byte logical blocks
 * addressed by LBA, not byte offsets.
 *
 * This file is the sanctioned bridge between the two seams. It exposes a bound
 * block device as a read-only ::ra_vsource_read_fn, performing the LBA<->byte
 * translation once so apps wiring a block device into the page cache via
 * ::ra_vsource_add_paged do not re-roll that arithmetic inline.
 *
 * ## Ring direction
 *
 * This adapter lives in `ra_io` (Ring 4) and depends on both `ra_vsource.h`
 * (Ring 2) and `ra_io_blockdev.h` (Ring 4). A Ring-4 file depending on a Ring-2
 * header is allowed; the reverse -- making Ring-2 `ra_vsource` depend on Ring-4
 * `ra_io_blockdev` -- is forbidden because it would invert ring ordering (see
 * `docs/RING_AND_WORLD.md`). The two storage seams are kept distinct on purpose:
 * `ra_io_blockdev` is read/write/erase at Ring 4; `ra_vsource` is a read-only
 * byte-offset view at Ring 2. This adapter is the intentional, ring-respecting
 * bridge, not a sign the seams should be unified.
 *
 * ## Bounce buffer
 *
 * Reads that are not sector-aligned, or that do not cover a whole number of
 * sectors, are serviced through a one-sector bounce buffer held inside the
 * caller-owned context (::ra_io_blockdev_vsource_ctx_t). No dynamic allocation
 * occurs (NASA P10 Rule 3); the caller owns the context and it must out-live
 * every read made through it.
 *
 * @code
 * static ra_io_blockdev_vsource_ctx_t s_vsrc_ctx;
 * (void)ra_io_blockdev_vsource_init(&s_vsrc_ctx, &bd);
 *
 * uint32_t oid = 0;
 * (void)ra_vsource_add_paged(&vs, ra_io_blockdev_vsource_read, &s_vsrc_ctx,
 *                            0U, file_size_bytes, &oid);
 * @endcode
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra_err.h"
#include "ra_io_blockdev.h"
#include "ra_vsource.h"

/**
 * @struct ra_io_blockdev_vsource_ctx_t
 * @brief Caller-owned adapter context binding a block device to a vsource read.
 *
 * @details
 * Zero-initialise and pass to ::ra_io_blockdev_vsource_init, which records the
 * bound block device and prepares the embedded one-sector bounce buffer. The
 * `ctx` you register with ::ra_vsource_add_paged is the address of this struct,
 * and ::ra_io_blockdev_vsource_read is the callback. Treat the fields as
 * private. The struct and the bound device must out-live every read made
 * through them.
 *
 * @invariant `bd` is non-NULL once ::ra_io_blockdev_vsource_init has succeeded.
 * @invariant `scratch` is exactly one logical block (512 bytes).
 *
 * @since 0.1.0
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  const ra_io_blockdev_t* bd; /**< Bound block device (private; read-only use). */
  uint8_t scratch[(uint32_t)k_ra_io_block_size_bytes]; /**< One-sector bounce buffer (private). */
} ra_io_blockdev_vsource_ctx_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @brief Bind a block device into a vsource adapter context.
 *
 * @details
 * Records `bd` in `ctx` so ::ra_io_blockdev_vsource_read can service byte-offset
 * reads against it. No device access occurs here; the device is only touched on
 * the first read. No allocation occurs.
 *
 * @param[out] ctx Caller-owned adapter context (zero-initialised by the caller).
 * @param[in]  bd  Bound block-device handle to expose as a read-only source.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok           Adapter bound; `ctx` is usable.
 * @retval k_ra_err_null_ptr `ctx` or `bd` was NULL.
 *
 * @pre `bd` has a backend bound and out-lives every read through `ctx`.
 * @pre `ctx` out-lives every read made through it.
 * @post On success `ctx->bd == bd`.
 * @post On any non-ok return `ctx` is left unbound.
 *
 * @note Not thread-safe with respect to the same context.
 *
 * @see ra_io_blockdev_vsource_read  The read callback to register.
 * @see ra_vsource_add_paged         Where to wire the callback + context.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_io_blockdev_vsource_init(ra_io_blockdev_vsource_ctx_t* ctx,
                                                   const ra_io_blockdev_t*       bd);

/**
 * @brief Read `len` bytes at byte `offset` from the bound block device.
 *
 * @details
 * The ::ra_vsource_read_fn-compatible entry point. Translates the absolute byte
 * `offset` to an LBA + intra-block offset and reads `len` bytes, splitting the
 * request into three parts as needed: an unaligned head sector, a run of whole
 * aligned sectors read straight into `buf`, and an unaligned tail sector. The
 * head and tail go through the context's one-sector bounce buffer; the aligned
 * middle is read directly with no copy. Reads that walk past the device end are
 * rejected by the block device, not silently truncated.
 *
 * @param[in]  ctx    The ::ra_io_blockdev_vsource_ctx_t (as a void cookie).
 * @param[in]  offset Absolute byte offset within the device.
 * @param[out] buf    Destination buffer (`len` writable bytes).
 * @param[in]  len    Number of bytes to read.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok               `len` bytes copied into `buf`.
 * @retval k_ra_err_null_ptr     `ctx` or `buf` was NULL.
 * @retval k_ra_err_out_of_range `offset + len` overflows the 64-bit range.
 * @retval k_ra_err_*            Any error reported by the block device's read.
 *
 * @pre `ctx` was populated by ::ra_io_blockdev_vsource_init.
 * @pre `buf` is writable for `len` bytes.
 * @post On success `buf[0 .. len)` mirrors the device bytes at `offset`.
 * @post On any non-ok return `buf` content is unspecified.
 *
 * @note Not thread-safe with respect to the same context (uses the shared
 *       bounce buffer).
 *
 * @see ra_io_blockdev_vsource_init  Bind a device before calling this.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_io_blockdev_vsource_read(void* ctx, uint64_t offset, uint8_t* buf, uint32_t len);

#ifdef __cplusplus
}
#endif
