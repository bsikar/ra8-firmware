/**
 * @file test_ra_book_cov.c
 * @brief Line-coverage tests for the compiled-book container loader
 *        (libs/ra_book/src/ra_book.c).
 *
 * @details
 * Drives the public `ra_book_validate()` and `ra_book_open()` entry points over
 * hand-built `.rabook` blobs and "RBKZ" containers so the validator's
 * size/offset guards and the open path's container-header check,
 * inflate-and-validate stage, and entry-point null guards all execute. The
 * focus is plain line coverage (gcovr), not MC/DC: each malformed or edge blob
 * is shaped to run one previously-unexecuted source line and the returned
 * ::ra_err_t is asserted.
 *
 * Source lines targeted in libs/ra_book/src/ra_book.c:
 * - 151 .......... `ra_book_validate` total_size out-of-range guard.
 * - 182-203 ...... `ra_book_container_inflated_size` ("RBKZ" header read).
 * - 210-233 ...... `ra_book_inflate_and_validate` (inflate + validate stage).
 * - 236-257 ...... `ra_book_open` entry guards, container call, and dispatch.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra_book.h"
#include "ra_err.h"
#include "unity_minimal.h"

/**
 * @enum bc_dim_t
 * @brief Fixture sizes, container offsets, and inflater control values.
 */
typedef enum : uint32_t {
  k_bc_str_cap        = 128U,        /**< String-pool bytes in the fixture.        */
  k_bc_node_cap       = 2U,          /**< DOM node-table slots in the fixture.     */
  k_bc_magic_len      = 4U,          /**< Length of the "RBKZ" container magic.    */
  k_bc_short_len      = 4U,          /**< File length below the container header.  */
  k_bc_scratch_cap    = 4096U,       /**< Scratch buffer capacity for inflate.     */
  k_bc_tiny_cap       = 16U,         /**< Scratch too small for the inflated size. */
  k_bc_bogus_inflated = 0x10000000U, /**< Inflated size that overflows scratch.    */
  k_bc_wrong_size     = 7U,          /**< "produced" mismatch for the inflater.    */
} bc_dim_t;

/**
 * @struct bc_book_t
 * @brief A flat, in-memory `.rabook` blob the validator can accept.
 * @details Mirrors the on-disk layout: a header carrying table offsets, then a
 *          single chapter, a small node table, and a NUL-separated string pool.
 */
typedef struct {
  ra_book_header_t  hdr;                   /**< Container header with offsets. */
  ra_book_chapter_t chapters[1];           /**< Single spine chapter.          */
  ra_book_node_t    nodes[k_bc_node_cap];  /**< DOM node table.                */
  char              strings[k_bc_str_cap]; /**< NUL-separated string pool.     */
} bc_book_t;

/**
 * @struct bc_container_t
 * @brief A `.rabook` file: the "RBKZ" header followed by a flat blob payload.
 * @details The host inflater treats the payload as an already-inflated blob and
 *          copies it verbatim into scratch, so the container exercises the open
 *          path without a real DEFLATE codec.
 */
typedef struct {
  uint8_t   magic[k_bc_magic_len]; /**< Always "RBKZ".                          */
  uint32_t  inflated_size;         /**< Little-endian inflated blob length.     */
  bc_book_t blob;                  /**< Verbatim flat blob the inflater copies. */
} bc_container_t;

/**
 * @var s_inflate_force_err
 * @brief When non-ok, ::bc_inflate returns this code without copying.
 * @note Per-process test scaffolding; reset before each open that needs it.
 */
static ra_err_t s_inflate_force_err = k_ra_ok;

/**
 * @var s_inflate_force_produced
 * @brief When non-zero, ::bc_inflate reports this `out_len` instead of the copy.
 * @note Per-process test scaffolding; reset before each open that needs it.
 */
static size_t s_inflate_force_produced = 0U;

