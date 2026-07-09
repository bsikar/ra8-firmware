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

#include "miniz.h"
#include "ra_book.h"
#include "ra_book_chunked.h"
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
 * @brief Bind a paged ::ra_book_src_t over @p bytes using the shared tiny cache.
 * @details Points the ::pbook_read backing at @p bytes / @p len, wires a single
 *          paged ::ra_vsource object through an ::ra_vmem cache, and binds
 *          @p psrc. Caller owns @p vs / @p objs / @p vm for the source's lifetime.
 * @param[out] psrc Receives the bound paged source.
 * @param[out] vs   Caller-owned vsource registry (initialised here).
 * @param[out] objs Caller-owned one-slot object array for @p vs.
 * @param[out] vm   Caller-owned vmem cache (initialised here).
 * @param[in]  bytes Blob the paged backing serves.
 * @param[in]  len   Length of @p bytes in bytes.
 * @pre All pointers are non-NULL; @p len > 0 and >= sizeof(ra_book_header_t).
 * @pre The shared cache statics are not concurrently bound elsewhere.
 * @post @p psrc is a paged source whose header slice is already cached.
 * @post ::s_pbook_bytes / ::s_pbook_len alias @p bytes / @p len.
 * @note Not thread-safe (mutates the shared backing + cache statics).
 */
static void pbook_bind_paged(ra_book_src_t*    psrc,
                             ra_vsource_t*     vs,
                             ra_vsource_obj_t* objs,
                             ra_vmem_t*        vm,
                             const void*       bytes,
                             uint32_t          len)
{
  s_pbook_bytes = (const uint8_t*)bytes;
  s_pbook_len   = len;
  TEST_ASSERT_EQ(k_ra_ok, ra_vsource_init(vs, objs, 1U));
  uint32_t oid = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_vsource_add_paged(vs, pbook_read, nullptr, 0U, (uint64_t)len, &oid));
  ra_vmem_cfg_t cfg = {
    .frame_mem    = s_pb_frames,
    .frame_bytes  = (uint32_t)k_pb_frame_bytes,
    .frame_count  = (uint32_t)k_pb_frame_count,
    .meta         = s_pb_meta,
    .buckets      = s_pb_buckets,
    .bucket_count = (uint32_t)k_pb_buckets,
    .loader       = ra_vsource_loader,
    .loader_ctx   = vs,
  };
  TEST_ASSERT_EQ(k_ra_ok, ra_vmem_init(vm, &cfg));
  TEST_ASSERT_EQ(k_ra_ok, ra_book_src_paged(psrc, vm, oid, (uint32_t)k_pb_frame_bytes, len));
}

/**
 * @test test_ra_book_paged_matches_resident
 * @brief Paged chapter text is byte-identical to the resident / legacy walk.
 *
 * @par MC/DC:
 * Decision: `(size == 0U) || (frame_bytes < k_ra_book_paged_min_frame)`
 * (2 conditions) in libs/ra_book/src/ra_book_paged.c@ra_book_src_paged:
 * - Vector 1: size=blob, frame_bytes=64 -> false (the successful bind).
 * - Vector 2: size=0,    frame_bytes=64 -> true  (varies size only).
 * - Vector 3: size=blob, frame_bytes=1  -> true  (varies frame_bytes only).
 * Vectors 1+2 and 1+3 prove each condition independently affects the
 * outcome; N+1 = 3 vectors for N = 2 conditions (asserted in the body).
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
    TEST_ASSERT_EQ(l_len, r_len);                  /* resident src == legacy   */
    TEST_ASSERT_EQ(l_len, p_len);                  /* paged == legacy (len)    */
    TEST_ASSERT_EQ(0, memcmp(legacy, res, l_len)); /* resident src bytes match */
    TEST_ASSERT_EQ(0, memcmp(legacy, pag, l_len)); /* paged bytes match (#163) */
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
 *
 * @par MC/DC:
 * (no compound decisions under test -- each binding/read guard asserted
 * here is an independent single-condition check; the paged-bind compound
 * is covered by test_ra_book_paged_matches_resident)
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
 *
 * @par MC/DC:
 * (no compound decisions under test -- byte-identity equivalence across the
 * chunked emitter, not a boolean decision)
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
  TEST_ASSERT_EQ(r_len, p_len);
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
 *
 * @par MC/DC:
 * (no compound decisions under test -- the unbound-source rejection is two
 * sequential single-condition checks (`base != NULL` fast path, then
 * `vm == NULL`), and the fault propagation is an error passthrough)
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
  TEST_ASSERT_EQ(k_ra_err_invalid_state, ra_book_src_read(&orphan, 0U, dst, (uint32_t)sizeof(dst)));

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

  char           out[512] = {};
  size_t         olen     = 0U;
  const ra_err_t e        = ra_book_chapter_text_src(&psrc, 0U, out, sizeof(out), &olen);
  TEST_ASSERT(e != k_ra_ok); /* the loader fault propagated through the walk */

  s_pbook_fault_armed = false; /* disarm so the backing is reusable */

  TEST_END("ra_book paged read faults propagate");
}

