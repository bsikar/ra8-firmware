/**
 * @file test_ra8_css.c
 * @brief Host unit tests + MC/DC for the minimal content-CSS cascade (#111).
 *
 * @details
 * Exercises the v1 CSS subset: selector parsing (universal / type / class / id,
 * comma groups, comments, unsupported-selector skip), inline declarations, the
 * property grammar (font-weight / font-style / text-decoration / text-align),
 * selector matching, and the full cascade (specificity, source order,
 * inheritance, inline override). MC/DC vectors cover the compound decisions in
 * rule matching and the cascade entry guard.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_reflow.h"
#include "ra8_reflow_css.h"
#include "unity_minimal.h"

/**
 * @enum css_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_css_decl_cap = 80, /**< Capacity of the serialized-declaration buffer. */
} css_uint8_const_t;

/**
 * @enum css_uint32_const_t
 * @brief Named uint32_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint32_t {
  k_css_parent_color =
    0x123456U, /**< A parent colour with three distinct channel bytes, so a child that inherited only part of it is visible. */
} css_uint32_const_t;

/** @brief Shared parsed stylesheet for the matching / cascade tests. */
static ra8_css_sheet_t s_sheet;

/** @brief Parse @p css into the shared sheet (reset first); assert success. */
static void load(const char* css)
{
  TEST_ASSERT_EQ(k_ra8_ok, ra8_css_sheet_reset(&s_sheet));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_css_parse(&s_sheet, css, (uint32_t)strlen(css)));
}

/** @brief Build an element identity (NULL id/class allowed). */
static ra8_css_element_t elem(ra8_reflow_html_tag_t tag, const char* id, const char* cls)
{
  ra8_css_element_t e = {};
  e.tag               = (uint8_t)tag;
  e.id                = id;
  e.id_len            = (id != nullptr) ? (uint16_t)strlen(id) : 0U;
  e.class_str         = cls;
  e.class_len         = (cls != nullptr) ? (uint16_t)strlen(cls) : 0U;
  return e;
}

/** @brief The empty inline declaration (nothing set). */
static ra8_css_style_t no_inline(void)
{
  return (ra8_css_style_t){};
}

/**
 * @test test_parse_props
 * @brief Each v1 property parses into the right set bit + value.
 */
static void test_parse_props(void)
{
  TEST_BEGIN("css parse properties");
  load("p { font-weight: bold; font-style: italic; text-decoration: underline; "
       "text-align: justify; }");
  TEST_ASSERT_EQ(1, s_sheet.rule_count);
  const ra8_css_style_t d = s_sheet.rules[0].decl;
  TEST_ASSERT((d.set & (uint8_t)k_ra8_css_set_bold) != 0U);
  TEST_ASSERT((d.set & (uint8_t)k_ra8_css_set_italic) != 0U);
  TEST_ASSERT((d.set & (uint8_t)k_ra8_css_set_underline) != 0U);
  TEST_ASSERT((d.set & (uint8_t)k_ra8_css_set_align) != 0U);
  TEST_ASSERT((d.style & (uint8_t)k_ra8_reflow_style_bold) != 0U);
  TEST_ASSERT((d.style & (uint8_t)k_ra8_reflow_style_italic) != 0U);
  TEST_ASSERT((d.style & (uint8_t)k_ra8_reflow_style_underline) != 0U);
  TEST_ASSERT_EQ(k_ra8_reflow_align_justify, d.align);
  TEST_ASSERT_EQ(k_ra8_reflow_tag_p, s_sheet.rules[0].sel_tag); /* type only */
  TEST_ASSERT_EQ(0, s_sheet.rules[0].class_len);
  TEST_ASSERT_EQ(0, s_sheet.rules[0].id_len);
  TEST_END("css parse properties");
}

/**
 * @test test_parse_normal_resets
 * @brief `normal` / `none` declare the property but clear the value bit.
 */
static void test_parse_normal_resets(void)
{
  TEST_BEGIN("css parse normal/none resets");
  load("em { font-weight: normal; font-style: normal; text-decoration: none; }");
  const ra8_css_style_t d = s_sheet.rules[0].decl;
  TEST_ASSERT((d.set & (uint8_t)k_ra8_css_set_bold) != 0U);        /* declared */
  TEST_ASSERT((d.style & (uint8_t)k_ra8_reflow_style_bold) == 0U); /* but off  */
  TEST_ASSERT((d.set & (uint8_t)k_ra8_css_set_italic) != 0U);
  TEST_ASSERT((d.style & (uint8_t)k_ra8_reflow_style_italic) == 0U);
  TEST_ASSERT((d.style & (uint8_t)k_ra8_reflow_style_underline) == 0U);
  TEST_END("css parse normal/none resets");
}

/**
 * @test test_parse_selectors
 * @brief Universal / class / id selectors, comma groups, comment skip.
 */
static void test_parse_selectors(void)
{
  TEST_BEGIN("css parse selectors");
  load("/* hello */ * { font-style: italic; }\n"
       ".note , #lead { font-weight: bold; }");
  TEST_ASSERT_EQ(3, s_sheet.rule_count); /* * , .note , #lead */
  /* universal: no type / class / id constraint */
  TEST_ASSERT_EQ(k_ra8_reflow_tag_unknown, s_sheet.rules[0].sel_tag);
  TEST_ASSERT_EQ(0, s_sheet.rules[0].class_len);
  TEST_ASSERT_EQ(0, s_sheet.rules[0].id_len);
  /* .note: class constraint only */
  TEST_ASSERT(s_sheet.rules[1].class_len > 0U);
  TEST_ASSERT_EQ(0, s_sheet.rules[1].id_len);
  /* #lead: id constraint only */
  TEST_ASSERT(s_sheet.rules[2].id_len > 0U);
  TEST_ASSERT_EQ(0, s_sheet.rules[2].class_len);
  TEST_END("css parse selectors");
}

