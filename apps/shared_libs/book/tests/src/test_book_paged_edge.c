/**
 * @file test_book_paged_edge.c
 * @brief Chunked-backing and hostile-blob tests for the paged book mode.
 *
 * @details
 * Split out of test_book_paged.c to keep each test translation unit
 * under the repository file-size cap. This sibling owns the
 * chunked-container backing test (the full production stack: paged accessor
 * over ra8_vmem over a chunked container inflated with miniz) plus the
 * hand-built hostile-blob edge tests (cyclic DOM walk guard, block-break
 * overflow, over-long tag names, prefetch fault / warm paths); the
 * resident-vs-paged equivalence and guard tests stay in
 * test_book_paged.c. The in-memory .rabook fixture builder is
 * duplicated per sibling.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "book.h"
#include "book_chunked.h"
#include "book_paged.h"
#include "miniz.h"
#include "ra8_err.h"
#include "ra8_vmem.h"
#include "ra8_vsource.h"
#include "unity_minimal.h"

/** @brief Copy an object representation through compatible byte-pointer types. */
static void internal_copy_object(void* dst, const void* src, size_t len)
{
  (void)memcpy(dst, src, len);
}

/** @brief Compare two object representations through compatible pointer types. */
static bool internal_objects_equal(const void* lhs, const void* rhs, size_t len)
{
  return memcmp(lhs, rhs, len) == 0;
}

/** @brief RBKC container header layout used by the fixture builder. */
typedef enum : uint8_t {
  k_pbook_off_reserved = 20U, /**< Reserved u32 field offset in the header. */
} pbook_layout_t;

/**
 * @enum pbook_fixture_t
 * @brief The fixture book's node indices, pool capacities and CRC constants.
 *
 * @details
 * Each node index names the node it addresses; the same index serves as both
 * the array slot and the parent's first_child link, so the two cannot drift.
 */
typedef enum : uint32_t {
  k_pbook_node_ch2_section   = 5U,          /**< <section> that chapter 2 roots at. */
  k_pbook_node_ch2_h1_text   = 7U,          /**< Text under that section's <h1>.    */
  k_pbook_node_ch2_para_text = 9U,          /**< Text under its <p>.                */
  k_pbook_node_count         = 10U,         /**< Nodes in the book: the nodes[]
                                         extent and the header's node_count
                                         are the same fact.                 */
  k_pbook_strings_cap        = 320U,        /**< String-pool capacity.          */
  k_pbook_render_cap         = 512U,        /**< Rendered-text compare buffers. */
  k_crc32_init               = 0xFFFFFFFFU, /**< CRC-32 init, and the XOR-out.  */
  k_crc32_poly_reflected     = 0xEDB88320U, /**< Reflected CRC-32 polynomial.   */
} pbook_fixture_t;

