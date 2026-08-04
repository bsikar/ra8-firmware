/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_unarch_tar_fields.c
 * @brief Untrusted-byte parsers for the tar walker: numerics, checksum, pax.
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * Every function in this TU consumes raw hostile bytes and must be
 * individually fail-closed; they are TU-external (declared in
 * `ra8_unarch_tar_internal.h`) so the host tests drive each rejection
 * branch directly. Grammar sources: the POSIX.1-2017 pax Interchange
 * Format (header block layout, octal fields, `%d key=value\n` records)
 * and the documented GNU tar base-256 numeric extension. Clean-room:
 * no code from GNU tar / libarchive / busybox.
 *
 * @since Version 0.1.0
 */
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_unarch_tar_internal.h"

/** @brief Log tag for tar field-parser diagnostics. */
static const char* const s_tag_tar_f = "ra8_unarch_tar";

/* The pax extended-header record parser walks length-prefixed key=value records
 * with a fail-closed guard at every step: the parse body clears clang-tidy's
 * statement/nesting/cognitive thresholds while staying within the 60-line NASA
 * Rule 4 gate. Same disposition as the other decode state machines in the tree
 * (ra8_jpeg_sw_encode, ra8_rmac_phy). */
/* NOLINTBEGIN(readability-function-size,readability-function-cognitive-complexity) */

/**
 * @enum tar_field_const_t
 * @brief Grammar constants for the numeric and pax record parsers.
 * @since Version 0.1.0
 */
typedef enum : uint32_t {
  k_tar_octal_base    = 8U,            /**< Octal radix.                       */
  k_tar_decimal_base  = 10U,           /**< pax record-length / size radix.    */
  k_tar_b256_flag     = 0x80U,         /**< Base-256 marker bit in byte 0.     */
  k_tar_b256_negative = 0x40U,         /**< Sign bit of a base-256 value.      */
  k_tar_b256_payload  = 0x7FU,         /**< Payload mask of base-256 byte 0.   */
  k_tar_b256_fit      = 8U,            /**< Trailing bytes that fit in uint64. */
  k_tar_shift_byte    = 8U,            /**< Bits per base-256 byte.            */
  k_tar_pax_len_max   = 7U,            /**< Max digits in a pax record length. */
  k_tar_chksum_space  = (uint32_t)' ', /**< chksum bytes count as spaces.      */
} tar_field_const_t;

/**
 * @brief Decode an octal ASCII field (leading spaces, NUL/space terminated).
 * @details The POSIX numeric field shape. Rejects an empty digit run, a
 *          non-octal byte before the terminator, and 64-bit overflow.
 * @param[in]  field Field bytes.
 * @param[in]  len   Field length in bytes.
 * @param[out] out   Receives the value.
 * @return ra8_err_t status.
 * @retval k_ra8_ok                    Decoded.
 * @retval k_ra8_err_validation_failed Empty / malformed / overflowing.
 * @pre @p field holds @p len readable bytes (caller-guarded non-NULL).
 * @pre @p out is non-NULL (caller-guarded).
 * @post On k_ra8_ok `*out` is exact; otherwise `*out` is 0.
 * @post No state outside @p out is modified.
 * @note Thread-safe: pure read.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t s_octal(const uint8_t* field, size_t len, uint64_t* out)
{
  size_t i = 0U;
  while (i < len) { /* bound: len */
    if (field[i] != (uint8_t)' ') {
      break;
    }
    i += 1U;
  }
  uint64_t v      = 0U;
  size_t   digits = 0U;
  while (i < len) { /* bound: len */
    const uint8_t c = field[i];
    if (c == 0U) {
      break;
    }
    if (c == (uint8_t)' ') {
      break;
    }
    if (c < (uint8_t)'0') {
      return k_ra8_err_validation_failed;
    }
    if (c > (uint8_t)'7') {
      return k_ra8_err_validation_failed;
    }
    const uint64_t d = (uint64_t)(c - (uint8_t)'0');
    if (v > ((UINT64_MAX - d) / (uint64_t)k_tar_octal_base)) {
      return k_ra8_err_validation_failed; /* would overflow uint64 */
    }
    v = (v * (uint64_t)k_tar_octal_base) + d;
    digits += 1U;
    i += 1U;
  }
  if (digits == 0U) {
    return k_ra8_err_validation_failed; /* empty field */
  }
  *out = v;
  return k_ra8_ok;
}

