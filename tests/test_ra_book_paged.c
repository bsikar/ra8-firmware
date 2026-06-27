/**
 * @file test_ra_book_paged.c
 * @brief Equivalence tests for the paged ra_book accessor mode (#163).
 *
 * @details
 * Builds a small, self-contained `.rabook` blob in memory (the inflated form: a
 * valid header + 2 chapters + a 10-node DOM + a string pool with whitespace and
 * block elements), then walks each chapter's plain text two ways:
 *   - resident, via ::ra_book_src_resident (zero-copy `base + off`), and
 *   - paged, via ::ra_book_src_paged over an ::ra_vmem cache fronting an
 *     ::ra_vsource object backed by the same blob bytes.
 *
 * The cache is deliberately tiny (8 frames x 64 bytes = 512 bytes for a ~520-byte
 * blob), so the node table and string pool span many frames and the walk forces
 * real evictions and re-faults. The acceptance bar for #163 is that the paged
 * walk produces byte-for-byte identical text to the resident walk (and to the
 * legacy ra_book_chapter_text()), proving the cache is a transparent data source.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ra_book.h"
#include "ra_book_paged.h"
#include "ra_err.h"
#include "ra_vmem.h"
#include "ra_vsource.h"
#include "unity_minimal.h"

/** @brief CRC-32/ISO-HDLC over the blob body (matches ra_book_validate). */
static uint32_t pbook_crc32(const uint8_t* data, size_t len)
{
  uint32_t crc = 0xFFFFFFFFU;
  for (size_t i = 0U; i < len; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0U; bit < 8U; ++bit) {
      const uint32_t mask = (uint32_t)(-(int32_t)(crc & 1U));
      crc                 = (crc >> 1U) ^ (0xEDB88320U & mask);
    }
  }
  return crc ^ 0xFFFFFFFFU;
}

/**
 * @brief Self-contained `.rabook` blob: 2 chapters over a 10-node DOM.
 * @details Contiguous fixed-layout record so byte offsets address the same bytes
 *          a real inflated blob would; the paged backing reads these bytes.
 */
