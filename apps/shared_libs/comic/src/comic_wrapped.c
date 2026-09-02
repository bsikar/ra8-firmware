/**
 * @file comic_wrapped.c
 * @brief Wrapped comic open: gzip / XZ unwrap in front of the container detect.
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * `comic_open_wrapped` extends the facade to gzip- and XZ-wrapped
 * archives (`.tar.gz`, `.tar.xz`, `.cbt.gz`, ...): the wrapper is decoded
 * whole into a caller-owned arena under the default decompression-limits
 * policy, then the inner bytes are opened through the ordinary container
 * detect over a flat-memory view. The view descriptor
 * (::unarch_mem_t) is stored at the arena's (8-aligned) start so it
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

#include "comic.h"
#include "comic_internal.h"
#include "ra8_attributes.h"
#include "ra8_check.h"
#include "unarch_gzip.h"
#include "unarch_io.h"
#include "unarch_xz.h"

/**
 * @enum wrap_dims_t
 * @brief Arena layout constants for the wrapped open.
 * @details The flat-memory descriptor occupies the first
 *          ::k_wrap_hdr_bytes of the caller arena (its size rounded up to
 *          the arena alignment); the unwrapped payload follows.
 * @since Version 0.1.0
 */
typedef enum : uint16_t {
  k_wrap_hdr_bytes = (uint16_t)((sizeof(unarch_mem_t) + ((size_t)k_comic_wrap_align - 1U)) &
                                ~((size_t)k_comic_wrap_align - 1U)),
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
static ra8_err_t internal_unwrap(comic_read_fn read,
                                 void*         ctx,
                                 uint64_t      size,
                                 bool          is_gzip,
                                 uint8_t*      payload,
                                 size_t        payload_cap,
                                 void*         xz_scratch,
                                 uint32_t      xz_scratch_len,
                                 size_t*       out_len)
{
  if (is_gzip) {
    return unarch_gzip_unwrap(read, ctx, size, payload, payload_cap, nullptr, out_len);
  }
  return unarch_xz_unwrap(read,
                          ctx,
                          size,
                          payload,
                          payload_cap,
                          xz_scratch,
                          xz_scratch_len,
                          nullptr,
                          out_len);
}

/**
 * @brief Check wrapper-arena alignment and payload capacity.
 * @details Converts the pointer representation without aliasing and checks the
 *          alignment and minimum header-plus-payload geometry.
 * @param[in] arena Candidate wrapper arena base.
 * @param[in] arena_cap Available arena bytes.
 * @return Whether the arena satisfies the wrapper storage contract.
 * @retval true The base is aligned and capacity exceeds the wrapper header.
 * @retval false Alignment or capacity is invalid.
 * @pre @p arena may carry any object-pointer representation.
 * @pre @p arena_cap may carry any representable size.
 * @post No arena byte is read or modified.
 * @post The result depends only on the supplied address and capacity.
 * @note Pure and thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL static bool internal_arena_valid(uint8_t* arena, size_t arena_cap)
{
  uintptr_t arena_address = 0U;
  static_assert(sizeof(arena_address) == sizeof(arena),
                "uintptr_t must preserve pointer representation");
  (void)memcpy((void*)&arena_address, (const void*)&arena, sizeof(arena_address));
  return ((arena_address % (uintptr_t)k_comic_wrap_align) == 0U) &&
         (arena_cap > (size_t)k_wrap_hdr_bytes);
}

/**
 * @brief Reject nested wrappers and open the decoded inner comic.
 * @details Installs the arena-resident memory-reader descriptor only after
 *          confirming the decoded payload is not another gzip or XZ stream.
 * @param[in,out] c Comic handle to initialise.
 * @param[in] payload Decoded inner-container bytes.
 * @param[in] inner_len Decoded byte count.
 * @param[out] pages Caller-owned page table.
 * @param[in] page_cap Capacity of @p pages.
 * @param[out] names Caller-owned page-name arena.
 * @param[in] names_cap Capacity of @p names.
 * @param[in,out] arena Wrapper arena whose prefix stores the reader descriptor.
 * @return Error code from nesting validation or ::comic_open.
 * @retval k_ra8_ok Inner comic opened successfully.
 * @retval k_ra8_err_decomp_depth A second compression wrapper was detected.
 * @retval k_ra8_err_* Inner comic detection or validation failed.
 * @pre @p payload addresses @p inner_len decoded bytes in @p arena.
 * @pre @p arena has room for the aligned ::unarch_mem_t descriptor.
 * @post On success @p c reads the decoded payload through the arena descriptor.
 * @post On error no nested wrapper is decoded.
 * @note Not thread-safe; the caller owns all supplied storage.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_open_unwrapped(comic_t*      c,
                                         uint8_t*      payload,
                                         size_t        inner_len,
                                         comic_page_t* pages,
                                         uint32_t      page_cap,
                                         char*         names,
                                         uint32_t      names_cap,
                                         uint8_t*      arena)
{
  /* Nesting bomb: a wrapper inside the wrapper is rejected before any inner
   * decode -- the one reachable depth bound in this pipeline. Both probes are
   * read into locals rather than tested in place, and the whole helper has a
   * single exit. */
  const bool inner_gzip = unarch_gzip_magic(payload, inner_len);
  const bool inner_xz   = unarch_xz_magic(payload, inner_len);
  ra8_err_t  err        = k_ra8_err_decomp_depth;
  if (!inner_gzip) {
    if (!inner_xz) {
      /* The flat-memory descriptor lives at the arena start so it out-lives
       * this call: page extraction keeps reading through it. */
      void* const         header_slot = arena;
      unarch_mem_t* const memory      = (unarch_mem_t*)header_slot;
      memory->base                    = payload;
      memory->len                     = inner_len;
      err                             = comic_open(c,
                                                   unarch_mem_read,
                                                   memory,
                                                   (uint64_t)inner_len,
                                                   pages,
                                                   page_cap,
                                                   names,
                                                   names_cap);
    }
  }
  return err;
}

ra8_err_t comic_open_wrapped(comic_t*      c,
                             comic_read_fn read,
                             void*         ctx,
                             uint64_t      size,
                             comic_page_t* pages,
                             uint32_t      page_cap,
                             char*         names,
                             uint32_t      names_cap,
                             uint8_t*      arena,
                             size_t        arena_cap,
                             void*         xz_scratch,
                             uint32_t      xz_scratch_len)
{
  /** @brief Log tag for wrapped-open diagnostics. */
  static const char* const s_tag_wrap = "comic_wrap";
  RA8_CHECK_NULL_PTR(c, s_tag_wrap, "wrapped open: null c");
  RA8_CHECK_NULL_PTR(read, s_tag_wrap, "wrapped open: null read");
  RA8_CHECK_NULL_PTR(arena, s_tag_wrap, "wrapped open: null arena");
  if (size == 0U) {
    return k_ra8_err_invalid_size;
  }
  uint8_t      sig[k_comic_magic_len] = {};
  const size_t got                    = read(ctx, 0U, sig, sizeof(sig));
  const bool   is_gzip                = unarch_gzip_magic(sig, got);
  const bool   is_xz                  = unarch_xz_magic(sig, got);
  if (!is_gzip && !is_xz) {
    /* Not a wrapper: the ordinary container detect owns it. */
    return comic_open(c, read, ctx, size, pages, page_cap, names, names_cap);
  }
  if (!internal_arena_valid(arena, arena_cap)) {
    return k_ra8_err_invalid_size;
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
  return internal_open_unwrapped(c, payload, inner_len, pages, page_cap, names, names_cap, arena);
}
