/**
 * @file examples/ek_ra8d2/hw_validated/hil/threadx_levelx_demo/main.c
 * @brief ThreadX + LevelX wear-levelling demo for EK-RA8D2 OSPI flash
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings the chip up the same way ``uart_hello`` does (CGC -> SCI8 @
 * 115200 8N1), then hands control to ThreadX. ``tx_application_define``
 * spawns one worker thread that:
 *
 *   1. Initialises ``ra8_xspi`` against the on-board EK-RA8D2 ISSI
 *      IS25LX512M octal-SPI flash chip.
 *   2. Calls ``lx_nor_flash_format`` once to lay down a fresh LevelX
 *      partition (small, 64 blocks * 4 KiB = 256 KiB) and immediately
 *      ``lx_nor_flash_open`` to mount it.
 *   3. Enters a continuous heartbeat loop:
 *        - Increments an in-RAM counter once per second.
 *        - Writes the counter into LevelX logical sector 0 with
 *          ``lx_nor_flash_sector_write``.
 *        - Reads it back with ``lx_nor_flash_sector_read`` and prints
 *          the value over SCI8.
 *   4. Once ``k_demo_burnin_writes`` (10000) heartbeats have run, the
 *      thread prints LevelX's wear-levelling statistics
 *      (``lx_nor_flash_write_requests``, ``lx_nor_flash_read_requests``,
 *      ``lx_nor_flash_extended_cache_hits/misses``,
 *      ``lx_nor_flash_minimum_erase_count`` /
 *      ``lx_nor_flash_maximum_erase_count``) so a human can see the
 *      wear distribution across blocks.
 *
 * Recipe (see also the README.md alongside this main.c):
 *
 *   - Connect a USB cable to the J-Link OB CDC port on the EK-RA8D2.
 *   - Open a 115200 8N1 terminal.
 *   - Flash this firmware (``make flash``).
 *   - Watch the heartbeat counter and final wear-levelling stats stream
 *     out over the terminal.
 *
 * @author Brighton Sikarskie
 * @date 2026-04-29
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_time.h"

/*
 * The host unit-test build (RA8_SIMULATOR_MODE) does not link the
 * ThreadX / LevelX vendor trees, so `tx_api.h` and `lx_api.h` are
 * unreachable when clang-tidy walks this file. Pull them in only on
 * the cross-compile target.
 */
#ifndef RA8_SIMULATOR_MODE
#include "lx_api.h"
#include "lx_nor_driver_ra8_xspi.h"
#include "tx_api.h"
#endif

/**
 * @brief Compile-time settings for the LevelX wear-levelling demo.
 */
typedef enum : uint32_t {
  k_lx_log_every_n = 100U, /**< Log once every N loop iterations. */
} lx_log_pace_t;

typedef enum : uint32_t {
  /** @brief Console baud (matches uart_hello / threadx_filex_demo). */
  k_demo_baud = 115200U,

  /** @brief Worker-thread stack size in bytes. */
  k_demo_thread_stack = 4096U,

  /** @brief LevelX logical sector the heartbeat counter is stored in. */
  k_demo_sector_index = 0U,

  /** @brief Number of heartbeats before printing wear-levelling stats. */
  k_demo_burnin_writes = 10000U,

  /** @brief Heartbeat tick in milliseconds. */
  k_demo_tick_ms = 1000U,

  /** @brief LevelX bytes per logical sector (= 512). */
  k_demo_sector_bytes = 512U,

  /** @brief Decimal radix used by ``demo_print_u32``. */
  k_demo_radix_dec = 10U,

  /** @brief Buffer size for ``demo_print_u32`` (10 digits + sign + NUL). */
  k_demo_u32_buf_size = 12U,
} demo_config_t;

#ifndef RA8_SIMULATOR_MODE
/* LevelX state. ThreadX requires statically-allocated control blocks
 * (NASA Power of 10 Rule 3 -- no dynamic memory). */
static LX_NOR_FLASH s_nor_flash;

/* LevelX logical-sector scratch buffer (512 bytes, ULONG-aligned). */
static ULONG s_demo_sector_io[k_demo_sector_bytes / sizeof(ULONG)];

