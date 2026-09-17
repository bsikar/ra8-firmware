/**
 * @file examples/ek_ra8d2/hw_pending/mpu_boot_map_hal/src/main.c
 * @brief "MPU boot map brought up through the ra8_mpu HAL" self-test (#576)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Single-core (Cortex-M85) demonstrator for issue #576. The app's build defines
 * ``RA8_BOOT_ENABLE_CACHE_MPU`` (see ``CMakeLists.txt``) and ships a per-app
 * ``system_init.c`` whose ``SystemInit()`` installs the boot MPU
 * memory-attribute map through the ``ra8_mpu`` HAL
 * (``ra8_mpu_apply_boot_map()``) instead of the raw MAIR/RBAR/RLAR/CTRL pokes
 * the shared board boot uses. It is the HAL-path twin of ``cache_mpu_hil``
 * (raw-poke path); the two boot files diff cleanly and can be compared on HIL.
 *
 * Self-test (each step independent; runs once, then the core parks in WFI):
 *
 *   1. **MPU enabled via the HAL.** ``ra8_mpu_is_enabled()`` must report the MPU
 *      is on -- i.e. ``ra8_mpu_apply_boot_map()`` ran during boot and enabled it
 *      -- without this app poking the MPU register block directly.
 *
 *   2. **Canonical boot map.** ``ra8_mpu_boot_map()`` must expose the 5-region
 *      map, and region 4 must be the 640 KiB (non-power-of-two)
 *      Normal-non-cacheable shared M85<->M33 bank at 0x22100000 -- the region a
 *      size-checked setter cannot express and the reason the boot map needs a
 *      dedicated HAL entry point.
 *
 *   3. **Device-nGnRE MMIO (MPU region 3, peripherals @ 0x40000000).** Read the
 *      live SYSTEM ``SCKDIVCR`` register and confirm it reads back the value
 *      ``ra8_cgc_init()`` programmed (0x32233432) -- proving the HAL-programmed
 *      peripheral region is mapped Device (not cached) and accessible.
 *
 * On success the app emits ``"mpu_boot_map_hal: mpu-via-hal PASS\r\n"`` over the
 * J-Link OB VCOM console (SCI8, PD02/PD03 @ 115200 8N1) and mirrors the verdict
 * over ``ra8_log``. On any mismatch it emits a distinct ``... FAIL`` line (never
 * containing "PASS") and parks in WFI so ``ra8_emulator``'s idle detector stops
 * the run after the one-shot banner is scraped.
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
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_mpu.h"
#include "ra8_system_regs.h"

/**
 * @enum mpu_boot_config_t
 * @brief Compile-time scalar parameters for the MPU-via-HAL self-test.
 * @details Groups the console baud and the golden constants the self-test
 *          checks: the expected boot-map region count, the shared-SRAM region
 *          geometry, and the SYSTEM ``SCKDIVCR`` readback.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_mpu_boot_baud         = 115200U,     /**< VCOM console line rate (8N1).              */
  k_mpu_boot_sckdivcr_exp = 0x32233432U, /**< SCKDIVCR readback after ra8_cgc_init().    */
  k_mpu_boot_shram_base   = 0x22100000U, /**< Region 4 base (shared M85<->M33 SRAM).     */
  k_mpu_boot_shram_size   = 0x000A0000U, /**< Region 4 size (640 KiB, not a power of 2). */
} mpu_boot_config_t;

/**
 * @enum mpu_boot_region_idx_t
 * @brief Named indices into the canonical boot map region table.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_mpu_boot_idx_shram = 4U, /**< Shared M85<->M33 SRAM region. */
} mpu_boot_region_idx_t;

/**
 * @enum mpu_boot_result_t
 * @brief Verdict of the self-test, naming which step failed (if any).
 * @details Drives the banner ``main()`` emits; ``k_mpu_boot_result_pass`` is the
 *          only value that produces the PASS line.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_mpu_boot_result_pass      = 0U, /**< All three steps passed.             */
  k_mpu_boot_result_fail_mpu  = 1U, /**< MPU was not enabled by the HAL.     */
  k_mpu_boot_result_fail_map  = 2U, /**< Boot map region table was wrong.    */
  k_mpu_boot_result_fail_mmio = 3U, /**< Device-MMIO (region 3) step failed. */
} mpu_boot_result_t;

