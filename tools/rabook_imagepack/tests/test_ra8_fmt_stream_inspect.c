/**
 * @file test_ra8_fmt_stream_inspect.c
 * @brief Golden and fault tests for callback-driven JOF inspection.
 * @details Uses bounded memory source/sink callbacks to prove exact legacy text,
 * verbose sections, insufficient workspaces, and sink failure propagation.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_fmt_stream.h"

/** @brief Synthetic raw-atlas layout and test buffer limits. */
typedef enum : uint32_t {
  k_test_atlas_size   = 68U,   /**< Complete two-tile fixture. */
  k_test_tile_count   = 2U,    /**< Fixture tile count.        */
  k_test_tile_bytes   = 2U,    /**< Bytes per raw tile.        */
  k_test_payload_a    = 32U,   /**< First payload offset.      */
  k_test_payload_b    = 34U,   /**< Second payload offset.     */
  k_test_index        = 36U,   /**< Index offset.              */
  k_test_footer       = 52U,   /**< Footer offset.             */
  k_test_text_cap     = 1024U, /**< Golden report capacity.    */
  k_test_shift_8      = 8U,    /**< LE field shift.            */
  k_test_shift_16     = 16U,   /**< LE field shift.            */
  k_test_shift_24     = 24U,   /**< LE field shift.            */
  k_test_sink_fail_at = 20U,   /**< Injected sink limit.       */
} test_const_t;

/** @brief Immutable test source. */
typedef struct {
  const uint8_t* bytes; /**< Source bytes. */
  size_t         len;   /**< Source size.  */
} test_source_t;

/** @brief Bounded output sink with one fault threshold. */
typedef struct {
  uint8_t bytes[k_test_text_cap]; /**< Captured report.         */
  size_t  len;                    /**< Captured bytes.          */
  size_t  fail_at;                /**< First rejected position. */
} test_sink_t;

static int s_failures;

/** @brief Record one failed test expectation without aborting later vectors. */
#define CHECK(expression)                                                                          \
  do {                                                                                             \
    if (!(expression)) {                                                                           \
      s_failures++;                                                                                \
    }                                                                                              \
  } while (false)

/**
 * @brief Encode one little-endian 16-bit fixture field.
 * @details Writes the two canonical bytes explicitly, independent of host order.
 * @param[out] bytes Destination spanning two bytes.
 * @param[in] value Fixture value to encode.
 * @pre @p bytes is non-null and writable for two bytes.
 * @pre @p value may be any `uint16_t` value.
 * @post Exactly two destination bytes encode @p value little-endian.
 * @post No later byte is changed.
 * @note Test-only and thread-safe for independent buffers.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_u16(uint8_t* bytes, uint16_t value)
{
  bytes[0] = (uint8_t)value;
  bytes[1] = (uint8_t)(value >> k_test_shift_8);
}

/**
 * @brief Encode one little-endian 32-bit fixture field.
 * @details Writes the four canonical bytes explicitly, independent of host order.
 * @param[out] bytes Destination spanning four bytes.
 * @param[in] value Fixture value to encode.
 * @pre @p bytes is non-null and writable for four bytes.
 * @pre @p value may be any `uint32_t` value.
 * @post Exactly four destination bytes encode @p value little-endian.
 * @post No later byte is changed.
 * @note Test-only and thread-safe for independent buffers.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_u32(uint8_t* bytes, uint32_t value)
{
  bytes[0] = (uint8_t)value;
  bytes[1] = (uint8_t)(value >> k_test_shift_8);
  bytes[2] = (uint8_t)(value >> k_test_shift_16);
  bytes[3] = (uint8_t)(value >> k_test_shift_24);
}

/**
 * @brief Construct a complete two-tile raw JOF.
 * @details Initializes header, payload, index, and footer for a valid bounded fixture.
 * @param[out] atlas Exact-size fixture buffer.
 * @pre @p atlas is non-null and spans ::k_test_atlas_size bytes.
 * @pre Test layout constants describe non-overlapping canonical sections.
 * @post Every atlas byte is initialized deterministically.
 * @post Fixture parses as two raw one-row tiles with exact coverage.
 * @note Test-only and thread-safe for independent buffers.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_atlas(uint8_t atlas[k_test_atlas_size])
{
  (void)memset(atlas, 0, k_test_atlas_size);
  (void)memcpy(&atlas[k_ra8_jof_ofs_magic], "JOF1", k_ra8_jof_magic_len); /* MAGIC-OK */
  internal_u16(&atlas[k_ra8_jof_ofs_width], 2U);
  internal_u16(&atlas[k_ra8_jof_ofs_height], 2U);
  internal_u16(&atlas[k_ra8_jof_ofs_tile_w], 2U);
  internal_u16(&atlas[k_ra8_jof_ofs_tile_h], 1U);
  atlas[k_ra8_jof_ofs_bpp]   = 1U;
  atlas[k_ra8_jof_ofs_codec] = (uint8_t)k_ra8_jof_codec_raw;
  internal_u32(&atlas[k_ra8_jof_ofs_tile_count], k_test_tile_count);
  atlas[k_test_payload_a]      = 1U;
  atlas[k_test_payload_a + 1U] = 2U;
  atlas[k_test_payload_b]      = 3U;
  atlas[k_test_payload_b + 1U] = 4U;
  internal_u32(&atlas[k_test_index], k_test_payload_a);
  internal_u32(&atlas[k_test_index + k_ra8_jof_idx_ofs_length], k_test_tile_bytes);
  internal_u32(&atlas[k_test_index + k_ra8_jof_index_entry], k_test_payload_b);
  internal_u32(&atlas[k_test_index + k_ra8_jof_index_entry + k_ra8_jof_idx_ofs_length],
               k_test_tile_bytes);
  internal_u32(&atlas[k_test_footer + k_ra8_jof_ftr_index_off], k_test_index);
  internal_u32(&atlas[k_test_footer + k_ra8_jof_ftr_tile_count], k_test_tile_count);
  internal_u32(&atlas[k_test_footer + k_ra8_jof_ftr_total_size], k_test_atlas_size);
  (void)memcpy(&atlas[k_test_footer + k_ra8_jof_ftr_magic],
               "JOFE",
               k_ra8_jof_magic_len); /* MAGIC-OK */
}

