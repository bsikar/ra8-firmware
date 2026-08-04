/**
 * @file ra8_unarch_tar.c
 * @brief Streaming tar walker: open / next / read over the shared read seam.
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * The block-chain half of the clean-room tar reader (`ra8_unarch_tar.h`);
 * the untrusted-byte field parsers live in `ra8_unarch_tar_fields.c`. One
 * ::ra8_unarch_tar_next call consumes a member's bounded pax / GNU meta
 * prelude and its real header, charging the walker's embedded
 * decompression budget one iteration per examined block and one entry per
 * returned member -- so the whole enumeration, not just one call, is
 * policy-bounded. All offset arithmetic is overflow-checked so a hostile
 * size field cannot wrap `next_off` backwards (the walk strictly
 * advances or dies).
 *
 * @since Version 0.1.0
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "ra8_unarch_tar.h"

#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_unarch_tar_internal.h"

/** @brief Log tag for tar-walker diagnostics. */
static const char* const s_tag_tar = "ra8_unarch_tar";

/* The streaming ustar/pax/GNU walker is a dense sequential validator (per-block
 * type dispatch, meta-prelude handling, bounded reads): the walk and read
 * bodies clear clang-tidy's statement/nesting/cognitive thresholds while each
 * stays within the 60-line NASA Rule 4 gate. Same disposition as the other
 * decode state machines in the tree (ra8_jpeg_sw_encode, ra8_rmac_phy). */
/* NOLINTBEGIN(readability-function-size,readability-function-cognitive-complexity) */

/**
 * @struct tar_meta_t
 * @brief Overrides accumulated over one member's pax / GNU meta prelude.
 * @details Reset at the start of every ::ra8_unarch_tar_next; the real
 *          header consumes whatever the prelude established (pax `path` /
 *          `size`, GNU longname).
 * @invariant `name_len <= name_cap` after every update.
 * @see ra8_unarch_tar_next()
 * @since Version 0.1.0
 */
typedef struct {
  char*    name_buf;  /**< Caller name buffer (clamp target).     */
  uint16_t name_cap;  /**< Caller name buffer capacity.           */
  uint16_t name_len;  /**< Name bytes established so far.         */
  bool     have_path; /**< A pax path / GNU longname was applied. */
  uint64_t size_ovr;  /**< pax size override value.               */
  bool     have_size; /**< A pax size override was applied.       */
} tar_meta_t;

bool ra8_unarch_tar_probe(const uint8_t* block, size_t len)
{
  if (block == nullptr) {
    return false;
  }
  if (len < (size_t)k_ra8_unarch_tar_block) {
    return false;
  }
  if (!ra8_unarch_tar_magic_ok(block)) {
    return false;
  }
  return ra8_unarch_tar_checksum_ok(block);
}

ra8_err_t ra8_unarch_tar_open(ra8_unarch_tar_t*          t,
                              ra8_unarch_read_fn         read,
                              void*                      ctx,
                              uint64_t                   size,
                              const ra8_decomp_limits_t* limits)
{
  RA8_CHECK_NULL_PTR(t, s_tag_tar, "open: null t");
  *t = (ra8_unarch_tar_t){};
  RA8_CHECK_NULL_PTR((const void*)read, s_tag_tar, "open: null read");
  if (size < (uint64_t)k_ra8_unarch_tar_block) {
    return k_ra8_err_invalid_size;
  }
  const ra8_err_t berr = ra8_decomp_budget_init(&t->budget, limits);
  if (berr != k_ra8_ok) {
    return berr;
  }
  uint8_t      block[k_ra8_unarch_tar_block] = {};
  const size_t got                           = read(ctx, 0U, block, sizeof(block));
  if (got != sizeof(block)) {
    return k_ra8_err_invalid_size;
  }
  if (!ra8_unarch_tar_probe(block, sizeof(block))) {
    return k_ra8_err_not_supported;
  }
  t->read = read;
  t->ctx  = ctx;
  t->size = size;
  t->live = true;
  return k_ra8_ok;
}

