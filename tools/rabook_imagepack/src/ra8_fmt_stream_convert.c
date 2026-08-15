/**
 * @file ra8_fmt_stream_convert.c
 * @brief Callback-driven, caller-workspace image-to-JOF conversion.
 * @details Probes image geometry through positioned reads, derives the exact
 * producer arenas, and streams the firmware JOF producer into an injected
 * durable transaction. Host paths and descriptors never enter this engine.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_fmt_stream.h"
#include "ra8_jof_produce.h"
#include "ra8_webp.h"

/** @brief Image-probe and report constants. */
typedef enum : uint32_t {
  k_convert_band_height    = 256U,  /**< Established media-downloader band height. */
  k_convert_probe_bytes    = 64U,   /**< WebP metadata prefix capacity.            */
  k_convert_jpeg_soi       = 0xD8U, /**< JPEG start-of-image marker.               */
  k_convert_jpeg_eoi       = 0xD9U, /**< JPEG end-of-image marker.                 */
  k_convert_jpeg_sos       = 0xDAU, /**< JPEG start-of-scan marker.                */
  k_convert_jpeg_sof0      = 0xC0U, /**< Baseline sequential frame marker.         */
  k_convert_jpeg_dht       = 0xC4U, /**< Non-frame marker inside SOF number range. */
  k_convert_jpeg_jpg       = 0xC8U, /**< Reserved non-frame marker.                */
  k_convert_jpeg_dac       = 0xCCU, /**< Arithmetic table non-frame marker.        */
  k_convert_jpeg_rst_lo    = 0xD0U, /**< First restart marker.                     */
  k_convert_jpeg_rst_hi    = 0xD7U, /**< Last restart marker.                      */
  k_convert_jpeg_tem       = 0x01U, /**< Standalone TEM marker.                    */
  k_convert_marker         = 0xFFU, /**< JPEG marker introducer.                   */
  k_convert_jpeg_8bit      = 8U,    /**< Supported JPEG sample precision.          */
  k_convert_png_dims_end   = 24U,   /**< PNG prefix through IHDR dimensions.       */
  k_convert_png_w          = 16U,   /**< PNG IHDR width offset.                    */
  k_convert_png_h          = 20U,   /**< PNG IHDR height offset.                   */
  k_convert_decimal_max    = 20U,   /**< Decimal digits in uint64_t.               */
  k_convert_decimal_base   = 10U,   /**< Report integer radix.                     */
  k_convert_u32_high_shift = 24U,   /**< Shift of a big-endian uint32 high byte.   */
  k_convert_jpeg_sof_last  = 0xCFU, /**< Last JPEG SOF-range marker.              */
  k_convert_probe_min      = 12U,   /**< Smallest supported image prefix.           */
} convert_const_t;

/** @brief Accepted source encoding selected by the header probe. */
typedef enum : uint8_t {
  k_convert_kind_jpeg = 0U, /**< Baseline JPEG.                                */
  k_convert_kind_png  = 1U, /**< Non-interlaced PNG (producer validates body). */
  k_convert_kind_webp = 2U, /**< WebP accepted by the firmware facade.         */
} convert_kind_t;

/** @brief Sequential cursor over one positioned source callback. */
typedef struct {
  const ra8_fmt_source_t* source; /**< Borrowed immutable source. */
  uint64_t                offset; /**< Next absolute byte offset. */
} convert_pull_t;