/**
 * @brief Decode a GNU base-256 (big-endian binary) numeric field.
 * @details Byte 0 carries the 0x80 marker; the remaining bits are a
 *          big-endian two's-complement value. Negative values and values
 *          that cannot fit a uint64 are rejected fail-closed.
 * @param[in]  field Field bytes (byte 0 has the marker set).
 * @param[in]  len   Field length in bytes.
 * @param[out] out   Receives the value.
 * @return ra8_err_t status.
 * @retval k_ra8_ok                    Decoded.
 * @retval k_ra8_err_validation_failed Negative or over 64 bits.
 * @pre `field[0] & 0x80` is set (caller-routed).
 * @pre @p field / @p out are non-NULL (caller-guarded).
 * @post On k_ra8_ok `*out` is exact; otherwise `*out` is 0.
 * @post No state outside @p out is modified.
 * @note Thread-safe: pure read.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t s_base256(const uint8_t* field, size_t len, uint64_t* out)
{
  if ((field[0] & (uint8_t)k_tar_b256_negative) != 0U) {
    return k_ra8_err_validation_failed; /* negative size: hostile */
  }
  if ((field[0] & (uint8_t)k_tar_b256_payload) != 0U) {
    return k_ra8_err_validation_failed; /* > 2^(8*(len-1)) - way over uint64 */
  }
  uint64_t v = 0U;
  for (size_t i = 1U; i < len; ++i) { /* bound: len */
    const size_t remaining = len - i;
    if (remaining > (size_t)k_tar_b256_fit) {
      if (field[i] != 0U) {
        return k_ra8_err_validation_failed; /* over 64 bits */
      }
      continue;
    }
    v = (v << (uint64_t)k_tar_shift_byte) | (uint64_t)field[i];
  }
  *out = v;
  return k_ra8_ok;
}

ra8_err_t ra8_unarch_tar_num(const uint8_t* field, size_t len, uint64_t* out)
{
  RA8_CHECK_NULL_PTR(field, s_tag_tar_f, "num: null field");
  RA8_CHECK_NULL_PTR(out, s_tag_tar_f, "num: null out");
  *out = 0U;
  if (len == 0U) {
    return k_ra8_err_validation_failed;
  }
  if ((field[0] & (uint8_t)k_tar_b256_flag) != 0U) {
    return s_base256(field, len, out);
  }
  return s_octal(field, len, out);
}

bool ra8_unarch_tar_block_zero(const uint8_t* block)
{
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_unarch_tar_block; ++i) { /* bound: block size */
    if (block[i] != 0U) {
      return false;
    }
  }
  return true;
}

bool ra8_unarch_tar_checksum_ok(const uint8_t* block)
{
  uint32_t sum = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_unarch_tar_block; ++i) { /* bound: block size */
    const bool in_chksum = (i >= (uint32_t)k_ra8_tar_off_chksum) &&
                           (i < ((uint32_t)k_ra8_tar_off_chksum + (uint32_t)k_ra8_tar_len_chksum));
    sum += in_chksum ? (uint32_t)k_tar_chksum_space : (uint32_t)block[i];
  }
  uint64_t stored = 0U;
  if (s_octal(&block[k_ra8_tar_off_chksum], (size_t)k_ra8_tar_len_chksum, &stored) != k_ra8_ok) {
    return false;
  }
  return stored == (uint64_t)sum;
}

bool ra8_unarch_tar_magic_ok(const uint8_t* block)
{
  static const char k_magic[] = "ustar";
  if (memcmp(&block[k_ra8_tar_off_magic], k_magic, (size_t)k_ra8_tar_len_magic) != 0) {
    return false;
  }
  const uint8_t term = block[k_ra8_tar_off_magic_term];
  return (term == 0U) || (term == (uint8_t)' ');
}

ra8_tar_type_t ra8_unarch_tar_classify(uint8_t typeflag)
{
  if (typeflag == (uint8_t)'0') {
    return k_ra8_tar_type_file;
  }
  if (typeflag == 0U) {
    return k_ra8_tar_type_file; /* pre-POSIX regular-file flag */
  }
  if (typeflag == (uint8_t)'5') {
    return k_ra8_tar_type_dir;
  }
  if (typeflag == (uint8_t)'x') {
    return k_ra8_tar_type_pax;
  }
  if (typeflag == (uint8_t)'g') {
    return k_ra8_tar_type_meta;
  }
  if (typeflag == (uint8_t)'K') {
    return k_ra8_tar_type_meta;
  }
  if (typeflag == (uint8_t)'L') {
    return k_ra8_tar_type_longname;
  }
  return k_ra8_tar_type_other;
}

