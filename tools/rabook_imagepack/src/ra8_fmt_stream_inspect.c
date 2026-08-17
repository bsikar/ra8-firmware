/**
 * @file ra8_fmt_stream_inspect.c
 * @brief Callback-driven, caller-workspace JOF inspection and reporting.
 * @ingroup grp_ereader
 *
 * @details Audits injected positioned-read sources with caller-owned records,
 * tile, and scratch spans, then emits bounded deterministic report sections.
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_fmt_stream.h"

/** @brief Text formatting constants. */
typedef enum : uint32_t {
  k_fmt_text_dec_digits  = 20U,   /**< Maximum u64 decimal digits. */
  k_fmt_text_hex_digits  = 16U,   /**< Maximum u64 hex digits.     */
  k_fmt_text_hex_row     = 16U,   /**< Bytes per hex dump row.     */
  k_fmt_text_dec_radix   = 10U,   /**< Decimal formatting radix.   */
  k_fmt_text_nibble      = 4U,    /**< Bits in one hex digit.      */
  k_fmt_text_nibble_mask = 15U,   /**< Low-nibble extraction mask. */
  k_fmt_text_max_rows    = 4096U, /**< Maximum verbose tile rows.  */
  k_fmt_text_col_width   = 10U,   /**< Stored-value column width.  */
  k_fmt_text_ascii_low   = 32U,   /**< First printable ASCII byte. */
  k_fmt_text_ascii_high  = 126U,  /**< Last printable ASCII byte.  */
} fmt_text_const_t;