/* --- chunked-container backing: the full production stack ------------------- */

/**
 * @enum pbc_dim_t
 * @brief Chunked-backing fixture geometry (chunk size == cache frame size).
 */
typedef enum : uint32_t {
  k_pbc_file_cap    = 2048U, /**< Container build-buffer budget.   */
  k_pbc_table_cap   = 16U,   /**< Chunk-table entry budget.        */
  k_pbc_staging_cap = 128U,  /**< Compressed-chunk staging budget. */
} pbc_dim_t;

/** @brief Static tinfl state (~11 KiB) kept off the test stack. */
static tinfl_decompressor s_pbc_tinfl;

/** @brief zlib inflater matching ra_book_inflate_fn (mirrors the shelf inflater). */
static ra_err_t
pbc_inflate(const void* src, size_t src_len, void* dst, size_t dst_cap, size_t* out_len)
{
  tinfl_init(&s_pbc_tinfl);
  size_t             in_n  = src_len;
  size_t             out_n = dst_cap;
  const tinfl_status st    = tinfl_decompress(
    &s_pbc_tinfl,
    (const mz_uint8*)src,
    &in_n,
    (mz_uint8*)dst,
    (mz_uint8*)dst,
    &out_n,
    (mz_uint32)(TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF));
  if (st != TINFL_STATUS_DONE) {
    return k_ra_err_invalid_size;
  }
  *out_len = out_n;
  return k_ra_ok;
}

/** @brief In-memory container "file" for the chunked reader's byte source. */
typedef struct {
  const uint8_t* data; /**< Container bytes.  */
  uint64_t       len;  /**< Container length. */
} pbc_file_t;

/** @brief ra_vsource_read_fn over the in-memory container file. */
static ra_err_t pbc_file_read(void* ctx, uint64_t offset, uint8_t* buf, uint32_t len)
{
  const pbc_file_t* f = (const pbc_file_t*)ctx;
  if ((offset + (uint64_t)len) > f->len) {
    return k_ra_err_out_of_range;
  }
  memcpy(buf, &f->data[offset], len);
  return k_ra_ok;
}

/**
 * @brief Pack an RBKC container over @p blob with real zlib chunk streams.
 * @details Mirrors tools/epub_compile's wrap_container: one zlib stream per
 *          `chunk_bytes` slice (last short), header + offset table + streams.
 * @return Packed container length in bytes.
 */
static uint64_t pbc_pack(uint8_t* out, const uint8_t* blob, uint32_t blob_len, uint32_t chunk_bytes)
{
  const uint32_t count = (blob_len + chunk_bytes - 1U) / chunk_bytes;
  size_t         pos =
    k_ra_book_container_header_len + ((size_t)(count + 1U) * k_ra_book_container_entry_len);
  uint64_t off = 0U;

  memcpy(&out[0], "RBKC", 4U);
  memcpy(&out[4], &chunk_bytes, sizeof(chunk_bytes));
  const uint64_t total64 = blob_len;
  memcpy(&out[8], &total64, sizeof(total64));
  memcpy(&out[16], &count, sizeof(count));
  const uint32_t reserved = 0U;
  memcpy(&out[20], &reserved, sizeof(reserved));

  for (uint32_t i = 0U; i < count; ++i) {
    memcpy(&out[k_ra_book_container_header_len + ((size_t)i * k_ra_book_container_entry_len)],
           &off,
           sizeof(off));
    uint32_t span = blob_len - (i * chunk_bytes);
    if (span > chunk_bytes) {
      span = chunk_bytes;
    }
    mz_ulong  slen = (mz_ulong)k_pbc_staging_cap;
    const int rc   = mz_compress2(&out[pos + (size_t)off],
                                  &slen,
                                  &blob[i * chunk_bytes],
                                  (mz_ulong)span,
                                  MZ_BEST_COMPRESSION);
    TEST_ASSERT_EQ(MZ_OK, rc);
    off += (uint64_t)slen;
  }
  memcpy(&out[k_ra_book_container_header_len + ((size_t)count * k_ra_book_container_entry_len)],
         &off,
         sizeof(off));
  return (uint64_t)pos + off;
}

