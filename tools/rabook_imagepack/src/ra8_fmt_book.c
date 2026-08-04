/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_fmt_book.c
 * @brief RBKC (`.rabook`) container inspector for `ra8_fmt`.
 *
 * @details
 * The RBKC container has a fixed header, a strictly increasing `count + 1`
 * offset table, then that many concatenated zlib streams. Dumping the chunk
 * inventory with each entry's byte range is exactly what reveals a malformed
 * book: a non-monotonic table, a final offset that disagrees with the payload
 * length, or an entry whose window runs past the file are all silent at open
 * time but fatal at read time.
 *
 * "One unit" for this container is one whole compiled book -- it is not built
 * from a single image, so it declares no `convert` verb; the compilers under
 * `tools/epub_compile` remain the producers. See the report note in
 * ra8_fmt_registry.c.
 *
 *
 * [Ring 4 / Domain] {World: NS}
 *
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_book.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_fmt.h"
#include "ra8_fmt_internal.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_fmt_book";

/**
 * @enum ra8_fmt_book_const_t
 * @brief Field offsets and widths of the RBKC container header.
 * @details Mirrors the layout tables in ra8_book.h.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_fmt_bk_off_chunk_bytes = 4U,  /**< RBKC: uint32 inflated bytes per chunk. */
  k_fmt_bk_off_inflated    = 8U,  /**< RBKC: uint64 total inflated length.    */
  k_fmt_bk_off_chunk_count = 16U, /**< RBKC: uint32 chunk count.              */
  k_fmt_bk_off_reserved    = 20U, /**< RBKC: uint32 reserved, must be 0.      */
  k_fmt_u64_hi_shift       = 32U, /**< High-word shift for a uint64 field.    */
} ra8_fmt_book_const_t;

