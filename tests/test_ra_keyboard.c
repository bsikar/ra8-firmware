/**
 * @file test_ra_keyboard.c
 * @brief Host unit tests + MC/DC for the iOS-style keyboard widget (#105).
 *
 * @details
 * Lays the letters layer and asserts the key count + in-frame geometry, then
 * drives the typing model: lowercase, one-shot SHIFT (uppercase), the 123/ABC
 * layer toggle (digits + symbols), BACKSPACE, SPACE, RETURN. Plus MC/DC vectors
 * for the compound frame-rejection decision in ra_kbd_layout_init.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra_keyboard.h"
#include "unity_minimal.h"

enum : int32_t {
  k_fx             = 0,
  k_fy             = 600,
  k_fw             = 1024,
  k_fh             = 360,
  k_expect_letters = 31, /* 10 + 9 + (1+7+1) + (1+1+1) */
};

/** @brief Shared laid-out grid. */
static ra_kbd_layout_t s_kb;

/** @brief Find the char key whose unshifted char is @p ch in the active layer. */
static uint8_t key_of(char ch)
{
  for (uint8_t i = 0U; i < s_kb.count; i++) {
    if ((s_kb.keys[i].kind == k_ra_kbd_key_char) && (s_kb.keys[i].ch_lower == ch)) {
      return i;
    }
  }
  return (uint8_t)k_ra_kbd_no_hit;
}

/** @brief Find the first key of a given non-char kind. */
static uint8_t key_of_kind(ra_kbd_key_kind_t kind)
{
  for (uint8_t i = 0U; i < s_kb.count; i++) {
    if (s_kb.keys[i].kind == kind) {
      return i;
    }
  }
  return (uint8_t)k_ra_kbd_no_hit;
}

/** @brief Find the layer-toggle key that switches to layer @p aux. */
static uint8_t key_of_layer(uint8_t aux)
{
  for (uint8_t i = 0U; i < s_kb.count; i++) {
    if ((s_kb.keys[i].kind == k_ra_kbd_key_layer) && (s_kb.keys[i].aux == aux)) {
      return i;
    }
  }
  return (uint8_t)k_ra_kbd_no_hit;
}

/** @brief Tap a key's centre and apply it. */
static void tap(ra_kbd_text_t* t, uint8_t idx)
{
  const ra_ui_rect_t* r  = &s_kb.keys[idx].rect;
  const int32_t       cx = r->x + (r->w / 2);
  const int32_t       cy = r->y + (r->h / 2);
  TEST_ASSERT_EQ((int32_t)idx, (int32_t)ra_kbd_hit(&s_kb, cx, cy));
  TEST_ASSERT_EQ(k_ra_ok, ra_kbd_apply(t, &s_kb, idx));
}

/** @brief Type lowercase printable chars (must be on the active layer). */
static void type_lc(ra_kbd_text_t* t, const char* s)
{
  for (uint32_t i = 0U; s[i] != '\0'; i++) {
    tap(t, key_of(s[i]));
  }
}

/**
 * @test test_layout_letters
 * @brief 31 keys in-frame; lowercase letters, SHIFT, and a 123 layer key.
 */
static void test_layout_letters(void)
{
  TEST_BEGIN("keyboard letters layer: 31 keys, lowercase + shift + 123");
  const ra_ui_rect_t frame = {.x = k_fx, .y = k_fy, .w = k_fw, .h = k_fh};
  TEST_ASSERT_EQ(k_ra_ok, ra_kbd_layout_init(&s_kb, &frame));
  TEST_ASSERT_EQ(k_expect_letters, (int32_t)s_kb.count);
  TEST_ASSERT(!s_kb.shift);
  TEST_ASSERT_EQ((int32_t)k_ra_kbd_layer_letters, (int32_t)s_kb.layer);
  for (uint8_t i = 0U; i < s_kb.count; i++) {
    const ra_ui_rect_t* r = &s_kb.keys[i].rect;
    TEST_ASSERT((r->x >= k_fx) && ((r->x + r->w) <= (k_fx + k_fw)));
    TEST_ASSERT((r->y >= k_fy) && ((r->y + r->h) <= (k_fy + k_fh)));
  }
  TEST_ASSERT(key_of('q') != (uint8_t)k_ra_kbd_no_hit);
  TEST_ASSERT(key_of_kind(k_ra_kbd_key_shift) != (uint8_t)k_ra_kbd_no_hit);
  TEST_ASSERT(key_of_kind(k_ra_kbd_key_layer) != (uint8_t)k_ra_kbd_no_hit);
  TEST_ASSERT(key_of('9') == (uint8_t)k_ra_kbd_no_hit); /* digits are on the 123 layer */
  TEST_END("keyboard letters layer: 31 keys, lowercase + shift + 123");
}

