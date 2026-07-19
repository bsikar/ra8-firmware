/**
 * @file test_ra8_book.c
 * @brief Unit tests for the flat, XIP e-book container format (libs/ra8_book).
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
 * @enum book_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_book_data_size_10 = 10,
  k_book_data_size_20 = 20,
  k_book_raw_size_50  = 50,
  k_book_val_128      = 128,
  k_book_val_64       = 64,
} book_uint8_const_t;

/**
 * @enum book_uint16_const_t
 * @brief Named uint16_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint16_t {
  k_book_format_version_999 = 999,
} book_uint16_const_t;

/**
 * @enum book_uint32_const_t
 * @brief Named uint32_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint32_t {
  k_book_crc32_ffffffff = 0xFFFFFFFF,
  k_book_crc_ffffffff   = 0xFFFFFFFFU,
  k_book_val_edb88320   = 0xEDB88320U,
} book_uint32_const_t;

static uint32_t compute_crc32(const uint8_t* data, size_t len)
{
  uint32_t crc = k_book_crc_ffffffff;
  for (size_t i = 0U; i < len; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0U; bit < 8U; ++bit) {
      uint32_t mask = (uint32_t)(-(int32_t)(crc & 1U));
      crc           = (crc >> 1U) ^ (k_book_val_edb88320 & mask);
    }
  }
  return crc ^ k_book_crc_ffffffff;
}

typedef struct {
  ra8_book_header_t     hdr;                       /**< Hdr.         */
  ra8_book_chapter_t    chapters[1];               /**< Chapters.    */
  ra8_book_node_t       nodes[2];                  /**< Nodes.       */
  ra8_book_attr_t       attrs[1];                  /**< Attrs.       */
  ra8_book_stylesheet_t stylesheets[1];            /**< Stylesheets. */
  ra8_book_image_t      images[2];                 /**< Images.      */
  char                  strings[k_book_val_128];   /**< Strings.     */
  uint8_t               image_pool[k_book_val_64]; /**< Image pool.  */
} mock_book_t;

/**
 * @struct mock_book_offsets_t
 * @brief String-pool offsets shared between the mock-book build phases.
 *
 * @details `setup_mock_book_strings` interns each label and records its offset
 *          here; `setup_mock_book_structs` reads them back when wiring the
 *          chapter / node / attribute / stylesheet / image tables.
 *
 * @invariant Every field is a byte offset into `mock_book_t::strings`.
 * @see setup_mock_book
 */
typedef struct {
  uint32_t ch_title;     /**< "Chapter 1" chapter title.  */
  uint32_t ch_href;      /**< "ch1.xhtml" chapter href.   */
  uint32_t div_tag;      /**< "div" element name.         */
  uint32_t class_attr;   /**< "class" attribute name.     */
  uint32_t main_val;     /**< "main" attribute value.     */
  uint32_t text_content; /**< "Hello World" text node.    */
  uint32_t css_content;  /**< Inline stylesheet source.   */
  uint32_t img_href;     /**< "img1.png" raster image id. */
  uint32_t svg_href;     /**< "svg1.svg" vector image id. */
} mock_book_offsets_t;

