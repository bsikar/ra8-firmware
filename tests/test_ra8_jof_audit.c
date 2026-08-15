/**
 * @file test_ra8_jof_audit.c
 * @brief Exact-capacity and corruption tests for the portable JOF audit.
 *
 * @details
 * Constructs a minimal in-memory atlas to verify exact workspace discovery,
 * capacity rejection, tile coverage and ordering failures, positioned-read
 * errors, and duplicate-content diagnostics. Separate uniform and non-uniform
 * repeated tiles confirm that valid repeated imagery is never rejected.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_jof_audit.h"
#include "support/ra8_test_output.h"

/** @brief Fixed geometry and byte offsets for the synthetic two-tile atlas. */
typedef enum : uint32_t {
  k_test_atlas_size         = 68U,         /**< Complete synthetic atlas bytes.        */
  k_test_tile_count         = 2U,          /**< Tiles stored in the fixture.           */
  k_test_tile_bytes         = 2U,          /**< Decoded bytes in each fixture tile.    */
  k_test_first_payload_off  = 32U,         /**< First raw tile offset.                 */
  k_test_second_payload_off = 34U,         /**< Second raw tile offset.                */
  k_test_index_off          = 36U,         /**< First index-entry offset.              */
  k_test_footer_off         = 52U,         /**< Footer offset after two index entries. */
  k_test_u16_hi_shift       = 8U,          /**< High-byte shift for LE16 encoding.     */
  k_test_u32_b2_shift       = 16U,         /**< Byte-two shift for LE32 encoding.      */
  k_test_u32_b3_shift       = 24U,         /**< Byte-three shift for LE32 encoding.    */
  k_test_magic_bytes        = 4U,          /**< Bytes in each fixture magic tag.       */
  k_test_record_poison      = 0xA5A5A5A5U, /**< Unchanged-record sentinel.             */
  k_test_record_poison_byte = 0xA5U,       /**< Record fill sentinel byte.             */
  k_test_tile_poison_byte   = 0x5AU,       /**< Tile fill sentinel byte.               */
  k_test_deflate_atlas_size = 78U,         /**< Complete deflate atlas bytes.          */
  k_test_deflate_stream     = 7U,          /**< Bytes per stored block.                */
  k_test_deflate_second_off = 39U,         /**< Second compressed tile.                */
  k_test_deflate_index_off  = 46U,         /**< Deflate atlas index.                   */
  k_test_deflate_footer_off = 62U,         /**< Deflate atlas footer.                  */
  k_test_deflate_scratch    = 258U,        /**< Exact two-byte stored bound.           */
} test_atlas_const_t;

/**
 * @struct test_store_t
 * @brief Immutable memory backing plus one injected read-failure control.
 */
typedef struct test_store_t {
  const uint8_t* bytes;          /**< Complete readable backing bytes.       */
  size_t         len;            /**< Valid byte count at @c bytes.          */
  bool           fail;           /**< Whether every read must fail.          */
  bool           fail_at_offset; /**< Whether one selected offset must fail. */
  uint64_t       fail_offset;    /**< Selected failing absolute offset.      */
} test_store_t;

/** @brief Shared-address storage for explicit result/record alias testing. */
typedef union test_alias_t {
  ra8_jof_audit_record_t records[k_test_tile_count]; /**< Record view. */
  ra8_jof_audit_result_t result;                     /**< Result view. */
} test_alias_t;

static int s_failures;

/**
 * @brief Write and count one nonfatal JOF audit expectation failure.
 * @details Composes the fixture line and stringified expression through a
 * caller-local diagnostic sink, then increments the suite's failure count.
 * @param[in] line Source line of the failed CHECK expression.
 * @param[in] expression NUL-terminated stringified expression.
 * @pre @p expression remains readable for the complete diagnostic composition.
 * @pre @p line is the positive source line captured by the CHECK macro.
 * @post ::s_failures is incremented exactly once.
 * @post One complete failure line has been attempted on descriptor 2.
 * @note Output failure does not prevent later audit vectors from running.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_report_failure(int line, const char* expression)
{
  ra8_test_output_t    output = {};
  ra8_test_output_fd_t state  = {};
  (void)internal_test_output_fd_init(&output, &state, STDERR_FILENO);
  (void)internal_test_output_text(&output, "FAIL:");
  (void)internal_test_output_i64(&output, (int64_t)line);
  (void)internal_test_output_text(&output, ": ");
  (void)internal_test_output_text(&output, expression);
  (void)internal_test_output_text(&output, "\n");
  s_failures++;
}

/**
 * @def CHECK
 * @brief Record one failed test expression without aborting later vectors.
 */
