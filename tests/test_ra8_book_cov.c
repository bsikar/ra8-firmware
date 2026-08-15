/**
 * @file test_ra8_book_cov.c
 * @brief Line-coverage tests for the compiled-book container loader
 *        (libs/ra8_book/src/ra8_book.c).
 *
 * @details
 * Drives the public `ra8_book_validate()` and `ra8_book_open()` entry points over
 * hand-built `.rabook` blobs and "RBKC" chunked containers so the validator's
 * size/offset guards and the open path's header parse, chunk-table walk,
 * per-chunk inflate stage, and entry-point null guards all execute. The focus
 * is plain line coverage (gcovr), not MC/DC: each malformed or edge container
 * is shaped to run one previously-unexecuted branch of the loader
 * (`ra8_book_container_header_fields`, `internal_container_view`, `internal_inflate_chunks`)
 * and the returned ::ra8_err_t is asserted.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_book.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/**
 * @enum book_cov_fixture_t
 * @brief Buffer capacities and payload sizes.
 */
typedef enum : uint8_t {
  k_book_trailing_bytes =
    64U, /**< Bytes the header claims past the end of the blob, so the size check must reject it. */
} book_cov_fixture_t;

/**
 * @enum book_cov_fixture2_t
 * @brief The byte-level helpers.
 */
typedef enum : uint32_t {
  k_crc32_init =
    0xFFFFFFFFU, /**< CRC-32 init value, final XOR-out, and the mask that corrupts a valid CRC. */
  /** The reflected CRC-32 polynomial. */
  k_crc32_poly_reflected = 0xEDB88320U,
} book_cov_fixture2_t;

/**
 * @enum bc_dim_t
 * @brief Fixture sizes, container geometry, and inflater control values.
 */
typedef enum : uint32_t {
  k_bc_str_cap     = 128U,  /**< String-pool bytes in the fixture.          */
  k_bc_node_cap    = 2U,    /**< DOM node-table slots in the fixture.       */
  k_bc_magic_len   = 4U,    /**< Length of the "RBKC" container magic.      */
  k_bc_short_len   = 4U,    /**< File length below the container header.    */
  k_bc_table_cut   = 30U,   /**< File length inside the chunk table.        */
  k_bc_scratch_cap = 4096U, /**< Scratch buffer capacity for inflate.       */
  k_bc_tiny_cap    = 16U,   /**< Scratch too small for the inflated total.  */
  k_bc_wrong_size  = 7U,    /**< "produced" mismatch for the inflater.      */
  k_bc_split_bytes = 64U,   /**< Chunk size for the multi-chunk container.  */
  k_bc_multi_cap   = 8192U, /**< Byte budget for the hand-packed container. */
} bc_dim_t;

/**
 * @struct bc_book_t
 * @brief A flat, in-memory `.rabook` blob the validator can accept.
 * @details Mirrors the on-disk layout: a header carrying table offsets, then a
 *          single chapter, a small node table, and a NUL-separated string pool.
 */
typedef struct {
  ra8_book_header_t  hdr;                   /**< Container header with offsets. */
  ra8_book_chapter_t chapters[1];           /**< Single spine chapter.          */
  ra8_book_node_t    nodes[k_bc_node_cap];  /**< DOM node table.                */
  char               strings[k_bc_str_cap]; /**< NUL-separated string pool.     */
} bc_book_t;

/**
 * @struct bc_container_t
 * @brief A single-chunk "RBKC" `.rabook` file with a verbatim payload.
 * @details Field order mirrors the wire layout exactly (static_asserts below
 *          pin the offsets): fixed header, a two-entry chunk table, then the
 *          flat blob as the one chunk's "stream". The host inflater treats the
 *          payload as an already-inflated blob and copies it verbatim into
 *          scratch, so the container exercises the open path without a real
 *          DEFLATE codec.
 */