/**
 * @brief Read an exact positioned source window.
 * @details Rejects an out-of-range request and turns a short successful read into protocol failure.
 * @param[in] source Immutable source.
 * @param[in] offset Absolute byte offset.
 * @param[out] bytes Exact destination span.
 * @param[in] len Required byte count.
 * @return Callback status or validation failure on a short success.
 * @retval k_ra8_ok Exactly @p len bytes were read.
 * @retval k_ra8_err_protocol_error Range or returned byte count was inconsistent.
 * @pre Requested half-open range lies within the declared source size.
 * @pre @p source, its callback, and @p bytes are non-null.
 * @post Success initializes all @p len bytes.
 * @post Source position and binding remain unchanged.
 * @note Thread safety inherits the positioned source callback.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_read_exact(const ra8_fmt_source_t* source, uint64_t offset, uint8_t* bytes, size_t len)
{
  if ((offset > source->size) || ((uint64_t)len > (source->size - offset))) {
    return k_ra8_err_protocol_error;
  }
  size_t done = 0U;
  while (done < len) {
    size_t          got = 0U;
    const ra8_err_t rc =
      source->read_at(source->ctx, offset + done, &bytes[done], len - done, &got);
    if (rc != k_ra8_ok) {
      return rc;
    }
    if ((got == 0U) || (got > (len - done))) {
      return k_ra8_err_protocol_error;
    }
    done += got;
  }
  return k_ra8_ok;
}

/**
 * @brief Decode one big-endian uint16 field.
 * @details Assembles exactly two network-order bytes without unaligned access.
 * @param[in] bytes Two readable source bytes.
 * @return Decoded unsigned value.
 * @retval 0 Both input bytes were zero.
 * @pre @p bytes spans two readable bytes.
 * @pre The field is encoded most-significant byte first.
 * @post Input bytes remain unchanged.
 * @post Result equals `(bytes[0] << 8) | bytes[1]`.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint16_t internal_be16(const uint8_t bytes[2])
{
  return (uint16_t)(((uint16_t)bytes[0] << 8U) | bytes[1]);
}

/**
 * @brief Decode one big-endian uint32 field.
 * @details Assembles exactly four network-order bytes without unaligned access.
 * @param[in] bytes Four readable source bytes.
 * @return Decoded unsigned value.
 * @retval 0 All four input bytes were zero.
 * @pre @p bytes spans four readable bytes.
 * @pre The field is encoded most-significant byte first.
 * @post Input bytes remain unchanged.
 * @post Result is the four-byte big-endian assembly.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_be32(const uint8_t bytes[4])
{
  return ((uint32_t)bytes[0] << k_convert_u32_high_shift) | ((uint32_t)bytes[1] << 16U) |
         ((uint32_t)bytes[2] << 8U) | bytes[3];
}

/**
 * @brief Classify JPEG SOF-range markers without mistaking table markers.
 * @details Excludes DHT, reserved JPG, and DAC codes embedded in the numeric SOF range.
 * @param[in] marker Marker code following the 0xFF introducer.
 * @return Whether @p marker is any frame-header marker.
 * @retval true The marker is a JPEG frame header.
 * @retval false The marker lies outside the range or is an excluded table marker.
 * @pre @p marker is the byte following a JPEG marker introducer.
 * @pre No entropy-coded byte stuffing is present at this scan point.
 * @post No state is mutated.
 * @post The result depends only on @p marker.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_is_sof(uint8_t marker)
{
  const bool in_range =
    (marker >= (uint8_t)k_convert_jpeg_sof0) && (marker <= (uint8_t)k_convert_jpeg_sof_last);
  return in_range && (marker != (uint8_t)k_convert_jpeg_dht) &&
         (marker != (uint8_t)k_convert_jpeg_jpg) && (marker != (uint8_t)k_convert_jpeg_dac);
}

/**
 * @brief Find the next non-fill JPEG marker code.
 * @details Skips data before an introducer plus repeated fill bytes and rejects stuffed zero.
 * @param[in] source JPEG source.
 * @param[in,out] offset Scan cursor.
 * @param[out] marker Receives marker code without introducer.
 * @return Read status or protocol failure at EOF/stuffed data.
 * @retval k_ra8_ok A complete non-zero marker code was returned.
 * @retval k_ra8_err_protocol_error Source ended or contained stuffed data before SOF.
 * @pre @p offset starts after SOI and before entropy-coded data.
 * @pre @p source, @p offset, and @p marker are non-null and writable as applicable.
 * @post Success advances beyond one complete marker prefix/code.
 * @post Success initializes @p marker; failure never claims a marker.
 * @note Loop work is bounded by the immutable source length.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_jpeg_marker(const ra8_fmt_source_t* source, uint64_t* offset, uint8_t* marker)
{
  uint8_t byte = 0U;
  do {
    const ra8_err_t rc = internal_read_exact(source, (*offset)++, &byte, 1U);
    if (rc != k_ra8_ok) {
      return rc;
    }
  } while (byte != (uint8_t)k_convert_marker);
  do {
    const ra8_err_t rc = internal_read_exact(source, (*offset)++, &byte, 1U);
    if (rc != k_ra8_ok) {
      return rc;
    }
  } while (byte == (uint8_t)k_convert_marker);
  if (byte == 0U) {
    return k_ra8_err_protocol_error;
  }
  *marker = byte;
  return k_ra8_ok;
}

/**
 * @brief Parse and validate one baseline JPEG SOF0 segment.
 * @details Reads precision and geometry fields, enforcing the producer's axis cap.
 * @param[in] source JPEG source.
 * @param[in] payload Offset of SOF payload after its length field.
 * @param[in] segment_len Declared segment length including its length field.
 * @param[out] out_w Receives width.
 * @param[out] out_h Receives height.
 * @return Geometry status.
 * @retval k_ra8_ok Valid dimensions were stored.
 * @retval k_ra8_err_not_supported Sample precision is not eight bits.
 * @pre @p source and both output pointers are non-null.
 * @pre @p payload follows the two-byte length of the selected SOF0 marker.
 * @post Success initializes both non-zero output dimensions.
 * @post Failure performs no filesystem or transaction I/O.
 * @note Thread safety inherits the positioned source callback.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_jpeg_sof0(const ra8_fmt_source_t* source,
                                    uint64_t                payload,
                                    uint16_t                segment_len,
                                    uint16_t*               out_w,
                                    uint16_t*               out_h)
{
  uint8_t fields[5U];
  if (segment_len < 8U) {
    return k_ra8_err_protocol_error;
  }
  ra8_err_t rc = internal_read_exact(source, payload, fields, sizeof(fields));
  if (rc != k_ra8_ok) {
    return rc;
  }
  const uint16_t height = internal_be16(&fields[1]);
  const uint16_t width  = internal_be16(&fields[3]);
  if (fields[0] != (uint8_t)k_convert_jpeg_8bit) {
    return k_ra8_err_not_supported;
  }
  if ((width == 0U) || (height == 0U) || ((uint32_t)width > (uint32_t)k_ra8_jof_max_dim) ||
      ((uint32_t)height > (uint32_t)k_ra8_jof_max_dim)) {
    return k_ra8_err_invalid_size;
  }
  *out_w = width;
  *out_h = height;
  return k_ra8_ok;
}

/**
 * @brief Walk a JPEG marker chain to its baseline SOF0 dimensions.
 * @details Skips length-delimited metadata without buffering it and rejects non-baseline SOF.
 * @param[in] source Source whose SOI was already recognized.
 * @param[out] out_w Receives width.
 * @param[out] out_h Receives height.
 * @return Geometry, unsupported-mode, or protocol status.
 * @retval k_ra8_ok Baseline SOF0 dimensions were returned.
 * @retval k_ra8_err_not_supported A different JPEG frame mode was declared.
 * @pre @p source begins with a recognized SOI marker.
 * @pre Both output pointers are non-null and writable.
 * @post Success initializes both output dimensions.
 * @post Source position and bytes remain unchanged.
 * @note Loop work is bounded by the immutable source byte length.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_jpeg_dims(const ra8_fmt_source_t* source, uint16_t* out_w, uint16_t* out_h)
{
  uint64_t offset = 2U;
  while (offset < source->size) {
    uint8_t   marker = 0U;
    ra8_err_t rc     = internal_jpeg_marker(source, &offset, &marker);
    if (rc != k_ra8_ok) {
      return rc;
    }
    const bool standalone =
      (marker == (uint8_t)k_convert_jpeg_soi) || (marker == (uint8_t)k_convert_jpeg_tem) ||
      ((marker >= (uint8_t)k_convert_jpeg_rst_lo) && (marker <= (uint8_t)k_convert_jpeg_rst_hi));
    if (standalone) {
      continue;
    }
    if ((marker == (uint8_t)k_convert_jpeg_eoi) || (marker == (uint8_t)k_convert_jpeg_sos)) {
      return k_ra8_err_protocol_error;
    }
    uint8_t length_bytes[2U];
    rc = internal_read_exact(source, offset, length_bytes, sizeof(length_bytes));
    if (rc != k_ra8_ok) {
      return rc;
    }
    const uint16_t segment_len = internal_be16(length_bytes);
    if (segment_len < 2U) {
      return k_ra8_err_protocol_error;
    }
    if (internal_is_sof(marker)) {
      return (marker == (uint8_t)k_convert_jpeg_sof0)
               ? internal_jpeg_sof0(source, offset + 2U, segment_len, out_w, out_h)
               : k_ra8_err_not_supported;
    }
    offset += segment_len;
    if (offset > source->size) {
      return k_ra8_err_protocol_error;
    }
  }
  return k_ra8_err_protocol_error;
}

/**
 * @brief Probe PNG dimensions from the fixed IHDR prefix.
 * @details Decodes both big-endian fields and enforces the JOF per-axis cap.
 * @param[in] prefix Prefix through both IHDR dimension fields.
 * @param[out] out_w Receives width.
 * @param[out] out_h Receives height.
 * @return Validated geometry status.
 * @retval k_ra8_ok Both dimensions were accepted and stored.
 * @retval k_ra8_err_invalid_size A dimension was zero or over the cap.
 * @pre @p prefix spans through byte ::k_convert_png_dims_end.
 * @pre Both output pointers are non-null and writable.
 * @post Success initializes both narrowed dimensions.
 * @post Prefix bytes remain unchanged.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_png_dims(const uint8_t prefix[k_convert_png_dims_end], uint16_t* out_w, uint16_t* out_h)
{
  const uint32_t width  = internal_be32(&prefix[k_convert_png_w]);
  const uint32_t height = internal_be32(&prefix[k_convert_png_h]);
  if ((width == 0U) || (height == 0U) || (width > (uint32_t)k_ra8_jof_max_dim) ||
      (height > (uint32_t)k_ra8_jof_max_dim)) {
    return k_ra8_err_invalid_size;
  }
  *out_w = (uint16_t)width;
  *out_h = (uint16_t)height;
  return k_ra8_ok;
}

/**
 * @brief Probe the exact source kind and dimensions through positioned reads.
 * @details Mirrors firmware dispatch across JPEG, PNG, and RIFF/WebP headers.
 * @param[in] source Immutable image source.
 * @param[out] kind Receives accepted encoding kind.
 * @param[out] out_w Receives width.
 * @param[out] out_h Receives height.
 * @return Probe status.
 * @retval k_ra8_ok Encoding and dimensions were returned.
 * @retval k_ra8_err_not_supported The prefix matched no supported source.
 * @pre @p source and its positioned callback are non-null.
 * @pre Every output pointer is non-null and writable.
 * @post Success initializes kind, width, and height together.
 * @post No backend cursor, source byte, or filesystem object changed.
 * @note Reads at most one fixed prefix plus the JPEG marker chain.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_probe(const ra8_fmt_source_t* source,
                                convert_kind_t*         kind,
                                uint16_t*               out_w,
                                uint16_t*               out_h)
{
  static const uint8_t png_magic[8U] =
    {0x89U, 'P', 'N', 'G', 0x0DU, 0x0AU, 0x1AU, 0x0AU}; /* MAGIC-OK */
  uint8_t prefix[k_convert_probe_bytes];
  size_t  count = sizeof(prefix);
  if (source->size < (uint64_t)count) {
    count = (size_t)source->size;
  }
  if (count < k_convert_probe_min) {
    return k_ra8_err_not_supported;
  }
  ra8_err_t rc = internal_read_exact(source, 0U, prefix, count);
  if (rc != k_ra8_ok) {
    return rc;
  }
  if ((prefix[0] == (uint8_t)k_convert_marker) && (prefix[1] == (uint8_t)k_convert_jpeg_soi)) {
    *kind = k_convert_kind_jpeg;
    return internal_jpeg_dims(source, out_w, out_h);
  }
  if (memcmp(prefix, png_magic, sizeof(png_magic)) == 0) {
    if (count < (size_t)k_convert_png_dims_end) {
      return k_ra8_err_not_supported;
    }
    *kind = k_convert_kind_png;
    return internal_png_dims(prefix, out_w, out_h);
  }
  if ((memcmp(prefix, "RIFF", 4U) == 0) && (memcmp(&prefix[8], "WEBP", 4U) == 0)) {
    uint32_t width  = 0U;
    uint32_t height = 0U;
    rc              = ra8_webp_get_info(prefix, count, &width, &height);
    if (rc != k_ra8_ok) {
      return rc;
    }
    if ((width == 0U) || (height == 0U) || (width > (uint32_t)k_ra8_jof_max_dim) ||
        (height > (uint32_t)k_ra8_jof_max_dim)) {
      return k_ra8_err_invalid_size;
    }
    *kind  = k_convert_kind_webp;
    *out_w = (uint16_t)width;
    *out_h = (uint16_t)height;
    return k_ra8_ok;
  }
  return k_ra8_err_not_supported;
}