/**
 * @brief Append a byte span to the report sink.
 * @details Delegates exactly once to the injected sink without retaining bytes.
 * @param[in] report Report sink.
 * @param[in] bytes  Bytes to append.
 * @param[in] len    Byte count.
 * @return Sink status.
 * @retval k_ra8_ok Sink accepted the complete span.
 * @retval other Injected sink rejected the span.
 * @pre @p report and its write callback are non-null.
 * @pre @p bytes spans @p len readable bytes.
 * @post Sink callback was invoked exactly once.
 * @post Source bytes and sink binding are unchanged.
 * @note Thread safety inherits the injected sink.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_put(const ra8_fmt_sink_t* report, const uint8_t* bytes, size_t len)
{
  return report->write(report->ctx, bytes, len);
}

/**
 * @brief Append a NUL-terminated literal to the report sink.
 * @details Measures the literal explicitly, then delegates one exact byte span.
 * @param[in] report Report sink.
 * @param[in] text   NUL-terminated text.
 * @return Sink status.
 * @retval k_ra8_ok Sink accepted the complete literal.
 * @retval other Injected sink rejected the literal.
 * @pre @p report and @p text are non-null.
 * @pre @p text is NUL-terminated.
 * @post Exactly the bytes before NUL were offered to the sink.
 * @post No NUL byte is appended.
 * @note Thread safety inherits the injected sink.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_text(const ra8_fmt_sink_t* report, const char* text)
{
  size_t len = 0U;
  while (text[len] != '\0') {
    len++;
  }
  return internal_put(report, (const uint8_t*)text, len);
}

/**
 * @brief Append one character.
 * @details Converts the character to one byte and delegates an exact one-byte span.
 * @param[in] report Report sink.
 * @param[in] value  Character byte.
 * @return Sink status.
 * @retval k_ra8_ok Sink accepted the character byte.
 * @retval other Injected sink rejected the byte.
 * @pre @p report and its callback are non-null.
 * @pre @p value is treated as one byte without locale conversion.
 * @post Exactly one sink write was attempted.
 * @post No caller storage was retained.
 * @note Thread safety inherits the injected sink.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_char(const ra8_fmt_sink_t* report, char value)
{
  const uint8_t byte = (uint8_t)value;
  return internal_put(report, &byte, 1U);
}

/**
 * @brief Append an unsigned decimal with optional left padding.
 * @details Converts in a fixed reverse buffer and emits padding and digits incrementally.
 * @param[in] report Report sink.
 * @param[in] value  Value to format.
 * @param[in] width  Minimum field width.
 * @return Sink status.
 * @retval k_ra8_ok Every padding and digit byte was accepted.
 * @retval other First injected sink failure.
 * @pre @p report and its callback are non-null.
 * @pre Fixed digit capacity covers every `uint64_t` value.
 * @post On success at least @p width characters were emitted.
 * @post On failure no further sink call is made.
 * @note Large widths are streamed and acquire no additional storage.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_u64(const ra8_fmt_sink_t* report, uint64_t value, uint32_t width)
{
  char     digits[k_fmt_text_dec_digits];
  uint32_t count = 0U;
  do {
    digits[count++] = (char)('0' + (char)(value % k_fmt_text_dec_radix));
    value /= k_fmt_text_dec_radix;
  } while (value != 0U);
  ra8_err_t rc = k_ra8_ok;
  for (uint32_t i = count; (i < width) && (rc == k_ra8_ok); ++i) {
    rc = internal_char(report, ' ');
  }
  while ((count != 0U) && (rc == k_ra8_ok)) {
    rc = internal_char(report, digits[--count]);
  }
  return rc;
}

/**
 * @brief Append an uppercase hexadecimal value with zero padding.
 * @details Extracts nibbles into a fixed buffer independent of host endianness.
 * @param[in] report Report sink.
 * @param[in] value  Value to format.
 * @param[in] width  Exact digit width (up to 16).
 * @return Sink status.
 * @retval k_ra8_ok Exactly @p width digits were accepted.
 * @retval k_ra8_err_invalid_size Width exceeds 16 hexadecimal digits.
 * @retval other Injected sink rejected the byte span.
 * @pre @p report and its callback are non-null.
 * @pre @p width may be zero through any `uint32_t` value.
 * @post Success emits uppercase digits with leading zeroes.
 * @post Invalid width invokes no sink callback.
 * @note Values wider than the requested field are intentionally truncated.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_hex(const ra8_fmt_sink_t* report, uint64_t value, uint32_t width)
{
  static const char s_digits[] = "0123456789ABCDEF";
  char              text[k_fmt_text_hex_digits];
  if (width > (uint32_t)sizeof(text)) {
    return k_ra8_err_invalid_size;
  }
  for (uint32_t i = 0U; i < width; ++i) {
    const uint32_t shift = (width - i - 1U) * k_fmt_text_nibble;
    text[i]              = s_digits[(value >> shift) & k_fmt_text_nibble_mask];
  }
  return internal_put(report, (const uint8_t*)text, width);
}

/**
 * @brief Emit one label/value decimal line.
 * @details Sequences prefix, unpadded integer, and suffix with fail-fast sink handling.
 * @param[in] report Report sink.
 * @param[in] label  Prefix including spacing and colon.
 * @param[in] value  Decimal value.
 * @param[in] suffix Text following the value, including newline.
 * @return Sink status.
 * @retval k_ra8_ok Every component was accepted.
 * @retval other First injected sink failure.
 * @pre @p report, @p label, and @p suffix are non-null.
 * @pre Both text fragments are NUL-terminated.
 * @post Success emits the three components contiguously.
 * @post Failure prevents all later components from being offered.
 * @note Thread safety inherits the injected sink.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_value_line(const ra8_fmt_sink_t* report,
                                     const char*           label,
                                     uint64_t              value,
                                     const char*           suffix)
{
  ra8_err_t rc = internal_text(report, label);
  if (rc == k_ra8_ok) {
    rc = internal_u64(report, value, 0U);
  }
  if (rc == k_ra8_ok) {
    rc = internal_text(report, suffix);
  }
  return rc;
}

/**
 * @brief Read an exact source window.
 * @details Converts a short successful callback result into validation failure.
 * @param[in]  source Source object.
 * @param[in]  offset Absolute offset.
 * @param[out] bytes  Destination.
 * @param[in]  len    Exact length.
 * @return Read status or validation failure on a short read.
 * @retval k_ra8_ok Exactly @p len bytes were read.
 * @retval k_ra8_err_validation_failed Callback returned a short success.
 * @retval other Injected source read failed.
 * @pre @p source and its callback are non-null.
 * @pre @p bytes spans @p len writable bytes.
 * @post Success initializes the complete destination range.
 * @post Failure never claims an exact read.
 * @note Thread safety inherits the injected source.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_read_exact(const ra8_fmt_source_t* source, uint64_t offset, uint8_t* bytes, size_t len)
{
  size_t          got = 0U;
  const ra8_err_t rc  = source->read_at(source->ctx, offset, bytes, len, &got);
  if (rc != k_ra8_ok) {
    return rc;
  }
  return (got == len) ? k_ra8_ok : k_ra8_err_validation_failed;
}

/**
 * @brief Emit one labelled hex/ASCII source window.
 * @details Reads through a fixed 16-byte row and renders stable offsets, hex, and ASCII.
 * @param[in] source Source object.
 * @param[in] report Report sink.
 * @param[in] label  Section label.
 * @param[in] offset Absolute source offset.
 * @param[in] len    Byte count.
 * @return Read or sink status.
 * @retval k_ra8_ok Complete labelled dump was emitted.
 * @retval other First exact-read or sink error.
 * @pre Source, report, label, and callbacks are non-null.
 * @pre Requested half-open source window is within `source->size`.
 * @post Success reads and reports exactly @p len source bytes.
 * @post No source or report binding is modified.
 * @note Uses bounded stack storage independent of dump length.
 * @since 0.1.0
 */
