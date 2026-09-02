/**
 * @file ra8_fs_utf.c
 * @brief The one UTF-8 <-> UTF-16LE seam between `ra8_fs`'s API and its disks.
 *
 * @details
 * Implements the contracts in `ra8_fs_utf_internal.h`. The decoder is split into
 * three deliberately dull steps -- classify the lead byte, fold in the
 * continuation bytes, then judge the assembled code point -- because that last
 * step is the one every naive UTF-8 reader skips, and skipping it is what lets
 * an over-long encoding or a raw surrogate through into a file name.
 *
 * Nothing here substitutes a character for a byte it did not understand. A
 * conversion either produces the caller's name or reports why it could not.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fs_fat_internal.h"
#include "ra8_fs_utf_internal.h"

/* Every UTF-8 name buffer in this module is sized from a UTF-16 unit cap times
 * the worst case per unit, plus a terminator. Asserting it here rather than
 * trusting the arithmetic in a comment is what stops the two drifting when one
 * of the unit caps moves: a name that fits on disk MUST fit in the buffer it is
 * handed back in, or the conversion reports ::k_ra8_err_no_mem for a name the
 * volume holds perfectly well. */
static_assert((uint32_t)k_lfn_utf8_cap ==
                (((uint32_t)k_utf8_max_per_unit * (uint32_t)k_lfn_write_max) + 1U),
              "k_lfn_utf8_cap must hold the longest VFAT long name in UTF-8");
static_assert((uint32_t)k_exfat_name_u8_cap ==
                (((uint32_t)k_utf8_max_per_unit * (uint32_t)k_exfat_name_cap) + 1U),
              "k_exfat_name_u8_cap must hold the longest exFAT name in UTF-8");

/* =============================================================================
 * UTF-8 -> UTF-16
 * =============================================================================
 */