/**
 * @test test_ra_book_paged_chunked_backing
 * @brief The full production stack -- RBKC container -> ra_book_chunked_read ->
 *        ra_vsource -> ra_vmem -> ra_book_src_paged -- matches resident text.
 *
 * @par Coverage:
 * The end-to-end acceptance for the chunked container: the fixture blob is
 * compressed into real zlib chunk streams sized to the cache frame
 * (`chunk_bytes == frame_bytes == 64`), every frame fault stages + inflates
 * exactly one chunk, and both chapters' paged text is byte-identical to the
 * resident walk. This is the multi-GB read path in miniature.
 *
 * @par MC/DC:
 * (no compound decisions under test -- end-to-end byte-identity through the
 * chunked backing; the container-header compound is covered by
 * test_mcdc_container_header_fields in test_ra_book_cov.c)
 */
static void test_ra_book_paged_chunked_backing(void)
{
  TEST_BEGIN("ra_book paged over chunked container == resident");

  static pbook_t book;
  pbook_setup(&book);
  TEST_ASSERT_EQ(k_ra_ok, ra_book_validate(&book, sizeof(book)));

  ra_book_src_t rsrc = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_book_src_resident(&rsrc, &book, (uint32_t)sizeof(book)));

  /* Pack the blob into a real chunked container, chunk == cache frame. */
  static uint8_t container[k_pbc_file_cap];
  const uint64_t file_len =
    pbc_pack(container, (const uint8_t*)&book, (uint32_t)sizeof(book), (uint32_t)k_pb_frame_bytes);

  /* Open the chunk reader over the container "file". */
  pbc_file_t        file                       = {.data = container, .len = file_len};
  ra_book_chunked_t rd                         = {};
  static uint64_t   table_buf[k_pbc_table_cap] = {};
  static uint8_t    staging[k_pbc_staging_cap] = {};
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_book_chunked_open(&rd,
                                      pbc_file_read,
                                      &file,
                                      file_len,
                                      pbc_inflate,
                                      table_buf,
                                      k_pbc_table_cap,
                                      staging,
                                      k_pbc_staging_cap));
  TEST_ASSERT_EQ(sizeof(book), rd.inflated_total);

  /* Register the reader as the paged object's backing. */
  ra_vsource_obj_t objs[1];
  ra_vsource_t     vs = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_vsource_init(&vs, objs, 1U));
  uint32_t oid = 0U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_vsource_add_paged(&vs, ra_book_chunked_read, &rd, 0U, rd.inflated_total, &oid));

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
    ra_book_src_paged(&psrc, &vm, oid, (uint32_t)k_pb_frame_bytes, (uint32_t)rd.inflated_total));

  /* Both chapters: paged-over-chunked text must equal the resident walk. */
  for (uint32_t ch = 0U; ch < book.hdr.chapter_count; ++ch) {
    char   res[512] = {};
    char   pag[512] = {};
    size_t r_len    = 0U;
    size_t p_len    = 0U;
    TEST_ASSERT_EQ(k_ra_ok, ra_book_chapter_text_src(&rsrc, ch, res, sizeof res, &r_len));
    TEST_ASSERT_EQ(k_ra_ok, ra_book_chapter_text_src(&psrc, ch, pag, sizeof pag, &p_len));
    TEST_ASSERT(r_len > 0U);
    TEST_ASSERT_EQ(r_len, p_len);
    TEST_ASSERT_EQ(0, memcmp(res, pag, r_len));
  }

  /* The tiny cache really evicted while serving chunk-inflate faults. */
  uint32_t hits = 0U;
  uint32_t miss = 0U;
  uint32_t evic = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_vmem_stats(&vm, &hits, &miss, &evic));
  TEST_ASSERT(miss > 0U);
  TEST_ASSERT(evic > 0U);

  TEST_END("ra_book paged over chunked container == resident");
}

/**
 * @enum pbook_edge_dim_t
 * @brief Sizes for the walk-guard / emit-overflow / long-tag edge fixtures.
 */
typedef enum : uint32_t {
  k_pbe_str_cap   = 128U, /**< String-pool bytes in the edge fixtures.         */
  k_pbe_div_off   = 1U,   /**< Pool offset of the "div" tag (after the "").    */
  k_pbe_ovf_cap   = 2U,   /**< Output buffer that fills before the block '\n'. */
  k_pbe_tag_chars = 70U,  /**< Tag-name length > the 63-byte str_short chunk.  */
  k_pbe_walk_out  = 64U,  /**< Roomy output for the cyclic walk (no overflow). */
} pbook_edge_dim_t;