/**
 * @test test_parse_unsupported_skipped
 * @brief Multi-class / unknown-tag selectors are dropped, not errored. (Descendant
 *        selectors ARE supported now -- see test_descendant.)
 */
static void test_parse_unsupported_skipped(void)
{
  TEST_BEGIN("css unsupported selectors skipped");
  load(".a.b { color: red; }"        /* two classes -> skip */
       "div { text-align: center; }" /* unknown tag -> skip */
       "h1 { text-align: right; }"); /* OK                  */
  TEST_ASSERT_EQ(1, s_sheet.rule_count);
  TEST_ASSERT_EQ(k_ra8_reflow_tag_h1, s_sheet.rules[0].sel_tag);
  TEST_END("css unsupported selectors skipped");
}

/**
 * @test test_inline
 * @brief Inline `style=""` parses without a selector or braces.
 */
static void test_inline(void)
{
  TEST_BEGIN("css inline declaration");
  ra8_css_style_t d = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_css_parse_inline("font-weight:bold;text-align:center",
                                      (uint32_t)strlen("font-weight:bold;text-align:center"),
                                      &d));
  TEST_ASSERT((d.set & (uint8_t)k_ra8_css_set_bold) != 0U);
  TEST_ASSERT((d.style & (uint8_t)k_ra8_reflow_style_bold) != 0U);
  TEST_ASSERT_EQ(k_ra8_reflow_align_center, d.align);
  TEST_END("css inline declaration");
}

/**
 * @test test_match_mcdc
 * @brief Selector matching, with MC/DC for the id 3-condition AND.
 *
 * @par MC/DC:
 * Decision (id match): `(el->id != NULL) && (el->id_len == name_len) &&
 * (memcmp == 0)` (3 conditions, AND; N+1 = 4 vectors):
 *  - id="lead",len=4,bytes==      -> true  (control: all true)
 *  - id=NULL                      -> false (varies cond 1)
 *  - id="leadX",len=5             -> false (varies cond 2)
 *  - id="xxxx",len=4,bytes!=      -> false (varies cond 3)
 * Vectors 1+2 isolate cond 1; 1+3 cond 2; 1+4 cond 3.
 */
static void test_match_mcdc(void)
{
  TEST_BEGIN("css match (id 3-AND MC/DC)");
  load("#lead { font-weight: bold; }");
  const ra8_css_rule_t* r  = &s_sheet.rules[0];
  ra8_css_element_t     v1 = elem(k_ra8_reflow_tag_p, "lead", nullptr);
  ra8_css_element_t     v2 = elem(k_ra8_reflow_tag_p, nullptr, nullptr);
  ra8_css_element_t     v3 = elem(k_ra8_reflow_tag_p, "leadX", nullptr);
  ra8_css_element_t     v4 = elem(k_ra8_reflow_tag_p, "xxxx", nullptr);
  TEST_ASSERT(ra8_css_rule_matches(r, &v1, &s_sheet));  /* all true    */
  TEST_ASSERT(!ra8_css_rule_matches(r, &v2, &s_sheet)); /* cond1 false */
  TEST_ASSERT(!ra8_css_rule_matches(r, &v3, &s_sheet)); /* cond2 false */
  TEST_ASSERT(!ra8_css_rule_matches(r, &v4, &s_sheet)); /* cond3 false */
  TEST_END("css match (id 3-AND MC/DC)");
}

/**
 * @test test_match_class_universal_type
 * @brief Class-list membership, universal-always, type-by-tag.
 */
static void test_match_class_universal_type(void)
{
  TEST_BEGIN("css match class/universal/type");
  load(".note { font-weight: bold; } * { font-style: italic; } h2 { text-align: center; }");
  const ra8_css_rule_t* rc = &s_sheet.rules[0];
  const ra8_css_rule_t* ru = &s_sheet.rules[1];
  const ra8_css_rule_t* rt = &s_sheet.rules[2];
  ra8_css_element_t     a  = elem(k_ra8_reflow_tag_p, nullptr, "intro note last");
  ra8_css_element_t     b  = elem(k_ra8_reflow_tag_p, nullptr, "introduction");
  ra8_css_element_t     h2 = elem(k_ra8_reflow_tag_h2, nullptr, nullptr);
  TEST_ASSERT(ra8_css_rule_matches(rc, &a, &s_sheet));  /* "note" present       */
  TEST_ASSERT(!ra8_css_rule_matches(rc, &b, &s_sheet)); /* substring only -> no */
  TEST_ASSERT(ra8_css_rule_matches(ru, &b, &s_sheet));  /* universal            */
  TEST_ASSERT(ra8_css_rule_matches(rt, &h2, &s_sheet)); /* h2 type match        */
  TEST_ASSERT(!ra8_css_rule_matches(rt, &a, &s_sheet)); /* p != h2              */
  TEST_END("css match class/universal/type");
}

/**
 * @test test_cascade_specificity
 * @brief id > class > type > universal > inherited; inline beats all.
 */