typedef struct {
  ra_book_header_t     hdr;
  ra_book_chapter_t    chapters[2];
  ra_book_node_t       nodes[10];
  ra_book_attr_t       attrs[1];
  ra_book_stylesheet_t stylesheets[1];
  ra_book_image_t      images[1];
  char                 strings[320];
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

/**
 * @brief Populate a valid 2-chapter book fixture with block + whitespace text.
 * @details Chapter 0: `<div><p>"  Hello   world  "</p><p>"Second para."</p></div>`.
 *          Chapter 1: `<section><h1>"Heading One"</h1><p>"Body  with   spaces."</p></section>`.
 */
static void pbook_setup(pbook_t* b)
{
  memset(b, 0, sizeof(*b));
  memcpy(b->hdr.magic, "RABOOK1", 8);
  b->hdr.format_version = k_ra_book_format_version;
  b->hdr.total_size     = (uint32_t)sizeof(*b);

  b->hdr.chapter_count    = 2U;
  b->hdr.chapter_off      = (uint32_t)offsetof(pbook_t, chapters);
  b->hdr.node_count       = 10U;
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
  b->chapters[1].root_node = 5U;

  /* Chapter 0: div > (p > text), (p > text). */
  b->nodes[0] = (ra_book_node_t){.kind         = k_ra_book_node_element,
                                 .name_off     = s_div,
                                 .first_child  = 1U,
                                 .next_sibling = k_ra_book_nil};
  b->nodes[1] = (ra_book_node_t){.kind         = k_ra_book_node_element,
                                 .name_off     = s_p,
                                 .first_child  = 2U,
                                 .next_sibling = 3U};
  b->nodes[2] = (ra_book_node_t){.kind         = k_ra_book_node_text,
                                 .text_off     = s_t0,
                                 .first_child  = k_ra_book_nil,
                                 .next_sibling = k_ra_book_nil};
  b->nodes[3] = (ra_book_node_t){.kind         = k_ra_book_node_element,
                                 .name_off     = s_p,
                                 .first_child  = 4U,
                                 .next_sibling = k_ra_book_nil};
  b->nodes[4] = (ra_book_node_t){.kind         = k_ra_book_node_text,
                                 .text_off     = s_t1,
                                 .first_child  = k_ra_book_nil,
                                 .next_sibling = k_ra_book_nil};

  /* Chapter 1: section > (h1 > text), (p > text). */
  b->nodes[5] = (ra_book_node_t){.kind         = k_ra_book_node_element,
                                 .name_off     = s_section,
                                 .first_child  = 6U,
                                 .next_sibling = k_ra_book_nil};
  b->nodes[6] = (ra_book_node_t){.kind         = k_ra_book_node_element,
                                 .name_off     = s_h1,
                                 .first_child  = 7U,
                                 .next_sibling = 8U};
  b->nodes[7] = (ra_book_node_t){.kind         = k_ra_book_node_text,
                                 .text_off     = s_t2,
                                 .first_child  = k_ra_book_nil,
                                 .next_sibling = k_ra_book_nil};
  b->nodes[8] = (ra_book_node_t){.kind         = k_ra_book_node_element,
                                 .name_off     = s_p,
                                 .first_child  = 9U,
                                 .next_sibling = k_ra_book_nil};
  b->nodes[9] = (ra_book_node_t){.kind         = k_ra_book_node_text,
                                 .text_off     = s_t3,
                                 .first_child  = k_ra_book_nil,
                                 .next_sibling = k_ra_book_nil};

  b->attrs[0]       = (ra_book_attr_t){.name_off = s_class, .value_off = s_main};
  b->stylesheets[0] = (ra_book_stylesheet_t){.source_off = 0U, .scope_chapter = k_ra_book_nil};
  b->images[0]      = (ra_book_image_t){.id_off = 0U, .format = k_ra_book_image_gray4};
  b->hdr.cover_image_index = k_ra_book_nil;

  const uint8_t* body     = (const uint8_t*)b + sizeof(ra_book_header_t);
  const uint32_t body_len = (uint32_t)(sizeof(*b) - sizeof(ra_book_header_t));
  b->hdr.crc32            = pbook_crc32(body, body_len);
}

/* --- paged backing: read straight from the fixture blob bytes --------------- */

/** @brief The fixture blob the paged backing reads from. */
static const uint8_t* s_pbook_bytes;

/** @brief Length of ::s_pbook_bytes. */
static uint32_t s_pbook_len;

/** @brief ra_vsource read callback: copy a byte range out of the fixture blob. */
static ra_err_t pbook_read(void* ctx, uint64_t offset, uint8_t* buf, uint32_t len)
{
  (void)ctx;
  if ((offset + (uint64_t)len) > (uint64_t)s_pbook_len) {
    return k_ra_err_out_of_range;
  }
  memcpy(buf, s_pbook_bytes + (size_t)offset, len);
  return k_ra_ok;
}

/** @brief When set, ::pbook_read_fault fails any backing read at/after the
 *         threshold; lets a test arm a fault only after the header is bound. */
static bool s_pbook_fault_armed;

/** @brief Byte offset at/after which ::pbook_read_fault returns an error. */
static uint32_t s_pbook_fault_off;

/** @brief Faulting variant of ::pbook_read used to exercise the paged read /
 *         walk fault-propagation branches (frames at/after the threshold fail). */
static ra_err_t pbook_read_fault(void* ctx, uint64_t offset, uint8_t* buf, uint32_t len)
{
  (void)ctx;
  if (s_pbook_fault_armed && (offset >= (uint64_t)s_pbook_fault_off)) {
    return k_ra_err_validation_failed;
  }
  if ((offset + (uint64_t)len) > (uint64_t)s_pbook_len) {
    return k_ra_err_out_of_range;
  }
  memcpy(buf, s_pbook_bytes + (size_t)offset, len);
  return k_ra_ok;
}

/* Tiny page cache: 8 frames x 64 bytes < the ~520-byte blob, so the walk evicts. */
typedef enum : uint32_t {
  k_pb_frame_bytes = 64U,
  k_pb_frame_count = 8U,
  k_pb_buckets     = 16U,
} pbook_cache_dim_t;

static uint8_t         s_pb_frames[(size_t)k_pb_frame_count * (size_t)k_pb_frame_bytes];
static ra_vmem_frame_t s_pb_meta[k_pb_frame_count];
static int32_t         s_pb_buckets[k_pb_buckets];

/**
 * @test test_ra_book_paged_matches_resident
 * @brief Paged chapter text is byte-identical to the resident / legacy walk.
 */
static void test_ra_book_paged_matches_resident(void)
{
  TEST_BEGIN("ra_book paged chapter text == resident");

  static pbook_t book;
  pbook_setup(&book);
  s_pbook_bytes = (const uint8_t*)&book;
  s_pbook_len   = (uint32_t)sizeof(book);

  /* The fixture must be a well-formed blob the resident accessors accept. */
  TEST_ASSERT_EQ(k_ra_ok, ra_book_validate(&book, sizeof(book)));

  /* Resident source. */
  ra_book_src_t rsrc = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_book_src_resident(&rsrc, &book, (uint32_t)sizeof(book)));

  /* Paged source over an ra_vmem cache fed by an ra_vsource paged object. */
  ra_vsource_obj_t objs[1];
  ra_vsource_t     vs = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_vsource_init(&vs, objs, 1U));
  uint32_t oid = 0U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_vsource_add_paged(&vs, pbook_read, nullptr, 0U, (uint64_t)sizeof(book), &oid));

  ra_vmem_cfg_t cfg = {
    .frame_mem    = s_pb_frames,
    .frame_bytes  = (uint32_t)k_pb_frame_bytes,
    .frame_count  = (uint32_t)k_pb_frame_count,
    .meta         = s_pb_meta,
    .buckets      = s_pb_buckets,
    .bucket_count = (uint32_t)k_pb_buckets,
    .loader       = ra_vsource_loader,
    .loader_ctx   = &vs,
  };
  ra_vmem_t vm = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_vmem_init(&vm, &cfg));

  ra_book_src_t psrc = {};
  TEST_ASSERT_EQ(
    k_ra_ok,
    ra_book_src_paged(&psrc, &vm, oid, (uint32_t)k_pb_frame_bytes, (uint32_t)sizeof(book)));

  /* MC/DC for ra_book_src_paged's `(size == 0U) || (frame_bytes < 2)` guard
   * (2 conditions, OR; N+1 = 3) plus its null guards:
   *  V1 (both false): the successful bind above -> k_ra_ok.
   *  V2 (size == 0, frame_bytes ok): C1 true  -> k_ra_err_invalid_size.
   *  V3 (size ok, frame_bytes < 2):  C2 true  -> k_ra_err_invalid_size. */
  ra_book_src_t bad = {};
  TEST_ASSERT_EQ(
    k_ra_err_null_ptr,
    ra_book_src_paged(nullptr, &vm, oid, (uint32_t)k_pb_frame_bytes, (uint32_t)sizeof(book)));
  TEST_ASSERT_EQ(
    k_ra_err_null_ptr,
    ra_book_src_paged(&bad, nullptr, oid, (uint32_t)k_pb_frame_bytes, (uint32_t)sizeof(book)));
  TEST_ASSERT_EQ(k_ra_err_invalid_size,
                 ra_book_src_paged(&bad, &vm, oid, (uint32_t)k_pb_frame_bytes, 0U)); /* V2 */
  TEST_ASSERT_EQ(k_ra_err_invalid_size,
                 ra_book_src_paged(&bad, &vm, oid, 1U, (uint32_t)sizeof(book))); /* V3 */

  /* For every chapter, the three text paths must agree byte-for-byte. */
  for (uint32_t ch = 0U; ch < book.hdr.chapter_count; ++ch) {
    char   legacy[512] = {};
    char   res[512]    = {};
    char   pag[512]    = {};
    size_t l_len       = 0U;
    size_t r_len       = 0U;
    size_t p_len       = 0U;

    TEST_ASSERT_EQ(k_ra_ok, ra_book_chapter_text(&book, ch, legacy, sizeof legacy, &l_len));
    TEST_ASSERT_EQ(k_ra_ok, ra_book_chapter_text_src(&rsrc, ch, res, sizeof res, &r_len));
    TEST_ASSERT_EQ(k_ra_ok, ra_book_chapter_text_src(&psrc, ch, pag, sizeof pag, &p_len));

    TEST_ASSERT(l_len > 0U);
    TEST_ASSERT_EQ((int64_t)l_len, (int64_t)r_len); /* resident src == legacy   */
    TEST_ASSERT_EQ((int64_t)l_len, (int64_t)p_len); /* paged == legacy (len)    */
    TEST_ASSERT_EQ(0, memcmp(legacy, res, l_len));  /* resident src bytes match */
    TEST_ASSERT_EQ(0, memcmp(legacy, pag, l_len));  /* paged bytes match (#163) */
  }

  /* The cache actually paged: misses occurred and the tiny budget forced evicts. */
  uint32_t hits = 0U;
  uint32_t miss = 0U;
  uint32_t evic = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_vmem_stats(&vm, &hits, &miss, &evic));
  TEST_ASSERT(miss > 0U);
  TEST_ASSERT(evic > 0U);

  /* Paged walk overflow arm: a 1-byte output forces an emit to fail mid-walk
   * (the `ok == false` exit of the walk loop) -> k_ra_err_invalid_size. */
  char   tiny[1] = {};
  size_t tlen    = 0U;
  TEST_ASSERT_EQ(k_ra_err_invalid_size,
                 ra_book_chapter_text_src(&psrc, 0U, tiny, sizeof tiny, &tlen));

  TEST_END("ra_book paged chapter text == resident");
}

