/**
 * @file test_ra8_book_paged.c
 * @brief Equivalence tests for the paged ra8_book accessor mode (#163).
 *
 * @details
 * Builds a small, self-contained `.rabook` blob in memory (the inflated form: a
 * valid header + 2 chapters + a 10-node DOM + a string pool with whitespace and
 * block elements), then walks each chapter's plain text two ways:
 *   - resident, via ::ra8_book_src_resident (zero-copy `base + off`), and
 *   - paged, via ::ra8_book_src_paged over an ::ra8_vmem cache fronting an
 *     ::ra8_vsource object backed by the same blob bytes.
 *
 * The cache is deliberately tiny (8 frames x 64 bytes = 512 bytes for a ~520-byte
 * blob), so the node table and string pool span many frames and the walk forces
 * real evictions and re-faults. The acceptance bar for #163 is that the paged
 * walk produces byte-for-byte identical text to the resident walk (and to the
 * legacy ra8_book_chapter_text()), proving the cache is a transparent data source.
 *
 * This sibling owns the resident-vs-paged equivalence, guard, long-run,
 * and read-fault tests; the chunked-container backing and hostile-blob
 * edge tests live in test_ra8_book_paged_edge.c.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "miniz.h"
#include "ra8_book.h"
#include "ra8_book_chunked.h"
#include "ra8_book_paged.h"
#include "ra8_err.h"
#include "ra8_vmem.h"
#include "ra8_vsource.h"
#include "unity_minimal.h"

/**
 * @enum book_paged_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_pbook_long_run_chars =
    253U, /**< Characters in that single text run: more than one paged chunk, so emit_run must take its multi-chunk path and stitch the pieces byte-identically. */
  k_pbook_node_ch2_h1_text   = 7U, /**< Text node under that section's <h1>.   */
  k_pbook_node_ch2_para_text = 9U, /**< Text node under its <p>.               */
  k_pbook_node_ch2_section   = 5U, /**< The <section> that chapter 2 roots at. */
  k_pbook_node_count =
    10, /**< Nodes in the fixture book: the nodes[] extent and the header's node_count are the same fact. */
  k_pbook_name_cap = 64, /**< Capacity of the single-name scratch buffer. */
} book_paged_uint8_const_t;

/**
 * @enum book_paged_uint16_const_t
 * @brief Named uint16_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint16_t {
  k_pbook_long_strings_cap =
    1024, /**< String-pool capacity of the long-run fixture, sized to hold one run longer than a paging chunk. */
  k_pbook_strings_cap = 320, /**< String-pool capacity of the fixture.              */
  k_pbook_render_cap  = 512, /**< Capacity of the rendered-text comparison buffers. */
} book_paged_uint16_const_t;

/**
 * @enum book_paged_uint32_const_t
 * @brief Named uint32_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint32_t {
  k_crc32_init           = 0xFFFFFFFFU, /**< CRC-32 initial value, and the final XOR-out. */
  k_crc32_poly_reflected = 0xEDB88320U, /**< The reflected CRC-32 polynomial.             */
} book_paged_uint32_const_t;

/** @brief CRC-32/ISO-HDLC over the blob body (matches ra8_book_validate). */
static uint32_t pbook_crc32(const uint8_t* data, size_t len)
{
  uint32_t crc = k_crc32_init;
  for (size_t i = 0U; i < len; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0U; bit < 8U; ++bit) {
      const uint32_t mask = (uint32_t)(-(int32_t)(crc & 1U));
      crc                 = (crc >> 1U) ^ (k_crc32_poly_reflected & mask);
    }
  }
  return crc ^ k_crc32_init;
}

/**
 * @brief Self-contained `.rabook` blob: 2 chapters over a 10-node DOM.
 * @details Contiguous fixed-layout record so byte offsets address the same bytes
 *          a real inflated blob would; the paged backing reads these bytes.
 */
