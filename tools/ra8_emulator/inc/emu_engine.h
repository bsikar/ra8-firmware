/**
 * @file emu_engine.h
 * @brief Shared Unicorn engine access utilities for the board emulator
 *
 * @details
 * The tiny register/memory accessors every emulator module leans on: 32-bit
 * little-endian reads/writes of emulated memory (the PPB words the exception
 * and fault models poll/edit) and whole-register reads/writes by UC_ARM_REG_*
 * id, plus the ARM register-index -> Unicorn-register-id mapping table shared
 * by the instruction seams (conditional-select, LOB, long-shift, div-0). The
 * accessors are `static inline` so each including translation unit gets a
 * zero-overhead copy; the mapping table has one definition (emu_engine.c).
 *
 * Split out of the ra8_emulator main translation unit; behaviour unchanged.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>
#include <unicorn/unicorn.h>

#include "ra8_attributes.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @var k_arm_reg_id
 * @brief ARM register index (0..15) -> Unicorn register id.
 *
 * @details
 * Unicorn's UC_ARM_REG_* enum is NOT contiguous (UC_ARM_REG_R0 + n !=
 * UC_ARM_REG_Rn), so every instruction seam that decodes a 4-bit register
 * field maps it through this table. PC(15)/SP(13)/LR(14) are never CSx
 * destinations in practice but are mapped for completeness.
 *
 * @note Read-only lookup table; safe to share across every module.
 * @warning Do not index with a value above 15 (a decoded 4-bit field cannot
 *          produce one).
 * @since 0.1.0
 */
extern const int k_arm_reg_id[16];

/**
 * @brief Read a 32-bit little-endian word from emulated memory.
 *
 * @details Thin uc_mem_read wrapper; an unmapped/failed read yields 0, which
 * every caller treats as the reset-default value of the polled word.
 *
 * @param[in,out] uc   Unicorn engine to read from.
 * @param[in]     addr Emulated address of the word.
 * @return The 32-bit word at @p addr, or 0 on a failed read.
 * @pre @p uc is an initialised engine.
 * @pre @p addr is intended to be a mapped 4-byte word.
 * @post No engine state is modified beyond the read itself.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @since 0.1.0
 */
static inline uint32_t rd32(uc_engine* uc, uint64_t addr)
{
  uint32_t v = 0U;
  (void)uc_mem_read(uc, addr, &v, sizeof(v));
  return v;
}

/**
 * @brief Write a 32-bit little-endian word to emulated memory.
 *
 * @details Thin uc_mem_write wrapper; a failed write (unmapped address) is
 * ignored, matching the original tolerant PPB-word edit behaviour.
 *
 * @param[in,out] uc   Unicorn engine to write into.
 * @param[in]     addr Emulated address of the word.
 * @param[in]     v    Value to store.
 * @return Nothing.
 * @pre @p uc is an initialised engine.
 * @pre @p addr is intended to be a mapped 4-byte word.
 * @post On success the word at @p addr holds @p v.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @since 0.1.0
 */
static inline void wr32(uc_engine* uc, uint64_t addr, uint32_t v)
{
  (void)uc_mem_write(uc, addr, &v, sizeof(v));
}

/**
 * @brief Read a Unicorn 32-bit register by its UC_ARM_REG_* id.
 *
 * @details Thin uc_reg_read wrapper returning the value directly so callers
 * can use it in expressions; a failed read yields 0.
 *
 * @param[in,out] uc  Unicorn engine.
 * @param[in]     reg UC_ARM_REG_* register id.
 * @return The register's 32-bit value, or 0 on a failed read.
 * @pre @p uc is an initialised engine.
 * @pre @p reg names a 32-bit-readable register.
 * @post No engine state is modified beyond the read itself.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @since 0.1.0
 */
static inline uint32_t reg_get(uc_engine* uc, int reg)
{
  uint32_t v = 0U;
  (void)uc_reg_read(uc, reg, &v);
  return v;
}

/**
 * @brief Write a Unicorn 32-bit register by its UC_ARM_REG_* id.
 *
 * @details Thin uc_reg_write wrapper; a failed write is ignored, matching the
 * original tolerant register-edit behaviour.
 *
 * @param[in,out] uc  Unicorn engine.
 * @param[in]     reg UC_ARM_REG_* register id.
 * @param[in]     v   Value to store.
 * @return Nothing.
 * @pre @p uc is an initialised engine.
 * @pre @p reg names a 32-bit-writable register.
 * @post On success the register holds @p v.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @since 0.1.0
 */
static inline void reg_set(uc_engine* uc, int reg, uint32_t v)
{
  (void)uc_reg_write(uc, reg, &v);
}

/**
 * @brief Mark the pending engine stop as a zero-time seam relaunch.
 *
 * @details A C-seam that returned to the firmware's caller (a --fast-sd block
 * serve, an emulated armed divide) consumed no modelled time: the inner run
 * loop must relaunch from the returned PC WITHOUT advancing SysTick or
 * charging an outer chunk. The seam sets this latch right before stopping the
 * engine; the run loop consumes it via emu_seam_take_relaunch().
 *
 * @return Nothing.
 * @pre A seam is about to stop the engine after editing PC.
 * @pre The run loop is mid-chunk (single-threaded).
 * @post The next engine stop is treated as a zero-time relaunch.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @see emu_seam_take_relaunch()  The run-loop consumer.
 * @since 0.1.0
 */
RA8_PRIV void emu_seam_request_relaunch(void);

/**
 * @brief Consume the zero-time seam-relaunch latch.
 *
 * @details Returns whether the latch was set and clears it, so the run loop
 * can `continue` at the returned PC without advancing modelled time. Exactly
 * one consumer (the inner run loop) per stop.
 *
 * @return true if a seam requested a zero-time relaunch since the last take.
 * @retval false No seam relaunch is pending (normal stop handling proceeds).
 * @pre The engine just stopped (uc_emu_start returned).
 * @pre The run loop is the only caller.
 * @post The latch is clear.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @see emu_seam_request_relaunch()  The seam-side producer.
 * @since 0.1.0
 */
RA8_PRIV bool emu_seam_take_relaunch(void);

#ifdef __cplusplus
}
#endif