#define CHECK(expr)                                                                                \
  do {                                                                                             \
    if (!(expr)) {                                                                                 \
      internal_report_failure(__LINE__, #expr);                                                    \
    }                                                                                              \
  } while (false)

/**
 * @brief Encode one little-endian 16-bit fixture value
 * @details Writes bytes explicitly so fixture construction is host-independent.
 * @param[out] p Two-byte destination.
 * @param[in] v Value to encode.
 * @pre @p p is writable for two bytes.
 * @pre @p p belongs to the synthetic atlas under construction.
 * @post Exactly two bytes are initialized.
 * @post Bytes use canonical little-endian order.
 * @note Test-local, thread-safe, and allocation-free.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_write_u16(uint8_t* p, uint16_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> k_test_u16_hi_shift);
}

/**
 * @brief Encode one little-endian 32-bit fixture value
 * @details Writes bytes explicitly so fixture construction is host-independent.
 * @param[out] p Four-byte destination.
 * @param[in] v Value to encode.
 * @pre @p p is writable for four bytes.
 * @pre @p p belongs to the synthetic atlas under construction.
 * @post Exactly four bytes are initialized.
 * @post Bytes use canonical little-endian order.
 * @note Test-local, thread-safe, and allocation-free.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_write_u32(uint8_t* p, uint32_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> k_test_u16_hi_shift);
  p[2] = (uint8_t)(v >> k_test_u32_b2_shift);
  p[3] = (uint8_t)(v >> k_test_u32_b3_shift);
}

/**
 * @brief Construct a valid two-tile raw JOF atlas
 * @details Emits canonical header, payload, index, and footer bytes in memory.
 * @param[out] atlas Complete synthetic atlas buffer.
 * @param[in] top Two decoded bytes for the first tile.
 * @param[in] bottom Two decoded bytes for the second tile.
 * @pre Every input and output pointer is non-null.
 * @pre @p atlas is writable for ::k_test_atlas_size bytes.
 * @post The complete atlas parses and covers both tiles exactly.
 * @post Payload bytes equal @p top followed by @p bottom.
 * @note Test-local, thread-safe, and allocation-free.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_make_atlas(uint8_t       atlas[k_test_atlas_size],
                                             const uint8_t top[k_test_tile_bytes],
                                             const uint8_t bottom[k_test_tile_bytes])
{
  (void)memset(atlas, 0, k_test_atlas_size);
  (void)memcpy(&atlas[k_ra8_jof_ofs_magic], "JOF1", k_test_magic_bytes); /* MAGIC-OK */
  internal_write_u16(&atlas[k_ra8_jof_ofs_width], 2U);
  internal_write_u16(&atlas[k_ra8_jof_ofs_height], 2U);
  internal_write_u16(&atlas[k_ra8_jof_ofs_tile_w], 2U);
  internal_write_u16(&atlas[k_ra8_jof_ofs_tile_h], 1U);
  atlas[k_ra8_jof_ofs_bpp]   = 1U;
  atlas[k_ra8_jof_ofs_codec] = (uint8_t)k_ra8_jof_codec_raw;
  internal_write_u32(&atlas[k_ra8_jof_ofs_tile_count], k_test_tile_count);
  (void)memcpy(&atlas[k_test_first_payload_off], top, k_test_tile_bytes);
  (void)memcpy(&atlas[k_test_second_payload_off], bottom, k_test_tile_bytes);
  internal_write_u32(&atlas[k_test_index_off], k_test_first_payload_off);
  internal_write_u32(&atlas[k_test_index_off + k_ra8_jof_idx_ofs_length], k_test_tile_bytes);
  internal_write_u32(&atlas[k_test_index_off + k_ra8_jof_index_entry], k_test_second_payload_off);
  internal_write_u32(&atlas[k_test_index_off + k_ra8_jof_index_entry + k_ra8_jof_idx_ofs_length],
                     k_test_tile_bytes);
  internal_write_u32(&atlas[k_test_footer_off + k_ra8_jof_ftr_index_off], k_test_index_off);
  internal_write_u32(&atlas[k_test_footer_off + k_ra8_jof_ftr_tile_count], k_test_tile_count);
  internal_write_u32(&atlas[k_test_footer_off + k_ra8_jof_ftr_total_size], k_test_atlas_size);
  (void)memcpy(&atlas[k_test_footer_off + k_ra8_jof_ftr_magic],
               "JOFE",
               k_test_magic_bytes); /* MAGIC-OK */
}

