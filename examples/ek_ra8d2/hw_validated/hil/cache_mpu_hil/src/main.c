/**
 * @file examples/ek_ra8d2/hw_validated/hil/cache_mpu_hil/src/main.c
 * @brief Cortex-M85 L1 cache + MPU "runs correctly with them on" HIL self-test
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Single-core (Cortex-M85) HIL validator for the T4 cache-coherency chain. The
 * app's build defines ``RA8_BOOT_ENABLE_CACHE_MPU`` (see ``CMakeLists.txt``), so
 * the shared boot ``SystemInit()`` programmes the 5-region MPU and enables the
 * I-cache, D-cache, and branch predictor *before* ``main()`` runs. Everything in
 * ``main()`` therefore executes with the caches and MPU live; this app proves the
 * core still produces correct results in that posture.
 *
 * Self-test (each step independent; runs once, then the core parks in WFI):
 *
 *   1. **Cacheable RW (MPU region 1, M85-private SRAM @ 0x22000000).** Fill a
 *      4 KiB static buffer (in ``.bss``, region 1) with a deterministic pattern
 *      ``buf[i] = (uint8_t)(i * 31 + 7)``, clean+invalidate those (now dirty)
 *      D-cache lines back to SRAM, then read every byte back -- the read misses
 *      the cache and refills from memory, exercising the full write-back + refill
 *      path with the D-cache enabled.
 *
 *   2. **RO MRAM const + code execution (MPU region 0, MRAM @ 0x02000000,
 *      RO + execute, cacheable).** Call a small non-inlined helper that lives in
 *      MRAM ``.text`` and sums a ``const`` table in MRAM ``.rodata`` via volatile
 *      loads -- proving code executes through the I-cache and the read-only MRAM
 *      region is readable.
 *
 *   3. **Device-nGnRE MMIO (MPU region 3, peripherals @ 0x40000000).** Read the
 *      live SYSTEM ``SCKDIVCR`` register through the ``ra8_sys_sckdivcr()`` HAL
 *      accessor and confirm it reads back the value ``ra8_cgc_init()`` programmed
 *      (0x32233432) -- proving peripheral MMIO is mapped Device (not cached) and
 *      accessible with the D-cache on.
 *
 * On success the app emits ``"cache_mpu_hil: cache+mpu PASS\r\n"`` over the
 * J-Link OB VCOM console (SCI8, PD02/PD03 @ 115200 8N1) and mirrors the verdict
 * over ``ra8_log`` (the emulator echoes it as an ``[itm]`` line). On any mismatch
 * it emits a distinct ``... FAIL`` line (never containing "PASS") and parks. The
 * core ends every path in WFI so ``ra8_emulator``'s ``RA8_EMU_IDLE_STOP``
 * terminates the run after the one-shot banner is scraped.
 *
 * @note ``ra8_emulator`` models memory byte-exact and does not model the L1 D-cache,
 *       so the cacheable-RW step passes there trivially; the cache hazard this
 *       app guards against is only real on silicon. The point of the emulator run is
 *       to prove the app boots and reports PASS with the cache+MPU boot path
 *       compiled in.
 *
 * @author Brighton Sikarskie
 * @date 2026-06-29
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_cache.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_system_regs.h"

/**
 * @enum cache_mpu_config_t
 * @brief Compile-time scalar parameters for the cache + MPU self-test.
 * @details Groups the console baud, the cacheable buffer size, and the two
 *          golden readback constants the self-test checks against.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_cache_mpu_baud         = 115200U,     /**< VCOM console line rate (8N1).               */
  k_cache_mpu_buf_bytes    = 4096U,       /**< Cacheable self-test buffer size (>= 4 KiB). */
  k_cache_mpu_buf_min      = 4096U,       /**< Self-test buffer floor (4 KiB minimum).     */
  k_cache_mpu_sckdivcr_exp = 0x32233432U, /**< SCKDIVCR readback after ra8_cgc_init().     */
  k_cache_mpu_mram_sum_exp = 0xAAAAAAAAU, /**< Expected sum of the MRAM const table.       */
} cache_mpu_config_t;

/**
 * @enum cache_mpu_pattern_t
 * @brief Affine byte-pattern coefficients: ``buf[i] = i * MUL + ADD``.
 * @details A per-index affine pattern makes every byte distinct modulo 256, so a
 *          stuck or swapped line is caught by the readback compare.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_cache_mpu_pat_mul = 31U, /**< Pattern multiplier. */
  k_cache_mpu_pat_add = 7U,  /**< Pattern addend.     */
} cache_mpu_pattern_t;