/**
 * @brief Read one full header block at @p off, bounds-checked.
 * @details A block that would overrun the archive or a short read from the
 *          backing rejects the member (truncation is hostile input).
 * @param[in]  t     Live walker.
 * @param[in]  off   Absolute block offset.
 * @param[out] block Destination (::k_ra8_unarch_tar_block bytes).
 * @return ra8_err_t status.
 * @retval k_ra8_ok                    Block read whole.
 * @retval k_ra8_err_validation_failed Partial block or short read.
 * @pre `t->live` and `off < t->size` (caller-checked).
 * @pre @p block holds a full block of writable bytes.
 * @post On k_ra8_ok the block is fully populated.
 * @post On any error the member must be rejected.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t s_read_block(const ra8_unarch_tar_t* t, uint64_t off, uint8_t* block)
{
  const uint64_t need = (uint64_t)k_ra8_unarch_tar_block;
  if (off > (t->size - need)) {
    return k_ra8_err_validation_failed; /* partial trailing block */
  }
  const size_t got = t->read(t->ctx, off, block, (size_t)need);
  if ((uint64_t)got != need) {
    return k_ra8_err_validation_failed; /* backing truncated */
  }
  return k_ra8_ok;
}

/**
 * @brief Compute the offset of the block after a member, overflow-checked.
 * @details `next = off + block + ceil(dsize / block) * block`. Any step
 *          that would wrap uint64 rejects the member -- a hostile size
 *          field must not move the walk backwards.
 * @param[in]  off   Member header offset.
 * @param[in]  dsize Member data length in bytes.
 * @param[out] next  Receives the following block's offset.
 * @return ra8_err_t status.
 * @retval k_ra8_ok                    @p next computed.
 * @retval k_ra8_err_validation_failed The arithmetic would overflow.
 * @pre @p next is a caller-owned local (non-NULL).
 * @pre @p off is a real block offset (caller-validated).
 * @post On k_ra8_ok, `*next > off`.
 * @post On any error `*next` is unspecified and unused.
 * @note Thread-safe: pure computation.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t s_next_off(uint64_t off, uint64_t dsize, uint64_t* next)
{
  const uint64_t blk = (uint64_t)k_ra8_unarch_tar_block;
  if (dsize > (UINT64_MAX - (blk - 1U))) {
    return k_ra8_err_validation_failed; /* rounding would wrap */
  }
  const uint64_t blocks = (dsize + (blk - 1U)) / blk;
  if (blocks > ((UINT64_MAX / blk) - 1U)) {
    return k_ra8_err_validation_failed; /* span would wrap */
  }
  const uint64_t span = (blocks + 1U) * blk;
  if (off > (UINT64_MAX - span)) {
    return k_ra8_err_validation_failed; /* next offset would wrap */
  }
  *next = off + span;
  return k_ra8_ok;
}

/**
 * @var s_tar_meta_scratch
 * @brief File-scope scratch for one pax / GNU-longname meta data area.
 * @details Sized to ::k_ra8_unarch_tar_pax_max. Held here rather than on the
 *          stack so s_meta_consume keeps its frame under the 2 KiB first-party
 *          budget; only the freshly-read `dsize` bytes are ever consumed, so no
 *          per-call clear is needed.
 * @note Not thread-safe; the tar walker is single-threaded and non-reentrant
 *       (see ra8_unarch_tar_next()), and this buffer is overwritten per block.
 * @warning Do not read past the byte count returned by the backing read.
 * @since Version 0.1.0
 */
static uint8_t s_tar_meta_scratch[k_ra8_unarch_tar_pax_max];

