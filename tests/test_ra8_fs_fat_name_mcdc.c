/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_fs_fat_name_mcdc.c
 * @brief MC/DC vectors for the compound decisions in ra8_fs_fat_name.c.
 *
 * @details
 * Sibling of `tests/test_ra8_fs_fat_name_cov.c`, which drives the same 8.3
 * pack/unpack primitives for statement/branch coverage. This file adds the
 * dedicated N+1 independent-influence vector sets that the compound-decision
 * ratchet (`scripts/checks/mcdc_compound_ratchet.py`, issue #426) requires for:
 *
 *   - `priv_to_upper`   -- the ASCII lower-case range test.
 *   - `priv_pack_base`  -- the 8.3 base-name scan loop head (reached through the
 *                          public `priv_path_to_83`, which owns the buffer).
 *   - `priv_path_to_83` -- the null-argument guard.
 *   - `priv_83_to_str`  -- the Shift-JIS kanji-escape restore, one condition of
 *                          which is structurally co-determined (deactivated).
 *
 * The name primitives are pure functions over caller-supplied buffers, so each
 * decision is driven directly with no block device; the citations name the
 * enclosing function as `libs/ra8_fs/src/ra8_fs_fat_name.c@<function>`.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ra8_fs.h"
#include "ra8_fs_fat_internal.h"
#include "unity_minimal.h"

/**
 * @enum ra8_fs_name_mcdc_t
 * @brief Fixed sizes and sentinel characters for the 8.3 name vectors.
 *
 * @details The packed 8.3 field is 11 bytes (8 base + 3 ext); the unpacked
 *          string needs at most 12 chars plus a NUL. The character sentinels
 *          bracket the `'a'..'z'` range that `priv_to_upper` folds.
 */
typedef enum : uint8_t {
  k_name_packed_len = 11U, /**< Packed 8.3 field width (8 base + 3 ext).       */
  k_name_str_cap    = 13U, /**< Unpacked "NAME.EXT" buffer incl. NUL.          */
  k_char_below_a    = '0', /**< 0x30: below 'a' -- fails the lower-bound test. */
  k_char_in_range   = 'm', /**< 0x6D: inside 'a'..'z' -- folds to 'M'.         */
  k_char_upper_m    = 'M', /**< Expected fold of 'm'.                          */
  k_char_above_z    = '{', /**< 0x7B: above 'z' -- fails the upper-bound test. */
} ra8_fs_name_mcdc_t;

/**
 * @test test_mcdc_to_upper_ascii_range
 * @par MC/DC:
 * Decision: `if (c >= 'a' && c <= 'z')` in
 * `libs/ra8_fs/src/ra8_fs_fat_name.c@priv_to_upper` (2 conditions).
 * N+1 = 3 vectors; each condition flips with the other held at its masking
 * value (T):
 * - V1: c='m' (0x6D) -> C1=T (>= 'a'), C2=T (<= 'z') -> dec T -> folds to 'M'.
 * - V2: c='0' (0x30) -> C1=F short-circuit -> dec F -> returned unchanged.
 * - V3: c='{' (0x7B) -> C1=T, C2=F (> 'z')       -> dec F -> returned unchanged.
 * V1+V2 prove the lower bound independently gates the fold; V1+V3 prove the
 * same for the upper bound. Minimal MC/DC for N=2.
 */
static void test_mcdc_to_upper_ascii_range(void)
{
  TEST_BEGIN("ra8_fs name MC/DC: priv_to_upper ASCII range (c>='a' && c<='z')");
  TEST_ASSERT_EQ((char)k_char_upper_m, priv_to_upper((char)k_char_in_range));
  TEST_ASSERT_EQ((char)k_char_below_a, priv_to_upper((char)k_char_below_a));
  TEST_ASSERT_EQ((char)k_char_above_z, priv_to_upper((char)k_char_above_z));
  TEST_END("ra8_fs name MC/DC: priv_to_upper ASCII range (c>='a' && c<='z')");
}

/**
 * @test test_mcdc_pack_base_scan_loop
 * @par MC/DC:
 * Decision: `while (*path != '\0' && *path != '.')` in
 * `libs/ra8_fs/src/ra8_fs_fat_name.c@priv_pack_base` (2 conditions), the 8.3
 * base-name scan loop head. `priv_pack_base` is TU-static, so it is driven
 * through the public `priv_path_to_83`, which pre-pads the buffer and hands the
 * cursor in.
 * - V1 (both true): every interior base character ("FILE") keeps the loop
 *   running -> C1=T, C2=T.
 * - V2 (C1=F): a dotless name "FILE" reaches its NUL after the base, so the loop
 *   exits on `*path == '\0'` (C1=F) with the last comparison's C2 still true.
 * - V3 (C2=F): "FILE.TXT" reaches the '.' after the base, so the loop exits on
 *   `*path == '.'` (C2=F) with C1 still true.
 * V1+V2 isolate the NUL bound; V1+V3 isolate the dot bound. Both names pack
 * their 8-char base identically, proving only the terminator differs. N+1 = 3.
 */
static void test_mcdc_pack_base_scan_loop(void)
{
  TEST_BEGIN("ra8_fs name MC/DC: priv_pack_base scan (*p!=0 && *p!='.')");
  uint8_t out_dotless[k_name_packed_len] = {};
  uint8_t out_dotted[k_name_packed_len]  = {};

  /* C1=F leg: dotless name, base loop exits on the terminating NUL. */
  TEST_ASSERT_EQ(1U, priv_path_to_83("FILE", out_dotless));
  /* C2=F leg: dotted name, base loop exits on the '.'. */
  TEST_ASSERT_EQ(1U, priv_path_to_83("FILE.TXT", out_dotted));

  /* Both share the identical 8-char base -- only the post-base char differs. */
  TEST_ASSERT_EQ(0, memcmp(out_dotless, out_dotted, (size_t)k_filename_base_len));
  TEST_ASSERT_EQ('F', (char)out_dotless[0]);
  TEST_ASSERT_EQ('E', (char)out_dotless[3]);
  TEST_END("ra8_fs name MC/DC: priv_pack_base scan (*p!=0 && *p!='.')");
}

/**
 * @test test_mcdc_path_to_83_null_guard
 * @par MC/DC:
 * Decision: `if (path == nullptr || out11 == nullptr)` in
 * `libs/ra8_fs/src/ra8_fs_fat_name.c@priv_path_to_83` (2 conditions).
 * N+1 = 3 vectors; each condition flips with the other held at its masking
 * value (F):
 * - V1: path=ok,   out11=ok    -> C1=F, C2=F -> dec F (packs, returns 1).
 * - V2: path=NULL, out11=ok    -> C1=T short -> dec T -> returns 0.
 * - V3: path=ok,   out11=NULL  -> C1=F, C2=T -> dec T -> returns 0.
 * V1+V2 isolate path; V1+V3 isolate out11. Minimal MC/DC for N=2.
 */
static void test_mcdc_path_to_83_null_guard(void)
{
  TEST_BEGIN("ra8_fs name MC/DC: priv_path_to_83 null guard");
  uint8_t out11[k_name_packed_len] = {};
  TEST_ASSERT_EQ(1U, priv_path_to_83("FILE.TXT", out11));
  TEST_ASSERT_EQ(0U, priv_path_to_83(nullptr, out11));
  TEST_ASSERT_EQ(0U, priv_path_to_83("FILE.TXT", nullptr));
  TEST_END("ra8_fs name MC/DC: priv_path_to_83 null guard");
}

/**
 * @test test_mcdc_83_to_str_kanji_escape
 * @par MC/DC:
 * Decision: `if (j > 0 && (uint8_t)out12[0] == k_dir_marker_kanji_e5 &&
 * in11[0] == k_dir_marker_kanji_e5)` in
 * `libs/ra8_fs/src/ra8_fs_fat_name.c@priv_83_to_str` (3 conditions), which
 * restores a 0x05-escaped leading byte back to the raw 0xE5 kanji byte.
 *
 * This decision is MC/DC-DEACTIVATED under DO-178C 6.4.4.3: independent
 * influence of any single condition is structurally unreachable.
 * - `out12[0]` is assigned `(char)in11[0]` when the first non-space byte is
 *   copied, so condition 2 (`out12[0] == 0x05`) and condition 3 (`in11[0] ==
 *   0x05`) are ALWAYS equal whenever j > 0; neither can be flipped with the
 *   other held.
 * - condition 1 (`j > 0`) is false only when `in11[0] == ' '` (empty base), which
 *   forces conditions 2 and 3 false too, so j cannot be isolated with them true.
 *
 * Both decision OUTCOMES are still exercised (the honest floor for a
 * deactivated compound):
 * - V-true:  in11[0]=0x05 (escaped kanji) -> j>0, out12[0]==0x05, in11[0]==0x05
 *            -> restored to 0xE5.
 * - V-false: in11[0]='F' (ordinary name)  -> out12[0]=='F' != 0x05 -> no restore.
 */
static void test_mcdc_83_to_str_kanji_escape(void)
{
  TEST_BEGIN("ra8_fs name MC/DC: priv_83_to_str kanji escape (deactivated)");
  char out12[k_name_str_cap] = {};

  /* V-true: an 0x05-escaped leading byte is restored to the raw 0xE5. */
  uint8_t in_kanji[k_name_packed_len] =
    {(uint8_t)k_dir_marker_kanji_e5, 'A', 'M', 'E', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
  priv_83_to_str(in_kanji, out12);
  const uint8_t restored = (uint8_t)out12[0];
  TEST_ASSERT_EQ(k_dir_marker_free_used, restored);

  /* V-false: an ordinary leading byte leaves the name untouched. */
  memset(out12, 0, sizeof out12);
  uint8_t in_plain[k_name_packed_len] = {'F', 'I', 'L', 'E', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
  priv_83_to_str(in_plain, out12);
  TEST_ASSERT_EQ('F', out12[0]);
  TEST_END("ra8_fs name MC/DC: priv_83_to_str kanji escape (deactivated)");
}

int32_t main(void)
{
  test_mcdc_to_upper_ascii_range();
  test_mcdc_pack_base_scan_loop();
  test_mcdc_path_to_83_null_guard();
  test_mcdc_83_to_str_kanji_escape();
  (void)fprintf(stderr, "[OK  ] test_ra8_fs_fat_name_mcdc.c\n");
  return 0;
}
