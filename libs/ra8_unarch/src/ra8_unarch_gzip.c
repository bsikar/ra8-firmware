/**
 * @file ra8_unarch_gzip.c
 * @brief RFC 1952 gzip member decoder over the vendored miniz tinfl core.
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * See `ra8_unarch_gzip.h` for the contract. The TU is organised as a small
 * cursor-tracked byte source over the shared read seam (exact reads,
 * running CRC32 for the optional FHCRC header check), a fail-closed header
 * parser, a windowed `tinfl_decompress` loop that charges the unified
 * decompression budget every pass, and a trailer verifier. The DEFLATE
 * decompressor state (~11 KiB) lives in one module-static arena rather
 * than the stack (single-client, zero heap -- the same idiom as the XZ
 * pool), so the reader task's stack budget is untouched.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since Version 0.1.0
 *
 */
#include "ra8_unarch_gzip.h"

#include <string.h>

#include "miniz.h"
#include "ra8_attributes.h"
#include "ra8_check.h"

/** @brief Log tag for gzip-decoder diagnostics. */
static const char* const s_tag_gz = "ra8_unarch_gzip";

/* The gzip member + windowed-inflate state machines are dense sequential
 * validators: many fail-closed guards and a bounded refill loop push several
 * bodies past clang-tidy's statement/nesting/cognitive thresholds while each
 * stays within the 60-line NASA Rule 4 gate. Same disposition as the other
 * decode state machines in the tree (ra8_jpeg_sw_encode, ra8_rmac_phy). */
/* NOLINTBEGIN(readability-function-size,readability-function-cognitive-complexity) */

/**
 * @enum gz_grammar_t
 * @brief RFC 1952 constants and this decoder's loop windows.
 * @since Version 0.1.0
 */
typedef enum : uint32_t {
  k_gz_id1          = 0x1FU,       /**< Magic byte 0.                        */
  k_gz_id2          = 0x8BU,       /**< Magic byte 1.                        */
  k_gz_cm_deflate   = 8U,          /**< The only defined compression method. */
  k_gz_flg_fhcrc    = 0x02U,       /**< Header CRC16 present.                */
  k_gz_flg_fextra   = 0x04U,       /**< Extra field present.                 */
  k_gz_flg_fname    = 0x08U,       /**< Original file name present.          */
  k_gz_flg_fcomment = 0x10U,       /**< Comment present.                     */
  k_gz_flg_reserved = 0xE0U,       /**< Reserved bits: must be zero.         */
  k_gz_hdr_fixed    = 10U,         /**< Fixed header length in bytes.        */
  k_gz_xlen_bytes   = 2U,          /**< FEXTRA length-field size.            */
  k_gz_fhcrc_bytes  = 2U,          /**< FHCRC field size.                    */
  k_gz_trailer_len  = 8U,          /**< CRC32 + ISIZE trailer length.        */
  k_gz_idx_id1      = 0U,          /**< Header index of ID1.                 */
  k_gz_idx_id2      = 1U,          /**< Header index of ID2.                 */
  k_gz_idx_cm       = 2U,          /**< Header index of CM.                  */
  k_gz_idx_flg      = 3U,          /**< Header index of FLG.                 */
  k_gz_in_chunk     = 512U,        /**< Input refill window per pass.        */
  k_gz_out_window   = 4096U,       /**< Output window per pass (charge
                                     *   granularity for bomb detection).     */
  k_gz_shift_byte   = 8U,          /**< Bits per byte for LE assembly.        */
  k_gz_crc16_mask   = 0xFFFFU,     /**< FHCRC is the low 16 CRC32 bits.       */
  k_gz_isize_mask   = 0xFFFFFFFFU, /**< ISIZE is the payload length mod 2^32. */
} gz_grammar_t;

/**
 * @struct gz_src_t
 * @brief Cursor-tracked exact-read byte source with a running header CRC.
 * @details Wraps the shared read seam: `pos` is the next unconsumed byte;
 *          every consumed header byte is folded into `crc` while
 *          `track_crc` is set so the optional FHCRC check needs no
 *          re-read.
 * @invariant `pos <= size` between operations.
 * @see ra8_unarch_gzip_unwrap()
 * @since Version 0.1.0
 */