/**
 * @brief Emit the label/length/offset header line for a hex/ASCII dump.
 * @param[in] report Report sink.
 * @param[in] label Section label.
 * @param[in] offset Absolute source offset.
 * @param[in] len Byte count.
 * @return Sink status.
 * @retval k_ra8_ok The complete header line was emitted.
 * @retval other First injected sink failure.
 * @pre @p report and @p label are non-null.
 * @post No source or report binding is modified.
 * @note Thread safety inherits the injected sink.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_hex_dump_header(const ra8_fmt_sink_t* report,
                                          const char*           label,
                                          uint64_t              offset,
                                          size_t                len)
{
  ra8_err_t rc = internal_text(report, label);
  if (rc == k_ra8_ok) {
    rc = internal_text(report, " (");
  }
  if (rc == k_ra8_ok) {
    rc = internal_u64(report, len, 0U);
  }
  if (rc == k_ra8_ok) {
    rc = internal_text(report, " bytes at +");
  }
  if (rc == k_ra8_ok) {
    rc = internal_u64(report, offset, 0U);
  }
  if (rc == k_ra8_ok) {
    rc = internal_text(report, "):\n");
  }
  return rc;
}

/**
 * @brief Render one already-loaded hex/ASCII row: offset, hex bytes, ASCII.
 * @param[in] report Report sink.
 * @param[in] offset Absolute source offset of @p row[0].
 * @param[in] row Loaded row bytes, exactly ::k_fmt_text_hex_row long.
 * @param[in] count Valid leading bytes of @p row (the rest pad with spaces).
 * @return Sink status.
 * @retval k_ra8_ok The complete row was emitted.
 * @retval other First injected sink failure.
 * @pre @p report and @p row are non-null; @p count is at most
 *      ::k_fmt_text_hex_row.
 * @post No source or report binding is modified.
 * @note Thread safety inherits the injected sink.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_hex_dump_row(const ra8_fmt_sink_t* report,
                                       uint64_t              offset,
                                       const uint8_t*        row,
                                       size_t                count)
{
  ra8_err_t rc = internal_text(report, "  ");
  if (rc == k_ra8_ok) {
    rc = internal_hex(report, offset, 8U);
  }
  if (rc == k_ra8_ok) {
    rc = internal_text(report, "  ");
  }
  for (size_t i = 0U; (i < (size_t)k_fmt_text_hex_row) && (rc == k_ra8_ok); ++i) {
    if (i < count) {
      rc = internal_hex(report, row[i], 2U);
      if (rc == k_ra8_ok) {
        rc = internal_char(report, ' ');
      }
    } else {
      rc = internal_text(report, "   ");
    }
  }
  if (rc == k_ra8_ok) {
    rc = internal_text(report, " |");
  }
  for (size_t i = 0U; (i < count) && (rc == k_ra8_ok); ++i) {
    const bool printable =
      (row[i] >= (uint8_t)k_fmt_text_ascii_low) && (row[i] <= (uint8_t)k_fmt_text_ascii_high);
    rc = internal_char(report, (char)(printable ? (char)row[i] : '.'));
  }
  if (rc == k_ra8_ok) {
    rc = internal_text(report, "|\n");
  }
  return rc;
}

RA8_INTERNAL
static ra8_err_t internal_hex_dump(const ra8_fmt_source_t* source,
                                   const ra8_fmt_sink_t*   report,
                                   const char*             label,
                                   uint64_t                offset,
                                   size_t                  len)
{
  ra8_err_t rc = internal_hex_dump_header(report, label, offset, len);
  uint8_t   row[k_fmt_text_hex_row];
  size_t    done = 0U;
  while ((done < len) && (rc == k_ra8_ok)) {
    size_t count = len - done;
    if (count > sizeof(row)) {
      count = sizeof(row);
    }
    rc = internal_read_exact(source, offset + done, row, count);
    if (rc == k_ra8_ok) {
      rc = internal_hex_dump_row(report, offset + done, row, count);
    }
    done += count;
  }
  return rc;
}

/**
 * @brief Emit image, tile, and grid dimensions in the legacy CLI layout.
 * @details Formats parsed dimensions in the exact established multi-line order.
 * @param[in] info   Parsed geometry.
 * @param[in] report Report sink.
 * @return Sink status.
 * @retval k_ra8_ok Every field was emitted.
 * @retval other First injected sink failure.
 * @pre @p info and @p report are non-null.
 * @pre Geometry was accepted by the JOF parser.
 * @post Success emits image, tile, and grid lines exactly once.
 * @post Parsed geometry is unchanged.
 * @note Thread safety inherits the injected sink.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_dimensions(const ra8_jof_info_t* info, const ra8_fmt_sink_t* report)
{
  ra8_err_t rc = internal_text(report, "  image      : ");
  if (rc == k_ra8_ok) {
    rc = internal_u64(report, info->width, 0U);
  }
  if (rc == k_ra8_ok) {
    rc = internal_text(report, " x ");
  }
  if (rc == k_ra8_ok) {
    rc = internal_u64(report, info->height, 0U);
  }
  if (rc == k_ra8_ok) {
    rc = internal_text(report, " px\n  tile       : ");
  }
  if (rc == k_ra8_ok) {
    rc = internal_u64(report, info->tile_w, 0U);
  }
  if (rc == k_ra8_ok) {
    rc = internal_text(report, " x ");
  }
  if (rc == k_ra8_ok) {
    rc = internal_u64(report, info->tile_h, 0U);
  }
  if (rc == k_ra8_ok) {
    rc = internal_text(report, " px\n  grid       : ");
  }
  if (rc == k_ra8_ok) {
    rc = internal_u64(report, info->tile_cols, 0U);
  }
  if (rc == k_ra8_ok) {
    rc = internal_text(report, " cols x ");
  }
  if (rc == k_ra8_ok) {
    rc = internal_u64(report, info->tile_rows, 0U);
  }
  if (rc == k_ra8_ok) {
    rc = internal_text(report, " rows\n");
  }
  return rc;
}

/**
 * @brief Emit parsed JOF geometry in the legacy CLI layout.
 * @details Reports source size, dimensions, codec, layout offsets, and long-strip evidence.
 * @param[in] source Source object.
 * @param[in] info   Parsed geometry.
 * @param[in] report Report sink.
 * @return Sink status.
 * @retval k_ra8_ok Every geometry field was emitted.
 * @retval other First injected sink failure.
 * @pre Source, parsed info, report, and callbacks are non-null.
 * @pre Source size and info came from the same parsed object.
 * @post Success emits the complete legacy geometry section.
 * @post Source and parsed metadata remain unchanged.
 * @note Thread safety inherits the injected sink.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_geometry(const ra8_fmt_source_t* source,
                                   const ra8_jof_info_t*   info,
                                   const ra8_fmt_sink_t*   report)
{
  ra8_err_t rc = internal_text(report, "JOF atlas: ");
  if (rc == k_ra8_ok) {
    rc = internal_u64(report, source->size, 0U);
  }
  if (rc == k_ra8_ok) {
    rc = internal_text(report, " bytes\n");
  }
  if (rc == k_ra8_ok) {
    rc = internal_dimensions(info, report);
  }
  if (rc == k_ra8_ok) {
    rc = internal_value_line(report, "  bpp        : ", info->bpp, "\n");
  }
  if (rc == k_ra8_ok) {
    rc = internal_value_line(report, "  codec      : ", info->codec, " (");
  }
  if (rc == k_ra8_ok) {
    rc = internal_text(report,
                       (info->codec == (uint8_t)k_ra8_jof_codec_deflate) ? "deflate)\n" : "raw)\n");
  }
  if (rc == k_ra8_ok) {
    rc = internal_value_line(report, "  tile_count : ", info->tile_count, "\n");
  }
  if (rc == k_ra8_ok) {
    rc = internal_value_line(report, "  index_off  : ", info->index_off, "\n");
  }
  if (rc == k_ra8_ok) {
    rc = internal_value_line(report, "  total_size : ", info->total_size, "\n  longstrip  : ");
  }
  if (rc == k_ra8_ok) {
    rc = internal_text(report,
                       (info->tile_w == info->width) ? "YES (tile_w == width)\n"
                                                     : "NO (tile_w != width)\n");
  }
  return rc;
}

/**
 * @brief Emit the verbose per-tile table.
 * @details Bounds output to the configured row ceiling and reports any elided count.
 * @param[in] records Completed audit records.
 * @param[in] count   Record count.
 * @param[in] report  Report sink.
 * @return Sink status.
 * @retval k_ra8_ok Header, bounded records, and elision notice were emitted.
 * @retval other First injected sink failure.
 * @pre @p records spans @p count completed audit records.
 * @pre @p report and its callback are non-null.
 * @post At most ::k_fmt_text_max_rows records are emitted.
 * @post Record storage is unchanged.
 * @note Output work is bounded even for the maximum legal atlas.
 * @since 0.1.0
 */