/**
 * @var k_mpu_boot_pass_banner
 * @brief Deterministic one-shot HIL success banner (uart_scrape gate).
 * @details Emitted only on the all-pass path. Contains "PASS" and is not a
 *          substring of any FAIL banner.
 * @warning Do not modify; ``hil.conf`` ``HIL_EXPECT`` matches it verbatim.
 * @since 0.1.0
 */
static const uint8_t k_mpu_boot_pass_banner[] = "mpu_boot_map_hal: mpu-via-hal PASS\r\n";

/**
 * @var k_mpu_boot_fail_setup
 * @brief Failure banner: clock / console bring-up failed before the self-test.
 * @details Distinct from the PASS banner and free of "PASS".
 * @since 0.1.0
 */
static const uint8_t k_mpu_boot_fail_setup[] = "mpu_boot_map_hal: setup FAIL\r\n";

/**
 * @var k_mpu_boot_fail_mpu
 * @brief Failure banner: ra8_mpu_is_enabled() reported the MPU is off.
 * @details Distinct from the PASS banner and free of "PASS".
 * @since 0.1.0
 */
static const uint8_t k_mpu_boot_fail_mpu[] = "mpu_boot_map_hal: mpu-disabled FAIL\r\n";

/**
 * @var k_mpu_boot_fail_map
 * @brief Failure banner: the boot map region table was not the canonical map.
 * @details Distinct from the PASS banner and free of "PASS".
 * @since 0.1.0
 */
static const uint8_t k_mpu_boot_fail_map[] = "mpu_boot_map_hal: boot-map FAIL\r\n";

/**
 * @var k_mpu_boot_fail_mmio
 * @brief Failure banner: the Device-nGnRE MMIO (region 3) readback mismatched.
 * @details Distinct from the PASS banner and free of "PASS".
 * @since 0.1.0
 */
static const uint8_t k_mpu_boot_fail_mmio[] = "mpu_boot_map_hal: device-MMIO FAIL\r\n";

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
[[noreturn]] static void mpu_boot_wfi_forever(void)
{
  while (1) {
    __asm volatile("wfi");
  }
}

/**
 * @brief Step 1 -- prove the MPU was enabled by the HAL boot path.
 *
 * @details Queries ``ra8_mpu_is_enabled()``. A ``true`` result means the per-app
 * ``SystemInit()`` called ``ra8_mpu_apply_boot_map()`` and the MPU is live -- all
 * without this app touching the MPU register block directly.
 *
 * @return Whether the MPU is enabled.
 * @retval true  ``ra8_mpu_is_enabled()`` reported the MPU on.
 * @retval false The MPU is off (apply_boot_map did not run or failed).
 *
 * @pre The cache + MPU boot path is active (``RA8_BOOT_ENABLE_CACHE_MPU``).
 * @pre ``ra8_mpu_apply_boot_map()`` ran during ``SystemInit()``.
 * @post No state is modified (pure query).
 * @post On true the MPU is proven enabled.
 *
 * @note Not thread-safe; single-threaded boot context.
 * @since 0.1.0
 */
static bool mpu_boot_test_enabled(void)
{
  return ra8_mpu_is_enabled();
}

/**
 * @brief Step 2 -- prove the canonical boot map (incl. the non-pow2 region 4).
 *
 * @details Reads the driver-owned boot map via ``ra8_mpu_boot_map()`` and checks
 * it has ``k_ra8_mpu_boot_region_count`` regions and that region 4 is the
 * 640 KiB Normal-non-cacheable shared M85<->M33 SRAM bank at 0x22100000 -- the
 * region a size-checked setter cannot express, which is why the boot map needs
 * its own HAL entry point.
 *
 * @return Whether the boot map matches the canonical layout.
 * @retval true  The map has 5 regions and region 4 is the expected shared bank.
 * @retval false The count or region-4 descriptor did not match.
 *
 * @pre ``ra8_mpu_boot_map()`` returns the driver-owned const table.
 * @post No state is modified (pure read of immutable data).
 * @post On true the boot map is proven to carry the non-power-of-two region.
 *
 * @note Not thread-safe; single-threaded boot context.
 * @since 0.1.0
 */