/* ThreadX worker thread. */
static TX_THREAD s_demo_thread;
static UCHAR     s_demo_stack[k_demo_thread_stack];
#endif /* !RA8_SIMULATOR_MODE */

/**
 * @brief Halt forever in WFI, after draining the J-Link OB VCOM TX FIFO.
 *
 * @details
 * Calls ``ra8_board_uart_console_flush`` so any panic message previously
 * queued via ``ra8_board_uart_console_write`` finishes clocking onto the
 * wire before WFI gates the SCI clock. Without the flush, only the
 * first 1-3 bytes of the failure log reach the host because WFI
 * silences the peripheral mid-frame. Return code is intentionally
 * discarded -- if the flush times out we still want to halt rather
 * than spin.
 *
 * @pre Called only after a fatal error.
 * @post Pending console TX has drained (or the flush budget expired)
 *       and the CPU is parked.
 *
 * @since 0.1.0
 */
static void demo_panic_halt(void)
{
  (void)ra8_board_uart_console_flush();
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Bring CGC + the J-Link OB VCOM console up. Panic-halts on any failure.
 *
 * @pre Reset_Handler / SystemInit complete.
 * @post On success the BSP console is sending at 115200 8N1.
 *
 * @since 0.1.0
 */
static void demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;

  if (ra8_cgc_init() != k_ra8_ok) {
    demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    demo_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_demo_baud) != k_ra8_ok) {
    demo_panic_halt();
  }
}

/**
 * @brief Convenience wrapper to write a NUL-terminated string to the console.
 *
 * @param[in] s NUL-terminated ASCII string. Must not be NULL.
 *
 * @pre s != NULL.
 * @post On success the bytes are queued in the BSP console TX FIFO.
 *
 * @since 0.1.0
 */
static void demo_print(const char* s)
{
  if (s == (const char*)0) {
    return;
  }
  size_t len = strlen(s);
  (void)ra8_board_uart_console_write((const uint8_t*)s, len);
}

/**
 * @brief Print an unsigned 32-bit integer in decimal to SCI8.
 *
 * @param[in] value Integer to print.
 *
 * @pre None.
 * @post Decimal representation of ``value`` is queued to SCI8.
 *
 * @since 0.1.0
 */
static void demo_print_u32(uint32_t value)
{
  /* Largest uint32_t = 4294967295 -> 10 chars + NUL. */
  char     buf[k_demo_u32_buf_size];
  uint32_t i = (uint32_t)sizeof(buf);
  buf[--i]   = '\0';
  if (value == 0U) {
    buf[--i] = '0';
  } else {
    while ((value != 0U) && (i > 0U)) {
      buf[--i] = (char)('0' + (value % k_demo_radix_dec));
      value /= k_demo_radix_dec;
    }
  }
  demo_print(&buf[i]);
}

#ifndef RA8_SIMULATOR_MODE
/**
 * @brief Print one labelled ``ULONG`` LevelX statistic line over SCI8.
 *
 * @param[in] label NUL-terminated stat name. Must not be NULL.
 * @param[in] value Stat value.
 *
 * @pre label != NULL.
 * @post One "  <label>: <value>\\r\\n" line is queued to SCI8.
 *
 * @since 0.1.0
 */
static void demo_print_stat(const char* label, ULONG value)
{
  demo_print("  ");
  demo_print(label);
  demo_print(": ");
  demo_print_u32((uint32_t)value);
  demo_print("\r\n");
}

/**
 * @brief Format + open the LevelX partition. Panics on any failure.
 *
 * @pre ``ra8_xspi_init`` succeeded for instance 0.
 * @post On success ``s_nor_flash`` is open and ready for sector I/O.
 *
 * @since 0.1.0
 */
static void demo_lx_format_or_panic(void)
{
  UINT status = lx_nor_flash_format(&s_nor_flash,
                                    (CHAR*)"ra8_xspi_nor",
                                    lx_nor_driver_ra8_xspi_initialize,
                                    LX_NULL);
  if (status != LX_SUCCESS) {
    demo_print("[lx] format failed\r\n");
    demo_panic_halt();
  }
  status =
    lx_nor_flash_open(&s_nor_flash, (CHAR*)"ra8_xspi_nor", lx_nor_driver_ra8_xspi_initialize);
  if (status != LX_SUCCESS) {
    demo_print("[lx] open failed\r\n");
    demo_panic_halt();
  }
}