/** @brief CRC-32/ISO-HDLC over the blob body (matches book_validate). */
static uint32_t pbook_crc32(const uint8_t* data, size_t len)
{
  uint32_t crc = k_crc32_init;
  for (size_t i = 0U; i < len; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0U; bit < 8U; ++bit) {
      const uint32_t mask = ((crc & 1U) != 0U) ? UINT32_MAX : 0U;
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
  book_header_t     hdr;                          /**< Hdr.         */
  book_chapter_t    chapters[2];                  /**< Chapters.    */
  book_node_t       nodes[k_pbook_node_count];    /**< Nodes.       */
  book_attr_t       attrs[1];                     /**< Attrs.       */
  book_stylesheet_t stylesheets[1];               /**< Stylesheets. */
  book_image_t      images[1];                    /**< Images.      */
  char              strings[k_pbook_strings_cap]; /**< Strings.     */
} pbook_t;

/** @brief Intern a NUL-terminated string into the pool; returns its offset. */
static uint32_t pbook_intern(pbook_t* b, uint32_t* cur, const char* s)
{
  const uint32_t off = *cur;
  const size_t   n   = strlen(s) + 1U;
  (void)memcpy(&b->strings[off], s, n);
  *cur += (uint32_t)n;
  return off;
}

/** @brief Fill the fixture header: magic, section counts, and offsets. */
static void pbook_fill_header(pbook_t* b)
{
  (void)memset(b, 0, sizeof(*b));
  (void)memcpy(b->hdr.magic, "RABOOK1", 8);
  b->hdr.format_version = k_book_format_version;
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
static book_node_t pbook_elem(uint32_t name_off, uint32_t first_child, uint32_t next_sibling)
{
  return (book_node_t){.kind         = k_book_node_element,
                       .name_off     = name_off,
                       .first_child  = first_child,
                       .next_sibling = next_sibling};
}

/** @brief Compose one text node row for the fixture DOM. */
static book_node_t pbook_text(uint32_t text_off)
{
  return (book_node_t){.kind         = k_book_node_text,
                       .text_off     = text_off,
                       .first_child  = k_book_nil,
                       .next_sibling = k_book_nil};
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
  b->nodes[0] = pbook_elem(s_div, 1U, k_book_nil);
  b->nodes[1] = pbook_elem(s_p, 2U, 3U);
  b->nodes[2] = pbook_text(s_t0);
  b->nodes[3] = pbook_elem(s_p, 4U, k_book_nil);
  b->nodes[4] = pbook_text(s_t1);

  /* Chapter 1: section > (h1 > text), (p > text). */
  b->nodes[k_pbook_node_ch2_section]   = pbook_elem(s_section, 6U, k_book_nil);
  b->nodes[6]                          = pbook_elem(s_h1, k_pbook_node_ch2_h1_text, 8U);
  b->nodes[k_pbook_node_ch2_h1_text]   = pbook_text(s_t2);
  b->nodes[8]                          = pbook_elem(s_p, k_pbook_node_ch2_para_text, k_book_nil);
  b->nodes[k_pbook_node_ch2_para_text] = pbook_text(s_t3);

  b->attrs[0]              = (book_attr_t){.name_off = s_class, .value_off = s_main};
  b->stylesheets[0]        = (book_stylesheet_t){.source_off = 0U, .scope_chapter = k_book_nil};
  b->images[0]             = (book_image_t){.id_off = 0U, .format = k_book_image_gray4};
  b->hdr.cover_image_index = k_book_nil;

  const uint8_t* body     = &((const uint8_t*)b)[sizeof(book_header_t)];
  const uint32_t body_len = (uint32_t)(sizeof(*b) - sizeof(book_header_t));
  b->hdr.crc32_val        = pbook_crc32(body, body_len);
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
  (void)memcpy(buf, &s_pbook_bytes[(size_t)offset], len);
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
  (void)memcpy(buf, &s_pbook_bytes[(size_t)offset], len);
  return k_ra8_ok;
}

/* Tiny page cache: 8 frames x 64 bytes < the ~520-byte blob, so the walk evicts. */
typedef enum : uint32_t {
  k_pb_frame_bytes = 64U, /**< Pb frame bytes. */
  k_pb_frame_count = 8U,  /**< Pb frame count. */
  k_pb_buckets     = 16U, /**< Pb buckets.     */
} pbook_cache_dim_t;

/** @brief Build the tiny paged-book vmem config over the shared static fixtures. */
static ra8_vmem_cfg_t pbe_vmem_cfg(void* loader_ctx)
{
  static uint8_t          s_pb_frames[(size_t)k_pb_frame_count * (size_t)k_pb_frame_bytes];
  static ra8_vmem_frame_t s_pb_meta[k_pb_frame_count];
  static ra8_vmem_key_t   s_pb_keys[k_pb_frame_count];
  static int32_t          s_pb_buckets[k_pb_buckets];
  return (ra8_vmem_cfg_t){
    .frame_mem    = s_pb_frames,
    .frame_bytes  = (uint32_t)k_pb_frame_bytes,
    .frame_count  = (uint32_t)k_pb_frame_count,
    .meta         = s_pb_meta,
    .keys         = s_pb_keys,
    .buckets      = s_pb_buckets,
    .bucket_count = (uint32_t)k_pb_buckets,
    .loader       = ra8_vsource_loader,
    .loader_ctx   = loader_ctx,
  };
}

/**
 * @brief Bind a paged ::book_src_t over @p bytes using the shared tiny cache.
 * @details Points the ::pbook_read backing at @p bytes / @p len, wires a single
 *          paged ::ra8_vsource object through an ::ra8_vmem cache, and binds
 *          @p psrc. Caller owns @p vs / @p objs / @p vm for the source's lifetime.
 * @param[out] psrc Receives the bound paged source.
 * @param[out] vs   Caller-owned vsource registry (initialised here).
 * @param[out] objs Caller-owned one-slot object array for @p vs.
 * @param[out] vm   Caller-owned vmem cache (initialised here).
 * @param[in]  bytes Blob the paged backing serves.
 * @param[in]  len   Length of @p bytes in bytes.
 * @pre All pointers are non-NULL; @p len > 0 and >= sizeof(book_header_t).
 * @pre The shared cache statics are not concurrently bound elsewhere.
 * @post @p psrc is a paged source whose header slice is already cached.
 * @post ::s_pbook_bytes / ::s_pbook_len alias @p bytes / @p len.
 * @note Not thread-safe (mutates the shared backing + cache statics).
 */
static void pbook_bind_paged(book_src_t*        psrc,
                             ra8_vsource_t*     vs,
                             ra8_vsource_obj_t* objs,
                             ra8_vmem_t*        vm,
                             const void*        bytes,
                             uint32_t           len)
{
  s_pbook_bytes = (const uint8_t*)bytes;
  s_pbook_len   = len;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vsource_init(vs, objs, 1U));
  uint32_t oid = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vsource_add_paged(vs, pbook_read, nullptr, 0U, (uint64_t)len, &oid));
  ra8_vmem_cfg_t cfg = pbe_vmem_cfg(vs);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_init(vm, &cfg));
  TEST_ASSERT_EQ(k_ra8_ok, book_src_paged(psrc, vm, oid, (uint32_t)k_pb_frame_bytes, len));
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