typedef struct {
  ra8_unarch_read_fn read;      /**< Byte reader over the member.       */
  void*              ctx;       /**< Context for @ref read.             */
  uint64_t           size;      /**< Member length in bytes.            */
  uint64_t           pos;       /**< Next unconsumed byte offset.       */
  uint32_t           crc;       /**< Running CRC32 of consumed bytes.   */
  bool               track_crc; /**< Fold consumed bytes into @ref crc. */
} gz_src_t;

/** @brief The single-client tinfl state (too large for a task stack). */
static tinfl_decompressor s_gz_inflator;

/**
 * @brief Consume exactly @p n bytes from the source into @p dst.
 * @details A short read (EOF under the parser) fails closed; consumed
 *          bytes advance the cursor and, while tracking, the running CRC.
 * @param[in,out] s   Byte source.
 * @param[out]    dst Destination (@p n writable bytes).
 * @param[in]     n   Bytes required.
 * @return ra8_err_t status.
 * @retval k_ra8_ok                    Bytes consumed.
 * @retval k_ra8_err_validation_failed Truncated member.
 * @pre @p s was bound to a live reader.
 * @pre @p dst holds @p n writable bytes.
 * @post On k_ra8_ok the cursor advanced by exactly @p n.
 * @post On any error the parse must stop (fail-closed).
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_src_take(gz_src_t* s, uint8_t* dst, size_t n)
{
  if ((uint64_t)n > (s->size - s->pos)) {
    return k_ra8_err_validation_failed; /* runs past the member end */
  }
  const size_t got = s->read(s->ctx, s->pos, dst, n);
  if (got != n) {
    return k_ra8_err_validation_failed; /* backing truncated */
  }
  if (s->track_crc) {
    s->crc = (uint32_t)mz_crc32(s->crc, dst, n);
  }
  s->pos += (uint64_t)n;
  return k_ra8_ok;
}

/**
 * @brief Skip a NUL-terminated header string (FNAME / FCOMMENT), bounded.
 * @details Consumes bytes until the terminator; a string longer than
 *          ::k_ra8_unarch_gzip_str_max or an unterminated one (EOF first)
 *          rejects the member.
 * @param[in,out] s Byte source positioned at the string start.
 * @return ra8_err_t status.
 * @retval k_ra8_ok                    Terminator consumed.
 * @retval k_ra8_err_validation_failed Over-long or unterminated string.
 * @pre @p s was bound to a live reader.
 * @pre The header flag for this string was set.
 * @post On k_ra8_ok the cursor sits just past the NUL.
 * @post On any error the parse must stop (fail-closed).
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_skip_string(gz_src_t* s)
{
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_unarch_gzip_str_max; ++i) { /* bound: string cap */
    uint8_t         b    = 0U;
    const ra8_err_t terr = internal_src_take(s, &b, 1U);
    if (terr != k_ra8_ok) {
      return terr;
    }
    if (b == 0U) {
      return k_ra8_ok;
    }
  }
  return k_ra8_err_validation_failed; /* string exceeds the fail-closed cap */
}

/**
 * @brief Skip the FEXTRA subfield block (RFC 1952 XLEN + XLEN bytes), bounded.
 * @details Reads the 2-byte little-endian XLEN, then consumes exactly that
 *          many bytes in ::k_gz_in_chunk-sized pieces. XLEN is at most
 *          65535, so the loop is statically bounded.
 * @param[in,out] s Byte source positioned at the FEXTRA XLEN field.
 * @return ra8_err_t status.
 * @retval k_ra8_ok                    XLEN and its payload consumed.
 * @retval k_ra8_err_validation_failed XLEN or the subfield ran past the member.
 * @pre @p s was bound to a live reader.
 * @pre The FEXTRA header flag was set.
 * @post On k_ra8_ok the cursor sits just past the subfield block.
 * @post On any error the parse must stop (fail-closed).
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_skip_fextra(gz_src_t* s)
{
  uint8_t         xl[k_gz_xlen_bytes] = {};
  const ra8_err_t xerr                = internal_src_take(s, xl, sizeof(xl));
  if (xerr != k_ra8_ok) {
    return xerr;
  }
  uint32_t xlen = (uint32_t)xl[0] | ((uint32_t)xl[1] << (uint32_t)k_gz_shift_byte);
  uint8_t  skip[k_gz_in_chunk];
  while (xlen > 0U) { /* bound: xlen <= 65535, consumed in chunks */
    const size_t    n    = (xlen > (uint32_t)sizeof(skip)) ? sizeof(skip) : (size_t)xlen;
    const ra8_err_t serr = internal_src_take(s, skip, n);
    if (serr != k_ra8_ok) {
      return serr;
    }
    xlen -= (uint32_t)n;
  }
  return k_ra8_ok;
}

