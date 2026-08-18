/**
 * @file test_ra8_board_ek_ra8d2_dualcore.c
 * @brief Unit tests for the board-declared M85 / M33 shared-RAM window
 *
 * @details
 * Drives both arms of the only decision in
 * ``libs/ra8_board_ek_ra8d2/src/ra8_board_ek_ra8d2_dualcore.c`` and pins the
 * published map against the numbers the linker scripts and the boot MPU use.
 * The map assertions matter as much as the null guard: the point of the
 * declaration is that seven applications stop carrying their own copy of these
 * addresses, so a silent change to one of them has to fail here rather than at
 * the first cross-core hand-off on silicon.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/**
 * @enum board_dualcore_fixture_t
 * @brief The dual-core map as the linker scripts and the boot MPU spell it.
 *
 * @details
 * Written out as literals ON PURPOSE. A test that re-derived these from the
 * header would agree with any value the header happened to hold, including a
 * wrong one; stating them independently is what makes the comparison a check.
 * The sources are ``linker_script.ld`` (SRAM at 0x22000000, 1024 KiB, so CPU0
 * ends at 0x22100000), ``linker_script_cpu1.ld`` (SRAM_CPU1 at 0x22190000,
 * 64 KiB; MRAM_CPU1 at 0x020C0000, 256 KiB), and MPU region 4 in the board's
 * ``boot/system_init.c``.
 */
typedef enum : uint32_t {
  k_bd_shared_base = 0x22100000UL, /**< SRAM2: where CPU0's allocation ends.  */
  k_bd_shared_size = 0x90000UL,    /**< 576 KiB, up to CPU1's private bank.   */
  k_bd_cpu1_sram   = 0x22190000UL, /**< CPU1's 64 KiB bank, top of on-chip.   */
  k_bd_cpu1_size   = 0x10000UL,    /**< 64 KiB.                               */
  k_bd_cpu1_image  = 0x020C0000UL, /**< MRAM_CPU1: the pinned M33 image.      */
  k_bd_image_size  = 0x40000UL,    /**< 256 KiB.                              */
  k_bd_poison_size = 0xDEADU,      /**< Written into the out-parameter first. */
} board_dualcore_fixture_t;

/**
 * @test board_shared_ram_rejects_null
 *
 * @brief Verify a null descriptor pointer is refused.
 *
 * @return Nothing.
 * @pre The board dual-core unit is linked into the test binary.
 * @post No caller-visible state is modified.
 * @note Single-threaded host test.
 * @since 0.1.0
 *
 * @par MC/DC:
 * Decision: `if ((out) == nullptr)` inside `RA8_CHECK_NULL_PTR` (1 condition).
 * - Vector 1: out=NULL -> true  (this test)
 * - Vector 2: out=&win -> false (the test below)
 * N+1 = 2 vectors for N=1: minimal MC/DC.
 */
RA8_INTERNAL static void internal_test_rejects_null(void)
{
  TEST_BEGIN("board_shared_ram: null descriptor rejected");
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_board_shared_ram(nullptr));
  TEST_END("board_shared_ram: null descriptor rejected");
}

/**
 * @test board_shared_ram_publishes_the_window
 *
 * @brief Verify the published window is the one both linker scripts leave free.
 *
 * @details
 * Checks the descriptor against independently written literals, and checks the
 * two structural properties an application relies on: the window starts where
 * CPU0's allocation ends, and it stops exactly where CPU1's private bank
 * begins, so a buffer carved from it cannot reach the M33's stack.
 *
 * @return Nothing.
 * @pre The board dual-core unit is linked into the test binary.
 * @post The caller-owned descriptor is fully overwritten.
 * @note Single-threaded host test.
 * @since 0.1.0
 *
 * @par MC/DC:
 * Supplies the false vector for the unit's null guard; paired with the test
 * above, the descriptor pointer is shown to independently decide the outcome.
 */
RA8_INTERNAL static void internal_test_publishes_the_window(void)
{
  TEST_BEGIN("board_shared_ram: publishes the CPU0/CPU1 window");
  ra8_board_shared_ram_t win = {.base = nullptr, .size_bytes = (uint32_t)k_bd_poison_size};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_shared_ram(&win));
  TEST_ASSERT_EQ((uintptr_t)k_bd_shared_base, (uintptr_t)win.base);
  TEST_ASSERT_EQ(k_bd_shared_size, win.size_bytes);
  /* The window must butt up against CPU1's bank exactly -- a gap would waste
   * SRAM, an overlap would let a carve-out land in the M33's stack. */
  TEST_ASSERT_EQ((uintptr_t)k_bd_cpu1_sram, (uintptr_t)win.base + win.size_bytes);
  TEST_END("board_shared_ram: publishes the CPU0/CPU1 window");
}

/**
 * @test board_dualcore_map_matches_the_linker_scripts
 *
 * @brief Verify the compile-time map still agrees with the linker scripts.
 *
 * @details
 * The constants are consumed directly by applications that place a statically
 * addressed mailbox, and by the boot MPU that marks the window non-cacheable.
 * They are only correct as long as they match the two linker scripts, which
 * this test restates independently.
 *
 * @return Nothing.
 * @pre The board headers are on the include path.
 * @post Nothing is modified.
 * @note Single-threaded host test.
 * @since 0.1.0
 *
 * @par MC/DC:
 * No decision under test -- this is an equality check on compile-time
 * constants, so there is no condition to vary.
 */
RA8_INTERNAL static void internal_test_map_matches_linker_scripts(void)
{
  TEST_BEGIN("board dual-core map: agrees with both linker scripts");
  TEST_ASSERT_EQ((uintptr_t)k_bd_shared_base, (uintptr_t)k_ra8_board_shared_ram_base);
  TEST_ASSERT_EQ(k_bd_shared_size, k_ra8_board_shared_ram_size_bytes);
  TEST_ASSERT_EQ((uintptr_t)k_bd_cpu1_sram, (uintptr_t)k_ra8_board_cpu1_sram_base);
  TEST_ASSERT_EQ(k_bd_cpu1_size, k_ra8_board_cpu1_sram_size_bytes);
  TEST_ASSERT_EQ((uintptr_t)k_bd_cpu1_image, (uintptr_t)k_ra8_board_cpu1_image_base);
  TEST_ASSERT_EQ(k_bd_image_size, k_ra8_board_cpu1_image_size_bytes);
  TEST_END("board dual-core map: agrees with both linker scripts");
}

/**
 * @brief Test binary entry point.
 *
 * @details No ordering dependency: the unit holds no mutable state.
 *
 * @return 0 on success; a failing assertion exits before this returns.
 *
 * @pre The test framework has started.
 * @post Both arms of the unit's only decision have been driven.
 * @note Not thread-safe; single-threaded test runner.
 * @since 0.1.0
 */
int main(void)
{
  internal_test_rejects_null();
  internal_test_publishes_the_window();
  internal_test_map_matches_linker_scripts();
  return 0;
}
