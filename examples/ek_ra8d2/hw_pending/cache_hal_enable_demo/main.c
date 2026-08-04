/**
 * @file examples/ek_ra8d2/hw_pending/cache_hal_enable_demo/main.c
 * @brief Cortex-M85 L1 cache brought up through the ra8_cache HAL (issue #577).
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Single-core (Cortex-M85) HIL demo that boots with the L1 I-cache + D-cache
 * enabled **through the ra8_cache HAL primitives** rather than the shared boot's
 * hand-rolled register pokes. The app's build defines both
 * ``RA8_BOOT_ENABLE_CACHE_MPU`` and ``RA8_BOOT_CACHE_VIA_HAL`` (see
 * ``CMakeLists.txt``), so the shared ``SystemInit()`` programmes the 5-region MPU
 * and then calls ``ra8_cache_icache_enable()`` / ``ra8_cache_dcache_enable()``
 * (each of which runs its architectural invalidate before setting the CCR enable
 * bit) *before* ``main()`` runs. This is the Liskov-equivalent of the raw-poke
 * ``cache_mpu_hil`` boot: the same ICIALLU + CCR.IC / CCR.DC sequence, now lifted
 * into the HAL. The raw ``internal_enable_icache`` / ``internal_enable_dcache``
 * helpers stay compiled as the reference path and are simply not called here.
 *
 * Self-test (runs once, then the core parks in WFI):
 *
 *   1. **Cacheable RW (M85-private SRAM @ 0x22000000, MPU region 1).** Fill a
 *      4 KiB static buffer (in ``.bss``, region 1) with a deterministic affine
 *      pattern ``buf[i] = (uint8_t)(i * 31 + 7)``, clean+invalidate those (now
 *      dirty) D-cache lines back to SRAM via ``ra8_cache_dcache_clean_invalidate_by_addr()``,
 *      then read every byte back -- the read misses the cache and refills from
 *      memory, exercising the full write-back + refill path with the HAL-enabled
 *      D-cache live.
 *
 * On success the app emits ``"cache_hal_enable_demo: L1-cache-via-HAL PASS\r\n"``
 * over the J-Link OB VCOM console (SCI8, PD02/PD03 @ 115200 8N1) and mirrors the
 * verdict over ``ra8_log`` (the emulator echoes it as an ``[itm]`` line). On any
 * mismatch it emits a distinct ``... FAIL`` line (never containing "PASS") and
 * parks. Every path ends in WFI so ``ra8_emulator``'s ``RA8_EMU_IDLE_STOP``
 * terminates the run after the one-shot banner is scraped.
 *
 * @note ``ra8_emulator`` models memory byte-exact and does not model the L1
 *       D-cache, so the cacheable-RW step passes there trivially; the point of
 *       the emulator run is to prove the app boots and reports PASS with the
 *       HAL-driven cache boot path compiled in. The cache hazard is only real on
 *       silicon.
 *
 * @author Brighton Sikarskie
 * @date 2026-08-02
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cache.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_log.h"

/**
 * @enum cache_hal_config_t
 * @brief Compile-time scalar parameters for the cache-via-HAL self-test.
 * @details Groups the console baud and the cacheable buffer size the round-trip
 *          fills, cleans, and reads back.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_cache_hal_baud      = 115200U, /**< VCOM console line rate (8N1).               */
  k_cache_hal_buf_bytes = 4096U,   /**< Cacheable self-test buffer size (>= 4 KiB). */
  k_cache_hal_buf_min   = 4096U,   /**< Self-test buffer floor (4 KiB minimum).     */
} cache_hal_config_t;

/**
 * @enum cache_hal_pattern_t
 * @brief Affine byte-pattern coefficients: ``buf[i] = i * MUL + ADD``.
 * @details A per-index affine pattern makes every byte distinct modulo 256, so a
 *          stuck or swapped line is caught by the readback compare.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_cache_hal_pat_mul = 31U, /**< Pattern multiplier. */
  k_cache_hal_pat_add = 7U,  /**< Pattern addend.     */
} cache_hal_pattern_t;

static_assert((uint32_t)k_cache_hal_buf_bytes >= (uint32_t)k_cache_hal_buf_min,
              "cacheable self-test buffer must be at least 4 KiB");

/**
 * @var s_cache_hal_buf
 * @brief 4 KiB cacheable scratch buffer in M85-private SRAM (MPU region 1).
 * @details Lives in ``.bss`` (SRAM @ 0x22000000, cacheable per MPU region 1).
 *          Cache-line aligned (32 bytes) so the clean+invalidate covers whole
 *          lines with no partial-line surprises.
 * @note Mutated only by the cacheable-RW self-test on the single boot thread.
 * @warning Not for cross-core use -- region 1 is M85-private cacheable.
 * @since 0.1.0
 */
