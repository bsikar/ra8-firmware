/**
 * @file ra8_unarch_xz.c
 * @brief Bounded XZ/LZMA2 decode wrapper over the vendored xz-embedded.
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * See `ra8_unarch_xz.h` for the contract. The session shape drives the SOUP
 * decoder in `XZ_PREALLOC` mode: begin installs the caller scratch as the
 * zero-heap allocation arena (`ra8_unarch_xz_pool.h`), reserves
 * `k_ra8_unarch_xz_state_reserve` of it for the decoder state, and hands the
 * remainder to xz-embedded as the maximum LZMA2 dictionary -- everything is
 * allocated once at init and a stream declaring a bigger dictionary fails
 * with `XZ_MEMLIMIT_ERROR` (mapped to `k_ra8_err_no_mem`) instead of
 * growing. The unwrap shape loops a session over the shared read seam in
 * fixed ::k_xz_chunk input windows, charging the unified decompression
 * budget every pass so a bomb or stuck stream terminates within policy.
 *
 * Every decision in this file is a single condition on purpose: the wrapper
 * fronts the most hostile input in the content path, so each bound is
 * tested independently (and carries no compound-decision MC/DC burden).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since Version 0.1.0
 *
 */
#include "ra8_unarch_xz.h"

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_unarch_xz_pool.h"
#include "xz_config.h"

/** @brief Log tag for XZ-wrapper diagnostics. */
static const char* const s_tag_xz = "ra8_unarch_xz";

/* The XZ session driver pumps the vendored xz-embedded stream decoder through a
 * bounded refill/flush loop with fail-closed guards on every return code: the
 * run and unwrap bodies clear clang-tidy's statement/nesting/cognitive
 * thresholds while staying within the 60-line NASA Rule 4 gate. Same
 * disposition as the other decode state machines (ra8_jpeg_sw_encode). */
/* NOLINTBEGIN(readability-function-size,readability-function-cognitive-complexity) */

/**
 * @enum xz_wrap_dims_t
 * @brief Fixed sizes for the unwrap input loop.
 * @details One ::k_xz_chunk stack window is refilled from the read seam per
 *          decode pass; small enough for the firmware stack budget, large
 *          enough that a policy-conformant stream finishes far inside the
 *          default iteration budget.
 * @since Version 0.1.0
 */
typedef enum : uint16_t {
  k_xz_chunk      = 512U,  /**< Input refill window per decode pass, bytes. */
  k_xz_out_window = 4096U, /**< Output window per decode pass, bytes; keeps
                            *   the budget charge granularity (and thus bomb
                            *   detection) within one window of the bound.   */
} xz_wrap_dims_t;

/**
 * @enum xz_magic_t
 * @brief The six XZ stream header magic bytes (`FD 37 7A 58 5A 00`).
 * @details Values from the .xz file format specification section 2.1.1.1;
 *          named per the no-magic-numbers rule.
 * @since Version 0.1.0
 */
typedef enum : uint8_t {
  k_xz_magic_b0 = 0xFDU, /**< Non-ASCII lead-in byte.      */
  k_xz_magic_b1 = 0x37U, /**< '7'.                         */
  k_xz_magic_b2 = 0x7AU, /**< 'z'.                         */
  k_xz_magic_b3 = 0x58U, /**< 'X'.                         */
  k_xz_magic_b4 = 0x5AU, /**< 'Z'.                         */
  k_xz_magic_b5 = 0x00U, /**< NUL terminator of the magic. */
} xz_magic_t;

bool ra8_unarch_xz_magic(const uint8_t* sig, size_t sig_len)
{
  if (sig == nullptr) {
    return false;
  }
  if (sig_len < (size_t)k_ra8_unarch_xz_sig_len) {
    return false;
  }
  const uint8_t want[k_ra8_unarch_xz_sig_len] = {
    (uint8_t)k_xz_magic_b0,
    (uint8_t)k_xz_magic_b1,
    (uint8_t)k_xz_magic_b2,
    (uint8_t)k_xz_magic_b3,
    (uint8_t)k_xz_magic_b4,
    (uint8_t)k_xz_magic_b5,
  };
  return memeq(sig, want, sizeof(want));
}

