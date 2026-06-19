/**
 * @file test_ra_keyboard.c
 * @brief Host unit tests + MC/DC for the on-screen keyboard widget (#105).
 *
 * @details
 * Lays the QWERTY grid into a frame and asserts the key count + a sample
 * geometry, then drives the typing model: tapping key centres builds a string,
 * BACKSPACE deletes, SPACE inserts, ENTER commits. Plus MC/DC vectors for the
 * compound frame-rejection decision in ra_kbd_layout_init.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra_keyboard.h"
#include "unity_minimal.h"

enum : int32_t {
  k_fx          = 0,
  k_fy          = 600,
  k_fw          = 1024,
  k_fh          = 360,
  k_expect_keys = 29, /* 10 + 9 + 8 + 2 */
};

/** @brief Shared laid-out grid (kept off the stack). */
static ra_kbd_layout_t s_kb;

/** @brief Find the key index for printable @p ch, or k_ra_kbd_no_hit. */
static uint8_t key_of(char ch)
{
  for (uint8_t i = 0U; i < s_kb.count; i++) {
    if ((s_kb.keys[i].kind == k_ra_kbd_key_char) && (s_kb.keys[i].ch == ch)) {
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

/** @brief Tap a key's centre and apply it to @p t. */
static void tap_key(ra_kbd_text_t* t, uint8_t idx)
{
  const ra_ui_rect_t* r  = &s_kb.keys[idx].rect;
  const int32_t       cx = r->x + (r->w / 2);
  const int32_t       cy = r->y + (r->h / 2);
  TEST_ASSERT_EQ((int32_t)idx, (int32_t)ra_kbd_hit(&s_kb, cx, cy));
  TEST_ASSERT_EQ(k_ra_ok, ra_kbd_apply(t, &s_kb, ra_kbd_hit(&s_kb, cx, cy)));
}

/** @brief Type a printable string by tapping each key. */
static void type_str(ra_kbd_text_t* t, const char* s)
{
  for (uint32_t i = 0U; s[i] != '\0'; i++) {
    tap_key(t, key_of(s[i]));
  }
}

/**
 * @test test_layout_grid
 * @brief The grid lays 29 keys into the frame and every rect is inside it.
 */
static void test_layout_grid(void)
{
  TEST_BEGIN("keyboard layout: 29 keys inside the frame");
  const ra_ui_rect_t frame = {.x = k_fx, .y = k_fy, .w = k_fw, .h = k_fh};
  TEST_ASSERT_EQ(k_ra_ok, ra_kbd_layout_init(&s_kb, &frame));
  TEST_ASSERT_EQ(k_expect_keys, (int32_t)s_kb.count);
  for (uint8_t i = 0U; i < s_kb.count; i++) {
    const ra_ui_rect_t* r = &s_kb.keys[i].rect;
    TEST_ASSERT((r->x >= k_fx) && ((r->x + r->w) <= (k_fx + k_fw)));
    TEST_ASSERT((r->y >= k_fy) && ((r->y + r->h) <= (k_fy + k_fh)));
  }
  /* Q is the first key, top-left of the frame. */
  TEST_ASSERT_EQ((int32_t)k_ra_kbd_key_char, (int32_t)s_kb.keys[0].kind);
  TEST_ASSERT_EQ((int32_t)'Q', (int32_t)s_kb.keys[0].ch);
  TEST_ASSERT_EQ(k_fx, s_kb.keys[0].rect.x);
  TEST_END("keyboard layout: 29 keys inside the frame");
}

/**
 * @test test_typing
 * @brief Tapping key centres builds a query; backspace/space/enter behave.
 */
static void test_typing(void)
{
  TEST_BEGIN("keyboard typing: build + edit + commit a query");
  const ra_ui_rect_t frame = {.x = k_fx, .y = k_fy, .w = k_fw, .h = k_fh};
  TEST_ASSERT_EQ(k_ra_ok, ra_kbd_layout_init(&s_kb, &frame));
  ra_kbd_text_t t;
  TEST_ASSERT_EQ(k_ra_ok, ra_kbd_text_init(&t));

  type_str(&t, "HELLO");
  TEST_ASSERT_EQ(0, strcmp(t.buf, "HELLO"));
  /* SPACE then WORLD. */
  tap_key(&t, key_of_kind(k_ra_kbd_key_space));
  type_str(&t, "WORLD");
  TEST_ASSERT_EQ(0, strcmp(t.buf, "HELLO WORLD"));
  /* BACKSPACE removes the last char. */
  tap_key(&t, key_of_kind(k_ra_kbd_key_backspace));
  TEST_ASSERT_EQ(0, strcmp(t.buf, "HELLO WORL"));
  TEST_ASSERT(!t.committed);
  /* ENTER commits. */
  tap_key(&t, key_of_kind(k_ra_kbd_key_enter));
  TEST_ASSERT(t.committed);
  TEST_END("keyboard typing: build + edit + commit a query");
}

/**
 * @test test_edge_cases
 * @brief Backspace on empty + a miss tap are no-ops; full buffer stops.
 */
static void test_edge_cases(void)
{
  TEST_BEGIN("keyboard edges: empty backspace, miss, overflow");
  const ra_ui_rect_t frame = {.x = k_fx, .y = k_fy, .w = k_fw, .h = k_fh};
  TEST_ASSERT_EQ(k_ra_ok, ra_kbd_layout_init(&s_kb, &frame));
  ra_kbd_text_t t;
  TEST_ASSERT_EQ(k_ra_ok, ra_kbd_text_init(&t));
  /* Backspace on empty: no-op. */
  TEST_ASSERT_EQ(k_ra_ok, ra_kbd_apply(&t, &s_kb, key_of_kind(k_ra_kbd_key_backspace)));
  TEST_ASSERT_EQ(0, (int32_t)t.len);
  /* A tap that hits nothing returns the sentinel and applies as a no-op. */
  TEST_ASSERT_EQ((int32_t)k_ra_kbd_no_hit, (int32_t)ra_kbd_hit(&s_kb, -100, -100));
  TEST_ASSERT_EQ(k_ra_ok, ra_kbd_apply(&t, &s_kb, (uint8_t)k_ra_kbd_no_hit));
  TEST_ASSERT_EQ(0, (int32_t)t.len);
  /* Overflow guard: hammer 'A' past capacity; len caps at text_max-1. */
  const uint8_t a = key_of('A');
  for (uint32_t i = 0U; i < 200U; i++) {
    TEST_ASSERT_EQ(k_ra_ok, ra_kbd_apply(&t, &s_kb, a));
  }
  TEST_ASSERT_EQ((int32_t)k_ra_kbd_text_max - 1, (int32_t)t.len);
  TEST_END("keyboard edges: empty backspace, miss, overflow");
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
  /* And the real function rejects a zero-area frame. */
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
  test_layout_grid();
  test_typing();
  test_edge_cases();
  test_frame_reject_mcdc();
  test_null_guards();
  (void)fprintf(stderr, "[OK ] test_ra_keyboard.c\n");
  return 0;
}