/** @brief Minimal single-node blob whose only node cycles onto itself. */
typedef struct {
  ra_book_header_t  hdr;         /**< Container header.              */
  ra_book_chapter_t chapters[1]; /**< One chapter rooted at node 0.  */
  ra_book_node_t    nodes[1];    /**< One self-cyclic <div> element. */
  char              strings[16]; /**< "" then "div".                 */
} pbook_cyclic_t;

/** @brief Stamp the trailer CRC over a fixture body so it is well-formed. */
static void pbook_stamp_crc(void* blob, uint32_t total)
{
  ra_book_header_t* h    = (ra_book_header_t*)blob;
  const uint8_t*    body = (const uint8_t*)blob + sizeof(ra_book_header_t);
  h->crc32               = pbook_crc32(body, total - (uint32_t)sizeof(ra_book_header_t));
}

/** @brief Fill the common header fields shared by the edge fixtures. */
static void pbook_edge_header(ra_book_header_t* h,
                              uint32_t          total,
                              uint32_t          chapter_off,
                              uint32_t          node_off,
                              uint32_t          node_count,
                              uint32_t          string_off,
                              uint32_t          string_size)
{
  memcpy(h->magic, "RABOOK1", 8);
  h->format_version    = k_ra_book_format_version;
  h->total_size        = total;
  h->chapter_count     = 1U;
  h->chapter_off       = chapter_off;
  h->node_count        = node_count;
  h->node_off          = node_off;
  h->string_off        = string_off;
  h->string_size       = string_size;
  h->image_pool_off    = string_off;
  h->image_pool_size   = 0U;
  h->cover_image_index = k_ra_book_nil;
}

/**
 * @test test_ra_book_paged_cyclic_walk_guard
 * @brief A blob whose node cycles onto itself is drained by the iteration guard
 *        (and the stack cap) rather than spinning forever.
 *
 * @par Coverage:
 * A single `<div>` whose first_child points back at itself makes the paged walk
 * re-push the same node every iteration; the accumulating nil-sibling frames grow
 * the walk stack to its cap while the iteration guard counts up, so the walk
 * stops on the guard instead of looping. The blob is otherwise well-formed
 * (valid header + trailer CRC); ra_book_validate does not check DOM acyclicity,
 * so this is a reachable public-API input.
 *
 * @par MC/DC:
 * One cyclic input drives three otherwise-unreachable compound legs:
 * - `if (ok && (*sp < k_ra_book_xhtml_stack))` in priv_paged_visit_node: the
 *   nil-sibling accumulation lifts *sp to k_ra_book_xhtml_stack, so the second
 *   condition is false (C1 true, C2 false) -- the stack-full leg.
 * - `while ((sp > 0U) && ok && (guard < max_iter))` in ra_book_walk_text_paged:
 *   the loop is still non-empty and ok when guard reaches max_iter, so the third
 *   condition is false (C1 true, C2 true, C3 false) -- the guard-exhaustion leg.
 * - `return ok && (guard < max_iter)`: ok is true but guard == max_iter, so the
 *   second condition is false (C1 true, C2 false).
 * The (all-true) control legs of all three decisions are supplied by every
 * acyclic walk in test_ra_book_paged_matches_resident; the ok-false legs by the
 * overflow tests. Together they complete the N+1 vector sets.
 */
static void test_ra_book_paged_cyclic_walk_guard(void)
{
  TEST_BEGIN("ra_book paged cyclic blob drained by the walk guard");

  static pbook_cyclic_t b;
  memset(&b, 0, sizeof(b));
  pbook_edge_header(&b.hdr,
                    (uint32_t)sizeof(b),
                    (uint32_t)offsetof(pbook_cyclic_t, chapters),
                    (uint32_t)offsetof(pbook_cyclic_t, nodes),
                    1U,
                    (uint32_t)offsetof(pbook_cyclic_t, strings),
                    (uint32_t)sizeof(b.strings));
  b.strings[0] = '\0';
  memcpy(&b.strings[k_pbe_div_off], "div", 4U);
  b.chapters[0].root_node = 0U;
  b.nodes[0]              = (ra_book_node_t){.kind         = k_ra_book_node_element,
                                             .name_off     = (uint32_t)k_pbe_div_off,
                                             .first_child  = 0U, /* self-cycle */
                                             .next_sibling = k_ra_book_nil};
  pbook_stamp_crc(&b, (uint32_t)sizeof(b));

  ra_book_src_t    psrc = {};
  ra_vsource_t     vs   = {};
  ra_vsource_obj_t objs[1];
  ra_vmem_t        vm = {};
  pbook_bind_paged(&psrc, &vs, objs, &vm, &b, (uint32_t)sizeof(b));

  char   out[k_pbe_walk_out] = {};
  size_t len                 = 0U;
  /* The guard stops the cyclic walk; the walker reports the non-clean run. */
  TEST_ASSERT_EQ(k_ra_err_invalid_size, ra_book_chapter_text_src(&psrc, 0U, out, sizeof out, &len));

  TEST_END("ra_book paged cyclic blob drained by the walk guard");
}

