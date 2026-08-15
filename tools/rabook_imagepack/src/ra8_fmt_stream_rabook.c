/**
 * @file ra8_fmt_stream_rabook.c
 * @brief Strict callback-driven RBKC and RABOOK1 inspection.
 * @details Opens the container through positioned reads, validates every zlib
 * stream and the complete inner book, then emits the established inventory.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "miniz.h"
#include "ra8_attributes.h"
#include "ra8_book_chunked.h"
#include "ra8_fmt_stream.h"

/** @brief Bounded report formatting constants. */
typedef enum : uint32_t {
  k_report_radix     = 10U,   /**< Decimal digit radix.            */
  k_report_digits    = 20U,   /**< Digits required for `uint64_t`. */
  k_report_rows      = 4096U, /**< Maximum verbose inventory rows. */
  k_report_idx_width = 5U,    /**< Legacy entry-column width.      */
  k_report_num_width = 10U,   /**< Legacy byte-column width.       */
} report_constant_t;

/** @brief Positioned-source binding for the exact book-reader callback. */
typedef struct {
  const ra8_fmt_source_t* source; /**< Borrowed immutable source. */
} source_adapter_t;

/**
 * @brief Append one NUL-terminated literal to the report sink.
 * @details Measures the literal once and delegates its exact payload span.
 * @param[in] report Bound injected text sink.
 * @param[in] text NUL-terminated source text.
 * @return Sink status.
 * @retval k_ra8_ok The complete literal was accepted.
 * @retval other The injected sink rejected the span.
 * @pre @p report and its callback are non-null.
 * @pre @p text is non-null and NUL-terminated.
 * @post Exactly the bytes before NUL were offered once.
 * @post Source text and sink binding remain unchanged.
 * @note Thread safety inherits the injected sink.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_text(const ra8_fmt_sink_t* report, const char* text)
{
  return report->write(report->ctx, (const uint8_t*)text, strlen(text));
}

/**
 * @brief Render one unsigned value with optional left padding.
 * @details Converts through fixed local buffers and emits spaces before digits
 * when the requested field is wider than the decimal spelling.
 * @param[in] report Bound injected text sink.
 * @param[in] value Unsigned value to render.
 * @param[in] width Minimum output field width.
 * @return Sink status.
 * @retval k_ra8_ok Every padding and digit byte was accepted.
 * @retval other The injected sink rejected a span.
 * @pre @p report and its callback are non-null.
 * @pre The fixed digit buffer covers every `uint64_t` value.
 * @post Success emits exactly `max(width, digit_count)` bytes.
 * @post No locale, source, or global state changes.
 * @note Thread safety inherits the injected sink.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_u64(const ra8_fmt_sink_t* report, uint64_t value, uint32_t width)
{
  char     reverse[k_report_digits];
  char     rendered[k_report_digits];
  uint32_t count = 0U;
  do {
    reverse[count++] = (char)('0' + (char)(value % k_report_radix));
    value /= k_report_radix;
  } while (value != 0U);
  while (width > count) {
    const ra8_err_t rc = internal_text(report, " ");
    if (rc != k_ra8_ok) {
      return rc;
    }
    --width;
  }
  for (uint32_t i = 0U; i < count; ++i) {
    rendered[i] = reverse[count - i - 1U];
  }
  return report->write(report->ctx, (const uint8_t*)rendered, count);
}

/**
 * @brief Emit a labelled unsigned field and its terminating newline.
 * @details Writes the label, decimal value, and line ending fail-fast.
 * @param[in] report Bound injected text sink.
 * @param[in] label NUL-terminated field label.
 * @param[in] value Unsigned value to render.
 * @return Sink status.
 * @retval k_ra8_ok The complete field line was accepted.
 * @retval other The first rejected sink span stopped output.
 * @pre @p report and its callback are non-null.
 * @pre @p label is non-null and NUL-terminated.
 * @post Success emits exactly one complete field line.
 * @post Input values and sink binding remain unchanged.
 * @note Thread safety inherits the injected sink.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_field(const ra8_fmt_sink_t* report, const char* label, uint64_t value)
{
  ra8_err_t rc = internal_text(report, label);
  if (rc == k_ra8_ok) {
    rc = internal_u64(report, value, 0U);
  }
  return (rc == k_ra8_ok) ? internal_text(report, "\n") : rc;
}

/**
 * @brief Adapt legal short positioned reads to one exact book read.
 * @details Bounds the complete requested range, then loops over positive short
 * reads until the strict book-reader contract is satisfied.
 * @param[in,out] opaque Bound ::source_adapter_t context.
 * @param[in] offset Absolute container byte offset.
 * @param[out] bytes Destination for exactly @p len bytes.
 * @param[in] len Exact requested byte count.
 * @return Source or range status.
 * @retval k_ra8_ok Exactly @p len bytes were copied.
 * @retval k_ra8_err_invalid_size The range exceeds the captured source.
 * @pre @p opaque resolves to a live immutable source.
 * @pre @p bytes spans @p len bytes when @p len is nonzero.
 * @post Success initializes the complete destination span.
 * @post Source position and adapter binding remain unchanged.
 * @note Supports injected sources that legally return short positive reads.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_exact_read(void* opaque, uint64_t offset, uint8_t* bytes, uint32_t len)
{
  source_adapter_t* adapter = (source_adapter_t*)opaque;
  if ((adapter == nullptr) || (adapter->source == nullptr) || ((bytes == nullptr) && (len != 0U))) {
    return k_ra8_err_null_ptr;
  }
  const ra8_fmt_source_t* source = adapter->source;
  if ((offset > source->size) || ((uint64_t)len > (source->size - offset))) {
    return k_ra8_err_invalid_size;
  }
  size_t done = 0U;
  while (done < (size_t)len) {
    size_t          got = 0U;
    const size_t    ask = (size_t)len - done;
    const ra8_err_t rc  = source->read_at(source->ctx, offset + done, &bytes[done], ask, &got);
    if (rc != k_ra8_ok) {
      return rc;
    }
    if ((got == 0U) || (got > ask)) {
      return k_ra8_err_invalid_size;
    }
    done += got;
  }
  return k_ra8_ok;
}

/**
 * @brief Inflate one RFC 1950 stream through miniz caller storage.
 * @details Parses the zlib wrapper and requires a non-wrapping output buffer.
 * @param[in] src Complete compressed stream bytes.
 * @param[in] src_len Compressed byte length.
 * @param[out] dst Caller-owned inflated destination.
 * @param[in] dst_cap Destination byte capacity.
 * @param[out] out_len Receives the inflated byte length.
 * @return Decompression status.
 * @retval k_ra8_ok One complete stream was inflated.
 * @retval k_ra8_err_validation_failed The stream was malformed or oversized.
 * @pre Input and output spans are non-null and disjoint.
 * @pre @p out_len is non-null and writable.
 * @post Success initializes @p out_len and its destination prefix.
 * @post Failure does not publish a usable output length.
 * @note All decompressor state is bounded local or caller storage.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_inflate(const void* src, size_t src_len, void* dst, size_t dst_cap, size_t* out_len)
{
  if ((src == nullptr) || (dst == nullptr) || (out_len == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  const size_t result = tinfl_decompress_mem_to_mem(
    dst,
    dst_cap,
    src,
    src_len,
    (int)(TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF));
  if (result == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) {
    return k_ra8_err_validation_failed;
  }
  *out_len = result;
  return k_ra8_ok;
}

/**
 * @brief Emit the established RBKC header block.
 * @details Preserves the original field labels and ordering for compatible CLI
 * output after the strict reader has accepted the container.
 * @param[in] report Bound injected text sink.
 * @param[in] source Validated immutable container source.
 * @param[in] reader Open strict chunk reader.
 * @return Sink status.
 * @retval k_ra8_ok The complete header block was accepted.
 * @retval other The first rejected sink span stopped output.
 * @pre All three arguments are non-null and remain live.
 * @pre @p reader describes @p source after a successful open.
 * @post Success emits five labelled container fields.
 * @post Source and reader state remain unchanged.
 * @note The reserved field is emitted as zero because open rejects otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_header(const ra8_fmt_sink_t*     report,
                                 const ra8_fmt_source_t*   source,
                                 const ra8_book_chunked_t* reader)
{
  ra8_err_t rc = internal_text(report, "RBKC rabook container: ");
  if (rc == k_ra8_ok) {
    rc = internal_u64(report, source->size, 0U);
  }
  if (rc == k_ra8_ok) {
    rc = internal_text(report, " bytes\n");
  }
  if (rc == k_ra8_ok) {
    rc = internal_field(report, "  chunk_bytes    : ", reader->chunk_bytes);
  }
  if (rc == k_ra8_ok) {
    rc = internal_field(report, "  inflated_total : ", reader->inflated_total);
  }
  if (rc == k_ra8_ok) {
    rc = internal_field(report, "  chunk_count    : ", reader->chunk_count);
  }
  return (rc == k_ra8_ok) ? internal_field(report, "  reserved       : ", 0U) : rc;
}

/**
 * @brief Emit one established-width chunk inventory row.
 * @details Renders index, absolute start, absolute end, and compressed length
 * with the column widths used by the prior inspector.
 * @param[in] report Bound injected text sink.
 * @param[in] idx Zero-based chunk index.
 * @param[in] begin Absolute compressed-stream start.
 * @param[in] end Absolute exclusive compressed-stream end.
 * @return Sink status.
 * @retval k_ra8_ok The complete row was accepted.
 * @retval other The first rejected sink span stopped output.
 * @pre @p report and its callback are non-null.
 * @pre @p end is not less than @p begin.
 * @post Success emits one newline-terminated row.
 * @post Numeric inputs and sink binding remain unchanged.
 * @note Values wider than their minimum columns are never truncated.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_row(const ra8_fmt_sink_t* report, uint32_t idx, uint64_t begin, uint64_t end)
{
  ra8_err_t rc = internal_text(report, "  ");
  if (rc == k_ra8_ok) {
    rc = internal_u64(report, idx, k_report_idx_width);
  }
  if (rc == k_ra8_ok) {
    rc = internal_u64(report, begin, k_report_num_width + 1U);
  }
  if (rc == k_ra8_ok) {
    rc = internal_u64(report, end, k_report_num_width + 1U);
  }
  if (rc == k_ra8_ok) {
    rc = internal_u64(report, end - begin, k_report_num_width + 1U);
  }
  return (rc == k_ra8_ok) ? internal_text(report, "\n") : rc;
}

/**
 * @brief Emit the bounded chunk inventory when verbose output is enabled.
 * @details Walks validated table entries and reports at most 4096 rows.
 * @param[in] report Bound injected text sink.
 * @param[in] reader Open validated chunk reader.
 * @param[in] verbose Whether inventory output is enabled.
 * @return Sink status.
 * @retval k_ra8_ok Output was disabled or every selected row was accepted.
 * @retval other The first rejected sink span stopped output.
 * @pre @p report and @p reader are non-null.
 * @pre Reader table covers `chunk_count + 1` validated entries.
 * @post Nonverbose success emits no bytes.
 * @post Verbose success emits a header and the bounded row prefix.
 * @note Table traversal is bounded independently of hostile input size.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_table(const ra8_fmt_sink_t* report, const ra8_book_chunked_t* reader, bool verbose)
{
  if (!verbose) {
    return k_ra8_ok;
  }
  ra8_err_t rc   = internal_text(report, "  entry     start        end       length\n");
  uint32_t  rows = reader->chunk_count;
  if (rows > k_report_rows) {
    rows = k_report_rows;
  }
  for (uint32_t i = 0U; (i < rows) && (rc == k_ra8_ok); ++i) {
    const uint64_t begin = reader->payload_off + reader->table[i];
    const uint64_t end   = reader->payload_off + reader->table[i + 1U];
    rc                   = internal_row(report, i, begin, end);
  }
  return rc;
}

/**
 * @brief Emit a validated header, optional inventory, and stable verdict.
 * @details Sequences the established report sections fail-fast after strict
 * outer and inner validation has completed.
 * @param[in] report Bound injected text sink.
 * @param[in] source Validated immutable container source.
 * @param[in] reader Open validated chunk reader.
 * @param[in] verbose Whether to include the bounded chunk inventory.
 * @return Sink status.
 * @retval k_ra8_ok The complete requested report was accepted.
 * @retval other The first rejected sink span stopped output.
 * @pre All pointer arguments are non-null and remain live.
 * @pre Outer streams and the inner book already passed strict validation.
 * @post Success ends with the established valid verdict line.
 * @post No source, reader, or workspace byte changes.
 * @note Reporting occurs only after source stability revalidation.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_report(const ra8_fmt_sink_t*     report,
                                 const ra8_fmt_source_t*   source,
                                 const ra8_book_chunked_t* reader,
                                 bool                      verbose)
{
  ra8_err_t rc = internal_header(report, source, reader);
  if (rc == k_ra8_ok) {
    rc = internal_table(report, reader, verbose);
  }
  return (rc == k_ra8_ok)
           ? internal_text(report, "verdict: VALID (chunk table monotonic and complete)\n")
           : rc;
}

ra8_err_t ra8_fmt_rabook_inspect_stream(const ra8_fmt_source_t*             source,
                                        bool                                verbose,
                                        ra8_fmt_rabook_inspect_workspace_t* workspace,
                                        const ra8_fmt_sink_t*               report)
{
  if ((source == nullptr) || (source->read_at == nullptr) || (workspace == nullptr) ||
      (workspace->table == nullptr) || (workspace->compressed == nullptr) ||
      (workspace->chunk == nullptr) || (workspace->scratch == nullptr) || (report == nullptr) ||
      (report->write == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  source_adapter_t   adapter = {.source = source};
  ra8_book_chunked_t reader  = {};
  ra8_err_t          rc      = ra8_book_chunked_open(&reader,
                                                     internal_exact_read,
                                                     &adapter,
                                                     source->size,
                                                     internal_inflate,
                                                     workspace->table,
                                                     workspace->table_cap,
                                                     workspace->compressed,
                                                     workspace->compressed_cap);
  ra8_book_header_t  header  = {};
  if (rc == k_ra8_ok) {
    rc = ra8_book_chunked_validate_strict(&reader,
                                          workspace->chunk,
                                          workspace->chunk_cap,
                                          workspace->scratch,
                                          workspace->scratch_cap,
                                          &header);
  }
  if ((rc == k_ra8_ok) && (source->validate != nullptr)) {
    rc = source->validate(source->ctx, source->size);
  }
  if (rc != k_ra8_ok) {
    (void)internal_text(report, "verdict: INVALID (strict RBKC/RABOOK1 validation failed)\n");
    return rc;
  }
  return internal_report(report, source, &reader, verbose);
}