/**
 * @brief Classify a UTF-8 lead byte into a sequence length and its payload bits.
 *
 * @details The four legal lead-byte shapes, tested longest-tag first so that a
 *          continuation byte (`10xxxxxx`) cannot be mistaken for anything: it
 *          matches none of them and is reported as illegal, which is what makes
 *          a sequence starting mid-character an error rather than a resync.
 *
 * @param[in]  b       Candidate lead byte.
 * @param[out] out_len Receives the total sequence length in bytes (1..4).
 * @param[out] out_cp  Receives the lead byte's payload bits.
 *
 * @return Legality flag.
 * @retval 1U @p b is a legal lead byte; both outputs are written.
 * @retval 0U @p b is a continuation byte or announces five or more bytes.
 *
 * @pre @p out_len and @p out_cp are non-NULL.
 * @pre @p b is the first byte of a candidate sequence.
 * @post On 1 the outputs describe @p b; on 0 they are untouched.
 * @post No state outside the outputs is modified.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static uint8_t internal_utf8_lead(uint8_t b, uint32_t* out_len, uint32_t* out_cp)
{
  const uint32_t u = (uint32_t)b;
  if (u <= (uint32_t)k_utf_ascii_max) {
    *out_len = (uint32_t)k_utf_len_1;
    *out_cp  = u;
    return 1U;
  }
  if ((u & (uint32_t)k_utf_lead2_mask) == (uint32_t)k_utf_lead2_tag) {
    *out_len = (uint32_t)k_utf_len_2;
    *out_cp  = u & (uint32_t)k_utf_lead2_payload;
    return 1U;
  }
  if ((u & (uint32_t)k_utf_lead3_mask) == (uint32_t)k_utf_lead3_tag) {
    *out_len = (uint32_t)k_utf_len_3;
    *out_cp  = u & (uint32_t)k_utf_lead3_payload;
    return 1U;
  }
  if ((u & (uint32_t)k_utf_lead4_mask) == (uint32_t)k_utf_lead4_tag) {
    *out_len = (uint32_t)k_utf_len_4;
    *out_cp  = u & (uint32_t)k_utf_lead4_payload;
    return 1U;
  }
  return 0U;
}

/**
 * @brief Fold a sequence's continuation bytes into the code point under assembly.
 *
 * @details Each continuation byte contributes ::k_utf_cont_shift payload bits.
 *          A NUL ends the string, so it fails the continuation test like any
 *          other non-continuation byte and a truncated sequence at the end of a
 *          name is rejected without reading past the terminator.
 *
 * @param[in]     in     NUL-terminated UTF-8 name.
 * @param[in]     lead   Index of the sequence's lead byte within @p in.
 * @param[in]     len    Total sequence length from ::priv_utf8_lead().
 * @param[in,out] io_cp  Code point under assembly; extended in place.
 *
 * @return Legality flag.
 * @retval 1U All @p len - 1 continuation bytes were present and well-formed.
 * @retval 0U A byte was missing or was not a continuation byte.
 *
 * @pre @p in and @p io_cp are non-NULL; @p len is 1..4.
 * @pre `in[lead]` is the lead byte @p len came from.
 * @post On 1 `*io_cp` holds every payload bit of the sequence.
 * @post On 0 `*io_cp` is unspecified and the caller must discard it.
 *
 * @note Pure apart from @p io_cp; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static uint8_t internal_utf8_tail(const char* in, uint32_t lead, uint32_t len, uint32_t* io_cp)
{
  for (uint32_t k = 1U; k < len; k++) {
    const uint32_t b = (uint32_t)(unsigned char)in[lead + k];
    if ((b & (uint32_t)k_utf_cont_mask) != (uint32_t)k_utf_cont_tag) {
      return 0U;
    }
    *io_cp = (*io_cp << (uint32_t)k_utf_cont_shift) | (b & (uint32_t)k_utf_cont_payload);
  }
  return 1U;
}

/**
 * @brief Is @p cp a code point that a @p len byte sequence may legally encode?
 *
 * @details Three refusals, all of which a decoder that only assembles bits will
 *          let through: an over-long form (the code point has a shorter
 *          encoding, so this byte string is a second spelling of it), a
 *          surrogate code point (UTF-8 does not encode them; UTF-16 uses them
 *          as machinery), and anything past U+10FFFF.
 *
 * @param[in] cp  Assembled code point.
 * @param[in] len Sequence length it was assembled from.
 *
 * @return Legality flag.
 * @retval 1U @p cp is well-formed for @p len.
 * @retval 0U Over-long, a surrogate, or out of range.
 *
 * @pre @p len is 1..4 and came from ::priv_utf8_lead().
 * @pre @p cp was assembled by ::priv_utf8_tail() from that sequence.
 * @post No state is modified.
 * @post The verdict depends only on the inputs.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static uint8_t internal_utf8_wellformed(uint32_t cp, uint32_t len)
{
  if (len == (uint32_t)k_utf_len_2) {
    return (cp >= (uint32_t)k_utf_min_2byte) ? 1U : 0U;
  }
  if (len == (uint32_t)k_utf_len_3) {
    if (cp < (uint32_t)k_utf_min_3byte) {
      return 0U;
    }
    /* CESU-8 / WTF-8 spell a surrogate as three bytes; UTF-8 never does. */
    if ((cp >= (uint32_t)k_utf_sur_hi_first) && (cp <= (uint32_t)k_utf_sur_last)) {
      return 0U;
    }
    return 1U;
  }
  if (len == (uint32_t)k_utf_len_4) {
    if (cp < (uint32_t)k_utf_min_4byte) {
      return 0U;
    }
    return (cp <= (uint32_t)k_utf_code_max) ? 1U : 0U;
  }
  return 1U; /* one-byte forms are ASCII by construction */
}

