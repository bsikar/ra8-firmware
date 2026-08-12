/**
 * @file ra8_rar5.c
 * @brief RAR 5.0 LZ decoder driver: token loop, data filters, public entry.
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * The upper half of the clean-room RAR 5.0 unpacker (paired with the entropy
 * front-end in `ra8_rar5_tables.c`). It drives an LZ77 token loop over the
 * Huffman-decoded symbol stream, writing the reconstructed bytes straight into
 * the caller's output buffer (which is also the sliding window), then replays any
 * recorded data filters (delta / x86 / ARM) over their output ranges.
 *
 * @par Structure
 * - LZ: ::s_decode_token classifies one main symbol into literal / match /
 *   repeat-match / filter, copying matches with ::ra8_rar5_copy_match;
 *   ::s_decode_stream drives the block + token loops.
 * - Filters: ::s_apply_filters replays delta / x86 / ARM transforms post-decode.
 * - Entry: ::ra8_rar5_decompress validates its arguments, resets the state, and
 *   runs the stream to completion.
 *
 * The front-end (`ra8_rar5_tables.c`) supplies the four ::ra8_rar5_internal.h
 * primitives this driver calls -- ::ra8_rar5_get, ::ra8_rar5_decode_num,
 * ::ra8_rar5_read_block_header, ::ra8_rar5_read_tables -- and never calls back.
 *
 * @par Conformance note (deliberate, verifiable scope)
 * The LZ + Huffman container grammar follows the published RAR 5.0 format so a
 * genuine WinRAR-produced member decodes. The delta / x86 / ARM data filters are
 * implemented as reversible transforms self-consistent with this codec's writer;
 * byte-exact equivalence to WinRAR's filter output cannot be cross-checked here (no
 * free RAR compressor exists) and comic JPEG/PNG pages never carry these filters,
 * so that last equivalence is left to an owner-supplied real archive.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since Version 0.1.0
 *
 */
#include "ra8_rar5.h"

#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_rar5_internal.h"

/** @brief Log tag for RAR5 decoder diagnostics. */
static const char* const s_tag_rar5 = "ra8_rar5";

/* ---- little-endian scalar reads/writes ---------------------------------- */

/**
 * @brief Read a little-endian uint32 from four unaligned bytes.
 * @details memcpy-based so the source may sit at any alignment on a little-endian
 *          host/target.
 * @param[in] p Pointer to four readable bytes (non-NULL).
 * @return The decoded value.
 * @retval 0 When all four bytes are zero.
 * @pre @p p addresses at least four readable bytes.
 * @pre The host is little-endian.
 * @post No state is modified (pure read).
 * @post The result is a pure function of the four bytes.
 * @note Thread-safe: pure read.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static uint32_t s_rd_le32(const uint8_t* p)
{
  uint32_t v = 0U;
  (void)memcpy(&v, p, sizeof(v));
  return v;
}

/**
 * @brief Write a uint32 as four little-endian bytes.
 * @details memcpy-based so the destination may sit at any alignment.
 * @param[out] p Pointer to four writable bytes (non-NULL).
 * @param[in]  v Value to store.
 * @return Nothing.
 * @pre @p p addresses at least four writable bytes.
 * @pre The host is little-endian.
 * @post `p[0..4)` holds @p v in little-endian order.
 * @post No other state is modified.
 * @note Thread-safe: writes only through @p p.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void s_wr_le32(uint8_t* p, uint32_t v)
{
  (void)memcpy(p, &v, sizeof(v));
}

/* ---- LZ token decode ---------------------------------------------------- */