/**
 * @brief Parse the whole RFC 1952 header, leaving the cursor at the data.
 * @details Fixed fields first (magic, method, flags -- reserved bits
 *          rejected), then the optional fields in wire order: FEXTRA
 *          (bounded by its own length field), FNAME, FCOMMENT (bounded
 *          strings), and FHCRC (verified against the running CRC of every
 *          header byte before it).
 * @param[in,out] s Byte source at offset 0.
 * @return ra8_err_t status.
 * @retval k_ra8_ok                    Header consumed; cursor at DEFLATE.
 * @retval k_ra8_err_not_supported     Bad magic / method / reserved bits.
 * @retval k_ra8_err_checksum_mismatch FHCRC verification failed.
 * @retval k_ra8_err_validation_failed Truncated or over-long fields.
 * @pre @p s was bound with `pos == 0` and CRC tracking on.
 * @pre The member is at least the fixed header long (checked here).
 * @post On k_ra8_ok, `s->pos` is the DEFLATE stream offset.
 * @post On any error the member is rejected.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_parse_header(gz_src_t* s)
{
  uint8_t         fixed[k_gz_hdr_fixed] = {};
  const ra8_err_t ferr                  = internal_src_take(s, fixed, sizeof(fixed));
  if (ferr != k_ra8_ok) {
    return ferr;
  }
  if (fixed[k_gz_idx_id1] != (uint8_t)k_gz_id1) {
    return k_ra8_err_not_supported;
  }
  if (fixed[k_gz_idx_id2] != (uint8_t)k_gz_id2) {
    return k_ra8_err_not_supported;
  }
  if (fixed[k_gz_idx_cm] != (uint8_t)k_gz_cm_deflate) {
    return k_ra8_err_not_supported; /* only DEFLATE is defined */
  }
  const uint8_t flg = fixed[k_gz_idx_flg];
  if ((flg & (uint8_t)k_gz_flg_reserved) != 0U) {
    return k_ra8_err_not_supported; /* reserved bits: unknown format */
  }
  if ((flg & (uint8_t)k_gz_flg_fextra) != 0U) {
    const ra8_err_t xerr = internal_skip_fextra(s);
    if (xerr != k_ra8_ok) {
      return xerr;
    }
  }
  if ((flg & (uint8_t)k_gz_flg_fname) != 0U) {
    const ra8_err_t nerr = internal_skip_string(s);
    if (nerr != k_ra8_ok) {
      return nerr;
    }
  }
  if ((flg & (uint8_t)k_gz_flg_fcomment) != 0U) {
    const ra8_err_t cerr = internal_skip_string(s);
    if (cerr != k_ra8_ok) {
      return cerr;
    }
  }
  if ((flg & (uint8_t)k_gz_flg_fhcrc) != 0U) {
    const uint32_t  want                 = s->crc & (uint32_t)k_gz_crc16_mask;
    uint8_t         hc[k_gz_fhcrc_bytes] = {};
    const ra8_err_t herr                 = internal_src_take(s, hc, sizeof(hc));
    if (herr != k_ra8_ok) {
      return herr;
    }
    const uint32_t stored = (uint32_t)hc[0] | ((uint32_t)hc[1] << (uint32_t)k_gz_shift_byte);
    if (stored != want) {
      return k_ra8_err_checksum_mismatch;
    }
  }
  s->track_crc = false; /* header ends here; the payload CRC is separate */
  return k_ra8_ok;
}

/**
 * @struct gz_inflate_t
 * @brief Loop-carried state of the windowed DEFLATE inflation.
 * @details The input window is refilled from the source cursor; `win_ofs`
 *          marks the consumed part. `total` / `crc` accumulate the
 *          produced payload for the trailer check.
 * @invariant `win_ofs <= win_len <= sizeof(win)`.
 * @see ra8_unarch_gzip_unwrap()
 * @since Version 0.1.0
 */
typedef struct {
  gz_src_t*           src;                /**< Byte source (cursor at the window end). */
  uint8_t*            out;                /**< Destination arena.                      */
  size_t              out_cap;            /**< Arena capacity.                         */
  size_t              total;              /**< Payload bytes produced so far.          */
  uint32_t            crc;                /**< Running CRC32 of the produced payload.  */
  ra8_decomp_budget_t budget;             /**< Unified decompression budget.           */
  uint8_t             win[k_gz_in_chunk]; /**< Input window.                           */
  size_t              win_len;            /**< Valid bytes in the window.              */
  size_t              win_ofs;            /**< Consumed bytes in the window.           */
  bool                done;               /**< tinfl reported stream end.              */
} gz_inflate_t;

