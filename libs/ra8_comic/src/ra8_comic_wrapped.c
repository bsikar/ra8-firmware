/**
 * @file ra8_comic_wrapped.c
 * @brief Wrapped comic open: gzip / XZ unwrap in front of the container detect.
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * `ra8_comic_open_wrapped` extends the facade to gzip- and XZ-wrapped
 * archives (`.tar.gz`, `.tar.xz`, `.cbt.gz`, ...): the wrapper is decoded
 * whole into a caller-owned arena under the default decompression-limits
 * policy, then the inner bytes are opened through the ordinary container
 * detect over a flat-memory view. The view descriptor
 * (::ra8_unarch_mem_t) is stored at the arena's (8-aligned) start so it
 * out-lives the open exactly as long as the arena does -- demand-paged
 * page reads keep hitting it for the comic's whole lifetime, and the
 * caller owns exactly one buffer.
 *
 * Nesting is bounded structurally: the unwrapped bytes are re-probed and a
 * second wrapper layer (gzip-in-gzip, xz-in-gzip, ...) is rejected as a
 * ::k_ra8_err_decomp_depth bomb before any inner decode starts.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since Version 0.1.0
 *
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_check.h"
#include "ra8_comic.h"
#include "ra8_comic_internal.h"

/* ra8_comic_open_wrapped probes the outer container, dispatches gz/xz/bare, and
 * fans out to the format backends behind fail-closed guards: the open body
 * clears clang-tidy's statement/nesting thresholds while staying within the
 * 60-line NASA Rule 4 gate. Same disposition as the decode state machines in
 * the tree (ra8_jpeg_sw_encode, ra8_rmac_phy). */
/* NOLINTBEGIN(readability-function-size,readability-function-cognitive-complexity) */
#include "ra8_attributes.h"
#include "ra8_unarch_gzip.h"
#include "ra8_unarch_io.h"
#include "ra8_unarch_xz.h"

/** @brief Log tag for wrapped-open diagnostics. */
static const char* const s_tag_wrap = "ra8_comic_wrap";

/**
 * @enum wrap_dims_t
 * @brief Arena layout constants for the wrapped open.
 * @details The flat-memory descriptor occupies the first
 *          ::k_wrap_hdr_bytes of the caller arena (its size rounded up to
 *          the arena alignment); the unwrapped payload follows.
 * @since Version 0.1.0
 */
typedef enum : uint16_t {
  k_wrap_hdr_bytes = (uint16_t)((sizeof(ra8_unarch_mem_t) + ((size_t)k_ra8_comic_wrap_align - 1U)) &
                                ~((size_t)k_ra8_comic_wrap_align - 1U)),
  /**< Descriptor slot at the arena start (aligned sizeof). */
} wrap_dims_t;