static void test_cascade_specificity(void)
{
  TEST_BEGIN("css cascade specificity");
  load("* { text-align: left; }"
       "p { text-align: right; }"
       ".c { text-align: center; }"
       "#i { text-align: justify; }");
  ra8_css_element_t el = elem(k_ra8_reflow_tag_p, "i", "c");
  ra8_css_style_t   r  = ra8_css_cascade(&s_sheet, &el, (ra8_css_style_t){}, no_inline());
  TEST_ASSERT_EQ(k_ra8_reflow_align_justify, r.align); /* #i wins */

  ra8_css_element_t el2 = elem(k_ra8_reflow_tag_p, nullptr, "c");
  ra8_css_style_t   r2  = ra8_css_cascade(&s_sheet, &el2, (ra8_css_style_t){}, no_inline());
  TEST_ASSERT_EQ(k_ra8_reflow_align_center, r2.align); /* .c beats p, * */

  ra8_css_style_t inl = {};
  inl.set             = (uint8_t)k_ra8_css_set_align;
  inl.align           = (uint8_t)k_ra8_reflow_align_left;
  ra8_css_style_t r3  = ra8_css_cascade(&s_sheet, &el, (ra8_css_style_t){}, inl);
  TEST_ASSERT_EQ(k_ra8_reflow_align_left, r3.align); /* inline beats #i */
  TEST_END("css cascade specificity");
}

/**
 * @test test_cascade_source_order
 * @brief Equal-specificity ties go to the later rule.
 */
static void test_cascade_source_order(void)
{
  TEST_BEGIN("css cascade source order");
  load("p { text-align: right; } p { text-align: center; }");
  ra8_css_element_t el = elem(k_ra8_reflow_tag_p, nullptr, nullptr);
  ra8_css_style_t   r  = ra8_css_cascade(&s_sheet, &el, (ra8_css_style_t){}, no_inline());
  TEST_ASSERT_EQ(k_ra8_reflow_align_center, r.align); /* later wins */
  TEST_END("css cascade source order");
}

/**
 * @test test_cascade_inheritance
 * @brief Inheritable props flow from parent; a child rule overrides; normal resets.
 */
static void test_cascade_inheritance(void)
{
  TEST_BEGIN("css cascade inheritance");
  load("em { font-weight: normal; }");
  /* Parent computed: bold on, italic on (inherited into the child). */
  ra8_css_style_t parent = {};
  parent.set             = (uint8_t)((uint8_t)k_ra8_css_set_bold | (uint8_t)k_ra8_css_set_italic);
  parent.style = (uint8_t)((uint8_t)k_ra8_reflow_style_bold | (uint8_t)k_ra8_reflow_style_italic);

  ra8_css_element_t child = elem(k_ra8_reflow_tag_em, nullptr, nullptr);
  ra8_css_style_t   r     = ra8_css_cascade(&s_sheet, &child, parent, no_inline());
  /* em rule turns bold OFF; italic still inherited ON. */
  TEST_ASSERT((r.set & (uint8_t)k_ra8_css_set_bold) != 0U);
  TEST_ASSERT((r.style & (uint8_t)k_ra8_reflow_style_bold) == 0U);
  TEST_ASSERT((r.style & (uint8_t)k_ra8_reflow_style_italic) != 0U);
  TEST_END("css cascade inheritance");
}

/**
 * @test test_cascade_null_mcdc
 * @brief Cascade guard returns the inherited style on a NULL sheet or element.
 *
 * @par MC/DC:
 * Decision: `(sheet == NULL) || (el == NULL)` (2 conditions, OR; N+1 = 3):
 *  - sheet!=NULL, el!=NULL -> false (control: cascade runs)
 *  - sheet==NULL, el!=NULL -> true  (varies cond 1)
 *  - sheet!=NULL, el==NULL -> true  (varies cond 2)
 */
static void test_cascade_null_mcdc(void)
{
  TEST_BEGIN("css cascade null guard MC/DC");
  load("p { text-align: center; }");
  ra8_css_element_t el  = elem(k_ra8_reflow_tag_p, nullptr, nullptr);
  ra8_css_style_t   inh = {};
  inh.set               = (uint8_t)k_ra8_css_set_align;
  inh.align             = (uint8_t)k_ra8_reflow_align_right;
  /* both non-NULL -> cascade runs, p rule wins -> center */
  ra8_css_style_t r0 = ra8_css_cascade(&s_sheet, &el, inh, no_inline());
  TEST_ASSERT_EQ(k_ra8_reflow_align_center, r0.align);
  /* sheet NULL -> returns inherited (right) */
  ra8_css_style_t r1 = ra8_css_cascade(nullptr, &el, inh, no_inline());
  TEST_ASSERT_EQ(k_ra8_reflow_align_right, r1.align);
  /* el NULL -> returns inherited (right) */
  ra8_css_style_t r2 = ra8_css_cascade(&s_sheet, nullptr, inh, no_inline());
  TEST_ASSERT_EQ(k_ra8_reflow_align_right, r2.align);
  TEST_END("css cascade null guard MC/DC");
}

/**
 * @test test_parse_color
 * @brief `color` parses #rrggbb / #rgb / named keywords; bad values are ignored.
 */
static void test_parse_color(void)
{
  TEST_BEGIN("css parse color");
  load("p { color: #ff8800; } .a { color: #abc; } #b { color: navy; } em { color: bogus; }");
  /* p: #ff8800 */
  TEST_ASSERT((s_sheet.rules[0].decl.set & (uint8_t)k_ra8_css_set_color) != 0U);
  TEST_ASSERT_EQ(0xFF8800, s_sheet.rules[0].decl.color);
  /* .a: #abc -> 0xaabbcc */
  TEST_ASSERT_EQ(0xAABBCC, s_sheet.rules[1].decl.color);
  /* #b: navy -> 0x000080 */
  TEST_ASSERT_EQ(0x000080, s_sheet.rules[2].decl.color);
  /* em: bogus -> color NOT set (invalid value ignored) */
  TEST_ASSERT((s_sheet.rules[3].decl.set & (uint8_t)k_ra8_css_set_color) == 0U);
  TEST_END("css parse color");
}

/**
 * @test test_cascade_color
 * @brief Colour cascades by specificity and inherits parent -> child.
 */