/** @brief span > (text, block-p) blob used to overflow emit_break on the block. */
typedef struct {
  ra_book_header_t  hdr;         /**< Container header.                      */
  ra_book_chapter_t chapters[1]; /**< One chapter rooted at the inline span. */
  ra_book_node_t    nodes[3];    /**< span, text, block <p>.                 */
  char              strings[32]; /**< "", "span", "p", "AB".                 */
} pbook_ovf_t;

/**
 * @test test_ra_book_paged_block_break_overflow
 * @brief A block break that cannot fit the output buffer makes the visit report
 *        failure through the emit-break leg.
 *
 * @par Coverage:
 * An inline `<span>` (no leading break) holds a two-byte text run then a block
 * `<p>`. With a two-byte output buffer the text exactly fills it, so the `<p>`
 * block break has no room for its newline and ra_book_emit_break returns false.
 *
 * @par MC/DC:
 * Drives the first-condition-false leg of `if (ok && (*sp < k_ra_book_xhtml_stack))`
 * in priv_paged_visit_node: emit_break fails on the block `<p>`, so ok is false
 * and the child is not pushed (C1 false, short-circuit). The (true, true) control
 * is supplied by the span visit here and every acyclic walk; the (true, false)
 * stack-full leg by test_ra_book_paged_cyclic_walk_guard. N+1 vectors complete.
 */
static void test_ra_book_paged_block_break_overflow(void)
{
  TEST_BEGIN("ra_book paged block break overflow reports failure");

  static pbook_ovf_t b;
  memset(&b, 0, sizeof(b));
  pbook_edge_header(&b.hdr,
                    (uint32_t)sizeof(b),
                    (uint32_t)offsetof(pbook_ovf_t, chapters),
                    (uint32_t)offsetof(pbook_ovf_t, nodes),
                    3U,
                    (uint32_t)offsetof(pbook_ovf_t, strings),
                    (uint32_t)sizeof(b.strings));
  uint32_t cur          = 0U;
  b.strings[cur++]      = '\0';
  const uint32_t s_span = cur;
  memcpy(&b.strings[cur], "span", 5U);
  cur += 5U;
  const uint32_t s_p = cur;
  memcpy(&b.strings[cur], "p", 2U);
  cur += 2U;
  const uint32_t s_ab = cur;
  memcpy(&b.strings[cur], "AB", 3U);
  cur += 3U;

  b.chapters[0].root_node = 0U;
  b.nodes[0]              = (ra_book_node_t){.kind         = k_ra_book_node_element,
                                             .name_off     = s_span,
                                             .first_child  = 1U,
                                             .next_sibling = k_ra_book_nil};
  b.nodes[1]              = (ra_book_node_t){.kind         = k_ra_book_node_text,
                                             .text_off     = s_ab,
                                             .first_child  = k_ra_book_nil,
                                             .next_sibling = 2U};
  b.nodes[2]              = (ra_book_node_t){.kind         = k_ra_book_node_element,
                                             .name_off     = s_p,
                                             .first_child  = k_ra_book_nil,
                                             .next_sibling = k_ra_book_nil};
  pbook_stamp_crc(&b, (uint32_t)sizeof(b));

  ra_book_src_t    psrc = {};
  ra_vsource_t     vs   = {};
  ra_vsource_obj_t objs[1];
  ra_vmem_t        vm = {};
  pbook_bind_paged(&psrc, &vs, objs, &vm, &b, (uint32_t)sizeof(b));

  char   out[k_pbe_ovf_cap] = {};
  size_t len                = 0U;
  /* "AB" fills the two-byte buffer; the <p> block break cannot fit its newline. */
  TEST_ASSERT_EQ(k_ra_err_invalid_size, ra_book_chapter_text_src(&psrc, 0U, out, sizeof out, &len));

  TEST_END("ra_book paged block break overflow reports failure");
}