/**
 * @brief Decode the gzip / XZ wrapper into the arena's payload region.
 * @details Routes on the probed magic; both legs run the default
 *          decompression-limits policy. An XZ file without a caller XZ
 *          scratch fails closed through the XZ wrapper's null guard.
 * @param[in]  read        Byte reader over the outer file.
 * @param[in]  ctx         Context for @p read.
 * @param[in]  size        Outer file length in bytes.
 * @param[in]  is_gzip     True for a gzip wrapper, false for XZ.
 * @param[out] payload     Unwrap destination (the arena past the header).
 * @param[in]  payload_cap Destination capacity in bytes.
 * @param[in]  xz_scratch  XZ session scratch (unused for gzip).
 * @param[in]  xz_scratch_len Scratch length in bytes.
 * @param[out] out_len     Receives the unwrapped byte count.
 * @return ra8_err_t status from the wrapper decoder.
 * @retval k_ra8_ok       Wrapper decoded and verified into @p payload.
 * @retval k_ra8_err_*    Any bounded unwrap failure (propagated verbatim).
 * @pre The leading magic matched the selected wrapper.
 * @pre @p payload holds @p payload_cap writable bytes.
 * @post On k_ra8_ok, `payload[0..*out_len)` holds the inner container.
 * @post On any error the open is abandoned (fail-closed).
 * @note Not thread-safe (single-client decoder states).
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_unwrap(ra8_comic_read_fn read,
                                 void*             ctx,
                                 uint64_t          size,
                                 bool              is_gzip,
                                 uint8_t*          payload,
                                 size_t            payload_cap,
                                 void*             xz_scratch,
                                 uint32_t          xz_scratch_len,
                                 size_t*           out_len)
{
  if (is_gzip) {
    return ra8_unarch_gzip_unwrap(read, ctx, size, payload, payload_cap, nullptr, out_len);
  }
  return ra8_unarch_xz_unwrap(read,
                              ctx,
                              size,
                              payload,
                              payload_cap,
                              xz_scratch,
                              xz_scratch_len,
                              nullptr,
                              out_len);
}

ra8_err_t ra8_comic_open_wrapped(ra8_comic_t*      c,
                                 ra8_comic_read_fn read,
                                 void*             ctx,
                                 uint64_t          size,
                                 ra8_comic_page_t* pages,
                                 uint32_t          page_cap,
                                 char*             names,
                                 uint32_t          names_cap,
                                 uint8_t*          arena,
                                 size_t            arena_cap,
                                 void*             xz_scratch,
                                 uint32_t          xz_scratch_len)
{
  RA8_CHECK_NULL_PTR(c, s_tag_wrap, "wrapped open: null c");
  RA8_CHECK_NULL_PTR((const void*)read, s_tag_wrap, "wrapped open: null read");
  RA8_CHECK_NULL_PTR(arena, s_tag_wrap, "wrapped open: null arena");
  if (size == 0U) {
    return k_ra8_err_invalid_size;
  }
  uint8_t      sig[k_ra8_comic_magic_len] = {};
  const size_t got                        = read(ctx, 0U, sig, sizeof(sig));
  const bool   is_gzip                    = ra8_unarch_gzip_magic(sig, got);
  const bool   is_xz                      = ra8_unarch_xz_magic(sig, got);
  if (!is_gzip && !is_xz) {
    /* Not a wrapper: the ordinary container detect owns it. */
    return ra8_comic_open(c, read, ctx, size, pages, page_cap, names, names_cap);
  }
  if (((uintptr_t)arena % (uintptr_t)k_ra8_comic_wrap_align) != 0U) {
    return k_ra8_err_invalid_size; /* descriptor slot needs the alignment */
  }
  if (arena_cap <= (size_t)k_wrap_hdr_bytes) {
    return k_ra8_err_invalid_size; /* no room for descriptor + payload */
  }
  uint8_t*        payload     = &arena[k_wrap_hdr_bytes];
  const size_t    payload_cap = arena_cap - (size_t)k_wrap_hdr_bytes;
  size_t          inner_len   = 0U;
  const ra8_err_t uerr        = internal_unwrap(read,
                                                ctx,
                                                size,
                                                is_gzip,
                                                payload,
                                                payload_cap,
                                                xz_scratch,
                                                xz_scratch_len,
                                                &inner_len);
  if (uerr != k_ra8_ok) {
    return uerr;
  }
  /* Nesting bomb: a wrapper inside the wrapper is rejected before any
   * inner decode -- the one reachable depth bound in this pipeline. */
  const bool inner_gzip = ra8_unarch_gzip_magic(payload, inner_len);
  const bool inner_xz   = ra8_unarch_xz_magic(payload, inner_len);
  if (inner_gzip || inner_xz) {
    return k_ra8_err_decomp_depth;
  }
  /* The flat-memory descriptor lives at the arena start so it out-lives
   * this call: page extraction keeps reading through it. */
  void* const             hdr_slot = arena;
  ra8_unarch_mem_t* const mem      = (ra8_unarch_mem_t*)hdr_slot;
  mem->base                        = payload;
  mem->len                         = inner_len;
  return ra8_comic_open(c,
                        ra8_unarch_mem_read,
                        mem,
                        (uint64_t)inner_len,
                        pages,
                        page_cap,
                        names,
                        names_cap);
}
/* NOLINTEND(readability-function-size,readability-function-cognitive-complexity) */