static void test_cascade_color(void)
{
  TEST_BEGIN("css cascade color");
  load("p { color: red; } .hi { color: #00ff00; }");
  /* .hi (class) beats p (type) -> green */
  ra8_css_element_t el = elem(k_ra8_reflow_tag_p, nullptr, "hi");
  ra8_css_style_t   r  = ra8_css_cascade(&s_sheet, &el, (ra8_css_style_t){}, no_inline());
  TEST_ASSERT((r.set & (uint8_t)k_ra8_css_set_color) != 0U);
  TEST_ASSERT_EQ(0x00FF00, r.color);
  /* a child with no colour rule inherits the parent's colour */
  ra8_css_style_t parent = {};
  parent.set             = (uint8_t)k_ra8_css_set_color;
  parent.color           = k_css_parent_color;
  ra8_css_element_t kid  = elem(k_ra8_reflow_tag_strong, nullptr, nullptr);
  ra8_css_style_t   rk   = ra8_css_cascade(&s_sheet, &kid, parent, no_inline());
  TEST_ASSERT_EQ(0x123456, rk.color);
  TEST_END("css cascade color");
}

/** @brief Parse an inline `font-size: <value>` and return the declaration. */
static ra8_css_style_t fs(const char* value)
{
  char buf[k_css_decl_cap] = {};
  (void)snprintf(buf, sizeof buf, "font-size:%s", value);
  ra8_css_style_t d = {};
  (void)ra8_css_parse_inline(buf, (uint32_t)strlen(buf), &d);
  return d;
}

/**
 * @test test_parse_fontsize
 * @brief font-size parses px/%/em (incl. fractional) with MC/DC for unit dispatch.
 *
 * @par MC/DC:
 * Three 2-condition AND unit dispatches in priv_parse_fontsize (each N+1 = 3):
 *   px:  `(rem == k_priv_fs_ulen) && ci_eq("px")`
 *   pct: `(rem == 1)              && (s[i] == '%')`
 *   em:  `(rem == k_priv_fs_ulen) && ci_eq("em")`
 * plus the fractional-digit loop's `fd < k_priv_fs_frac` upper bound:
 *  - "16px"    -> px=16     (px both true)
 *  - "16pt"    -> unset     (px cond2 false: rem==2, not "px"; em cond2 false)
 *  - "120%"    -> pct=120   (pct both true)
 *  - "9x"      -> unset     (pct cond2 false: rem==1, char != '%')
 *  - "16"      -> unset     (all cond1 false: rem==0, no unit)
 *  - "1.5em"   -> pct=150   (em both true; px cond2 false)
 *  - "1.5pt"   -> unset     (em cond2 false: rem==2, not "em")
 *  - "1.567em" -> pct=156   (fractional loop stops via fd>=k_priv_fs_frac)
 *  - "99999px" -> px=9999   (integer accumulation saturates to k_priv_fs_max)
 *  - "huge"    -> unset     (no digits)
 */
static void test_parse_fontsize(void)
{
  TEST_BEGIN("css parse font-size");
  const uint8_t   fset = (uint8_t)k_ra8_css_set_fontsize;
  ra8_css_style_t d;
  d = fs("16px");
  TEST_ASSERT((d.set & fset) != 0U);
  TEST_ASSERT_EQ(16, d.font_val);
  TEST_ASSERT_EQ(k_ra8_css_font_px, d.font_unit);
  TEST_ASSERT((fs("16pt").set & fset) == 0U); /* rem==2, not "px"/"em" */
  d = fs("120%");
  TEST_ASSERT_EQ(120, d.font_val);
  TEST_ASSERT_EQ(k_ra8_css_font_pct, d.font_unit);
  TEST_ASSERT((fs("9x").set & fset) == 0U); /* rem==1, char != '%' */
  TEST_ASSERT((fs("16").set & fset) == 0U); /* rem==0, no unit     */
  d = fs("1.5em");
  TEST_ASSERT_EQ(150, d.font_val); /* 1.5em -> 150% */
  TEST_ASSERT_EQ(k_ra8_css_font_pct, d.font_unit);
  TEST_ASSERT((fs("1.5pt").set & fset) == 0U); /* rem==2, not "em" */
  d = fs("1.567em");
  TEST_ASSERT_EQ(156, d.font_val); /* 3rd frac digit dropped (fd cap) */
  d = fs("99999px");
  TEST_ASSERT_EQ(9999, d.font_val);           /* saturates to k_priv_fs_max */
  TEST_ASSERT((fs("huge").set & fset) == 0U); /* no digits                  */
  TEST_END("css parse font-size");
}

/**
 * @test test_decoration_line_alias
 * @brief `text-decoration-line` is accepted alongside `text-decoration`.
 *
 * @par MC/DC:
 * Decision: `ci_eq("text-decoration") || ci_eq("text-decoration-line")` (OR; N+1=3):
 *  - "text-decoration"      -> true  (cond1 true)
 *  - "text-decoration-line" -> true  (cond1 false, cond2 true)
 *  - "text-transform"       -> false (both false: property ignored)
 */
static void test_decoration_line_alias(void)
{
  TEST_BEGIN("css text-decoration-line alias MC/DC");
  ra8_css_style_t d1 = {};
  (void)ra8_css_parse_inline("text-decoration:underline",
                             (uint32_t)strlen("text-decoration:underline"),
                             &d1);
  TEST_ASSERT((d1.style & (uint8_t)k_ra8_reflow_style_underline) != 0U); /* cond1 */
  ra8_css_style_t d2 = {};
  (void)ra8_css_parse_inline("text-decoration-line:underline",
                             (uint32_t)strlen("text-decoration-line:underline"),
                             &d2);
  TEST_ASSERT((d2.style & (uint8_t)k_ra8_reflow_style_underline) != 0U); /* cond2 */
  ra8_css_style_t d3 = {};
  (void)ra8_css_parse_inline("text-transform:upper", (uint32_t)strlen("text-transform:upper"), &d3);
  TEST_ASSERT(d3.set == 0U); /* neither -> ignored */
  TEST_END("css text-decoration-line alias MC/DC");
}

