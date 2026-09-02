/**
 * @file ra8_fmt_stream_verify.c
 * @brief Caller-workspace two-spool JOF round-trip verification.
 * @details Produces an independently decoded one-row reference atlas and the
 * normal banded subject atlas, validates both, then compares one row at a time.
 * Host paths and descriptors remain behind injected source, spool, transaction,
 * and report callbacks.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "jof_produce.h"
#include "ra8_attributes.h"
#include "ra8_fmt_stream.h"

/** @brief Verification geometry, report, and probe constants. */
typedef enum : uint32_t {
  k_verify_band_height  = 256U, /**< Production band height under test. */
  k_verify_diffs_max    = 8U,   /**< Maximum detailed byte differences. */
  k_verify_decimal_max  = 20U,  /**< Digits in one uint64_t.            */
  k_verify_decimal      = 10U,  /**< Decimal report radix.              */
  k_verify_hex_radix    = 16U,  /**< Hexadecimal report radix.          */
  k_verify_ppm_max_text = 5U,   /**< Bytes in the PPM maximum line.     */
} verify_const_t;

/** @brief Sequential cursor over one positioned source context. */
typedef struct {
  const ra8_fmt_source_t* source; /**< Immutable positioned source. */
  uint64_t                offset; /**< Next sequential byte offset. */
} verify_pull_t;

/** @brief Comparison state shared by bounded tile/row helpers. */
typedef struct {
  /** Exact geometry and capacities. */
  const ra8_fmt_jof_verify_requirements_t* need;
  /** Phase-reused caller buffers. */
  ra8_fmt_jof_verify_workspace_t* work;
  /** Sealed reference atlas. */
  const ra8_fmt_spool_t* ref;
  /** Sealed banded atlas. */
  const ra8_fmt_spool_t* got;
  /** Parsed reference geometry. */
  const jof_info_t* rinfo;
  /** Parsed subject geometry. */
  const jof_info_t* ginfo;
  /** Optional unpublished PPM stage. */
  ra8_fmt_transaction_t* dump;
  /** Human-readable report sink. */
  const ra8_fmt_sink_t* report;
  /** Differing byte count. */
  uint64_t diffs;
  /** Detailed differences emitted. */
  uint32_t shown;
  /** PPM stage remains complete. */
  bool dump_ok;
} verify_compare_t;

