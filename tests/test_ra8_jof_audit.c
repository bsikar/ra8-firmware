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
 * @enum test_geometry_const_t
 * @brief Layout of the single-tile atlases used for derived-size rejections.
 */
typedef enum : uint32_t {
  k_test_geo_index_off  = 32U, /**< Index directly after the fixed header. */
  k_test_geo_footer_off = 40U, /**< Footer after the single index entry.   */
  k_test_geo_size       = 56U, /**< Header + one index entry + footer.     */
  k_test_geo_tiles      = 1U,  /**< Single-tile grid.                      */
  k_test_geo_dim        = 1U,  /**< One-pixel image width and height.      */
} test_geometry_const_t;

/**
 * @enum test_geometry_tile_t
 * @brief Tile edges whose derived byte counts exceed 32-bit budgets.
 *
 * @details The bound pair is exact rather than merely large: with four bytes
 *          per pixel a tile of 17920 x 53261 pixels is 3817748480 bytes, and
 *          `raw + raw / 8 + 256` is then exactly 2^32, so the stored bound
 *          wraps to zero and must be refused.
 */
typedef enum : uint16_t {
  k_test_geo_huge_tile    = 65535U, /**< Largest tile edge: tile bytes over 2^32. */
  k_test_geo_bound_tile_w = 17920U, /**< Bound-overflow tile width.               */
  k_test_geo_bound_tile_h = 53261U, /**< Bound-overflow tile height.              */
} test_geometry_tile_t;

/**
 * @enum test_pixel_const_t
 * @brief Pixel depth selecting the four-byte JOF layout.
 */
typedef enum : uint8_t {
  k_test_bpp_rgba = 4U, /**< RGBA8888 bytes per pixel. */
} test_pixel_const_t;

/**
 * @enum test_address_const_t
 * @brief Address-ceiling gap used by the unrepresentable-span vector.
 */
typedef enum : uintptr_t {
  k_test_ceiling_gap = 1U, /**< Bytes between a fabricated base and UINTPTR_MAX. */
} test_address_const_t;

/**
 * @enum test_fault_const_t
 * @brief Injected fault magnitudes for the per-tile audit rejections.
 */
typedef enum : uint32_t {
  k_test_offset_ceiling  = UINT32_MAX, /**< Stored offset whose end wraps 32 bits. */
  k_test_reads_per_parse = 2U,         /**< Header plus footer reads in one parse. */
  k_test_tiny_scratch    = 1U,         /**< Scratch capacity below the exact need. */
} test_fault_const_t;

/**
 * @struct test_store_t
 * @brief Immutable memory backing plus injected read-failure controls.
 */
typedef struct test_store_t {
  const uint8_t* bytes;          /**< Complete readable backing bytes.       */
  size_t         len;            /**< Valid byte count at @c bytes.          */
  bool           fail;           /**< Whether every read must fail.          */
  bool           fail_at_offset; /**< Whether one selected offset must fail. */
  uint64_t       fail_offset;    /**< Selected failing absolute offset.      */
  uint32_t       calls;          /**< Positioned reads served so far.        */
  uint32_t       fail_after;     /**< Reads served before failing (0 = off). */
} test_store_t;

/** @brief Shared-address storage for explicit result/record alias testing. */
typedef union test_alias_t {
  ra8_jof_audit_record_t records[k_test_tile_count]; /**< Record view. */
  ra8_jof_audit_result_t result;                     /**< Result view. */
} test_alias_t;

/** @brief Shared-address storage for explicit result/workspace alias testing. */
typedef union test_ws_alias_t {
  ra8_jof_audit_workspace_t workspace; /**< Workspace view. */
  ra8_jof_audit_result_t    result;    /**< Result view.    */
} test_ws_alias_t;

/** @brief Shared-address storage for explicit result/tile alias testing. */
typedef union test_tile_alias_t {
  uint8_t                tile[k_test_tile_bytes]; /**< Decoded-tile view. */
  ra8_jof_audit_result_t result;                  /**< Result view.       */
} test_tile_alias_t;

/**
 * @struct test_deflate_bufs_t
 * @brief Fixed-layout audit buffers separated by more than one stored bound.
 * @details The pads make the scratch-overlap vectors deterministic: a staging
 *          span of ::k_test_deflate_scratch bytes started at one member cannot
 *          reach the next member, so exactly the intended pair overlaps.
 * @invariant Each pad is at least as long as one complete staging span.
 * @see internal_test_scratch_overlap_guards
 */