ra8_err_t
ra8_unarch_xz_stream_begin(ra8_unarch_xz_stream_t* xs, void* scratch, uint32_t scratch_len)
{
  RA8_CHECK_NULL_PTR(xs, s_tag_xz, "begin: null session");
  *xs = (ra8_unarch_xz_stream_t){};
  RA8_CHECK_NULL_PTR(scratch, s_tag_xz, "begin: null scratch");
  if (scratch_len <= (uint32_t)k_ra8_unarch_xz_state_reserve) {
    return k_ra8_err_invalid_size;
  }
  const ra8_err_t perr = ra8_unarch_xz_pool_install(scratch, scratch_len);
  if (perr != k_ra8_ok) {
    return perr;
  }
  /* The CRC lookup tables must exist before any other xz_* call; both
   * initialisers are idempotent, so calling per-session is safe. */
  xz_crc32_init();
  xz_crc64_init();
  const uint32_t dict_max = scratch_len - (uint32_t)k_ra8_unarch_xz_state_reserve;
  xs->dec                 = xz_dec_init(XZ_PREALLOC, dict_max);
  if (xs->dec == nullptr) {
    ra8_unarch_xz_pool_reset();
    return k_ra8_err_no_mem;
  }
  xs->live = true;
  return k_ra8_ok;
}

/**
 * @brief Map an xz-embedded return code onto the wrapper's error space.
 * @details The success codes (`XZ_OK`, `XZ_STREAM_END`) are handled by the
 *          caller before this runs; every remaining code is a bounded
 *          failure. `XZ_MEMLIMIT_ERROR` is the declared-dictionary breach;
 *          `XZ_FORMAT_ERROR` / `XZ_OPTIONS_ERROR` are non-XZ or
 *          unsupported-feature streams; `XZ_DATA_ERROR` / `XZ_BUF_ERROR`
 *          are corruption, truncation, or a stuck stream. `XZ_MEM_ERROR`
 *          and `XZ_UNSUPPORTED_CHECK` cannot occur in this build
 *          (`XZ_PREALLOC` allocates only at init; `XZ_DEC_ANY_CHECK` is
 *          off) and fall to the defensive default.
 * @param[in] ret xz-embedded return code (a failure code).
 * @return The bounded ::ra8_err_t for @p ret.
 * @retval k_ra8_err_no_mem            Dictionary over the scratch budget.
 * @retval k_ra8_err_not_supported     Not XZ / unsupported check or filter.
 * @retval k_ra8_err_validation_failed Corrupt, truncated, or stuck stream.
 * @pre @p ret is not `XZ_OK` / `XZ_STREAM_END` (caller-handled).
 * @pre The failed session will be ended by the caller.
 * @post No state is modified (pure mapping).
 * @post The result is never `k_ra8_ok`.
 * @note Thread-safe: pure mapping.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t s_xz_map_err(enum xz_ret ret)
{
  if (ret == XZ_MEMLIMIT_ERROR) {
    return k_ra8_err_no_mem;
  }
  if (ret == XZ_FORMAT_ERROR) {
    return k_ra8_err_not_supported;
  }
  if (ret == XZ_OPTIONS_ERROR) {
    return k_ra8_err_not_supported;
  }
  if (ret == XZ_DATA_ERROR) {
    return k_ra8_err_validation_failed;
  }
  if (ret == XZ_BUF_ERROR) {
    return k_ra8_err_validation_failed;
  }
  return k_ra8_err_validation_failed; /* GCOVR_EXCL_LINE -- XZ_MEM_ERROR /
                                       * XZ_UNSUPPORTED_CHECK are impossible in
                                       * this XZ_PREALLOC, no-ANY_CHECK build */
}

ra8_err_t ra8_unarch_xz_stream_run(ra8_unarch_xz_stream_t* xs,
                                   const uint8_t*          in,
                                   size_t                  in_len,
                                   size_t*                 in_used,
                                   uint8_t*                out,
                                   size_t                  out_cap,
                                   size_t*                 out_used,
                                   bool*                   end)
{
  RA8_CHECK_NULL_PTR(xs, s_tag_xz, "run: null session");
  RA8_CHECK_NULL_PTR(in_used, s_tag_xz, "run: null in_used");
  RA8_CHECK_NULL_PTR(out, s_tag_xz, "run: null out");
  RA8_CHECK_NULL_PTR(out_used, s_tag_xz, "run: null out_used");
  RA8_CHECK_NULL_PTR(end, s_tag_xz, "run: null end");
  *in_used  = 0U;
  *out_used = 0U;
  *end      = false;
  if (in_len > 0U) {
    RA8_CHECK_NULL_PTR(in, s_tag_xz, "run: null in with input");
  }
  if (!xs->live) {
    return k_ra8_err_invalid_state;
  }
  struct xz_buf b = {
    .in       = in,
    .in_pos   = 0U,
    .in_size  = in_len,
    .out      = out,
    .out_pos  = 0U,
    .out_size = out_cap,
  };
  const enum xz_ret ret = xz_dec_run(xs->dec, &b);
  *in_used              = b.in_pos;
  *out_used             = b.out_pos;
  if (ret == XZ_STREAM_END) {
    *end = true;
    return k_ra8_ok;
  }
  if (ret == XZ_OK) {
    return k_ra8_ok;
  }
  return s_xz_map_err(ret);
}