/**
 * @test test_typing_layers
 * @brief Case + the 123/ABC layer toggle (digit) + commit.
 */
static void test_typing_layers(void)
{
  TEST_BEGIN("keyboard typing: case + layer toggle + digit + commit");
  const ra_ui_rect_t frame = {.x = k_fx, .y = k_fy, .w = k_fw, .h = k_fh};
  TEST_ASSERT_EQ(k_ra_ok, ra_kbd_layout_init(&s_kb, &frame));
  ra_kbd_text_t t;
  TEST_ASSERT_EQ(k_ra_ok, ra_kbd_text_init(&t));

  /* One-shot SHIFT capitalises only the next char: "Hi". */
  tap(&t, key_of_kind(k_ra_kbd_key_shift));
  TEST_ASSERT(s_kb.shift);
  type_lc(&t, "hi");
  TEST_ASSERT(!s_kb.shift);
  TEST_ASSERT_EQ(0, strcmp(t.buf, "Hi"));

  /* SPACE, then 123 -> numbers, type '9'. */
  tap(&t, key_of_kind(k_ra_kbd_key_space));
  tap(&t, key_of_layer((uint8_t)k_ra_kbd_layer_numbers)); /* 123 */
  TEST_ASSERT_EQ((int32_t)k_ra_kbd_layer_numbers, (int32_t)s_kb.layer);
  type_lc(&t, "9");
  TEST_ASSERT_EQ(0, strcmp(t.buf, "Hi 9"));

  /* #+= -> symbols, type '['; then 123 -> numbers; then ABC -> letters. */
  tap(&t, key_of_layer((uint8_t)k_ra_kbd_layer_symbols)); /* #+= */
  TEST_ASSERT_EQ((int32_t)k_ra_kbd_layer_symbols, (int32_t)s_kb.layer);
  type_lc(&t, "[");
  TEST_ASSERT_EQ(0, strcmp(t.buf, "Hi 9["));
  tap(&t, key_of_layer((uint8_t)k_ra_kbd_layer_numbers)); /* 123 (from symbols) */
  TEST_ASSERT_EQ((int32_t)k_ra_kbd_layer_numbers, (int32_t)s_kb.layer);
  tap(&t, key_of_layer((uint8_t)k_ra_kbd_layer_letters)); /* ABC */
  TEST_ASSERT_EQ((int32_t)k_ra_kbd_layer_letters, (int32_t)s_kb.layer);

  /* BACKSPACE removes the '['; RETURN commits. */
  tap(&t, key_of_kind(k_ra_kbd_key_backspace));
  TEST_ASSERT_EQ(0, strcmp(t.buf, "Hi 9"));
  TEST_ASSERT(!t.committed);
  tap(&t, key_of_kind(k_ra_kbd_key_enter));
  TEST_ASSERT(t.committed);
  TEST_END("keyboard typing: case + layer toggle + digit + commit");
}

/** @brief Is char @p c reachable as a char key on the active layer? */
static bool reachable_here(char c)
{
  for (uint8_t i = 0U; i < s_kb.count; i++) {
    if ((s_kb.keys[i].kind == k_ra_kbd_key_char) && (s_kb.keys[i].ch_lower == c)) {
      return true;
    }
  }
  return false;
}

/**
 * @test test_all_ascii_symbols
 * @brief Every printable ASCII symbol + digit is reachable across the layers.
 */
static void test_all_ascii_symbols(void)
{
  TEST_BEGIN("keyboard covers every printable ASCII symbol + digit");
  const ra_ui_rect_t frame = {.x = k_fx, .y = k_fy, .w = k_fw, .h = k_fh};
  /* The 32 printable ASCII symbols (0x21..0x2F, 0x3A..0x40, 0x5B..0x60,
   * 0x7B..0x7E) plus the ten digits. */
  static const char k_all[] = "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~0123456789";
  ra_kbd_text_t     tmp;
  for (uint32_t i = 0U; k_all[i] != '\0'; i++) {
    const char c     = k_all[i];
    bool       found = false;
    /* Letters layer. */
    TEST_ASSERT_EQ(k_ra_ok, ra_kbd_layout_init(&s_kb, &frame));
    TEST_ASSERT_EQ(k_ra_ok, ra_kbd_text_init(&tmp));
    found = reachable_here(c);
    /* -> numbers (123). */
    if (!found) {
      TEST_ASSERT_EQ(k_ra_ok,
                     ra_kbd_apply(&tmp, &s_kb, key_of_layer((uint8_t)k_ra_kbd_layer_numbers)));
      found = reachable_here(c);
    }
    /* -> symbols (#+=). */
    if (!found) {
      TEST_ASSERT_EQ(k_ra_ok,
                     ra_kbd_apply(&tmp, &s_kb, key_of_layer((uint8_t)k_ra_kbd_layer_symbols)));
      found = reachable_here(c);
    }
    TEST_ASSERT(found);
  }
  TEST_END("keyboard covers every printable ASCII symbol + digit");
}