/** @brief Table-driven reflected CRC-32 matching the validator's trailer CRC. */
static uint32_t bc_crc32(const uint8_t* data, size_t len)
{
  uint32_t crc = 0xFFFFFFFFU;
  for (size_t i = 0U; i < len; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0U; bit < 8U; ++bit) {
      uint32_t mask = (uint32_t)(-(int32_t)(crc & 1U));
      crc           = (crc >> 1U) ^ (0xEDB88320U & mask);
    }
  }
  return crc ^ 0xFFFFFFFFU;
}

/**
 * @brief Build a minimal, valid `.rabook` blob and stamp its body CRC.
 * @details One empty chapter, two empty nodes, an empty string pool slot. The
 *          tables fit inside `total_size`, so ra_book_validate() accepts it.
 */
static void bc_build_blob(bc_book_t* b)
{
  memset(b, 0, sizeof(*b));
  memcpy(b->hdr.magic, "RABOOK1", 8);
  b->hdr.format_version = k_ra_book_format_version;
  b->hdr.total_size     = sizeof(*b);

  b->hdr.chapter_count = 1U;
  b->hdr.chapter_off   = (uint32_t)offsetof(bc_book_t, chapters);
  b->hdr.node_count    = k_bc_node_cap;
  b->hdr.node_off      = (uint32_t)offsetof(bc_book_t, nodes);
  b->hdr.string_off    = (uint32_t)offsetof(bc_book_t, strings);
  b->hdr.string_size   = k_bc_str_cap;

  b->strings[0]            = '\0';
  b->chapters[0].root_node = 0U;
  b->nodes[0].kind         = (uint8_t)k_ra_book_node_element;
  b->nodes[0].first_attr   = k_ra_book_nil;
  b->nodes[0].first_child  = k_ra_book_nil;
  b->nodes[0].next_sibling = k_ra_book_nil;
  b->nodes[1].kind         = (uint8_t)k_ra_book_node_text;
  b->nodes[1].first_attr   = k_ra_book_nil;
  b->nodes[1].first_child  = k_ra_book_nil;
  b->nodes[1].next_sibling = k_ra_book_nil;

  const uint8_t* body     = (const uint8_t*)b + sizeof(ra_book_header_t);
  uint32_t       body_len = sizeof(*b) - (uint32_t)sizeof(ra_book_header_t);
  b->hdr.crc32            = bc_crc32(body, body_len);
}

/** @brief Wrap @p b in a valid "RBKZ" container reporting the right size. */
static void bc_build_container(bc_container_t* c, const bc_book_t* b)
{
  memcpy(c->magic, "RBKZ", k_bc_magic_len);
  c->inflated_size = sizeof(*b);
  c->blob          = *b;
}

/**
 * @brief Test inflater: copy the verbatim payload into scratch (no real codec).
 * @details Honours ::s_inflate_force_err to fake a decompression failure and
 *          ::s_inflate_force_produced to fake a size disagreement. Otherwise it
 *          copies the payload bytes and reports the true length.
 */
static ra_err_t
bc_inflate(const void* src, size_t src_len, void* dst, size_t dst_cap, size_t* out_len)
{
  if (s_inflate_force_err != k_ra_ok) {
    return s_inflate_force_err;
  }
  if (src_len > dst_cap) {
    return k_ra_err_invalid_size;
  }
  memcpy(dst, src, src_len);
  if (s_inflate_force_produced != 0U) {
    *out_len = s_inflate_force_produced;
  } else {
    *out_len = src_len;
  }
  return k_ra_ok;
}

/**
 * @test test_ra_book_validate_total_size_range
 * @brief The `total_size` bounds guard rejects too-small and too-large headers.
 *
 * @par Targeted line(s):
 * libs/ra_book/src/ra_book.c:150-151 -- the
 * `(total < sizeof(header)) || (total > size)` invalid-size return. One vector
 * drives `total_size` below the header size; the other drives it past the
 * supplied buffer length.
 */
static void test_ra_book_validate_total_size_range(void)
{
  TEST_BEGIN("ra_book_validate total_size out-of-range guard");
  bc_book_t b;

  bc_build_blob(&b);
  b.hdr.total_size = (uint32_t)sizeof(ra_book_header_t) - 1U;
  TEST_ASSERT_EQ(k_ra_err_invalid_size, ra_book_validate(&b, sizeof(b)));

  bc_build_blob(&b);
  b.hdr.total_size = (uint32_t)sizeof(b) + 64U;
  TEST_ASSERT_EQ(k_ra_err_invalid_size, ra_book_validate(&b, sizeof(b)));

  TEST_END("ra_book_validate total_size out-of-range guard");
}