/**
 * @brief Consume one pax / GNU-longname meta block's data area.
 * @details Bounds the data to ::k_ra8_unarch_tar_pax_max, reads it whole,
 *          and applies it: a pax header contributes `path` / `size`
 *          overrides, a longname contributes the (NUL-stripped) name.
 *          Skipped meta kinds ('g', 'K') consume nothing here.
 * @param[in]     t     Live walker.
 * @param[in]     type  The block's normalised class (pax or longname).
 * @param[in]     doff  Absolute offset of the meta data area.
 * @param[in]     dsize Meta data length in bytes.
 * @param[in,out] meta  Override accumulator to update.
 * @return ra8_err_t status.
 * @retval k_ra8_ok                    Meta applied.
 * @retval k_ra8_err_validation_failed Oversized, truncated, or malformed
 *                                     meta data.
 * @pre @p type is ::k_ra8_tar_type_pax or ::k_ra8_tar_type_longname.
 * @pre `doff + dsize` was bounds-checked against the archive by the caller.
 * @post On k_ra8_ok the accumulator reflects the meta content.
 * @post On any error the member must be rejected.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t s_meta_consume(const ra8_unarch_tar_t* t,
                                ra8_tar_type_t          type,
                                uint64_t                doff,
                                uint64_t                dsize,
                                tar_meta_t*             meta)
{
  if (dsize > (uint64_t)k_ra8_unarch_tar_pax_max) {
    return k_ra8_err_validation_failed; /* meta flood: reject the member */
  }
  uint8_t* const data = s_tar_meta_scratch;
  const size_t   got  = t->read(t->ctx, doff, data, (size_t)dsize);
  if ((uint64_t)got != dsize) {
    return k_ra8_err_validation_failed; /* backing truncated */
  }
  if (type == k_ra8_tar_type_pax) {
    return ra8_unarch_tar_pax_parse(data,
                                    (size_t)dsize,
                                    meta->name_buf,
                                    meta->name_cap,
                                    &meta->name_len,
                                    &meta->have_path,
                                    &meta->size_ovr,
                                    &meta->have_size);
  }
  /* GNU longname: the data is the member name, usually NUL-padded. */
  size_t nlen = (size_t)dsize;
  while (nlen > 0U) { /* bound: dsize */
    if (data[nlen - 1U] != 0U) {
      break;
    }
    nlen -= 1U;
  }
  const size_t want = (nlen > (size_t)meta->name_cap) ? (size_t)meta->name_cap : nlen;
  if (want > 0U) {
    (void)memcpy(meta->name_buf, data, want);
  }
  meta->name_len  = (uint16_t)want;
  meta->have_path = true;
  return k_ra8_ok;
}