typedef struct {
  uint8_t   magic[k_bc_magic_len]; /**< Always "RBKC".                           */
  uint32_t  chunk_bytes;           /**< Inflated bytes per chunk (== blob size). */
  uint64_t  inflated_total;        /**< Flat-blob length (== blob size).         */
  uint32_t  chunk_count;           /**< Always 1 in this fixture.                */
  uint32_t  reserved;              /**< Must be 0.                               */
  uint64_t  offsets[2];            /**< Chunk table: {0, payload length}.        */
  bc_book_t blob;                  /**< Verbatim flat blob the inflater copies.  */
} bc_container_t;

static_assert(offsetof(bc_container_t, offsets) == k_ra8_book_container_header_len,
              "bc_container_t chunk table must sit at the wire offset");
static_assert(offsetof(bc_container_t, blob) ==
                (k_ra8_book_container_header_len + (2U * k_ra8_book_container_entry_len)),
              "bc_container_t payload must follow the two-entry table");

/**
 * @var s_inflate_force_err
 * @brief When non-ok, ::bc_inflate returns this code without copying.
 * @note Per-process test scaffolding; reset before each open that needs it.
 */
static ra8_err_t s_inflate_force_err = k_ra8_ok;

/**
 * @var s_inflate_force_produced
 * @brief When non-zero, ::bc_inflate reports this `out_len` instead of the copy.
 * @note Per-process test scaffolding; reset before each open that needs it.
 */
static size_t s_inflate_force_produced = 0U;

/** @brief Table-driven reflected CRC-32 matching the validator's trailer CRC. */
static uint32_t bc_crc32(const uint8_t* data, size_t len)
{
  uint32_t crc = k_crc32_init;
  for (size_t i = 0U; i < len; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0U; bit < 8U; ++bit) {
      uint32_t mask = (uint32_t)(-(int32_t)(crc & 1U));
      crc           = (crc >> 1U) ^ (k_crc32_poly_reflected & mask);
    }
  }
  return crc ^ k_crc32_init;
}

/**
 * @brief Build a minimal, valid `.rabook` blob and stamp its body CRC.
 * @details One empty chapter, two empty nodes, an empty string pool slot. The
 *          tables fit inside `total_size`, so ra8_book_validate() accepts it.
 */
static void bc_build_blob(bc_book_t* b)
{
  memset(b, 0, sizeof(*b));
  memcpy(b->hdr.magic, "RABOOK1", 8);
  b->hdr.format_version = k_ra8_book_format_version;
  b->hdr.total_size     = sizeof(*b);

  b->hdr.chapter_count = 1U;
  b->hdr.chapter_off   = (uint32_t)offsetof(bc_book_t, chapters);
  b->hdr.node_count    = k_bc_node_cap;
  b->hdr.node_off      = (uint32_t)offsetof(bc_book_t, nodes);
  b->hdr.string_off    = (uint32_t)offsetof(bc_book_t, strings);
  b->hdr.string_size   = k_bc_str_cap;

  b->strings[0]            = '\0';
  b->chapters[0].root_node = 0U;
  b->nodes[0].kind         = (uint8_t)k_ra8_book_node_element;
  b->nodes[0].first_attr   = k_ra8_book_nil;
  b->nodes[0].first_child  = k_ra8_book_nil;
  b->nodes[0].next_sibling = k_ra8_book_nil;
  b->nodes[1].kind         = (uint8_t)k_ra8_book_node_text;
  b->nodes[1].first_attr   = k_ra8_book_nil;
  b->nodes[1].first_child  = k_ra8_book_nil;
  b->nodes[1].next_sibling = k_ra8_book_nil;

  const uint8_t* body     = (const uint8_t*)b + sizeof(ra8_book_header_t);
  uint32_t       body_len = sizeof(*b) - (uint32_t)sizeof(ra8_book_header_t);
  b->hdr.crc32            = bc_crc32(body, body_len);
}

/** @brief Wrap @p b in a valid single-chunk "RBKC" container. */
static void bc_build_container(bc_container_t* c, const bc_book_t* b)
{
  memcpy(c->magic, "RBKC", k_bc_magic_len);
  c->chunk_bytes    = sizeof(*b);
  c->inflated_total = sizeof(*b);
  c->chunk_count    = 1U;
  c->reserved       = 0U;
  c->offsets[0]     = 0U;
  c->offsets[1]     = sizeof(*b);
  c->blob           = *b;
}

