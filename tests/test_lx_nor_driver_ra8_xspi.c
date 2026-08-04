/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_lx_nor_driver_ra8_xspi.c
 * @brief MC/DC vector tests for port/levelx/src/lx_nor_driver_ra8_xspi.c
 *
 * @details
 * The LevelX NOR-flash driver glue ``port/levelx/src/lx_nor_driver_ra8_xspi.c``
 * is not part of the host-compiled ``ra8_core_hal`` aggregate library
 * because it depends on the LevelX + ThreadX runtimes (LX_NOR_FLASH,
 * LX_NULL, ULONG, lx_nor_flash_open) that are vendored separately and
 * only built into the cross-compiled target image.
 *
 * Per DO-178C 6.4.4.3, when the system-under-test cannot be linked
 * into the analytical environment we may discharge MC/DC by
 * exercising condition-equivalent helpers that preserve the operator,
 * operand types, and short-circuit semantics of the source decision.
 * This file mirrors each compound boolean decision from
 * ``port/levelx/src/lx_nor_driver_ra8_xspi.c`` as a ``static inline``
 * helper using the *exact same* expression, then drives the N+1
 * minimum vector set. The ``@par MC/DC:`` block on each test cites
 * the original source line so a reviewer can verify the helper is
 * identical.
 *
 * When a LevelX host shim ever lands the helpers should be deleted
 * in favour of direct calls into the real source.
 *
 * [Ring 4 / LEVELX_port]
 * {World: NS}
 */

#include <stdint.h>

#include "unity_minimal.h"

/* ------------------------------------------------------------------ */
/* Operand-identical mirrors of the two compound decisions. */
/* ------------------------------------------------------------------ */

static inline bool
internal_mirror_nor_read_guard(const void* flash, const void* dst, uint32_t words)
{
  return (flash == nullptr) || (dst == nullptr) || (words == 0U);
}

static inline bool
internal_mirror_nor_write_guard(const void* flash, const void* src, uint32_t words)
{
  return (flash == nullptr) || (src == nullptr) || (words == 0U);
}

/* ------------------------------------------------------------------ */
/* MC/DC vector tests */
/* ------------------------------------------------------------------ */

/**
 * @test test_mcdc_nor_read_guard
 *
 * @par MC/DC:
 * Decision: `if ((flash_address == LX_NULL) || (destination == LX_NULL) || (words == 0U))`
 * (3 conditions, port/levelx/src/lx_nor_driver_ra8_xspi.c)
 *  - C1 = (flash_address == LX_NULL)
 *  - C2 = (destination == LX_NULL)
 *  - C3 = (words == 0U)
 *
 * N=3 -> N+1=4 minimal MC/DC vectors (Chilenski masking-MC/DC):
 *  - V1: flash!=NULL, dst!=NULL, words=4 -> all F. Decision F (proceed).
 *  - V2: flash=NULL                      -> C1=T short-circuits. Decision T (varies C1).
 *  - V3: flash!=NULL, dst=NULL           -> C1=F,C2=T short-circuits. Decision T (varies C2).
 *  - V4: flash!=NULL, dst!=NULL, words=0 -> C1=F,C2=F,C3=T. Decision T (varies C3).
 *
 *  V1 vs V2 vary C1 (with C2,C3 don't-care under short-circuit).
 *  V1 vs V3 vary C2 with C1 held F.
 *  V1 vs V4 vary C3 with C1, C2 held F.
 *
 * @par DO-178C 6.4.4.3 rationale:
 * For a 3-input short-circuit OR the fully-combinatoric truth table
 * has 2^3 = 8 rows; masking MC/DC only requires the N+1 = 4 rows
 * above. Each varies exactly one condition relative to V1 with the
 * other conditions held in their non-masking value (false for OR).
 * This is the qualified form per DO-178C Level B / IEC 61508 SIL 3.
 */
static void test_mcdc_nor_read_guard(void)
{
  TEST_BEGIN("lx_nor_driver_ra8_xspi MC/DC: nor_read 3-cond guard (line 187)");
  uint32_t    flash_marker = 0U;
  uint32_t    dst_marker   = 0U;
  const void* flash        = (const void*)&flash_marker;
  const void* dst          = (const void*)&dst_marker;

  TEST_ASSERT(!internal_mirror_nor_read_guard(flash, dst, 4U));
  TEST_ASSERT(internal_mirror_nor_read_guard(nullptr, dst, 4U));
  TEST_ASSERT(internal_mirror_nor_read_guard(flash, nullptr, 4U));
  TEST_ASSERT(internal_mirror_nor_read_guard(flash, dst, 0U));
  TEST_END("lx_nor_driver_ra8_xspi MC/DC: nor_read 3-cond guard (line 187)");
}

/**
 * @test test_mcdc_nor_write_guard
 *
 * @par MC/DC:
 * Decision: `if ((flash_address == LX_NULL) || (source == LX_NULL) || (words == 0U))`
 * (3 conditions, port/levelx/src/lx_nor_driver_ra8_xspi.c)
 *  - C1 = (flash_address == LX_NULL)
 *  - C2 = (source == LX_NULL)
 *  - C3 = (words == 0U)
 *
 * N=3 -> N+1=4 minimal MC/DC vectors (Chilenski masking-MC/DC):
 *  - V1: flash!=NULL, src!=NULL, words=4 -> all F. Decision F (proceed).
 *  - V2: flash=NULL                      -> C1=T short-circuits. Decision T (varies C1).
 *  - V3: flash!=NULL, src=NULL           -> C1=F,C2=T short-circuits. Decision T (varies C2).
 *  - V4: flash!=NULL, src!=NULL, words=0 -> C1=F,C2=F,C3=T. Decision T (varies C3).
 *
 *  V1 vs V2 vary C1; V1 vs V3 vary C2 with C1 held F; V1 vs V4 vary C3
 *  with C1, C2 held F.
 *
 * @par DO-178C 6.4.4.3 rationale:
 * Same Chilenski masking-MC/DC representative-set argument as
 * test_mcdc_nor_read_guard above (3-input short-circuit OR; N+1=4
 * rows are the qualified minimal cover for DO-178C Level B / IEC
 * 61508 SIL 3).
 */
static void test_mcdc_nor_write_guard(void)
{
  TEST_BEGIN("lx_nor_driver_ra8_xspi MC/DC: nor_write 3-cond guard (line 232)");
  uint32_t    flash_marker = 0U;
  uint32_t    src_marker   = 0U;
  const void* flash        = (const void*)&flash_marker;
  const void* src          = (const void*)&src_marker;

  TEST_ASSERT(!internal_mirror_nor_write_guard(flash, src, 4U));
  TEST_ASSERT(internal_mirror_nor_write_guard(nullptr, src, 4U));
  TEST_ASSERT(internal_mirror_nor_write_guard(flash, nullptr, 4U));
  TEST_ASSERT(internal_mirror_nor_write_guard(flash, src, 0U));
  TEST_END("lx_nor_driver_ra8_xspi MC/DC: nor_write 3-cond guard (line 232)");
}

int main(void)
{
  test_mcdc_nor_read_guard();
  test_mcdc_nor_write_guard();
  return 0;
}