/**
 * @brief Intern one NUL-terminated string into the mock book's string pool.
 *
 * @param[in,out] b      Mock book whose string pool receives the copy.
 * @param[in,out] offset Running write cursor, advanced past the copy + NUL.
 * @param[in]     s      NUL-terminated string to append.
 *
 * @return The pool offset the string was written at.
 * @pre @p offset points at a value within `b->strings`.
 * @post @p *offset advanced by strlen(@p s) + 1.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static uint32_t mock_intern(mock_book_t* b, int* offset, const char* s)
{
  uint32_t at = (uint32_t)*offset;
  strcpy(&b->strings[*offset], s);
  *offset += (int)strlen(s) + 1;
  return at;
}

/**
 * @brief Fill the mock book's fixed header (magic, version, table counts/offsets).
 *
 * @param[out] b Mock book to initialise; fully zeroed first.
 *
 * @return None.
 * @pre @p b is non-null and writable.
 * @post Header magic, version and every table offset/count are populated.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void setup_mock_book_header(mock_book_t* b)
{
  memset(b, 0, sizeof(*b));

  memcpy(b->hdr.magic, "RABOOK1", 8);
  b->hdr.format_version = k_ra8_book_format_version;
  b->hdr.total_size     = sizeof(*b);
  b->hdr.flags          = 0;

  b->hdr.chapter_count = 1;
  b->hdr.chapter_off   = offsetof(mock_book_t, chapters);

  b->hdr.node_count = 2;
  b->hdr.node_off   = offsetof(mock_book_t, nodes);

  b->hdr.attr_count = 1;
  b->hdr.attr_off   = offsetof(mock_book_t, attrs);

  b->hdr.stylesheet_count = 1;
  b->hdr.stylesheet_off   = offsetof(mock_book_t, stylesheets);

  b->hdr.image_count = 2;
  b->hdr.image_off   = offsetof(mock_book_t, images);

  b->hdr.string_off  = offsetof(mock_book_t, strings);
  b->hdr.string_size = sizeof(b->strings);

  b->hdr.image_pool_off  = offsetof(mock_book_t, image_pool);
  b->hdr.image_pool_size = sizeof(b->image_pool);
}

/**
 * @brief Intern every mock-book label and record the shared table offsets.
 *
 * @param[in,out] b   Mock book whose string pool and header offsets are filled.
 * @param[out]    off Receives the offsets consumed by setup_mock_book_structs.
 *
 * @return None.
 * @pre setup_mock_book_header(@p b) already ran (string_off/size set).
 * @post Header title/author/language/identifier offsets and @p off are set.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void setup_mock_book_strings(mock_book_t* b, mock_book_offsets_t* off)
{
  int offset         = 0;
  b->strings[offset] = '\0';
  offset += 1;

  b->hdr.title_off      = mock_intern(b, &offset, "Title");
  b->hdr.author_off     = mock_intern(b, &offset, "Author");
  b->hdr.language_off   = mock_intern(b, &offset, "en");
  b->hdr.identifier_off = mock_intern(b, &offset, "ID123");

  off->ch_title     = mock_intern(b, &offset, "Chapter 1");
  off->ch_href      = mock_intern(b, &offset, "ch1.xhtml");
  off->div_tag      = mock_intern(b, &offset, "div");
  off->class_attr   = mock_intern(b, &offset, "class");
  off->main_val     = mock_intern(b, &offset, "main");
  off->text_content = mock_intern(b, &offset, "Hello World");
  off->css_content  = mock_intern(b, &offset, "body { margin: 0; }");
  off->img_href     = mock_intern(b, &offset, "img1.png");
  off->svg_href     = mock_intern(b, &offset, "svg1.svg");
}

/**
 * @brief Wire the mock book's chapter, node, attribute, stylesheet and image tables.
 *
 * @param[in,out] b   Mock book whose structural tables are populated.
 * @param[in]     off Interned string offsets from setup_mock_book_strings.
 *
 * @return None.
 * @pre setup_mock_book_strings(@p b, @p off) already ran.
 * @post All structural tables and the cover-image index are set.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void setup_mock_book_structs(mock_book_t* b, const mock_book_offsets_t* off)
{
  b->chapters[0].title_off = off->ch_title;
  b->chapters[0].href_off  = off->ch_href;
  b->chapters[0].root_node = 0;

  b->nodes[0].kind         = k_ra8_book_node_element;
  b->nodes[0].reserved     = 0;
  b->nodes[0].attr_count   = 1;
  b->nodes[0].name_off     = off->div_tag;
  b->nodes[0].text_off     = 0;
  b->nodes[0].first_attr   = 0;
  b->nodes[0].first_child  = 1;
  b->nodes[0].next_sibling = k_ra8_book_nil;

  b->nodes[1].kind         = k_ra8_book_node_text;
  b->nodes[1].reserved     = 0;
  b->nodes[1].attr_count   = 0;
  b->nodes[1].name_off     = 0;
  b->nodes[1].text_off     = off->text_content;
  b->nodes[1].first_attr   = k_ra8_book_nil;
  b->nodes[1].first_child  = k_ra8_book_nil;
  b->nodes[1].next_sibling = k_ra8_book_nil;

  b->attrs[0].name_off  = off->class_attr;
  b->attrs[0].value_off = off->main_val;

  b->stylesheets[0].source_off    = off->css_content;
  b->stylesheets[0].scope_chapter = k_ra8_book_nil;

  b->images[0].id_off    = off->img_href;
  b->images[0].width     = 16;
  b->images[0].height    = 16;
  b->images[0].format    = k_ra8_book_image_gray4;
  b->images[0].reserved  = 0;
  b->images[0].reserved2 = 0;
  b->images[0].data_off  = 0;
  b->images[0].data_size = k_book_data_size_10;
  b->images[0].raw_size  = k_book_val_128;

  b->images[1].id_off    = off->svg_href;
  b->images[1].width     = 0;
  b->images[1].height    = 0;
  b->images[1].format    = k_ra8_book_image_svg;
  b->images[1].reserved  = 0;
  b->images[1].reserved2 = 0;
  b->images[1].data_off  = k_book_data_size_10;
  b->images[1].data_size = k_book_data_size_20;
  b->images[1].raw_size  = k_book_raw_size_50;

  b->hdr.cover_image_index = 0;
}

/**
 * @brief Build a complete, CRC-valid in-memory mock book for the reader tests.
 *
 * @param[out] b Mock book to populate end to end.
 *
 * @return None.
 * @pre @p b is non-null and writable.
 * @post @p b is a self-consistent book whose header CRC covers the body.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void setup_mock_book(mock_book_t* b)
{
  mock_book_offsets_t off = {};
  setup_mock_book_header(b);
  setup_mock_book_strings(b, &off);
  setup_mock_book_structs(b, &off);

  const uint8_t* body     = (const uint8_t*)b + sizeof(ra8_book_header_t);
  uint32_t       body_len = sizeof(*b) - sizeof(ra8_book_header_t);
  b->hdr.crc32            = compute_crc32(body, body_len);
}

/**
 * @test test_ra8_book_invalid
 * @brief ra8_book_validate() rejects null, short, mis-magicked, mis-versioned,
 *        CRC-corrupt, and out-of-bounds blobs with the documented codes.
 *
 * @par MC/DC:
 * (no compound decisions newly claimed by this test -- it exercises the
 * public rejection contract one malformed blob at a time; the validator's
 * compound decisions carry their MC/DC vector sets in
 * tests/test_ra8_book_cov.c)
 */