/**
 * @test test_ra_book_open_null_guards
 * @brief Every required pointer argument is rejected by the entry-point guards.
 *
 * @par Targeted line(s):
 * libs/ra_book/src/ra_book.c:236, 244-248 -- the `ra_book_open` entry and its
 * five `RA_CHECK_NULL_PTR` guards (file, inflate, scratch, out_base, out_size),
 * each returning ::k_ra_err_null_ptr.
 */
static void test_ra_book_open_null_guards(void)
{
  TEST_BEGIN("ra_book_open null-argument guards");
  bc_book_t      b;
  bc_container_t c;
  bc_build_blob(&b);
  bc_build_container(&c, &b);

  uint8_t     scratch[k_bc_scratch_cap] = {};
  const void* out_base                  = nullptr;
  size_t      out_size                  = 0U;

  TEST_ASSERT_EQ(
    k_ra_err_null_ptr,
    ra_book_open(nullptr, sizeof(c), bc_inflate, scratch, sizeof(scratch), &out_base, &out_size));
  TEST_ASSERT_EQ(
    k_ra_err_null_ptr,
    ra_book_open(&c, sizeof(c), nullptr, scratch, sizeof(scratch), &out_base, &out_size));
  TEST_ASSERT_EQ(
    k_ra_err_null_ptr,
    ra_book_open(&c, sizeof(c), bc_inflate, nullptr, sizeof(scratch), &out_base, &out_size));
  TEST_ASSERT_EQ(
    k_ra_err_null_ptr,
    ra_book_open(&c, sizeof(c), bc_inflate, scratch, sizeof(scratch), nullptr, &out_size));
  TEST_ASSERT_EQ(
    k_ra_err_null_ptr,
    ra_book_open(&c, sizeof(c), bc_inflate, scratch, sizeof(scratch), &out_base, nullptr));

  TEST_END("ra_book_open null-argument guards");
}

/**
 * @test test_ra_book_open_container_header
 * @brief The "RBKZ" container header guards reject short, mis-magicked, and
 *        over-large files.
 *
 * @par Targeted line(s):
 * libs/ra_book/src/ra_book.c:182-200, 250-254 -- the
 * `ra_book_container_inflated_size` header read reached through
 * `ra_book_open`: the `file_len < header_len` short-file return (187-188), the
 * "RBKZ" magic mismatch return (190-193), and the
 * `inflated > scratch_cap` over-capacity return (197-200). The container error
 * also drives the early `if (err != k_ra_ok) return err;` in `ra_book_open`
 * (252-254).
 */
static void test_ra_book_open_container_header(void)
{
  TEST_BEGIN("ra_book_open container header guards");
  bc_book_t      b;
  bc_container_t c;
  bc_build_blob(&b);
  bc_build_container(&c, &b);

  uint8_t     scratch[k_bc_scratch_cap] = {};
  const void* out_base                  = nullptr;
  size_t      out_size                  = 0U;

  /* File shorter than the 8-byte "RBKZ" header (187-188). */
  TEST_ASSERT_EQ(
    k_ra_err_invalid_size,
    ra_book_open(&c, k_bc_short_len, bc_inflate, scratch, sizeof(scratch), &out_base, &out_size));

  /* Wrong container magic (190-193). */
  bc_container_t bad_magic = c;
  memcpy(bad_magic.magic, "XXXX", k_bc_magic_len);
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_book_open(&bad_magic,
                              sizeof(bad_magic),
                              bc_inflate,
                              scratch,
                              sizeof(scratch),
                              &out_base,
                              &out_size));

  /* Declared inflated size larger than scratch_cap (197-200). */
  bc_container_t big = c;
  big.inflated_size  = k_bc_bogus_inflated;
  TEST_ASSERT_EQ(
    k_ra_err_invalid_size,
    ra_book_open(&big, sizeof(big), bc_inflate, scratch, k_bc_tiny_cap, &out_base, &out_size));

  TEST_END("ra_book_open container header guards");
}