/** @brief zlib inflater matching book_inflate_fn (mirrors the shelf inflater). */
static ra8_err_t
pbc_inflate(const void* src, size_t src_len, void* dst, size_t dst_cap, size_t* out_len)
{
  static tinfl_decompressor s_pbc_tinfl;
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
    return k_ra8_err_invalid_size;
  }
  *out_len = out_n;
  return k_ra8_ok;
}

/** @brief In-memory container "file" for the chunked reader's byte source. */
typedef struct {
  const uint8_t* data; /**< Container bytes.  */
  uint64_t       len;  /**< Container length. */
} pbc_file_t;

/** @brief ra8_vsource_read_fn over the in-memory container file. */
static ra8_err_t pbc_file_read(void* ctx, uint64_t offset, uint8_t* buf, uint32_t len)
{
  const pbc_file_t* f = (const pbc_file_t*)ctx;
  if ((offset + (uint64_t)len) > f->len) {
    return k_ra8_err_out_of_range;
  }
  (void)memcpy(buf, &f->data[offset], len);
  return k_ra8_ok;
}

/**
 * @brief Pack an RBKC container over @p blob with real zlib chunk streams.
 * @details Mirrors tools/epub_compile's wrap_container: one zlib stream per
 *          `chunk_bytes` slice (last short), header + offset table + streams.
 * @return Packed container length in bytes.
 */
static uint64_t pbc_pack(uint8_t* out, const uint8_t* blob, uint32_t blob_len, uint32_t chunk_bytes)
{
  /** @brief RBKC chunked-container magic (raw bytes; no string terminator). */
  static const uint8_t k_pbook_magic_rbkc[] = {'R', 'B', 'K', 'C'};
  const uint32_t       count                = (blob_len + chunk_bytes - 1U) / chunk_bytes;
  size_t   pos = k_book_container_header_len + ((size_t)(count + 1U) * k_book_container_entry_len);
  uint64_t off = 0U;

  (void)memcpy(&out[0], k_pbook_magic_rbkc, sizeof(k_pbook_magic_rbkc));
  internal_copy_object(&out[4], &chunk_bytes, sizeof(chunk_bytes));
  const uint64_t total64 = blob_len;
  internal_copy_object(&out[8], &total64, sizeof(total64));
  internal_copy_object(&out[16], &count, sizeof(count));
  const uint32_t reserved = 0U;
  internal_copy_object(&out[k_pbook_off_reserved], &reserved, sizeof(reserved));

  for (uint32_t i = 0U; i < count; ++i) {
    internal_copy_object(
      &out[k_book_container_header_len + ((size_t)i * k_book_container_entry_len)],
      &off,
      sizeof(off));
    uint32_t span = blob_len - (i * chunk_bytes);
    if (span > chunk_bytes) {
      span = chunk_bytes;
    }
    mz_ulong  slen = (mz_ulong)k_pbc_staging_cap;
    const int rc   = mz_compress2(&out[pos + (size_t)off],
                                  &slen,
                                  &blob[(size_t)i * chunk_bytes],
                                  (mz_ulong)span,
                                  MZ_BEST_COMPRESSION);
    TEST_ASSERT_EQ(MZ_OK, rc);
    off += (uint64_t)slen;
  }
  internal_copy_object(
    &out[k_book_container_header_len + ((size_t)count * k_book_container_entry_len)],
    &off,
    sizeof(off));
  return (uint64_t)pos + off;
}