/**
 * @brief Decode one pax record's decimal length prefix.
 * @details Digits up to ::k_tar_pax_len_max, terminated by one space; the
 *          resulting length must cover at least the prefix itself.
 * @param[in]  data   Record bytes.
 * @param[in]  len    Bytes available from the record start.
 * @param[out] reclen Receives the declared record length.
 * @param[out] body   Receives the offset of the first byte after the space.
 * @return ra8_err_t status.
 * @retval k_ra8_ok                    Length decoded.
 * @retval k_ra8_err_validation_failed Malformed / oversized prefix.
 * @pre @p data holds @p len readable bytes (caller-guarded non-NULL).
 * @pre @p reclen / @p body are non-NULL (caller-owned locals).
 * @post On k_ra8_ok, `*body <= *reclen <= len` is NOT yet guaranteed --
 *       the caller still range-checks `*reclen` against @p len.
 * @post On any error the outputs are unspecified and unused.
 * @note Thread-safe: pure read.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t s_pax_reclen(const uint8_t* data, size_t len, size_t* reclen, size_t* body)
{
  uint64_t v      = 0U;
  size_t   digits = 0U;
  size_t   i      = 0U;
  while (i < len) { /* bound: len */
    const uint8_t c = data[i];
    if (c == (uint8_t)' ') {
      break;
    }
    if (c < (uint8_t)'0') {
      return k_ra8_err_validation_failed;
    }
    if (c > (uint8_t)'9') {
      return k_ra8_err_validation_failed;
    }
    if (digits >= (size_t)k_tar_pax_len_max) {
      return k_ra8_err_validation_failed; /* absurd length prefix */
    }
    v = (v * (uint64_t)k_tar_decimal_base) + (uint64_t)(c - (uint8_t)'0');
    digits += 1U;
    i += 1U;
  }
  if (digits == 0U) {
    return k_ra8_err_validation_failed; /* no digits before the space */
  }
  if (i >= len) {
    return k_ra8_err_validation_failed; /* ran out before the space */
  }
  *reclen = (size_t)v;
  *body   = i + 1U;
  return k_ra8_ok;
}

/**
 * @brief Decimal-decode a pax `size` record value with overflow checks.
 * @details Plain non-negative decimal digits only (the pax `size` value
 *          grammar); an empty value, any non-digit byte, or a value that
 *          would overflow uint64 rejects the record fail-closed.
 * @param[in]  val  Value bytes (between '=' and the newline).
 * @param[in]  len  Value length in bytes.
 * @param[out] out  Receives the size.
 * @return ra8_err_t status.
 * @retval k_ra8_ok                    Decoded.
 * @retval k_ra8_err_validation_failed Empty, non-decimal, or overflowing.
 * @pre @p val holds @p len readable bytes (caller-guarded non-NULL).
 * @pre @p out is a caller-owned local (non-NULL).
 * @post On k_ra8_ok `*out` is exact.
 * @post On any error `*out` is unspecified and unused.
 * @note Thread-safe: pure read.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t s_pax_size_value(const uint8_t* val, size_t len, uint64_t* out)
{
  if (len == 0U) {
    return k_ra8_err_validation_failed;
  }
  uint64_t v = 0U;
  for (size_t i = 0U; i < len; ++i) { /* bound: len */
    const uint8_t c = val[i];
    if (c < (uint8_t)'0') {
      return k_ra8_err_validation_failed;
    }
    if (c > (uint8_t)'9') {
      return k_ra8_err_validation_failed;
    }
    const uint64_t d = (uint64_t)(c - (uint8_t)'0');
    if (v > ((UINT64_MAX - d) / (uint64_t)k_tar_decimal_base)) {
      return k_ra8_err_validation_failed; /* would overflow uint64 */
    }
    v = (v * (uint64_t)k_tar_decimal_base) + d;
  }
  *out = v;
  return k_ra8_ok;
}

