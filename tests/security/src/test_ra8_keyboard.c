/**
 * @file test_ra8_keyboard.c
 * @brief Host unit tests + MC/DC for the iOS-style keyboard widget (#105).
 *
 * @details
 * Lays the letters layer and asserts the key count + in-frame geometry, then
 * drives the typing model: lowercase, one-shot SHIFT (uppercase), the 123/ABC
 * layer toggle (digits + symbols), BACKSPACE, SPACE, RETURN. Plus MC/DC vectors
 * for the compound frame-rejection decision in ra8_kbd_layout_init.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_keyboard.h"
#include "unity_minimal.h"

/**
 * @enum keyboard_fixture_t
 * @brief Loop bounds and counts, sized so the case under test is actually reached.
 */
typedef enum : uint8_t {
  k_keyboard_scan_rounds =
    200U, /**< Scan rounds driven back to back, past the debounce window, so the state settles. */
} keyboard_fixture_t;

enum : int32_t {
  k_fx             = 0,    /**< Fx.                                          */
  k_fy             = 600,  /**< Fy.                                          */
  k_fw             = 1024, /**< Fw.                                          */
  k_fh             = 360,  /**< Fh.                                          */
  k_expect_letters = 31,   /**< Expect letters (10 + 9 + (1+7+1) + (1+1+1)). */
};

/** @brief Shared laid-out grid. */
static ra8_kbd_layout_t s_kb;

#include "ra8_keyboard_test_contracts.h"

/** @copydoc internal_key_of */
RA8_INTERNAL static uint8_t internal_key_of(char ch)
{
  for (uint8_t i = 0U; i < s_kb.count; i++) {
    if ((s_kb.keys[i].kind == k_ra8_kbd_key_char) && (s_kb.keys[i].ch_lower == ch)) {
      return i;
    }
  }
  return (uint8_t)k_ra8_kbd_no_hit;
}

/** @copydoc internal_key_of_kind */
RA8_INTERNAL static uint8_t internal_key_of_kind(ra8_kbd_key_kind_t kind)
{
  for (uint8_t i = 0U; i < s_kb.count; i++) {
    if (s_kb.keys[i].kind == kind) {
      return i;
    }
  }
  return (uint8_t)k_ra8_kbd_no_hit;
}

/** @copydoc internal_key_of_layer */
RA8_INTERNAL static uint8_t internal_key_of_layer(uint8_t aux)
{
  for (uint8_t i = 0U; i < s_kb.count; i++) {
    if ((s_kb.keys[i].kind == k_ra8_kbd_key_layer) && (s_kb.keys[i].aux == aux)) {
      return i;
    }
  }
  return (uint8_t)k_ra8_kbd_no_hit;
}

/** @copydoc internal_tap */
RA8_INTERNAL static void internal_tap(ra8_kbd_text_t* t, uint8_t idx)
{
  TEST_ASSERT(idx < (uint8_t)(sizeof(s_kb.keys) / sizeof(s_kb.keys[0])));
  const ra8_ui_rect_t* r  = &s_kb.keys[idx].rect;
  const int32_t        cx = r->x + (r->w / 2);
  const int32_t        cy = r->y + (r->h / 2);
  TEST_ASSERT_EQ(idx, ra8_kbd_hit(&s_kb, cx, cy));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_kbd_apply(t, &s_kb, idx));
}

/** @copydoc internal_type_lc */
RA8_INTERNAL static void internal_type_lc(ra8_kbd_text_t* t, const char* s)
{
  for (uint32_t i = 0U; s[i] != '\0'; i++) {
    internal_tap(t, internal_key_of(s[i]));
  }
}

/**
 * @copydoc internal_test_layout_letters
 * @test internal_test_layout_letters
 * @brief 31 keys in-frame; lowercase letters, SHIFT, and a 123 layer key.
 *
 * @par MC/DC:
 * Decision (in-test in-frame invariant): the per-key assertion
 * `(r->x >= k_fx) && ((r->x + r->w) <= (k_fx + k_fw))` (and the paired y-bounds)
 * (2 conditions, AND). It is asserted TRUE for every one of the 31 keys, so both
 * conditions are held at C1=T,C2=T -> T across the whole grid. This is a
 * conjunctive layout invariant, not an independence-demonstrating vector set: a
 * false arm would be an out-of-frame key -- the defect the conjunction exists to
 * catch -- so no false vector is driven. The frame-reject OR
 * `(w <= 0) || (h <= 0)` is held at its both-false control here; its N+1 vectors
 * live in internal_test_frame_reject_mcdc.
 */