/** @brief Open the chunk reader over @p file and bind a paged book source. */
static void pbc_bind_chunked(book_src_t*        psrc,
                             ra8_vsource_t*     vs,
                             ra8_vsource_obj_t* objs,
                             ra8_vmem_t*        vm,
                             book_chunked_t*    rd,
                             pbc_file_t*        file)
{
  static uint64_t s_pbc_table_buf[k_pbc_table_cap];
  static uint8_t  s_pbc_staging[k_pbc_staging_cap];
  TEST_ASSERT_EQ(k_ra8_ok,
                 book_chunked_open(rd,
                                   pbc_file_read,
                                   file,
                                   file->len,
                                   pbc_inflate,
                                   s_pbc_table_buf,
                                   k_pbc_table_cap,
                                   s_pbc_staging,
                                   k_pbc_staging_cap));

  /* Register the reader as the paged object's backing. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vsource_init(vs, objs, 1U));
  uint32_t oid = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_vsource_add_paged(vs, book_chunked_read, rd, 0U, rd->inflated_total, &oid));

  ra8_vmem_cfg_t cfg = pbe_vmem_cfg(vs);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_init(vm, &cfg));

  book_src_t bound = {};
  TEST_ASSERT_EQ(
    k_ra8_ok,
    book_src_paged(&bound, vm, oid, (uint32_t)k_pb_frame_bytes, (uint32_t)rd->inflated_total));
  *psrc = bound;
}

/**
 * @test test_book_paged_chunked_backing
 * @brief The full production stack -- RBKC container -> book_chunked_read ->
 *        ra8_vsource -> ra8_vmem -> book_src_paged -- matches resident text.
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
 * test_mcdc_container_header_fields in test_book_cov.c)
 */
static void test_book_paged_chunked_backing(void)
{
  TEST_BEGIN("book paged over chunked container == resident");

  static pbook_t s_book;
  pbook_setup(&s_book);
  TEST_ASSERT_EQ(k_ra8_ok, book_validate(&s_book, sizeof(s_book)));

  book_src_t rsrc = {};
  TEST_ASSERT_EQ(k_ra8_ok, book_src_resident(&rsrc, &s_book, (uint32_t)sizeof(s_book)));

  /* Pack the blob into a real chunked container, chunk == cache frame. */
  static uint8_t s_container[k_pbc_file_cap];
  const uint64_t file_len = pbc_pack(s_container,
                                     (const uint8_t*)&s_book,
                                     (uint32_t)sizeof(s_book),
                                     (uint32_t)k_pb_frame_bytes);

  /* Open the chunk reader over the container "file" + bind the paged source. */
  pbc_file_t        file = {.data = s_container, .len = file_len};
  book_chunked_t    rd   = {};
  ra8_vsource_obj_t objs[1];
  ra8_vsource_t     vs   = {};
  ra8_vmem_t        vm   = {};
  book_src_t        psrc = {};
  pbc_bind_chunked(&psrc, &vs, objs, &vm, &rd, &file);
  TEST_ASSERT_EQ(sizeof(s_book), rd.inflated_total);

  /* Both chapters: paged-over-chunked text must equal the resident walk. */
  for (uint32_t ch = 0U; ch < s_book.hdr.chapter_count; ++ch) {
    char   res[k_pbook_render_cap] = {};
    char   pag[k_pbook_render_cap] = {};
    size_t r_len                   = 0U;
    size_t p_len                   = 0U;
    TEST_ASSERT_EQ(k_ra8_ok, book_chapter_text_src(&rsrc, ch, res, sizeof res, &r_len));
    TEST_ASSERT_EQ(k_ra8_ok, book_chapter_text_src(&psrc, ch, pag, sizeof pag, &p_len));
    TEST_ASSERT(r_len > 0U);
    TEST_ASSERT_EQ(r_len, p_len);
    TEST_ASSERT(internal_objects_equal(res, pag, r_len));
  }

  /* The tiny cache really evicted while serving chunk-inflate faults. */
  uint32_t hits = 0U;
  uint32_t miss = 0U;
  uint32_t evic = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_stats(&vm, &hits, &miss, &evic));
  TEST_ASSERT(miss > 0U);
  TEST_ASSERT(evic > 0U);

  TEST_END("book paged over chunked container == resident");
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
  book_header_t  hdr;         /**< Container header.              */
  book_chapter_t chapters[1]; /**< One chapter rooted at node 0.  */
  book_node_t    nodes[1];    /**< One self-cyclic <div> element. */
  char           strings[16]; /**< "" then "div".                 */
} pbook_cyclic_t;

/** @brief Stamp the trailer CRC over a fixture body so it is well-formed. */
static void pbook_stamp_crc(void* blob, uint32_t total)
{
  book_header_t* h    = (book_header_t*)blob;
  const uint8_t* body = &((const uint8_t*)blob)[sizeof(book_header_t)];
  h->crc32_val        = pbook_crc32(body, total - (uint32_t)sizeof(book_header_t));
}

/** @brief Fill the common header fields shared by the edge fixtures. */
static void pbook_edge_header(book_header_t* h,
                              uint32_t       total,
                              uint32_t       chapter_off,
                              uint32_t       node_off,
                              uint32_t       node_count,
                              uint32_t       string_off,
                              uint32_t       string_size)
{
  (void)memcpy(h->magic, "RABOOK1", 8);
  h->format_version    = k_book_format_version;
  h->total_size        = total;
  h->chapter_count     = 1U;
  h->chapter_off       = chapter_off;
  h->node_count        = node_count;
  h->node_off          = node_off;
  h->string_off        = string_off;
  h->string_size       = string_size;
  h->image_pool_off    = string_off;
  h->image_pool_size   = 0U;
  h->cover_image_index = k_book_nil;
}

/**
 * @test test_book_paged_cyclic_walk_guard
 * @brief A blob whose node cycles onto itself is drained by the iteration guard
 *        (and the stack cap) rather than spinning forever.
 *
 * @par Coverage:
 * A single `<div>` whose first_child points back at itself makes the paged walk
 * re-push the same node every iteration; the accumulating nil-sibling frames grow
 * the walk stack to its cap while the iteration guard counts up, so the walk
 * stops on the guard instead of looping. The blob is otherwise well-formed
 * (valid header + trailer CRC); book_validate does not check DOM acyclicity,
 * so this is a reachable public-API input.
 *
 * @par MC/DC:
 * One cyclic input drives three otherwise-unreachable compound legs:
 * - `if (ok && (*sp < k_book_xhtml_stack))` in internal_paged_visit_node: the
 *   nil-sibling accumulation lifts *sp to k_book_xhtml_stack, so the second
 *   condition is false (C1 true, C2 false) -- the stack-full leg.
 * - `while ((sp > 0U) && ok && (guard < max_iter))` in internal_walk_text_paged:
 *   the loop is still non-empty and ok when guard reaches max_iter, so the third
 *   condition is false (C1 true, C2 true, C3 false) -- the guard-exhaustion leg.
 * - `return ok && (guard < max_iter)`: ok is true but guard == max_iter, so the
 *   second condition is false (C1 true, C2 false).
 * The (all-true) control legs of all three decisions are supplied by every
 * acyclic walk in test_book_paged_matches_resident; the ok-false legs by the
 * overflow tests. Together they complete the N+1 vector sets.
 */