/**
 * @test test_ra_book_open_inflate_stage
 * @brief The inflate-and-validate stage surfaces inflater failure, size
 *        disagreement, and a post-inflate CRC mismatch.
 *
 * @par Targeted line(s):
 * libs/ra_book/src/ra_book.c:210-229 -- `ra_book_inflate_and_validate`: the
 * inflater-error pass-through (219-222), the `produced != expected` size guard
 * (224-225), and the post-inflate `ra_book_validate` failure pass-through
 * (227-229). The valid-container leg also runs the success container-read line
 * (202-203) and the open dispatch tail (257).
 */
static void test_ra_book_open_inflate_stage(void)
{
  TEST_BEGIN("ra_book_open inflate-and-validate stage");
  bc_book_t      b;
  bc_container_t c;
  bc_build_blob(&b);
  bc_build_container(&c, &b);

  uint8_t     scratch[k_bc_scratch_cap] = {};
  const void* out_base                  = nullptr;
  size_t      out_size                  = 0U;

  /* Inflater reports a hard failure (219-222). */
  s_inflate_force_err      = k_ra_err_invalid_arg;
  s_inflate_force_produced = 0U;
  TEST_ASSERT_EQ(
    k_ra_err_invalid_arg,
    ra_book_open(&c, sizeof(c), bc_inflate, scratch, sizeof(scratch), &out_base, &out_size));

  /* Inflater succeeds but reports a length that disagrees with the header
   * (224-225). */
  s_inflate_force_err      = k_ra_ok;
  s_inflate_force_produced = k_bc_wrong_size;
  TEST_ASSERT_EQ(
    k_ra_err_invalid_size,
    ra_book_open(&c, sizeof(c), bc_inflate, scratch, sizeof(scratch), &out_base, &out_size));

  /* Inflated bytes form a blob whose CRC no longer matches (227-229). */
  s_inflate_force_err      = k_ra_ok;
  s_inflate_force_produced = 0U;
  bc_container_t corrupt   = c;
  corrupt.blob.hdr.crc32 ^= 0xFFFFFFFFU;
  TEST_ASSERT_EQ(k_ra_err_range_check_failed,
                 ra_book_open(&corrupt,
                              sizeof(corrupt),
                              bc_inflate,
                              scratch,
                              sizeof(scratch),
                              &out_base,
                              &out_size));

  TEST_END("ra_book_open inflate-and-validate stage");
}

/**
 * @test test_ra_book_open_success
 * @brief A well-formed container inflates and validates end-to-end.
 *
 * @par Targeted line(s):
 * libs/ra_book/src/ra_book.c:202-203, 231-233, 257 -- the container-read
 * success return, the inflate-and-validate success return that publishes
 * `*out_base`/`*out_size`, and the `ra_book_open` dispatch tail-call. Confirms
 * the published base aliases scratch and the size equals the blob length.
 */
static void test_ra_book_open_success(void)
{
  TEST_BEGIN("ra_book_open full success path");
  bc_book_t      b;
  bc_container_t c;
  bc_build_blob(&b);
  bc_build_container(&c, &b);

  uint8_t     scratch[k_bc_scratch_cap] = {};
  const void* out_base                  = nullptr;
  size_t      out_size                  = 0U;

  s_inflate_force_err      = k_ra_ok;
  s_inflate_force_produced = 0U;
  TEST_ASSERT_EQ(
    k_ra_ok,
    ra_book_open(&c, sizeof(c), bc_inflate, scratch, sizeof(scratch), &out_base, &out_size));
  TEST_ASSERT(out_base == (const void*)scratch);
  TEST_ASSERT_EQ(sizeof(b), out_size);

  TEST_END("ra_book_open full success path");
}

int main(void)
{
  test_ra_book_validate_total_size_range();
  test_ra_book_open_null_guards();
  test_ra_book_open_container_header();
  test_ra_book_open_inflate_stage();
  test_ra_book_open_success();
  return 0;
}