/**
 * @enum cache_mpu_mram_t
 * @brief Geometry of the read-only MRAM const table summed in MPU region 0.
 * @details The table is summed via a non-inlined MRAM helper to prove code
 *          executes from MRAM and the RO MRAM region is readable.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_cache_mpu_mram_words = 4U, /**< Number of 32-bit words in the const table. */
} cache_mpu_mram_t;

/**
 * @enum cache_mpu_result_t
 * @brief Verdict of the self-test, naming which step failed (if any).
 * @details Drives the banner ``main()`` emits; ``k_cache_mpu_result_pass`` is the
 *          only value that produces the PASS line.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_cache_mpu_result_pass      = 0U, /**< All three steps passed.               */
  k_cache_mpu_result_fail_sram = 1U, /**< Cacheable-RW (region 1) step failed.  */
  k_cache_mpu_result_fail_mram = 2U, /**< RO-MRAM-const (region 0) step failed. */
  k_cache_mpu_result_fail_mmio = 3U, /**< Device-MMIO (region 3) step failed.   */
} cache_mpu_result_t;

static_assert((uint32_t)k_cache_mpu_buf_bytes >= (uint32_t)k_cache_mpu_buf_min,
              "cacheable self-test buffer must be at least 4 KiB");

/**
 * @var s_cache_mpu_buf
 * @brief 4 KiB cacheable scratch buffer in M85-private SRAM (MPU region 1).
 * @details Lives in ``.bss`` (SRAM @ 0x22000000, cacheable per MPU region 1).
 *          Cache-line aligned (32 bytes) so the clean+invalidate covers whole
 *          lines with no partial-line surprises.
 * @note Mutated only by the cacheable-RW self-test on the single boot thread.
 * @warning Not for cross-core use -- region 1 is M85-private cacheable.
 * @since 0.1.0
 */
alignas(32) static uint8_t s_cache_mpu_buf[k_cache_mpu_buf_bytes];

/**
 * @var k_cache_mpu_mram_table
 * @brief Read-only word table placed in MRAM ``.rodata`` (MPU region 0).
 * @details Summed by ::cache_mpu_mram_const_sum through volatile loads so the
 *          read genuinely targets the RO MRAM region. Sum is 0xAAAAAAAA.
 * @note Const; never written at run time.
 * @since 0.1.0
 */
static const uint32_t k_cache_mpu_mram_table[k_cache_mpu_mram_words] = {
  0x11111111U,
  0x22222222U,
  0x33333333U,
  0x44444444U,
};

/**
 * @var k_cache_mpu_pass_banner
 * @brief Deterministic one-shot HIL success banner (uart_scrape gate).
 * @details Emitted only on the all-pass path over the VCOM console. The trailing
 *          CRLF terminates the line on the wire; the HIL gate matches the text
 *          exactly. Contains "PASS" and is not a substring of any FAIL banner.
 * @warning Do not modify; ``hil.conf`` ``HIL_EXPECT`` matches it verbatim.
 * @since 0.1.0
 */
static const uint8_t k_cache_mpu_pass_banner[] = "cache_mpu_hil: cache+mpu PASS\r\n";

/**
 * @var k_cache_mpu_fail_setup
 * @brief Failure banner: clock / console bring-up failed before the self-test.
 * @details Distinct from the PASS banner and free of "PASS"; matched by the
 *          ``hil.conf`` ``HIL_EXPECT_NEGATIVE`` "FAIL" alternative.
 * @since 0.1.0
 */
static const uint8_t k_cache_mpu_fail_setup[] = "cache_mpu_hil: setup FAIL\r\n";

/**
 * @var k_cache_mpu_fail_sram
 * @brief Failure banner: the cacheable-SRAM (region 1) readback mismatched.
 * @details Distinct from the PASS banner and free of "PASS".
 * @since 0.1.0
 */
static const uint8_t k_cache_mpu_fail_sram[] = "cache_mpu_hil: cacheable-SRAM FAIL\r\n";

/**
 * @var k_cache_mpu_fail_mram
 * @brief Failure banner: the RO-MRAM-const (region 0) sum mismatched.
 * @details Distinct from the PASS banner and free of "PASS".
 * @since 0.1.0
 */
static const uint8_t k_cache_mpu_fail_mram[] = "cache_mpu_hil: MRAM-const FAIL\r\n";