/** @brief One-element blob whose tag name is longer than the str_short chunk. */
typedef struct {
  ra_book_header_t  hdr;                    /**< Container header.              */
  ra_book_chapter_t chapters[1];            /**< One chapter rooted at node 0.  */
  ra_book_node_t    nodes[2];               /**< long-tag element + text child. */
  char              strings[k_pbe_str_cap]; /**< "" then the 70-char tag name.  */
} pbook_longtag_t;

/**
 * @test test_ra_book_paged_long_tag_name
 * @brief A tag name longer than the paged tag chunk exercises the chunk-full leg
 *        of the short-string copy loop.
 *
 * @par Coverage:
 * The element's tag name is 70 bytes with no embedded NUL, so
 * ra_book_paged_str_short reads its full 63-byte chunk without hitting a
 * terminator and truncates -- treating the tag as inline (no break), which is the
 * documented degradation.
 *
 * @par MC/DC:
 * Drives the first-condition-false leg of `while ((nlen < chunk) && (buf[nlen] !=
 * '\0'))` in ra_book_paged_str_short: nlen reaches `chunk` (63) before any NUL,
 * so the first condition is false and the scan stops (C1 false, short-circuit).
 * The (true, true) scanning control and the (true, false) NUL-found leg are
 * supplied by the short tags (div/p/section/h1) of
 * test_ra_book_paged_matches_resident. N+1 vectors complete.
 */
static void test_ra_book_paged_long_tag_name(void)
{
  TEST_BEGIN("ra_book paged over-long tag name truncates");

  static pbook_longtag_t b;
  memset(&b, 0, sizeof(b));
  pbook_edge_header(&b.hdr,
                    (uint32_t)sizeof(b),
                    (uint32_t)offsetof(pbook_longtag_t, chapters),
                    (uint32_t)offsetof(pbook_longtag_t, nodes),
                    2U,
                    (uint32_t)offsetof(pbook_longtag_t, strings),
                    (uint32_t)sizeof(b.strings));
  uint32_t cur         = 0U;
  b.strings[cur++]     = '\0';
  const uint32_t s_tag = cur;
  for (uint32_t i = 0U; i < (uint32_t)k_pbe_tag_chars; ++i) {
    b.strings[cur++] = 'a';
  }
  b.strings[cur++]      = '\0';
  const uint32_t s_text = cur;
  memcpy(&b.strings[cur], "hi", 3U);
  cur += 3U;

  b.chapters[0].root_node = 0U;
  b.nodes[0]              = (ra_book_node_t){.kind         = k_ra_book_node_element,
                                             .name_off     = s_tag,
                                             .first_child  = 1U,
                                             .next_sibling = k_ra_book_nil};
  b.nodes[1]              = (ra_book_node_t){.kind         = k_ra_book_node_text,
                                             .text_off     = s_text,
                                             .first_child  = k_ra_book_nil,
                                             .next_sibling = k_ra_book_nil};
  pbook_stamp_crc(&b, (uint32_t)sizeof(b));

  ra_book_src_t    psrc = {};
  ra_vsource_t     vs   = {};
  ra_vsource_obj_t objs[1];
  ra_vmem_t        vm = {};
  pbook_bind_paged(&psrc, &vs, objs, &vm, &b, (uint32_t)sizeof(b));

  char   out[k_pbe_walk_out] = {};
  size_t len                 = 0U;
  /* The over-long tag is treated as inline, so the child text still emits. */
  TEST_ASSERT_EQ(k_ra_ok, ra_book_chapter_text_src(&psrc, 0U, out, sizeof out, &len));
  TEST_ASSERT(len >= 2U);

  TEST_END("ra_book paged over-long tag name truncates");
}

/**
 * @brief Fixture whose chapter table sits past the two header-load frames, so a
 *        faulting backing can make the prefetch's chapter-record read fault.
 * @details The 100-byte header binds through frames 0..1; a k_pb_frame_bytes gap
 *          pushes the chapter record into a cold frame that faults once the
 *          backing is armed -- reaching ra_book_src_prefetch_chapter's record-read
 *          error return without needing to walk any DOM.
 */
typedef struct {
  ra_book_header_t  hdr;                   /**< Container header (frames 0..1).      */
  uint8_t           gap[k_pb_frame_bytes]; /**< Pad chapters past the header frames. */
  ra_book_chapter_t chapters[1];           /**< One chapter rooted at node 0.        */
  ra_book_node_t    nodes[2];              /**< div element + text child.            */
  char              strings[16];           /**< "" then "div".                       */
} pbook_farchap_t;