typedef struct {
  ra8_book_header_t     hdr;                          /**< Hdr.         */
  ra8_book_chapter_t    chapters[2];                  /**< Chapters.    */
  ra8_book_node_t       nodes[k_pbook_node_count];    /**< Nodes.       */
  ra8_book_attr_t       attrs[1];                     /**< Attrs.       */
  ra8_book_stylesheet_t stylesheets[1];               /**< Stylesheets. */
  ra8_book_image_t      images[1];                    /**< Images.      */
  char                  strings[k_pbook_strings_cap]; /**< Strings.     */
} pbook_t;

/** @brief Intern a NUL-terminated string into the pool; returns its offset. */
static uint32_t pbook_intern(pbook_t* b, uint32_t* cur, const char* s)
{
  const uint32_t off = *cur;
  const size_t   n   = strlen(s) + 1U;
  memcpy(&b->strings[off], s, n);
  *cur += (uint32_t)n;
  return off;
}

/** @brief Fill the fixture header: magic, section counts, and offsets. */
static void pbook_fill_header(pbook_t* b)
{
  memset(b, 0, sizeof(*b));
  memcpy(b->hdr.magic, "RABOOK1", 8);
  b->hdr.format_version = k_ra8_book_format_version;
  b->hdr.total_size     = (uint32_t)sizeof(*b);

  b->hdr.chapter_count    = 2U;
  b->hdr.chapter_off      = (uint32_t)offsetof(pbook_t, chapters);
  b->hdr.node_count       = k_pbook_node_count;
  b->hdr.node_off         = (uint32_t)offsetof(pbook_t, nodes);
  b->hdr.attr_count       = 1U;
  b->hdr.attr_off         = (uint32_t)offsetof(pbook_t, attrs);
  b->hdr.stylesheet_count = 1U;
  b->hdr.stylesheet_off   = (uint32_t)offsetof(pbook_t, stylesheets);
  b->hdr.image_count      = 1U;
  b->hdr.image_off        = (uint32_t)offsetof(pbook_t, images);
  b->hdr.string_off       = (uint32_t)offsetof(pbook_t, strings);
  b->hdr.string_size      = (uint32_t)sizeof(b->strings);
  b->hdr.image_pool_off   = (uint32_t)offsetof(pbook_t, strings); /* empty pool, in-bounds */
  b->hdr.image_pool_size  = 0U;
}

/** @brief Compose one element node row for the fixture DOM. */
static ra8_book_node_t pbook_elem(uint32_t name_off, uint32_t first_child, uint32_t next_sibling)
{
  return (ra8_book_node_t){.kind         = k_ra8_book_node_element,
                           .name_off     = name_off,
                           .first_child  = first_child,
                           .next_sibling = next_sibling};
}

/** @brief Compose one text node row for the fixture DOM. */
static ra8_book_node_t pbook_text(uint32_t text_off)
{
  return (ra8_book_node_t){.kind         = k_ra8_book_node_text,
                           .text_off     = text_off,
                           .first_child  = k_ra8_book_nil,
                           .next_sibling = k_ra8_book_nil};
}

/**
 * @brief Populate a valid 2-chapter book fixture with block + whitespace text.
 * @details Chapter 0: `<div><p>"  Hello   world  "</p><p>"Second para."</p></div>`.
 *          Chapter 1: `<section><h1>"Heading One"</h1><p>"Body  with   spaces."</p></section>`.
 */