/**
 * @var k_cache_mpu_fail_mmio
 * @brief Failure banner: the Device-nGnRE MMIO (region 3) readback mismatched.
 * @details Distinct from the PASS banner and free of "PASS".
 * @since 0.1.0
 */
static const uint8_t k_cache_mpu_fail_mmio[] = "cache_mpu_hil: device-MMIO FAIL\r\n";

/**
 * @brief Park the Cortex-M85 forever in a WFI idle loop.
 *
 * @details Reached after the one-shot banner has been emitted (pass or fail).
 * The WFI lets ``ra8_emulator``'s idle detector stop the run cleanly and models the
 * low-power posture on silicon.
 *
 * @return This function never returns.
 * @note The core stays asleep between (unconfigured) wake events.
 *
 * @pre The self-test verdict has been emitted over the console.
 * @pre No further forward progress is required of the M85.
 * @post The core makes no further architectural progress.
 * @post Any pending console bytes have already been flushed by the caller.
 *
 * @note Not thread-safe; single-threaded boot context.
 * @since 0.1.0
 */
[[noreturn]] static void cache_mpu_wfi_forever(void)
{
  while (1) {
    __asm volatile("wfi");
  }
}

/**
 * @brief Sum the read-only MRAM const table (executes from MRAM, region 0).
 *
 * @details Kept non-inlined so a real call/branch retires from MRAM ``.text``
 * (proving I-cache + executable MRAM), and reads the table through a
 * ``volatile`` pointer so the loads are not constant-folded away -- they genuinely
 * touch the RO MRAM ``.rodata`` region.
 *
 * @return The 32-bit sum of all table words (wrapping). Expected 0xAAAAAAAA.
 * @retval 0xAAAAAAAA The table is intact and the MRAM read path works.
 *
 * @pre ::k_cache_mpu_mram_table is resident in the RO MRAM region.
 * @pre The MPU has region 0 mapped RO + execute, cacheable (cache+MPU boot).
 * @post No memory is modified (pure read of a const table).
 * @post The returned value reflects the live MRAM contents, not a folded const.
 *
 * @note Thread-safe; reads immutable data only.
 * @since 0.1.0
 */
[[gnu::noinline]] static uint32_t cache_mpu_mram_const_sum(void)
{
  const volatile uint32_t* p   = k_cache_mpu_mram_table;
  uint32_t                 sum = 0U;
  for (uint8_t i = 0U; i < (uint8_t)k_cache_mpu_mram_words; i++) {
    sum += p[i];
  }
  return sum;
}

/**
 * @brief Step 1 -- prove cacheable RW works in M85-private SRAM (region 1).
 *
 * @details Fills ::s_cache_mpu_buf with the affine pattern, cleans + invalidates
 * the dirty D-cache lines back to SRAM, then verifies every byte on read-back so
 * the refill path from memory is exercised with the D-cache enabled.
 *
 * @return Whether the buffer read back exactly as written.
 * @retval true  Every byte matched the pattern after the cache round-trip.
 * @retval false A byte mismatched, or the cache maintenance call failed.
 *
 * @pre The D-cache + MPU are enabled (cache+MPU boot path active).
 * @pre ::s_cache_mpu_buf lies in cacheable SRAM (MPU region 1).
 * @post On true the buffer holds the affine pattern in SRAM.
 * @post No memory outside ::s_cache_mpu_buf is modified.
 *
 * @note Not thread-safe; single-threaded boot context.
 * @since 0.1.0
 */