/**
 * @brief Apply one parsed pax record's key/value to the override outputs.
 * @details `path` copies the value into the caller name buffer (clamped);
 *          `size` decimal-decodes it; every other key is ignored.
 * @param[in]  key       Key bytes.
 * @param[in]  key_len   Key length.
 * @param[in]  val       Value bytes.
 * @param[in]  val_len   Value length.
 * @param[out] name_buf  Caller name buffer (may be NULL if cap 0).
 * @param[in]  name_cap  Name buffer capacity.
 * @param[out] name_len  Receives a copied path length.
 * @param[out] have_path Set when a path was applied.
 * @param[out] size_ovr  Receives a decoded size.
 * @param[out] have_size Set when a size was applied.
 * @return ra8_err_t status.
 * @retval k_ra8_ok                    Record applied or ignored.
 * @retval k_ra8_err_validation_failed A malformed `size` value.
 * @pre The key/value spans lie inside the caller's pax data.
 * @pre The out-flags were initialised by the caller.
 * @post A `path` / `size` record updates exactly its own outputs.
 * @post Unknown keys change nothing.
 * @note Thread-safe: writes only through its out-parameters.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t s_pax_apply(const uint8_t* key,
                             size_t         key_len,
                             const uint8_t* val,
                             size_t         val_len,
                             char*          name_buf,
                             uint16_t       name_cap,
                             uint16_t*      name_len,
                             bool*          have_path,
                             uint64_t*      size_ovr,
                             bool*          have_size)
{
  static const char k_key_path[] = "path";
  static const char k_key_size[] = "size";
  const bool        is_path =
    (key_len == (sizeof(k_key_path) - 1U)) && (memcmp(key, k_key_path, key_len) == 0);
  if (is_path) {
    const size_t want = (val_len > (size_t)name_cap) ? (size_t)name_cap : val_len;
    if (want > 0U) {
      (void)memcpy(name_buf, val, want);
    }
    *name_len  = (uint16_t)want;
    *have_path = true;
    return k_ra8_ok;
  }
  const bool is_size =
    (key_len == (sizeof(k_key_size) - 1U)) && (memcmp(key, k_key_size, key_len) == 0);
  if (is_size) {
    const ra8_err_t verr = s_pax_size_value(val, val_len, size_ovr);
    if (verr != k_ra8_ok) {
      return verr;
    }
    *have_size = true;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_unarch_tar_pax_parse(const uint8_t* data,
                                   size_t         len,
                                   char*          name_buf,
                                   uint16_t       name_cap,
                                   uint16_t*      name_len,
                                   bool*          have_path,
                                   uint64_t*      size_ovr,
                                   bool*          have_size)
{
  RA8_CHECK_NULL_PTR(data, s_tag_tar_f, "pax: null data");
  RA8_CHECK_NULL_PTR(name_len, s_tag_tar_f, "pax: null name_len");
  RA8_CHECK_NULL_PTR(have_path, s_tag_tar_f, "pax: null have_path");
  RA8_CHECK_NULL_PTR(size_ovr, s_tag_tar_f, "pax: null size_ovr");
  RA8_CHECK_NULL_PTR(have_size, s_tag_tar_f, "pax: null have_size");
  if (name_cap > 0U) {
    RA8_CHECK_NULL_PTR(name_buf, s_tag_tar_f, "pax: null name_buf");
  }
  size_t off = 0U;
  while (off < len) { /* bound: every record advances by reclen >= body + 1 */
    size_t          reclen = 0U;
    size_t          body   = 0U;
    const ra8_err_t lerr   = s_pax_reclen(&data[off], len - off, &reclen, &body);
    if (lerr != k_ra8_ok) {
      return lerr;
    }
    if (reclen <= body) {
      return k_ra8_err_validation_failed; /* length cannot cover its prefix */
    }
    if (reclen > (len - off)) {
      return k_ra8_err_validation_failed; /* record overruns the data */
    }
    if (data[off + reclen - 1U] != (uint8_t)'\n') {
      return k_ra8_err_validation_failed; /* record must end in newline */
    }
    const uint8_t* kv     = &data[off + body];
    const size_t   kv_len = reclen - body - 1U; /* strip the newline */
    const void*    eq     = memchr(kv, (int)'=', kv_len);
    if (eq == nullptr) {
      return k_ra8_err_validation_failed; /* no key/value delimiter */
    }
    const size_t    key_len = (size_t)((const uint8_t*)eq - kv);
    const ra8_err_t aerr    = s_pax_apply(kv,
                                          key_len,
                                          &kv[key_len + 1U],
                                          kv_len - key_len - 1U,
                                          name_buf,
                                          name_cap,
                                          name_len,
                                          have_path,
                                          size_ovr,
                                          have_size);
    if (aerr != k_ra8_ok) {
      return aerr;
    }
    off += reclen;
  }
  return k_ra8_ok;
}
/* NOLINTEND(readability-function-size,readability-function-cognitive-complexity) */