/**
 * @brief Producer pull adapter over positioned source reads.
 * @details Converts a sequential producer request into a bounded absolute read.
 * @param[in,out] ctx Bound ::convert_pull_t cursor.
 * @param[out] bytes Destination for up to @p cap bytes.
 * @param[in] cap Requested byte capacity.
 * @param[out] got Receives bytes supplied, including zero at EOF.
 * @return Positioned source status.
 * @retval k_ra8_ok A legal span or EOF was returned.
 * @retval k_ra8_err_protocol_error Callback claimed more than @p cap bytes.
 * @pre @p ctx and @p got are non-null.
 * @pre @p bytes spans @p cap bytes when @p cap is non-zero.
 * @post Success advances the cursor by exactly `*got`.
 * @post The cursor never advances beyond declared source size.
 * @note Not thread-safe through one shared cursor.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_pull(void* ctx, uint8_t* bytes, size_t cap, size_t* got)
{
  convert_pull_t* pull = (convert_pull_t*)ctx;
  if ((pull == nullptr) || (got == nullptr) || ((bytes == nullptr) && (cap != 0U))) {
    return k_ra8_err_null_ptr;
  }
  *got = 0U;
  if ((cap == 0U) || (pull->offset >= pull->source->size)) {
    return k_ra8_ok;
  }
  uint64_t remain = pull->source->size - pull->offset;
  if ((uint64_t)cap > remain) {
    cap = (size_t)remain;
  }
  const ra8_err_t rc = pull->source->read_at(pull->source->ctx, pull->offset, bytes, cap, got);
  if ((rc == k_ra8_ok) && (*got <= cap)) {
    pull->offset += *got;
  } else if (rc == k_ra8_ok) {
    return k_ra8_err_protocol_error;
  }
  return rc;
}

/**
 * @brief Producer sink adapter over the durable transaction append operation.
 * @details Delegates one exact producer byte span to the unpublished stage.
 * @param[in,out] ctx Active ::ra8_fmt_transaction_t.
 * @param[in] bytes Producer output bytes.
 * @param[in] len Exact append length.
 * @return Transaction append status.
 * @retval k_ra8_ok Complete span was staged.
 * @retval other Injected transaction rejected the append.
 * @pre @p ctx names an active transaction with an append operation.
 * @pre @p bytes spans @p len readable bytes.
 * @post Success stages exactly @p len additional bytes.
 * @post No publication occurs in this adapter.
 * @note Thread safety inherits the transaction backend.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_append(void* ctx, const uint8_t* bytes, size_t len)
{
  ra8_fmt_transaction_t* transaction = (ra8_fmt_transaction_t*)ctx;
  return transaction->ops->append(transaction->ctx, bytes, len);
}

/**
 * @brief Abort an active transaction after any pre-publication error.
 * @details Null-safe cleanup delegates only when a complete abort operation exists.
 * @param[in,out] transaction Transaction to discard, or null.
 * @pre @p transaction is null or caller-owned transaction state.
 * @pre No concurrent append or commit uses the same transaction.
 * @post A complete transaction receives exactly one abort call.
 * @post Null or partial transaction bindings are ignored safely.
 * @note Cleanup has no status channel by transaction contract.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_abort(ra8_fmt_transaction_t* transaction)
{
  if ((transaction != nullptr) && (transaction->ops != nullptr) &&
      (transaction->ops->abort != nullptr)) {
    transaction->ops->abort(transaction->ctx);
  }
}

/**
 * @brief Append a NUL-terminated text fragment to a report sink.
 * @details Measures the fragment and delegates one exact byte span.
 * @param[in] report Bound report sink.
 * @param[in] text NUL-terminated text.
 * @return Injected sink status.
 * @retval k_ra8_ok Complete text was accepted.
 * @pre @p report and its callback are non-null.
 * @pre @p text is non-null and NUL-terminated.
 * @post Exactly the bytes before NUL were offered once.
 * @post Neither report binding nor source text changed.
 * @note Thread safety inherits the report sink.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_text(const ra8_fmt_sink_t* report, const char* text)
{
  return report->write(report->ctx, (const uint8_t*)text, strlen(text));
}

/**
 * @brief Append one unsigned decimal to a report sink.
 * @details Converts through fixed local buffers without locale-sensitive formatting.
 * @param[in] report Bound report sink.
 * @param[in] value Unsigned value to render.
 * @return Injected sink status.
 * @retval k_ra8_ok Complete decimal spelling was accepted.
 * @pre @p report and its callback are non-null.
 * @pre Fixed digit storage covers every uint64_t value.
 * @post One non-empty base-ten spelling was offered.
 * @post No global or filesystem state changed.
 * @note Thread safety inherits the report sink.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_u64(const ra8_fmt_sink_t* report, uint64_t value)
{
  char   reverse[k_convert_decimal_max];
  size_t count = 0U;
  do {
    reverse[count++] = (char)('0' + (char)(value % k_convert_decimal_base));
    value /= k_convert_decimal_base;
  } while (value != 0U);
  char text[k_convert_decimal_max];
  for (size_t i = 0U; i < count; ++i) {
    text[i] = reverse[count - i - 1U];
  }
  return report->write(report->ctx, (const uint8_t*)text, count);
}

/**
 * @brief Append one numeric report field and its suffix fail-fast.
 * @details Centralizes bounded report composition without local formatting macros.
 * @param[in] report Bound report sink.
 * @param[in] value Unsigned field value.
 * @param[in] suffix NUL-terminated field suffix.
 * @param[in,out] status Current and resulting report status.
 * @pre Every pointer argument is non-null.
 * @pre @p suffix is NUL-terminated and `*status` is canonical.
 * @post Successful status attempts number then suffix in order.
 * @post A failing status prevents all later writes.
 * @note Thread safety inherits the report sink.
 * @since 0.1.0
 */