RA8_INTERNAL static void internal_test_layout_letters(void)
{
  TEST_BEGIN("keyboard letters layer: 31 keys, lowercase + shift + 123");
  const ra8_ui_rect_t frame = {.x = k_fx, .y = k_fy, .w = k_fw, .h = k_fh};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_kbd_layout_init(&s_kb, &frame));
  TEST_ASSERT_EQ(k_expect_letters, s_kb.count);
  TEST_ASSERT(!s_kb.shift);
  TEST_ASSERT_EQ(k_ra8_kbd_layer_letters, s_kb.layer);
  for (uint8_t i = 0U; i < s_kb.count; i++) {
    const ra8_ui_rect_t* r = &s_kb.keys[i].rect;
    TEST_ASSERT((r->x >= k_fx) && ((r->x + r->w) <= (k_fx + k_fw)));
    TEST_ASSERT((r->y >= k_fy) && ((r->y + r->h) <= (k_fy + k_fh)));
  }
  TEST_ASSERT(internal_key_of('q') != (uint8_t)k_ra8_kbd_no_hit);
  TEST_ASSERT(internal_key_of_kind(k_ra8_kbd_key_shift) != (uint8_t)k_ra8_kbd_no_hit);
  TEST_ASSERT(internal_key_of_kind(k_ra8_kbd_key_layer) != (uint8_t)k_ra8_kbd_no_hit);
  TEST_ASSERT(internal_key_of('9') == (uint8_t)k_ra8_kbd_no_hit); /* digits are on the 123 layer */
  TEST_END("keyboard letters layer: 31 keys, lowercase + shift + 123");
}

/**
 * @copydoc internal_test_typing_layers
 * @test internal_test_typing_layers
 * @brief Case + the 123/ABC layer toggle (digit) + commit.
 *
 * @par MC/DC:
 * Decision (in the test's `internal_key_of` lookup, driven by every `internal_type_lc`):
 * `(keys[i].kind == k_ra8_kbd_key_char) && (keys[i].ch_lower == ch)`
 * (2 conditions, AND). Scanning the laid-out grid supplies all three states in a
 * single pass -- N+1 = 3:
 * - a non-char key (shift/space/layer/backspace/enter) -> C1=F -> false.
 * - a char key whose glyph differs                     -> C1=T,C2=F -> false.
 * - the target char key                                -> C1=T,C2=T -> true.
 * The F and T,F rows each pair with T,T to prove C1 and C2 independent. The
 * production `ra8_kbd_apply` path under test is a switch on key kind with no
 * compound boolean.
 */