void ra8_unarch_xz_stream_end(ra8_unarch_xz_stream_t* xs)
{
  if (xs == nullptr) {
    return; /* teardown paths call unconditionally */
  }
  if (!xs->live) {
    return; /* never begun (or already ended): nothing to release */
  }
  xz_dec_end(xs->dec); /* frees are no-ops; the pool reset reclaims all */
  ra8_unarch_xz_pool_reset();
  xs->dec  = nullptr;
  xs->live = false;
}

/**
 * @brief Reject any NULL required argument to ::ra8_unarch_xz_unwrap.
 * @details Runs the mandatory null guards so the entry point stays within
 *          the function-size budget; scalar validation stays inline there.
 * @param[in] read    Byte reader over the stream.
 * @param[in] out     Destination arena.
 * @param[in] scratch Session scratch.
 * @param[in] out_len Decoded-length out-pointer.
 * @return ra8_err_t status.
 * @retval k_ra8_ok           Every required argument is non-NULL.
 * @retval k_ra8_err_null_ptr Some required argument was NULL.
 * @pre The caller forwards ::ra8_unarch_xz_unwrap's arguments unchanged.
 * @pre No argument is dereferenced before this returns k_ra8_ok.
 * @post On k_ra8_ok each checked pointer is safe to use.
 * @post On any error the reason is logged against ::s_tag_xz.
 * @note Thread-safe: reads only its pointer arguments.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t s_unwrap_reject_null(ra8_unarch_read_fn read,
                                      const uint8_t*     out,
                                      const void*        scratch,
                                      const size_t*      out_len)
{
  RA8_CHECK_NULL_PTR((const void*)read, s_tag_xz, "unwrap: null read");
  RA8_CHECK_NULL_PTR(out, s_tag_xz, "unwrap: null out");
  RA8_CHECK_NULL_PTR(scratch, s_tag_xz, "unwrap: null scratch");
  RA8_CHECK_NULL_PTR(out_len, s_tag_xz, "unwrap: null out_len");
  return k_ra8_ok;
}

/**
 * @struct xz_unwrap_state_t
 * @brief Loop-carried state of one unwrap: cursors, session, and budget.
 * @details Bundled so the refill/pass helpers stay under the function-size
 *          budget while sharing one coherent record.
 * @invariant `in_off <= size` and `total_out <= out_cap` between passes.
 * @see ra8_unarch_xz_unwrap()
 * @since Version 0.1.0
 */
typedef struct {
  ra8_unarch_read_fn     read;              /**< Byte reader over the stream.    */
  void*                  ctx;               /**< Context for @ref read.          */
  uint64_t               size;              /**< Stream length in bytes.         */
  uint8_t*               out;               /**< Destination arena.              */
  size_t                 out_cap;           /**< Arena capacity in bytes.        */
  uint64_t               in_off;            /**< Input bytes consumed so far.    */
  size_t                 total_out;         /**< Output bytes produced so far.   */
  ra8_unarch_xz_stream_t xs;                /**< Live decode session.            */
  ra8_decomp_budget_t    budget;            /**< Unified decompression budget.   */
  uint8_t                chunk[k_xz_chunk]; /**< Input refill window (one pass). */
} xz_unwrap_state_t;