/**
 * @brief Validate a source through its optional stability callback.
 * @details Delegates immutable-view revalidation while preserving optional bindings.
 * @param[in] source Source to validate.
 * @return Stability callback status or success when validation is absent.
 * @retval k_ra8_ok The source is unchanged or has no validator.
 * @retval other The backend observed mutation or validation failure.
 * @pre @p source is non-null.
 * @pre The captured size still describes the intended object.
 * @post No source position changes.
 * @post Success permits continued trust in the captured view.
 * @note Thread safety inherits the backend validator.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_stable(const ra8_fmt_source_t* source)
{
  return (source->validate == nullptr) ? k_ra8_ok : source->validate(source->ctx, source->size);
}

/**
 * @brief Pull the next bounded source prefix for streamed JOF production.
 * @details Reads at the cursor's current offset, validates the backend's
 * reported count, and advances only after a successful bounded read.
 * @param[in,out] ctx Mutable ::verify_pull_t cursor context.
 * @param[out] bytes Destination for the next source prefix.
 * @param[in] cap Writable capacity of @p bytes.
 * @param[out] got Number of bytes returned by the source.
 * @return Canonical stream callback status.
 * @retval k_ra8_ok A valid prefix or clean end-of-source was returned.
 * @retval k_ra8_err_null_ptr A required callback argument was null.
 * @retval k_ra8_err_protocol_error The source reported more than @p cap.
 * @retval other Propagated source read error.
 * @pre Nonzero @p cap requires writable @p bytes storage.
 * @pre The cursor source and @p got remain valid for the call.
 * @post Success advances the cursor by exactly @p *got bytes.
 * @post Failure never advances past an unvalidated source result.
 * @note The callback performs no allocation and never retains @p bytes.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_pull(void* ctx, uint8_t* bytes, size_t cap, size_t* got)
{
  verify_pull_t* pull = (verify_pull_t*)ctx;
  if ((pull == nullptr) || (got == nullptr) || ((bytes == nullptr) && (cap != 0U))) {
    return k_ra8_err_null_ptr;
  }
  *got = 0U;
  if ((cap == 0U) || (pull->offset >= pull->source->size)) {
    return k_ra8_ok;
  }
  const uint64_t remain = pull->source->size - pull->offset;
  if ((uint64_t)cap > remain) {
    cap = (size_t)remain;
  }
  ra8_err_t rc = pull->source->read_at(pull->source->ctx, pull->offset, bytes, cap, got);
  if ((rc == k_ra8_ok) && (*got <= cap)) {
    pull->offset += *got;
  } else if (rc == k_ra8_ok) {
    rc = k_ra8_err_protocol_error;
  }
  return rc;
}

/**
 * @brief Append one produced JOF span to the injected verifier spool.
 * @details Adapts the producer sink signature directly to the caller-owned
 * spool without retaining the supplied byte span.
 * @param[in,out] ctx Caller-owned ::ra8_fmt_spool_t descriptor.
 * @param[in] bytes Produced JOF bytes to append.
 * @param[in] len Number of bytes in @p bytes.
 * @return Canonical spool append status.
 * @retval k_ra8_ok The complete span was accepted.
 * @retval other Propagated spool append failure.
 * @pre @p ctx points to a bound spool with a valid append callback.
 * @pre Nonzero @p len requires a readable @p bytes span.
 * @post The callback result exactly matches the injected append result.
 * @post This adapter retains no caller-owned pointer.
 * @note Capacity and publication policy belong to the injected spool.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_spool_append(void* ctx, const uint8_t* bytes, size_t len)
{
  ra8_fmt_spool_t* spool = (ra8_fmt_spool_t*)ctx;
  return spool->append(spool->ctx, bytes, len);
}

/**
 * @brief Produce and seal one deflate JOF into an injected spool.
 * @details Binds a sequential pull cursor and exact caller arenas to the producer.
 * @param[in] source Encoded immutable source.
 * @param[in] need Exact verifier requirements.
 * @param[in] tile_h Requested reference-row or subject-band height.
 * @param[in,out] work Caller-owned producer arenas.
 * @param[in,out] spool Empty append/seal scratch binding.
 * @param[out] info Receives produced geometry and extent.
 * @return Producer, stability, or seal status.
 * @retval k_ra8_ok The complete atlas is stable and sealed.
 * @retval other Source, producer, mutation, or spool status.
 * @pre Every pointer and callback binding is valid.
 * @pre Workspace capacities satisfy @p need.
 * @post Success leaves @p spool sealed at @p info total size.
 * @post Failure publishes no output transaction.
 * @note Producer execution is bounded by source geometry and caller storage.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_produce(const ra8_fmt_source_t*                  source,
                                  const ra8_fmt_jof_verify_requirements_t* need,
                                  uint16_t                                 tile_h,
                                  ra8_fmt_jof_verify_workspace_t*          work,
                                  ra8_fmt_spool_t*                         spool,
                                  jof_info_t*                              info)
{
  verify_pull_t           pull = {.source = source, .offset = 0U};
  const jof_produce_cfg_t cfg  = {
    .pull          = internal_pull,
    .pull_ctx      = &pull,
    .sink          = internal_spool_append,
    .sink_ctx      = spool,
    .tile_w        = need->width,
    .tile_h        = tile_h,
    .codec         = (uint8_t)k_jof_codec_deflate,
    .max_width     = need->width,
    .max_height    = need->height,
    .work          = work->work,
    .work_cap      = work->work_cap,
    .webp_work     = work->webp_work,
    .webp_work_cap = work->webp_work_cap,
  };
  ra8_err_t rc = internal_stable(source);
  if (rc == k_ra8_ok) {
    rc = jof_produce(&cfg, info);
  }
  if (rc == k_ra8_ok) {
    rc = internal_stable(source);
  }
  if (rc == k_ra8_ok) {
    rc = spool->seal(spool->ctx, info->total_size);
  }
  return rc;
}

/**
 * @brief Append one NUL-terminated report fragment.
 * @details Measures the fixed spelling and delegates one exact sink write.
 * @param[in] sink Bound report sink.
 * @param[in] text NUL-terminated spelling.
 * @return Sink status.
 * @retval k_ra8_ok The complete spelling was appended.
 * @retval other Injected sink failure.
 * @pre @p sink and callback are valid.
 * @pre @p text is NUL-terminated.
 * @post Success appends exactly strlen(@p text) bytes.
 * @post No input byte changes.
 * @note Thread safety inherits the sink.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_text(const ra8_fmt_sink_t* sink, const char* text)
{
  return sink->write(sink->ctx, (const uint8_t*)text, strlen(text));
}

/**
 * @brief Append one uint64_t as canonical decimal.
 * @details Uses fixed reverse-digit storage and emits no terminator.
 * @param[in] sink Bound report sink.
 * @param[in] value Value to spell.
 * @return Sink status.
 * @retval k_ra8_ok The complete decimal was appended.
 * @retval other Injected sink failure.
 * @pre @p sink and callback are valid.
 * @pre Fixed storage spans ::k_verify_decimal_max digits.
 * @post Success appends only decimal digits.
 * @post No global state changes.
 * @note Thread safety inherits the sink.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_u64(const ra8_fmt_sink_t* sink, uint64_t value)
{
  char   reverse[k_verify_decimal_max];
  size_t count = 0U;
  do {
    reverse[count++] = (char)('0' + (char)(value % k_verify_decimal));
    value /= k_verify_decimal;
  } while (value != 0U);
  char text[k_verify_decimal_max];
  for (size_t i = 0U; i < count; ++i) {
    text[i] = reverse[count - i - 1U];
  }
  return sink->write(sink->ctx, (const uint8_t*)text, count);
}

/**
 * @brief Append one byte as two uppercase hexadecimal digits.
 * @details Emits the high then low nibble through one exact sink call.
 * @param[in] sink Bound report sink.
 * @param[in] value Byte value.
 * @return Sink status.
 * @retval k_ra8_ok Both digits were appended.
 * @retval other Injected sink failure.
 * @pre @p sink and callback are valid.
 * @pre @p value is one complete byte.
 * @post Success appends exactly two uppercase digits.
 * @post No caller input changes.
 * @note Pure apart from the injected sink.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_hex(const ra8_fmt_sink_t* sink, uint8_t value)
{
  static const uint8_t digits[] = "0123456789ABCDEF";
  const uint8_t        text[2]  = {
    digits[value / k_verify_hex_radix],
    digits[value % k_verify_hex_radix],
  };
  return sink->write(sink->ctx, text, sizeof(text));
}

/**
 * @brief Append one numeric field and suffix while status succeeds.
 * @details Preserves the first report-sink error across chained appends.
 * @param[in] sink Bound report sink.
 * @param[in] value Numeric field.
 * @param[in] suffix NUL-terminated suffix.
 * @param[in,out] status Current and resulting report status.
 * @pre Every pointer argument is non-null.
 * @pre @p status contains the prior append result.
 * @post Existing failure skips every append.
 * @post Success appends both field and suffix.
 * @note Thread safety inherits the sink.
 * @since 0.1.0
 */