static void pbook_setup(pbook_t* b)
{
  pbook_fill_header(b);

  uint32_t cur          = 0U;
  b->strings[cur]       = '\0'; /* offset 0 == empty string */
  cur                   = 1U;
  b->hdr.title_off      = pbook_intern(b, &cur, "Paged Book");
  b->hdr.author_off     = pbook_intern(b, &cur, "Tester");
  b->hdr.language_off   = pbook_intern(b, &cur, "en");
  b->hdr.identifier_off = pbook_intern(b, &cur, "PB-1");

  const uint32_t s_div     = pbook_intern(b, &cur, "div");
  const uint32_t s_p       = pbook_intern(b, &cur, "p");
  const uint32_t s_section = pbook_intern(b, &cur, "section");
  const uint32_t s_h1      = pbook_intern(b, &cur, "h1");
  const uint32_t s_t0      = pbook_intern(b, &cur, "  Hello   world  ");
  const uint32_t s_t1      = pbook_intern(b, &cur, "Second para.");
  const uint32_t s_t2      = pbook_intern(b, &cur, "Heading One");
  const uint32_t s_t3      = pbook_intern(b, &cur, "Body  with   spaces.");
  const uint32_t s_class   = pbook_intern(b, &cur, "class");
  const uint32_t s_main    = pbook_intern(b, &cur, "main");

  b->chapters[0].root_node = 0U;
  b->chapters[1].root_node = k_pbook_node_ch2_section;

  /* Chapter 0: div > (p > text), (p > text). */
  b->nodes[0] = pbook_elem(s_div, 1U, k_ra8_book_nil);
  b->nodes[1] = pbook_elem(s_p, 2U, 3U);
  b->nodes[2] = pbook_text(s_t0);
  b->nodes[3] = pbook_elem(s_p, 4U, k_ra8_book_nil);
  b->nodes[4] = pbook_text(s_t1);

  /* Chapter 1: section > (h1 > text), (p > text). */
  b->nodes[k_pbook_node_ch2_section] = pbook_elem(s_section, 6U, k_ra8_book_nil);
  b->nodes[6]                        = pbook_elem(s_h1, k_pbook_node_ch2_h1_text, 8U);
  b->nodes[k_pbook_node_ch2_h1_text] = pbook_text(s_t2);
  b->nodes[8]                        = pbook_elem(s_p, k_pbook_node_ch2_para_text, k_ra8_book_nil);
  b->nodes[k_pbook_node_ch2_para_text] = pbook_text(s_t3);

  b->attrs[0]       = (ra8_book_attr_t){.name_off = s_class, .value_off = s_main};
  b->stylesheets[0] = (ra8_book_stylesheet_t){.source_off = 0U, .scope_chapter = k_ra8_book_nil};
  b->images[0]      = (ra8_book_image_t){.id_off = 0U, .format = k_ra8_book_image_gray4};
  b->hdr.cover_image_index = k_ra8_book_nil;

  const uint8_t* body     = (const uint8_t*)b + sizeof(ra8_book_header_t);
  const uint32_t body_len = (uint32_t)(sizeof(*b) - sizeof(ra8_book_header_t));
  b->hdr.crc32            = pbook_crc32(body, body_len);
}

/* --- paged backing: read straight from the fixture blob bytes --------------- */

/** @brief The fixture blob the paged backing reads from. */
static const uint8_t* s_pbook_bytes;

/** @brief Length of ::s_pbook_bytes. */
static uint32_t s_pbook_len;

/** @brief ra8_vsource read callback: copy a byte range out of the fixture blob. */
static ra8_err_t pbook_read(void* ctx, uint64_t offset, uint8_t* buf, uint32_t len)
{
  (void)ctx;
  if ((offset + (uint64_t)len) > (uint64_t)s_pbook_len) {
    return k_ra8_err_out_of_range;
  }
  memcpy(buf, s_pbook_bytes + (size_t)offset, len);
  return k_ra8_ok;
}

/** @brief When set, ::pbook_read_fault fails any backing read at/after the
 *         threshold; lets a test arm a fault only after the header is bound. */
static bool s_pbook_fault_armed;

/** @brief Byte offset at/after which ::pbook_read_fault returns an error. */
static uint32_t s_pbook_fault_off;

/** @brief Faulting variant of ::pbook_read used to exercise the paged read /
 *         walk fault-propagation branches (frames at/after the threshold fail). */