RA8_INTERNAL
static void
internal_field(const ra8_fmt_sink_t* report, uint64_t value, const char* suffix, ra8_err_t* status)
{
  if (*status == k_ra8_ok) {
    *status = internal_u64(report, value);
  }
  if (*status == k_ra8_ok) {
    *status = internal_text(report, suffix);
  }
}

/**
 * @brief Emit the established successful-conversion report line.
 * @details Reproduces legacy field order and punctuation through bounded fragments.
 * @param[in] report Injected report sink.
 * @param[in] info Produced JOF geometry.
 * @param[in] output_name Destination spelling.
 * @return First sink failure or success.
 * @retval k_ra8_ok The complete report line was accepted.
 * @retval other The injected sink rejected a fragment.
 * @pre @p report, @p info, and @p output_name are non-null.
 * @pre @p output_name is NUL-terminated and @p info describes committed bytes.
 * @post Success emits one newline-terminated conversion summary.
 * @post Produced geometry and destination spelling remain unchanged.
 * @note Report work is independent of artifact length.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_report(const ra8_fmt_sink_t* report, const ra8_jof_info_t* info, const char* output_name)
{
  ra8_err_t rc = internal_text(report, "convert: ");
  internal_field(report, info->width, "x", &rc);
  internal_field(report, info->height, " bpp=", &rc);
  internal_field(report, info->bpp, " band=", &rc);
  internal_field(report, info->tile_h, " tiles=", &rc);
  internal_field(report, info->tile_count, " -> ", &rc);
  if (rc == k_ra8_ok) {
    rc = internal_text(report, output_name);
  }
  if (rc == k_ra8_ok) {
    rc = internal_text(report, " (");
  }
  internal_field(report, info->total_size, " bytes)\n", &rc);
  return rc;
}

ra8_err_t ra8_fmt_jof_convert_requirements(const ra8_fmt_source_t*             source,
                                           ra8_fmt_jof_convert_requirements_t* out)
{
  if ((source == nullptr) || (source->read_at == nullptr) || (out == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *out                = (ra8_fmt_jof_convert_requirements_t){};
  convert_kind_t kind = k_convert_kind_jpeg;
  ra8_err_t      rc   = internal_probe(source, &kind, &out->width, &out->height);
  if (rc != k_ra8_ok) {
    return rc;
  }
  out->tile_width = out->width;
  out->tile_height =
    (out->height < (uint16_t)k_convert_band_height) ? out->height : (uint16_t)k_convert_band_height;
  out->work_bytes = ra8_jof_work_bytes(out->width, out->height, out->tile_width, out->tile_height);
  if (out->work_bytes == 0U) {
    return k_ra8_err_invalid_size;
  }
  if (kind == k_convert_kind_webp) {
    if (source->size > (uint64_t)UINT32_MAX) {
      return k_ra8_err_invalid_size;
    }
    out->webp_work_bytes = ra8_jof_webp_work_bytes(out->width, out->height, (uint32_t)source->size);
    if (out->webp_work_bytes == 0U) {
      return k_ra8_err_invalid_size;
    }
  }
  return k_ra8_ok;
}

ra8_err_t ra8_fmt_jof_convert_stream(const ra8_fmt_source_t*                   source,
                                     const ra8_fmt_jof_convert_requirements_t* requirements,
                                     ra8_fmt_jof_convert_workspace_t*          workspace,
                                     ra8_fmt_transaction_t*                    transaction,
                                     const ra8_fmt_sink_t*                     report,
                                     const char*                               output_name)
{
  if ((source == nullptr) || (source->read_at == nullptr) || (requirements == nullptr) ||
      (workspace == nullptr) || (transaction == nullptr) || (transaction->ops == nullptr) ||
      (transaction->ops->append == nullptr) || (transaction->ops->commit == nullptr) ||
      (transaction->ops->abort == nullptr) || (report == nullptr) || (report->write == nullptr) ||
      (output_name == nullptr)) {
    internal_abort(transaction);
    return k_ra8_err_null_ptr;
  }
  const bool webp_missing = (requirements->webp_work_bytes != 0U) &&
                            ((workspace->webp_work == nullptr) ||
                             (workspace->webp_work_cap < requirements->webp_work_bytes));
  if ((workspace->work == nullptr) || (workspace->work_cap < requirements->work_bytes) ||
      webp_missing) {
    internal_abort(transaction);
    return k_ra8_err_invalid_size;
  }
  convert_pull_t              pull = {.source = source, .offset = 0U};
  const ra8_jof_produce_cfg_t cfg  = {
    .pull          = internal_pull,
    .pull_ctx      = &pull,
    .sink          = internal_append,
    .sink_ctx      = transaction,
    .tile_w        = requirements->tile_width,
    .tile_h        = requirements->tile_height,
    .codec         = (uint8_t)k_ra8_jof_codec_deflate,
    .max_width     = requirements->width,
    .max_height    = requirements->height,
    .work          = workspace->work,
    .work_cap      = workspace->work_cap,
    .webp_work     = workspace->webp_work,
    .webp_work_cap = workspace->webp_work_cap,
  };
  ra8_jof_info_t info = {};
  ra8_err_t      rc   = ra8_jof_produce(&cfg, &info);
  if (rc != k_ra8_ok) {
    internal_abort(transaction);
    return rc;
  }
  rc = transaction->ops->commit(transaction->ctx);
  if (rc != k_ra8_ok) {
    internal_abort(transaction);
    return rc;
  }
  return internal_report(report, &info, output_name);
}
