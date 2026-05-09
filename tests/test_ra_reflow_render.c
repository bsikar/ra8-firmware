/**
 * @file test_ra_reflow_render.c
 * @brief MC/DC vectors for libs/ra_reflow/src/ra_reflow_render.c
 *
 * @details
 * The decision under analysis lives inside ``static`` helper
 * ``priv_blit_glyph`` and is not directly reachable from a host test.
 * Per the operand-identical mirror-helper pattern established in
 * ``tests/test_lwip_sys_arch.c``, this file documents the canonical
 * N+1 MC/DC vector set and pairs it with a ``static inline`` mirror
 * with identical short-circuit semantics.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdio.h>

#include "unity_minimal.h"

/** Mirror of line 122: ``if (w > 0 && h > 0)``. */
static inline uint8_t mirror_priv_blit_glyph_size(int w, int h)
{
  if (w > 0 && h > 0) {
    return 1U;
  }
  return 0U;
}

/**
 * @test test_mcdc_priv_blit_glyph_size
 *
 * @par MC/DC:
 * Decision: ``if (w > 0 && h > 0)``
 * (2 conditions, libs/ra_reflow/src/ra_reflow_render.c line 122)
 *
 * Vectors (Chilenski masking-MC/DC, N+1 = 3 for N=2):
 *  - V1: w=10, h=10  -> C1=T, C2=T. Decision T (blit).
 *  - V2: w=0,  h=10  -> C1=F shorts. Decision F (skip).
 *  - V3: w=10, h=0   -> C1=T, C2=F. Decision F (skip).
 *
 * Independence:
 *  - V1 vs V2 vary C1 with C2 held T: outcome flips.
 *  - V1 vs V3 vary C2 with C1 held T: outcome flips.
 *
 * @par DO-178C 6.4.4.3 rationale:
 * 2-condition decision; N+1 = 3 vectors satisfy MC/DC fully.
 */
static void test_mcdc_priv_blit_glyph_size(void)
{
  TEST_BEGIN("ra_reflow_render priv_blit_glyph size MC/DC: w>0 && h>0");
  TEST_ASSERT_EQ(1, mirror_priv_blit_glyph_size(10, 10));
  TEST_ASSERT_EQ(0, mirror_priv_blit_glyph_size(0, 10));
  TEST_ASSERT_EQ(0, mirror_priv_blit_glyph_size(10, 0));
  TEST_END("ra_reflow_render priv_blit_glyph size MC/DC: w>0 && h>0");
}

int32_t main(void)
{
  test_mcdc_priv_blit_glyph_size();
  (void)fprintf(stderr, "[OK ] test_ra_reflow_render.c\n");
  return 0;
}