/**
 * @test test_cascade_fontsize
 * @brief font-size cascades by specificity (class beats type); raw value carried.
 */
static void test_cascade_fontsize(void)
{
  TEST_BEGIN("css cascade font-size");
  load("p { font-size: 14px; } .big { font-size: 200%; }");
  ra8_css_element_t el = elem(k_ra8_reflow_tag_p, nullptr, "big");
  ra8_css_style_t   r  = ra8_css_cascade(&s_sheet, &el, (ra8_css_style_t){}, no_inline());
  TEST_ASSERT((r.set & (uint8_t)k_ra8_css_set_fontsize) != 0U);
  TEST_ASSERT_EQ(200, r.font_val); /* .big wins */
  TEST_ASSERT_EQ(k_ra8_css_font_pct, r.font_unit);
  ra8_css_element_t el2 = elem(k_ra8_reflow_tag_p, nullptr, nullptr);
  ra8_css_style_t   r2  = ra8_css_cascade(&s_sheet, &el2, (ra8_css_style_t){}, no_inline());
  TEST_ASSERT_EQ(14, r2.font_val);
  TEST_ASSERT_EQ(k_ra8_css_font_px, r2.font_unit);
  TEST_END("css cascade font-size");
}

/**
 * @test test_display
 * @brief `display:none` parses + cascades; other display values are visible.
 */
static void test_display(void)
{
  TEST_BEGIN("css display");
  load(".h { display: none; } .v { display: block; } p { display: none; }");
  /* .h: display declared, none */
  TEST_ASSERT((s_sheet.rules[0].decl.set & (uint8_t)k_ra8_css_set_display) != 0U);
  TEST_ASSERT_EQ(1, s_sheet.rules[0].decl.display);
  /* .v: display declared, visible (block != none) */
  TEST_ASSERT((s_sheet.rules[1].decl.set & (uint8_t)k_ra8_css_set_display) != 0U);
  TEST_ASSERT_EQ(0, s_sheet.rules[1].decl.display);
  /* cascade: a matching p{display:none} hides; a non-matching element does not */
  ra8_css_element_t p  = elem(k_ra8_reflow_tag_p, nullptr, nullptr);
  ra8_css_style_t   rp = ra8_css_cascade(&s_sheet, &p, (ra8_css_style_t){}, no_inline());
  TEST_ASSERT((rp.set & (uint8_t)k_ra8_css_set_display) != 0U);
  TEST_ASSERT_EQ(1, rp.display);
  ra8_css_element_t h1  = elem(k_ra8_reflow_tag_h1, nullptr, nullptr);
  ra8_css_style_t   rh1 = ra8_css_cascade(&s_sheet, &h1, (ra8_css_style_t){}, no_inline());
  TEST_ASSERT((rh1.set & (uint8_t)k_ra8_css_set_display) == 0U); /* no rule -> unset */
  TEST_END("css display");
}

/**
 * @test test_compound_selectors
 * @brief `type.class` / `type#id` match all parts; specificity sums; `.a.b` skipped.
 */
static void test_compound_selectors(void)
{
  TEST_BEGIN("css compound selectors");
  load("p.note { color: #ff0000; }"                /* type + class           */
       "a#x { text-align: center; }"               /* type + id              */
       ".a.b { color: #0000ff; }"                  /* two classes -> dropped */
       "p { color: #00ff00; }");                   /* plain type             */
  TEST_ASSERT_EQ(3, s_sheet.rule_count);           /* .a.b dropped           */
  const ra8_css_rule_t* rnote = &s_sheet.rules[0]; /* p.note                 */
  TEST_ASSERT_EQ(k_ra8_reflow_tag_p, rnote->sel_tag);
  TEST_ASSERT(rnote->class_len > 0U);

  /* p.note matches only when BOTH the tag and the class match. */
  ra8_css_element_t p_note  = elem(k_ra8_reflow_tag_p, nullptr, "intro note");
  ra8_css_element_t p_plain = elem(k_ra8_reflow_tag_p, nullptr, nullptr);
  ra8_css_element_t h1_note = elem(k_ra8_reflow_tag_h1, nullptr, "note");
  TEST_ASSERT(ra8_css_rule_matches(rnote, &p_note, &s_sheet));   /* tag + class      */
  TEST_ASSERT(!ra8_css_rule_matches(rnote, &p_plain, &s_sheet)); /* tag, no class    */
  TEST_ASSERT(!ra8_css_rule_matches(rnote, &h1_note, &s_sheet)); /* class, wrong tag */

  /* Specificity: p.note (type+class) beats the plain p rule. */
  ra8_css_style_t r1 = ra8_css_cascade(&s_sheet, &p_note, (ra8_css_style_t){}, no_inline());
  TEST_ASSERT_EQ(0xFF0000, r1.color); /* p.note wins */
  ra8_css_style_t r2 = ra8_css_cascade(&s_sheet, &p_plain, (ra8_css_style_t){}, no_inline());
  TEST_ASSERT_EQ(0x00FF00, r2.color); /* only p matches */
  TEST_END("css compound selectors");
}

/**
 * @test test_resolve_skip_mcdc
 * @brief MC/DC for the priv_resolve per-rule skip guard (driven via cascade).
 *
 * @par MC/DC:
 * Decision: `!matched[i] || ((rule.decl.set & setbit) == 0)` (2 conditions, OR;
 * N+1 = 3 vectors), observed by resolving the ALIGN property of a `<p>`:
 *  - V1: a `p` rule that sets text-align       -> false (participates -> center)
 *  - V2: an `h1` rule that sets text-align     -> true  (skipped: !matched alone)
 *  - V3: a `p` rule that sets only font-weight -> true  (skipped: set&align==0)
 * V1 vs V2 isolates condition 1 (matched); V1 vs V3 isolates condition 2.
 */