static bool mpu_boot_test_map(void)
{
  uint8_t                 count = 0U;
  const ra8_mpu_region_t* map   = ra8_mpu_boot_map(&count);
  if (map == nullptr) {
    return false;
  }
  if (count != (uint8_t)k_ra8_mpu_boot_region_count) {
    return false;
  }
  const ra8_mpu_region_t* shram = &map[k_mpu_boot_idx_shram];
  if (shram->base != (uintptr_t)k_mpu_boot_shram_base) {
    return false;
  }
  if (shram->size != (uint32_t)k_mpu_boot_shram_size) {
    return false;
  }
  if (shram->attr_idx != k_ra8_mpu_attr_idx_1) {
    return false;
  }
  return true;
}

/**
 * @brief Step 3 -- prove Device-nGnRE peripheral MMIO is accessible (region 3).
 *
 * @details Reads the live SYSTEM ``SCKDIVCR`` register (peripheral window @
 * 0x40000000, mapped Device-nGnRE by boot-map region 3) and checks it equals
 * the divider word ``ra8_cgc_init()`` programmed. A correct readback proves the
 * HAL-programmed peripheral region is reachable and uncached.
 *
 * @return Whether the SYSTEM register read back the post-init value.
 * @retval true  ``SCKDIVCR`` read back 0x32233432.
 * @retval false The read returned 0 (dead bus) or a value other than expected.
 *
 * @pre ``ra8_cgc_init()`` has programmed ``SCKDIVCR`` earlier this boot.
 * @pre The boot map maps the peripheral window Device-nGnRE (region 3).
 * @post No register is modified (pure read).
 * @post On true the peripheral MMIO path is proven accessible and uncached.
 *
 * @note Not thread-safe; single-threaded boot context.
 * @since 0.1.0
 */