/**
 * @brief Test inflater: copy the verbatim payload into scratch (no real codec).
 * @details Honours ::s_inflate_force_err to fake a decompression failure and
 *          ::s_inflate_force_produced to fake a size disagreement. Otherwise it
 *          copies the payload bytes and reports the true length.
 */
static ra8_err_t
bc_inflate(const void* src, size_t src_len, void* dst, size_t dst_cap, size_t* out_len)
{
  if (s_inflate_force_err != k_ra8_ok) {
    return s_inflate_force_err;
  }
  if (src_len > dst_cap) {
    return k_ra8_err_invalid_size;
  }
  memcpy(dst, src, src_len);
  if (s_inflate_force_produced != 0U) {
    *out_len = s_inflate_force_produced;
  } else {
    *out_len = src_len;
  }
  return k_ra8_ok;
}

/**
 * @test test_ra8_book_validate_total_size_range
 * @brief The `total_size` bounds guard rejects too-small and too-large headers.
 *
 * @par Targeted code:
 * `ra8_book_validate`'s `(total < sizeof(header)) || (total > size)` invalid-size
 * return. One vector drives `total_size` below the header size; the other drives
 * it past the supplied buffer length.
 *
 * @par MC/DC:
 * Decision: `(total < sizeof(header)) || (total > size)` (2 conditions) in
 * libs/ra8_book/src/ra8_book.c@ra8_book_validate:
 * - Vector 1: total in range            -> false (every passing open/validate
 *   in this file exercises the all-false leg).
 * - Vector 2: total below the header    -> true  (varies condition 1 only).
 * - Vector 3: total past the buffer     -> true  (varies condition 2 only).
 * N+1 = 3 vectors for N = 2 conditions: minimal MC/DC.
 */
static void test_ra8_book_validate_total_size_range(void)
{
  TEST_BEGIN("ra8_book_validate total_size out-of-range guard");
  bc_book_t b;

  bc_build_blob(&b);
  b.hdr.total_size = (uint32_t)sizeof(ra8_book_header_t) - 1U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_book_validate(&b, sizeof(b)));

  bc_build_blob(&b);
  b.hdr.total_size = (uint32_t)sizeof(b) + k_book_trailing_bytes;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_book_validate(&b, sizeof(b)));

  TEST_END("ra8_book_validate total_size out-of-range guard");
}

/**
 * @enum bc_extent_t
 * @brief Node-count value that keeps the table offset in range but drives its
 *        computed extent (off + count * sizeof(node)) past the blob end.
 */
typedef enum : uint32_t {
  k_bc_overflow_node_count = 0x01000000U, /**< 16M nodes -> extent >> total_size. */
} bc_extent_t;

/**
 * @test test_ra8_book_validate_table_extent
 * @brief The per-table extent guard rejects a table whose offset is in range but
 *        whose (offset + count * elem) reaches past the blob.
 *
 * @par Targeted code:
 * `ra8_book_table_fits`'s `return (off <= total) && (end <= total)` -- the
 * right-operand-false leg. The node table keeps a valid offset (off <= total ->
 * C1 true) but an oversized count pushes the computed end past total (C2 false),
 * so the conjunction is false and ra8_book_validate returns invalid_size.
 *
 * @par MC/DC:
 * Decision: `(off <= total) && (end <= (uint64_t)total)` (2 conditions) in
 * libs/ra8_book/src/ra8_book.c@ra8_book_table_fits:
 * - Vector 1: off <= total, end <= total -> true  (every valid blob validated in
 *   this file supplies the all-true control leg).
 * - Vector 2: off  > total               -> C1 false (chapter_off past the buffer,
 *   supplied by test_ra8_book.c@test_ra8_book_invalid; isolates C1 vs V1).
 * - Vector 3: off <= total, end  > total -> C1 true, C2 false (this test's
 *   oversized node_count; isolates C2 vs V1).
 * V1+V2 prove the offset condition independently flips the outcome; V1+V3 prove
 * the extent condition does. N+1 = 3 vectors for N = 2 conditions: minimal MC/DC.
 */