/**
 * @brief Run one refill + decode pass of the unwrap loop.
 * @details Charges one iteration, refills the input window from the read
 *          seam at the current cursor, runs the session, advances both
 *          cursors, and charges the produced output against the budget.
 *          A zero-byte refill before the stream ended is a truncated
 *          backing (fail-closed); a full arena with the stream still open
 *          is an undersized arena.
 * @param[in,out] st  Unwrap state (live session, cursors in range).
 * @param[out]    end Receives true when the stream verified and ended.
 * @return ra8_err_t status of this pass.
 * @retval k_ra8_ok                    Pass complete; check @p end.
 * @retval k_ra8_err_no_mem            Arena full with the stream open, or
 *                                     the dictionary over budget.
 * @retval k_ra8_err_validation_failed Truncated / corrupt / stuck stream.
 * @retval k_ra8_err_not_supported     Unsupported format/check/filter.
 * @retval k_ra8_err_decomp_*          A policy bound was breached.
 * @pre `st->xs` is live and `st->in_off <= st->size`.
 * @pre `st->total_out <= st->out_cap`.
 * @post On k_ra8_ok the cursors advanced by the consumed/produced counts.
 * @post On any error the caller must end the session (not recoverable).
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t s_unwrap_pass(xz_unwrap_state_t* st, bool* end)
{
  const ra8_err_t ierr = ra8_decomp_budget_charge_iter(&st->budget);
  if (ierr != k_ra8_ok) {
    return ierr;
  }
  const uint64_t remain = st->size - st->in_off;
  const size_t   want   = (remain > (uint64_t)k_xz_chunk) ? (size_t)k_xz_chunk : (size_t)remain;
  size_t         got    = 0U;
  if (want > 0U) {
    got = st->read(st->ctx, st->in_off, st->chunk, want);
  }
  if (got > want) {
    got = want; /* clamp a misbehaving backing to the request */
  }
  if (got == 0U) {
    return k_ra8_err_validation_failed; /* backing truncated under the stream */
  }
  const size_t    space    = st->out_cap - st->total_out;
  const size_t    owin     = (space > (size_t)k_xz_out_window) ? (size_t)k_xz_out_window : space;
  size_t          used     = 0U;
  size_t          produced = 0U;
  const ra8_err_t rerr     = ra8_unarch_xz_stream_run(&st->xs,
                                                  st->chunk,
                                                  got,
                                                  &used,
                                                  &st->out[st->total_out],
                                                  owin,
                                                  &produced,
                                                  end);
  st->in_off += (uint64_t)used;
  st->total_out += produced;
  if (rerr != k_ra8_ok) {
    if (rerr == k_ra8_err_validation_failed) {
      if (st->total_out == st->out_cap) {
        return k_ra8_err_no_mem; /* stuck only because the arena is full */
      }
    }
    return rerr;
  }
  return ra8_decomp_budget_charge_output(&st->budget, st->in_off, (uint64_t)produced);
}

ra8_err_t ra8_unarch_xz_unwrap(ra8_unarch_read_fn         read,
                               void*                      ctx,
                               uint64_t                   size,
                               uint8_t*                   out,
                               size_t                     out_cap,
                               void*                      scratch,
                               uint32_t                   scratch_len,
                               const ra8_decomp_limits_t* limits,
                               size_t*                    out_len)
{
  const ra8_err_t nerr = s_unwrap_reject_null(read, out, scratch, out_len);
  if (nerr != k_ra8_ok) {
    return nerr;
  }
  *out_len = 0U;
  if (size == 0U) {
    return k_ra8_err_invalid_size;
  }
  if (out_cap == 0U) {
    return k_ra8_err_invalid_size;
  }
  xz_unwrap_state_t st = {};
  st.read              = read;
  st.ctx               = ctx;
  st.size              = size;
  st.out               = out;
  st.out_cap           = out_cap;
  const ra8_err_t berr = ra8_decomp_budget_init(&st.budget, limits);
  if (berr != k_ra8_ok) {
    return berr;
  }
  const ra8_err_t serr = ra8_unarch_xz_stream_begin(&st.xs, scratch, scratch_len);
  if (serr != k_ra8_ok) {
    return serr;
  }
  bool      end  = false;
  ra8_err_t perr = k_ra8_ok;
  while (!end) { /* bound: every pass charges the policy iteration budget */
    perr = s_unwrap_pass(&st, &end);
    if (perr != k_ra8_ok) {
      break;
    }
  }
  ra8_unarch_xz_stream_end(&st.xs);
  if (perr != k_ra8_ok) {
    return perr;
  }
  if (st.in_off < size) {
    return k_ra8_err_validation_failed; /* trailing bytes after the footer */
  }
  *out_len = st.total_out;
  return k_ra8_ok;
}
/* NOLINTEND(readability-function-size,readability-function-cognitive-complexity) */
