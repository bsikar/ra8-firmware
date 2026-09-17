/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/ra8_cache_store_demo/src/main.c
 * @brief ra8_cache_store on-media render/glyph cache demo for the EK-RA8D2 (#257)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings the chip up the same way ``crc_demo`` does (CGC -> SysTick -> the
 * J-Link OB VCOM console @ 115200 8N1), then runs the whole ra8_cache_store
 * demonstration once through ::cache_store_demo_run:
 *
 *   1. Formats + mounts a cache_store over LevelX standalone, bound to a
 *      RAM-backed NOR driver (::lx_nor_ram_init) that lives entirely in SRAM.
 *   2. Puts four keyed "render/glyph cache" blobs and reads each back
 *      byte-identical.
 *   3. Pins the open-book cover atlas, forces an eviction of a render tile,
 *      confirms the survivors still resolve and the pinned entry refuses
 *      eviction, and re-puts a new blob into the reclaimed sectors.
 *   4. Checkpoint-closes, then re-mounts over the same (persisted) media and
 *      confirms the survivors + the pin came back and the evicted key stayed
 *      gone.
 *
 * On success it prints ``[rcs] cache_store demo PASS survivors=N ...`` -- the
 * success-only banner the HIL / EIL scrape keys on -- and then idles, re-emitting
 * it so the ra8_emulator STOP_ON guard always sees the steady-state line. On any
 * failure it prints ``[rcs] cache_store demo FAIL stage=S status=C`` instead,
 * which the negative regex catches.
 *
 * ## Why a RAM-backed NOR driver
 * ra8_cache_store's physical-flash bind is an injected callback. Production binds
 * the Octo-SPI driver; this demo binds a RAM driver so the entire path runs in
 * SRAM with no MMIO. That is what makes the run emu-gateable AND makes the
 * emulated run byte-identical to the on-silicon run (EIL equals HIL): there is no
 * peripheral for ra8_emulator to model differently from the chip.
 *
 * @author Brighton Sikarskie
 * @date 2026-07-15
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "cache_store_demo.h"
#include "lx_api.h"
#include "lx_nor_ram.h"
#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_cache_store.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_time.h"

/**
 * @enum rcs_demo_config_t
 * @brief Compile-time settings + fixture geometry for the demo app.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_rcs_baud            = 115200U, /**< Console baud (matches crc_demo).             */
  k_rcs_index_cap       = 16U,     /**< cache_store index slots.                     */
  k_rcs_staging_bytes   = 512U,    /**< One LevelX logical sector.                   */
  k_rcs_scratch_bytes   = 1536U,   /**< Read/write scratch (>= largest demo blob).   */
  k_rcs_logical_sectors = 128U,    /**< Usable LevelX logical-sector span.           */
  k_rcs_reemit_ms       = 1000U,   /**< Steady-state banner re-emit cadence (ms).    */
  k_rcs_radix_dec       = 10U,     /**< Decimal radix for the u32 printer.           */
  k_rcs_u32_buf_size    = 12U,     /**< u32 printer buffer (10 digits + sign + NUL). */
} rcs_demo_config_t;

/** @brief LevelX control block (statically allocated -- NASA P10 Rule 3). */
static LX_NOR_FLASH s_nor_flash;
/** @brief cache_store index array (caller-owned). */
static ra8_cache_store_entry_t s_index[k_rcs_index_cap];
/** @brief One-sector staging buffer (caller-owned). */
static uint8_t s_staging[k_rcs_staging_bytes];
/** @brief Read/write scratch buffer for the demo core (caller-owned). */
static uint8_t s_scratch[k_rcs_scratch_bytes];