static void test_book_paged_cyclic_walk_guard(void)
{
  TEST_BEGIN("book paged cyclic blob drained by the walk guard");

  static pbook_cyclic_t s_b;
  (void)memset(&s_b, 0, sizeof(s_b));
  pbook_edge_header(&s_b.hdr,
                    (uint32_t)sizeof(s_b),
                    (uint32_t)offsetof(pbook_cyclic_t, chapters),
                    (uint32_t)offsetof(pbook_cyclic_t, nodes),
                    1U,
                    (uint32_t)offsetof(pbook_cyclic_t, strings),
                    (uint32_t)sizeof(s_b.strings));
  s_b.strings[0] = '\0';
  (void)memcpy(&s_b.strings[k_pbe_div_off], "div", 4U);
  s_b.chapters[0].root_node = 0U;
  s_b.nodes[0]              = (book_node_t){.kind         = k_book_node_element,
                                            .name_off     = (uint32_t)k_pbe_div_off,
                                            .first_child  = 0U, /* self-cycle */
                                            .next_sibling = k_book_nil};
  pbook_stamp_crc(&s_b, (uint32_t)sizeof(s_b));

  book_src_t        psrc = {};
  ra8_vsource_t     vs   = {};
  ra8_vsource_obj_t objs[1];
  ra8_vmem_t        vm = {};
  pbook_bind_paged(&psrc, &vs, objs, &vm, &s_b, (uint32_t)sizeof(s_b));

  char   out[k_pbe_walk_out] = {};
  size_t len                 = 0U;
  /* The guard stops the cyclic walk; the walker reports the non-clean run. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, book_chapter_text_src(&psrc, 0U, out, sizeof out, &len));

  TEST_END("book paged cyclic blob drained by the walk guard");
}

/** @brief span > (text, block-p) blob used to overflow emit_break on the block. */
typedef struct {
  book_header_t  hdr;         /**< Container header.                      */
  book_chapter_t chapters[1]; /**< One chapter rooted at the inline span. */
  book_node_t    nodes[3];    /**< span, text, block <p>.                 */
  char           strings[32]; /**< "", "span", "p", "AB".                 */
} pbook_ovf_t;

/**
 * @test test_book_paged_block_break_overflow
 * @brief A block break that cannot fit the output buffer makes the visit report
 *        failure through the emit-break leg.
 *
 * @par Coverage:
 * An inline `<span>` (no leading break) holds a two-byte text run then a block
 * `<p>`. With a two-byte output buffer the text exactly fills it, so the `<p>`
 * block break has no room for its newline and priv_book_emit_break returns false.
 *
 * @par MC/DC:
 * Drives the first-condition-false leg of `if (ok && (*sp < k_book_xhtml_stack))`
 * in internal_paged_visit_node: emit_break fails on the block `<p>`, so ok is false
 * and the child is not pushed (C1 false, short-circuit). The (true, true) control
 * is supplied by the span visit here and every acyclic walk; the (true, false)
 * stack-full leg by test_book_paged_cyclic_walk_guard. N+1 vectors complete.
 */
static void test_book_paged_block_break_overflow(void)
{
  TEST_BEGIN("book paged block break overflow reports failure");

  static pbook_ovf_t s_b;
  (void)memset(&s_b, 0, sizeof(s_b));
  pbook_edge_header(&s_b.hdr,
                    (uint32_t)sizeof(s_b),
                    (uint32_t)offsetof(pbook_ovf_t, chapters),
                    (uint32_t)offsetof(pbook_ovf_t, nodes),
                    3U,
                    (uint32_t)offsetof(pbook_ovf_t, strings),
                    (uint32_t)sizeof(s_b.strings));
  uint32_t cur     = 0U;
  s_b.strings[cur] = '\0';
  cur += 1U;
  const uint32_t s_span = cur;
  (void)memcpy(&s_b.strings[cur], "span", sizeof("span"));
  cur += k_pbook_node_ch2_section;
  const uint32_t s_p = cur;
  (void)memcpy(&s_b.strings[cur], "p", 2U);
  cur += 2U;
  const uint32_t s_ab = cur;
  (void)memcpy(&s_b.strings[cur], "AB", 3U);
  cur += 3U;

  /* The fixture pool must still have room; this also consumes the final
   * cursor value so the running offset is not a dead store. */
  TEST_ASSERT(cur <= (uint32_t)sizeof(s_b.strings));

  s_b.chapters[0].root_node = 0U;
  s_b.nodes[0]              = (book_node_t){.kind         = k_book_node_element,
                                            .name_off     = s_span,
                                            .first_child  = 1U,
                                            .next_sibling = k_book_nil};
  s_b.nodes[1]              = (book_node_t){.kind         = k_book_node_text,
                                            .text_off     = s_ab,
                                            .first_child  = k_book_nil,
                                            .next_sibling = 2U};
  s_b.nodes[2]              = (book_node_t){.kind         = k_book_node_element,
                                            .name_off     = s_p,
                                            .first_child  = k_book_nil,
                                            .next_sibling = k_book_nil};
  pbook_stamp_crc(&s_b, (uint32_t)sizeof(s_b));

  book_src_t        psrc = {};
  ra8_vsource_t     vs   = {};
  ra8_vsource_obj_t objs[1];
  ra8_vmem_t        vm = {};
  pbook_bind_paged(&psrc, &vs, objs, &vm, &s_b, (uint32_t)sizeof(s_b));

  char   out[k_pbe_ovf_cap] = {};
  size_t len                = 0U;
  /* "AB" fills the two-byte buffer; the <p> block break cannot fit its newline. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, book_chapter_text_src(&psrc, 0U, out, sizeof out, &len));

  TEST_END("book paged block break overflow reports failure");
}

/** @brief One-element blob whose tag name is longer than the str_short chunk. */
typedef struct {
  book_header_t  hdr;                    /**< Container header.              */
  book_chapter_t chapters[1];            /**< One chapter rooted at node 0.  */
  book_node_t    nodes[2];               /**< long-tag element + text child. */
  char           strings[k_pbe_str_cap]; /**< "" then the 70-char tag name.  */
} pbook_longtag_t;

/**
 * @test test_book_paged_long_tag_name
 * @brief A tag name longer than the paged tag chunk exercises the chunk-full leg
 *        of the short-string copy loop.
 *
 * @par Coverage:
 * The element's tag name is 70 bytes with no embedded NUL, so
 * internal_paged_str_short reads its full 63-byte chunk without hitting a
 * terminator and truncates -- treating the tag as inline (no break), which is the
 * documented degradation.
 *
 * @par MC/DC:
 * Drives the first-condition-false leg of `while ((nlen < chunk) && (buf[nlen] !=
 * '\0'))` in internal_paged_str_short: nlen reaches `chunk` (63) before any NUL,
 * so the first condition is false and the scan stops (C1 false, short-circuit).
 * The (true, true) scanning control and the (true, false) NUL-found leg are
 * supplied by the short tags (div/p/section/h1) of
 * test_book_paged_matches_resident. N+1 vectors complete.
 */
static void test_book_paged_long_tag_name(void)
{
  TEST_BEGIN("book paged over-long tag name truncates");

  static pbook_longtag_t s_b;
  (void)memset(&s_b, 0, sizeof(s_b));
  pbook_edge_header(&s_b.hdr,
                    (uint32_t)sizeof(s_b),
                    (uint32_t)offsetof(pbook_longtag_t, chapters),
                    (uint32_t)offsetof(pbook_longtag_t, nodes),
                    2U,
                    (uint32_t)offsetof(pbook_longtag_t, strings),
                    (uint32_t)sizeof(s_b.strings));
  uint32_t cur     = 0U;
  s_b.strings[cur] = '\0';
  cur += 1U;
  const uint32_t s_tag = cur;
  for (uint32_t i = 0U; i < (uint32_t)k_pbe_tag_chars; ++i) {
    s_b.strings[cur] = 'a';
    cur += 1U;
  }
  s_b.strings[cur] = '\0';
  cur += 1U;
  const uint32_t s_text = cur;
  (void)memcpy(&s_b.strings[cur], "hi", 3U);
  cur += 3U;

  /* The fixture pool must still have room; this also consumes the final
   * cursor value so the running offset is not a dead store. */
  TEST_ASSERT(cur <= (uint32_t)sizeof(s_b.strings));

  s_b.chapters[0].root_node = 0U;
  s_b.nodes[0]              = (book_node_t){.kind         = k_book_node_element,
                                            .name_off     = s_tag,
                                            .first_child  = 1U,
                                            .next_sibling = k_book_nil};
  s_b.nodes[1]              = (book_node_t){.kind         = k_book_node_text,
                                            .text_off     = s_text,
                                            .first_child  = k_book_nil,
                                            .next_sibling = k_book_nil};
  pbook_stamp_crc(&s_b, (uint32_t)sizeof(s_b));

  book_src_t        psrc = {};
  ra8_vsource_t     vs   = {};
  ra8_vsource_obj_t objs[1];
  ra8_vmem_t        vm = {};
  pbook_bind_paged(&psrc, &vs, objs, &vm, &s_b, (uint32_t)sizeof(s_b));

  char   out[k_pbe_walk_out] = {};
  size_t len                 = 0U;
  /* The over-long tag is treated as inline, so the child text still emits. */
  TEST_ASSERT_EQ(k_ra8_ok, book_chapter_text_src(&psrc, 0U, out, sizeof out, &len));
  TEST_ASSERT(len >= 2U);

  TEST_END("book paged over-long tag name truncates");
}

/**
 * @brief Fixture whose chapter table sits past the two header-load frames, so a
 *        faulting backing can make the prefetch's chapter-record read fault.
 * @details The 100-byte header binds through frames 0..1; a k_pb_frame_bytes gap
 *          pushes the chapter record into a cold frame that faults once the
 *          backing is armed -- reaching book_src_prefetch_chapter's record-read
 *          error return without needing to walk any DOM.
 */
typedef struct {
  book_header_t  hdr;                   /**< Container header (frames 0..1).      */
  uint8_t        gap[k_pb_frame_bytes]; /**< Pad chapters past the header frames. */
  book_chapter_t chapters[1];           /**< One chapter rooted at node 0.        */
  book_node_t    nodes[2];              /**< div element + text child.            */
  char           strings[16];           /**< "" then "div".                       */
} pbook_farchap_t;

/** @brief Populate the far-chapter fixture used by the prefetch fault test. */
static void pbe_farchap_setup(pbook_farchap_t* b)
{
  (void)memset(b, 0, sizeof(*b));
  pbook_edge_header(&b->hdr,
                    (uint32_t)sizeof(*b),
                    (uint32_t)offsetof(pbook_farchap_t, chapters),
                    (uint32_t)offsetof(pbook_farchap_t, nodes),
                    2U,
                    (uint32_t)offsetof(pbook_farchap_t, strings),
                    (uint32_t)sizeof(b->strings));
  b->strings[0] = '\0';
  (void)memcpy(&b->strings[k_pbe_div_off], "div", 4U);
  b->chapters[0].root_node = 0U;
  b->nodes[0]              = (book_node_t){.kind         = k_book_node_element,
                                           .name_off     = (uint32_t)k_pbe_div_off,
                                           .first_child  = 1U,
                                           .next_sibling = k_book_nil};
  b->nodes[1]              = (book_node_t){.kind         = k_book_node_text,
                                           .text_off     = 0U,
                                           .first_child  = k_book_nil,
                                           .next_sibling = k_book_nil};
  pbook_stamp_crc(b, (uint32_t)sizeof(*b));
}

/**
 * @test test_book_paged_prefetch_record_fault
 * @brief A backing fault on the chapter-record read propagates out of
 *        book_src_prefetch_chapter() verbatim.
 *
 * @par Coverage:
 * Reaches book_src_prefetch_chapter's `if (ce != k_ra8_ok) return ce;` arm: the
 * chapter table is placed in a cold frame (past the header-load frames) and the
 * backing is armed to fault every read beyond the header once bound, so the
 * prefetch's chapter-record read faults and the loader error is returned.
 *
 * @par MC/DC:
 * (no compound decisions under test -- a single error passthrough of the
 * chapter-record read; book_src_prefetch_chapter has no compound decisions)
 */
static void test_book_paged_prefetch_record_fault(void)
{
  TEST_BEGIN("book prefetch propagates a chapter-record read fault");

  static pbook_farchap_t s_b;
  pbe_farchap_setup(&s_b);

  s_pbook_bytes       = (const uint8_t*)&s_b;
  s_pbook_len         = (uint32_t)sizeof(s_b);
  s_pbook_fault_armed = false;
  s_pbook_fault_off   = (uint32_t)k_pb_frame_bytes * 2U; /* frames 0,1 hold the header */

  ra8_vsource_obj_t objs[1];
  ra8_vsource_t     vs = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vsource_init(&vs, objs, 1U));
  uint32_t oid = 0U;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_vsource_add_paged(&vs, pbook_read_fault, nullptr, 0U, (uint64_t)sizeof(s_b), &oid));
  ra8_vmem_cfg_t cfg = pbe_vmem_cfg(&vs);
  ra8_vmem_t     vm  = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_init(&vm, &cfg));
  book_src_t psrc = {};
  TEST_ASSERT_EQ(
    k_ra8_ok,
    book_src_paged(&psrc, &vm, oid, (uint32_t)k_pb_frame_bytes, (uint32_t)sizeof(s_b)));

  /* Header is bound; now every cold frame faults. The chapter record lives past
   * the header frames, so the prefetch's record read faults and returns it. */
  s_pbook_fault_armed = true;
  const ra8_err_t e   = book_src_prefetch_chapter(&psrc, 0U);
  TEST_ASSERT(e != k_ra8_ok);
  s_pbook_fault_armed = false; /* disarm so the backing is reusable */

  TEST_END("book prefetch propagates a chapter-record read fault");
}

