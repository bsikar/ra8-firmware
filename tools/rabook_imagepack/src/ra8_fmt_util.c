/**
 * @file ra8_fmt_util.c
 * @brief Shared file I/O, byte-field readers and the hex dumper for `ra8_fmt`.
 *
 * @details
 * Format-agnostic plumbing every descriptor leans on: slurp a file into an
 * owned blob, write one back out, read the little-endian scalar fields the
 * first-party containers store their headers in, and render a labelled hex +
 * ASCII window -- the byte-level evidence that makes `inspect` a debugging
 * instrument rather than a pretty-printer.
 *
 *
 * [Ring 4 / Domain] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_fmt.h"
#include "ra8_arena.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_fmt_util";

/**
 * @enum ra8_fmt_util_const_t
 * @brief Byte-field widths and hex-dump layout constants.
 * @details Named so the readers and the dumper carry no bare numeric literals.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_fmt_byte_mask  = 0xFFU, /**< Low-byte mask.                      */
  k_fmt_sh8        = 8U,    /**< Little-endian shift, byte 1.        */
  k_fmt_sh16       = 16U,   /**< Little-endian shift, byte 2.        */
  k_fmt_sh24       = 24U,   /**< Little-endian shift, byte 3.        */
  k_fmt_idx0       = 0U,    /**< Byte index 0.                       */
  k_fmt_idx1       = 1U,    /**< Byte index 1.                       */
  k_fmt_idx2       = 2U,    /**< Byte index 2.                       */
  k_fmt_idx3       = 3U,    /**< Byte index 3.                       */
  k_fmt_magic_len  = 4U,    /**< First-party magics are 4 bytes.     */
  k_fmt_ascii_low  = 32U,   /**< Lowest printable ASCII code.        */
  k_fmt_ascii_high = 126U,  /**< Highest printable ASCII code.       */
  k_fmt_ascii_dot  = 46U,   /**< '.' substituted for non-printables. */
} ra8_fmt_util_const_t;

uint16_t ra8_fmt_rd_u16(const uint8_t* buf)
{
  return (uint16_t)((uint32_t)buf[k_fmt_idx0] | ((uint32_t)buf[k_fmt_idx1] << k_fmt_sh8));
}

uint32_t ra8_fmt_rd_u32(const uint8_t* buf)
{
  return (uint32_t)buf[k_fmt_idx0] | ((uint32_t)buf[k_fmt_idx1] << k_fmt_sh8) |
         ((uint32_t)buf[k_fmt_idx2] << k_fmt_sh16) | ((uint32_t)buf[k_fmt_idx3] << k_fmt_sh24);
}

bool ra8_fmt_magic_is(const ra8_fmt_blob_t* src, const char* magic)
{
  if ((src == nullptr) || (src->bytes == nullptr) || (magic == nullptr)) {
    return false;
  }
  if (src->len < (size_t)k_fmt_magic_len) {
    return false;
  }
  return memcmp(src->bytes, magic, (size_t)k_fmt_magic_len) == 0;
}