/**
 * @brief Decode the sequence at `*io_pos`, advancing the cursor past it.
 *
 * @details The three steps in order: classify, gather, judge. Every failure is
 *          the same answer to the caller -- this is not UTF-8 -- because a
 *          filesystem has nothing useful to do with the distinction and a
 *          caller that could tell them apart would be tempted to recover from
 *          one of them.
 *
 * @param[in]     in     NUL-terminated UTF-8 name.
 * @param[in,out] io_pos Byte index to decode at; advanced past the sequence.
 * @param[out]    out_cp Receives the decoded code point.
 *
 * @return Error code.
 * @retval k_ra8_ok              One code point decoded; cursor advanced.
 * @retval k_ra8_err_invalid_arg The bytes at `*io_pos` are not well-formed.
 *
 * @pre All pointers are non-NULL and `in[*io_pos]` is not the NUL terminator.
 * @pre `*io_pos` indexes a lead byte, not the middle of a sequence.
 * @post On success `*io_pos` advanced by 1..4 and `*out_cp` is a scalar value.
 * @post On failure `*io_pos` and `*out_cp` are unspecified.
 *
 * @note Pure apart from the outputs; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_utf8_next(const char* in, uint32_t* io_pos, uint32_t* out_cp)
{
  const uint32_t pos = *io_pos;
  uint32_t       len = 0U;
  uint32_t       cp  = 0U;
  if (internal_utf8_lead((uint8_t)(unsigned char)in[pos], &len, &cp) == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (internal_utf8_tail(in, pos, len, &cp) == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (internal_utf8_wellformed(cp, len) == 0U) {
    return k_ra8_err_invalid_arg;
  }
  *io_pos = pos + len;
  *out_cp = cp;
  return k_ra8_ok;
}

/**
 * @brief Append @p cp to a UTF-16 buffer as one unit or as a surrogate pair.
 *
 * @details The capacity test covers the WHOLE character: a supplementary code
 *          point with one unit of room left is refused rather than half-written,
 *          so a truncated buffer never ends in a lone high surrogate.
 *
 * @param[in]     cp    Code point to append.
 * @param[out]    out   Destination unit buffer.
 * @param[in]     cap   Capacity of @p out in units.
 * @param[in,out] io_n  Units already written; advanced by 1 or 2.
 *
 * @return Error code.
 * @retval k_ra8_ok         Appended.
 * @retval k_ra8_err_no_mem @p cap has no room for the whole character.
 *
 * @pre @p out addresses @p cap writable units; @p io_n is non-NULL.
 * @pre @p cp is a Unicode scalar value (never a surrogate).
 * @post On success `*io_n` grew by exactly the units the character needs.
 * @post On failure @p out and `*io_n` are unchanged.
 *
 * @note Pure apart from the outputs; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_utf16_put(uint32_t cp, uint16_t* out, uint32_t cap, uint32_t* io_n)
{
  const uint32_t n = *io_n;
  /* One assignment of a single constant per arm, rather than a ternary whose
   * composite value MISRA 10.6 forbids assigning to the wider uint32_t. */
  uint32_t need = 1U;
  if (cp >= (uint32_t)k_utf_min_4byte) {
    need = 2U;
  }
  if ((n + need) > cap) {
    return k_ra8_err_no_mem;
  }
  if (need == 1U) {
    out[n] = (uint16_t)cp;
  } else {
    const uint32_t rest = cp - (uint32_t)k_utf_min_4byte;
    out[n]      = (uint16_t)((uint32_t)k_utf_sur_hi_first + (rest >> (uint32_t)k_utf_sur_shift));
    out[n + 1U] = (uint16_t)((uint32_t)k_utf_sur_lo_first + (rest & (uint32_t)k_utf_sur_mask));
  }
  *io_n = n + need;
  return k_ra8_ok;
}