/**
 * @brief Run one heartbeat: program ``counter`` into sector 0 + read back.
 *
 * @param[in] counter Heartbeat sequence number.
 *
 * @return ``LX_SUCCESS`` on success, otherwise the LevelX error code
 *         from the underlying read or write call.
 *
 * @pre LevelX flash control block ``s_nor_flash`` is open.
 *
 * @post On success the read-back word equals ``counter``.
 *
 * @since 0.1.0
 */
static UINT demo_heartbeat(uint32_t counter)
{
  /* Stamp the counter into the first ULONG of the scratch buffer. */
  s_demo_sector_io[0] = (ULONG)counter;
  UINT status =
    lx_nor_flash_sector_write(&s_nor_flash, (ULONG)k_demo_sector_index, &s_demo_sector_io[0]);
  if (status != LX_SUCCESS) {
    return status;
  }
  /* Wipe the buffer before read-back so we know the value really came
   * out of flash. */
  s_demo_sector_io[0] = 0U;
  status = lx_nor_flash_sector_read(&s_nor_flash, (ULONG)k_demo_sector_index, &s_demo_sector_io[0]);
  return status;
}

/**
 * @brief Dump LevelX's wear-levelling statistics to SCI8.
 *
 * @pre ``s_nor_flash`` is open.
 * @post Statistics are queued to SCI8.
 *
 * @since 0.1.0
 */
static void demo_print_wear_stats(void)
{
  demo_print("[lx] wear stats:\r\n");
  demo_print_stat("write_requests       ", s_nor_flash.lx_nor_flash_write_requests);
  demo_print_stat("read_requests        ", s_nor_flash.lx_nor_flash_read_requests);
  demo_print_stat("minimum_erase_count  ", s_nor_flash.lx_nor_flash_minimum_erase_count);
  demo_print_stat("maximum_erase_count  ", s_nor_flash.lx_nor_flash_maximum_erase_count);
  demo_print_stat("free_physical_sectors", s_nor_flash.lx_nor_flash_free_physical_sectors);
  demo_print_stat("mapped_phys_sectors  ", s_nor_flash.lx_nor_flash_mapped_physical_sectors);
  demo_print_stat("obsolete_phys_sectors", s_nor_flash.lx_nor_flash_obsolete_physical_sectors);
  demo_print_stat("phys_block_allocates ", s_nor_flash.lx_nor_flash_physical_block_allocates);
  demo_print_stat("sector_cache_hits    ", s_nor_flash.lx_nor_flash_sector_mapping_cache_hits);
  demo_print_stat("sector_cache_misses  ", s_nor_flash.lx_nor_flash_sector_mapping_cache_misses);
#ifndef LX_NOR_DISABLE_EXTENDED_CACHE
  demo_print_stat("extended_cache_hits  ", s_nor_flash.lx_nor_flash_extended_cache_hits);
  demo_print_stat("extended_cache_misses", s_nor_flash.lx_nor_flash_extended_cache_misses);
#endif
}

/**
 * @brief Emit a one-shot HIL success banner after the first verified heartbeat.
 *
 * @details
 * On the first heartbeat (counter == 1) the worker has written the
 * counter into LevelX logical sector 0 and read it back. A matching
 * read-back proves the whole stack (LevelX format + open + sector
 * write/read over the xSPI NOR) round-tripped real data -- not just
 * that the chip booted. Emits a distinct success-only banner the HIL
 * uart_scrape gate keys on, or a MISMATCH banner its negative regex
 * catches. Does nothing on later heartbeats.
 *
 * @param[in] counter  Current heartbeat sequence number.
 * @param[in] readback Value read back from LevelX logical sector 0.
 *
 * @pre Called once per heartbeat from the worker loop.
 * @post On counter == 1 exactly one banner line is queued to SCI8.
 *
 * @since 0.1.0
 */
static void demo_report_rw_once(uint32_t counter, uint32_t readback)
{
  if (counter != 1U) {
    return;
  }
  if (readback == counter) {
    demo_print("[lx] sector rw verified readback=");
    demo_print_u32(readback);
    demo_print("\r\n");
  } else {
    demo_print("[lx] sector rw MISMATCH\r\n");
  }
}