RA8_INTERNAL
static void
internal_field(const ra8_fmt_sink_t* sink, uint64_t value, const char* suffix, ra8_err_t* status)
{
  if (*status == k_ra8_ok) {
    *status = internal_u64(sink, value);
  }
  if (*status == k_ra8_ok) {
    *status = internal_text(sink, suffix);
  }
}

/**
 * @brief Emit the exact legacy verifier geometry line.
 * @details Reports trusted reference and subject dimensions and tiling.
 * @param[in] report Bound report sink.
 * @param[in] info Trusted reference geometry.
 * @param[in] subject Trusted banded geometry.
 * @pre Every pointer argument is valid.
 * @pre Both geometries passed full parse and decode preflight.
 * @post Best effort emits one newline-terminated geometry line.
 * @post Geometry inputs remain unchanged.
 * @note Report failures do not mutate verification state.
 * @since 0.1.0
 */
RA8_INTERNAL
static void
internal_geometry(const ra8_fmt_sink_t* report, const jof_info_t* info, const jof_info_t* subject)
{
  ra8_err_t rc = internal_text(report, "verify: ");
  internal_field(report, info->width, "x", &rc);
  internal_field(report, info->height, " bpp=", &rc);
  internal_field(report, info->bpp, " | reference 1 tile | banded ", &rc);
  internal_field(report, subject->tile_count, " tiles of ", &rc);
  internal_field(report, subject->tile_h, " rows\n", &rc);
}