static void test_ra8_book_validate_table_extent(void)
{
  TEST_BEGIN("ra8_book_validate table extent overflow guard");
  bc_book_t b;

  /* Valid header + in-range node_off, but a node_count whose extent overflows the
   * blob: off <= total (C1 true) while off + count * sizeof(node) > total (C2
   * false). The tables_ok check runs before the CRC, so no CRC recompute needed. */
  bc_build_blob(&b);
  b.hdr.node_count = (uint32_t)k_bc_overflow_node_count;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_book_validate(&b, sizeof(b)));

  TEST_END("ra8_book_validate table extent overflow guard");
}

/**
 * @test test_ra8_book_open_null_guards
 * @brief Every required pointer argument is rejected by the entry-point guards.
 *
 * @par Targeted code:
 * The `ra8_book_open` entry and its five `RA8_CHECK_NULL_PTR` guards (file,
 * inflate, scratch, out_base, out_size), each returning ::k_ra8_err_null_ptr.
 *
 * @par MC/DC:
 * (no compound decisions under test -- each null guard is an independent
 * single-condition check)
 */
static void test_ra8_book_open_null_guards(void)
{
  TEST_BEGIN("ra8_book_open null-argument guards");
  bc_book_t      b;
  bc_container_t c;
  bc_build_blob(&b);
  bc_build_container(&c, &b);

  uint8_t     scratch[k_bc_scratch_cap] = {};
  const void* out_base                  = nullptr;
  size_t      out_size                  = 0U;

  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_book_open(nullptr, sizeof(c), bc_inflate, scratch, sizeof(scratch), &out_base, &out_size));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_book_open(&c, sizeof(c), nullptr, scratch, sizeof(scratch), &out_base, &out_size));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_book_open(&c, sizeof(c), bc_inflate, nullptr, sizeof(scratch), &out_base, &out_size));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_book_open(&c, sizeof(c), bc_inflate, scratch, sizeof(scratch), nullptr, &out_size));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_book_open(&c, sizeof(c), bc_inflate, scratch, sizeof(scratch), &out_base, nullptr));

  TEST_END("ra8_book_open null-argument guards");
}

/**
 * @brief Open a container image and assert the expected header-guard result.
 * @param[in]  want    Expected `ra8_book_open` result code.
 * @param[in]  cont    Container image under test.
 * @param[in]  len     Presented image length in bytes.
 * @param[out] scratch Inflate scratch buffer.
 * @param[in]  scap    Scratch capacity in bytes.
 * @param[out] ob      Receives the mapped output base.
 * @param[out] os      Receives the mapped output size.
 * @pre @p cont references a container image.
 * @post `ra8_book_open` returned @p want.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void bc_expect_open(ra8_err_t             want,
                           const bc_container_t* cont,
                           size_t                len,
                           uint8_t*              scratch,
                           size_t                scap,
                           const void**          ob,
                           size_t*               os)
{
  TEST_ASSERT_EQ(want, ra8_book_open(cont, len, bc_inflate, scratch, scap, ob, os));
}

/**
 * @test test_mcdc_container_header_fields
 * @brief The "RBKC" header-field guards reject short, mis-magicked, and
 *        geometrically inconsistent files.
 *
 * @par Targeted code:
 * `internal_container_view`'s short-file return, then every
 * `ra8_book_container_header_fields` rejection reached through `ra8_book_open`:
 * the magic mismatch, the zero `chunk_bytes`, the zero `inflated_total`, the
 * non-zero `reserved` word, and a `chunk_count` disagreeing with
 * `ceil(total / chunk_bytes)`. Each error also drives the early error return
 * in `ra8_book_open`.
 *
 * @par MC/DC:
 * Decision: `if ((chunk_bytes == 0U) || (total == 0U) || (reserved != 0U))`
 * (3 conditions) in libs/ra8_book/src/ra8_book.c@ra8_book_container_header_fields:
 * - Vector 1: chunk_bytes=blob, total=blob, reserved=0 -> false (the
 *   well-formed fixture; all conditions false -- exercised by every passing
 *   open in this file).
 * - Vector 2: chunk_bytes=0,    total=blob, reserved=0 -> true (varies
 *   chunk_bytes only).
 * - Vector 3: chunk_bytes=blob, total=0,    reserved=0 -> true (varies total
 *   only; note the count check cannot mask it -- the decision runs first).
 * - Vector 4: chunk_bytes=blob, total=blob, reserved=1 -> true (varies
 *   reserved only).
 * Vectors 1+2, 1+3, and 1+4 prove each condition independently affects the
 * outcome; N+1 = 4 vectors for N = 3 conditions: minimal MC/DC.
 */