static void test_ra8_book_invalid(void)
{
  TEST_BEGIN("ra8_book validation invalid parameters");

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_book_validate(NULL, 100));

  uint8_t buffer[k_book_val_64] = {0};
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_book_validate(buffer, sizeof(buffer)));

  mock_book_t b;
  setup_mock_book(&b);
  memcpy(b.hdr.magic, "WRONGMAG", 8);
  const uint8_t* body     = (const uint8_t*)&b + sizeof(ra8_book_header_t);
  uint32_t       body_len = sizeof(b) - sizeof(ra8_book_header_t);
  b.hdr.crc32             = compute_crc32(body, body_len);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_book_validate(&b, sizeof(b)));

  setup_mock_book(&b);
  b.hdr.format_version = k_book_format_version_999;
  body                 = (const uint8_t*)&b + sizeof(ra8_book_header_t);
  b.hdr.crc32          = compute_crc32(body, body_len);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_book_validate(&b, sizeof(b)));

  setup_mock_book(&b);
  b.hdr.crc32 ^= k_book_crc32_ffffffff;
  TEST_ASSERT_EQ(k_ra8_err_range_check_failed, ra8_book_validate(&b, sizeof(b)));

  setup_mock_book(&b);
  b.hdr.chapter_off = sizeof(b) + k_book_data_size_10;
  body              = (const uint8_t*)&b + sizeof(ra8_book_header_t);
  b.hdr.crc32       = compute_crc32(body, body_len);
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_book_validate(&b, sizeof(b)));

  TEST_END("ra8_book validation invalid parameters");
}