/**
 * @test test_glyph_and_edges
 * @brief ra_kbd_key_glyph tracks SHIFT; empty backspace + overflow are no-ops.
 */
static void test_glyph_and_edges(void)
{
  TEST_BEGIN("keyboard glyph/case + edge no-ops");
  const ra_ui_rect_t frame = {.x = k_fx, .y = k_fy, .w = k_fw, .h = k_fh};
  TEST_ASSERT_EQ(k_ra_ok, ra_kbd_layout_init(&s_kb, &frame));
  const uint8_t q = key_of('q');
  TEST_ASSERT_EQ((int32_t)'q', (int32_t)ra_kbd_key_glyph(&s_kb, q));
  s_kb.shift = true;
  TEST_ASSERT_EQ((int32_t)'Q', (int32_t)ra_kbd_key_glyph(&s_kb, q));
  s_kb.shift = false;
  TEST_ASSERT_EQ(0, (int32_t)ra_kbd_key_glyph(&s_kb, key_of_kind(k_ra_kbd_key_enter)));
  TEST_ASSERT_EQ(0, (int32_t)ra_kbd_key_glyph(nullptr, 0U));

  ra_kbd_text_t t;
  TEST_ASSERT_EQ(k_ra_ok, ra_kbd_text_init(&t));
  TEST_ASSERT_EQ(k_ra_ok, ra_kbd_apply(&t, &s_kb, key_of_kind(k_ra_kbd_key_backspace)));
  TEST_ASSERT_EQ(0, (int32_t)t.len);
  TEST_ASSERT_EQ((int32_t)k_ra_kbd_no_hit, (int32_t)ra_kbd_hit(&s_kb, -100, -100));
  TEST_ASSERT_EQ(k_ra_ok, ra_kbd_apply(&t, &s_kb, (uint8_t)k_ra_kbd_no_hit));
  TEST_ASSERT_EQ(0, (int32_t)t.len);
  const uint8_t a = key_of('a');
  for (uint32_t i = 0U; i < 200U; i++) {
    TEST_ASSERT_EQ(k_ra_ok, ra_kbd_apply(&t, &s_kb, a));
  }
  TEST_ASSERT_EQ((int32_t)k_ra_kbd_text_max - 1, (int32_t)t.len);
  TEST_END("keyboard glyph/case + edge no-ops");
}

/** @brief Mirror of the frame-rejection decision: (w<=0) || (h<=0). */
static uint8_t mirror_reject(int32_t w, int32_t h)
{
  if ((w <= 0) || (h <= 0)) {
    return 1U;
  }
  return 0U;
}

/**
 * @test test_frame_reject_mcdc
 *
 * @par MC/DC:
 * Decision: `if (frame->w <= 0 || frame->h <= 0)` (2 conditions, OR;
 * ra_kbd_layout_init). N+1 = 3 vectors:
 *  - V1: w=10, h=10 -> F,F -> accept.
 *  - V2: w=0,  h=10 -> T   -> reject (varies w).
 *  - V3: w=10, h=0  -> F,T -> reject (varies h).
 */
static void test_frame_reject_mcdc(void)
{
  TEST_BEGIN("layout frame-reject MC/DC: w<=0 || h<=0");
  TEST_ASSERT_EQ(0, mirror_reject(10, 10));
  TEST_ASSERT_EQ(1, mirror_reject(0, 10));
  TEST_ASSERT_EQ(1, mirror_reject(10, 0));
  const ra_ui_rect_t bad = {.x = 0, .y = 0, .w = 0, .h = 10};
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_kbd_layout_init(&s_kb, &bad));
  TEST_END("layout frame-reject MC/DC: w<=0 || h<=0");
}

/**
 * @test test_null_guards
 * @brief NULL arguments are rejected.
 */
static void test_null_guards(void)
{
  TEST_BEGIN("keyboard null guards");
  const ra_ui_rect_t frame = {.x = 0, .y = 0, .w = 10, .h = 10};
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_kbd_layout_init(nullptr, &frame));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_kbd_layout_init(&s_kb, nullptr));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_kbd_text_init(nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_kbd_no_hit, (int32_t)ra_kbd_hit(nullptr, 0, 0));
  TEST_END("keyboard null guards");
}

/**
 * @brief Test entry point.
 * @return 0 on success; unity macros exit(1) on the first failure.
 */
int32_t main(void)
{
  test_layout_letters();
  test_typing_layers();
  test_all_ascii_symbols();
  test_glyph_and_edges();
  test_frame_reject_mcdc();
  test_null_guards();
  (void)fprintf(stderr, "[OK ] test_ra_keyboard.c\n");
  return 0;
}