/**
 * @brief Encode one final raw-DEFLATE stored block for a two-byte tile
 * @details Emits BFINAL, LEN, NLEN, and the two literal payload bytes.
 * @param[out] dst Seven-byte compressed-stream destination.
 * @param[in] pixels Two decoded fixture bytes.
 * @pre Both pointers are non-null and cover their documented spans.
 * @pre @p dst does not overlap @p pixels.
 * @post @p dst holds one independently decodable raw-DEFLATE stream.
 * @post Decoding the stream yields exactly @p pixels.
 * @note Test-local, thread-safe, and allocation-free.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_write_stored_deflate(uint8_t       dst[k_test_deflate_stream],
                                                       const uint8_t pixels[k_test_tile_bytes])
{
  dst[0] = 0x01U;
  internal_write_u16(&dst[1], k_test_tile_bytes);
  internal_write_u16(&dst[3], (uint16_t)~(uint16_t)k_test_tile_bytes);
  (void)memcpy(&dst[5], pixels, k_test_tile_bytes);
}

/**
 * @brief Construct a valid two-tile raw-DEFLATE JOF atlas
 * @details Mirrors ::internal_make_atlas while storing each tile as an
 * independent final stored block, exercising the compressed audit path.
 * @param[out] atlas Complete deflate atlas buffer.
 * @param[in] top Two decoded bytes for the first tile.
 * @param[in] bottom Two decoded bytes for the second tile.
 * @pre Every pointer is non-null and covers its documented span.
 * @pre @p atlas is writable for ::k_test_deflate_atlas_size bytes.
 * @post The complete atlas parses and covers both compressed tiles exactly.
 * @post Each tile inflates to its corresponding input bytes.
 * @note Test-local, thread-safe, and allocation-free.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_make_deflate_atlas(uint8_t       atlas[k_test_deflate_atlas_size],
                                                     const uint8_t top[k_test_tile_bytes],
                                                     const uint8_t bottom[k_test_tile_bytes])
{
  (void)memset(atlas, 0, k_test_deflate_atlas_size);
  (void)memcpy(&atlas[k_ra8_jof_ofs_magic], "JOF1", k_test_magic_bytes); /* MAGIC-OK */
  internal_write_u16(&atlas[k_ra8_jof_ofs_width], 2U);
  internal_write_u16(&atlas[k_ra8_jof_ofs_height], 2U);
  internal_write_u16(&atlas[k_ra8_jof_ofs_tile_w], 2U);
  internal_write_u16(&atlas[k_ra8_jof_ofs_tile_h], 1U);
  atlas[k_ra8_jof_ofs_bpp]   = 1U;
  atlas[k_ra8_jof_ofs_codec] = (uint8_t)k_ra8_jof_codec_deflate;
  internal_write_u32(&atlas[k_ra8_jof_ofs_tile_count], k_test_tile_count);
  internal_write_stored_deflate(&atlas[k_test_first_payload_off], top);
  internal_write_stored_deflate(&atlas[k_test_deflate_second_off], bottom);
  internal_write_u32(&atlas[k_test_deflate_index_off], k_test_first_payload_off);
  internal_write_u32(&atlas[k_test_deflate_index_off + k_ra8_jof_idx_ofs_length],
                     k_test_deflate_stream);
  internal_write_u32(&atlas[k_test_deflate_index_off + k_ra8_jof_index_entry],
                     k_test_deflate_second_off);
  internal_write_u32(
    &atlas[k_test_deflate_index_off + k_ra8_jof_index_entry + k_ra8_jof_idx_ofs_length],
    k_test_deflate_stream);
  internal_write_u32(&atlas[k_test_deflate_footer_off + k_ra8_jof_ftr_index_off],
                     k_test_deflate_index_off);
  internal_write_u32(&atlas[k_test_deflate_footer_off + k_ra8_jof_ftr_tile_count],
                     k_test_tile_count);
  internal_write_u32(&atlas[k_test_deflate_footer_off + k_ra8_jof_ftr_total_size],
                     k_test_deflate_atlas_size);
  (void)memcpy(&atlas[k_test_deflate_footer_off + k_ra8_jof_ftr_magic],
               "JOFE",
               k_test_magic_bytes); /* MAGIC-OK */
}