/**
 * @brief Assert the two-node div/text tree and its single class attribute.
 *
 * @param[in] b   Validated mock book.
 * @param[in] hdr Parsed header (for node_count).
 *
 * @return None.
 * @pre @p b passed ra8_book_validate().
 * @post The element node, its attribute, and the text child all match.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void book_walk_check_nodes(const mock_book_t* b, const ra8_book_header_t* hdr)
{
  TEST_ASSERT_EQ(2, hdr->node_count);
  const ra8_book_node_t* nodes = ra8_book_nodes(b);
  TEST_ASSERT_NOT_NULL(nodes);

  const ra8_book_node_t* n0 = &nodes[0];
  TEST_ASSERT_EQ(k_ra8_book_node_element, n0->kind);
  TEST_ASSERT_EQ(0, strcmp(ra8_book_node_name(b, n0), "div"));
  TEST_ASSERT_EQ(1, n0->attr_count);
  TEST_ASSERT_EQ(1, n0->first_child);
  TEST_ASSERT_EQ(k_ra8_book_nil, n0->next_sibling);

  const ra8_book_attr_t* attrs = ra8_book_attrs(b);
  TEST_ASSERT_NOT_NULL(attrs);
  const ra8_book_attr_t* a0 = &attrs[n0->first_attr];
  TEST_ASSERT_EQ(0, strcmp(ra8_book_string(b, a0->name_off), "class"));
  TEST_ASSERT_EQ(0, strcmp(ra8_book_string(b, a0->value_off), "main"));

  const ra8_book_node_t* n1 = &nodes[n0->first_child];
  TEST_ASSERT_EQ(k_ra8_book_node_text, n1->kind);
  TEST_ASSERT_EQ(0, strcmp(ra8_book_node_text(b, n1), "Hello World"));
  TEST_ASSERT_EQ(0, n1->attr_count);
  TEST_ASSERT_EQ(k_ra8_book_nil, n1->first_child);
  TEST_ASSERT_EQ(k_ra8_book_nil, n1->next_sibling);
}

/**
 * @brief Assert the raster and vector image manifest entries.
 *
 * @param[in] b   Validated mock book.
 * @param[in] hdr Parsed header (for image_count).
 *
 * @return None.
 * @pre @p b passed ra8_book_validate().
 * @post Both image records match their fixture geometry and data pointers.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void book_walk_check_images(const mock_book_t* b, const ra8_book_header_t* hdr)
{
  TEST_ASSERT_EQ(2, hdr->image_count);
  const ra8_book_image_t* imgs = ra8_book_images(b);
  TEST_ASSERT_NOT_NULL(imgs);

  TEST_ASSERT_EQ(0, strcmp(ra8_book_string(b, imgs[0].id_off), "img1.png"));
  TEST_ASSERT_EQ(16, imgs[0].width);
  TEST_ASSERT_EQ(16, imgs[0].height);
  TEST_ASSERT_EQ(k_ra8_book_image_gray4, imgs[0].format);
  TEST_ASSERT_EQ(10, imgs[0].data_size);
  TEST_ASSERT_EQ(128, imgs[0].raw_size);
  TEST_ASSERT_NOT_NULL(ra8_book_image_data(b, &imgs[0]));

  TEST_ASSERT_EQ(0, strcmp(ra8_book_string(b, imgs[1].id_off), "svg1.svg"));
  TEST_ASSERT_EQ(0, imgs[1].width);
  TEST_ASSERT_EQ(0, imgs[1].height);
  TEST_ASSERT_EQ(k_ra8_book_image_svg, imgs[1].format);
  TEST_ASSERT_EQ(20, imgs[1].data_size);
  TEST_ASSERT_EQ(50, imgs[1].raw_size);
  TEST_ASSERT_NOT_NULL(ra8_book_image_data(b, &imgs[1]));
}

/**
 * @test test_ra8_book_valid_walk
 * @brief A valid mock blob passes validation and every inline accessor
 *        resolves the expected strings, nodes, attributes, and images.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- the inline accessors under test
 * are pure offset arithmetic with no `&&` or `||`; the walk asserts
 * values, not decision outcomes)
 */
static void test_ra8_book_valid_walk(void)
{
  TEST_BEGIN("ra8_book validation and walking valid book");

  mock_book_t b;
  setup_mock_book(&b);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_validate(&b, sizeof(b)));

  const ra8_book_header_t* hdr = ra8_book_header(&b);
  TEST_ASSERT_NOT_NULL(hdr);

  TEST_ASSERT_EQ(0, strcmp(ra8_book_string(&b, hdr->title_off), "Title"));
  TEST_ASSERT_EQ(0, strcmp(ra8_book_string(&b, hdr->author_off), "Author"));
  TEST_ASSERT_EQ(0, strcmp(ra8_book_string(&b, hdr->language_off), "en"));
  TEST_ASSERT_EQ(0, strcmp(ra8_book_string(&b, hdr->identifier_off), "ID123"));
  TEST_ASSERT_EQ(0, hdr->cover_image_index);

  TEST_ASSERT_EQ(1, hdr->chapter_count);
  const ra8_book_chapter_t* chaps = ra8_book_chapters(&b);
  TEST_ASSERT_NOT_NULL(chaps);
  TEST_ASSERT_EQ(0, strcmp(ra8_book_string(&b, chaps[0].title_off), "Chapter 1"));
  TEST_ASSERT_EQ(0, strcmp(ra8_book_string(&b, chaps[0].href_off), "ch1.xhtml"));
  TEST_ASSERT_EQ(0, chaps[0].root_node);

  book_walk_check_nodes(&b, hdr);

  TEST_ASSERT_EQ(1, hdr->stylesheet_count);
  const ra8_book_stylesheet_t* styles = ra8_book_stylesheets(&b);
  TEST_ASSERT_NOT_NULL(styles);
  TEST_ASSERT_EQ(0, strcmp(ra8_book_string(&b, styles[0].source_off), "body { margin: 0; }"));
  TEST_ASSERT_EQ(k_ra8_book_nil, styles[0].scope_chapter);

  book_walk_check_images(&b, hdr);

  TEST_END("ra8_book validation and walking valid book");
}