/**
 * @brief Implement positioned-read semantics over immutable fixture bytes.
 * @details Clamps at EOF and returns legal short success without retaining outputs.
 * @param[in] ctx Bound ::test_source_t.
 * @param[in] offset Absolute source offset.
 * @param[out] bytes Destination spanning @p len bytes.
 * @param[in] len Requested byte count.
 * @param[out] got Receives actual byte count.
 * @return Canonical source status.
 * @retval k_ra8_ok Requested prefix, short EOF, or empty EOF was handled.
 * @pre @p ctx, @p bytes, and @p got are non-null for the test call.
 * @pre Source extent is valid and @p bytes spans @p len writable bytes.
 * @post @p got is initialized and never exceeds @p len.
 * @post Source bytes are unchanged.
 * @note Test callback always succeeds to isolate parser/report behavior.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_read(void* ctx, uint64_t offset, uint8_t* bytes, size_t len, size_t* got)
{
  const test_source_t* source = (const test_source_t*)ctx;
  *got                        = 0U;
  if (offset >= source->len) {
    return k_ra8_ok;
  }
  size_t count = source->len - (size_t)offset;
  if (count > len) {
    count = len;
  }
  (void)memcpy(bytes, &source->bytes[offset], count);
  *got = count;
  return k_ra8_ok;
}

/**
 * @brief Capture report bytes with deterministic failure injection.
 * @details Accepts a complete append only when both physical and injected bounds permit it.
 * @param[in,out] ctx Bound ::test_sink_t.
 * @param[in] bytes Source report span.
 * @param[in] len Exact append length.
 * @return Canonical sink status.
 * @retval k_ra8_ok Complete span was captured.
 * @retval k_ra8_fail Physical or configured capture ceiling would be exceeded.
 * @pre @p ctx and @p bytes are non-null for non-empty writes.
 * @pre Sink length does not exceed either configured capacity.
 * @post Success appends exactly @p len bytes and advances length once.
 * @post Failure leaves capture bytes and length unchanged.
 * @note Test-only fault seam; not thread-safe through one sink.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_write(void* ctx, const uint8_t* bytes, size_t len)
{
  test_sink_t* sink = (test_sink_t*)ctx;
  if ((sink->len + len > sizeof(sink->bytes)) || (sink->len + len > sink->fail_at)) {
    return k_ra8_fail;
  }
  (void)memcpy(&sink->bytes[sink->len], bytes, len);
  sink->len += len;
  return k_ra8_ok;
}

/**
 * @brief Find one literal within captured report bytes.
 * @details Performs a bounded sliding comparison without requiring report NUL termination.
 * @param[in] sink Captured report bytes.
 * @param[in] text NUL-terminated search literal.
 * @return Whether one complete literal occurrence is present.
 * @retval true A byte-equal occurrence fits wholly inside capture length.
 * @retval false No complete occurrence exists.
 * @pre @p sink and @p text are non-null.
 * @pre @p text is NUL-terminated and sink length is within capacity.
 * @post Inputs are unchanged.
 * @post No byte beyond capture length is inspected.
 * @note Test-only and thread-safe for immutable inputs.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_contains(const test_sink_t* sink, const char* text)
{
  const size_t text_len = strlen(text);
  for (size_t offset = 0U; (offset + text_len) <= sink->len; ++offset) {
    if (memcmp(&sink->bytes[offset], text, text_len) == 0) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Check the exact legacy report bytes over a valid baseline fixture.
 * @details Runs non-verbose inspection once and compares status, length, and
 * the complete captured span against the golden legacy text byte for byte.
 * @param[in] source Bound fixture source.
 * @param[in,out] workspace Inspection workspace at its full capacity.
 * @param[in,out] sink Capture sink reset to its full capacity before the call.
 * @param[in] report Bound diagnostic sink wrapping @p sink.
 * @param[in] expected Golden legacy report text.
 * @param[in] expected_len Exact expected byte count.
 * @pre Every pointer argument is non-null.
 * @pre @p sink is empty and @p workspace covers the complete fixture.
 * @post Any mismatch contributes exactly one failure through CHECK.
 * @post @p sink holds the captured report on return.
 * @note Test-only and intentionally single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_golden(const ra8_fmt_source_t*          source,
                                 ra8_fmt_jof_inspect_workspace_t* workspace,
                                 test_sink_t*                     sink,
                                 const ra8_fmt_sink_t*            report,
                                 const char*                      expected,
                                 size_t                           expected_len)
{
  CHECK(ra8_fmt_jof_inspect_stream(source, false, workspace, report) == k_ra8_ok);
  CHECK(sink->len == expected_len);
  CHECK(memcmp(sink->bytes, expected, expected_len) == 0);
}

/**
 * @brief Check that verbose inspection includes the optional header/tile sections.
 * @details Resets the sink, runs verbose inspection, and looks for two literal
 * section markers rather than the complete byte-exact text.
 * @param[in] source Bound fixture source.
 * @param[in,out] workspace Inspection workspace at its full capacity.
 * @param[in,out] sink Capture sink; reset to full capacity by this call.
 * @param[in] report Bound diagnostic sink wrapping @p sink.
 * @pre Every pointer argument is non-null.
 * @pre @p workspace covers the complete fixture.
 * @post Any missing section marker contributes exactly one failure through CHECK.
 * @post @p sink holds the captured verbose report on return.
 * @note Test-only and intentionally single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_verbose(const ra8_fmt_source_t*          source,
                                  ra8_fmt_jof_inspect_workspace_t* workspace,
                                  test_sink_t*                     sink,
                                  const ra8_fmt_sink_t*            report)
{
  *sink = (test_sink_t){.fail_at = sizeof(sink->bytes)};
  CHECK(ra8_fmt_jof_inspect_stream(source, true, workspace, report) == k_ra8_ok);
  CHECK(internal_contains(sink, "header (32 bytes at +0):"));
  CHECK(internal_contains(sink, "  tile      offset"));
}

/**
 * @brief Check that an undersized tile workspace rejects inspection cleanly.
 * @details Shrinks the workspace tile capacity below one raw tile, confirms
 * the invalid-size rejection with no partial report, then restores capacity.
 * @param[in] source Bound fixture source.
 * @param[in,out] workspace Inspection workspace; tile_cap is shrunk then restored.
 * @param[in,out] sink Capture sink; reset to full capacity by this call.
 * @param[in] report Bound diagnostic sink wrapping @p sink.
 * @param[in] tile_bytes Exact bytes needed for one raw tile.
 * @param[in] full_tile_cap Tile capacity to restore afterward.
 * @pre Every pointer argument is non-null.
 * @pre @p tile_bytes is at least one, so the shrunk capacity underflows to zero.
 * @post Rejection or a non-empty capture contributes exactly one failure through CHECK.
 * @post @p workspace tile_cap is @p full_tile_cap on return.
 * @note Test-only and intentionally single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_undersized_workspace(const ra8_fmt_source_t*          source,
                                               ra8_fmt_jof_inspect_workspace_t* workspace,
                                               test_sink_t*                     sink,
                                               const ra8_fmt_sink_t*            report,
                                               size_t                           tile_bytes,
                                               size_t                           full_tile_cap)
{
  workspace->tile_cap = tile_bytes - 1U;
  *sink               = (test_sink_t){.fail_at = sizeof(sink->bytes)};
  CHECK(ra8_fmt_jof_inspect_stream(source, false, workspace, report) == k_ra8_err_invalid_size);
  CHECK(sink->len == 0U);
  workspace->tile_cap = full_tile_cap;
}

/**
 * @brief Check that a sink rejecting mid-report stops the capture in place.
 * @details Arms the sink to fail at a fixed byte position and confirms the
 * inspection call fails while the capture never exceeds that position.
 * @param[in] source Bound fixture source.
 * @param[in,out] workspace Inspection workspace at its full capacity.
 * @param[in,out] sink Capture sink; armed with a low fail threshold by this call.
 * @param[in] report Bound diagnostic sink wrapping @p sink.
 * @pre Every pointer argument is non-null.
 * @pre @p workspace covers the complete fixture.
 * @post A missing failure or an over-length capture contributes exactly one
 * failure through CHECK.
 * @note Test-only and intentionally single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test_sink_fault(const ra8_fmt_source_t*          source,
                                     ra8_fmt_jof_inspect_workspace_t* workspace,
                                     test_sink_t*                     sink,
                                     const ra8_fmt_sink_t*            report)
{
  *sink = (test_sink_t){.fail_at = k_test_sink_fail_at};
  CHECK(ra8_fmt_jof_inspect_stream(source, false, workspace, report) == k_ra8_fail);
  CHECK(sink->len <= k_test_sink_fail_at);
}

/**
 * @brief Run the complete golden, verbose, capacity, and sink-fault vectors.
 * @details Exercises exact legacy output, optional sections, workspace rejection,
 * and deterministic sink failure through real portable inspection.
 * @pre Static failure counter is initialized for this process.
 * @pre Test fixture and workspace constants satisfy the valid baseline vector.
 * @post Every vector contributes failures through the CHECK macro only.
 * @post No dynamic allocation or persistent filesystem state is used.
 * @note Test-only and intentionally single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_test(void)
{
  static const char expected[] = "JOF atlas: 68 bytes\n"
                                 "  image      : 2 x 2 px\n"
                                 "  tile       : 2 x 1 px\n"
                                 "  grid       : 1 cols x 2 rows\n"
                                 "  bpp        : 1\n"
                                 "  codec      : 0 (raw)\n"
                                 "  tile_count : 2\n"
                                 "  index_off  : 36\n"
                                 "  total_size : 68\n"
                                 "  longstrip  : YES (tile_w == width)\n"
                                 "verdict: VALID (coverage exact, no duplicate tiles)\n";
  uint8_t           atlas[k_test_atlas_size];
  internal_atlas(atlas);
  test_source_t    backing = {.bytes = atlas, .len = sizeof(atlas)};
  ra8_fmt_source_t source  = {.read_at = internal_read, .ctx = &backing, .size = sizeof(atlas)};
  ra8_jof_audit_record_t          records[k_test_tile_count];
  uint8_t                         tile[k_test_tile_bytes];
  ra8_fmt_jof_inspect_workspace_t workspace = {.records    = records,
                                               .record_cap = k_test_tile_count,
                                               .tile       = tile,
                                               .tile_cap   = sizeof(tile)};
  test_sink_t                     sink      = {.fail_at = sizeof(sink.bytes)};
  ra8_fmt_sink_t                  report    = {.write = internal_write, .ctx = &sink};

  internal_test_golden(&source, &workspace, &sink, &report, expected, sizeof(expected) - 1U);
  internal_test_verbose(&source, &workspace, &sink, &report);
  internal_test_undersized_workspace(&source,
                                     &workspace,
                                     &sink,
                                     &report,
                                     k_test_tile_bytes,
                                     sizeof(tile));
  internal_test_sink_fault(&source, &workspace, &sink, &report);
}

int main(void)
{
  internal_test();
  return s_failures;
}