static ra8_err_t pbook_read_fault(void* ctx, uint64_t offset, uint8_t* buf, uint32_t len)
{
  (void)ctx;
  if (s_pbook_fault_armed && (offset >= (uint64_t)s_pbook_fault_off)) {
    return k_ra8_err_validation_failed;
  }
  if ((offset + (uint64_t)len) > (uint64_t)s_pbook_len) {
    return k_ra8_err_out_of_range;
  }
  memcpy(buf, s_pbook_bytes + (size_t)offset, len);
  return k_ra8_ok;
}

/* Tiny page cache: 8 frames x 64 bytes < the ~520-byte blob, so the walk evicts. */
typedef enum : uint32_t {
  k_pb_frame_bytes = 64U, /**< Pb frame bytes. */
  k_pb_frame_count = 8U,  /**< Pb frame count. */
  k_pb_buckets     = 16U, /**< Pb buckets.     */
} pbook_cache_dim_t;

static uint8_t          s_pb_frames[(size_t)k_pb_frame_count * (size_t)k_pb_frame_bytes];
static ra8_vmem_frame_t s_pb_meta[k_pb_frame_count];
static int32_t          s_pb_buckets[k_pb_buckets];

/** @brief Bind a paged book source over the shared cache + s_pbook backing. */
static void pbook_bind(ra8_book_src_t*    psrc,
                       ra8_vsource_t*     vs,
                       ra8_vsource_obj_t* objs,
                       ra8_vmem_t*        vm,
                       uint32_t*          out_oid)
{
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vsource_init(vs, objs, 1U));
  uint32_t oid = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_vsource_add_paged(vs, pbook_read, nullptr, 0U, (uint64_t)s_pbook_len, &oid));

  ra8_vmem_cfg_t cfg = {
    .frame_mem    = s_pb_frames,
    .frame_bytes  = (uint32_t)k_pb_frame_bytes,
    .frame_count  = (uint32_t)k_pb_frame_count,
    .meta         = s_pb_meta,
    .buckets      = s_pb_buckets,
    .bucket_count = (uint32_t)k_pb_buckets,
    .loader       = ra8_vsource_loader,
    .loader_ctx   = vs,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_init(vm, &cfg));

  ra8_book_src_t bound = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_book_src_paged(&bound, vm, oid, (uint32_t)k_pb_frame_bytes, s_pbook_len));
  *psrc    = bound;
  *out_oid = oid;
}

/** @brief MC/DC + null-guard vectors for the ra8_book_src_paged bind guard. */
static void pbook_bind_guard_vectors(ra8_vmem_t* vm, uint32_t oid)
{
  /* MC/DC for ra8_book_src_paged's `(size == 0U) || (frame_bytes < 2)` guard
   * (2 conditions, OR; N+1 = 3) plus its null guards:
   *  V1 (both false): the successful bind in the caller -> k_ra8_ok.
   *  V2 (size == 0, frame_bytes ok): C1 true  -> k_ra8_err_invalid_size.
   *  V3 (size ok, frame_bytes < 2):  C2 true  -> k_ra8_err_invalid_size. */
  ra8_book_src_t bad = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_book_src_paged(nullptr, vm, oid, (uint32_t)k_pb_frame_bytes, s_pbook_len));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_book_src_paged(&bad, nullptr, oid, (uint32_t)k_pb_frame_bytes, s_pbook_len));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_book_src_paged(&bad, vm, oid, (uint32_t)k_pb_frame_bytes, 0U)); /* V2 */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_book_src_paged(&bad, vm, oid, 1U, s_pbook_len)); /* V3 */
}