static bool cache_mpu_test_cacheable_rw(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_cache_mpu_buf_bytes; i++) {
    s_cache_mpu_buf[i] =
      (uint8_t)((i * (uint32_t)k_cache_mpu_pat_mul) + (uint32_t)k_cache_mpu_pat_add);
  }

  /* Force the freshly written (dirty) lines back to SRAM and drop them, so the
   * verify pass below must refill from memory -- the full write-back + refill
   * round-trip with the D-cache on. */
  if (ra8_cache_dcache_clean_invalidate_by_addr(s_cache_mpu_buf,
                                                (uint32_t)sizeof(s_cache_mpu_buf)) != k_ra8_ok) {
    return false;
  }

  for (uint32_t i = 0U; i < (uint32_t)k_cache_mpu_buf_bytes; i++) {
    const uint8_t expected =
      (uint8_t)((i * (uint32_t)k_cache_mpu_pat_mul) + (uint32_t)k_cache_mpu_pat_add);
    if (s_cache_mpu_buf[i] != expected) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Step 2 -- prove RO MRAM const + MRAM code execution (region 0).
 *
 * @details Calls ::cache_mpu_mram_const_sum twice: once to check the value
 * against the golden sum, and again to confirm the RO MRAM region reads back
 * identically (immutable), executing the MRAM helper through the I-cache both
 * times.
 *
 * @return Whether the MRAM const path is correct and stable.
 * @retval true  The sum equals 0xAAAAAAAA and is stable across two reads.
 * @retval false The sum mismatched the golden value, or two reads disagreed.
 *
 * @pre The I-cache + MPU are enabled with region 0 RO + execute.
 * @pre ::k_cache_mpu_mram_table is resident in MRAM ``.rodata``.
 * @post No memory is modified.
 * @post On true the MRAM read-only region is proven readable and stable.
 *
 * @note Not thread-safe; single-threaded boot context.
 * @since 0.1.0
 */
static bool cache_mpu_test_mram_const(void)
{
  const uint32_t sum1 = cache_mpu_mram_const_sum();
  const uint32_t sum2 = cache_mpu_mram_const_sum();
  if (sum1 != (uint32_t)k_cache_mpu_mram_sum_exp) {
    return false;
  }
  if (sum1 != sum2) {
    return false;
  }
  return true;
}

/**
 * @brief Step 3 -- prove Device-nGnRE peripheral MMIO is accessible (region 3).
 *
 * @details Reads the live SYSTEM ``SCKDIVCR`` register through the
 * ``ra8_sys_sckdivcr()`` HAL accessor (peripheral window @ 0x40000000, mapped
 * Device-nGnRE by MPU region 3) and checks it equals the divider word
 * ``ra8_cgc_init()`` programmed. A correct readback proves the peripheral bus is
 * reachable and uncached with the D-cache on; a zero/garbage read would indicate
 * a dead or mis-mapped Device region.
 *
 * @return Whether the SYSTEM register read back the post-init value.
 * @retval true  ``SCKDIVCR`` read back 0x32233432.
 * @retval false The read returned 0 (dead bus) or a value other than expected.
 *
 * @pre ``ra8_cgc_init()`` has programmed ``SCKDIVCR`` earlier this boot.
 * @pre The MPU maps the peripheral window Device-nGnRE (region 3).
 * @post No register is modified (pure read).
 * @post On true the peripheral MMIO path is proven accessible and uncached.
 *
 * @note Not thread-safe; single-threaded boot context.
 * @since 0.1.0
 */
static bool cache_mpu_test_device_mmio(void)
{
  /* HUM Ch 9.2.2 "SCKDIVCR : System Clock Division Control Register" p 326 */
  const uint32_t sckdivcr = *ra8_sys_sckdivcr();
  if (sckdivcr == 0U) {
    return false;
  }
  if (sckdivcr != (uint32_t)k_cache_mpu_sckdivcr_exp) {
    return false;
  }
  return true;
}

/**
 * @brief Run all three self-test steps in order, naming the first failure.
 *
 * @details Steps are independent and short-circuit: the first failing step
 * determines the verdict so ``main()`` can emit a step-specific FAIL banner.
 *
 * @return The self-test verdict.
 * @retval k_cache_mpu_result_pass      All three steps passed.
 * @retval k_cache_mpu_result_fail_sram The cacheable-RW step failed.
 * @retval k_cache_mpu_result_fail_mram The RO-MRAM-const step failed.
 * @retval k_cache_mpu_result_fail_mmio The Device-MMIO step failed.
 *
 * @pre The cache + MPU boot path is active (set by ``RA8_BOOT_ENABLE_CACHE_MPU``).
 * @pre ``ra8_cgc_init()`` has run (Device-MMIO step needs the programmed divider).
 * @post No persistent state beyond ::s_cache_mpu_buf is modified.
 * @post Exactly one verdict is returned.
 *
 * @note Not thread-safe; single-threaded boot context.
 * @since 0.1.0
 */
static cache_mpu_result_t cache_mpu_run_selftest(void)
{
  if (!cache_mpu_test_cacheable_rw()) {
    return k_cache_mpu_result_fail_sram;
  }
  if (!cache_mpu_test_mram_const()) {
    return k_cache_mpu_result_fail_mram;
  }
  if (!cache_mpu_test_device_mmio()) {
    return k_cache_mpu_result_fail_mmio;
  }
  return k_cache_mpu_result_pass;
}

/**
 * @brief Bring up the clock tree and the VCOM console for the banner.
 *
 * @details ``ra8_cgc_init()`` programmes the FSP-quickstart tree (and thereby the
 * ``SCKDIVCR`` the Device-MMIO step checks) and publishes PCLKA; the EK-RA8D2
 * debug console (SCI8 on PD02/PD03 @ ::k_cache_mpu_baud) then comes up over the
 * J-Link OB VCOM bridge.
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
static bool cache_mpu_setup(void)
{
  if (ra8_cgc_init() != k_ra8_ok) {
    return false;
  }
  if (ra8_board_uart_console_init((uint32_t)k_cache_mpu_baud) != k_ra8_ok) {
    return false;
  }
  return true;
}

/**
 * @brief Emit one banner line over the VCOM console and flush it.
 *
 * @details Writes @p line then drains the SCI8 TX so the bytes clock out before
 * the core parks. A no-op if @p line is NULL or empty, or if the console never
 * came up (the write returns an error that is ignored here).
 *
 * @param[in] line Pointer to the ASCII banner bytes (no NUL sent). Must be
 *                 non-NULL for output.
 * @param[in] len  Number of bytes to send; 0 sends nothing.
 *
 * @return Nothing.
 *
 * @pre @p line points to at least @p len readable bytes when @p len > 0.
 * @pre ::cache_mpu_setup was attempted during bring-up.
 * @post The bytes have been handed to SCI8 and the TX FIFO drained (if up).
 * @post No application state is modified.
 *
 * @note Not thread-safe; single-threaded boot context.
 * @since 0.1.0
 */
static void cache_mpu_emit(const uint8_t* line, size_t len)
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
 * @brief Application entry: run the cache + MPU self-test and report the verdict.
 *
 * @details Brings up logging, the clock tree, and the VCOM console, runs the
 * three-step self-test (cacheable SRAM, RO MRAM const, Device MMIO), emits the
 * matching PASS / FAIL banner over the console and ``ra8_log``, then parks in WFI.
 * Every byte the self-test touches runs with the L1 caches and MPU enabled by
 * the shared boot (``RA8_BOOT_ENABLE_CACHE_MPU``).
 *
 * @pre ``Reset_Handler`` has copied ``.data`` and zeroed ``.bss``.
 * @pre ``SystemInit`` enabled the MPU + I-cache + D-cache (cache+MPU build).
 * @post Exactly one banner (PASS or a step-specific FAIL) has been emitted.
 * @post The core is parked in WFI.
 *
 * @note Single-threaded; no RTOS and no IRQ sources in this template.
 * @since 0.1.0
 */
void main(void)
{
  ra8_log_init();
  ra8_log_info("CACHE_MPU", "==== cache_mpu_hil: M85 L1 cache + MPU self-test ====");

  if (!cache_mpu_setup()) {
    ra8_log_info("CACHE_MPU", "setup FAILED (CGC / console) -- halting");
    cache_mpu_emit(k_cache_mpu_fail_setup, sizeof(k_cache_mpu_fail_setup) - 1U);
    cache_mpu_wfi_forever();
  }

  const cache_mpu_result_t result = cache_mpu_run_selftest();

  switch (result) {
    case k_cache_mpu_result_pass:
      ra8_log_info("CACHE_MPU", "cache+mpu PASS (cacheable RW + MRAM const + Device MMIO)");
      cache_mpu_emit(k_cache_mpu_pass_banner, sizeof(k_cache_mpu_pass_banner) - 1U);
      break;
    case k_cache_mpu_result_fail_sram:
      ra8_log_info("CACHE_MPU", "cacheable-SRAM step FAILED");
      cache_mpu_emit(k_cache_mpu_fail_sram, sizeof(k_cache_mpu_fail_sram) - 1U);
      break;
    case k_cache_mpu_result_fail_mram:
      ra8_log_info("CACHE_MPU", "MRAM-const step FAILED");
      cache_mpu_emit(k_cache_mpu_fail_mram, sizeof(k_cache_mpu_fail_mram) - 1U);
      break;
    case k_cache_mpu_result_fail_mmio:
    default:
      ra8_log_info("CACHE_MPU", "device-MMIO step FAILED");
      cache_mpu_emit(k_cache_mpu_fail_mmio, sizeof(k_cache_mpu_fail_mmio) - 1U);
      break;
  }

  cache_mpu_wfi_forever();
}