static void test_mcdc_container_header_fields(void)
{
  TEST_BEGIN("ra8_book_open container header guards");
  bc_book_t      b;
  bc_container_t c;
  bc_build_blob(&b);
  bc_build_container(&c, &b);

  uint8_t      scratch[k_bc_scratch_cap] = {};
  const void*  ob                        = nullptr;
  size_t       os                        = 0U;
  const size_t scap                      = sizeof(scratch);

  /* File shorter than the fixed container header. */
  bc_expect_open(k_ra8_err_invalid_size, &c, k_bc_short_len, scratch, scap, &ob, &os);

  /* Wrong container magic. */
  bc_container_t bad_magic = c;
  memcpy(bad_magic.magic, "XXXX", k_bc_magic_len);
  bc_expect_open(k_ra8_err_invalid_arg, &bad_magic, sizeof(bad_magic), scratch, scap, &ob, &os);

  /* Zero chunk size. */
  bc_container_t zero_chunk = c;
  zero_chunk.chunk_bytes    = 0U;
  bc_expect_open(k_ra8_err_invalid_arg, &zero_chunk, sizeof(zero_chunk), scratch, scap, &ob, &os);

  /* Zero inflated total. */
  bc_container_t zero_total = c;
  zero_total.inflated_total = 0U;
  bc_expect_open(k_ra8_err_invalid_arg, &zero_total, sizeof(zero_total), scratch, scap, &ob, &os);

  /* Reserved word must be zero. */
  bc_container_t bad_reserved = c;
  bad_reserved.reserved       = 1U;
  bc_expect_open(k_ra8_err_invalid_arg,
                 &bad_reserved,
                 sizeof(bad_reserved),
                 scratch,
                 scap,
                 &ob,
                 &os);

  /* Chunk count disagreeing with ceil(total / chunk_bytes). */
  bc_container_t bad_count = c;
  bad_count.chunk_count    = 2U;
  bc_expect_open(k_ra8_err_invalid_arg, &bad_count, sizeof(bad_count), scratch, scap, &ob, &os);

  TEST_END("ra8_book_open container header guards");
}

/**
 * @test test_ra8_book_open_container_geometry
 * @brief The chunk-table walk rejects truncated files, over-capacity totals,
 *        and malformed tables.
 *
 * @par Targeted code:
 * `internal_container_view` after a good header parse: the header-plus-table
 * short-file return, the `total > scratch_cap` return, the `offset[0] != 0`
 * rejection, the non-monotonic-entry rejection, and the
 * `offset[chunk_count] != payload_len` rejection.
 *
 * @par MC/DC:
 * (no compound decisions under test -- every geometry guard is an
 * independent single-condition check)
 */