/**
 * @brief ThreadX worker entry: format LevelX, run the heartbeat loop, dump stats.
 *
 * @param[in] thread_input Unused.
 *
 * @pre ``tx_application_define`` has scheduled this thread.
 * @post The heartbeat loop has run ``k_demo_burnin_writes`` cycles and
 *       wear-levelling statistics have been printed.
 *
 * @since 0.1.0
 */
static void demo_thread_entry(ULONG thread_input)
{
  (void)thread_input;

  demo_print("[lx] booting xSPI flash\r\n");
  /* The LevelX NOR driver owns OCTA bus bring-up: lx_nor_flash_format ->
   * lx_nor_driver_ra8_xspi_initialize -> priv_bus_init_once routes the
   * pins, runs the 8D/1S software-reset recovery, calls ra8_xspi_init,
   * and probes RDID exactly once. Doing it here as well double-routes
   * the PFS pins (the validator rejects the second route with
   * k_ra8_err_gpio_conflict), so the driver's initialize bails before
   * wiring the sector buffer and format returns LX_NO_MEMORY. Let the
   * driver be the single owner. */
  demo_print("[lx] formatting + opening LevelX partition\r\n");
  demo_lx_format_or_panic();

  demo_print("[lx] heartbeat begins\r\n");
  uint32_t counter = 0U;
  while (counter < (uint32_t)k_demo_burnin_writes) {
    counter++;
    UINT status = demo_heartbeat(counter);
    if (status != LX_SUCCESS) {
      demo_print("[lx] heartbeat I/O error at #");
      demo_print_u32(counter);
      demo_print("\r\n");
      break;
    }
    demo_report_rw_once(counter, (uint32_t)s_demo_sector_io[0]);
    /* Print every 100 cycles so the SCI8 stream stays readable. */
    if ((counter % k_lx_log_every_n) == 0U) {
      demo_print("[lx] cycle #");
      demo_print_u32(counter);
      demo_print(" readback=");
      demo_print_u32((uint32_t)s_demo_sector_io[0]);
      demo_print("\r\n");
    }
    /* Sleep ``k_demo_tick_ms`` between writes so a human can watch
     * the flash actually being beat on. ThreadX's tick rate is set by
     * ``port/threadx/inc/tx_user.h``; one tick is one millisecond at the
     * project default of ``TX_TIMER_TICKS_PER_SECOND = 1000``. */
    tx_thread_sleep((ULONG)k_demo_tick_ms);
  }

  demo_print("[lx] burn-in complete\r\n");
  demo_print_wear_stats();

  (void)lx_nor_flash_close(&s_nor_flash);
  demo_print("[lx] done\r\n");
}

/**
 * @brief ThreadX system-define hook: build the worker thread + LevelX core.
 *
 * @param[in] first_unused_memory Pointer to the start of free RAM
 *   provided by the ThreadX port; unused -- we statically allocate.
 *
 * @pre ``tx_kernel_enter()`` has been called.
 * @post One worker thread is created and LevelX has been initialized.
 *
 * @since 0.1.0
 */
void tx_application_define(void* first_unused_memory)
{
  (void)first_unused_memory;

  lx_nor_flash_initialize();

  (void)tx_thread_create(&s_demo_thread,
                         "lx_demo",
                         demo_thread_entry,
                         0U,
                         s_demo_stack,
                         (ULONG)sizeof(s_demo_stack),
                         8U, /* priority          */
                         8U, /* preempt threshold */
                         TX_NO_TIME_SLICE,
                         TX_AUTO_START);
}
#endif /* !RA8_SIMULATOR_MODE */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry. Brings up clocks + UART, then enters ThreadX.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @post On clean entry the kernel runs the worker thread once.
 *
 * @since 0.1.0
 */
int32_t main(void)
{
  demo_setup_or_halt();
  ra8_isr_globals_enable();
  demo_print("[lx] booting ThreadX + LevelX...\r\n");

#ifndef RA8_SIMULATOR_MODE
  /* Hands control over to ThreadX permanently. */
  tx_kernel_enter();
#endif

  /* Should never return. */
  demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