/**
 * @brief Run one refill + inflate pass of the DEFLATE loop.
 * @details Charges one iteration, refills the input window when drained,
 *          inflates into a bounded output window, folds the produced
 *          bytes into the payload CRC, and charges the output budget.
 *          Truncation (input exhausted before stream end), corruption,
 *          and an overrun arena each fail closed.
 * @param[in,out] st Inflation state.
 * @return ra8_err_t status of this pass.
 * @retval k_ra8_ok                    Pass complete; check `st->done`.
 * @retval k_ra8_err_no_mem            Arena full with the stream open.
 * @retval k_ra8_err_validation_failed Truncated / corrupt stream.
 * @retval k_ra8_err_decomp_*          A policy bound was breached.
 * @pre `st->src` is positioned inside the DEFLATE stream.
 * @pre `st->done` is false.
 * @post On k_ra8_ok the cursors advanced by the consumed/produced counts.
 * @post On any error the member is rejected.
 * @note Not thread-safe (module-static tinfl state).
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_inflate_pass(gz_inflate_t* st)
{
  const ra8_err_t ierr = ra8_decomp_budget_charge_iter(&st->budget);
  if (ierr != k_ra8_ok) {
    return ierr;
  }
  if (st->win_ofs == st->win_len) {
    const uint64_t remain = st->src->size - st->src->pos;
    const size_t   want   = (remain > (uint64_t)sizeof(st->win)) ? sizeof(st->win) : (size_t)remain;
    if (want == 0U) {
      return k_ra8_err_validation_failed; /* input exhausted mid-stream */
    }
    const ra8_err_t rerr = internal_src_take(st->src, st->win, want);
    if (rerr != k_ra8_ok) {
      return rerr;
    }
    st->win_len = want;
    st->win_ofs = 0U;
  }
  const bool      more_input = (st->src->pos < st->src->size);
  size_t          in_bytes   = st->win_len - st->win_ofs;
  const size_t    space      = st->out_cap - st->total;
  size_t          out_bytes  = (space > (size_t)k_gz_out_window) ? (size_t)k_gz_out_window : space;
  const mz_uint32 flags      = (mz_uint32)TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF |
                               (more_input ? (mz_uint32)TINFL_FLAG_HAS_MORE_INPUT : 0U);
  const tinfl_status ts      = tinfl_decompress(&s_gz_inflator,
                                                &st->win[st->win_ofs],
                                                &in_bytes,
                                                st->out,
                                                &st->out[st->total],
                                                &out_bytes,
                                                flags);
  st->win_ofs += in_bytes;
  if (out_bytes > 0U) {
    st->crc = (uint32_t)mz_crc32(st->crc, &st->out[st->total], out_bytes);
  }
  st->total += out_bytes;
  const ra8_err_t oerr =
    ra8_decomp_budget_charge_output(&st->budget, st->src->pos, (uint64_t)out_bytes);
  if (oerr != k_ra8_ok) {
    return oerr;
  }
  if (ts == TINFL_STATUS_DONE) {
    st->done = true;
    return k_ra8_ok;
  }
  if (ts == TINFL_STATUS_NEEDS_MORE_INPUT) {
    return k_ra8_ok; /* the next pass refills */
  }
  if (ts == TINFL_STATUS_HAS_MORE_OUTPUT) {
    if (st->total == st->out_cap) {
      return k_ra8_err_no_mem; /* arena full with the stream still open */
    }
    return k_ra8_ok; /* output window filled; the next pass continues */
  }
  return k_ra8_err_validation_failed; /* corrupt DEFLATE stream */
}