/**
 * @brief Read a little-endian uint64 from a byte window.
 * @details The RBKC container stores its offset table as uint64, so the walker
 *          needs a 64-bit reader alongside the 32-bit one.
 * @param[in] buf Source bytes (at least 8 readable).
 * @return The decoded value.
 * @retval 0 All eight source bytes were zero.
 * @pre @p buf holds 8 readable bytes.
 * @pre The field is little-endian per the container spec.
 * @post No state is mutated.
 * @post The result equals the eight bytes assembled little-endian.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint64_t priv_rd_u64(const uint8_t* buf)
{
  const uint64_t lo = (uint64_t)ra8_fmt_rd_u32(buf);
  const uint64_t hi = (uint64_t)ra8_fmt_rd_u32(&buf[4]);
  return lo | (hi << (uint64_t)k_fmt_u64_hi_shift);
}

/**
 * @brief Walk a `count + 1` uint64 offset table, printing and validating it.
 * @details Enforces the invariants the RBKC offset table must hold:
 *          `offset[0] == 0`, the table is strictly increasing, and
 *          `offset[count]` equals the remaining payload length. Each entry's
 *          byte range is printed so a malformed inventory is visible, not
 *          merely reported.
 * @param[in] src        Container bytes (non-NULL).
 * @param[in] table_off  Byte offset of `offset[0]` within the container.
 * @param[in] count      Entry count (the table holds `count + 1` offsets).
 * @param[in] payload    Byte offset where the concatenated streams begin.
 * @param[in] opts       Options carrying the report sink and verbosity.
 * @return Result code.
 * @retval k_ra8_ok                    Table is well-formed.
 * @retval k_ra8_err_validation_failed A bound, ordering or length check failed.
 * @pre @p src holds the whole container.
 * @pre @p count is the header-declared entry count.
 * @post Every anomaly found was written to the report sink.
 * @post No state other than the stream is mutated.
 * @note Not thread-safe (writes one stream).
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_walk_table(const ra8_fmt_blob_t* src,
                                 size_t                table_off,
                                 uint32_t              count,
                                 size_t                payload,
                                 const ra8_fmt_opts_t* opts)
{
  const size_t need = table_off + (((size_t)count + 1U) * sizeof(uint64_t));
  if (need > src->len) {
    (void)fprintf(opts->report, "  ANOMALY offset table runs past the file end\n");
    return k_ra8_err_validation_failed;
  }
  ra8_err_t verdict = k_ra8_ok;
  uint64_t  prev    = 0U;
  if (opts->verbose) {
    (void)fprintf(opts->report, "  entry     start        end       length\n");
  }
  for (uint32_t i = 0U; i <= count; ++i) { /* bounded by count + 1 */
    const uint64_t off = priv_rd_u64(&src->bytes[table_off + ((size_t)i * sizeof(uint64_t))]);
    if ((i == 0U) && (off != 0U)) {
      (void)fprintf(opts->report,
                    "  ANOMALY offset[0] is %llu, must be 0\n",
                    (unsigned long long)off);
      verdict = k_ra8_err_validation_failed;
    }
    /* RBKC specifies a STRICTLY increasing table, so an offset equal to its
     * predecessor is as invalid as one that goes backwards: it declares a
     * zero-length stream that no reader can inflate. Testing `<=` rather than
     * `<` is what makes a table rewritten to a constant fail here. */
    if ((i > 0U) && (off <= prev)) {
      (void)fprintf(opts->report,
                    "  ANOMALY offset[%u]=%llu does not exceed its predecessor %llu -- "
                    "entries %s\n",
                    (unsigned)i,
                    (unsigned long long)off,
                    (unsigned long long)prev,
                    (off < prev) ? "OVERLAP" : "are ZERO-LENGTH");
      verdict = k_ra8_err_validation_failed;
    }
    if ((i > 0U) && opts->verbose && (i <= (uint32_t)k_ra8_fmt_max_dump_rows)) {
      (void)fprintf(opts->report,
                    "  %5u %10llu %10llu %10llu\n",
                    (unsigned)(i - 1U),
                    (unsigned long long)(payload + prev),
                    (unsigned long long)(payload + off),
                    (unsigned long long)(off - prev));
    }
    prev = off;
  }
  const uint64_t payload_len = (uint64_t)(src->len - payload);
  if (prev != payload_len) {
    (void)fprintf(opts->report,
                  "  ANOMALY final offset %llu != payload length %llu (%s)\n",
                  (unsigned long long)prev,
                  (unsigned long long)payload_len,
                  (prev < payload_len) ? "trailing bytes" : "TRUNCATED");
    verdict = k_ra8_err_validation_failed;
  }
  return verdict;
}

bool ra8_fmt_rabook_sniff(const ra8_fmt_blob_t* src)
{
  return ra8_fmt_magic_is(src, "RBKC"); /* MAGIC-OK: the RBKC container fourCC */
}

