/**
 * @file examples/ek_ra8d2/threadx_filex_demo/main.c
 * @brief ThreadX + FileX SD-card demo for EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings the chip up the same way ``uart_hello`` does (CGC -> SCI8 @
 * 115200 8N1), then hands control to ThreadX. ``tx_application_define``
 * spawns one worker thread that:
 *
 *   1. Calls ``fx_media_open`` against the FileX <-> ``ra_sdhi``
 *      driver shim (``fx_media_driver_ra_sdhi``).
 *   2. Walks the root directory with
 *      ``fx_directory_first_full_entry_find`` /
 *      ``fx_directory_next_full_entry_find`` and prints each entry
 *      name + size to SCI8.
 *   3. If a ``README.TXT`` exists at the root, opens it with
 *      ``fx_file_open`` and dumps its contents to SCI8.
 *
 * Recipe (see also the README.md alongside this main.c):
 *
 *   - Format a micro-SD card as FAT32.
 *   - Copy a plain-text file named ``README.TXT`` to the root.
 *   - Insert the card into the EK-RA8D2 micro-SD slot (J6).
 *   - Flash this firmware (``make flash``).
 *   - Connect a 115200 8N1 terminal to the J-Link OB CDC port.
 *
 * @author Brighton Sikarskie
 * @date 2026-04-29
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_isr.h"
#include "ra_time.h"

/*
 * The host unit-test build (RA_SIMULATOR_MODE) does not link the
 * ThreadX or FileX vendor trees, so `tx_api.h` and `fx_api.h` are
 * unreachable when clang-tidy walks this file. Pull them in only on
 * the cross-compile target.
 */
#ifndef RA_SIMULATOR_MODE
#include "fx_api.h"
#include "fx_media_driver_ra_sdhi.h"
#include "tx_api.h"
#endif

/** @brief Compile-time settings shared with uart_hello. */
typedef enum : uint32_t {
  k_demo_baud           = 115200U,
  k_demo_thread_stack   = 4096U,
  k_demo_media_buf_size = 512U,
  k_demo_file_chunk     = 64U,
} demo_config_t;

#ifndef RA_SIMULATOR_MODE
/* FileX state. ThreadX requires statically-allocated control blocks
 * (NASA Power of 10 Rule 3 -- no dynamic memory). */
static FX_MEDIA s_sd_media;
static FX_FILE  s_readme_file;
static UCHAR    s_media_memory[k_demo_media_buf_size];

/* ThreadX worker thread. */
static TX_THREAD s_demo_thread;
static UCHAR     s_demo_stack[k_demo_thread_stack];
#endif /* !RA_SIMULATOR_MODE */

/**
 * @brief Halt forever in WFI.
 *
 * @pre Called only after a fatal error.
 * @post CPU is parked.
 *
 * @since 0.1.0
 */