/**
 * @test test_ra_book_paged_guards
 * @brief Source binding + read primitive reject bad arguments.
 */
static void test_ra_book_paged_guards(void)
{
  TEST_BEGIN("ra_book paged source guards");

  static pbook_t book;
  pbook_setup(&book);

  ra_book_src_t rsrc = {};
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_book_src_resident(nullptr, &book, (uint32_t)sizeof(book)));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_book_src_resident(&rsrc, nullptr, (uint32_t)sizeof(book)));
  TEST_ASSERT_EQ(k_ra_err_invalid_size, ra_book_src_resident(&rsrc, &book, 0U));
  TEST_ASSERT_EQ(k_ra_ok, ra_book_src_resident(&rsrc, &book, (uint32_t)sizeof(book)));

  /* Read primitive: range + null guards. */
  uint8_t dst[16] = {};
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_book_src_read(nullptr, 0U, dst, 1U));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_book_src_read(&rsrc, 0U, nullptr, 1U));
  TEST_ASSERT_EQ(k_ra_err_out_of_range, ra_book_src_read(&rsrc, (uint32_t)sizeof(book), dst, 1U));
  TEST_ASSERT_EQ(k_ra_ok, ra_book_src_read(&rsrc, 0U, dst, 8U)); /* header magic */
  TEST_ASSERT_EQ(0, memcmp(dst, "RABOOK1", 7));
  TEST_ASSERT_EQ(k_ra_ok, ra_book_src_read(&rsrc, 0U, dst, 0U)); /* len==0 short-circuit */

  /* chapter_text_src: arg + range guards. */
  char   out[64] = {};
  size_t len     = 0U;
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_book_chapter_text_src(nullptr, 0U, out, sizeof out, &len));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_book_chapter_text_src(&rsrc, book.hdr.chapter_count, out, sizeof out, &len));

  TEST_END("ra_book paged source guards");
}