RA8_INTERNAL static void internal_test_typing_layers(void)
{
  TEST_BEGIN("keyboard typing: case + layer toggle + digit + commit");
  const ra8_ui_rect_t frame = {.x = k_fx, .y = k_fy, .w = k_fw, .h = k_fh};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_kbd_layout_init(&s_kb, &frame));
  ra8_kbd_text_t t;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_kbd_text_init(&t));

  /* One-shot SHIFT capitalises only the next char: "Hi". */
  internal_tap(&t, internal_key_of_kind(k_ra8_kbd_key_shift));
  TEST_ASSERT(s_kb.shift);
  internal_type_lc(&t, "hi");
  TEST_ASSERT(!s_kb.shift);
  TEST_ASSERT_EQ(0, strcmp(t.buf, "Hi"));

  /* SPACE, then 123 -> numbers, type '9'. */
  internal_tap(&t, internal_key_of_kind(k_ra8_kbd_key_space));
  internal_tap(&t, internal_key_of_layer((uint8_t)k_ra8_kbd_layer_numbers)); /* 123 */
  TEST_ASSERT_EQ(k_ra8_kbd_layer_numbers, s_kb.layer);
  internal_type_lc(&t, "9");
  TEST_ASSERT_EQ(0, strcmp(t.buf, "Hi 9"));

  /* #+= -> symbols, type '['; then 123 -> numbers; then ABC -> letters. */
  internal_tap(&t, internal_key_of_layer((uint8_t)k_ra8_kbd_layer_symbols)); /* #+= */
  TEST_ASSERT_EQ(k_ra8_kbd_layer_symbols, s_kb.layer);
  internal_type_lc(&t, "[");
  TEST_ASSERT_EQ(0, strcmp(t.buf, "Hi 9["));
  internal_tap(&t,
               internal_key_of_layer((uint8_t)k_ra8_kbd_layer_numbers)); /* 123 (from symbols) */
  TEST_ASSERT_EQ(k_ra8_kbd_layer_numbers, s_kb.layer);
  internal_tap(&t, internal_key_of_layer((uint8_t)k_ra8_kbd_layer_letters)); /* ABC */
  TEST_ASSERT_EQ(k_ra8_kbd_layer_letters, s_kb.layer);

  /* BACKSPACE removes the '['; RETURN commits. */
  internal_tap(&t, internal_key_of_kind(k_ra8_kbd_key_backspace));
  TEST_ASSERT_EQ(0, strcmp(t.buf, "Hi 9"));
  TEST_ASSERT(!t.committed);
  internal_tap(&t, internal_key_of_kind(k_ra8_kbd_key_enter));
  TEST_ASSERT(t.committed);
  TEST_END("keyboard typing: case + layer toggle + digit + commit");
}

/** @copydoc internal_reachable_here */
RA8_INTERNAL static bool internal_reachable_here(char c)
{
  for (uint8_t i = 0U; i < s_kb.count; i++) {
    if ((s_kb.keys[i].kind == k_ra8_kbd_key_char) && (s_kb.keys[i].ch_lower == c)) {
      return true;
    }
  }
  return false;
}

/**
 * @copydoc internal_test_all_ascii_symbols
 * @test internal_test_all_ascii_symbols
 * @brief Every printable ASCII symbol + digit is reachable across the layers.
 *
 * @par MC/DC:
 * Decision (in the test's `internal_reachable_here` scan):
 * `(keys[i].kind == k_ra8_kbd_key_char) && (keys[i].ch_lower == c)`
 * (2 conditions, AND). Sweeping every char across all three layers drives all
 * three states -- N+1 = 3:
 * - a non-char key                    -> C1=F -> false.
 * - a char key with a different glyph -> C1=T,C2=F -> false.
 * - the reachable glyph               -> C1=T,C2=T -> true.
 * The F and T,F rows each pair with T,T to prove C1 and C2 independent. The
 * `if (!found)` layer-advance checks are single-condition.
 */
RA8_INTERNAL static void internal_test_all_ascii_symbols(void)
{
  TEST_BEGIN("keyboard covers every printable ASCII symbol + digit");
  const ra8_ui_rect_t frame = {.x = k_fx, .y = k_fy, .w = k_fw, .h = k_fh};
  /* The 32 printable ASCII symbols (0x21..0x2F, 0x3A..0x40, 0x5B..0x60,
   * 0x7B..0x7E) plus the ten digits. */
  static const char k_all[] = "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~0123456789";
  ra8_kbd_text_t    tmp;
  for (uint32_t i = 0U; k_all[i] != '\0'; i++) {
    const char c     = k_all[i];
    bool       found = false;
    /* Letters layer. */
    TEST_ASSERT_EQ(k_ra8_ok, ra8_kbd_layout_init(&s_kb, &frame));
    TEST_ASSERT_EQ(k_ra8_ok, ra8_kbd_text_init(&tmp));
    found = internal_reachable_here(c);
    /* -> numbers (123). */
    if (!found) {
      TEST_ASSERT_EQ(
        k_ra8_ok,
        ra8_kbd_apply(&tmp, &s_kb, internal_key_of_layer((uint8_t)k_ra8_kbd_layer_numbers)));
      found = internal_reachable_here(c);
    }
    /* -> symbols (#+=). */
    if (!found) {
      TEST_ASSERT_EQ(
        k_ra8_ok,
        ra8_kbd_apply(&tmp, &s_kb, internal_key_of_layer((uint8_t)k_ra8_kbd_layer_symbols)));
      found = internal_reachable_here(c);
    }
    TEST_ASSERT(found);
  }
  TEST_END("keyboard covers every printable ASCII symbol + digit");
}