/** @brief For chapter @p ch, the legacy / resident / paged text agree byte-for-byte. */
static void pbook_compare_chapter(const pbook_t*        book,
                                  const ra8_book_src_t* rsrc,
                                  const ra8_book_src_t* psrc,
                                  uint32_t              ch)
{
  char   legacy[k_pbook_render_cap] = {};
  char   res[k_pbook_render_cap]    = {};
  char   pag[k_pbook_render_cap]    = {};
  size_t l_len                      = 0U;
  size_t r_len                      = 0U;
  size_t p_len                      = 0U;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_chapter_text(book, ch, legacy, sizeof legacy, &l_len));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_chapter_text_src(rsrc, ch, res, sizeof res, &r_len));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_chapter_text_src(psrc, ch, pag, sizeof pag, &p_len));

  TEST_ASSERT(l_len > 0U);
  TEST_ASSERT_EQ(l_len, r_len);                  /* resident src == legacy   */
  TEST_ASSERT_EQ(l_len, p_len);                  /* paged == legacy (len)    */
  TEST_ASSERT_EQ(0, memcmp(legacy, res, l_len)); /* resident src bytes match */
  TEST_ASSERT_EQ(0, memcmp(legacy, pag, l_len)); /* paged bytes match (#163) */
}

/**
 * @test test_ra8_book_paged_matches_resident
 * @brief Paged chapter text is byte-identical to the resident / legacy walk.
 *
 * @par MC/DC:
 * Decision: `(size == 0U) || (frame_bytes < k_ra8_book_paged_min_frame)`
 * (2 conditions) in libs/ra8_book/src/ra8_book_paged.c@ra8_book_src_paged:
 * - Vector 1: size=blob, frame_bytes=64 -> false (the successful bind).
 * - Vector 2: size=0,    frame_bytes=64 -> true  (varies size only).
 * - Vector 3: size=blob, frame_bytes=1  -> true  (varies frame_bytes only).
 * Vectors 1+2 and 1+3 prove each condition independently affects the
 * outcome; N+1 = 3 vectors for N = 2 conditions (asserted in the body).
 */
static void test_ra8_book_paged_matches_resident(void)
{
  TEST_BEGIN("ra8_book paged chapter text == resident");

  static pbook_t s_book;
  pbook_setup(&s_book);
  s_pbook_bytes = (const uint8_t*)&s_book;
  s_pbook_len   = (uint32_t)sizeof(s_book);

  /* The fixture must be a well-formed blob the resident accessors accept. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_validate(&s_book, sizeof(s_book)));

  /* Resident source. */
  ra8_book_src_t rsrc = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_src_resident(&rsrc, &s_book, (uint32_t)sizeof(s_book)));

  /* Paged source over an ra8_vmem cache fed by an ra8_vsource paged object. */
  ra8_vsource_obj_t objs[1];
  ra8_vsource_t     vs   = {};
  ra8_vmem_t        vm   = {};
  ra8_book_src_t    psrc = {};
  uint32_t          oid  = 0U;
  pbook_bind(&psrc, &vs, objs, &vm, &oid);
  pbook_bind_guard_vectors(&vm, oid);

  /* For every chapter, the three text paths must agree byte-for-byte. */
  for (uint32_t ch = 0U; ch < s_book.hdr.chapter_count; ++ch) {
    pbook_compare_chapter(&s_book, &rsrc, &psrc, ch);
  }

  /* The cache actually paged: misses occurred and the tiny budget forced evicts. */
  uint32_t hits = 0U;
  uint32_t miss = 0U;
  uint32_t evic = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_stats(&vm, &hits, &miss, &evic));
  TEST_ASSERT(miss > 0U);
  TEST_ASSERT(evic > 0U);

  /* Paged walk overflow arm: a 1-byte output forces an emit to fail mid-walk
   * (the `ok == false` exit of the walk loop) -> k_ra8_err_invalid_size. */
  char   tiny[1] = {};
  size_t tlen    = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_book_chapter_text_src(&psrc, 0U, tiny, sizeof tiny, &tlen));

  TEST_END("ra8_book paged chapter text == resident");
}

/**
 * @test test_ra8_book_paged_guards
 * @brief Source binding + read primitive reject bad arguments.
 *
 * @par MC/DC:
 * (no compound decisions under test -- each binding/read guard asserted
 * here is an independent single-condition check; the paged-bind compound
 * is covered by test_ra8_book_paged_matches_resident)
 */