/**
 * @brief Verify the gzip trailer against the produced payload.
 * @details The trailer starts at the first unconsumed input byte:
 *          `win_len - win_ofs` leftover window bytes plus whatever the
 *          source still holds. CRC32 and ISIZE must both match, and
 *          nothing may follow the trailer.
 * @param[in] st The completed inflation state.
 * @return ra8_err_t status.
 * @retval k_ra8_ok                    Trailer verified, member ends cleanly.
 * @retval k_ra8_err_checksum_mismatch CRC32 or ISIZE mismatch.
 * @retval k_ra8_err_validation_failed Truncated trailer or trailing bytes.
 * @pre `st->done` is true (tinfl consumed the whole DEFLATE stream).
 * @pre The source cursor sits at the window's end.
 * @post No state is modified (pure verification).
 * @post On any error the member is rejected.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_verify_trailer(const gz_inflate_t* st)
{
  const uint64_t leftover    = (uint64_t)(st->win_len - st->win_ofs);
  const uint64_t trailer_pos = st->src->pos - leftover;
  const uint64_t remain      = st->src->size - trailer_pos;
  if (remain < (uint64_t)k_gz_trailer_len) {
    return k_ra8_err_validation_failed; /* truncated trailer */
  }
  if (remain > (uint64_t)k_gz_trailer_len) {
    return k_ra8_err_validation_failed; /* trailing bytes: no concatenation */
  }
  uint8_t      tr[k_gz_trailer_len] = {};
  const size_t got                  = st->src->read(st->src->ctx, trailer_pos, tr, sizeof(tr));
  if (got != sizeof(tr)) {
    return k_ra8_err_validation_failed; /* backing truncated */
  }
  uint32_t crc_stored   = 0U;
  uint32_t isize_stored = 0U;
  for (uint32_t i = 0U; i < 4U; ++i) { /* bound: fixed field width */
    crc_stored |= (uint32_t)tr[i] << ((uint32_t)k_gz_shift_byte * i);
    isize_stored |= (uint32_t)tr[i + 4U] << ((uint32_t)k_gz_shift_byte * i);
  }
  if (crc_stored != st->crc) {
    return k_ra8_err_checksum_mismatch;
  }
  if (isize_stored != ((uint32_t)st->total & (uint32_t)k_gz_isize_mask)) {
    return k_ra8_err_checksum_mismatch;
  }
  return k_ra8_ok;
}

bool ra8_unarch_gzip_magic(const uint8_t* sig, size_t sig_len)
{
  if (sig == nullptr) {
    return false;
  }
  if (sig_len < (size_t)k_ra8_unarch_gzip_sig_len) {
    return false;
  }
  if (sig[k_gz_idx_id1] != (uint8_t)k_gz_id1) {
    return false;
  }
  return sig[k_gz_idx_id2] == (uint8_t)k_gz_id2;
}

ra8_err_t ra8_unarch_gzip_unwrap(ra8_unarch_read_fn         read,
                                 void*                      ctx,
                                 uint64_t                   size,
                                 uint8_t*                   out,
                                 size_t                     out_cap,
                                 const ra8_decomp_limits_t* limits,
                                 size_t*                    out_len)
{
  RA8_CHECK_NULL_PTR((const void*)read, s_tag_gz, "unwrap: null read");
  RA8_CHECK_NULL_PTR(out, s_tag_gz, "unwrap: null out");
  RA8_CHECK_NULL_PTR(out_len, s_tag_gz, "unwrap: null out_len");
  *out_len = 0U;
  if (size == 0U) {
    return k_ra8_err_invalid_size;
  }
  if (out_cap == 0U) {
    return k_ra8_err_invalid_size;
  }
  gz_src_t src = {
    .read      = read,
    .ctx       = ctx,
    .size      = size,
    .pos       = 0U,
    .crc       = (uint32_t)MZ_CRC32_INIT,
    .track_crc = true,
  };
  const ra8_err_t herr = internal_parse_header(&src);
  if (herr != k_ra8_ok) {
    return herr;
  }
  gz_inflate_t st      = {};
  st.src               = &src;
  st.out               = out;
  st.out_cap           = out_cap;
  st.crc               = (uint32_t)MZ_CRC32_INIT;
  const ra8_err_t berr = ra8_decomp_budget_init(&st.budget, limits);
  if (berr != k_ra8_ok) {
    return berr;
  }
  tinfl_init(&s_gz_inflator);
  while (!st.done) { /* bound: every pass charges the policy iteration budget */
    const ra8_err_t perr = internal_inflate_pass(&st);
    if (perr != k_ra8_ok) {
      return perr;
    }
  }
  const ra8_err_t terr = internal_verify_trailer(&st);
  if (terr != k_ra8_ok) {
    return terr;
  }
  *out_len = st.total;
  return k_ra8_ok;
}
/* NOLINTEND(readability-function-size,readability-function-cognitive-complexity) */