/**
 * @brief One-chapter fixture whose body is a single long text run.
 * @details The run is longer than one paged staging chunk
 *          (k_ra_book_paged_strbuf - 1 == 255) so the paged emitter chunks it,
 *          and a whitespace sequence straddles the 255-byte boundary so the
 *          across-chunk whitespace-collapse carry is exercised.
 */
typedef struct {
  ra_book_header_t     hdr;
  ra_book_chapter_t    chapters[1];
  ra_book_node_t       nodes[2];
  ra_book_attr_t       attrs[1];
  ra_book_stylesheet_t stylesheets[1];
  ra_book_image_t      images[1];
  char                 strings[1024];
} plbook_t;

/**
 * @test test_ra_book_paged_long_run
 * @brief A text run spanning multiple paged chunks stays byte-identical.
 *
 * @par Coverage:
 * Drives ra_book_paged_emit_run's multi-chunk path (the "no NUL in chunk" arm:
 * `nlen == chunk`) and the whitespace-collapse carry across a chunk boundary --
 * the trickiest paged behaviour for byte-identity.
 */
static void test_ra_book_paged_long_run(void)
{
  TEST_BEGIN("ra_book paged long text run == resident");

  static plbook_t b;
  memset(&b, 0, sizeof(b));
  memcpy(b.hdr.magic, "RABOOK1", 8);
  b.hdr.format_version    = k_ra_book_format_version;
  b.hdr.total_size        = (uint32_t)sizeof(b);
  b.hdr.chapter_count     = 1U;
  b.hdr.chapter_off       = (uint32_t)offsetof(plbook_t, chapters);
  b.hdr.node_count        = 2U;
  b.hdr.node_off          = (uint32_t)offsetof(plbook_t, nodes);
  b.hdr.attr_count        = 0U;
  b.hdr.attr_off          = (uint32_t)offsetof(plbook_t, attrs);
  b.hdr.stylesheet_count  = 0U;
  b.hdr.stylesheet_off    = (uint32_t)offsetof(plbook_t, stylesheets);
  b.hdr.image_count       = 0U;
  b.hdr.image_off         = (uint32_t)offsetof(plbook_t, images);
  b.hdr.string_off        = (uint32_t)offsetof(plbook_t, strings);
  b.hdr.string_size       = (uint32_t)sizeof(b.strings);
  b.hdr.image_pool_off    = (uint32_t)offsetof(plbook_t, strings);
  b.hdr.image_pool_size   = 0U;
  b.hdr.cover_image_index = k_ra_book_nil;

  uint32_t cur         = 0U;
  b.strings[cur++]     = '\0'; /* offset 0 = empty string */
  const uint32_t s_div = cur;
  memcpy(&b.strings[cur], "div", 4U);
  cur += 4U;
  /* Long run: 253 'x', then 4 spaces straddling the 255-byte chunk boundary,
   * then "END" -> 260 bytes, forcing a second chunk mid-whitespace. */
  const uint32_t s_run = cur;
  for (uint32_t i = 0U; i < 253U; ++i) {
    b.strings[cur++] = 'x';
  }
  for (uint32_t i = 0U; i < 4U; ++i) {
    b.strings[cur++] = ' ';
  }
  memcpy(&b.strings[cur], "END", 4U);
  cur += 4U;

  b.chapters[0].root_node = 0U;
  b.nodes[0]              = (ra_book_node_t){.kind         = k_ra_book_node_element,
                                             .name_off     = s_div,
                                             .first_child  = 1U,
                                             .next_sibling = k_ra_book_nil};
  b.nodes[1]              = (ra_book_node_t){.kind         = k_ra_book_node_text,
                                             .text_off     = s_run,
                                             .first_child  = k_ra_book_nil,
                                             .next_sibling = k_ra_book_nil};

  const uint8_t* body     = (const uint8_t*)&b + sizeof(ra_book_header_t);
  const uint32_t body_len = (uint32_t)(sizeof(b) - sizeof(ra_book_header_t));
  b.hdr.crc32             = pbook_crc32(body, body_len);
  TEST_ASSERT_EQ(k_ra_ok, ra_book_validate(&b, sizeof(b)));

  s_pbook_bytes = (const uint8_t*)&b;
  s_pbook_len   = (uint32_t)sizeof(b);

  ra_book_src_t rsrc = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_book_src_resident(&rsrc, &b, (uint32_t)sizeof(b)));

  ra_vsource_obj_t objs[1];
  ra_vsource_t     vs = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_vsource_init(&vs, objs, 1U));
  uint32_t oid = 0U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_vsource_add_paged(&vs, pbook_read, nullptr, 0U, (uint64_t)sizeof(b), &oid));
  ra_vmem_cfg_t cfg = {
    .frame_mem    = s_pb_frames,
    .frame_bytes  = (uint32_t)k_pb_frame_bytes,
    .frame_count  = (uint32_t)k_pb_frame_count,
    .meta         = s_pb_meta,
    .buckets      = s_pb_buckets,
    .bucket_count = (uint32_t)k_pb_buckets,
    .loader       = ra_vsource_loader,
    .loader_ctx   = &vs,
  };
  ra_vmem_t vm = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_vmem_init(&vm, &cfg));
  ra_book_src_t psrc = {};
  TEST_ASSERT_EQ(
    k_ra_ok,
    ra_book_src_paged(&psrc, &vm, oid, (uint32_t)k_pb_frame_bytes, (uint32_t)sizeof(b)));

  char   res[512] = {};
  char   pag[512] = {};
  size_t r_len    = 0U;
  size_t p_len    = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_book_chapter_text_src(&rsrc, 0U, res, sizeof res, &r_len));
  TEST_ASSERT_EQ(k_ra_ok, ra_book_chapter_text_src(&psrc, 0U, pag, sizeof pag, &p_len));
  TEST_ASSERT(r_len > 255U); /* the run spanned more than one chunk */
  TEST_ASSERT_EQ((int64_t)r_len, (int64_t)p_len);
  TEST_ASSERT_EQ(0, memcmp(res, pag, r_len));

  TEST_END("ra_book paged long text run == resident");
}