/** @brief Case A -- cold: the root-node frame is not resident, so the read misses. */
static void prefetch_cold_case(const pbook_t* book, uint32_t node1_off, uint32_t rlen)
{
  book_src_t        psrc = {};
  ra8_vsource_t     vs   = {};
  ra8_vsource_obj_t objs[1];
  ra8_vmem_t        vm       = {};
  uint8_t           probe[8] = {};
  pbook_bind_paged(&psrc, &vs, objs, &vm, book, (uint32_t)sizeof(*book));

  uint32_t h0 = 0U;
  uint32_t m0 = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_stats(&vm, &h0, &m0, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, book_src_read(&psrc, node1_off, probe, rlen));
  uint32_t h1 = 0U;
  uint32_t m1 = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_stats(&vm, &h1, &m1, nullptr));
  TEST_ASSERT(m1 > m0); /* cold: the read faulted the frame in */
}

/**
 * @test test_book_paged_prefetch_warms
 * @brief book_src_prefetch_chapter() warms a chapter's first content frame so
 *        the next real read of it is a cache HIT (the #207 read-ahead primitive).
 *
 * @par Coverage:
 * Proves the flush-idle read-ahead #207 wires into the reader loop: a cold read of
 * chapter 1's root-node frame is a cache MISS, whereas the same read after
 * book_src_prefetch_chapter(&psrc, 1) is a HIT with no new miss -- the frame was
 * warmed and left resident. Chapter-text byte-identity is unchanged (the warm is
 * transparent), and all three argument guards are exercised.
 *
 * @par MC/DC:
 * (no compound decisions under test -- book_src_prefetch_chapter's guards are
 * independent single-condition checks (null src, resident/unbound, chapter range),
 * and the warm/hit result is a cache-counter delta, not a boolean decision)
 */