/**
 * @brief Convert an LZ length slot into a match length, reading any extra bits.
 * @details Slots 0-7 map to lengths 2-9 directly; higher slots add
 *          `(4 | slot&3) << (slot/4 - 1)` plus that many extra bits.
 * @param[in,out] st   Decoder state (non-NULL).
 * @param[in]     slot Length slot value.
 * @return The decoded match length.
 * @retval 2 For length slot 0.
 * @pre @p st is a bound decoder state.
 * @pre @p slot is a valid length slot.
 * @post `st->consumed` advanced by the extra-bit count.
 * @post The result is >= 2.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static uint32_t s_slot_to_length(ra8_rar5_state_t* st, uint32_t slot)
{
  uint32_t length = 2U;
  if (slot < 8U) {
    return length + slot;
  }
  const uint32_t lbits = (slot / 4U) - 1U;
  length += (4U | (slot & 3U)) << lbits;
  length += ra8_rar5_get(st, lbits);
  return length;
}

/**
 * @brief Decode an LZ match distance from the distance / low-distance tables.
 * @details Slots 0-3 give a direct distance; higher slots reconstruct the top bits
 *          from the slot and the remaining bits from either extra bits or the
 *          low-distance table.
 * @param[in,out] st Decoder state (non-NULL).
 * @return The decoded 1-based match distance.
 * @retval 1 For distance slot 0.
 * @pre @p st is a bound decoder state with the DD/LDD tables built.
 * @pre @p st scratch tables are usable.
 * @post `st->consumed` advanced past the distance code and extras.
 * @post The result is >= 1.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static uint64_t s_decode_distance(ra8_rar5_state_t* st)
{
  const uint32_t slot = ra8_rar5_decode_num(st, &st->dd);
  if (slot < 4U) {
    return (uint64_t)1U + slot;
  }
  const uint32_t dbits = (slot / 2U) - 1U;
  uint64_t       dist  = (uint64_t)1U + ((uint64_t)(2U | (slot & 1U)) << dbits);
  if (dbits < 4U) {
    return dist + ra8_rar5_get(st, dbits);
  }
  if (dbits > 4U) {
    dist += (uint64_t)ra8_rar5_get(st, dbits - 4U) << 4U;
  }
  dist += ra8_rar5_decode_num(st, &st->ldd);
  return dist;
}

/**
 * @brief Add the RAR5 distance-dependent bias to a match length.
 * @details Distances above 0x100 / 0x2000 / 0x40000 add 1 / 2 / 3 to the length.
 * @param[in] length Base match length.
 * @param[in] dist   Match distance.
 * @return The biased match length.
 * @retval length When @p dist <= 0x100.
 * @pre @p length is a decoded base length.
 * @pre @p dist is a decoded distance.
 * @post No state is modified (pure function).
 * @post The result is >= @p length.
 * @note Thread-safe: pure.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static uint32_t s_adjust_length(uint32_t length, uint64_t dist)
{
  uint32_t adjusted = length;
  if (dist > (uint64_t)k_r5_dist_th1) {
    adjusted += 1U;
    if (dist > (uint64_t)k_r5_dist_th2) {
      adjusted += 1U;
      if (dist > (uint64_t)k_r5_dist_th3) {
        adjusted += 1U;
      }
    }
  }
  return adjusted;
}

/**
 * @brief Push a fresh match distance onto the recent-distance ring.
 * @details Shifts the four remembered distances down by one and stores @p dist at the
 *          front, so a later remembered-distance symbol can reuse it.
 * @param[in,out] st   Decoder state (non-NULL).
 * @param[in]     dist Distance to remember.
 * @return Nothing.
 * @pre @p st is a bound decoder state.
 * @pre @p st::old_dist has ::k_ra8_rar5_old_dist entries.
 * @post `st->old_dist[0] == dist` and the older entries shift down.
 * @post The oldest remembered distance is discarded.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void s_push_dist(ra8_rar5_state_t* st, uint64_t dist)
{
  for (uint32_t i = (uint32_t)k_ra8_rar5_old_dist - 1U; i > 0U; --i) { /* bound: ring size */
    st->old_dist[i] = st->old_dist[i - 1U];
  }
  st->old_dist[0] = dist;
}

/** @brief Implementation of `ra8_rar5_copy_match()` -- self-overlapping back-copy. */
bool ra8_rar5_copy_match(uint8_t* out, size_t* out_pos, size_t unp, uint32_t length, uint64_t dist)
{
  size_t pos = *out_pos;
  if ((dist == 0U) || (dist > (uint64_t)pos)) {
    return false;
  }
  const size_t back = (size_t)dist;
  for (uint32_t k = 0U; (k < length) && (pos < unp); ++k) { /* bound: length, pos<unp */
    out[pos] = out[pos - back];
    pos += 1U;
  }
  *out_pos = pos;
  return true;
}