/**
 * @enum ra8_book_test_flag_t
 * @brief Header `flags` vectors used by the flags-mask validation tests.
 * @details Values chosen around ::k_ra8_book_flag_mask_known: the first bit
 *          just past the known mask and the top bit (far past it), plus a
 *          mixed word combining a known bit with an unknown one.
 */
typedef enum : uint32_t {
  k_rb_flag_unknown_lo = 0x00000002U, /**< First undefined bit above the mask.    */
  k_rb_flag_unknown_hi = 0x80000000U, /**< Top bit; also outside the known mask.  */
  k_rb_flag_mixed      = 0x80000001U, /**< Known RTL bit plus an unknown top bit. */
} ra8_book_test_flag_t;

/**
 * @test test_ra8_book_flags_known_mask
 * @brief ra8_book_validate() accepts every defined header flag bit and rejects
 *        any blob carrying a bit outside ::k_ra8_book_flag_mask_known.
 *
 * @par MC/DC:
 * (no compound decisions under test -- the flags guard in
 * ra8_book_validate() is the single condition
 * `(hdr->flags & ~k_ra8_book_flag_mask_known) != 0U`; the vectors below are
 * boundary coverage of that one condition, both polarities)
 * - flags = 0                       -> accepted (legacy v1 blob).
 * - flags = k_ra8_book_flag_rtl      -> accepted (all defined bits set).
 * - flags = first undefined bit     -> rejected, k_ra8_err_invalid_arg.
 * - flags = top bit                 -> rejected, k_ra8_err_invalid_arg.
 * - flags = RTL bit + undefined bit -> rejected (a known bit cannot smuggle
 *   an unknown one through).
 */
static void test_ra8_book_flags_known_mask(void)
{
  TEST_BEGIN("ra8_book flags known-mask validation");

  /* The flags word lives in the header, and the body CRC covers only the
   * bytes AFTER the header, so flag flips need no CRC recompute. */
  mock_book_t b;
  setup_mock_book(&b);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_validate(&b, sizeof(b)));

  b.hdr.flags = k_ra8_book_flag_rtl;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_validate(&b, sizeof(b)));

  b.hdr.flags = k_rb_flag_unknown_lo;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_book_validate(&b, sizeof(b)));

  b.hdr.flags = k_rb_flag_unknown_hi;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_book_validate(&b, sizeof(b)));

  b.hdr.flags = k_rb_flag_mixed;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_book_validate(&b, sizeof(b)));

  TEST_END("ra8_book flags known-mask validation");
}

/**
 * @test test_ra8_book_rtl_accessor
 * @brief ra8_book_is_rtl() decodes exactly the RTL bit of the header flags.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- ra8_book_is_rtl() is a single
 * masked-bit condition with no `&&` or `||`; both polarities are
 * exercised: bit clear -> false, bit set -> true, and a blob validated
 * with the bit set still reads back true through the accessor)
 */
static void test_ra8_book_rtl_accessor(void)
{
  TEST_BEGIN("ra8_book RTL flag accessor");

  mock_book_t b;
  setup_mock_book(&b);
  TEST_ASSERT_EQ(false, ra8_book_is_rtl(&b));

  b.hdr.flags = k_ra8_book_flag_rtl;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_validate(&b, sizeof(b)));
  TEST_ASSERT_EQ(true, ra8_book_is_rtl(&b));

  b.hdr.flags = 0;
  TEST_ASSERT_EQ(false, ra8_book_is_rtl(&b));

  TEST_END("ra8_book RTL flag accessor");
}

int main(void)
{
  test_ra8_book_invalid();
  test_ra8_book_valid_walk();
  test_ra8_book_flags_known_mask();
  test_ra8_book_rtl_accessor();
  return 0;
}