static void test_resolve_skip_mcdc(void)
{
  TEST_BEGIN("css resolve skip-guard MC/DC");
  ra8_css_element_t p = elem(k_ra8_reflow_tag_p, nullptr, nullptr);
  load("p { text-align: center; }"); /* V1: matches + sets align -> participates */
  ra8_css_style_t r1 = ra8_css_cascade(&s_sheet, &p, (ra8_css_style_t){}, no_inline());
  TEST_ASSERT((r1.set & (uint8_t)k_ra8_css_set_align) != 0U);
  TEST_ASSERT_EQ(k_ra8_reflow_align_center, r1.align);
  load("h1 { text-align: center; }"); /* V2: sets align but does NOT match -> skip */
  ra8_css_style_t r2 = ra8_css_cascade(&s_sheet, &p, (ra8_css_style_t){}, no_inline());
  TEST_ASSERT((r2.set & (uint8_t)k_ra8_css_set_align) == 0U);
  load("p { font-weight: bold; }"); /* V3: matches but does NOT set align -> skip */
  ra8_css_style_t r3 = ra8_css_cascade(&s_sheet, &p, (ra8_css_style_t){}, no_inline());
  TEST_ASSERT((r3.set & (uint8_t)k_ra8_css_set_align) == 0U);
  TEST_END("css resolve skip-guard MC/DC");
}

/**
 * @test test_resolve_winner_mcdc
 * @brief MC/DC for the priv_resolve winner-selection (driven via cascade).
 *
 * @par MC/DC:
 * Decision: `(!have) || (rank > best_rank) ||
 *            ((rank == best_rank) && (order >= best_order))` (3 conditions;
 * N+1 = 4 vectors), resolving ALIGN; rules are processed in source order:
 *  - V1 control: higher-specificity rule first, lower after -> all three false
 *    (no override): `#i{center} p{right}` on `<p id=i>` -> center.
 *  - V2 (!have):  first matching setter wins: `p{right}` on `<p>` -> right.
 *  - V3 (rank>best_rank): higher rule after lower overrides:
 *    `p{right} #i{center}` on `<p id=i>` -> center.
 *  - V4 (rank==best && order>=best_order): later same-specificity wins:
 *    `p{right} p{center}` on `<p>` -> center.
 */
static void test_resolve_winner_mcdc(void)
{
  TEST_BEGIN("css resolve winner-selection MC/DC");
  ra8_css_element_t pid = elem(k_ra8_reflow_tag_p, "i", nullptr);
  ra8_css_element_t p   = elem(k_ra8_reflow_tag_p, nullptr, nullptr);
  load("#i { text-align: center; } p { text-align: right; }"); /* V1 control */
  ra8_css_style_t v1 = ra8_css_cascade(&s_sheet, &pid, (ra8_css_style_t){}, no_inline());
  TEST_ASSERT_EQ(k_ra8_reflow_align_center, v1.align);
  load("p { text-align: right; }"); /* V2: !have */
  ra8_css_style_t v2 = ra8_css_cascade(&s_sheet, &p, (ra8_css_style_t){}, no_inline());
  TEST_ASSERT_EQ(k_ra8_reflow_align_right, v2.align);
  load("p { text-align: right; } #i { text-align: center; }"); /* V3: rank>best */
  ra8_css_style_t v3 = ra8_css_cascade(&s_sheet, &pid, (ra8_css_style_t){}, no_inline());
  TEST_ASSERT_EQ(k_ra8_reflow_align_center, v3.align);
  load("p { text-align: right; } p { text-align: center; }"); /* V4: order tiebreak */
  ra8_css_style_t v4 = ra8_css_cascade(&s_sheet, &p, (ra8_css_style_t){}, no_inline());
  TEST_ASSERT_EQ(k_ra8_reflow_align_center, v4.align);
  TEST_END("css resolve winner-selection MC/DC");
}

/**
 * @test test_null_guards
 * @brief NULL arguments are rejected by the parse / reset API.
 */