static void demo_panic_halt(void)
{
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

  if (ra_cgc_init() != k_ra_ok) {
    demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    demo_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    demo_panic_halt();
  }
  if (ra_board_uart_console_init((uint32_t)k_demo_baud) != k_ra_ok) {
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
  (void)ra_board_uart_console_write((const uint8_t*)s, len);
}

#ifndef RA_SIMULATOR_MODE
/**
 * @brief Walk the root directory and dump every entry to SCI8.
 *
 * @param[in,out] media Open FileX media.
 *
 * @pre media has been successfully ``fx_media_open()``-ed.
 * @post All root-directory entries (or first error) are printed.
 *
 * @since 0.1.0
 */
static void demo_list_root(FX_MEDIA* media)
{
  CHAR  name[FX_MAX_LONG_NAME_LEN];
  UINT  attributes = 0U;
  ULONG size       = 0U;
  UINT  status;

  demo_print("[filex] root listing:\r\n");

  status = fx_directory_first_full_entry_find(media,
                                              name,
                                              &attributes,
                                              &size,
                                              FX_NULL,
                                              FX_NULL,
                                              FX_NULL,
                                              FX_NULL,
                                              FX_NULL,
                                              FX_NULL);
  while (status == FX_SUCCESS) {
    demo_print("  ");
    demo_print(name);
    demo_print("\r\n");
    status = fx_directory_next_full_entry_find(media,
                                               name,
                                               &attributes,
                                               &size,
                                               FX_NULL,
                                               FX_NULL,
                                               FX_NULL,
                                               FX_NULL,
                                               FX_NULL,
                                               FX_NULL);
  }
}

/**
 * @brief Open README.TXT (if present) and dump its contents to SCI8.
 *
 * @param[in,out] media Open FileX media.
 *
 * @pre media has been successfully ``fx_media_open()``-ed.
 * @post README.TXT bytes are streamed to SCI8 (or skipped if absent).
 *
 * @since 0.1.0
 */
static void demo_dump_readme(FX_MEDIA* media)
{
  UINT  status;
  ULONG actual = 0U;
  CHAR  buf[k_demo_file_chunk];

  status = fx_file_open(media, &s_readme_file, "README.TXT", FX_OPEN_FOR_READ);
  if (status != FX_SUCCESS) {
    demo_print("[filex] no README.TXT at root\r\n");
    return;
  }
  demo_print("[filex] README.TXT contents:\r\n");
  do {
    actual = 0U;
    status = fx_file_read(&s_readme_file, buf, (ULONG)k_demo_file_chunk, &actual);
    if (actual > 0U) {
      (void)ra_board_uart_console_write((const uint8_t*)buf, (size_t)actual);
    }
  } while (status == FX_SUCCESS && actual > 0U);

  demo_print("\r\n[filex] END README.TXT\r\n");
  (void)fx_file_close(&s_readme_file);
}

/**
 * @brief ThreadX worker entry: open SD, list root, dump README.
 *
 * @param[in] thread_input Unused.
 *
 * @pre fx_system_initialize() has been called.
 * @post Either the listing + README dump completed, or an error was printed.
 *
 * @since 0.1.0
 */
static void demo_thread_entry(ULONG thread_input)
{
  (void)thread_input;

  UINT status = fx_media_open(&s_sd_media,
                              (CHAR*)"SD",
                              fx_media_driver_ra_sdhi,
                              FX_NULL,
                              s_media_memory,
                              (ULONG)sizeof(s_media_memory));
  if (status != FX_SUCCESS) {
    demo_print("[filex] fx_media_open failed\r\n");
    return;
  }

  demo_list_root(&s_sd_media);
  demo_dump_readme(&s_sd_media);

  (void)fx_media_close(&s_sd_media);
  demo_print("[filex] done\r\n");
}

/**
 * @brief ThreadX system-define hook: build the worker thread + FileX core.
 *
 * @param[in] first_unused_memory Pointer to the start of free RAM
 *   provided by the ThreadX port; unused -- we statically allocate.
 *
 * @pre tx_kernel_enter() has been called.
 * @post One worker thread is created and FileX is initialised.
 *
 * @since 0.1.0
 */
void tx_application_define(void* first_unused_memory)
{
  (void)first_unused_memory;

  fx_system_initialize();

  (void)tx_thread_create(&s_demo_thread,
                         "filex_demo",
                         demo_thread_entry,
                         0U,
                         s_demo_stack,
                         (ULONG)sizeof(s_demo_stack),
                         8U, /* priority */
                         8U, /* preempt threshold */
                         TX_NO_TIME_SLICE,
                         TX_AUTO_START);
}
#endif /* !RA_SIMULATOR_MODE */

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
  ra_isr_globals_enable();
  demo_print("[filex] booting ThreadX + FileX...\r\n");

#ifndef RA_SIMULATOR_MODE
  /* Hands control over to ThreadX permanently. */
  tx_kernel_enter();
#endif

  /* Should never return. */
  demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