/**
 * @brief Record and optionally report one differing byte.
 * @details Counts every difference but emits detail only up to the legacy ceiling.
 * @param[in,out] state Comparison state.
 * @param[in] x Pixel x coordinate.
 * @param[in] y Pixel y coordinate.
 * @param[in] reference Reference byte.
 * @param[in] banded Subject byte.
 * @pre @p state and its report binding are valid.
 * @pre Coordinates identify the compared row byte.
 * @post Difference count advances exactly once.
 * @post At most ::k_verify_diffs_max detail lines are emitted.
 * @note Counter saturation is prevented by bounded raster geometry.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_difference(verify_compare_t* state,
                                uint64_t          x,
                                uint64_t          y,
                                uint8_t           reference,
                                uint8_t           banded)
{
  if (state->shown >= k_verify_diffs_max) {
    return;
  }
  ra8_err_t rc = internal_text(state->report, "  DIFF at pixel (");
  internal_field(state->report, x, ", ", &rc);
  internal_field(state->report, y, "): reference=0x", &rc);
  if (rc == k_ra8_ok) {
    rc = internal_hex(state->report, reference);
  }
  if (rc == k_ra8_ok) {
    rc = internal_text(state->report, " banded=0x");
  }
  if (rc == k_ra8_ok) {
    rc = internal_hex(state->report, banded);
  }
  if (rc == k_ra8_ok) {
    (void)internal_text(state->report, "\n");
  }
  state->shown++;
}

/**
 * @brief Abort an incomplete optional PPM stage once.
 * @details Delegates abort only while the comparison still owns a live stage.
 * @param[in,out] state Comparison state.
 * @pre @p state is valid.
 * @pre Optional transaction callbacks are complete.
 * @post Any live stage is aborted.
 * @post @p state records that the dump is unavailable.
 * @note Idempotent through the dump-ok guard.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_abort_dump(verify_compare_t* state)
{
  if ((state->dump != nullptr) && (state->dump->ops != nullptr) &&
      (state->dump->ops->abort != nullptr)) {
    state->dump->ops->abort(state->dump->ctx);
  }
  state->dump_ok = false;
}

/**
 * @brief Append optional PPM bytes or mark the dump failed.
 * @details Preserves verification progress when a diagnostic output fails.
 * @param[in,out] state Comparison state.
 * @param[in] bytes PPM bytes.
 * @param[in] len Exact byte count.
 * @pre @p state is valid and @p bytes spans @p len.
 * @pre Optional transaction callbacks are complete.
 * @post Success extends only the unpublished stage.
 * @post Failure aborts the stage and records dump failure.
 * @note Dump failure does not alter the comparison verdict.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_dump_bytes(verify_compare_t* state, const uint8_t* bytes, size_t len)
{
  if (state->dump_ok && (state->dump != nullptr) &&
      (state->dump->ops->append(state->dump->ctx, bytes, len) != k_ra8_ok)) {
    internal_abort_dump(state);
  }
}

/**
 * @brief Append one decimal PPM header field.
 * @details Uses fixed reverse-digit storage through the optional dump helper.
 * @param[in,out] state Comparison state.
 * @param[in] value Header value.
 * @pre @p state is valid.
 * @pre @p value is a planned image dimension or maximum sample.
 * @post A live stage receives the canonical decimal.
 * @post A failed stage remains aborted.
 * @note No allocation or global storage is used.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_dump_u64(verify_compare_t* state, uint64_t value)
{
  char   reverse[k_verify_decimal_max];
  size_t count = 0U;
  do {
    reverse[count++] = (char)('0' + (char)(value % k_verify_decimal));
    value /= k_verify_decimal;
  } while (value != 0U);
  char text[k_verify_decimal_max];
  for (size_t i = 0U; i < count; ++i) {
    text[i] = reverse[count - i - 1U];
  }
  internal_dump_bytes(state, (const uint8_t*)text, count);
}

/**
 * @brief Stage the exact P5 or P6 header for subject geometry.
 * @details Selects gray or colour magic and emits dimensions plus sample maximum.
 * @param[in,out] state Comparison state.
 * @pre Subject bpp is one, three, or four.
 * @pre Optional transaction callbacks are complete.
 * @post A live stage contains a complete PPM header.
 * @post Unsupported or failed output leaves the stage aborted.
 * @note RGBA subjects use P6 and drop alpha in row writes.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_dump_header(verify_compare_t* state)
{
  const uint8_t magic[3] = {'P', (state->need->bpp == 1U) ? '5' : '6', '\n'};
  internal_dump_bytes(state, magic, sizeof(magic));
  internal_dump_u64(state, state->need->width);
  internal_dump_bytes(state, (const uint8_t*)" ", 1U);
  internal_dump_u64(state, state->need->height);
  internal_dump_bytes(state, (const uint8_t*)"\n255\n", k_verify_ppm_max_text);
}

/**
 * @brief Stage one subject row in PPM channel order.
 * @details Appends gray/RGB rows directly and strips RGBA alpha per pixel.
 * @param[in,out] state Comparison state.
 * @param[in] row Decoded subject row.
 * @pre @p row spans the planned full-width subject bytes.
 * @pre PPM header staging already ran.
 * @post A live stage gains exactly one visible raster row.
 * @post Comparison buffers and decoded pixels remain unchanged.
 * @note Output remains unpublished until final commit.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_dump_row(verify_compare_t* state, const uint8_t* row)
{
  if ((state->dump == nullptr) || !state->dump_ok) {
    return;
  }
  if (state->need->bpp != 4U) {
    internal_dump_bytes(state, row, state->need->row_bytes);
    return;
  }
  for (uint32_t x = 0U; x < state->need->width; ++x) {
    (void)memcpy(&state->work->row[(size_t)x * 3U], &row[(size_t)x * 4U], 3U);
  }
  internal_dump_bytes(state, state->work->row, (size_t)state->need->width * 3U);
}

/**
 * @brief Decode every sealed atlas tile before reporting or output staging.
 * @details Validates dimensions for each reference row or subject band.
 * @param[in] spool Sealed atlas binding.
 * @param[in] info Trusted parsed geometry.
 * @param[in,out] work Caller decode buffers.
 * @param[in] reference Select reference-row buffer use.
 * @return First tile-read or geometry status.
 * @retval k_ra8_ok Every tile decoded with exact dimensions.
 * @retval k_ra8_err_validation_failed A decoded tile shape differed.
 * @retval other Parser, decompressor, or scratch status.
 * @pre Every pointer and callback binding is valid.
 * @pre @p info total size matches the sealed spool.
 * @post Success proves every tile can be fully decoded.
 * @post No output transaction is opened or published.
 * @note Work is bounded by trusted tile counts and caller buffers.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_preflight(const ra8_fmt_spool_t*          spool,
                                    const jof_info_t*               info,
                                    ra8_fmt_jof_verify_workspace_t* work,
                                    bool                            reference)
{
  uint8_t* const buffer = reference ? work->row : work->band_tile;
  const uint32_t cap    = reference ? work->row_cap : work->band_tile_cap;
  for (uint16_t y = 0U; y < info->tile_rows; ++y) {
    uint16_t        out_w = 0U;
    uint16_t        out_h = 0U;
    const ra8_err_t rc    = jof_read_tile(spool->read_at,
                                          spool->ctx,
                                          info,
                                          0U,
                                          y,
                                          work->scratch,
                                          work->scratch_cap,
                                          buffer,
                                          cap,
                                          &out_w,
                                          &out_h);
    if ((rc != k_ra8_ok) || (out_w != info->width) || (reference && (out_h != 1U))) {
      return (rc == k_ra8_ok) ? k_ra8_err_validation_failed : rc;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Compare one decoded subject row with its reference row.
 * @details Loads the matching reference tile and records differing channel bytes.
 * @param[in,out] state Comparison state.
 * @param[in] band_row Decoded subject row.
 * @param[in] y Global raster row.
 * @pre @p band_row spans the planned row byte count.
 * @pre @p y is below planned image height.
 * @post Every differing byte in the row increments the count.
 * @post At most the legacy detail ceiling is reported globally.
 * @note Reference decoding uses the caller-owned row buffer.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_compare_row(verify_compare_t* state, const uint8_t* band_row, uint16_t y)
{
  for (uint32_t i = 0U; i < state->need->row_bytes; ++i) {
    if (state->work->row[i] != band_row[i]) {
      state->diffs++;
      internal_difference(state,
                          (uint64_t)(i / state->need->bpp),
                          y,
                          state->work->row[i],
                          band_row[i]);
    }
  }
  internal_dump_row(state, band_row);
}

/**
 * @brief Compare all subject bands against streamed reference rows.
 * @details Decodes one band, compares each row, and stages optional PPM rows.
 * @param[in,out] state Fully prepared comparison state.
 * @return First tile-read or geometry status.
 * @retval k_ra8_ok The complete raster was compared.
 * @retval k_ra8_err_validation_failed Decoded geometry was inconsistent.
 * @retval other Tile decoder or scratch status.
 * @pre Both spools are sealed, parsed, and preflighted.
 * @pre All comparison buffers satisfy exact requirements.
 * @post Success visits exactly the planned image height.
 * @post Optional PPM remains unpublished.
 * @note Difference count does not itself stop the full comparison.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_compare(verify_compare_t* state)
{
  if (state->dump != nullptr) {
    state->dump_ok = true;
    internal_dump_header(state);
  }
  uint16_t global_y = 0U;
  for (uint16_t band_y = 0U; band_y < state->ginfo->tile_rows; ++band_y) {
    uint16_t  band_w = 0U;
    uint16_t  band_h = 0U;
    ra8_err_t rc     = jof_read_tile(state->got->read_at,
                                     state->got->ctx,
                                     state->ginfo,
                                     0U,
                                     band_y,
                                     state->work->scratch,
                                     state->work->scratch_cap,
                                     state->work->band_tile,
                                     state->work->band_tile_cap,
                                     &band_w,
                                     &band_h);
    if ((rc != k_ra8_ok) || (band_w != state->need->width)) {
      return (rc == k_ra8_ok) ? k_ra8_err_validation_failed : rc;
    }
    for (uint16_t row = 0U; row < band_h; ++row) {
      uint16_t ref_w = 0U;
      uint16_t ref_h = 0U;
      rc             = jof_read_tile(state->ref->read_at,
                                     state->ref->ctx,
                                     state->rinfo,
                                     0U,
                                     global_y,
                                     state->work->scratch,
                                     state->work->scratch_cap,
                                     state->work->row,
                                     state->work->row_cap,
                                     &ref_w,
                                     &ref_h);
      if ((rc != k_ra8_ok) || (ref_w != state->need->width) || (ref_h != 1U)) {
        return (rc == k_ra8_ok) ? k_ra8_err_validation_failed : rc;
      }
      const uint8_t* band_row = &state->work->band_tile[(size_t)row * state->need->row_bytes];
      internal_compare_row(state, band_row, global_y++);
    }
  }
  return (global_y == state->need->height) ? k_ra8_ok : k_ra8_err_validation_failed;
}

/**
 * @brief Check parsed atlas geometry against exact planned policy.
 * @details Requires one full-width column and the requested row/band tiling.
 * @param[in] info Parsed atlas geometry.
 * @param[in] need Exact verifier requirements.
 * @param[in] tile_h Expected tile height.
 * @return Whether every geometry and codec invariant matches.
 * @retval true The atlas matches the complete policy.
 * @retval false One geometry, tile-count, bpp, or codec field differs.
 * @pre @p info and @p need are non-null.
 * @pre Both structures are completely initialized.
 * @post Neither structure changes.
 * @post Classification is deterministic.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_info(const jof_info_t*                        info,
                          const ra8_fmt_jof_verify_requirements_t* need,
                          uint16_t                                 tile_h)
{
  const uint16_t rows = (uint16_t)(((uint32_t)need->height + tile_h - 1U) / tile_h);
  return (info->width == need->width) && (info->height == need->height) &&
         (info->tile_w == need->width) && (info->tile_h == tile_h) && (info->tile_cols == 1U) &&
         (info->tile_rows == rows) && (info->tile_count == rows) && (info->bpp == need->bpp) &&
         (info->codec == (uint8_t)k_jof_codec_deflate);
}

/**
 * @brief Report one exact legacy phase-failure line.
 * @details Appends a fixed prefix, decimal status, and closing line syntax.
 * @param[in] report Bound report sink.
 * @param[in] prefix NUL-terminated phase prefix.
 * @param[in] status Phase failure status.
 * @pre @p report and @p prefix are valid.
 * @pre The prefix leaves the status parenthesis open.
 * @post Best effort emits one newline-terminated line.
 * @post No verification state changes.
 * @note Sink failure is not recursively reported.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_phase_error(const ra8_fmt_sink_t* report, const char* prefix, ra8_err_t status)
{
  ra8_err_t rc = internal_text(report, prefix);
  internal_field(report, status, ")\n", &rc);
}

/**
 * @brief Commit or abort the optional PPM and emit its legacy status line.
 * @details Publishes only a complete stage after comparison and stability checks.
 * @param[in,out] state Comparison state.
 * @param[in] dump_name Output spelling.
 * @pre @p state is valid.
 * @pre A non-null dump has a non-null @p dump_name.
 * @post A complete stage is committed; any failed stage is aborted.
 * @post Exactly one best-effort output status line is emitted.
 * @note Output failure does not alter the exact comparison verdict.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_finish_dump(verify_compare_t* state, const char* dump_name)
{
  if (state->dump == nullptr) {
    return;
  }
  if (state->dump_ok && (state->dump->ops->commit(state->dump->ctx) != k_ra8_ok)) {
    internal_abort_dump(state);
  }
  ra8_err_t rc = internal_text(state->report, "  wrote reassembled raster to ");
  if (rc == k_ra8_ok) {
    rc = internal_text(state->report, dump_name);
  }
  if (rc == k_ra8_ok) {
    (void)internal_text(state->report, state->dump_ok ? " (ok)\n" : " (FAILED)\n");
  }
}

/**
 * @brief Emit the exact legacy final verdict line.
 * @details Selects exact or mismatch wording from the complete difference count.
 * @param[in] report Bound report sink.
 * @param[in] diffs Complete differing-byte count.
 * @pre @p report and its callback are valid.
 * @pre Full comparison and final stability checks completed.
 * @post Best effort emits one newline-terminated verdict.
 * @post No comparison or output state changes.
 * @note Sink failure does not change the returned verifier status.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_verdict(const ra8_fmt_sink_t* report, uint64_t diffs)
{
  ra8_err_t rc = internal_text(report, "verdict: ");
  if (rc == k_ra8_ok) {
    rc = internal_text(report,
                       (diffs == 0U) ? "ROUND-TRIP EXACT -- the produced file is correct"
                                     : "MISMATCH");
  }
  if (rc == k_ra8_ok) {
    rc = internal_text(report, " (");
  }
  internal_field(report, diffs, " differing bytes)\n", &rc);
}

/**
 * @brief Reject incomplete callbacks, aliased contexts, or undersized arenas.
 * @details Validates every independent binding before producer or transaction mutation.
 * @param[in] reference_source Reference source binding.
 * @param[in] banded_source Subject source binding.
 * @param[in] need Exact verifier requirements.
 * @param[in] work Caller workspace views.
 * @param[in] ref_spool Reference spool binding.
 * @param[in] got_spool Subject spool binding.
 * @param[in] report Report sink.
 * @return Binding validation status.
 * @retval k_ra8_ok Every contract is complete and sufficiently sized.
 * @retval k_ra8_err_null_ptr A binding is incomplete or contexts alias.
 * @retval k_ra8_err_invalid_size One caller arena is insufficient.
 * @pre Inputs may be null and are checked before dereference.
 * @pre Requirement capacities are exact planner outputs.
 * @post No source, spool, workspace, report, or transaction is mutated.
 * @post Success permits safe producer entry.
 * @note Pure apart from no state.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_check(const ra8_fmt_source_t*                  reference_source,
                                const ra8_fmt_source_t*                  banded_source,
                                const ra8_fmt_jof_verify_requirements_t* need,
                                const ra8_fmt_jof_verify_workspace_t*    work,
                                const ra8_fmt_spool_t*                   ref_spool,
                                const ra8_fmt_spool_t*                   got_spool,
                                const ra8_fmt_sink_t*                    report)
{
  if ((reference_source == nullptr) || (banded_source == nullptr) ||
      (reference_source->read_at == nullptr) || (banded_source->read_at == nullptr) ||
      (reference_source->ctx == banded_source->ctx) || (need == nullptr) || (work == nullptr) ||
      (ref_spool == nullptr) || (got_spool == nullptr) || (ref_spool->read_at == nullptr) ||
      (ref_spool->append == nullptr) || (ref_spool->seal == nullptr) ||
      (got_spool->read_at == nullptr) || (got_spool->append == nullptr) ||
      (got_spool->seal == nullptr) || (ref_spool->ctx == got_spool->ctx) || (report == nullptr) ||
      (report->write == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  const uint32_t producer = (need->reference_work_bytes > need->banded_work_bytes)
                              ? need->reference_work_bytes
                              : need->banded_work_bytes;
  const bool     webp_bad =
    (need->webp_work_bytes != 0U) &&
    ((work->webp_work == nullptr) || (work->webp_work_cap < need->webp_work_bytes));
  if ((work->work == nullptr) || (work->work_cap < producer) || webp_bad ||
      (work->band_tile == nullptr) || (work->band_tile_cap < need->band_tile_bytes) ||
      (work->scratch == nullptr) || (work->scratch_cap < need->scratch_bytes) ||
      (work->row == nullptr) || (work->row_cap < need->row_bytes)) {
    return k_ra8_err_invalid_size;
  }
  return k_ra8_ok;
}

/** @brief Parsed and fully preflighted reference and subject atlases. */
typedef struct {
  jof_info_t reference; /**< One-row reference geometry. */
  jof_info_t banded;    /**< Production-band geometry.   */
} verify_atlases_t;