/**
 * @brief Assemble a ustar member name (prefix + '/' + name), clamped.
 * @details Used when no pax path / longname override exists. Both fields
 *          are NUL-terminated-or-full per the ustar layout; a non-empty
 *          prefix contributes `prefix '/'` before the name.
 * @param[in]     block The member's header block.
 * @param[in,out] meta  Accumulator whose name buffer receives the result.
 * @pre @p block is a full validated header block.
 * @pre `meta->name_buf` holds `meta->name_cap` writable bytes when > 0.
 * @post `meta->name_len` is the clamped assembled length.
 * @post Only the name fields of @p meta change.
 * @note Thread-safe: writes only the caller's accumulator.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void s_ustar_name(const uint8_t* block, tar_meta_t* meta)
{
  const void*  nz    = memchr(&block[k_ra8_tar_off_name], 0, (size_t)k_ra8_tar_len_name);
  const size_t n_len = (nz != nullptr) ? (size_t)((const uint8_t*)nz - &block[k_ra8_tar_off_name])
                                       : (size_t)k_ra8_tar_len_name;
  const void*  pz    = memchr(&block[k_ra8_tar_off_prefix], 0, (size_t)k_ra8_tar_len_prefix);
  const size_t p_len = (pz != nullptr) ? (size_t)((const uint8_t*)pz - &block[k_ra8_tar_off_prefix])
                                       : (size_t)k_ra8_tar_len_prefix;
  size_t       used  = 0U;
  if (p_len > 0U) {
    const size_t pw = (p_len > (size_t)meta->name_cap) ? (size_t)meta->name_cap : p_len;
    if (pw > 0U) {
      (void)memcpy(meta->name_buf, &block[k_ra8_tar_off_prefix], pw);
    }
    used = pw;
    if (used < (size_t)meta->name_cap) {
      meta->name_buf[used] = '/';
      used += 1U;
    }
  }
  const size_t room = (size_t)meta->name_cap - used;
  const size_t nw   = (n_len > room) ? room : n_len;
  if (nw > 0U) {
    (void)memcpy(&meta->name_buf[used], &block[k_ra8_tar_off_name], nw);
  }
  meta->name_len = (uint16_t)(used + nw);
}

/**
 * @brief Validate one header block and decode its size / type / next offset.
 * @details The per-block core of the walk: checksum, magic, numeric size
 *          field, and the overflow-checked advance. Shared by the meta
 *          prelude and the real member.
 * @param[in]  block The header block to decode.
 * @param[in]  off   Its absolute offset.
 * @param[out] type  Receives the normalised typeflag class.
 * @param[out] dsize Receives the declared data length.
 * @param[out] next  Receives the following block's offset.
 * @return ra8_err_t status.
 * @retval k_ra8_ok                    Header decoded.
 * @retval k_ra8_err_validation_failed Checksum / magic / size failure.
 * @pre @p block is a full block read by ::s_read_block.
 * @pre The out-pointers are caller-owned locals.
 * @post On k_ra8_ok every output is populated.
 * @post On any error the member must be rejected.
 * @note Thread-safe: pure computation over the block.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t s_decode_header(const uint8_t*  block,
                                 uint64_t        off,
                                 ra8_tar_type_t* type,
                                 uint64_t*       dsize,
                                 uint64_t*       next)
{
  if (!ra8_unarch_tar_checksum_ok(block)) {
    return k_ra8_err_validation_failed;
  }
  if (!ra8_unarch_tar_magic_ok(block)) {
    return k_ra8_err_validation_failed;
  }
  const ra8_err_t serr =
    ra8_unarch_tar_num(&block[k_ra8_tar_off_size], (size_t)k_ra8_tar_len_size, dsize);
  if (serr != k_ra8_ok) {
    return serr;
  }
  *type = ra8_unarch_tar_classify(block[k_ra8_tar_off_type]);
  return s_next_off(off, *dsize, next);
}

/**
 * @brief Finish decoding the real member header into the caller entry.
 * @details Applies the accumulated overrides (pax size wins over the
 *          header field and re-derives the data span; pax path / longname
 *          wins over the ustar name fields), bounds the data area against
 *          the archive and the policy's output cap, and charges the entry
 *          budget.
 * @param[in,out] t     Live walker (budget charged).
 * @param[in]     block The member's header block.
 * @param[in]     off   The member header's absolute offset.
 * @param[in]     type  The member's normalised class.
 * @param[in]     dsize The header-declared data length.
 * @param[in,out] meta  Accumulated overrides.
 * @param[out]    out   Entry to populate.
 * @return ra8_err_t status.
 * @retval k_ra8_ok                    Entry populated.
 * @retval k_ra8_err_validation_failed The data area overruns the archive
 *                                     or the advance would wrap.
 * @retval k_ra8_err_decomp_output_cap The member exceeds the policy cap.
 * @retval k_ra8_err_decomp_entries    The entry budget is exhausted.
 * @pre @p block passed ::s_decode_header.
 * @pre @p out was zeroed by the caller.
 * @post On k_ra8_ok, `out->next_off > off`.
 * @post On any error @p out is re-zeroed by the caller's error path.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t s_finish_member(ra8_unarch_tar_t*       t,
                                 const uint8_t*          block,
                                 uint64_t                off,
                                 ra8_tar_type_t          type,
                                 uint64_t                dsize,
                                 tar_meta_t*             meta,
                                 ra8_unarch_tar_entry_t* out)
{
  uint64_t msize = dsize;
  if (meta->have_size) {
    msize = meta->size_ovr; /* pax size record overrides the header field */
  }
  uint64_t        next = 0U;
  const ra8_err_t aerr = s_next_off(off, msize, &next);
  if (aerr != k_ra8_ok) {
    return aerr;
  }
  const uint64_t doff = off + (uint64_t)k_ra8_unarch_tar_block;
  if (msize > (t->size - doff)) {
    return k_ra8_err_validation_failed; /* lying size: data overruns archive */
  }
  const ra8_err_t derr = ra8_decomp_check_declared(&t->budget.limits, msize, msize);
  if (derr != k_ra8_ok) {
    return derr;
  }
  const ra8_err_t eerr = ra8_decomp_budget_charge_entry(&t->budget);
  if (eerr != k_ra8_ok) {
    return eerr;
  }
  if (!meta->have_path) {
    s_ustar_name(block, meta);
  }
  out->data_off = doff;
  out->size     = msize;
  out->next_off = next;
  out->name_len = meta->name_len;
  out->is_file  = (type == k_ra8_tar_type_file) ? 1U : 0U;
  out->is_dir   = (type == k_ra8_tar_type_dir) ? 1U : 0U;
  return k_ra8_ok;
}