/* ---- in-stream data filters --------------------------------------------- */

/**
 * @brief Read one length/offset field of a RAR5 filter descriptor.
 * @details A 2-bit byte count selects 1..4 little-endian bytes of the value.
 * @param[in,out] st Decoder state (non-NULL).
 * @return The decoded field value.
 * @retval 0 When the encoded bytes are all zero.
 * @pre @p st is a bound decoder state at a filter descriptor.
 * @pre @p st has bits remaining or pads with zero.
 * @post `st->consumed` advanced past the field.
 * @post The result fits 32 bits.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static uint32_t s_read_filter_data(ra8_rar5_state_t* st)
{
  const uint32_t bc   = ra8_rar5_get(st, 2U) + 1U;
  uint32_t       data = 0U;
  for (uint32_t i = 0U; i < bc; ++i) { /* bound: bc <= 4 */
    data |= ra8_rar5_get(st, (uint32_t)k_r5_byte_bits) << (i * (uint32_t)k_r5_byte_bits);
  }
  return data;
}

/**
 * @brief Read one filter descriptor and queue it for post-decode replay.
 * @details Records the transform's absolute output start (relative offset plus the
 *          current output position), its length, kind, and delta channel count.
 * @param[in,out] st      Decoder state (non-NULL).
 * @param[in]     out_pos Current output position (filter start base).
 * @return ra8_err_t status.
 * @retval k_ra8_ok                    The filter was queued.
 * @retval k_ra8_err_validation_failed The filter list is full or the kind is invalid.
 * @pre @p st is a bound decoder state.
 * @pre @p out_pos is the current output length.
 * @post On k_ra8_ok, `st->filter_count` incremented by one.
 * @post `st->consumed` advanced past the descriptor.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t s_read_filter(ra8_rar5_state_t* st, size_t out_pos)
{
  if (st->filter_count >= (uint16_t)k_ra8_rar5_max_filters) {
    return k_ra8_err_validation_failed;
  }
  const uint32_t rel  = s_read_filter_data(st);
  const uint32_t len  = s_read_filter_data(st);
  const uint32_t type = ra8_rar5_get(st, (uint32_t)k_r5_ftype_bits);
  if (type > (uint32_t)k_ra8_rar5_filter_arm) {
    return k_ra8_err_validation_failed;
  }
  uint32_t channels = 1U;
  if (type == (uint32_t)k_ra8_rar5_filter_delta) {
    channels = ra8_rar5_get(st, (uint32_t)k_r5_fchan_bits) + 1U;
  }
  ra8_rar5_filter_t* f = &st->filters[st->filter_count];
  f->start             = (uint64_t)out_pos + (uint64_t)rel;
  f->len               = len;
  f->type              = (uint8_t)type;
  f->channels          = (uint8_t)channels;
  st->filter_count += 1U;
  return k_ra8_ok;
}

/** @brief Implementation of `ra8_rar5_filter_delta()` -- per-channel running byte-sum. */
void ra8_rar5_filter_delta(ra8_rar5_state_t* st, uint8_t* d, uint32_t len, uint32_t channels)
{
  if ((len > (uint32_t)k_ra8_rar5_delta_scratch) || (channels == 0U)) {
    return;
  }
  (void)memcpy(st->delta, d, (size_t)len);
  uint32_t dpos = 0U;
  for (uint32_t ch = 0U; ch < channels; ++ch) { /* bound: channels <= 32 */
    uint8_t prev = 0U;
    for (uint32_t i = ch; i < len; i += channels) { /* bound: i < len */
      prev = (uint8_t)(prev - st->delta[dpos]);
      dpos += 1U;
      d[i] = prev;
    }
  }
}