/**
 * @test test_ra_book_paged_prefetch_record_fault
 * @brief A backing fault on the chapter-record read propagates out of
 *        ra_book_src_prefetch_chapter() verbatim.
 *
 * @par Coverage:
 * Reaches ra_book_src_prefetch_chapter's `if (ce != k_ra_ok) return ce;` arm: the
 * chapter table is placed in a cold frame (past the header-load frames) and the
 * backing is armed to fault every read beyond the header once bound, so the
 * prefetch's chapter-record read faults and the loader error is returned.
 *
 * @par MC/DC:
 * (no compound decisions under test -- a single error passthrough of the
 * chapter-record read; ra_book_src_prefetch_chapter has no compound decisions)
 */
static void test_ra_book_paged_prefetch_record_fault(void)
{
  TEST_BEGIN("ra_book prefetch propagates a chapter-record read fault");

  static pbook_farchap_t b;
  memset(&b, 0, sizeof(b));
  pbook_edge_header(&b.hdr,
                    (uint32_t)sizeof(b),
                    (uint32_t)offsetof(pbook_farchap_t, chapters),
                    (uint32_t)offsetof(pbook_farchap_t, nodes),
                    2U,
                    (uint32_t)offsetof(pbook_farchap_t, strings),
                    (uint32_t)sizeof(b.strings));
  b.strings[0] = '\0';
  memcpy(&b.strings[k_pbe_div_off], "div", 4U);
  b.chapters[0].root_node = 0U;
  b.nodes[0]              = (ra_book_node_t){.kind         = k_ra_book_node_element,
                                             .name_off     = (uint32_t)k_pbe_div_off,
                                             .first_child  = 1U,
                                             .next_sibling = k_ra_book_nil};
  b.nodes[1]              = (ra_book_node_t){.kind         = k_ra_book_node_text,
                                             .text_off     = 0U,
                                             .first_child  = k_ra_book_nil,
                                             .next_sibling = k_ra_book_nil};
  pbook_stamp_crc(&b, (uint32_t)sizeof(b));

  s_pbook_bytes       = (const uint8_t*)&b;
  s_pbook_len         = (uint32_t)sizeof(b);
  s_pbook_fault_armed = false;
  s_pbook_fault_off   = (uint32_t)k_pb_frame_bytes * 2U; /* frames 0,1 hold the header */

  ra_vsource_obj_t objs[1];
  ra_vsource_t     vs = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_vsource_init(&vs, objs, 1U));
  uint32_t oid = 0U;
  TEST_ASSERT_EQ(
    k_ra_ok,
    ra_vsource_add_paged(&vs, pbook_read_fault, nullptr, 0U, (uint64_t)sizeof(b), &oid));
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

  /* Header is bound; now every cold frame faults. The chapter record lives past
   * the header frames, so the prefetch's record read faults and returns it. */
  s_pbook_fault_armed = true;
  const ra_err_t e    = ra_book_src_prefetch_chapter(&psrc, 0U);
  TEST_ASSERT(e != k_ra_ok);
  s_pbook_fault_armed = false; /* disarm so the backing is reusable */

  TEST_END("ra_book prefetch propagates a chapter-record read fault");
}

/**
 * @test test_ra_book_paged_prefetch_warms
 * @brief ra_book_src_prefetch_chapter() warms a chapter's first content frame so
 *        the next real read of it is a cache HIT (the #207 read-ahead primitive).
 *
 * @par Coverage:
 * Proves the flush-idle read-ahead #207 wires into the reader loop: a cold read of
 * chapter 1's root-node frame is a cache MISS, whereas the same read after
 * ra_book_src_prefetch_chapter(&psrc, 1) is a HIT with no new miss -- the frame was
 * warmed and left resident. Chapter-text byte-identity is unchanged (the warm is
 * transparent), and all three argument guards are exercised.
 *
 * @par MC/DC:
 * (no compound decisions under test -- ra_book_src_prefetch_chapter's guards are
 * independent single-condition checks (null src, resident/unbound, chapter range),
 * and the warm/hit result is a cache-counter delta, not a boolean decision)
 */