static void test_ra8_book_paged_guards(void)
{
  TEST_BEGIN("ra8_book paged source guards");

  static pbook_t s_book;
  pbook_setup(&s_book);

  ra8_book_src_t rsrc = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_book_src_resident(nullptr, &s_book, (uint32_t)sizeof(s_book)));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_book_src_resident(&rsrc, nullptr, (uint32_t)sizeof(s_book)));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_book_src_resident(&rsrc, &s_book, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_src_resident(&rsrc, &s_book, (uint32_t)sizeof(s_book)));

  /* Read primitive: range + null guards. */
  uint8_t dst[16] = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_book_src_read(nullptr, 0U, dst, 1U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_book_src_read(&rsrc, 0U, nullptr, 1U));
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 ra8_book_src_read(&rsrc, (uint32_t)sizeof(s_book), dst, 1U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_src_read(&rsrc, 0U, dst, 8U)); /* header magic */
  TEST_ASSERT_EQ(0, memcmp(dst, "RABOOK1", 7));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_src_read(&rsrc, 0U, dst, 0U)); /* len==0 short-circuit */

  /* chapter_text_src: arg + range guards. */
  char   out[k_pbook_name_cap] = {};
  size_t len                   = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_book_chapter_text_src(nullptr, 0U, out, sizeof out, &len));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_book_chapter_text_src(&rsrc, s_book.hdr.chapter_count, out, sizeof out, &len));

  TEST_END("ra8_book paged source guards");
}

/**
 * @brief One-chapter fixture whose body is a single long text run.
 * @details The run is longer than one paged staging chunk
 *          (k_ra8_book_paged_strbuf - 1 == 255) so the paged emitter chunks it,
 *          and a whitespace sequence straddles the 255-byte boundary so the
 *          across-chunk whitespace-collapse carry is exercised.
 */
typedef struct {
  ra8_book_header_t     hdr;                               /**< Hdr.         */
  ra8_book_chapter_t    chapters[1];                       /**< Chapters.    */
  ra8_book_node_t       nodes[2];                          /**< Nodes.       */
  ra8_book_attr_t       attrs[1];                          /**< Attrs.       */
  ra8_book_stylesheet_t stylesheets[1];                    /**< Stylesheets. */
  ra8_book_image_t      images[1];                         /**< Images.      */
  char                  strings[k_pbook_long_strings_cap]; /**< Strings.     */
} plbook_t;

/**
 * @brief Fill the fixture book header: magic, counts, table offsets, sizes.
 *
 * @param[out] b Fixture book to initialise.
 *
 * @pre @p b is non-NULL.
 * @pre The caller has not yet written the string pool or node table.
 * @post Every table offset points at the matching member of @p b.
 * @post The struct is zeroed first, so unset fields read as 0.
 *
 * @note Not thread-safe with respect to @p b.
 */
static void plbook_fill_header(plbook_t* b)
{
  memset(b, 0, sizeof(*b));
  memcpy(b->hdr.magic, "RABOOK1", 8);
  b->hdr.format_version    = k_ra8_book_format_version;
  b->hdr.total_size        = (uint32_t)sizeof(*b);
  b->hdr.chapter_count     = 1U;
  b->hdr.chapter_off       = (uint32_t)offsetof(plbook_t, chapters);
  b->hdr.node_count        = 2U;
  b->hdr.node_off          = (uint32_t)offsetof(plbook_t, nodes);
  b->hdr.attr_count        = 0U;
  b->hdr.attr_off          = (uint32_t)offsetof(plbook_t, attrs);
  b->hdr.stylesheet_count  = 0U;
  b->hdr.stylesheet_off    = (uint32_t)offsetof(plbook_t, stylesheets);
  b->hdr.image_count       = 0U;
  b->hdr.image_off         = (uint32_t)offsetof(plbook_t, images);
  b->hdr.string_off        = (uint32_t)offsetof(plbook_t, strings);
  b->hdr.string_size       = (uint32_t)sizeof(b->strings);
  b->hdr.image_pool_off    = (uint32_t)offsetof(plbook_t, strings);
  b->hdr.image_pool_size   = 0U;
  b->hdr.cover_image_index = k_ra8_book_nil;
}