/**
 * @copydoc internal_test_glyph_and_edges
 * @test internal_test_glyph_and_edges
 * @brief ra8_kbd_key_glyph tracks SHIFT; empty backspace + overflow are no-ops.
 *
 * @par MC/DC:
 * Decision: `(kb == nullptr) || (key_idx >= kb->count)` (2 conditions, OR;
 * ra8_kbd_key_glyph). This case drives C1=F,C2=F (the 'q' and enter keys ->
 * guard false, glyph returned) and C1=T (nullptr kb -> 0); the C2=T
 * (key_idx >= count) leg that completes N+1 = 3 is supplied by
 * internal_test_key_glyph_guard_mcdc. The test's own `internal_key_of` lookup additionally drives
 * `(keys[i].kind == char) && (keys[i].ch_lower == ch)` through all three states
 * (non-char C1=F, wrong-glyph C1=T,C2=F, match C1=T,C2=T) across the key scan.
 * The apply/backspace/overflow edges are single-condition guards.
 */
RA8_INTERNAL static void internal_test_glyph_and_edges(void)
{
  TEST_BEGIN("keyboard glyph/case + edge no-ops");
  const ra8_ui_rect_t frame = {.x = k_fx, .y = k_fy, .w = k_fw, .h = k_fh};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_kbd_layout_init(&s_kb, &frame));
  const uint8_t q = internal_key_of('q');
  TEST_ASSERT_EQ('q', ra8_kbd_key_glyph(&s_kb, q));
  s_kb.shift = true;
  TEST_ASSERT_EQ('Q', ra8_kbd_key_glyph(&s_kb, q));
  s_kb.shift = false;
  TEST_ASSERT_EQ(0, ra8_kbd_key_glyph(&s_kb, internal_key_of_kind(k_ra8_kbd_key_enter)));
  TEST_ASSERT_EQ(0, ra8_kbd_key_glyph(nullptr, 0U));

  ra8_kbd_text_t t;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_kbd_text_init(&t));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_kbd_apply(&t, &s_kb, internal_key_of_kind(k_ra8_kbd_key_backspace)));
  TEST_ASSERT_EQ(0, t.len);
  TEST_ASSERT_EQ(k_ra8_kbd_no_hit, ra8_kbd_hit(&s_kb, -100, -100));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_kbd_apply(&t, &s_kb, (uint8_t)k_ra8_kbd_no_hit));
  TEST_ASSERT_EQ(0, t.len);
  const uint8_t a = internal_key_of('a');
  for (uint32_t i = 0U; i < k_keyboard_scan_rounds; i++) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_kbd_apply(&t, &s_kb, a));
  }
  TEST_ASSERT_EQ(k_ra8_kbd_text_max - 1, t.len);
  TEST_END("keyboard glyph/case + edge no-ops");
}

/**
 * @copydoc internal_test_frame_reject_mcdc
 * @test internal_test_frame_reject_mcdc
 *
 * @par MC/DC:
 * Decision: `if (frame->w <= 0 || frame->h <= 0)` (2 conditions, OR) in the
 * production entry point libs/ra8_keyboard/src/ra8_keyboard.c@ra8_kbd_layout_init.
 * The vectors drive the real API (not a hand-copied mirror), so the coverage
 * lands on the production decision. N+1 = 3 vectors for N=2:
 *  - V1: w=1024, h=360 -> F,F -> accept (k_ra8_ok).
 *  - V2: w=0,    h=360 -> T,- -> reject (varies w, h held > 0).
 *  - V3: w=1024, h=0   -> F,T -> reject (varies h, w held > 0).
 * V1 vs V2 prove w independently flips the decision; V1 vs V3 prove the same
 * for h.
 */
