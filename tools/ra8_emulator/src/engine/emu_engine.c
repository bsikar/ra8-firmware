/**
 * @file emu_engine.c
 * @brief Shared engine-access table definition (see emu_engine.h)
 *
 * @details
 * Holds the one definition of the ARM register-index -> Unicorn register-id
 * mapping the instruction seams share; the inline accessors live entirely in
 * emu_engine.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "emu_engine.h"

const int k_arm_reg_id[16] = {
  UC_ARM_REG_R0,
  UC_ARM_REG_R1,
  UC_ARM_REG_R2,
  UC_ARM_REG_R3,
  UC_ARM_REG_R4,
  UC_ARM_REG_R5,
  UC_ARM_REG_R6,
  UC_ARM_REG_R7,
  UC_ARM_REG_R8,
  UC_ARM_REG_R9,
  UC_ARM_REG_R10,
  UC_ARM_REG_R11,
  UC_ARM_REG_R12,
  UC_ARM_REG_SP,
  UC_ARM_REG_LR,
  UC_ARM_REG_PC,
};

/**
 * @var s_seam_relaunch
 * @brief Set by a C-seam to relaunch WITHOUT consuming a chunk.
 * @details A block-serve or emulated-instruction seam stops emulation to
 *          return to the caller; left alone the inner run loop would treat
 *          that clean stop as a full instruction budget elapsing and advance
 *          SysTick + charge a chunk. This latch marks the stop as a zero-time
 *          seam relaunch -- the inner loop just re-enters at the returned PC,
 *          exactly like the exception-return path.
 * @note Single-threaded run loop only; cleared by each take.
 * @warning Producers must stop the engine right after setting it.
 * @since 0.1.0
 */
static bool s_seam_relaunch;

/** @brief Implementation of `emu_seam_request_relaunch()` -- set the latch. */
void emu_seam_request_relaunch(void)
{
  s_seam_relaunch = true;
}

/** @brief Implementation of `emu_seam_take_relaunch()` -- read + clear. */
bool emu_seam_take_relaunch(void)
{
  const bool hit  = s_seam_relaunch;
  s_seam_relaunch = false;
  return hit;
}