/**
 * @brief Test whether @p op is an x86 branch opcode this filter transforms.
 * @details CALL (0xE8) always; JMP (0xE9) only when @p e9 is set (E8E9 variant).
 * @param[in] op Instruction opcode byte.
 * @param[in] e9 Whether 0xE9 is also transformed.
 * @return Whether @p op is a transformed branch.
 * @retval true  @p op is CALL, or JMP with @p e9 set.
 * @retval false Any other opcode.
 * @pre @p op is the candidate opcode byte.
 * @pre @p e9 selects the E8E9 variant.
 * @post No state is modified (pure function).
 * @post Only 0xE8 / (0xE9 with @p e9) return true.
 * @note Thread-safe: pure.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static bool s_x86_is_op(uint8_t op, bool e9)
{
  if (op == (uint8_t)k_r5_x86_call) {
    return true;
  }
  return e9 && (op == (uint8_t)k_r5_x86_jmp);
}

/**
 * @brief Apply the x86 CALL/JMP relative-address filter over @p d.
 * @details For each transformed branch, subtracts the operand's stream position from
 *          the stored 32-bit operand -- the inverse of the writer's add.
 * @param[in,out] d       Output range to transform (non-NULL).
 * @param[in]     len     Range length in bytes.
 * @param[in]     filepos Absolute output offset of `d[0]`.
 * @param[in]     e9      Whether 0xE9 JMP is transformed too.
 * @return Nothing.
 * @pre @p d holds @p len writable bytes.
 * @pre @p len fits an x86 instruction (>= 5) to transform anything.
 * @post Each transformed operand is relative-decoded in place.
 * @post Non-branch bytes are unchanged.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void s_filter_x86(uint8_t* d, uint32_t len, uint64_t filepos, bool e9)
{
  if (len < (uint32_t)k_r5_x86_ilen) {
    return;
  }
  const uint32_t limit = len - (uint32_t)k_r5_x86_ilen;
  uint32_t       i     = 0U;
  while (i <= limit) { /* bound: i <= limit < len */
    if (s_x86_is_op(d[i], e9)) {
      const uint32_t off = i + 1U;
      const uint32_t pos = (uint32_t)(filepos + (uint64_t)off);
      s_wr_le32(&d[off], s_rd_le32(&d[off]) - pos);
      i += (uint32_t)k_r5_x86_ilen;
    } else {
      i += 1U;
    }
  }
}

/**
 * @brief Apply the ARM BL branch-offset filter over @p d.
 * @details For each 4-byte word whose top byte is the BL opcode, subtracts the word
 *          position from the 24-bit branch offset -- the inverse of the writer's add.
 * @param[in,out] d       Output range to transform (non-NULL).
 * @param[in]     len     Range length in bytes.
 * @param[in]     filepos Absolute output offset of `d[0]`.
 * @return Nothing.
 * @pre @p d holds @p len writable bytes.
 * @pre @p len >= 4 to transform anything.
 * @post Each BL offset is relative-decoded in place.
 * @post Non-BL words are unchanged.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void s_filter_arm(uint8_t* d, uint32_t len, uint64_t filepos)
{
  if (len < 4U) {
    return;
  }
  const uint32_t limit = len - 4U;
  uint32_t       i     = 0U;
  while (i <= limit) { /* bound: i steps by 4 to limit */
    if (d[i + 3U] == (uint8_t)k_r5_arm_bl) {
      uint32_t       v = (uint32_t)d[i] | ((uint32_t)d[i + 1U] << k_r5_byte_bits) |
                         ((uint32_t)d[i + 2U] << (k_r5_byte_bits * 2U));
      const uint32_t p = (uint32_t)((filepos + (uint64_t)i) >> 2U);
      v                = (v - p) & (uint32_t)k_r5_arm_off_mask;
      d[i]             = (uint8_t)(v & (uint32_t)k_r5_byte_mask);
      d[i + 1U]        = (uint8_t)((v >> k_r5_byte_bits) & (uint32_t)k_r5_byte_mask);
      d[i + 2U]        = (uint8_t)((v >> (k_r5_byte_bits * 2U)) & (uint32_t)k_r5_byte_mask);
    }
    i += 4U;
  }
}