ra8_err_t ra8_unarch_tar_next(ra8_unarch_tar_t*       t,
                              uint64_t                off,
                              char*                   name_buf,
                              uint16_t                name_cap,
                              ra8_unarch_tar_entry_t* out)
{
  RA8_CHECK_NULL_PTR(t, s_tag_tar, "next: null t");
  RA8_CHECK_NULL_PTR(out, s_tag_tar, "next: null out");
  *out = (ra8_unarch_tar_entry_t){};
  if (!t->live) {
    return k_ra8_err_invalid_state;
  }
  if (name_cap > 0U) {
    RA8_CHECK_NULL_PTR(name_buf, s_tag_tar, "next: null name_buf");
  }
  tar_meta_t meta = {.name_buf = name_buf, .name_cap = name_cap};
  uint64_t   cur  = off;
  for (uint32_t m = 0U; m <= (uint32_t)k_ra8_unarch_tar_meta_max; ++m) { /* bound: meta cap */
    const ra8_err_t ierr = ra8_decomp_budget_charge_iter(&t->budget);
    if (ierr != k_ra8_ok) {
      return ierr;
    }
    if (cur >= t->size) {
      return k_ra8_err_not_found; /* clean EOF without an end marker */
    }
    uint8_t         block[k_ra8_unarch_tar_block] = {};
    const ra8_err_t rerr                          = s_read_block(t, cur, block);
    if (rerr != k_ra8_ok) {
      return rerr;
    }
    if (ra8_unarch_tar_block_zero(block)) {
      return k_ra8_err_not_found; /* end-of-archive marker */
    }
    ra8_tar_type_t  type  = k_ra8_tar_type_other;
    uint64_t        dsize = 0U;
    uint64_t        next  = 0U;
    const ra8_err_t herr  = s_decode_header(block, cur, &type, &dsize, &next);
    if (herr != k_ra8_ok) {
      return herr;
    }
    if ((type == k_ra8_tar_type_pax) || (type == k_ra8_tar_type_longname)) {
      const uint64_t doff = cur + (uint64_t)k_ra8_unarch_tar_block;
      if (dsize > (t->size - doff)) {
        return k_ra8_err_validation_failed; /* meta data overruns archive */
      }
      const ra8_err_t merr = s_meta_consume(t, type, doff, dsize, &meta);
      if (merr != k_ra8_ok) {
        return merr;
      }
      cur = next;
      continue;
    }
    if (type == k_ra8_tar_type_meta) {
      cur = next; /* skipped meta ('g' global, 'K' longlink) */
      continue;
    }
    const ra8_err_t ferr = s_finish_member(t, block, cur, type, dsize, &meta, out);
    if (ferr != k_ra8_ok) {
      *out = (ra8_unarch_tar_entry_t){};
      return ferr;
    }
    return k_ra8_ok;
  }
  return k_ra8_err_validation_failed; /* meta prelude longer than the cap */
}

ra8_err_t ra8_unarch_tar_read(const ra8_unarch_tar_t*       t,
                              const ra8_unarch_tar_entry_t* ent,
                              uint8_t*                      buf,
                              size_t                        cap,
                              size_t*                       got)
{
  RA8_CHECK_NULL_PTR(t, s_tag_tar, "read: null t");
  RA8_CHECK_NULL_PTR(ent, s_tag_tar, "read: null ent");
  RA8_CHECK_NULL_PTR(buf, s_tag_tar, "read: null buf");
  RA8_CHECK_NULL_PTR(got, s_tag_tar, "read: null got");
  *got = 0U;
  if (!t->live) {
    return k_ra8_err_invalid_state;
  }
  if (ent->is_file == 0U) {
    return k_ra8_err_not_supported;
  }
  if ((uint64_t)cap < ent->size) {
    return k_ra8_err_no_mem;
  }
  if (ent->data_off > t->size) {
    return k_ra8_err_invalid_size; /* forged entry: data area outside archive */
  }
  if (ent->size > (t->size - ent->data_off)) {
    return k_ra8_err_invalid_size; /* forged entry: data area overruns archive */
  }
  if (ent->size == 0U) {
    return k_ra8_ok; /* empty member: nothing to fetch */
  }
  const size_t n = t->read(t->ctx, ent->data_off, buf, (size_t)ent->size);
  if ((uint64_t)n != ent->size) {
    return k_ra8_err_invalid_size; /* backing truncated */
  }
  *got = n;
  return k_ra8_ok;
}
/* NOLINTEND(readability-function-size,readability-function-cognitive-complexity) */