RA8_INTERNAL static void internal_test_frame_reject_mcdc(void)
{
  TEST_BEGIN("layout frame-reject MC/DC: w<=0 || h<=0");
  const ra8_ui_rect_t v1_ok = {.x = k_fx, .y = k_fy, .w = k_fw, .h = k_fh};
  const ra8_ui_rect_t v2_w0 = {.x = k_fx, .y = k_fy, .w = 0, .h = k_fh};
  const ra8_ui_rect_t v3_h0 = {.x = k_fx, .y = k_fy, .w = k_fw, .h = 0};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_kbd_layout_init(&s_kb, &v1_ok));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_kbd_layout_init(&s_kb, &v2_w0));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_kbd_layout_init(&s_kb, &v3_h0));
  TEST_END("layout frame-reject MC/DC: w<=0 || h<=0");
}

/**
 * @copydoc internal_test_key_glyph_guard_mcdc
 * @test internal_test_key_glyph_guard_mcdc
 *
 * @par MC/DC:
 * Decision: `if (kb == nullptr || key_idx >= kb->count)` (2 conditions, OR) in
 * libs/ra8_keyboard/src/ra8_keyboard.c@ra8_kbd_key_glyph. Driving the real API
 * lands the coverage on the production guard. N+1 = 3 vectors for N=2:
 *  - V1: kb=laid-out, key_idx='q'      -> F,F -> returns the glyph.
 *  - V2: kb=nullptr,  key_idx=0        -> T,- -> returns 0 (varies kb).
 *  - V3: kb=laid-out, key_idx==count   -> F,T -> returns 0 (varies key_idx).
 * V1 vs V2 prove kb independently flips the guard; V1 vs V3 prove the same for
 * key_idx.
 */
RA8_INTERNAL static void internal_test_key_glyph_guard_mcdc(void)
{
  TEST_BEGIN("key-glyph guard MC/DC: kb==nullptr || key_idx>=count");
  const ra8_ui_rect_t frame = {.x = k_fx, .y = k_fy, .w = k_fw, .h = k_fh};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_kbd_layout_init(&s_kb, &frame));
  const uint8_t q = internal_key_of('q');
  TEST_ASSERT(ra8_kbd_key_glyph(&s_kb, q) != (char)0);     /* V1: F,F */
  TEST_ASSERT_EQ(0, ra8_kbd_key_glyph(nullptr, 0U));       /* V2: T,- */
  TEST_ASSERT_EQ(0, ra8_kbd_key_glyph(&s_kb, s_kb.count)); /* V3: F,T */
  TEST_END("key-glyph guard MC/DC: kb==nullptr || key_idx>=count");
}

/**
 * @copydoc internal_test_null_guards
 * @test internal_test_null_guards
 * @brief NULL arguments are rejected.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- it drives the single-condition guards:
 * the two separate RA8_CHECK_NULL_PTR checks in ra8_kbd_layout_init (kb, then
 * frame), the RA8_CHECK_NULL_PTR in ra8_kbd_text_init, and the `kb == nullptr`
 * guard in ra8_kbd_hit. Each is one condition; the `w <= 0 || h <= 0` frame OR
 * is never reached because the null check returns first)
 */
RA8_INTERNAL static void internal_test_null_guards(void)
{
  TEST_BEGIN("keyboard null guards");
  const ra8_ui_rect_t frame = {.x = 0, .y = 0, .w = 10, .h = 10};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_kbd_layout_init(nullptr, &frame));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_kbd_layout_init(&s_kb, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_kbd_text_init(nullptr));
  TEST_ASSERT_EQ(k_ra8_kbd_no_hit, ra8_kbd_hit(nullptr, 0, 0));
  TEST_END("keyboard null guards");
}

/**
 * @brief Test entry point.
 * @return 0 on success; unity macros exit(1) on the first failure.
 */
int main(void)
{
  internal_test_layout_letters();
  internal_test_typing_layers();
  internal_test_all_ascii_symbols();
  internal_test_glyph_and_edges();
  internal_test_frame_reject_mcdc();
  internal_test_key_glyph_guard_mcdc();
  internal_test_null_guards();
  return 0;
}