alignas(32) static uint8_t s_cache_hal_buf[k_cache_hal_buf_bytes];

/**
 * @var k_cache_hal_pass_banner
 * @brief Deterministic one-shot HIL success banner (uart_scrape gate).
 * @details Emitted only on the pass path over the VCOM console. The trailing
 *          CRLF terminates the line on the wire; the HIL gate matches the text
 *          exactly. Contains "PASS" and is not a substring of any FAIL banner.
 * @warning Do not modify; ``hil.conf`` ``HIL_EXPECT`` matches it verbatim.
 * @since 0.1.0
 */
static const uint8_t k_cache_hal_pass_banner[] = "cache_hal_enable_demo: L1-cache-via-HAL PASS\r\n";

/**
 * @var k_cache_hal_fail_setup
 * @brief Failure banner: clock / console bring-up failed before the self-test.
 * @details Distinct from the PASS banner and free of "PASS"; matched by the
 *          ``hil.conf`` ``HIL_EXPECT_NEGATIVE`` "FAIL" alternative.
 * @since 0.1.0
 */
static const uint8_t k_cache_hal_fail_setup[] = "cache_hal_enable_demo: setup FAIL\r\n";

/**
 * @var k_cache_hal_fail_rw
 * @brief Failure banner: the cacheable-SRAM readback mismatched.
 * @details Distinct from the PASS banner and free of "PASS".
 * @since 0.1.0
 */
static const uint8_t k_cache_hal_fail_rw[] = "cache_hal_enable_demo: cacheable-RW FAIL\r\n";

/**
 * @brief Park the Cortex-M85 forever in a WFI idle loop.
 *
 * @details Reached after the one-shot banner has been emitted (pass or fail).
 * The WFI lets ``ra8_emulator``'s idle detector stop the run cleanly and models
 * the low-power posture on silicon.
 *
 * @return This function never returns.
 *
 * @pre The self-test verdict has been emitted over the console.
 * @pre No further forward progress is required of the M85.
 * @post The core makes no further architectural progress.
 * @post Any pending console bytes have already been flushed by the caller.
 *
 * @note Not thread-safe; single-threaded boot context.
 * @since 0.1.0
 */
[[noreturn]] static void cache_hal_wfi_forever(void)
{
  while (1) {
    __asm volatile("wfi");
  }
}

/**
 * @brief Prove cacheable RW works with the HAL-enabled D-cache (SRAM region 1).
 *
 * @details Fills ::s_cache_hal_buf with the affine pattern, cleans + invalidates
 * the now-dirty D-cache lines back to SRAM through the HAL by-address primitive,
 * then verifies every byte on read-back so the refill path from memory is
 * exercised with the D-cache (enabled at boot via ``ra8_cache_dcache_enable()``)
 * live.
 *
 * @return Whether the buffer read back exactly as written.
 * @retval true  Every byte matched the pattern after the cache round-trip.
 * @retval false A byte mismatched, or the cache maintenance call failed.
 *
 * @pre The D-cache + MPU are enabled (HAL cache boot path active).
 * @pre ::s_cache_hal_buf lies in cacheable SRAM (MPU region 1).
 * @post On true the buffer holds the affine pattern in SRAM.
 * @post No memory outside ::s_cache_hal_buf is modified.
 *
 * @note Not thread-safe; single-threaded boot context.
 * @since 0.1.0
 */