/**
 * @brief Replay one queued filter over its (clamped) output range.
 * @details Clamps the filter's range to the decoded output, then dispatches to the
 *          delta / x86 / x86-e8e9 / ARM transform; a range starting past the output
 *          end is skipped.
 * @param[in,out] st  Decoder state (non-NULL).
 * @param[in,out] out Decoded output buffer (non-NULL).
 * @param[in]     unp Total unpacked length (range clamp).
 * @param[in]     f   Filter to apply (non-NULL).
 * @return Nothing.
 * @pre @p out holds @p unp writable bytes.
 * @pre @p f came from ::s_read_filter.
 * @post The output range is transformed in place, or skipped if out of range.
 * @post No byte outside `[f->start, unp)` is touched.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void
s_apply_one_filter(ra8_rar5_state_t* st, uint8_t* out, size_t unp, const ra8_rar5_filter_t* f)
{
  if (f->start >= (uint64_t)unp) {
    return;
  }
  const uint64_t avail = (uint64_t)unp - f->start;
  const uint32_t len   = ((uint64_t)f->len > avail) ? (uint32_t)avail : f->len;
  if (len == 0U) {
    return;
  }
  uint8_t* d = &out[(size_t)f->start];
  if (f->type == (uint8_t)k_ra8_rar5_filter_delta) {
    ra8_rar5_filter_delta(st, d, len, f->channels);
  } else if (f->type == (uint8_t)k_ra8_rar5_filter_e8) {
    s_filter_x86(d, len, f->start, false);
  } else if (f->type == (uint8_t)k_ra8_rar5_filter_e8e9) {
    s_filter_x86(d, len, f->start, true);
  } else {
    s_filter_arm(d, len, f->start);
  }
}

/**
 * @brief Replay every queued filter in stream order.
 * @details Iterates the recorded filter list front to back, applying each via
 *          ::s_apply_one_filter; a no-op when no filter was recorded (the comic case).
 * @param[in,out] st  Decoder state (non-NULL).
 * @param[in,out] out Decoded output buffer (non-NULL).
 * @param[in]     unp Total unpacked length.
 * @return Nothing.
 * @pre @p out holds @p unp writable bytes.
 * @pre @p st::filter_count <= ::k_ra8_rar5_max_filters.
 * @post Each queued filter has been applied.
 * @post No byte past @p unp is written.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void s_apply_filters(ra8_rar5_state_t* st, uint8_t* out, size_t unp)
{
  for (uint16_t f = 0U; f < st->filter_count; ++f) { /* bound: filter_count */
    s_apply_one_filter(st, out, unp, &st->filters[f]);
  }
}

/* ---- token dispatch + block/stream loops -------------------------------- */