typedef struct test_deflate_bufs_t {
  ra8_jof_audit_record_t records[k_test_tile_count];          /**< Record array.   */
  uint8_t                pad_records[k_test_deflate_scratch]; /**< Record padding. */
  uint8_t                tile[k_test_tile_bytes];             /**< Decoded tile.   */
  uint8_t                pad_tile[k_test_deflate_scratch];    /**< Tile padding.   */
  uint8_t                scratch[k_test_deflate_scratch];     /**< Staging buffer. */
} test_deflate_bufs_t;

/** @brief The separated deflate audit buffers used by the overlap vectors. */
static test_deflate_bufs_t s_deflate_bufs;

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
  store->calls++;
  if (store->fail || (store->fail_at_offset && (offset == store->fail_offset))) {
    return k_ra8_fail;
  }
  if ((store->fail_after != 0U) && (store->calls > store->fail_after)) {
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
RA8_INTERNAL static ra8_err_t internal_run_audit(const uint8_t           atlas[k_test_atlas_size],
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

/**
 * @brief Assert one audit call reports the expected status.
 * @details Shared by every guard vector that only needs the top-level
 * status checked; the caller poisons or arranges @p result beforehand
 * and inspects it afterward.
 * @param[in,out] store Backing description passed to the injected reader.
 * @param[in,out] ws Workspace passed to the audit call.
 * @param[in,out] result Result storage passed to the audit call.
 * @param[in] expected Expected status from ::ra8_jof_audit.
 * @return Nothing; failures are recorded through ::CHECK.
 * @pre Every pointer is non-null.
 * @pre @p store, @p ws, and @p result are consistent with the vector under test.
 * @post @p result carries whatever ::ra8_jof_audit left in it.
 * @note Test-local and allocation-free.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_expect_audit_status(test_store_t*              store,
                                                      ra8_jof_audit_workspace_t* ws,
                                                      ra8_jof_audit_result_t*    result,
                                                      ra8_err_t                  expected)
{
  CHECK(ra8_jof_audit(internal_test_pread, store, store->len, ws, result) == expected);
  CHECK(result->decoded_tiles == k_test_record_poison);
}

/**
 * @brief Assert the exact two-tile raw-atlas storage requirements.
 * @details Derives the requirements record through the public entry point
 * and checks every field against the fixture's known two-tile geometry.
 * @param[in,out] store Backing description passed to the injected reader.
 * @return Nothing; failures are recorded through ::CHECK.
 * @pre @p store addresses a parseable raw two-tile atlas.
 * @post No caller-visible state outside ::CHECK bookkeeping is modified.
 * @note Test-local and allocation-free.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_expect_raw_requirements(test_store_t* store)
{
  ra8_jof_audit_requirements_t need = {};
  CHECK(ra8_jof_audit_requirements(internal_test_pread, store, store->len, &need) == k_ra8_ok);
  CHECK(need.record_count == 2U);
  CHECK(need.tile_bytes == 2U);
  CHECK(need.scratch_bytes == 0U);
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
  internal_expect_raw_requirements(&store);

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

/**
 * @brief Assert both undersized-capacity workspace vectors are rejected.
 * @details Requests one record when two are needed, then one decoded-tile
 * byte when two are needed, and requires the poisoned record array to
 * survive the first rejection untouched.
 * @param[in] atlas Complete synthetic atlas.
 * @param[in,out] records Two-entry record storage, pre-poisoned by the caller.
 * @param[in,out] tile Two-byte decoded tile storage, pre-poisoned by the caller.
 * @param[in,out] result Audit result storage.
 * @return Nothing; failures are recorded through ::CHECK.
 * @pre @p records and @p tile were poisoned before this call.
 * @pre @p atlas parses far enough to reach capacity validation.
 * @post Both vectors returned k_ra8_err_invalid_size.
 * @post @p records[0] carries the poison byte pattern, not a decoded value.
 * @note Test-local and allocation-free.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_expect_capacity_guards(const uint8_t           atlas[k_test_atlas_size],
                                ra8_jof_audit_record_t  records[k_test_tile_count],
                                uint8_t                 tile[k_test_tile_bytes],
                                ra8_jof_audit_result_t* result)
{
  CHECK(internal_run_audit(atlas, records, 1U, tile, k_test_tile_bytes, result) ==
        k_ra8_err_invalid_size);
  CHECK(records[0].offset == k_test_record_poison);
  CHECK(internal_run_audit(atlas, records, 2U, tile, 1U, result) == k_ra8_err_invalid_size);
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
  internal_expect_capacity_guards(atlas, records, tile, &result);

  ra8_jof_audit_workspace_t alias = {.records    = records,
                                     .record_cap = k_test_tile_count,
                                     .tile       = (uint8_t*)records,
                                     .tile_cap   = k_test_tile_bytes};
  result                          = (ra8_jof_audit_result_t){.decoded_tiles = k_test_record_poison};
  internal_expect_audit_status(&store, &alias, &result, k_ra8_err_invalid_arg);

  test_alias_t result_alias = {};
  alias                     = (ra8_jof_audit_workspace_t){.records    = result_alias.records,
                                                          .record_cap = k_test_tile_count,
                                                          .tile       = tile,
                                                          .tile_cap   = sizeof(tile)};
  CHECK(ra8_jof_audit(internal_test_pread, &store, store.len, &alias, &result_alias.result) ==
        k_ra8_err_invalid_arg);
}

/**
 * @brief Rebuild the atlas from one tile pair and assert its duplicate count.
 * @details Runs the raw audit and checks the reported duplicate-candidate
 * count against the caller's expectation.
 * @param[out] atlas Atlas buffer rebuilt from @p top and @p bottom.
 * @param[in,out] records Two-entry record storage.
 * @param[out] tile Two-byte decoded tile storage.
 * @param[in,out] result Audit result storage.
 * @param[in] top First tile's raw content.
 * @param[in] bottom Second tile's raw content.
 * @param[in] expected_candidates Expected reported duplicate-candidate count.
 * @return Nothing; failures are recorded through ::CHECK.
 * @pre @p atlas holds ::k_test_atlas_size writable bytes.
 * @pre @p top and @p bottom each hold ::k_test_tile_bytes readable bytes.
 * @post The rebuilt atlas parses successfully.
 * @post @p result carries the freshly reported duplicate-candidate count.
 * @note Test-local and allocation-free.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_expect_duplicate_candidates(uint8_t                 atlas[k_test_atlas_size],
                                     ra8_jof_audit_record_t  records[k_test_tile_count],
                                     uint8_t                 tile[k_test_tile_bytes],
                                     ra8_jof_audit_result_t* result,
                                     const uint8_t           top[k_test_tile_bytes],
                                     const uint8_t           bottom[k_test_tile_bytes],
                                     uint32_t                expected_candidates)
{
  internal_make_atlas(atlas, top, bottom);
  CHECK(internal_run_audit(atlas, records, 2U, tile, k_test_tile_bytes, result) == k_ra8_ok);
  CHECK(result->duplicate_candidates == expected_candidates);
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
  internal_expect_duplicate_candidates(atlas, records, tile, &result, duplicate, duplicate, 1U);

  const uint8_t uniform[k_test_tile_bytes] = {7U, 7U};
  internal_expect_duplicate_candidates(atlas, records, tile, &result, uniform, uniform, 0U);

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

/** @brief Construct a single-tile atlas carrying only geometry @details Emits a header, an all-zero index entry, and a footer whose cross-checks accept the caller's tile geometry, so requirement derivation is reached without any stored payload. @param[out] atlas Complete geometry-only atlas buffer. @param[in] tile_w Declared tile width in pixels. @param[in] tile_h Declared tile height in pixels. @param[in] codec Declared per-atlas tile codec. @pre @p atlas is writable for ::k_test_geo_size bytes. @pre @p tile_w and @p tile_h are non-zero, so the parse accepts them. @post The atlas parses as a one-tile grid with the requested geometry. @post No tile stream exists, so only requirement derivation is exercised. @note Test-local, thread-safe, and allocation-free. @since 0.1.0 */
RA8_INTERNAL static void internal_make_geometry_atlas(uint8_t  atlas[k_test_geo_size],
                                                      uint16_t tile_w,
                                                      uint16_t tile_h,
                                                      uint8_t  codec)
{
  (void)memset(atlas, 0, k_test_geo_size);
  (void)memcpy(&atlas[k_ra8_jof_ofs_magic], "JOF1", k_test_magic_bytes); /* MAGIC-OK */
  internal_write_u16(&atlas[k_ra8_jof_ofs_width], (uint16_t)k_test_geo_dim);
  internal_write_u16(&atlas[k_ra8_jof_ofs_height], (uint16_t)k_test_geo_dim);
  internal_write_u16(&atlas[k_ra8_jof_ofs_tile_w], tile_w);
  internal_write_u16(&atlas[k_ra8_jof_ofs_tile_h], tile_h);
  atlas[k_ra8_jof_ofs_bpp]   = (uint8_t)k_test_bpp_rgba;
  atlas[k_ra8_jof_ofs_codec] = codec;
  internal_write_u32(&atlas[k_ra8_jof_ofs_tile_count], k_test_geo_tiles);
  internal_write_u32(&atlas[k_test_geo_footer_off + k_ra8_jof_ftr_index_off], k_test_geo_index_off);
  internal_write_u32(&atlas[k_test_geo_footer_off + k_ra8_jof_ftr_tile_count], k_test_geo_tiles);
  internal_write_u32(&atlas[k_test_geo_footer_off + k_ra8_jof_ftr_total_size], k_test_geo_size);
  (void)memcpy(&atlas[k_test_geo_footer_off + k_ra8_jof_ftr_magic],
               "JOFE",
               k_test_magic_bytes); /* MAGIC-OK */
}

/** @brief Assert one overlapping workspace is refused before any tile is read @details Poisons the result, runs the audit, and requires both the documented rejection code and a completely untouched result object. @param[in,out] store Backing description passed to the injected reader. @param[in,out] ws Workspace whose spans overlap exactly one neighbour. @param[out] out Result storage that does not alias @p ws. @pre @p out is a distinct object from every span described by @p ws. @pre The backing atlas parses, so the audit reaches the overlap check. @post The audit returned k_ra8_err_invalid_arg. @post The poisoned result value survived unchanged. @note Test-local and allocation-free. @since 0.1.0 */
RA8_INTERNAL static void internal_expect_overlap(test_store_t*              store,
                                                 ra8_jof_audit_workspace_t* ws,
                                                 ra8_jof_audit_result_t*    out)
{
  *out = (ra8_jof_audit_result_t){.decoded_tiles = k_test_record_poison};
  CHECK(ra8_jof_audit(internal_test_pread, store, store->len, ws, out) == k_ra8_err_invalid_arg);
  CHECK(out->decoded_tiles == k_test_record_poison);
}

/**
 * @brief Reject each null top-level operand of both audit entry points.
 * @details Supplies one absent pointer per call so each guard is the only
 * reason the request can fail: the requirements' output, the reader, the
 * result, and the workspace itself.
 * @param[in,out] store Backing description passed to the injected reader.
 * @param[in,out] workspace Workspace passed to the guarded calls.
 * @param[in,out] result Result storage passed to the guarded calls.
 * @return Nothing; failures are recorded through ::CHECK.
 * @pre Every pointer is non-null except the one operand under test.
 * @post Every call returned k_ra8_err_null_ptr.
 * @note Test-local and allocation-free.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_expect_top_level_null_guards(test_store_t*              store,
                                                               ra8_jof_audit_workspace_t* workspace,
                                                               ra8_jof_audit_result_t*    result)
{
  CHECK(ra8_jof_audit_requirements(internal_test_pread, store, store->len, nullptr) ==
        k_ra8_err_null_ptr);
  CHECK(ra8_jof_audit(nullptr, store, store->len, workspace, result) == k_ra8_err_null_ptr);
  CHECK(ra8_jof_audit(internal_test_pread, store, store->len, workspace, nullptr) ==
        k_ra8_err_null_ptr);
  CHECK(ra8_jof_audit(internal_test_pread, store, store->len, nullptr, result) ==
        k_ra8_err_null_ptr);
}

/** @brief Reject every null public operand of both audit entry points @details Supplies one absent pointer per call so each guard is the only reason the request can fail, then proves a failed parse is propagated. @pre The synthetic raw atlas parses when no fault is injected. @pre The caller-owned workspace buffers are exclusively owned here. @post Every absent operand returned k_ra8_err_null_ptr. @post A failed requirement derivation propagated verbatim and preserved the caller's result object. @note Test-local and allocation-free. @since 0.1.0 */
RA8_INTERNAL static void internal_test_public_argument_guards(void)
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

  internal_expect_top_level_null_guards(&store, &workspace, &result);
  workspace.records = nullptr;
  CHECK(ra8_jof_audit(internal_test_pread, &store, store.len, &workspace, &result) ==
        k_ra8_err_null_ptr);
  workspace.records = records;
  workspace.tile    = nullptr;
  CHECK(ra8_jof_audit(internal_test_pread, &store, store.len, &workspace, &result) ==
        k_ra8_err_null_ptr);
  workspace.tile = tile;
  store.fail     = true;
  internal_expect_audit_status(&store, &workspace, &result, k_ra8_fail);
}

/** @brief Reject an absent or undersized compressed-stream workspace @details A deflate atlas requires an exact stored-stream bound, so a null scratch and a scratch smaller than that bound are independently refused. @pre The synthetic deflate atlas parses and requires non-zero scratch. @pre Record and tile capacities already satisfy the derived requirements. @post The absent scratch returned k_ra8_err_null_ptr. @post The undersized scratch returned k_ra8_err_invalid_size and left both the caller's result object and the record array untouched. @note Test-local and allocation-free. @since 0.1.0 */
RA8_INTERNAL static void internal_test_workspace_capacity_guards(void)
{
  const uint8_t top[k_test_tile_bytes]           = {1U, 2U};
  const uint8_t bottom[k_test_tile_bytes]        = {3U, 4U};
  uint8_t       atlas[k_test_deflate_atlas_size] = {};
  internal_make_deflate_atlas(atlas, top, bottom);
  test_store_t           store = {.bytes = atlas, .len = sizeof(atlas)};
  ra8_jof_audit_record_t records[k_test_tile_count];
  uint8_t                tile[k_test_tile_bytes]         = {};
  uint8_t                scratch[k_test_deflate_scratch] = {};
  ra8_jof_audit_result_t result                          = {.decoded_tiles = k_test_record_poison};
  (void)memset(records, k_test_record_poison_byte, sizeof(records));
  ra8_jof_audit_workspace_t workspace = {.records     = records,
                                         .record_cap  = k_test_tile_count,
                                         .tile        = tile,
                                         .tile_cap    = sizeof(tile),
                                         .scratch     = nullptr,
                                         .scratch_cap = sizeof(scratch)};

  CHECK(ra8_jof_audit(internal_test_pread, &store, store.len, &workspace, &result) ==
        k_ra8_err_null_ptr);
  workspace.scratch     = scratch;
  workspace.scratch_cap = k_test_tiny_scratch;
  CHECK(ra8_jof_audit(internal_test_pread, &store, store.len, &workspace, &result) ==
        k_ra8_err_invalid_size);
  CHECK(result.decoded_tiles == k_test_record_poison);
  CHECK(records[0].offset == k_test_record_poison);
}

/** @brief Reject workspace spans that overlap the workspace descriptor itself @details Points the record array and then the decoded tile at the descriptor, and finally advertises a tile span whose end address is not representable. @pre The synthetic raw atlas parses and needs no compressed scratch. @pre No fabricated span is ever dereferenced by the implementation. @post Both descriptor overlaps returned k_ra8_err_invalid_arg. @post The unrepresentable span returned k_ra8_err_invalid_size and preserved the caller's result object. @note Test-local and allocation-free. @since 0.1.0 */
RA8_INTERNAL static void internal_test_descriptor_overlap_guards(void)
{
  const uint8_t top[k_test_tile_bytes]    = {1U, 2U};
  const uint8_t bottom[k_test_tile_bytes] = {3U, 4U};
  uint8_t       atlas[k_test_atlas_size];
  internal_make_atlas(atlas, top, bottom);
  test_store_t              store                      = {.bytes = atlas, .len = sizeof(atlas)};
  ra8_jof_audit_record_t    records[k_test_tile_count] = {};
  uint8_t                   tile[k_test_tile_bytes]    = {};
  ra8_jof_audit_result_t    result                     = {};
  ra8_jof_audit_workspace_t workspace                  = {.records    = records,
                                                          .record_cap = k_test_tile_count,
                                                          .tile       = tile,
                                                          .tile_cap   = sizeof(tile)};

  workspace.records = (ra8_jof_audit_record_t*)&workspace;
  internal_expect_overlap(&store, &workspace, &result);
  workspace.records = records;

  workspace.tile = (uint8_t*)&workspace;
  internal_expect_overlap(&store, &workspace, &result);

  workspace.tile = (uint8_t*)(UINTPTR_MAX - (uintptr_t)k_test_ceiling_gap);
  result         = (ra8_jof_audit_result_t){.decoded_tiles = k_test_record_poison};
  CHECK(ra8_jof_audit(internal_test_pread, &store, store.len, &workspace, &result) ==
        k_ra8_err_invalid_size);
  CHECK(result.decoded_tiles == k_test_record_poison);
}

/** @brief Reject a result object that aliases the tile or the descriptor @details Unions place the public result over the decoded-tile buffer and then over the workspace descriptor, which are the two writable spans a caller can most plausibly reuse. @pre The synthetic raw atlas parses and needs no compressed scratch. @pre Every non-aliased span references distinct caller storage. @post Both aliases returned k_ra8_err_invalid_arg. @post Neither the poisoned result nor the descriptor was modified. @note Test-local and allocation-free. @since 0.1.0 */
RA8_INTERNAL static void internal_test_result_alias_guards(void)
{
  const uint8_t top[k_test_tile_bytes]    = {1U, 2U};
  const uint8_t bottom[k_test_tile_bytes] = {3U, 4U};
  uint8_t       atlas[k_test_atlas_size];
  internal_make_atlas(atlas, top, bottom);
  test_store_t           store                      = {.bytes = atlas, .len = sizeof(atlas)};
  ra8_jof_audit_record_t records[k_test_tile_count] = {};
  uint8_t                tile[k_test_tile_bytes]    = {};

  test_tile_alias_t         tile_alias = {.result = {.decoded_tiles = k_test_record_poison}};
  ra8_jof_audit_workspace_t workspace  = {.records    = records,
                                          .record_cap = k_test_tile_count,
                                          .tile       = tile_alias.tile,
                                          .tile_cap   = sizeof(tile_alias.tile)};
  CHECK(ra8_jof_audit(internal_test_pread, &store, store.len, &workspace, &tile_alias.result) ==
        k_ra8_err_invalid_arg);
  CHECK(tile_alias.result.decoded_tiles == k_test_record_poison);

  test_ws_alias_t ws_alias = {};
  ws_alias.workspace       = (ra8_jof_audit_workspace_t){.records    = records,
                                                         .record_cap = k_test_tile_count,
                                                         .tile       = tile,
                                                         .tile_cap   = sizeof(tile)};
  const ra8_jof_audit_record_t* before = ws_alias.workspace.records;
  CHECK(
    ra8_jof_audit(internal_test_pread, &store, store.len, &ws_alias.workspace, &ws_alias.result) ==
    k_ra8_err_invalid_arg);
  CHECK(ws_alias.workspace.records == before);
}

/** @brief Reject a compressed-stream workspace that overlaps another span @details A fixed-layout static block separates the record array, the decoded tile, and the staging buffer by more than one stored-stream bound, so a staging span pointed at one neighbour cannot reach any other. @pre The synthetic deflate atlas parses and requires non-zero scratch. @pre The descriptor and result live outside the static block. @post Staging over the descriptor, the records, and the tile each returned k_ra8_err_invalid_arg. @post Every rejection preserved the caller's result object. @note Test-local and allocation-free. @since 0.1.0 */
RA8_INTERNAL static void internal_test_scratch_overlap_guards(void)
{
  const uint8_t top[k_test_tile_bytes]           = {1U, 2U};
  const uint8_t bottom[k_test_tile_bytes]        = {3U, 4U};
  uint8_t       atlas[k_test_deflate_atlas_size] = {};
  internal_make_deflate_atlas(atlas, top, bottom);
  test_store_t              store  = {.bytes = atlas, .len = sizeof(atlas)};
  ra8_jof_audit_result_t    result = {};
  ra8_jof_audit_workspace_t workspace =
    (ra8_jof_audit_workspace_t){.records     = s_deflate_bufs.records,
                                .record_cap  = k_test_tile_count,
                                .tile        = s_deflate_bufs.tile,
                                .tile_cap    = sizeof(s_deflate_bufs.tile),
                                .scratch     = s_deflate_bufs.scratch,
                                .scratch_cap = sizeof(s_deflate_bufs.scratch)};

  workspace.scratch = (uint8_t*)&workspace;
  internal_expect_overlap(&store, &workspace, &result);

  workspace.scratch = (uint8_t*)s_deflate_bufs.records;
  internal_expect_overlap(&store, &workspace, &result);

  workspace.scratch = s_deflate_bufs.tile;
  internal_expect_overlap(&store, &workspace, &result);
}

/** @brief Preserve public outputs when the audit's own reads or windows fail @details Fails the index-entry read, then the second structural parse, then presents a stored window whose end offset does not fit 32 bits. @pre The synthetic raw atlas parses when no fault is injected. @pre The result object is poisoned before every vector. @post The read faults propagated k_ra8_fail verbatim. @post The 32-bit window overflow returned k_ra8_err_validation_failed and no vector published a partial result. @note Test-local and allocation-free. @since 0.1.0 */
RA8_INTERNAL static void internal_test_tile_fault_paths(void)
{
  const uint8_t top[k_test_tile_bytes]    = {1U, 2U};
  const uint8_t bottom[k_test_tile_bytes] = {3U, 4U};
  uint8_t       atlas[k_test_atlas_size];
  internal_make_atlas(atlas, top, bottom);
  ra8_jof_audit_record_t    records[k_test_tile_count] = {};
  uint8_t                   tile[k_test_tile_bytes]    = {};
  ra8_jof_audit_result_t    result                     = {.decoded_tiles = k_test_record_poison};
  ra8_jof_audit_workspace_t workspace                  = {.records    = records,
                                                          .record_cap = k_test_tile_count,
                                                          .tile       = tile,
                                                          .tile_cap   = sizeof(tile)};

  test_store_t store = {.bytes          = atlas,
                        .len            = sizeof(atlas),
                        .fail_at_offset = true,
                        .fail_offset    = k_test_index_off};
  internal_expect_audit_status(&store, &workspace, &result, k_ra8_fail);

  store =
    (test_store_t){.bytes = atlas, .len = sizeof(atlas), .fail_after = k_test_reads_per_parse};
  internal_expect_audit_status(&store, &workspace, &result, k_ra8_fail);

  internal_write_u32(&atlas[k_test_index_off], k_test_offset_ceiling);
  store = (test_store_t){.bytes = atlas, .len = sizeof(atlas)};
  internal_expect_audit_status(&store, &workspace, &result, k_ra8_err_validation_failed);
}

/** @brief Refuse geometries whose derived byte counts leave the 32-bit budget @details One atlas declares a tile larger than 2^32 bytes; the other declares the exact tile size whose worst-case stored bound is 2^32 and therefore cannot be represented. @pre Both geometry-only atlases satisfy every structural cross-check. @pre The requirements record is poisoned before each vector. @post Both derivations returned k_ra8_err_invalid_size. @post Neither derivation published a usable requirements record. @note Test-local and allocation-free. @since 0.1.0 */
RA8_INTERNAL static void internal_test_geometry_limits(void)
{
  uint8_t atlas[k_test_geo_size] = {};
  internal_make_geometry_atlas(atlas,
                               (uint16_t)k_test_geo_huge_tile,
                               (uint16_t)k_test_geo_huge_tile,
                               (uint8_t)k_ra8_jof_codec_raw);
  test_store_t                 store = {.bytes = atlas, .len = sizeof(atlas)};
  ra8_jof_audit_requirements_t need  = {.record_count  = k_test_record_poison,
                                        .tile_bytes    = k_test_record_poison,
                                        .scratch_bytes = k_test_record_poison};
  CHECK(ra8_jof_audit_requirements(internal_test_pread, &store, store.len, &need) ==
        k_ra8_err_invalid_size);
  CHECK(need.tile_bytes == k_test_record_poison);

  internal_make_geometry_atlas(atlas,
                               (uint16_t)k_test_geo_bound_tile_w,
                               (uint16_t)k_test_geo_bound_tile_h,
                               (uint8_t)k_ra8_jof_codec_deflate);
  store = (test_store_t){.bytes = atlas, .len = sizeof(atlas)};
  CHECK(ra8_jof_audit_requirements(internal_test_pread, &store, store.len, &need) ==
        k_ra8_err_invalid_size);
  CHECK(need.scratch_bytes == k_test_record_poison);
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
  internal_test_public_argument_guards();
  internal_test_workspace_capacity_guards();
  internal_test_descriptor_overlap_guards();
  internal_test_result_alias_guards();
  internal_test_scratch_overlap_guards();
  internal_test_tile_fault_paths();
  internal_test_geometry_limits();
  if (s_failures != 0) {
    return 1;
  }
  (void)internal_test_output_fd_text(
    STDOUT_FILENO,
    "portable JOF audit: exact buffers, corruption, duplicates passed\n");
  return 0;
}