/**
 * @brief Populate the single-chapter long-run fixture.
 * @details Long run: 253 'x', then 4 spaces straddling the 255-byte chunk
 *          boundary, then "END" -> 260 bytes, forcing a second chunk
 *          mid-whitespace.
 */
static void plbook_setup(plbook_t* b)
{
  plbook_fill_header(b);

  uint32_t cur         = 0U;
  b->strings[cur++]    = '\0'; /* offset 0 = empty string */
  const uint32_t s_div = cur;
  memcpy(&b->strings[cur], "div", 4U);
  cur += 4U;
  const uint32_t s_run = cur;
  for (uint32_t i = 0U; i < k_pbook_long_run_chars; ++i) {
    b->strings[cur++] = 'x';
  }
  for (uint32_t i = 0U; i < 4U; ++i) {
    b->strings[cur++] = ' ';
  }
  memcpy(&b->strings[cur], "END", 4U);
  cur += 4U;

  /* The fixture pool must still have room; this also consumes the final
   * cursor value so the running offset is not a dead store. */
  TEST_ASSERT(cur <= (uint32_t)sizeof(b->strings));

  b->chapters[0].root_node = 0U;
  b->nodes[0]              = (ra8_book_node_t){.kind         = k_ra8_book_node_element,
                                               .name_off     = s_div,
                                               .first_child  = 1U,
                                               .next_sibling = k_ra8_book_nil};
  b->nodes[1]              = (ra8_book_node_t){.kind         = k_ra8_book_node_text,
                                               .text_off     = s_run,
                                               .first_child  = k_ra8_book_nil,
                                               .next_sibling = k_ra8_book_nil};

  const uint8_t* body     = (const uint8_t*)b + sizeof(ra8_book_header_t);
  const uint32_t body_len = (uint32_t)(sizeof(*b) - sizeof(ra8_book_header_t));
  b->hdr.crc32            = pbook_crc32(body, body_len);
}

/**
 * @test test_ra8_book_paged_long_run
 * @brief A text run spanning multiple paged chunks stays byte-identical.
 *
 * @par Coverage:
 * Drives ra8_book_paged_emit_run's multi-chunk path (the "no NUL in chunk" arm:
 * `nlen == chunk`) and the whitespace-collapse carry across a chunk boundary --
 * the trickiest paged behaviour for byte-identity.
 *
 * @par MC/DC:
 * (no compound decisions under test -- byte-identity equivalence across the
 * chunked emitter, not a boolean decision)
 */
static void test_ra8_book_paged_long_run(void)
{
  TEST_BEGIN("ra8_book paged long text run == resident");

  static plbook_t s_b;
  plbook_setup(&s_b);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_validate(&s_b, sizeof(s_b)));

  s_pbook_bytes = (const uint8_t*)&s_b;
  s_pbook_len   = (uint32_t)sizeof(s_b);

  ra8_book_src_t rsrc = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_src_resident(&rsrc, &s_b, (uint32_t)sizeof(s_b)));

  ra8_vsource_obj_t objs[1];
  ra8_vsource_t     vs   = {};
  ra8_vmem_t        vm   = {};
  ra8_book_src_t    psrc = {};
  uint32_t          oid  = 0U;
  pbook_bind(&psrc, &vs, objs, &vm, &oid);

  char   res[k_pbook_render_cap] = {};
  char   pag[k_pbook_render_cap] = {};
  size_t r_len                   = 0U;
  size_t p_len                   = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_chapter_text_src(&rsrc, 0U, res, sizeof res, &r_len));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_chapter_text_src(&psrc, 0U, pag, sizeof pag, &p_len));
  TEST_ASSERT(r_len > 255U); /* the run spanned more than one chunk */
  TEST_ASSERT_EQ(r_len, p_len);
  TEST_ASSERT_EQ(0, memcmp(res, pag, r_len));

  TEST_END("ra8_book paged long text run == resident");
}