/**
 * @brief Decode a length-slot LZ match (main symbol >= 262) into @p out.
 * @details Reads the length from the slot, the distance from the DD/LDD tables,
 *          applies the distance-length bias, remembers the distance, and copies the
 *          match into the output window.
 * @param[in,out] st      Decoder state (non-NULL).
 * @param[in]     slot    Main symbol (>= ::k_r5_sym_lenbase).
 * @param[in,out] out     Output/window buffer (non-NULL).
 * @param[in,out] out_pos Current output length; advanced (non-NULL).
 * @param[in]     unp     Target unpacked length.
 * @return ra8_err_t status.
 * @retval k_ra8_ok                    The match was copied.
 * @retval k_ra8_err_validation_failed An invalid back-distance.
 * @pre @p st has the LZ tables built.
 * @pre `*out_pos <= unp`.
 * @post On k_ra8_ok, `*out_pos` advanced and the recent-distance ring updated.
 * @post On error no output byte is written.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
s_do_match(ra8_rar5_state_t* st, uint32_t slot, uint8_t* out, size_t* out_pos, size_t unp)
{
  uint32_t       length = s_slot_to_length(st, slot - (uint32_t)k_r5_sym_lenbase);
  const uint64_t dist   = s_decode_distance(st);
  length                = s_adjust_length(length, dist);
  s_push_dist(st, dist);
  st->last_length = length;
  if (!ra8_rar5_copy_match(out, out_pos, unp, length, dist)) {
    return k_ra8_err_validation_failed;
  }
  return k_ra8_ok;
}

/**
 * @brief Decode a remembered-distance LZ match (main symbols 258..261) into @p out.
 * @details Selects one of the four remembered distances, promotes it to most-recent,
 *          reads a repeat length from the RD table, and copies the match.
 * @param[in,out] st      Decoder state (non-NULL).
 * @param[in]     slot    Main symbol in 258..261.
 * @param[in,out] out     Output/window buffer (non-NULL).
 * @param[in,out] out_pos Current output length; advanced (non-NULL).
 * @param[in]     unp     Target unpacked length.
 * @return ra8_err_t status.
 * @retval k_ra8_ok                    The match was copied.
 * @retval k_ra8_err_validation_failed An invalid back-distance.
 * @pre @p st has the LZ tables built.
 * @pre `*out_pos <= unp`.
 * @post On k_ra8_ok, the chosen distance is promoted to most-recent.
 * @post On error no output byte is written.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
s_do_repdist(ra8_rar5_state_t* st, uint32_t slot, uint8_t* out, size_t* out_pos, size_t unp)
{
  const uint32_t idx  = slot - (uint32_t)k_r5_sym_repdist0;
  const uint64_t dist = st->old_dist[idx];
  for (uint32_t i = idx; i > 0U; --i) { /* bound: idx <= 3 */
    st->old_dist[i] = st->old_dist[i - 1U];
  }
  st->old_dist[0]        = dist;
  const uint32_t lenslot = ra8_rar5_decode_num(st, &st->rd);
  const uint32_t length  = s_slot_to_length(st, lenslot);
  st->last_length        = length;
  if (!ra8_rar5_copy_match(out, out_pos, unp, length, dist)) {
    return k_ra8_err_validation_failed;
  }
  return k_ra8_ok;
}