static bool cache_hal_test_cacheable_rw(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_cache_hal_buf_bytes; i++) {
    s_cache_hal_buf[i] =
      (uint8_t)((i * (uint32_t)k_cache_hal_pat_mul) + (uint32_t)k_cache_hal_pat_add);
  }

  /* Force the freshly written (dirty) lines back to SRAM and drop them, so the
   * verify pass below must refill from memory -- the full write-back + refill
   * round-trip with the HAL-enabled D-cache on. */
  if (ra8_cache_dcache_clean_invalidate_by_addr(s_cache_hal_buf,
                                                (uint32_t)sizeof(s_cache_hal_buf)) != k_ra8_ok) {
    return false;
  }

  for (uint32_t i = 0U; i < (uint32_t)k_cache_hal_buf_bytes; i++) {
    const uint8_t expected =
      (uint8_t)((i * (uint32_t)k_cache_hal_pat_mul) + (uint32_t)k_cache_hal_pat_add);
    if (s_cache_hal_buf[i] != expected) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Bring up the clock tree and the VCOM console for the banner.
 *
 * @details ``ra8_cgc_init()`` programmes the FSP-quickstart clock tree and
 * publishes PCLKA; the EK-RA8D2 debug console (SCI8 on PD02/PD03 @
 * ::k_cache_hal_baud) then comes up over the J-Link OB VCOM bridge.
 *
 * @return Whether the clock + console are ready to carry the banner.
 * @retval true  CGC and SCI8 console are up.
 * @retval false A bring-up step failed.
 *
 * @pre Called once during M85 bring-up, before the self-test.
 * @pre ``ra8_log_init()`` has run (failures are narrated over ITM).
 * @post On true SCI8 is enabled and PD02/PD03 route to it.
 * @post On false no console state persists.
 *
 * @note Not thread-safe; single-threaded boot context.
 * @since 0.1.0
 */
static bool cache_hal_setup(void)
{
  if (ra8_cgc_init() != k_ra8_ok) {
    return false;
  }
  if (ra8_board_uart_console_init((uint32_t)k_cache_hal_baud) != k_ra8_ok) {
    return false;
  }
  return true;
}

/**
 * @brief Emit one banner line over the VCOM console and flush it.
 *
 * @details Writes @p line then drains the SCI8 TX so the bytes clock out before
 * the core parks. A no-op if @p line is NULL or @p len is 0, or if the console
 * never came up (the write returns an error that is ignored here).
 *
 * @param[in] line Pointer to the ASCII banner bytes (no NUL sent). Must be
 *                 non-NULL for output.
 * @param[in] len  Number of bytes to send; 0 sends nothing.
 *
 * @return Nothing.
 *
 * @pre @p line points to at least @p len readable bytes when @p len > 0.
 * @pre ::cache_hal_setup was attempted during bring-up.
 * @post The bytes have been handed to SCI8 and the TX FIFO drained (if up).
 * @post No application state is modified.
 *
 * @note Not thread-safe; single-threaded boot context.
 * @since 0.1.0
 */
static void cache_hal_emit(const uint8_t* line, size_t len)
{
  if (line == nullptr) {
    return;
  }
  if (len == 0U) {
    return;
  }
  (void)ra8_board_uart_console_write(line, len);
  (void)ra8_board_uart_console_flush();
}

/**
 * @brief Application entry: prove the HAL-enabled L1 caches compute correctly.
 *
 * @details Brings up logging, the clock tree, and the VCOM console, runs the
 * cacheable-SRAM round-trip with the D-cache that ``SystemInit()`` enabled
 * through ``ra8_cache_dcache_enable()``, emits the matching PASS / FAIL banner
 * over the console and ``ra8_log``, then parks in WFI. Every byte the self-test
 * touches runs with the L1 caches + MPU enabled by the shared boot
 * (``RA8_BOOT_ENABLE_CACHE_MPU`` + ``RA8_BOOT_CACHE_VIA_HAL``).
 *
 * @return Never returns (ends in ::cache_hal_wfi_forever).
 * @retval (none) Control stays in the WFI idle loop.
 *
 * @pre ``Reset_Handler`` has copied ``.data`` and zeroed ``.bss``.
 * @pre ``SystemInit`` enabled the MPU + I-cache + D-cache via the ra8_cache HAL.
 * @post Exactly one banner (PASS or FAIL) has been emitted.
 * @post The core is parked in WFI.
 *
 * @note Single-threaded; no RTOS and no IRQ sources in this template.
 * @since 0.1.0
 */
int main(void)
{
  ra8_log_init();
  ra8_log_info("CACHE_HAL", "==== cache_hal_enable_demo: L1 cache via ra8_cache HAL ====");

  if (!cache_hal_setup()) {
    ra8_log_info("CACHE_HAL", "setup FAILED (CGC / console) -- halting");
    cache_hal_emit(k_cache_hal_fail_setup, sizeof(k_cache_hal_fail_setup) - 1U);
    cache_hal_wfi_forever();
  }

  if (!cache_hal_test_cacheable_rw()) {
    ra8_log_info("CACHE_HAL", "cacheable-RW step FAILED");
    cache_hal_emit(k_cache_hal_fail_rw, sizeof(k_cache_hal_fail_rw) - 1U);
    cache_hal_wfi_forever();
  }

  ra8_log_info("CACHE_HAL", "L1-cache-via-HAL PASS (cacheable RW round-trip)");
  cache_hal_emit(k_cache_hal_pass_banner, sizeof(k_cache_hal_pass_banner) - 1U);
  cache_hal_wfi_forever();
}