static void test_ra_book_paged_prefetch_warms(void)
{
  TEST_BEGIN("ra_book prefetch warms adjacent chapter into the cache");

  static pbook_t book;
  pbook_setup(&book);
  TEST_ASSERT_EQ(k_ra_ok, ra_book_validate(&book, sizeof(book)));

  /* Chapter 1's first content read is its root DOM node (root_node == 5). Read a
   * short slice that stays inside the single frame the prefetch warms. */
  const uint32_t node1_off =
    book.hdr.node_off + (book.chapters[1].root_node * (uint32_t)sizeof(ra_book_node_t));
  const uint32_t in_frame = node1_off % (uint32_t)k_pb_frame_bytes;
  const uint32_t within   = (uint32_t)k_pb_frame_bytes - in_frame;
  const uint32_t rlen     = (within < 8U) ? within : 8U;
  uint8_t        probe[8] = {};

  /* Case A -- cold: the root-node frame is not resident, so the read misses. */
  {
    ra_book_src_t    psrc = {};
    ra_vsource_t     vs   = {};
    ra_vsource_obj_t objs[1];
    ra_vmem_t        vm = {};
    pbook_bind_paged(&psrc, &vs, objs, &vm, &book, (uint32_t)sizeof(book));

    uint32_t h0 = 0U;
    uint32_t m0 = 0U;
    TEST_ASSERT_EQ(k_ra_ok, ra_vmem_stats(&vm, &h0, &m0, nullptr));
    TEST_ASSERT_EQ(k_ra_ok, ra_book_src_read(&psrc, node1_off, probe, rlen));
    uint32_t h1 = 0U;
    uint32_t m1 = 0U;
    TEST_ASSERT_EQ(k_ra_ok, ra_vmem_stats(&vm, &h1, &m1, nullptr));
    TEST_ASSERT(m1 > m0); /* cold: the read faulted the frame in */
  }

  /* Case B -- prefetched: warm chapter 1, then the same read is a HIT. */
  {
    ra_book_src_t    psrc = {};
    ra_vsource_t     vs   = {};
    ra_vsource_obj_t objs[1];
    ra_vmem_t        vm = {};
    pbook_bind_paged(&psrc, &vs, objs, &vm, &book, (uint32_t)sizeof(book));

    TEST_ASSERT_EQ(k_ra_ok, ra_book_src_prefetch_chapter(&psrc, 1U)); /* warm N+1 */

    uint32_t h0 = 0U;
    uint32_t m0 = 0U;
    TEST_ASSERT_EQ(k_ra_ok, ra_vmem_stats(&vm, &h0, &m0, nullptr));
    TEST_ASSERT_EQ(k_ra_ok, ra_book_src_read(&psrc, node1_off, probe, rlen));
    uint32_t h1 = 0U;
    uint32_t m1 = 0U;
    TEST_ASSERT_EQ(k_ra_ok, ra_vmem_stats(&vm, &h1, &m1, nullptr));
    TEST_ASSERT(h1 > h0);   /* warm: the read hit the prefetched frame */
    TEST_ASSERT_EQ(m0, m1); /* no new miss -- it was already resident      */

    /* Transparency: the warm changes only residency, never the bytes read back. */
    ra_book_src_t rsrc = {};
    TEST_ASSERT_EQ(k_ra_ok, ra_book_src_resident(&rsrc, &book, (uint32_t)sizeof(book)));
    char   res[512] = {};
    char   pag[512] = {};
    size_t r_len    = 0U;
    size_t p_len    = 0U;
    TEST_ASSERT_EQ(k_ra_ok, ra_book_chapter_text_src(&rsrc, 1U, res, sizeof res, &r_len));
    TEST_ASSERT_EQ(k_ra_ok, ra_book_chapter_text_src(&psrc, 1U, pag, sizeof pag, &p_len));
    TEST_ASSERT(r_len > 0U);
    TEST_ASSERT_EQ(r_len, p_len);
    TEST_ASSERT_EQ(0, memcmp(res, pag, r_len));

    /* Argument guards: null src, resident (no cache) src, out-of-range chapter. */
    TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_book_src_prefetch_chapter(nullptr, 0U));
    TEST_ASSERT_EQ(k_ra_err_invalid_state, ra_book_src_prefetch_chapter(&rsrc, 0U));
    TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                   ra_book_src_prefetch_chapter(&psrc, book.hdr.chapter_count));
  }

  TEST_END("ra_book prefetch warms adjacent chapter into the cache");
}

int32_t main(void)
{
  test_ra_book_paged_matches_resident();
  test_ra_book_paged_guards();
  test_ra_book_paged_long_run();
  test_ra_book_paged_read_faults();
  test_ra_book_paged_chunked_backing();
  test_ra_book_paged_cyclic_walk_guard();
  test_ra_book_paged_block_break_overflow();
  test_ra_book_paged_long_tag_name();
  test_ra_book_paged_prefetch_record_fault();
  test_ra_book_paged_prefetch_warms();
  (void)fprintf(stderr, "[OK ] test_ra_book_paged.c\n");
  return 0;
}