/**
 * @brief Replay the last match (main symbol 257) into @p out.
 * @details Copies the most-recent match's length at the most-recent distance; a no-op
 *          when no match has been seen yet (last length is zero).
 * @param[in,out] st      Decoder state (non-NULL).
 * @param[in,out] out     Output/window buffer (non-NULL).
 * @param[in,out] out_pos Current output length; advanced (non-NULL).
 * @param[in]     unp     Target unpacked length.
 * @return ra8_err_t status.
 * @retval k_ra8_ok                    The repeat was copied (or was a no-op).
 * @retval k_ra8_err_validation_failed An invalid back-distance.
 * @pre @p st is a bound decoder state.
 * @pre `*out_pos <= unp`.
 * @post On k_ra8_ok, `*out_pos` advanced by the last length (0 if none yet).
 * @post On error no output byte is written.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t s_do_replast(ra8_rar5_state_t* st, uint8_t* out, size_t* out_pos, size_t unp)
{
  if (st->last_length == 0U) {
    return k_ra8_ok;
  }
  if (!ra8_rar5_copy_match(out, out_pos, unp, st->last_length, st->old_dist[0])) {
    return k_ra8_err_validation_failed;
  }
  return k_ra8_ok;
}

/**
 * @brief Decode one LZ token: literal, match, repeat-match, or filter.
 * @details Classifies the main symbol: below 256 is a literal byte, 256 a filter,
 *          257 the last-match repeat, 258-261 a remembered-distance match, 262+ a
 *          length-slot match.
 * @param[in,out] st      Decoder state (non-NULL).
 * @param[in,out] out     Output/window buffer (non-NULL).
 * @param[in,out] out_pos Current output length; advanced (non-NULL).
 * @param[in]     unp     Target unpacked length.
 * @return ra8_err_t status.
 * @retval k_ra8_ok                    One token was decoded.
 * @retval k_ra8_err_validation_failed A malformed match or filter.
 * @pre @p st has the LZ tables built.
 * @pre `*out_pos < unp`.
 * @post On k_ra8_ok, output/filters advanced by exactly one token.
 * @post On error no partial token corrupts prior output.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t s_decode_token(ra8_rar5_state_t* st, uint8_t* out, size_t* out_pos, size_t unp)
{
  const uint32_t slot = ra8_rar5_decode_num(st, &st->ld);
  if (slot < (uint32_t)k_r5_sym_filter) {
    out[*out_pos] = (uint8_t)slot;
    *out_pos += 1U;
    return k_ra8_ok;
  }
  if (slot == (uint32_t)k_r5_sym_filter) {
    return s_read_filter(st, *out_pos);
  }
  if (slot == (uint32_t)k_r5_sym_replast) {
    return s_do_replast(st, out, out_pos, unp);
  }
  if (slot < (uint32_t)k_r5_sym_lenbase) {
    return s_do_repdist(st, slot, out, out_pos, unp);
  }
  return s_do_match(st, slot, out, out_pos, unp);
}

/**
 * @brief Open the next compressed block: header, tables, and its bit-end.
 * @details Reads and validates the block header, rejects a zero-size block, computes
 *          the block's end bit position, and (re)builds the Huffman tables when the
 *          block carries them -- else requires that a prior block already did.
 * @param[in,out] st      Decoder state (non-NULL).
 * @param[out]    end_bit Receives the block's absolute end bit position (non-NULL).
 * @param[out]    last    Receives whether this is the last block (non-NULL).
 * @return ra8_err_t status.
 * @retval k_ra8_ok                    Block opened and tables ready.
 * @retval k_ra8_err_validation_failed A bad header, zero-size block, or no tables.
 * @pre @p st is a bound decoder state.
 * @pre @p end_bit and @p last are writable.
 * @post On k_ra8_ok, `*end_bit` is the block's end and tables are usable.
 * @post On error the decode is abandoned.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t s_open_block(ra8_rar5_state_t* st, uint64_t* end_bit, bool* last)
{
  r5_block_t      blk = {};
  const ra8_err_t e   = ra8_rar5_read_block_header(st, &blk);
  if (e != k_ra8_ok) {
    return e;
  }
  if (blk.size == 0U) {
    return k_ra8_err_validation_failed;
  }
  const uint64_t start = st->consumed;
  *end_bit = start + ((blk.size - 1U) * (uint64_t)k_r5_byte_bits) + (uint64_t)blk.last_bits;
  *last    = blk.last;
  if (blk.tables) {
    return ra8_rar5_read_tables(st);
  }
  if (!st->tables_ready) {
    return k_ra8_err_validation_failed;
  }
  return k_ra8_ok;
}

/**
 * @brief Drive the block + token loops until @p unp bytes are produced.
 * @details Opens the first compressed block, then reads a fresh block header
 *          whenever the current block's bits are exhausted, decoding tokens into
 *          @p out until the output is complete, the last block ends, or the packed
 *          member (plus a small pad) is fully consumed. Opening the first block
 *          before the loop keeps the in-loop block check single-condition -- the
 *          entry-time `!have_block` / `consumed >= block_end` pair was correlated
 *          (on the first pass `block_end` is still zero) and so could never reach
 *          MC/DC; hoisting the open removes that correlated compound decision.
 * @param[in,out] st      Decoder state (non-NULL).
 * @param[in,out] out     Output/window buffer (non-NULL).
 * @param[in]     out_cap Capacity of @p out in bytes.
 * @param[in]     unp     Target unpacked length (<= @p out_cap, > 0).
 * @return ra8_err_t status.
 * @retval k_ra8_ok                    Exactly @p unp bytes were produced.
 * @retval k_ra8_err_validation_failed Truncated / malformed stream, short output.
 * @pre @p out holds @p out_cap writable bytes, @p unp <= @p out_cap.
 * @pre @p st is a freshly reset decoder state and @p unp > 0.
 * @post On k_ra8_ok, `out[0..unp)` holds the decoded bytes.
 * @post On error the output is incomplete.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t s_decode_stream(ra8_rar5_state_t* st, uint8_t* out, size_t out_cap, size_t unp)
{
  (void)out_cap;
  size_t         out_pos   = 0U;
  bool           last      = false;
  uint64_t       block_end = 0U;
  const uint64_t cap_bits  = (st->packlen * (uint64_t)k_r5_byte_bits) + (uint64_t)k_r5_max_pad_bits;
  ra8_err_t      e         = s_open_block(st, &block_end, &last);
  if (e != k_ra8_ok) {
    return e;
  }
  while ((out_pos < unp) && (st->consumed <= cap_bits)) { /* bound: consumed strictly rises */
    if (st->consumed >= block_end) {
      if (last) {
        break;
      }
      e = s_open_block(st, &block_end, &last);
      if (e != k_ra8_ok) {
        return e;
      }
      continue;
    }
    e = s_decode_token(st, out, &out_pos, unp);
    if (e != k_ra8_ok) {
      return e;
    }
  }
  if (out_pos != unp) {
    return k_ra8_err_validation_failed;
  }
  return k_ra8_ok;
}

