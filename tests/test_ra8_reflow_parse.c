/**
 * @file test_ra8_reflow_parse.c
 * @brief MC/DC vectors for libs/ra8_reflow/src/ra8_reflow_parse.c
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdio.h>

#include "ra8_err.h"
#include "ra8_reflow.h"
#include "unity_minimal.h"

/* The reflow engine struct is large (>64 KiB). We allocate it as a
 * file-scope static so the test stack stays small. */
static ra8_reflow_t s_engine;

/**
 * @test test_mcdc_reflow_parse_xhtml_null_guard
 *
 * @par MC/DC:
 * Decision: ``if (engine == NULL || xhtml_buf == NULL)``
 * (2 conditions, libs/ra8_reflow/src/ra8_reflow_parse.c line 62)
 *
 * Vectors (Chilenski masking-MC/DC, N+1 = 3 for N=2):
 *  - V1: engine=&e, xhtml_buf=&buf (both non-NULL)
 *        -> C1=F, C2=F. Decision F (proceeds to next check).
 *  - V2: engine=NULL, xhtml_buf=&buf
 *        -> C1=T shorts. Decision T -> k_ra8_err_null_ptr.
 *  - V3: engine=&e, xhtml_buf=NULL
 *        -> C1=F, C2=T. Decision T -> k_ra8_err_null_ptr.
 *
 * Independence:
 *  - V1 vs V2 vary C1 with C2 held F: outcome flips.
 *  - V1 vs V3 vary C2 with C1 held F: outcome flips.
 *
 * Note: V1 returns ``k_ra8_err_not_initialized`` (the next guard), not
 * ``k_ra8_ok``, because s_engine.in_use == 0. The MC/DC obligation
 * concerns the line-62 decision only; downstream checks are out of
 * scope for this vector set.
 *
 * @par DO-178C 6.4.4.3 rationale:
 * 2-condition decision; N+1 = 3 vectors satisfy MC/DC fully.
 */
static void test_mcdc_reflow_parse_xhtml_null_guard(void)
{
  TEST_BEGIN("ra8_reflow_parse_xhtml MC/DC: engine NULL || xhtml_buf NULL");
  const uint8_t buf[1] = {0x20U};

  /* V1: both non-NULL. Decision F. Engine not initialized so the next
   * guard returns not_initialized; the line-62 decision is exercised
   * with outcome F. */
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_reflow_parse_xhtml(&s_engine, buf, 1U));

  /* V2: engine NULL -> decision T -> null_ptr. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_reflow_parse_xhtml(nullptr, buf, 1U));

  /* V3: xhtml_buf NULL -> decision T -> null_ptr. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_reflow_parse_xhtml(&s_engine, nullptr, 1U));

  TEST_END("ra8_reflow_parse_xhtml MC/DC: engine NULL || xhtml_buf NULL");
}

int32_t main(void)
{
  test_mcdc_reflow_parse_xhtml_null_guard();
  (void)fprintf(stderr, "[OK ] test_ra8_reflow_parse.c\n");
  return 0;
}