/* `priv_utf8_to_utf16()`: see header for the documented contract. */
ra8_err_t priv_utf8_to_utf16(const char* in, uint16_t* out, uint32_t cap, uint32_t* out_units)
{
  if ((in == nullptr) || (out == nullptr) || (out_units == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *out_units   = 0U;
  uint32_t pos = 0U;
  uint32_t n   = 0U;
  /* Bounded (NASA Rule 2): every pass either returns or appends at least one
   * unit, and ::priv_utf16_put refuses to grow past `cap`. */
  while (n <= cap) {
    if (in[pos] == '\0') {
      *out_units = n;
      return k_ra8_ok;
    }
    uint32_t  cp  = 0U;
    ra8_err_t err = internal_utf8_next(in, &pos, &cp);
    if (err != k_ra8_ok) {
      return err;
    }
    err = internal_utf16_put(cp, out, cap, &n);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  /* Unreachable: the loop's only exits are the three returns above. This is the
   * Rule 2 bound's exit, not a fourth answer. */
  return k_ra8_err_no_mem; /* GCOVR_EXCL_LINE -- bounded-loop fallback after exhaustive returns */
}

/* =============================================================================
 * UTF-16 -> UTF-8
 * =============================================================================
 */

/**
 * @brief Take the character at `*io_i`, consuming a surrogate pair as one.
 *
 * @details A high surrogate is only a character together with the low surrogate
 *          that follows it. Either half on its own is refused here rather than
 *          replaced, which is what keeps ::priv_utf16_to_utf8()'s promise that
 *          every name it returns can be handed straight back to
 *          ::priv_utf8_to_utf16().
 *
 * @param[in]     in     Code units.
 * @param[in]     units  Number of units in @p in.
 * @param[in,out] io_i   Unit index to read at; advanced by 1 or 2.
 * @param[out]    out_cp Receives the code point.
 *
 * @return Error code.
 * @retval k_ra8_ok              One character taken; cursor advanced.
 * @retval k_ra8_err_invalid_arg An unpaired surrogate sits at `*io_i`.
 *
 * @pre All pointers are non-NULL and `*io_i` is below @p units.
 * @pre @p in addresses at least @p units readable units.
 * @post On success `*out_cp` is a Unicode scalar value.
 * @post On failure `*io_i` and `*out_cp` are unspecified.
 *
 * @note Pure apart from the outputs; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_utf16_take(const uint16_t* in, uint32_t units, uint32_t* io_i, uint32_t* out_cp)
{
  const uint32_t i  = *io_i;
  const uint32_t hi = (uint32_t)in[i];
  if ((hi < (uint32_t)k_utf_sur_hi_first) || (hi > (uint32_t)k_utf_sur_last)) {
    *out_cp = hi;
    *io_i   = i + 1U;
    return k_ra8_ok;
  }
  if (hi >= (uint32_t)k_utf_sur_lo_first) {
    return k_ra8_err_invalid_arg; /* a low surrogate with no high one before it */
  }
  if ((i + 1U) >= units) {
    return k_ra8_err_invalid_arg; /* a high surrogate at the end of the name */
  }
  const uint32_t lo = (uint32_t)in[i + 1U];
  if ((lo < (uint32_t)k_utf_sur_lo_first) || (lo > (uint32_t)k_utf_sur_last)) {
    return k_ra8_err_invalid_arg; /* a high surrogate followed by something else */
  }
  *out_cp = (uint32_t)k_utf_min_4byte +
            (((hi - (uint32_t)k_utf_sur_hi_first) << (uint32_t)k_utf_sur_shift) |
             (lo - (uint32_t)k_utf_sur_lo_first));
  *io_i   = i + 2U;
  return k_ra8_ok;
}

/**
 * @brief How many UTF-8 bytes does @p cp occupy?
 *
 * @details The thresholds are the same ::ra8_fs_utf_t constants the decoder
 *          tests over-long forms against, which is the point of naming them:
 *          the encoder and the validator cannot drift into disagreeing about
 *          which length a code point belongs to.
 *
 * @param[in] cp Unicode scalar value.
 *
 * @return Byte count.
 * @retval 1..4 The shortest UTF-8 form's length.
 *
 * @pre @p cp is at most ::k_utf_code_max.
 * @pre @p cp is not a surrogate.
 * @post No state is modified.
 * @post The result is the SHORTEST form's length, never a longer legal one.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_utf8_len_of(uint32_t cp)
{
  if (cp < (uint32_t)k_utf_min_2byte) {
    return (uint32_t)k_utf_len_1;
  }
  if (cp < (uint32_t)k_utf_min_3byte) {
    return (uint32_t)k_utf_len_2;
  }
  if (cp < (uint32_t)k_utf_min_4byte) {
    return (uint32_t)k_utf_len_3;
  }
  return (uint32_t)k_utf_len_4;
}

/**
 * @brief Write the continuation bytes of @p cp after its lead byte.
 *
 * @details Emitted from the LAST byte backwards, because each one carries the
 *          low ::k_utf_cont_shift bits of what is left. Splitting this out of
 *          ::priv_utf8_put() is what keeps that function's four length cases
 *          from becoming four copies of the same shift loop.
 *
 * @param[out] out  Destination byte buffer.
 * @param[in]  from Index of the sequence's lead byte.
 * @param[in]  len  Total sequence length (2..4).
 * @param[in]  cp   Code point being written.
 *
 * @return Nothing.
 *
 * @pre @p out has at least @p from + @p len writable bytes.
 * @pre @p len is ::priv_utf8_len_of(@p cp) and is at least 2.
 * @post Bytes @p from + 1 .. @p from + @p len - 1 are continuation bytes.
 * @post The lead byte at @p from is NOT written here.
 *
 * @note Pure apart from @p out; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_utf8_put_tail(char* out, uint32_t from, uint32_t len, uint32_t cp)
{
  uint32_t rest = cp;
  for (uint32_t k = len - 1U; k >= 1U; k--) {
    const uint32_t b = (uint32_t)k_utf_cont_tag | (rest & (uint32_t)k_utf_cont_payload);
    out[from + k]    = (char)(unsigned char)b;
    rest >>= (uint32_t)k_utf_cont_shift;
  }
}

/**
 * @brief Append @p cp to a UTF-8 buffer, reserving room for the terminator.
 *
 * @details The capacity test keeps one byte back for the NUL, so a caller never
 *          has to remember to. As with ::priv_utf16_put(), a character that does
 *          not fit whole is refused whole.
 *
 * @param[in]     cp   Unicode scalar value to append.
 * @param[out]    out  Destination byte buffer.
 * @param[in]     cap  Capacity of @p out in bytes, including the terminator.
 * @param[in,out] io_n Bytes already written; advanced by 1..4.
 *
 * @return Error code.
 * @retval k_ra8_ok         Appended.
 * @retval k_ra8_err_no_mem @p cap has no room for the character and a NUL.
 *
 * @pre @p out addresses @p cap writable bytes; @p io_n is non-NULL.
 * @pre @p cp is a Unicode scalar value (never a surrogate).
 * @post On success `*io_n` grew by ::priv_utf8_len_of(@p cp).
 * @post On failure @p out and `*io_n` are unchanged.
 *
 * @note Pure apart from the outputs; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_utf8_put(uint32_t cp, char* out, uint32_t cap, uint32_t* io_n)
{
  const uint32_t n   = *io_n;
  const uint32_t len = internal_utf8_len_of(cp);
  if ((n + len + 1U) > cap) {
    return k_ra8_err_no_mem;
  }
  if (len == (uint32_t)k_utf_len_1) {
    out[n] = (char)(unsigned char)cp;
    *io_n  = n + len;
    return k_ra8_ok;
  }
  uint32_t lead_tag = (uint32_t)k_utf_lead4_tag;
  if (len == (uint32_t)k_utf_len_2) {
    lead_tag = (uint32_t)k_utf_lead2_tag;
  } else if (len == (uint32_t)k_utf_len_3) {
    lead_tag = (uint32_t)k_utf_lead3_tag;
  } else {
    /* four-byte lead: the initialiser above */
  }
  const uint32_t lead_bits = cp >> ((len - 1U) * (uint32_t)k_utf_cont_shift);
  out[n]                   = (char)(unsigned char)(lead_tag | lead_bits);
  internal_utf8_put_tail(out, n, len, cp);
  *io_n = n + len;
  return k_ra8_ok;
}