/**
 * @brief Validate the non-output arguments of ::ra8_rar5_decompress.
 * @details Null-checks the archive, output, and scratch pointers, then rejects a
 *          non-RAR5 archive, an output buffer smaller than the unpacked size, and a
 *          zero packed size. Split out so the entry point stays within the
 *          function-size budget.
 * @param[in] rar       Archive to decode from.
 * @param[in] out       Output buffer.
 * @param[in] st        Decoder scratch.
 * @param[in] out_cap   Output-buffer capacity in bytes.
 * @param[in] unp_size  Expected unpacked length.
 * @param[in] pack_size Packed member length.
 * @return ra8_err_t status.
 * @retval k_ra8_ok                Arguments are usable.
 * @retval k_ra8_err_null_ptr      @p rar, @p out, or @p st was NULL.
 * @retval k_ra8_err_invalid_state @p rar is not a RAR5 archive.
 * @retval k_ra8_err_no_mem        @p out_cap is smaller than @p unp_size.
 * @retval k_ra8_err_validation_failed @p pack_size is zero.
 * @pre @p got was already validated and cleared by the caller.
 * @pre The caller forwards its arguments unchanged.
 * @post On k_ra8_ok every pointer is safe to dereference.
 * @post No state is modified.
 * @note Not thread-safe (reads @p rar fields).
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t s_decompress_check(const ra8_rar_t*        rar,
                                    const uint8_t*          out,
                                    const ra8_rar5_state_t* st,
                                    size_t                  out_cap,
                                    uint64_t                unp_size,
                                    uint64_t                pack_size)
{
  RA8_CHECK_NULL_PTR(rar, s_tag_rar5, "decompress: null rar");
  RA8_CHECK_NULL_PTR(out, s_tag_rar5, "decompress: null out");
  RA8_CHECK_NULL_PTR(st, s_tag_rar5, "decompress: null st");
  if (rar->version != k_ra8_rar_ver_5) {
    return k_ra8_err_invalid_state;
  }
  if (unp_size > (uint64_t)out_cap) {
    return k_ra8_err_no_mem;
  }
  if (pack_size == 0U) {
    return k_ra8_err_validation_failed;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_rar5_decompress(const ra8_rar_t*  rar,
                              uint64_t          data_off,
                              uint64_t          pack_size,
                              uint8_t*          out,
                              size_t            out_cap,
                              uint64_t          unp_size,
                              ra8_rar5_state_t* st,
                              size_t*           got)
{
  RA8_CHECK_NULL_PTR(got, s_tag_rar5, "decompress: null got");
  *got                   = 0U;
  const ra8_err_t vcheck = s_decompress_check(rar, out, st, out_cap, unp_size, pack_size);
  if (vcheck != k_ra8_ok) {
    return vcheck;
  }
  *st         = (ra8_rar5_state_t){};
  st->rar     = rar;
  st->base    = data_off;
  st->packlen = pack_size;
  if (unp_size == 0U) {
    return k_ra8_ok;
  }
  const ra8_err_t e = s_decode_stream(st, out, out_cap, (size_t)unp_size);
  if (e != k_ra8_ok) {
    return e;
  }
  s_apply_filters(st, out, (size_t)unp_size);
  *got = (size_t)unp_size;
  return k_ra8_ok;
}