static bool mpu_boot_test_device_mmio(void)
{
  /* HUM Ch 9.2.2 "SCKDIVCR : System Clock Division Control Register" p 326 */
  const uint32_t sckdivcr = *ra8_sys_sckdivcr();
  if (sckdivcr == 0U) {
    return false;
  }
  if (sckdivcr != (uint32_t)k_mpu_boot_sckdivcr_exp) {
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
 * @retval k_mpu_boot_result_pass      All three steps passed.
 * @retval k_mpu_boot_result_fail_mpu  The MPU-enabled step failed.
 * @retval k_mpu_boot_result_fail_map  The boot-map step failed.
 * @retval k_mpu_boot_result_fail_mmio The Device-MMIO step failed.
 *
 * @pre The cache + MPU boot path is active (set by ``RA8_BOOT_ENABLE_CACHE_MPU``).
 * @pre ``ra8_cgc_init()`` has run (Device-MMIO step needs the programmed divider).
 * @post No persistent state is modified.
 * @post Exactly one verdict is returned.
 *
 * @note Not thread-safe; single-threaded boot context.
 * @since 0.1.0
 */
static mpu_boot_result_t mpu_boot_run_selftest(void)
{
  if (!mpu_boot_test_enabled()) {
    return k_mpu_boot_result_fail_mpu;
  }
  if (!mpu_boot_test_map()) {
    return k_mpu_boot_result_fail_map;
  }
  if (!mpu_boot_test_device_mmio()) {
    return k_mpu_boot_result_fail_mmio;
  }
  return k_mpu_boot_result_pass;
}

/**
 * @brief Bring up the clock tree and the VCOM console for the banner.
 *
 * @details ``ra8_cgc_init()`` programmes the clock tree (and thereby the
 * ``SCKDIVCR`` the Device-MMIO step checks); the EK-RA8D2 debug console (SCI8 on
 * PD02/PD03 @ ::k_mpu_boot_baud) then comes up over the J-Link OB VCOM bridge.
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
static bool mpu_boot_setup(void)
{
  if (ra8_cgc_init() != k_ra8_ok) {
    return false;
  }
  if (ra8_board_uart_console_init((uint32_t)k_mpu_boot_baud) != k_ra8_ok) {
    return false;
  }
  return true;
}

/**
 * @brief Emit one banner line over the VCOM console and flush it.
 *
 * @details Writes @p line then drains the SCI8 TX so the bytes clock out before
 * the core parks. A no-op if @p line is NULL or empty.
 *
 * @param[in] line Pointer to the ASCII banner bytes (no NUL sent). Must be
 *                 non-NULL for output.
 * @param[in] len  Number of bytes to send; 0 sends nothing.
 *
 * @return Nothing.
 *
 * @pre @p line points to at least @p len readable bytes when @p len > 0.
 * @pre ::mpu_boot_setup was attempted during bring-up.
 * @post The bytes have been handed to SCI8 and the TX FIFO drained (if up).
 * @post No application state is modified.
 *
 * @note Not thread-safe; single-threaded boot context.
 * @since 0.1.0
 */
static void mpu_boot_emit(const uint8_t* line, size_t len)
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
 * @brief Application entry: prove the MPU boot map came up via the HAL.
 *
 * @details Brings up logging, the clock tree, and the VCOM console, runs the
 * three-step self-test (MPU enabled, canonical boot map, Device MMIO), emits the
 * matching PASS / FAIL banner over the console and ``ra8_log``, then parks in WFI.
 *
 * @pre ``Reset_Handler`` has copied ``.data`` and zeroed ``.bss``.
 * @pre ``SystemInit`` installed the boot map via ``ra8_mpu_apply_boot_map()``.
 * @post Exactly one banner (PASS or a step-specific FAIL) has been emitted.
 * @post The core is parked in WFI.
 *
 * @note Single-threaded; no RTOS and no IRQ sources in this template.
 * @since 0.1.0
 */
void main(void)
{
  ra8_log_init();
  ra8_log_info("MPU_BOOT", "==== mpu_boot_map_hal: MPU boot map via ra8_mpu HAL ====");

  if (!mpu_boot_setup()) {
    ra8_log_info("MPU_BOOT", "setup FAILED (CGC / console) -- halting");
    mpu_boot_emit(k_mpu_boot_fail_setup, sizeof(k_mpu_boot_fail_setup) - 1U);
    mpu_boot_wfi_forever();
  }

  const mpu_boot_result_t result = mpu_boot_run_selftest();

  switch (result) {
    case k_mpu_boot_result_pass:
      ra8_log_info("MPU_BOOT", "mpu-via-hal PASS (enabled + canonical map + Device MMIO)");
      mpu_boot_emit(k_mpu_boot_pass_banner, sizeof(k_mpu_boot_pass_banner) - 1U);
      break;
    case k_mpu_boot_result_fail_mpu:
      ra8_log_info("MPU_BOOT", "MPU-enabled step FAILED (apply_boot_map did not enable)");
      mpu_boot_emit(k_mpu_boot_fail_mpu, sizeof(k_mpu_boot_fail_mpu) - 1U);
      break;
    case k_mpu_boot_result_fail_map:
      ra8_log_info("MPU_BOOT", "boot-map step FAILED (region table mismatch)");
      mpu_boot_emit(k_mpu_boot_fail_map, sizeof(k_mpu_boot_fail_map) - 1U);
      break;
    case k_mpu_boot_result_fail_mmio:
    default:
      ra8_log_info("MPU_BOOT", "device-MMIO step FAILED");
      mpu_boot_emit(k_mpu_boot_fail_mmio, sizeof(k_mpu_boot_fail_mmio) - 1U);
      break;
  }

  mpu_boot_wfi_forever();
}