static void test_ra8_book_open_container_geometry(void)
{
  TEST_BEGIN("ra8_book_open container geometry guards");
  bc_book_t      b;
  bc_container_t c;
  bc_build_blob(&b);
  bc_build_container(&c, &b);

  uint8_t     scratch[k_bc_scratch_cap] = {};
  const void* out_base                  = nullptr;
  size_t      out_size                  = 0U;

  /* File ends inside the chunk table. */
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_size,
    ra8_book_open(&c, k_bc_table_cut, bc_inflate, scratch, sizeof(scratch), &out_base, &out_size));

  /* Inflated total larger than the scratch capacity. */
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_size,
    ra8_book_open(&c, sizeof(c), bc_inflate, scratch, k_bc_tiny_cap, &out_base, &out_size));

  /* Table must start at zero. */
  bc_container_t bad_start = c;
  bad_start.offsets[0]     = 1U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_book_open(&bad_start,
                               sizeof(bad_start),
                               bc_inflate,
                               scratch,
                               sizeof(scratch),
                               &out_base,
                               &out_size));

  /* Table entries must strictly increase. */
  bc_container_t flat_table = c;
  flat_table.offsets[1]     = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_book_open(&flat_table,
                               sizeof(flat_table),
                               bc_inflate,
                               scratch,
                               sizeof(scratch),
                               &out_base,
                               &out_size));

  /* Table end must equal the payload length. */
  bc_container_t bad_end = c;
  bad_end.offsets[1]     = sizeof(b) - 1U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_book_open(&bad_end,
                               sizeof(bad_end),
                               bc_inflate,
                               scratch,
                               sizeof(scratch),
                               &out_base,
                               &out_size));

  TEST_END("ra8_book_open container geometry guards");
}

/**
 * @test test_ra8_book_open_inflate_stage
 * @brief The per-chunk inflate stage surfaces inflater failure, size
 *        disagreement, and a post-inflate CRC mismatch.
 *
 * @par Targeted code:
 * `internal_inflate_chunks`: the inflater-error pass-through and the
 * `produced != expected` size guard, plus the post-inflate `ra8_book_validate`
 * failure pass-through in `ra8_book_open`.
 *
 * @par MC/DC:
 * (no compound decisions under test -- the inflate-stage guards are
 * independent single-condition checks)
 */
static void test_ra8_book_open_inflate_stage(void)
{
  TEST_BEGIN("ra8_book_open per-chunk inflate stage");
  bc_book_t      b;
  bc_container_t c;
  bc_build_blob(&b);
  bc_build_container(&c, &b);

  uint8_t     scratch[k_bc_scratch_cap] = {};
  const void* out_base                  = nullptr;
  size_t      out_size                  = 0U;

  /* Inflater reports a hard failure. */
  s_inflate_force_err      = k_ra8_err_invalid_arg;
  s_inflate_force_produced = 0U;
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_book_open(&c, sizeof(c), bc_inflate, scratch, sizeof(scratch), &out_base, &out_size));

  /* Inflater succeeds but reports a length that disagrees with the chunk span. */
  s_inflate_force_err      = k_ra8_ok;
  s_inflate_force_produced = k_bc_wrong_size;
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_size,
    ra8_book_open(&c, sizeof(c), bc_inflate, scratch, sizeof(scratch), &out_base, &out_size));

  /* Inflated bytes form a blob whose CRC no longer matches. */
  s_inflate_force_err      = k_ra8_ok;
  s_inflate_force_produced = 0U;
  bc_container_t corrupt   = c;
  corrupt.blob.hdr.crc32 ^= k_crc32_init;
  TEST_ASSERT_EQ(k_ra8_err_range_check_failed,
                 ra8_book_open(&corrupt,
                               sizeof(corrupt),
                               bc_inflate,
                               scratch,
                               sizeof(scratch),
                               &out_base,
                               &out_size));

  TEST_END("ra8_book_open per-chunk inflate stage");
}

/**
 * @test test_ra8_book_open_success
 * @brief A well-formed single-chunk container inflates and validates end-to-end.
 *
 * @par Targeted code:
 * `internal_container_view`'s success return, `internal_inflate_chunks`' single-iteration
 * success, and the `ra8_book_open` tail that publishes `*out_base`/`*out_size`.
 * Confirms the published base aliases scratch and the size equals the blob
 * length.
 *
 * @par MC/DC:
 * (no compound decisions under test -- the success path is the all-false
 * leg of every guard, exercised end-to-end)
 */