/* `priv_utf16_to_utf8()`: see header for the documented contract. */
ra8_err_t priv_utf16_to_utf8(const uint16_t* in, uint32_t units, char* out, uint32_t cap)
{
  if ((in == nullptr) || (out == nullptr) || (cap == 0U)) {
    return k_ra8_err_null_ptr;
  }
  out[0]     = '\0';
  uint32_t i = 0U;
  uint32_t n = 0U;
  while (i < units) {
    uint32_t  cp  = 0U;
    ra8_err_t err = internal_utf16_take(in, units, &i, &cp);
    if (err != k_ra8_ok) {
      return err;
    }
    err = internal_utf8_put(cp, out, cap, &n);
    if (err != k_ra8_ok) {
      out[0] = '\0';
      return err;
    }
  }
  out[n] = '\0';
  return k_ra8_ok;
}

/* =============================================================================
 * Folding and inspection
 * =============================================================================
 */

/* `priv_utf16_ieq()`: see header for the documented contract. */
uint8_t priv_utf16_ieq(const uint16_t* a, uint32_t an, const uint16_t* b, uint32_t bn)
{
  if (an != bn) {
    return 0U;
  }
  for (uint32_t i = 0U; i < an; i++) {
    if (priv_exfat_upcase_unit(a[i]) != priv_exfat_upcase_unit(b[i])) {
      return 0U;
    }
  }
  return 1U;
}

/* `priv_utf16_all_ascii()`: see header for the documented contract. */
uint8_t priv_utf16_all_ascii(const uint16_t* in, uint32_t units)
{
  for (uint32_t i = 0U; i < units; i++) {
    if ((uint32_t)in[i] > (uint32_t)k_utf_ascii_max) {
      return 0U;
    }
  }
  return 1U;
}