static void test_book_paged_prefetch_warms(void)
{
  TEST_BEGIN("book prefetch warms adjacent chapter into the cache");

  static pbook_t s_book;
  pbook_setup(&s_book);
  TEST_ASSERT_EQ(k_ra8_ok, book_validate(&s_book, sizeof(s_book)));

  /* Chapter 1's first content read is its root DOM node (root_node == 5). Read a
   * short slice that stays inside the single frame the prefetch warms. */
  const uint32_t node1_off =
    s_book.hdr.node_off + (s_book.chapters[1].root_node * (uint32_t)sizeof(book_node_t));
  const uint32_t in_frame = node1_off % (uint32_t)k_pb_frame_bytes;
  const uint32_t within   = (uint32_t)k_pb_frame_bytes - in_frame;
  const uint32_t rlen     = (within < 8U) ? within : 8U;
  uint8_t        probe[8] = {};

  prefetch_cold_case(&s_book, node1_off, rlen);

  /* Case B -- prefetched: warm chapter 1, then the same read is a HIT. */
  {
    book_src_t        psrc = {};
    ra8_vsource_t     vs   = {};
    ra8_vsource_obj_t objs[1];
    ra8_vmem_t        vm = {};
    pbook_bind_paged(&psrc, &vs, objs, &vm, &s_book, (uint32_t)sizeof(s_book));

    TEST_ASSERT_EQ(k_ra8_ok, book_src_prefetch_chapter(&psrc, 1U)); /* warm N+1 */

    uint32_t h0 = 0U;
    uint32_t m0 = 0U;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_stats(&vm, &h0, &m0, nullptr));
    TEST_ASSERT_EQ(k_ra8_ok, book_src_read(&psrc, node1_off, probe, rlen));
    uint32_t h1 = 0U;
    uint32_t m1 = 0U;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_stats(&vm, &h1, &m1, nullptr));
    TEST_ASSERT(h1 > h0);   /* warm: the read hit the prefetched frame */
    TEST_ASSERT_EQ(m0, m1); /* no new miss -- it was already resident  */

    /* Transparency: the warm changes only residency, never the bytes read back. */
    book_src_t rsrc = {};
    TEST_ASSERT_EQ(k_ra8_ok, book_src_resident(&rsrc, &s_book, (uint32_t)sizeof(s_book)));
    char   res[k_pbook_render_cap] = {};
    char   pag[k_pbook_render_cap] = {};
    size_t r_len                   = 0U;
    size_t p_len                   = 0U;
    TEST_ASSERT_EQ(k_ra8_ok, book_chapter_text_src(&rsrc, 1U, res, sizeof res, &r_len));
    TEST_ASSERT_EQ(k_ra8_ok, book_chapter_text_src(&psrc, 1U, pag, sizeof pag, &p_len));
    TEST_ASSERT(r_len > 0U);
    TEST_ASSERT_EQ(r_len, p_len);
    TEST_ASSERT(internal_objects_equal(res, pag, r_len));

    /* Argument guards: null src, resident (no cache) src, out-of-range chapter. */
    TEST_ASSERT_EQ(k_ra8_err_null_ptr, book_src_prefetch_chapter(nullptr, 0U));
    TEST_ASSERT_EQ(k_ra8_err_invalid_state, book_src_prefetch_chapter(&rsrc, 0U));
    TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                   book_src_prefetch_chapter(&psrc, s_book.hdr.chapter_count));
  }

  TEST_END("book prefetch warms adjacent chapter into the cache");
}
int main(void)
{
  test_book_paged_chunked_backing();
  test_book_paged_cyclic_walk_guard();
  test_book_paged_block_break_overflow();
  test_book_paged_long_tag_name();
  test_book_paged_prefetch_record_fault();
  test_book_paged_prefetch_warms();
  return 0;
}