static void test_ra8_book_open_success(void)
{
  TEST_BEGIN("ra8_book_open full success path");
  bc_book_t      b;
  bc_container_t c;
  bc_build_blob(&b);
  bc_build_container(&c, &b);

  uint8_t     scratch[k_bc_scratch_cap] = {};
  const void* out_base                  = nullptr;
  size_t      out_size                  = 0U;

  s_inflate_force_err      = k_ra8_ok;
  s_inflate_force_produced = 0U;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_book_open(&c, sizeof(c), bc_inflate, scratch, sizeof(scratch), &out_base, &out_size));
  TEST_ASSERT(out_base == (const void*)scratch);
  TEST_ASSERT_EQ(sizeof(b), out_size);

  TEST_END("ra8_book_open full success path");
}

/**
 * @brief Hand-pack a multi-chunk "RBKC" container with verbatim chunk streams.
 * @details Splits @p blob into `chunk_bytes` slices, writes each slice as its
 *          own "stream" (the passthrough inflater copies it verbatim), and
 *          packs the header + table + payload into @p out. Returns the packed
 *          byte count.
 */
static size_t
bc_pack_multi(uint8_t* out, const uint8_t* blob, uint32_t blob_len, uint32_t chunk_bytes)
{
  const uint32_t count = (blob_len + chunk_bytes - 1U) / chunk_bytes;
  size_t         pos   = 0U;
  memcpy(&out[pos], "RBKC", k_bc_magic_len);
  pos += k_bc_magic_len;
  memcpy(&out[pos], &chunk_bytes, sizeof(chunk_bytes));
  pos += sizeof(chunk_bytes);
  const uint64_t total64 = blob_len;
  memcpy(&out[pos], &total64, sizeof(total64));
  pos += sizeof(total64);
  memcpy(&out[pos], &count, sizeof(count));
  pos += sizeof(count);
  const uint32_t reserved = 0U;
  memcpy(&out[pos], &reserved, sizeof(reserved));
  pos += sizeof(reserved);
  for (uint32_t i = 0U; i <= count; ++i) {
    const uint64_t off = (uint64_t)i * chunk_bytes;
    const uint64_t ent = (off < blob_len) ? off : blob_len;
    memcpy(&out[pos], &ent, sizeof(ent));
    pos += sizeof(ent);
  }
  memcpy(&out[pos], blob, blob_len);
  return pos + blob_len;
}

/**
 * @test test_ra8_book_open_multi_chunk
 * @brief A multi-chunk container reassembles the blob across the chunk loop.
 *
 * @par Targeted code:
 * `internal_inflate_chunks`' loop across several chunks including the short final
 * chunk (`expected < chunk_bytes` clamp), and `internal_container_view`'s table walk
 * over more than one entry. The reassembled blob must pass validation and the
 * open must publish the full blob length.
 *
 * @par MC/DC:
 * (no compound decisions under test -- the chunk loop's clamp and length
 * checks are independent single-condition checks)
 */
static void test_ra8_book_open_multi_chunk(void)
{
  TEST_BEGIN("ra8_book_open multi-chunk reassembly");
  bc_book_t b;
  bc_build_blob(&b);

  static uint8_t s_file[k_bc_multi_cap] = {};
  const size_t   file_len =
    bc_pack_multi(s_file, (const uint8_t*)&b, (uint32_t)sizeof(b), k_bc_split_bytes);

  uint8_t     scratch[k_bc_scratch_cap] = {};
  const void* out_base                  = nullptr;
  size_t      out_size                  = 0U;

  s_inflate_force_err      = k_ra8_ok;
  s_inflate_force_produced = 0U;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_book_open(s_file, file_len, bc_inflate, scratch, sizeof(scratch), &out_base, &out_size));
  TEST_ASSERT_EQ(sizeof(b), out_size);
  TEST_ASSERT_EQ(0, memcmp(scratch, &b, sizeof(b)));

  TEST_END("ra8_book_open multi-chunk reassembly");
}

int main(void)
{
  test_ra8_book_validate_total_size_range();
  test_ra8_book_validate_table_extent();
  test_ra8_book_open_null_guards();
  test_mcdc_container_header_fields();
  test_ra8_book_open_container_geometry();
  test_ra8_book_open_inflate_stage();
  test_ra8_book_open_success();
  test_ra8_book_open_multi_chunk();
  return 0;
}
