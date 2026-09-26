/**
 * @file test_ra8_xml_writer.c
 * @brief Host tests for the bounded XML emitter.
 * @ingroup grp_fmt
 *
 * @par Tag
 * [Ring 2 / Interface] {World: Any}
 *
 * @details Exercises escaping, tag balance, the empty-element form, the
 *          attribute ordering rule, and the sticky-failure contract that lets
 *          a caller test once at finish().
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_xml_writer.h"
#include "unity_minimal.h"

/**
 * @enum xml_test_limits_t
 * @brief Fixture sizes shared by the cases below.
 */
typedef enum : uint16_t {
  k_xml_test_buf   = 512U, /**< Document buffer for a roomy case. */
  k_xml_test_depth = 8U,   /**< Element stack for a roomy case.   */
} xml_test_limits_t;

/** @brief Fixture: a writer over its own buffer and stack. */
typedef struct {
  /** @brief Builder under test. */
  ra8_xml_writer_t w;
  /** @brief Element stack the builder pushes open elements onto. */
  ra8_xml_writer_frame_t frames[k_xml_test_depth];
  /** @brief Document buffer the builder appends into. */
  char buf[k_xml_test_buf];
} xml_test_fixture_t;

/** @brief Arm `f` over its own storage, asserting init accepted it. */
RA8_INTERNAL static void internal_arm(xml_test_fixture_t* f)
{
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_xml_writer_init(&f->w,
                                     f->buf,
                                     sizeof(f->buf),
                                     f->frames,
                                     (uint32_t)k_xml_test_depth));
}

/** @brief The escaper substitutes all five entities and refuses overflow. */
RA8_INTERNAL static void internal_test_escape(void)
{
  TEST_BEGIN("xml_escape");

  char out[64];
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xml_escape("a&b<c>\"d'e", out, sizeof(out)));
  TEST_ASSERT(strcmp(out, "a&amp;b&lt;c&gt;&quot;d&apos;e") == 0);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_xml_escape("page_001.jpg", out, sizeof(out)));
  TEST_ASSERT(strcmp(out, "page_001.jpg") == 0);

  char tiny[4];
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_xml_escape("&&&", tiny, sizeof(tiny)));
  TEST_ASSERT(tiny[0] == '\0');

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_xml_escape(nullptr, out, sizeof(out)));
  TEST_ASSERT(out[0] == '\0');
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_xml_escape("x", nullptr, 1U));

  TEST_END("xml_escape");
}

/** @brief A nested document with attributes and text comes out well formed. */
RA8_INTERNAL static void internal_test_document(void)
{
  TEST_BEGIN("xml_document");

  xml_test_fixture_t f;
  internal_arm(&f);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_xml_writer_declaration(&f.w));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xml_writer_start_element(&f.w, "ComicInfo"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xml_writer_attr(&f.w, "xmlns:xsi", "urn:x"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xml_writer_start_element(&f.w, "Title"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xml_writer_text(&f.w, "Tom & Jerry"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xml_writer_end_element(&f.w));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xml_writer_end_element(&f.w));

  size_t len = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xml_writer_finish(&f.w, &len));
  TEST_ASSERT(strcmp(f.buf,
                     "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                     "<ComicInfo xmlns:xsi=\"urn:x\">"
                     "<Title>Tom &amp; Jerry</Title></ComicInfo>") == 0);
  TEST_ASSERT_EQ(strlen(f.buf), len);

  TEST_END("xml_document");
}

/** @brief An element that took nothing closes in the empty form. */
RA8_INTERNAL static void internal_test_empty_element(void)
{
  TEST_BEGIN("xml_empty_element");

  xml_test_fixture_t f;
  internal_arm(&f);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_xml_writer_start_element(&f.w, "spine"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xml_writer_start_element(&f.w, "itemref"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xml_writer_attr(&f.w, "idref", "pg1"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xml_writer_end_element(&f.w));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xml_writer_end_element(&f.w));

  size_t len = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xml_writer_finish(&f.w, &len));
  TEST_ASSERT(strcmp(f.buf, "<spine><itemref idref=\"pg1\"/></spine>") == 0);

  TEST_END("xml_empty_element");
}

/** @brief An untrusted value cannot break out of an attribute or of text. */
RA8_INTERNAL static void internal_test_injection(void)
{
  TEST_BEGIN("xml_injection");

  xml_test_fixture_t f;
  internal_arm(&f);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_xml_writer_start_element(&f.w, "item"));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_xml_writer_attr(&f.w, "href", "a\"><script>.jpg"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xml_writer_end_element(&f.w));

  size_t len = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xml_writer_finish(&f.w, &len));
  TEST_ASSERT(strstr(f.buf, "<script>") == nullptr);
  TEST_ASSERT(strstr(f.buf, "a&quot;&gt;&lt;script&gt;.jpg") != nullptr);

  TEST_END("xml_injection");
}

/** @brief An attribute after content, and a bad name, are refused. */
RA8_INTERNAL static void internal_test_ordering(void)
{
  TEST_BEGIN("xml_ordering");

  xml_test_fixture_t f;
  internal_arm(&f);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xml_writer_start_element(&f.w, "a"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xml_writer_text(&f.w, "x"));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_xml_writer_attr(&f.w, "k", "v"));

  xml_test_fixture_t g;
  internal_arm(&g);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_xml_writer_start_element(&g.w, "1bad"));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_xml_writer_start_element(&g.w, "al so"));

  TEST_END("xml_ordering");
}

/** @brief An unclosed element, and a stray close, are refused at finish. */
RA8_INTERNAL static void internal_test_balance(void)
{
  TEST_BEGIN("xml_balance");

  xml_test_fixture_t f;
  internal_arm(&f);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xml_writer_start_element(&f.w, "open"));
  size_t len = 1U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_xml_writer_finish(&f.w, &len));
  TEST_ASSERT_EQ(0U, len);

  xml_test_fixture_t g;
  internal_arm(&g);
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_xml_writer_end_element(&g.w));

  TEST_END("xml_balance");
}

/** @brief The first refusal sticks and is what finish() reports. */
RA8_INTERNAL static void internal_test_sticky(void)
{
  TEST_BEGIN("xml_sticky");

  ra8_xml_writer_t       w;
  ra8_xml_writer_frame_t frames[2];
  char                   buf[24];

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_xml_writer_init(&w, buf, sizeof(buf), frames, 2U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xml_writer_start_element(&w, "root"));
  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 ra8_xml_writer_text(&w, "far too long for this buffer"));
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_xml_writer_end_element(&w));

  size_t len = 1U;
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_xml_writer_finish(&w, &len));
  TEST_ASSERT_EQ(0U, len);

  TEST_END("xml_sticky");
}

/** @brief The stack depth the caller supplied is the depth enforced. */
RA8_INTERNAL static void internal_test_depth(void)
{
  TEST_BEGIN("xml_depth");

  ra8_xml_writer_t       w;
  ra8_xml_writer_frame_t frames[1];
  char                   buf[64];

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_xml_writer_init(&w, buf, sizeof(buf), frames, 0U));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_xml_writer_init(&w, buf, sizeof(buf), frames, 1U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_xml_writer_start_element(&w, "a"));
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_xml_writer_start_element(&w, "b"));

  TEST_END("xml_depth");
}

/** @brief Run every case in order. */
int main(void)
{
  internal_test_escape();
  internal_test_document();
  internal_test_empty_element();
  internal_test_injection();
  internal_test_ordering();
  internal_test_balance();
  internal_test_sticky();
  internal_test_depth();
  return 0;
}