/**
 * @brief Abort an optional complete transaction binding.
 * @details Calls abort only when the transaction and callback are present.
 * @param[in,out] dump Optional transaction.
 * @pre @p dump is null or owns a complete transaction vtable.
 * @pre No commit is executing concurrently.
 * @post Any present stage receives one abort request.
 * @post A null or incomplete optional binding causes no dereference.
 * @note Used only on pre-comparison failures.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_abort_transaction(ra8_fmt_transaction_t* dump)
{
  if ((dump != nullptr) && (dump->ops != nullptr) && (dump->ops->abort != nullptr)) {
    dump->ops->abort(dump->ctx);
  }
}

/**
 * @brief Parse, check geometry, and decode every tile of one sealed spool.
 * @details Replaces producer geometry with independently parsed trusted geometry.
 * @param[in] spool Sealed reference or subject spool.
 * @param[in] need Exact verifier requirements.
 * @param[in,out] work Caller decode buffers.
 * @param[in] reference Select one-row reference geometry.
 * @param[in,out] info Producer geometry replaced by parsed geometry.
 * @return Parse, geometry, or tile-read status.
 * @retval k_ra8_ok The complete atlas passed every check.
 * @retval k_ra8_err_validation_failed Parsed policy differed.
 * @retval other Parser or decoder status.
 * @pre Every pointer and callback binding is valid.
 * @pre @p spool was sealed at @p info total size.
 * @post Success fully preflights the complete atlas.
 * @post No output transaction is staged or published.
 * @note Work is bounded by planned geometry and caller buffers.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_accept(const ra8_fmt_spool_t*                   spool,
                                 const ra8_fmt_jof_verify_requirements_t* need,
                                 ra8_fmt_jof_verify_workspace_t*          work,
                                 bool                                     reference,
                                 jof_info_t*                              info)
{
  const uint16_t tile_h = reference ? 1U : need->band_height;
  ra8_err_t      rc     = jof_parse(spool->read_at, spool->ctx, info->total_size, info);
  if ((rc == k_ra8_ok) && !internal_info(info, need, tile_h)) {
    rc = k_ra8_err_validation_failed;
  }
  if (rc == k_ra8_ok) {
    rc = internal_preflight(spool, info, work, reference);
  }
  return rc;
}

/**
 * @brief Produce, seal, parse, and preflight both independent atlases.
 * @details Completes the row-reference phase before starting the banded subject.
 * @param[in] reference_source First encoded-source context.
 * @param[in] banded_source Second encoded-source context.
 * @param[in] need Exact verifier requirements.
 * @param[in,out] work Phase-reused caller workspace.
 * @param[in,out] reference_spool Empty reference spool.
 * @param[in,out] banded_spool Empty subject spool.
 * @param[in] report Legacy report sink.
 * @param[out] atlases Receives trusted parsed geometry.
 * @return First producer, spool, parse, or decode status.
 * @retval k_ra8_ok Both atlases are sealed and trusted.
 * @retval other First reference or subject phase failure.
 * @pre All bindings are independent and validated.
 * @pre Workspace satisfies the exact planner requirements.
 * @post Success leaves both spools sealed and fully decoded once.
 * @post Failure emits exactly one legacy phase diagnostic.
 * @note No output transaction is published in preparation.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_prepare(const ra8_fmt_source_t*                  reference_source,
                                  const ra8_fmt_source_t*                  banded_source,
                                  const ra8_fmt_jof_verify_requirements_t* need,
                                  ra8_fmt_jof_verify_workspace_t*          work,
                                  ra8_fmt_spool_t*                         reference_spool,
                                  ra8_fmt_spool_t*                         banded_spool,
                                  const ra8_fmt_sink_t*                    report,
                                  verify_atlases_t*                        atlases)
{
  ra8_err_t rc =
    internal_produce(reference_source, need, 1U, work, reference_spool, &atlases->reference);
  if (rc == k_ra8_ok) {
    rc = internal_accept(reference_spool, need, work, true, &atlases->reference);
  }
  if (rc != k_ra8_ok) {
    internal_phase_error(report, "verify: reference encode failed (rc=", rc);
    return rc;
  }
  rc =
    internal_produce(banded_source, need, need->band_height, work, banded_spool, &atlases->banded);
  if (rc == k_ra8_ok) {
    rc = internal_accept(banded_spool, need, work, false, &atlases->banded);
  }
  if (rc != k_ra8_ok) {
    internal_phase_error(report, "verify: banded encode failed (rc=", rc);
  }
  return rc;
}

ra8_err_t ra8_fmt_jof_verify_stream(const ra8_fmt_source_t*                  reference_source,
                                    const ra8_fmt_source_t*                  banded_source,
                                    const ra8_fmt_jof_verify_requirements_t* requirements,
                                    ra8_fmt_jof_verify_workspace_t*          workspace,
                                    ra8_fmt_spool_t*                         reference_spool,
                                    ra8_fmt_spool_t*                         banded_spool,
                                    ra8_fmt_transaction_t*                   dump,
                                    const char*                              dump_name,
                                    const ra8_fmt_sink_t*                    report)
{
  ra8_err_t  rc = internal_check(reference_source,
                                 banded_source,
                                 requirements,
                                 workspace,
                                 reference_spool,
                                 banded_spool,
                                 report);
  const bool dump_bad =
    (dump != nullptr) &&
    ((dump->ops == nullptr) || (dump->ops->append == nullptr) || (dump->ops->commit == nullptr) ||
     (dump->ops->abort == nullptr) || (dump_name == nullptr));
  if ((rc != k_ra8_ok) || dump_bad) {
    return (rc == k_ra8_ok) ? k_ra8_err_null_ptr : rc;
  }
  verify_atlases_t atlases = {};
  rc                       = internal_prepare(reference_source,
                                              banded_source,
                                              requirements,
                                              workspace,
                                              reference_spool,
                                              banded_spool,
                                              report,
                                              &atlases);
  if (rc != k_ra8_ok) {
    internal_abort_transaction(dump);
    return rc;
  }
  internal_geometry(report, &atlases.reference, &atlases.banded);
  verify_compare_t compare = {
    .need    = requirements,
    .work    = workspace,
    .ref     = reference_spool,
    .got     = banded_spool,
    .rinfo   = &atlases.reference,
    .ginfo   = &atlases.banded,
    .dump    = dump,
    .report  = report,
    .dump_ok = dump != nullptr,
  };
  rc = internal_compare(&compare);
  if (rc == k_ra8_ok) {
    rc = internal_stable(reference_source);
  }
  if (rc == k_ra8_ok) {
    rc = internal_stable(banded_source);
  }
  if (rc != k_ra8_ok) {
    internal_abort_dump(&compare);
    return rc;
  }
  internal_finish_dump(&compare, dump_name);
  internal_verdict(report, compare.diffs);
  return (compare.diffs == 0U) ? k_ra8_ok : k_ra8_err_validation_failed;
}