/**
 * @brief Park the CPU forever after draining the console TX FIFO.
 *
 * @details Flushes pending diagnostics once, then executes wait-for-interrupt
 * indefinitely so fatal startup state remains observable.
 *
 * @pre A required startup step has failed irrecoverably.
 * @pre The board console may have pending diagnostic bytes.
 * @post This function does not return.
 * @post The processor remains in a low-activity wait loop after the flush.
 * @note The terminal loop preserves static fixture state for debugger inspection.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_rcs_panic_halt(void)
{
  (void)ra8_board_uart_console_flush();
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Bring CGC, SysTick, and the J-Link VCOM console up or halt.
 *
 * @details Initializes the clock generator, resolves CPUCLK0, starts the time
 * base, and configures the board console in dependency order.
 *
 * @pre Reset startup has initialized data and BSS storage.
 * @pre Board clock and console register mappings are accessible.
 * @post On return, delays and console diagnostics are available.
 * @post Any required setup failure transfers to ::internal_rcs_panic_halt.
 * @note Call once from the single-threaded application startup path.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_rcs_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    internal_rcs_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    internal_rcs_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    internal_rcs_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_rcs_baud) != k_ra8_ok) {
    internal_rcs_panic_halt();
  }
}

/**
 * @brief Write a NUL-terminated ASCII string to the console (fire-and-forget).
 * @details Ignores NULL input and delegates non-NULL byte emission to the board
 * console without allocating or retrying on diagnostic sink errors.
 * @param[in] s NUL-terminated string, or NULL (ignored).
 * @return Nothing.
 * @pre The console is initialised.
 * @pre @p s is NUL-terminated when non-NULL.
 * @post On a non-NULL @p s its bytes are queued to the console TX FIFO.
 * @post A NULL @p s is a no-op.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_rcs_print(const char* s)
{
  if (s == nullptr) {
    return;
  }
  (void)ra8_board_uart_console_write((const uint8_t*)s, strlen(s));
}

/**
 * @brief Print an unsigned 32-bit value in decimal to the console.
 * @details Converts the value from least significant digit into a bounded local
 * buffer, then emits the resulting forward substring through the string helper.
 * @param[in] value Integer to print.
 * @return Nothing.
 * @pre The console is initialised.
 * @pre None on @p value (full range accepted).
 * @post The decimal representation of @p value is queued to the console.
 * @post No other state changes.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_rcs_print_u32(uint32_t value)
{
  char     buf[k_rcs_u32_buf_size];
  uint32_t i = (uint32_t)sizeof(buf);
  buf[--i]   = '\0';
  if (value == 0U) {
    buf[--i] = '0';
  } else {
    while ((value != 0U) && (i > 0U)) {
      buf[--i] = (char)('0' + (value % (uint32_t)k_rcs_radix_dec));
      value /= (uint32_t)k_rcs_radix_dec;
    }
  }
  internal_rcs_print(&buf[i]);
}

/**
 * @brief Emit the one-line verdict banner for a completed demo run.
 * @details Selects PASS only when both the public return code and result status
 * agree, then prints the fixed diagnostic fields using bounded helpers.
 * @param[in] rc  Return code from ::cache_store_demo_run.
 * @param[in] res Populated demo result.
 * @return Nothing.
 * @pre The console is initialised.
 * @pre @p res reflects the run @p rc came from.
 * @post Exactly one PASS or FAIL banner line is queued to the console.
 * @post The banner is success-only on the PASS path (never printed on failure).
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_rcs_report(ra8_err_t rc, const cache_store_demo_result_t* res)
{
  if ((rc == k_ra8_ok) && (res->status == k_cache_store_demo_ok)) {
    internal_rcs_print("[rcs] cache_store demo PASS survivors=");
    internal_rcs_print_u32(res->survivors);
    internal_rcs_print(" evicted_gone=");
    internal_rcs_print_u32(res->evicted_stayed_gone ? 1U : 0U);
    internal_rcs_print(" pin=");
    internal_rcs_print_u32(res->pin_survived ? 1U : 0U);
    internal_rcs_print("\r\n");
    return;
  }
  internal_rcs_print("[rcs] cache_store demo FAIL stage=");
  internal_rcs_print_u32((uint32_t)res->last_stage);
  internal_rcs_print(" status=");
  internal_rcs_print_u32((uint32_t)res->status);
  internal_rcs_print("\r\n");
}

/**
 * @brief Build the demo configuration over the app's static fixture buffers.
 *
 * @details Binds the LevelX control block, RAM NOR driver, index, staging, and
 * scratch storage into the value object consumed by the reusable demo core.
 *
 * @return Fully populated caller-owned demo configuration.
 * @retval cache_store_demo_cfg_t Configuration referencing static fixture storage.
 * @pre All file-scope fixture arrays have their declared compile-time capacities.
 * @pre ``lx_nor_ram_init`` remains compatible with the LevelX NOR driver seam.
 * @post The returned object references only storage with firmware lifetime.
 * @post No fixture byte or LevelX control state is modified.
 * @note The returned structure is copied by value and owns no memory.
 * @since 0.1.0
 */
RA8_INTERNAL static cache_store_demo_cfg_t internal_rcs_build_cfg(void)
{
  return (cache_store_demo_cfg_t){
    .nor_flash       = &s_nor_flash,
    .nor_driver_init = lx_nor_ram_init,
    .index           = s_index,
    .staging         = s_staging,
    .scratch         = s_scratch,
    .staging_bytes   = (uint32_t)sizeof(s_staging),
    .scratch_bytes   = (uint32_t)sizeof(s_scratch),
    .logical_sectors = (uint32_t)k_rcs_logical_sectors,
    .index_cap       = (uint16_t)k_rcs_index_cap,
  };
}

/**
 * @brief Application entry: bring up clocks + console, run the demo, report.
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre The shared board boot files installed the vector table.
 * @post The demo has run once and its verdict banner is streaming steadily.
 * @post The CPU idles re-emitting the banner (or halts after a fatal init error).
 * @since 0.1.0
 */
void main(void)
{
  internal_rcs_setup_or_halt();
  ra8_isr_globals_enable();
  internal_rcs_print("[rcs] cache_store demo: mounting RAM-backed LevelX...\r\n");

  cache_store_demo_cfg_t    cfg = internal_rcs_build_cfg();
  cache_store_demo_result_t res = {};
  const ra8_err_t           rc  = cache_store_demo_run(&cfg, &res);
  internal_rcs_report(rc, &res);

  /* Idle, re-emitting the verdict so the STOP_ON / scrape always sees the
   * steady-state banner and the run reaches its budget cleanly. */
  while (1) {
    ra8_delay_ms((uint32_t)k_rcs_reemit_ms);
    internal_rcs_report(rc, &res);
  }
}
