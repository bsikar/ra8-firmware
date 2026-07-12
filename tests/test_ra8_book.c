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

static uint32_t compute_crc32(const uint8_t* data, size_t len)
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

typedef struct {
  ra8_book_header_t     hdr;
  ra8_book_chapter_t    chapters[1];
  ra8_book_node_t       nodes[2];
  ra8_book_attr_t       attrs[1];
  ra8_book_stylesheet_t stylesheets[1];
  ra8_book_image_t      images[2];
  char                  strings[128];
  uint8_t               image_pool[64];
} mock_book_t;

static void setup_mock_book(mock_book_t* b)
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

  int offset         = 0;
  b->strings[offset] = '\0';
  offset += 1;

  const char* title = "Title";
  b->hdr.title_off  = offset;
  strcpy(&b->strings[offset], title);
  offset += strlen(title) + 1;

  const char* author = "Author";
  b->hdr.author_off  = offset;
  strcpy(&b->strings[offset], author);
  offset += strlen(author) + 1;

  const char* language = "en";
  b->hdr.language_off  = offset;
  strcpy(&b->strings[offset], language);
  offset += strlen(language) + 1;

  const char* identifier = "ID123";
  b->hdr.identifier_off  = offset;
  strcpy(&b->strings[offset], identifier);
  offset += strlen(identifier) + 1;

  const char* ch_title     = "Chapter 1";
  uint32_t    ch_title_off = offset;
  strcpy(&b->strings[offset], ch_title);
  offset += strlen(ch_title) + 1;

  const char* ch_href     = "ch1.xhtml";
  uint32_t    ch_href_off = offset;
  strcpy(&b->strings[offset], ch_href);
  offset += strlen(ch_href) + 1;

  const char* div_tag     = "div";
  uint32_t    div_tag_off = offset;
  strcpy(&b->strings[offset], div_tag);
  offset += strlen(div_tag) + 1;

  const char* class_attr     = "class";
  uint32_t    class_attr_off = offset;
  strcpy(&b->strings[offset], class_attr);
  offset += strlen(class_attr) + 1;

  const char* main_val     = "main";
  uint32_t    main_val_off = offset;
  strcpy(&b->strings[offset], main_val);
  offset += strlen(main_val) + 1;

  const char* text_content     = "Hello World";
  uint32_t    text_content_off = offset;
  strcpy(&b->strings[offset], text_content);
  offset += strlen(text_content) + 1;

  const char* css_content     = "body { margin: 0; }";
  uint32_t    css_content_off = offset;
  strcpy(&b->strings[offset], css_content);
  offset += strlen(css_content) + 1;

  const char* img_href     = "img1.png";
  uint32_t    img_href_off = offset;
  strcpy(&b->strings[offset], img_href);
  offset += strlen(img_href) + 1;

  const char* svg_href     = "svg1.svg";
  uint32_t    svg_href_off = offset;
  strcpy(&b->strings[offset], svg_href);
  offset += strlen(svg_href) + 1;

  b->chapters[0].title_off = ch_title_off;
  b->chapters[0].href_off  = ch_href_off;
  b->chapters[0].root_node = 0;

  b->nodes[0].kind         = k_ra8_book_node_element;
  b->nodes[0].reserved     = 0;
  b->nodes[0].attr_count   = 1;
  b->nodes[0].name_off     = div_tag_off;
  b->nodes[0].text_off     = 0;
  b->nodes[0].first_attr   = 0;
  b->nodes[0].first_child  = 1;
  b->nodes[0].next_sibling = k_ra8_book_nil;

  b->nodes[1].kind         = k_ra8_book_node_text;
  b->nodes[1].reserved     = 0;
  b->nodes[1].attr_count   = 0;
  b->nodes[1].name_off     = 0;
  b->nodes[1].text_off     = text_content_off;
  b->nodes[1].first_attr   = k_ra8_book_nil;
  b->nodes[1].first_child  = k_ra8_book_nil;
  b->nodes[1].next_sibling = k_ra8_book_nil;

  b->attrs[0].name_off  = class_attr_off;
  b->attrs[0].value_off = main_val_off;

  b->stylesheets[0].source_off    = css_content_off;
  b->stylesheets[0].scope_chapter = k_ra8_book_nil;

  b->images[0].id_off    = img_href_off;
  b->images[0].width     = 16;
  b->images[0].height    = 16;
  b->images[0].format    = k_ra8_book_image_gray4;
  b->images[0].reserved  = 0;
  b->images[0].reserved2 = 0;
  b->images[0].data_off  = 0;
  b->images[0].data_size = 10;
  b->images[0].raw_size  = 128;

  b->images[1].id_off    = svg_href_off;
  b->images[1].width     = 0;
  b->images[1].height    = 0;
  b->images[1].format    = k_ra8_book_image_svg;
  b->images[1].reserved  = 0;
  b->images[1].reserved2 = 0;
  b->images[1].data_off  = 10;
  b->images[1].data_size = 20;
  b->images[1].raw_size  = 50;

  b->hdr.cover_image_index = 0;

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

  uint8_t buffer[64] = {0};
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_book_validate(buffer, sizeof(buffer)));

  mock_book_t b;
  setup_mock_book(&b);
  memcpy(b.hdr.magic, "WRONGMAG", 8);
  const uint8_t* body     = (const uint8_t*)&b + sizeof(ra8_book_header_t);
  uint32_t       body_len = sizeof(b) - sizeof(ra8_book_header_t);
  b.hdr.crc32             = compute_crc32(body, body_len);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_book_validate(&b, sizeof(b)));

  setup_mock_book(&b);
  b.hdr.format_version = 999;
  body                 = (const uint8_t*)&b + sizeof(ra8_book_header_t);
  b.hdr.crc32          = compute_crc32(body, body_len);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_book_validate(&b, sizeof(b)));

  setup_mock_book(&b);
  b.hdr.crc32 ^= 0xFFFFFFFF;
  TEST_ASSERT_EQ(k_ra8_err_range_check_failed, ra8_book_validate(&b, sizeof(b)));

  setup_mock_book(&b);
  b.hdr.chapter_off = sizeof(b) + 10;
  body              = (const uint8_t*)&b + sizeof(ra8_book_header_t);
  b.hdr.crc32       = compute_crc32(body, body_len);
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_book_validate(&b, sizeof(b)));

  TEST_END("ra8_book validation invalid parameters");
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

  TEST_ASSERT_EQ(2, hdr->node_count);
  const ra8_book_node_t* nodes = ra8_book_nodes(&b);
  TEST_ASSERT_NOT_NULL(nodes);

  const ra8_book_node_t* n0 = &nodes[0];
  TEST_ASSERT_EQ(k_ra8_book_node_element, n0->kind);
  TEST_ASSERT_EQ(0, strcmp(ra8_book_node_name(&b, n0), "div"));
  TEST_ASSERT_EQ(1, n0->attr_count);
  TEST_ASSERT_EQ(1, n0->first_child);
  TEST_ASSERT_EQ(k_ra8_book_nil, n0->next_sibling);

  const ra8_book_attr_t* attrs = ra8_book_attrs(&b);
  TEST_ASSERT_NOT_NULL(attrs);
  const ra8_book_attr_t* a0 = &attrs[n0->first_attr];
  TEST_ASSERT_EQ(0, strcmp(ra8_book_string(&b, a0->name_off), "class"));
  TEST_ASSERT_EQ(0, strcmp(ra8_book_string(&b, a0->value_off), "main"));

  const ra8_book_node_t* n1 = &nodes[n0->first_child];
  TEST_ASSERT_EQ(k_ra8_book_node_text, n1->kind);
  TEST_ASSERT_EQ(0, strcmp(ra8_book_node_text(&b, n1), "Hello World"));
  TEST_ASSERT_EQ(0, n1->attr_count);
  TEST_ASSERT_EQ(k_ra8_book_nil, n1->first_child);
  TEST_ASSERT_EQ(k_ra8_book_nil, n1->next_sibling);

  TEST_ASSERT_EQ(1, hdr->stylesheet_count);
  const ra8_book_stylesheet_t* styles = ra8_book_stylesheets(&b);
  TEST_ASSERT_NOT_NULL(styles);
  TEST_ASSERT_EQ(0, strcmp(ra8_book_string(&b, styles[0].source_off), "body { margin: 0; }"));
  TEST_ASSERT_EQ(k_ra8_book_nil, styles[0].scope_chapter);

  TEST_ASSERT_EQ(2, hdr->image_count);
  const ra8_book_image_t* imgs = ra8_book_images(&b);
  TEST_ASSERT_NOT_NULL(imgs);

  TEST_ASSERT_EQ(0, strcmp(ra8_book_string(&b, imgs[0].id_off), "img1.png"));
  TEST_ASSERT_EQ(16, imgs[0].width);
  TEST_ASSERT_EQ(16, imgs[0].height);
  TEST_ASSERT_EQ(k_ra8_book_image_gray4, imgs[0].format);
  TEST_ASSERT_EQ(10, imgs[0].data_size);
  TEST_ASSERT_EQ(128, imgs[0].raw_size);
  TEST_ASSERT_NOT_NULL(ra8_book_image_data(&b, &imgs[0]));

  TEST_ASSERT_EQ(0, strcmp(ra8_book_string(&b, imgs[1].id_off), "svg1.svg"));
  TEST_ASSERT_EQ(0, imgs[1].width);
  TEST_ASSERT_EQ(0, imgs[1].height);
  TEST_ASSERT_EQ(k_ra8_book_image_svg, imgs[1].format);
  TEST_ASSERT_EQ(20, imgs[1].data_size);
  TEST_ASSERT_EQ(50, imgs[1].raw_size);
  TEST_ASSERT_NOT_NULL(ra8_book_image_data(&b, &imgs[1]));

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