/**
 * @brief Read from the immutable in-memory fixture backing
 * @details Models positioned short reads and one injected backend failure.
 * @param[in] ctx ::test_store_t backing description.
 * @param[in] offset Absolute byte offset.
 * @param[out] buf Caller destination.
 * @param[in] len Maximum readable byte count.
 * @param[out] got Actual initialized byte count.
 * @return Fixture read status.
 * @retval k_ra8_ok The bounded read or EOF completed.
 * @retval k_ra8_fail Fault injection is active.
 * @pre @p ctx, @p buf, and @p got are non-null.
 * @pre @p buf is writable for @p len bytes.
 * @post Success sets @p got no greater than @p len.
 * @post Failure sets @p got to zero.
 * @note Test-local and allocation-free.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_test_pread(void* ctx, uint64_t offset, uint8_t* buf, size_t len, size_t* got)
{
  test_store_t* store = (test_store_t*)ctx;
  *got                = 0U;
  if (store->fail || (store->fail_at_offset && (offset == store->fail_offset))) {
    return k_ra8_fail;
  }
  if (offset >= store->len) {
    return k_ra8_ok;
  }
  size_t n = store->len - (size_t)offset;
  if (n > len) {
    n = len;
  }
  (void)memcpy(buf, &store->bytes[offset], n);
  *got = n;
  return k_ra8_ok;
}

/**
 * @brief Run one audit against caller-selected workspace capacities
 * @details Binds the memory reader and forwards exact spans to the public API.
 * @param[in] atlas Complete synthetic atlas.
 * @param[in,out] records Two-entry record storage.
 * @param[in] record_cap Advertised record capacity.
 * @param[in,out] tile Two-byte decoded tile storage.
 * @param[in] tile_cap Advertised tile capacity.
 * @param[out] result Audit result storage.
 * @return Public audit status.
 * @retval k_ra8_ok The fixture passed.
 * @retval k_ra8_err_invalid_size An advertised workspace is undersized.
 * @pre Every pointer is non-null.
 * @pre Advertised capacities do not exceed their physical arrays.
 * @post No input atlas byte is modified.
 * @post Success reports two decoded tiles.
 * @note Test-local and allocation-free.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_run_audit(uint8_t                 atlas[k_test_atlas_size],
                                                 ra8_jof_audit_record_t  records[k_test_tile_count],
                                                 uint32_t                record_cap,
                                                 uint8_t                 tile[k_test_tile_bytes],
                                                 uint32_t                tile_cap,
                                                 ra8_jof_audit_result_t* result)
{
  test_store_t              store = {.bytes          = atlas,
                                     .len            = k_test_atlas_size,
                                     .fail           = false,
                                     .fail_at_offset = false,
                                     .fail_offset    = 0U};
  ra8_jof_audit_workspace_t ws    = {.records     = records,
                                     .record_cap  = record_cap,
                                     .tile        = tile,
                                     .tile_cap    = tile_cap,
                                     .scratch     = nullptr,
                                     .scratch_cap = 0U};
  return ra8_jof_audit(internal_test_pread, &store, store.len, &ws, result);
}

/** @brief Qualify exact raw-atlas requirements and decoded evidence. @details Executes the raw audit scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL
static void internal_test_raw_audit(void)
{
  const uint8_t distinct_top[k_test_tile_bytes]    = {1U, 2U};
  const uint8_t distinct_bottom[k_test_tile_bytes] = {3U, 4U};
  uint8_t       atlas[k_test_atlas_size];
  internal_make_atlas(atlas, distinct_top, distinct_bottom);
  test_store_t store = {.bytes = atlas, .len = sizeof(atlas)};

  ra8_jof_audit_requirements_t need = {};
  CHECK(ra8_jof_audit_requirements(internal_test_pread, &store, store.len, &need) == k_ra8_ok);
  CHECK(need.record_count == 2U);
  CHECK(need.tile_bytes == 2U);
  CHECK(need.scratch_bytes == 0U);

  ra8_jof_audit_record_t records[k_test_tile_count] = {};
  uint8_t                tile[k_test_tile_bytes]    = {};
  ra8_jof_audit_result_t result                     = {};
  CHECK(internal_run_audit(atlas, records, 2U, tile, sizeof(tile), &result) == k_ra8_ok);
  CHECK(result.decoded_tiles == 2U);
  CHECK((result.coverage_errors == 0U) && (result.geometry_errors == 0U));
  CHECK(result.duplicate_candidates == 0U);
}

/** @brief Qualify exact compressed-atlas scratch and decoded evidence. @details Executes the deflate audit scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL
static void internal_test_deflate_audit(void)
{
  const uint8_t distinct_top[k_test_tile_bytes]    = {1U, 2U};
  const uint8_t distinct_bottom[k_test_tile_bytes] = {3U, 4U};
  uint8_t       atlas[k_test_deflate_atlas_size]   = {};
  internal_make_deflate_atlas(atlas, distinct_top, distinct_bottom);
  test_store_t store = {.bytes = atlas, .len = sizeof(atlas)};

  ra8_jof_audit_requirements_t need = {};
  CHECK(ra8_jof_audit_requirements(internal_test_pread, &store, store.len, &need) == k_ra8_ok);
  CHECK((need.record_count == k_test_tile_count) && (need.tile_bytes == k_test_tile_bytes) &&
        (need.scratch_bytes == k_test_deflate_scratch));
  ra8_jof_audit_record_t    records[k_test_tile_count]      = {};
  uint8_t                   tile[k_test_tile_bytes]         = {};
  uint8_t                   scratch[k_test_deflate_scratch] = {};
  ra8_jof_audit_result_t    result                          = {};
  ra8_jof_audit_workspace_t workspace                       = {.records     = records,
                                                               .record_cap  = k_test_tile_count,
                                                               .tile        = tile,
                                                               .tile_cap    = sizeof(tile),
                                                               .scratch     = scratch,
                                                               .scratch_cap = sizeof(scratch)};
  CHECK(ra8_jof_audit(internal_test_pread, &store, store.len, &workspace, &result) == k_ra8_ok);
  CHECK((result.decoded_tiles == k_test_tile_count) && (result.coverage_errors == 0U) &&
        (result.geometry_errors == 0U));
  CHECK((records[0].payload == 2U) && !records[0].uniform);
}

/** @brief Reject undersized and overlapping audit workspaces transactionally. @details Executes the workspace guards scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL
static void internal_test_workspace_guards(void)
{
  const uint8_t top[k_test_tile_bytes]    = {1U, 2U};
  const uint8_t bottom[k_test_tile_bytes] = {3U, 4U};
  uint8_t       atlas[k_test_atlas_size];
  internal_make_atlas(atlas, top, bottom);
  test_store_t           store = {.bytes = atlas, .len = sizeof(atlas)};
  ra8_jof_audit_record_t records[k_test_tile_count];
  uint8_t                tile[k_test_tile_bytes];
  ra8_jof_audit_result_t result = {};
  (void)memset(records, k_test_record_poison_byte, sizeof(records));
  (void)memset(tile, k_test_tile_poison_byte, sizeof(tile));
  CHECK(internal_run_audit(atlas, records, 1U, tile, sizeof(tile), &result) ==
        k_ra8_err_invalid_size);
  CHECK(records[0].offset == k_test_record_poison);
  CHECK(internal_run_audit(atlas, records, 2U, tile, 1U, &result) == k_ra8_err_invalid_size);

  ra8_jof_audit_workspace_t alias = {.records    = records,
                                     .record_cap = k_test_tile_count,
                                     .tile       = (uint8_t*)records,
                                     .tile_cap   = k_test_tile_bytes};
  result                          = (ra8_jof_audit_result_t){.decoded_tiles = k_test_record_poison};
  CHECK(ra8_jof_audit(internal_test_pread, &store, store.len, &alias, &result) ==
        k_ra8_err_invalid_arg);
  CHECK(result.decoded_tiles == k_test_record_poison);

  test_alias_t result_alias = {};
  alias                     = (ra8_jof_audit_workspace_t){.records    = result_alias.records,
                                                          .record_cap = k_test_tile_count,
                                                          .tile       = tile,
                                                          .tile_cap   = sizeof(tile)};
  CHECK(ra8_jof_audit(internal_test_pread, &store, store.len, &alias, &result_alias.result) ==
        k_ra8_err_invalid_arg);
}

/** @brief Distinguish duplicates, uniform tiles, and reversed payload coverage. @details Executes the duplicate and coverage scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL
static void internal_test_duplicate_and_coverage(void)
{
  uint8_t                atlas[k_test_atlas_size];
  ra8_jof_audit_record_t records[k_test_tile_count]   = {};
  uint8_t                tile[k_test_tile_bytes]      = {};
  ra8_jof_audit_result_t result                       = {};
  const uint8_t          duplicate[k_test_tile_bytes] = {9U, 10U};
  internal_make_atlas(atlas, duplicate, duplicate);
  CHECK(internal_run_audit(atlas, records, 2U, tile, sizeof(tile), &result) == k_ra8_ok);
  CHECK(result.duplicate_candidates == 1U);

  const uint8_t uniform[k_test_tile_bytes] = {7U, 7U};
  internal_make_atlas(atlas, uniform, uniform);
  CHECK(internal_run_audit(atlas, records, 2U, tile, sizeof(tile), &result) == k_ra8_ok);
  CHECK(result.duplicate_candidates == 0U);

  const uint8_t top[k_test_tile_bytes]    = {1U, 2U};
  const uint8_t bottom[k_test_tile_bytes] = {3U, 4U};
  internal_make_atlas(atlas, top, bottom);
  internal_write_u32(&atlas[k_test_index_off], k_test_second_payload_off);
  internal_write_u32(&atlas[k_test_index_off + k_ra8_jof_index_entry], k_test_first_payload_off);
  CHECK(internal_run_audit(atlas, records, 2U, tile, sizeof(tile), &result) ==
        k_ra8_err_validation_failed);
  CHECK(result.coverage_errors >= 1U);
}

/** @brief Preserve public outputs when positioned reads fail. @details Executes the read failures scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL
static void internal_test_read_failures(void)
{
  const uint8_t top[k_test_tile_bytes]    = {1U, 2U};
  const uint8_t bottom[k_test_tile_bytes] = {3U, 4U};
  uint8_t       atlas[k_test_atlas_size];
  internal_make_atlas(atlas, top, bottom);
  test_store_t              store                      = {.bytes = atlas, .len = sizeof(atlas)};
  ra8_jof_audit_record_t    records[k_test_tile_count] = {};
  uint8_t                   tile[k_test_tile_bytes]    = {};
  ra8_jof_audit_result_t    result                     = {.decoded_tiles = k_test_record_poison};
  ra8_jof_audit_workspace_t workspace                  = {.records    = records,
                                                          .record_cap = k_test_tile_count,
                                                          .tile       = tile,
                                                          .tile_cap   = sizeof(tile)};
  store.fail_at_offset                                 = true;
  store.fail_offset                                    = k_test_second_payload_off;
  CHECK(ra8_jof_audit(internal_test_pread, &store, store.len, &workspace, &result) == k_ra8_fail);
  CHECK(result.decoded_tiles == k_test_record_poison);

  store.fail                        = true;
  ra8_jof_audit_requirements_t need = {.record_count  = k_test_record_poison,
                                       .tile_bytes    = k_test_record_poison,
                                       .scratch_bytes = k_test_record_poison};
  CHECK(ra8_jof_audit_requirements(internal_test_pread, &store, store.len, &need) == k_ra8_fail);
  CHECK((need.record_count == k_test_record_poison) && (need.tile_bytes == k_test_record_poison) &&
        (need.scratch_bytes == k_test_record_poison));
  CHECK(ra8_jof_audit_requirements(nullptr, &store, store.len, &need) == k_ra8_err_null_ptr);
}

/**
 * @brief Exercise exact capacities, malformed coverage, and duplicate evidence
 * @details Builds deterministic raw atlases and verifies every public outcome.
 * @return Process exit status.
 * @retval 0 Every assertion passed.
 * @retval 1 At least one assertion failed.
 * @pre No external file or network resource is required.
 * @pre Standard output and error streams are available.
 * @post The return code reflects all accumulated assertions.
 * @post No persistent artifact is created.
 * @note Single-threaded test entry point.
 * @since 0.1.0
 */
int main(void)
{
  internal_test_raw_audit();
  internal_test_deflate_audit();
  internal_test_workspace_guards();
  internal_test_duplicate_and_coverage();
  internal_test_read_failures();
  if (s_failures != 0) {
    return 1;
  }
  (void)internal_test_output_fd_text(
    STDOUT_FILENO,
    "portable JOF audit: exact buffers, corruption, duplicates passed\n");
  return 0;
}