/**
 * @brief Render one audit-record row of the verbose per-tile table.
 * @param[in] report Report sink.
 * @param[in] index Zero-based row index, printed in the tile column.
 * @param[in] record Completed audit record for this row.
 * @return Sink status.
 * @retval k_ra8_ok The complete row was emitted.
 * @retval other First injected sink failure.
 * @pre @p report and @p record are non-null.
 * @post Record storage is unchanged.
 * @note Thread safety inherits the injected sink.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_table_row(const ra8_fmt_sink_t*         report,
                                    uint32_t                      index,
                                    const ra8_jof_audit_record_t* record)
{
  ra8_err_t rc = internal_u64(report, index, 8U);
  if (rc == k_ra8_ok) {
    rc = internal_u64(report, record->offset, k_fmt_text_col_width);
  }
  if (rc == k_ra8_ok) {
    rc = internal_u64(report, record->length, k_fmt_text_col_width);
  }
  if (rc == k_ra8_ok) {
    rc = internal_u64(report, record->payload, k_fmt_text_col_width);
  }
  if (rc == k_ra8_ok) {
    rc = internal_u64(report, record->width, 6U);
  }
  if (rc == k_ra8_ok) {
    rc = internal_u64(report, record->height, 6U);
  }
  if (rc == k_ra8_ok) {
    rc = internal_text(report, "   ");
  }
  if (rc == k_ra8_ok) {
    rc = internal_hex(report, record->content_hash, 8U);
  }
  if (rc == k_ra8_ok) {
    rc = internal_char(report, '\n');
  }
  return rc;
}

RA8_INTERNAL
static ra8_err_t
internal_table(const ra8_jof_audit_record_t* records, uint32_t count, const ra8_fmt_sink_t* report)
{
  ra8_err_t rc =
    internal_text(report, "  tile      offset    length   payload   tw    th   hash\n");
  uint32_t shown = count;
  if (shown > k_fmt_text_max_rows) {
    shown = k_fmt_text_max_rows;
  }
  for (uint32_t i = 0U; (i < shown) && (rc == k_ra8_ok); ++i) {
    rc = internal_table_row(report, i, &records[i]);
  }
  if ((shown < count) && (rc == k_ra8_ok)) {
    rc = internal_text(report, "  ... ");
  }
  if ((shown < count) && (rc == k_ra8_ok)) {
    rc = internal_u64(report, count - shown, 0U);
  }
  if ((shown < count) && (rc == k_ra8_ok)) {
    rc = internal_text(report, " more tiles elided\n");
  }
  return rc;
}

ra8_err_t ra8_fmt_jof_inspect_stream(const ra8_fmt_source_t*          source,
                                     bool                             verbose,
                                     ra8_fmt_jof_inspect_workspace_t* workspace,
                                     const ra8_fmt_sink_t*            report)
{
  if ((source == nullptr) || (source->read_at == nullptr) || (workspace == nullptr) ||
      (report == nullptr) || (report->write == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  ra8_jof_audit_workspace_t audit_workspace = {.records     = workspace->records,
                                               .record_cap  = workspace->record_cap,
                                               .tile        = workspace->tile,
                                               .tile_cap    = workspace->tile_cap,
                                               .scratch     = workspace->scratch,
                                               .scratch_cap = workspace->scratch_cap};
  ra8_jof_audit_result_t    result          = {};
  const ra8_err_t           audit_rc =
    ra8_jof_audit(source->read_at, source->ctx, source->size, &audit_workspace, &result);
  if ((audit_rc != k_ra8_ok) && (audit_rc != k_ra8_err_validation_failed)) {
    return audit_rc;
  }
  ra8_err_t rc = internal_geometry(source, &result.info, report);
  if (verbose && (rc == k_ra8_ok)) {
    rc = internal_hex_dump(source, report, "header", 0U, (size_t)k_ra8_jof_hdr_bytes);
  }
  if (verbose && (rc == k_ra8_ok)) {
    rc = internal_hex_dump(source,
                           report,
                           "footer",
                           source->size - (uint64_t)k_ra8_jof_footer_bytes,
                           (size_t)k_ra8_jof_footer_bytes);
  }
  if (verbose && (rc == k_ra8_ok)) {
    rc = internal_table(workspace->records, result.info.tile_count, report);
  }
  if ((result.duplicate_candidates != 0U) && (rc == k_ra8_ok)) {
    rc = internal_value_line(report,
                             "  duplicate hash candidates: ",
                             result.duplicate_candidates,
                             " (diagnostic only)\n");
  }
  if (rc == k_ra8_ok) {
    const char* verdict = "verdict: INVALID (see anomalies above)\n";
    if ((audit_rc == k_ra8_ok) && (result.duplicate_candidates == 0U)) {
      verdict = "verdict: VALID (coverage exact, no duplicate tiles)\n";
    } else if (audit_rc == k_ra8_ok) {
      verdict = "verdict: VALID (coverage exact; duplicate hashes are diagnostic)\n";
    }
    rc = internal_text(report, verdict);
  }
  return (rc == k_ra8_ok) ? audit_rc : rc;
}
