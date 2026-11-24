/**
 * @file examples/ek_ra8d2/hw_validated/hil/dfu_bootloader/payload/confirm_payload.c
 * @brief Sample copy-to-run payload that runs a self-test and calls ::ra8_dfu_confirm.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Demonstrates the payload-side trial confirmation contract on EK-RA8D2:
 * linked once at ::k_ra8_dfu_run_base (`0x22020000`, SRAM), executes a
 * bounded self-test, calls ::ra8_dfu_confirm() only after that self-test
 * passes, and latches the confirm status word and advancing heartbeat in the
 * bootloader-shared probe region (`0x22010000..0x2201000C`) before looping.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdbool.h>
#include <stdint.h>

#include "ra8_dfu.h"
#include "ra8_err.h"
#include "ra8_wdt.h"

/**
 * @enum confirm_payload_probe_t
 * @brief Fixed SRAM probe words shared with J-Link / emulator inspection.
 */
typedef enum : uintptr_t {
  k_confirm_probe_sentinel = 0x22010000U, /**< Proof-of-run magic word.     */
  k_confirm_probe_counter  = 0x22010004U, /**< Free-running heartbeat word. */
  k_confirm_probe_err      = 0x22010008U, /**< Returned ra8_err_t from API. */
} confirm_payload_probe_t;

/** @brief Payload probe magic and bounded memory-test constants. */
typedef enum : uint32_t {
  k_confirm_sentinel_ok   = 0xC0DF124DU, /**< Confirmed after self-test OK. */
  k_confirm_sentinel_fail = 0xFA11DEADU, /**< Self-test or confirm failed.  */
  k_confirm_scratch_seed  = 0xA55A5AA5U, /**< SRAM scratch test word.       */
  k_confirm_stack_align   = 0x7U,        /**< AAPCS 8-byte SP mask.         */
} confirm_payload_const_t;

extern uint32_t g_ra8_ls_stack_top;
extern uint32_t g_payload_sbss;
extern uint32_t g_payload_ebss;

/**
 * @brief Zero-initialize `.bss` before running application C/Zig logic.
 * @pre Copy-to-run copied the binary (incl. `.vectors`, `.text`, `.data`) to SRAM.
 * @post All words in `[g_payload_sbss, g_payload_ebss)` hold 0.
 * @note Bounded by the linker `.bss` extent. @since 0.1.0
 */
static void payload_zero_bss(void)
{
  for (uint32_t* p = &g_payload_sbss; p < &g_payload_ebss; ++p) {
    *p = 0U;
  }
}

/**
 * @brief Minimal payload self-test: verify SRAM execution window, SP, and scratch RAM.
 * @return `true` iff all post-boot self-checks succeed.
 * @pre Running at ::k_ra8_dfu_run_base with valid MSP.
 * @post No persistent state mutated.
 * @note Bounded; deterministic. @since 0.1.0
 */
static bool payload_self_test(void)
{
  const uintptr_t pc_hint = (uintptr_t)&payload_self_test;
  if (pc_hint < (uintptr_t)k_ra8_dfu_run_base) {
    return false;
  }
  const uint32_t sp_val = (uint32_t)(uintptr_t)&pc_hint;
  if ((sp_val & (uint32_t)k_confirm_stack_align) != 0U) {
    return false;
  }
  volatile uint32_t scratch = (uint32_t)k_confirm_scratch_seed;
  if ((scratch ^ (uint32_t)k_confirm_scratch_seed) != 0U) {
    return false;
  }
  return true;
}

/**
 * @brief Quiet log sink needed by `ra8_flash.c` when built freestanding.
 * @param[in] tag     Component tag (unused).
 * @param[in] message Message text (unused).
 * @pre None.
 * @post No state mutated.
 * @note Freestanding weak sink for minimal copy-to-run binary. @since 0.1.0
 */
[[gnu::weak]] void ra8_log_emit_error(const char* tag, const char* message);
void               ra8_log_emit_error(const char* tag, const char* message)
{
  (void)tag;
  (void)message;
}

/**
 * @brief Quiet value log sink needed by `ra8_flash.c` when built freestanding.
 * @param[in] tag     Component tag (unused).
 * @param[in] message Message text (unused).
 * @param[in] val     Associated numeric value (unused).
 * @pre None.
 * @post No state mutated.
 * @note Freestanding weak sink for minimal copy-to-run binary. @since 0.1.0
 */
[[gnu::weak]] void ra8_log_emit_error_val(const char* tag, const char* message, uint32_t val);
void               ra8_log_emit_error_val(const char* tag, const char* message, uint32_t val)
{
  (void)tag;
  (void)message;
  (void)val;
}

/**
 * @brief Payload entry: zero BSS, run self-test, confirm trial slot, spin safely.
 * @return Does not return.
 * @pre Launched via copy-to-run at ::k_ra8_dfu_run_base.
 * @post Trial slot confirmed in MRAM if ::payload_self_test() succeeded.
 * @note Non-static so `ENTRY(payload_reset)` resolves cleanly. @since 0.1.0
 */
[[noreturn]] void payload_reset(void);

void payload_reset(void)
{
  payload_zero_bss();

  volatile uint32_t* const sentinel = (volatile uint32_t*)(uintptr_t)k_confirm_probe_sentinel;
  volatile uint32_t* const counter  = (volatile uint32_t*)(uintptr_t)k_confirm_probe_counter;
  volatile uint32_t* const err_word = (volatile uint32_t*)(uintptr_t)k_confirm_probe_err;

  *counter = 0U;
  if (!payload_self_test()) {
    *err_word = (uint32_t)k_ra8_err_validation_failed;
    *sentinel = (uint32_t)k_confirm_sentinel_fail;
    for (;;) {
      __asm__ volatile("wfi");
    }
  }

  const ra8_err_t confirm_err = ra8_dfu_confirm();
  *err_word                   = (uint32_t)confirm_err;
  *sentinel =
    (confirm_err == k_ra8_ok) ? (uint32_t)k_confirm_sentinel_ok : (uint32_t)k_confirm_sentinel_fail;

  /* NASA Rule 2 exemption: terminal leaf payload heartbeat loop; refreshes WDT0
   * if trial watchdog was armed by the bootloader before hand-off. */
  for (;;) {
    (void)ra8_wdt_refresh_for(k_ra8_wdt0);
    *counter = *counter + 1U;
  }
}

/** @brief 2-entry vector table placed at ::k_ra8_dfu_run_base. */
[[gnu::section(".vectors"), gnu::used]] static const uintptr_t s_vectors[2] = {
  (uintptr_t)&g_ra8_ls_stack_top,
  (uintptr_t)&payload_reset,
};