static void test_null_guards(void)
{
  TEST_BEGIN("css null guards");
  ra8_css_style_t d = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_css_sheet_reset(nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_css_parse(nullptr, "p{}", 3U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_css_parse(&s_sheet, nullptr, 3U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_css_parse_inline(nullptr, 0U, &d));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_css_parse_inline("x", 1U, nullptr));
  TEST_ASSERT(!ra8_css_rule_matches(nullptr, nullptr, nullptr));
  TEST_END("css null guards");
}

/**
 * @test test_fontface_parse
 * @brief `@font-face` at-rules populate the face table; they are not rules.
 */
static void test_fontface_parse(void)
{
  TEST_BEGIN("css @font-face parse");
  load("@font-face{font-family:\"Body\";src:url(fonts/Body.ttf)}"
       "@font-face{font-family:Body;font-weight:bold;src:url('fonts/Body-Bold.ttf')}"
       "@font-face{font-family:Body;font-style:italic;src:url(fonts/Body-It.ttf)}"
       "p{color:red}");
  TEST_ASSERT_EQ(3, s_sheet.face_count);
  TEST_ASSERT_EQ(1, s_sheet.rule_count); /* @font-face blocks are not rules */
  TEST_ASSERT_EQ(0, s_sheet.faces[0].weight_bold);
  TEST_ASSERT_EQ(0, s_sheet.faces[0].style_italic);
  const char* src  = nullptr;
  uint16_t    slen = 0U;
  TEST_ASSERT(ra8_css_face_src(&s_sheet, 0U, &src, &slen));
  TEST_ASSERT((slen == (uint16_t)strlen("fonts/Body.ttf")) &&
              (memcmp(src, "fonts/Body.ttf", slen) == 0));
  TEST_ASSERT_EQ(1, s_sheet.faces[1].weight_bold);
  TEST_ASSERT(ra8_css_face_src(&s_sheet, 1U, &src, &slen)); /* quotes stripped from url() */
  TEST_ASSERT((slen == (uint16_t)strlen("fonts/Body-Bold.ttf")) &&
              (memcmp(src, "fonts/Body-Bold.ttf", slen) == 0));
  TEST_ASSERT_EQ(1, s_sheet.faces[2].style_italic);
  TEST_END("css @font-face parse");
}

/**
 * @test test_fontface_cascade
 * @brief `font-family` resolves on a rule and inherits to a child element.
 */
static void test_fontface_cascade(void)
{
  TEST_BEGIN("css font-family cascade");
  load("@font-face{font-family:Body;src:url(b.ttf)}p{font-family:Body}");
  ra8_css_element_t p  = elem(k_ra8_reflow_tag_p, nullptr, nullptr);
  ra8_css_style_t   ps = ra8_css_cascade(&s_sheet, &p, no_inline(), no_inline());
  TEST_ASSERT((ps.set & (uint8_t)k_ra8_css_set_family) != 0U);
  TEST_ASSERT((ps.family_len == 4U) && (memcmp(&s_sheet.names[ps.family_off], "Body", 4U) == 0));
  ra8_css_element_t em = elem(k_ra8_reflow_tag_em, nullptr, nullptr);
  ra8_css_style_t   es = ra8_css_cascade(&s_sheet, &em, ps, no_inline());
  TEST_ASSERT((es.set & (uint8_t)k_ra8_css_set_family) != 0U); /* inherited from parent */
  TEST_ASSERT((es.family_len == 4U) && (memcmp(&s_sheet.names[es.family_off], "Body", 4U) == 0));
  TEST_END("css font-family cascade");
}

/**
 * @test test_fontface_match_mcdc
 * @brief Face selection: exact (family,weight,style), regular fallback, no-match.
 *
 * @par MC/DC:
 * Decision: `((weight_bold!=0)==want_bold) && ((style_italic!=0)==want_italic)`
 * tested against one bold+italic face "BI" (weight_bold=1, style_italic=1):
 * - V1: want_bold=1, want_italic=1 -> A=T, B=T -> true  (returns face 0)
 * - V2: want_bold=0, want_italic=1 -> A=F, B=T -> false (no exact -> no-face)
 * - V3: want_bold=1, want_italic=0 -> A=T, B=F -> false (no exact -> no-face)
 * V1+V2 prove the weight condition independently flips the outcome; V1+V3 the
 * style condition. N+1 = 3 vectors for the 2-condition decision.
 */
static void test_fontface_match_mcdc(void)
{
  TEST_BEGIN("css @font-face match MC/DC");
  load("@font-face{font-family:BI;font-weight:bold;font-style:italic;src:url(bi.ttf)}");
  TEST_ASSERT_EQ(0, ra8_css_match_face(&s_sheet, "BI", 2U, true, true)); /* V1 */
  TEST_ASSERT_EQ(k_ra8_css_no_face,
                 ra8_css_match_face(&s_sheet, "BI", 2U, false, true)); /* V2: vary weight */
  TEST_ASSERT_EQ(k_ra8_css_no_face,
                 ra8_css_match_face(&s_sheet, "BI", 2U, true, false)); /* V3: vary style */
  load("@font-face{font-family:Body;src:url(r.ttf)}"
       "@font-face{font-family:Body;font-weight:bold;src:url(b.ttf)}");
  TEST_ASSERT_EQ(0, ra8_css_match_face(&s_sheet, "Body", 4U, false, false)); /* exact regular    */
  TEST_ASSERT_EQ(1, ra8_css_match_face(&s_sheet, "Body", 4U, true, false));  /* exact bold       */
  TEST_ASSERT_EQ(0, ra8_css_match_face(&s_sheet, "Body", 4U, true, true));   /* fallback regular */
  TEST_ASSERT_EQ(0, ra8_css_match_face(&s_sheet, "body", 4U, false, false)); /* case-insens.     */
  TEST_ASSERT_EQ(k_ra8_css_no_face,
                 ra8_css_match_face(&s_sheet, "Other", 5U, false, false)); /* no such family */
  TEST_END("css @font-face match MC/DC");
}

/**
 * @test test_fontface_null_guards
 * @brief match / face_src reject NULLs, a zero family, and out-of-range indices.
 */
static void test_fontface_null_guards(void)
{
  TEST_BEGIN("css @font-face null guards");
  load("@font-face{font-family:Body;src:url(b.ttf)}");
  const char* src  = nullptr;
  uint16_t    slen = 0U;
  TEST_ASSERT_EQ(k_ra8_css_no_face, ra8_css_match_face(nullptr, "Body", 4U, false, false));
  TEST_ASSERT_EQ(k_ra8_css_no_face, ra8_css_match_face(&s_sheet, nullptr, 4U, false, false));
  TEST_ASSERT_EQ(k_ra8_css_no_face, ra8_css_match_face(&s_sheet, "Body", 0U, false, false));
  TEST_ASSERT(!ra8_css_face_src(nullptr, 0U, &src, &slen));
  TEST_ASSERT(!ra8_css_face_src(&s_sheet, 99U, &src, &slen));
  TEST_ASSERT(!ra8_css_face_src(&s_sheet, 0U, nullptr, &slen));
  TEST_END("css @font-face null guards");
}

/**
 * @test test_descendant
 * @brief Descendant selectors: class + tag ancestors, no-match, specificity,
 *        and ancestor overflow.
 *
 * @par MC/DC:
 * Decision (`.chapter p` matches): `subject_matches(p) && ancestor_has(.chapter)`
 * (2 conditions, AND; N+1 = 3 vectors):
 *  - el=p,  ancestor=.chapter -> true  (control: both true)
 *  - el=h1, ancestor=.chapter -> false (varies the subject condition)
 *  - el=p,  ancestor=none     -> false (varies the ancestor condition)
 */
static void test_descendant(void)
{
  TEST_BEGIN("css descendant selectors");
  load(".chapter p { font-weight: bold; }\n"
       "p { font-style: italic; }");
  TEST_ASSERT_EQ(2, s_sheet.rule_count);
  TEST_ASSERT_EQ(1, s_sheet.rules[0].anc_count);                /* one ancestor part */
  TEST_ASSERT(s_sheet.rules[0].anc[0].class_len > 0U);          /* .chapter          */
  TEST_ASSERT_EQ(k_ra8_reflow_tag_p, s_sheet.rules[0].sel_tag); /* subject p         */

  const ra8_css_style_t   inh     = {};
  const ra8_css_style_t   inl     = no_inline();
  const ra8_css_element_t p       = elem(k_ra8_reflow_tag_p, nullptr, nullptr);
  const ra8_css_element_t h1      = elem(k_ra8_reflow_tag_h1, nullptr, nullptr);
  const ra8_css_element_t chap[1] = {elem(k_ra8_reflow_tag_h1, nullptr, "chapter")};

  /* V1: p under .chapter -> `.chapter p` (bold) AND `p` (italic) both apply. */
  const ra8_css_style_t c1 = ra8_css_cascade_ctx(&s_sheet, &p, inh, inl, chap, 1U);
  TEST_ASSERT((c1.style & (uint8_t)k_ra8_reflow_style_bold) != 0U);
  TEST_ASSERT((c1.style & (uint8_t)k_ra8_reflow_style_italic) != 0U);

  /* V3: p with no ancestor -> descendant does NOT match (italic only). */
  const ra8_css_style_t c3 = ra8_css_cascade_ctx(&s_sheet, &p, inh, inl, nullptr, 0U);
  TEST_ASSERT((c3.style & (uint8_t)k_ra8_reflow_style_bold) == 0U);
  TEST_ASSERT((c3.style & (uint8_t)k_ra8_reflow_style_italic) != 0U);

  /* V2: h1 (not the subject) under .chapter -> no rule matches it. */
  const ra8_css_style_t c2 = ra8_css_cascade_ctx(&s_sheet, &h1, inh, inl, chap, 1U);
  TEST_ASSERT((c2.style & (uint8_t)k_ra8_reflow_style_bold) == 0U);

  /* Specificity: `.chapter p` (101) beats `p` (1) for a shared property. */
  load(".chapter p { color: #ff0000; }\n"
       "p { color: #0000ff; }");
  const ra8_css_style_t cs = ra8_css_cascade_ctx(&s_sheet, &p, inh, inl, chap, 1U);
  TEST_ASSERT((cs.set & (uint8_t)k_ra8_css_set_color) != 0U);
  TEST_ASSERT_EQ(0xFF0000, cs.color);

  /* Tag-ancestor descendant: `p em` (em inside p). */
  load("p em { text-decoration: underline; }");
  TEST_ASSERT_EQ(1, s_sheet.rule_count);
  TEST_ASSERT_EQ(1, s_sheet.rules[0].anc_count);
  const ra8_css_element_t em    = elem(k_ra8_reflow_tag_em, nullptr, nullptr);
  const ra8_css_element_t pa[1] = {elem(k_ra8_reflow_tag_p, nullptr, nullptr)};
  const ra8_css_style_t   ce    = ra8_css_cascade_ctx(&s_sheet, &em, inh, inl, pa, 1U);
  TEST_ASSERT((ce.style & (uint8_t)k_ra8_reflow_style_underline) != 0U);
  const ra8_css_style_t cen = ra8_css_cascade_ctx(&s_sheet, &em, inh, inl, nullptr, 0U);
  TEST_ASSERT((cen.style & (uint8_t)k_ra8_reflow_style_underline) == 0U);

  /* Overflow: more than k_ra8_css_max_anc ancestor parts -> the rule is dropped. */
  load(".a .b .c .d .e p { font-weight: bold; }");
  TEST_ASSERT_EQ(0, s_sheet.rule_count);
  TEST_END("css descendant selectors");
}

/**
 * @brief Test entry point.
 * @return 0 on success; unity macros exit(1) on the first failure.
 */
/**
 * @var s_test_roster
 * @brief Fixed-order roster of every test case in this translation unit.
 *
 * @details
 * main() walks this table instead of naming each case, so its size does not
 * grow with the number of tests and adding a case is a one-line edit.
 *
 * @note Order is significant: cases run top to bottom, exactly as before.
 */
static void (*const s_test_roster[])(void) = {
  test_descendant,
  test_fontface_parse,
  test_fontface_cascade,
  test_fontface_match_mcdc,
  test_fontface_null_guards,
  test_parse_props,
  test_parse_normal_resets,
  test_parse_selectors,
  test_parse_unsupported_skipped,
  test_inline,
  test_match_mcdc,
  test_match_class_universal_type,
  test_compound_selectors,
  test_cascade_specificity,
  test_cascade_source_order,
  test_cascade_inheritance,
  test_cascade_null_mcdc,
  test_parse_color,
  test_cascade_color,
  test_parse_fontsize,
  test_decoration_line_alias,
  test_cascade_fontsize,
  test_display,
  test_resolve_skip_mcdc,
  test_resolve_winner_mcdc,
  test_null_guards,
};

int32_t main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  (void)fprintf(stderr, "[OK ] test_ra8_css.c\n");
  return 0;
}