/**
 * @test test_ra8_book_paged_read_faults
 * @brief Unbound-source guard and paged backing-fault propagation.
 *
 * @par Coverage:
 * Two otherwise-unreached error paths in ra8_book_paged.c:
 *  - ::ra8_book_src_read 's `(src->base == NULL) && (src->vm == NULL)` arm, which
 *    returns ::k_ra8_err_invalid_state for a zero-initialised source whose size
 *    is non-zero (neither resident nor paged mode bound).
 *  - The demand-fetch fault path: a backing whose loader starts failing after
 *    the header is bound makes ::ra8_vmem_get fault mid-walk, so
 *    `priv_book_src_read_paged` returns the loader error verbatim and
 *    ::ra8_book_chapter_text_src propagates a non-ok code instead of `k_ra8_ok`.
 *
 * @par MC/DC:
 * (no compound decisions under test -- the unbound-source rejection is two
 * sequential single-condition checks (`base != NULL` fast path, then
 * `vm == NULL`), and the fault propagation is an error passthrough)
 */
static void test_ra8_book_paged_read_faults(void)
{
  TEST_BEGIN("ra8_book paged read faults propagate");

  static pbook_t s_book;
  pbook_setup(&s_book);
  s_pbook_bytes = (const uint8_t*)&s_book;
  s_pbook_len   = (uint32_t)sizeof(s_book);

  /* (A) Unbound source: base AND vm both NULL, size > 0 -> invalid_state. */
  ra8_book_src_t orphan = {};
  orphan.size           = s_pbook_len;
  uint8_t dst[8]        = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_book_src_read(&orphan, 0U, dst, (uint32_t)sizeof(dst)));

  /* (B) Paged source over a faulting backing. The header (frames 0..1) binds
   *     while the fault is disarmed; arming it then makes every cold frame
   *     fault, so the chapter walk's first beyond-header node read faults. */
  s_pbook_fault_armed = false;
  s_pbook_fault_off   = (uint32_t)k_pb_frame_bytes * 2U; /* frames 0,1 hold the header */

  ra8_vsource_obj_t objs[1];
  ra8_vsource_t     vs = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vsource_init(&vs, objs, 1U));
  uint32_t oid = 0U;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_vsource_add_paged(&vs, pbook_read_fault, nullptr, 0U, (uint64_t)sizeof(s_book), &oid));

  ra8_vmem_cfg_t cfg = {
    .frame_mem    = s_pb_frames,
    .frame_bytes  = (uint32_t)k_pb_frame_bytes,
    .frame_count  = (uint32_t)k_pb_frame_count,
    .meta         = s_pb_meta,
    .buckets      = s_pb_buckets,
    .bucket_count = (uint32_t)k_pb_buckets,
    .loader       = ra8_vsource_loader,
    .loader_ctx   = &vs,
  };
  ra8_vmem_t vm = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_init(&vm, &cfg));

  ra8_book_src_t psrc = {};
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_book_src_paged(&psrc, &vm, oid, (uint32_t)k_pb_frame_bytes, (uint32_t)sizeof(s_book)));

  /* Header bound; now make all cold frames fault. */
  s_pbook_fault_armed = true;

  char            out[k_pbook_render_cap] = {};
  size_t          olen                    = 0U;
  const ra8_err_t e = ra8_book_chapter_text_src(&psrc, 0U, out, sizeof(out), &olen);
  TEST_ASSERT(e != k_ra8_ok); /* the loader fault propagated through the walk */

  s_pbook_fault_armed = false; /* disarm so the backing is reusable */

  TEST_END("ra8_book paged read faults propagate");
}

int32_t main(void)
{
  test_ra8_book_paged_matches_resident();
  test_ra8_book_paged_guards();
  test_ra8_book_paged_long_run();
  test_ra8_book_paged_read_faults();
  (void)fprintf(stderr, "[OK ] test_ra8_book_paged.c\n");
  return 0;
}