/**
 * @brief Check the two RBKC header words: reserved and count.
 *
 * @details
 * The RBKC header carries a reserved word that must read zero and a chunk count
 * that must be non-zero. A non-zero reserved word is an anomaly the caller still
 * reports around -- the rest of the container is readable -- whereas a zero
 * count makes every derived table offset meaningless, so the walk cannot
 * continue.
 *
 * @param[in]     out        Report stream.
 * @param[in]     reserved   Reserved header word, expected zero.
 * @param[in]     count      Chunk or page count, expected non-zero.
 * @param[in]     count_name Field name to use in the anomaly line.
 * @param[in,out] verdict    Downgraded to a validation failure on any anomaly.
 * @return Whether the container is coherent enough to keep inspecting.
 * @retval true  @p count is non-zero; the table walk can proceed.
 * @retval false @p count is zero; @p verdict is already a failure.
 *
 * @pre @p out, @p count_name and @p verdict are non-null.
 * @pre @p verdict holds the verdict so far.
 * @post Any anomaly has been reported to @p out.
 * @post @p verdict is never upgraded, only downgraded.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool priv_hdr_sanity(FILE*       out,
                            uint32_t    reserved,
                            uint32_t    count,
                            const char* count_name,
                            ra8_err_t*  verdict)
{
  if (reserved != 0U) {
    (void)fprintf(out, "  ANOMALY reserved word is not zero\n");
    *verdict = k_ra8_err_validation_failed;
  }
  if (count == 0U) {
    (void)fprintf(out, "  ANOMALY %s is zero\n", count_name);
    *verdict = k_ra8_err_validation_failed;
    return false;
  }
  return true;
}

/**
 * @brief Print the RBKC header block.
 *
 * @details
 * Writes the five decoded RBKC container-header fields -- container length,
 * per-chunk inflated size, total inflated size, chunk count and the reserved
 * word -- as a labelled block. This is the first part of an `inspect` dump,
 * emitted before the chunk offset table is walked and sanity-checked.
 *
 * @param[in] out         Report stream.
 * @param[in] len         Container length in bytes.
 * @param[in] chunk_bytes Declared per-chunk inflated size.
 * @param[in] inflated    Declared total inflated size.
 * @param[in] chunks      Declared chunk count.
 * @param[in] reserved    Reserved header word.
 * @return None.
 *
 * @pre @p out is non-null.
 * @pre The values were read from a header of at least the minimum length.
 * @post The header block has been written to @p out.
 * @post No argument is modified.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_print_rabook_hdr(FILE*    out,
                                  size_t   len,
                                  uint32_t chunk_bytes,
                                  uint64_t inflated,
                                  uint32_t chunks,
                                  uint32_t reserved)
{
  (void)fprintf(out, "RBKC rabook container: %zu bytes\n", len);
  (void)fprintf(out, "  chunk_bytes    : %u\n", (unsigned)chunk_bytes);
  (void)fprintf(out, "  inflated_total : %llu\n", (unsigned long long)inflated);
  (void)fprintf(out, "  chunk_count    : %u\n", (unsigned)chunks);
  (void)fprintf(out, "  reserved       : %u\n", (unsigned)reserved);
}

ra8_err_t ra8_fmt_rabook_inspect(const ra8_fmt_blob_t* src, const ra8_fmt_opts_t* opts)
{
  RA8_CHECK_NULL_PTR(src, s_tag, "src must not be nullptr");
  RA8_CHECK_NULL_PTR(opts, s_tag, "opts must not be nullptr");
  if (src->len < (size_t)k_ra8_book_container_header_len) {
    (void)fprintf(opts->report, "RBKC too short to hold a header\n");
    return k_ra8_err_invalid_size;
  }
  const uint32_t chunk_bytes = ra8_fmt_rd_u32(&src->bytes[k_fmt_bk_off_chunk_bytes]);
  const uint64_t inflated    = priv_rd_u64(&src->bytes[k_fmt_bk_off_inflated]);
  const uint32_t chunks      = ra8_fmt_rd_u32(&src->bytes[k_fmt_bk_off_chunk_count]);
  const uint32_t reserved    = ra8_fmt_rd_u32(&src->bytes[k_fmt_bk_off_reserved]);
  priv_print_rabook_hdr(opts->report, src->len, chunk_bytes, inflated, chunks, reserved);

  ra8_err_t verdict = k_ra8_ok;
  if (!priv_hdr_sanity(opts->report, reserved, chunk_bytes, "chunk_bytes", &verdict)) {
    return verdict;
  }
  const uint64_t expect = (inflated + (uint64_t)chunk_bytes - 1U) / (uint64_t)chunk_bytes;
  if ((uint64_t)chunks != expect) {
    (void)fprintf(opts->report,
                  "  ANOMALY chunk_count %u != ceil(inflated/chunk_bytes) = %llu\n",
                  (unsigned)chunks,
                  (unsigned long long)expect);
    verdict = k_ra8_err_validation_failed;
  }
  const size_t    table   = (size_t)k_ra8_book_container_header_len;
  const size_t    payload = table + (((size_t)chunks + 1U) * sizeof(uint64_t));
  const ra8_err_t trc     = priv_walk_table(src, table, chunks, payload, opts);
  if (trc != k_ra8_ok) {
    verdict = trc;
  }
  (void)fprintf(opts->report,
                "verdict: %s\n",
                (verdict == k_ra8_ok) ? "VALID (chunk table monotonic and complete)"
                                      : "INVALID (see anomalies above)");
  return verdict;
}