/**
 * @brief Determine an open file's byte length via a seek to the end.
 * @details Split out of ::ra8_fmt_slurp so that entry point stays within the
 *          statement budget; leaves the cursor rewound to byte 0.
 * @param[in]  fp      Open readable stream (non-NULL).
 * @param[out] out_len Receives the length in bytes.
 * @return Result code.
 * @retval k_ra8_ok               Length measured and the cursor rewound.
 * @retval k_ra8_err_invalid_size The stream is empty or above the input cap.
 * @retval k_ra8_err_not_found    A seek or tell failed (not a regular file).
 * @pre @p fp is open in binary read mode.
 * @pre @p out_len is writable.
 * @post On success the read cursor sits at byte 0.
 * @post On any error `*out_len` is not relied upon by the caller.
 * @note Not thread-safe (moves one stream's cursor).
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_stream_len(FILE* fp, size_t* out_len)
{
  if (fseek(fp, 0, SEEK_END) != 0) {
    return k_ra8_err_not_found;
  }
  const long sz = ftell(fp);
  if (fseek(fp, 0, SEEK_SET) != 0) {
    return k_ra8_err_not_found;
  }
  if (sz <= 0) {
    return k_ra8_err_invalid_size;
  }
  if ((uint64_t)sz > (uint64_t)k_ra8_fmt_max_input) {
    return k_ra8_err_invalid_size;
  }
  *out_len = (size_t)sz;
  return k_ra8_ok;
}

ra8_err_t ra8_fmt_slurp(ra8_arena_t* arena, const char* path, ra8_fmt_blob_t* out)
{
  RA8_CHECK_NULL_PTR(path, s_tag, "path must not be nullptr");
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  *out    = (ra8_fmt_blob_t){};
  FILE* f = fopen(path, "rb");
  if (f == nullptr) {
    return k_ra8_err_not_found;
  }
  size_t    len = 0U;
  ra8_err_t rc  = priv_stream_len(f, &len);
  if (rc != k_ra8_ok) {
    (void)fclose(f);
    return rc;
  }
  uint8_t* buf = (uint8_t*)ra8_arena_alloc(arena, (uint32_t)len);
  if (buf == nullptr) {
    (void)fclose(f);
    return k_ra8_err_no_mem;
  }
  const size_t got = fread(buf, 1U, len, f);
  (void)fclose(f);
  if (got != len) {
    return k_ra8_err_not_found;
  }
  out->bytes = buf;
  out->len   = len;
  return k_ra8_ok;
}

void ra8_fmt_blob_free(ra8_fmt_blob_t* blob)
{
  if (blob == nullptr) {
    return;
  }
  blob->bytes = nullptr;
  blob->len   = 0U;
}

ra8_err_t ra8_fmt_write_file(const char* path, const uint8_t* buf, size_t len)
{
  RA8_CHECK_NULL_PTR(path, s_tag, "path must not be nullptr");
  if ((buf == nullptr) && (len > 0U)) {
    ra8_log_error(s_tag, "buf must not be nullptr for a non-empty write");
    return k_ra8_err_null_ptr;
  }
  FILE* f = fopen(path, "wb");
  if (f == nullptr) {
    return k_ra8_fail;
  }
  const bool wrote_ok = (len == 0U) || (fwrite(buf, 1U, len, f) == len);
  const bool close_ok = (fclose(f) == 0);
  if (!wrote_ok || !close_ok) {
    (void)remove(path);
    return k_ra8_fail;
  }
  return k_ra8_ok;
}

/**
 * @brief Print one 16-byte hex + ASCII dump line.
 * @details Renders the absolute offset, the hex bytes padded to a fixed column
 *          width, then the printable-ASCII gutter with '.' for other codes.
 * @param[in] out   Report sink (non-NULL).
 * @param[in] row   Bytes for this line (non-NULL).
 * @param[in] n     Bytes in this line (1..16).
 * @param[in] addr  Absolute offset of `row[0]`.
 * @pre @p out is an open stream.
 * @pre @p row holds @p n readable bytes and `n <= 16`.
 * @post Exactly one line was written to @p out.
 * @post No state other than the stream is mutated.
 * @note Not thread-safe (writes one stream).
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_hex_line(FILE* out, const uint8_t* row, size_t n, uint64_t addr)
{
  (void)fprintf(out, "  %08llX  ", (unsigned long long)addr);
  for (size_t i = 0U; i < (size_t)k_ra8_fmt_hex_row; ++i) {
    if (i < n) {
      (void)fprintf(out, "%02X ", row[i]);
    } else {
      (void)fprintf(out, "   ");
    }
  }
  (void)fprintf(out, " |");
  for (size_t i = 0U; i < n; ++i) {
    const bool printable =
      (row[i] >= (uint8_t)k_fmt_ascii_low) && (row[i] <= (uint8_t)k_fmt_ascii_high);
    (void)fputc(printable ? (int)row[i] : (int)k_fmt_ascii_dot, out);
  }
  (void)fprintf(out, "|\n");
}

void ra8_fmt_hex_dump(FILE* out, const char* label, const uint8_t* buf, size_t len, uint64_t base)
{
  if ((out == nullptr) || (label == nullptr)) {
    return;
  }
  (void)fprintf(out, "%s (%zu bytes at +%llu):\n", label, len, (unsigned long long)base);
  if ((buf == nullptr) || (len == 0U)) {
    (void)fprintf(out, "  <empty>\n");
    return;
  }
  size_t done = 0U;
  while (done < len) { /* bounded by len */
    size_t n = len - done;
    if (n > (size_t)k_ra8_fmt_hex_row) {
      n = (size_t)k_ra8_fmt_hex_row;
    }
    priv_hex_line(out, &buf[done], n, base + (uint64_t)done);
    done += n;
  }
}