/**
 * @test test_ra_book_paged_read_faults
 * @brief Unbound-source guard and paged backing-fault propagation.
 *
 * @par Coverage:
 * Two otherwise-unreached error paths in ra_book_paged.c:
 *  - ::ra_book_src_read 's `(src->base == NULL) && (src->vm == NULL)` arm, which
 *    returns ::k_ra_err_invalid_state for a zero-initialised source whose size
 *    is non-zero (neither resident nor paged mode bound).
 *  - The demand-fetch fault path: a backing whose loader starts failing after
 *    the header is bound makes ::ra_vmem_get fault mid-walk, so
 *    `priv_book_src_read_paged` returns the loader error verbatim and
 *    ::ra_book_chapter_text_src propagates a non-ok code instead of `k_ra_ok`.
 */
static void test_ra_book_paged_read_faults(void)
{
  TEST_BEGIN("ra_book paged read faults propagate");

  static pbook_t book;
  pbook_setup(&book);
  s_pbook_bytes = (const uint8_t*)&book;
  s_pbook_len   = (uint32_t)sizeof(book);

  /* (A) Unbound source: base AND vm both NULL, size > 0 -> invalid_state. */
  ra_book_src_t orphan = {};
  orphan.size          = s_pbook_len;
  uint8_t dst[8]       = {};
  TEST_ASSERT_EQ(k_ra_err_invalid_state,
                 ra_book_src_read(&orphan, 0U, dst, (uint32_t)sizeof(dst)));

  /* (B) Paged source over a faulting backing. The header (frames 0..1) binds
   *     while the fault is disarmed; arming it then makes every cold frame
   *     fault, so the chapter walk's first beyond-header node read faults. */
  s_pbook_fault_armed = false;
  s_pbook_fault_off   = (uint32_t)k_pb_frame_bytes * 2U; /* frames 0,1 hold the header */

  ra_vsource_obj_t objs[1];
  ra_vsource_t     vs = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_vsource_init(&vs, objs, 1U));
  uint32_t oid = 0U;
  TEST_ASSERT_EQ(
    k_ra_ok,
    ra_vsource_add_paged(&vs, pbook_read_fault, nullptr, 0U, (uint64_t)sizeof(book), &oid));

  ra_vmem_cfg_t cfg = {
    .frame_mem    = s_pb_frames,
    .frame_bytes  = (uint32_t)k_pb_frame_bytes,
    .frame_count  = (uint32_t)k_pb_frame_count,
    .meta         = s_pb_meta,
    .buckets      = s_pb_buckets,
    .bucket_count = (uint32_t)k_pb_buckets,
    .loader       = ra_vsource_loader,
    .loader_ctx   = &vs,
  };
  ra_vmem_t vm = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_vmem_init(&vm, &cfg));

  ra_book_src_t psrc = {};
  TEST_ASSERT_EQ(
    k_ra_ok,
    ra_book_src_paged(&psrc, &vm, oid, (uint32_t)k_pb_frame_bytes, (uint32_t)sizeof(book)));

  /* Header bound; now make all cold frames fault. */
  s_pbook_fault_armed = true;

  char   out[512] = {};
  size_t olen     = 0U;
  const ra_err_t e = ra_book_chapter_text_src(&psrc, 0U, out, sizeof(out), &olen);
  TEST_ASSERT(e != k_ra_ok); /* the loader fault propagated through the walk */

  s_pbook_fault_armed = false; /* disarm so the backing is reusable */

  TEST_END("ra_book paged read faults propagate");
}

int32_t main(void)
{
  test_ra_book_paged_matches_resident();
  test_ra_book_paged_guards();
  test_ra_book_paged_long_run();
  test_ra_book_paged_read_faults();
  (void)fprintf(stderr, "[OK ] test_ra_book_paged.c\n");
  return 0;
}
