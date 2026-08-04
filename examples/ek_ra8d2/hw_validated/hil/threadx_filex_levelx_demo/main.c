/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hw_validated/hil/threadx_filex_levelx_demo/main.c
 * @brief ThreadX + FileX-on-LevelX-on-OSPI wear-levelled FAT demo for EK-RA8D2
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
 *      partition and ``lx_nor_flash_open`` to mount it.
 *   3. Binds the LevelX flash to the FileX adapter
 *      (``lx_filex_adapter_bind``).
 *   4. Calls ``fx_media_format`` followed by ``fx_media_open`` to
 *      lay down a FAT12 volume on top of the wear-levelled blocks.
 *   5. Creates ``/levelx_test.txt``, writes a known message, closes
 *      it, reopens for read, reads it back and prints both the
 *      message and the file size to SCI8.
 *   6. Closes the media + LevelX cleanly and idles.
 *
 * Recipe (see also the README.md alongside this main.c):
 *
 *   - Connect a USB cable to the J-Link OB CDC port on the EK-RA8D2.
 *   - Open a 115200 8N1 terminal.
 *   - Flash this firmware (``make flash``).
 *   - Watch the [fxlx] log lines stream out, ending with
 *     ``[fxlx] readback: Hello from wear-leveled FAT!`` on success.
 *
 * @author Brighton Sikarskie
 * @date 2026-04-29
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
 * The host unit-test build (RA8_OFF_TARGET) does not link the
 * ThreadX / FileX / LevelX vendor trees, so their headers are
 * unreachable when clang-tidy walks this file. Pull them in only on
 * the cross-compile target.
 */
#ifndef RA8_OFF_TARGET
#include "fx_api.h"
#include "lx_api.h"
#include "lx_filex_adapter.h"
#include "lx_nor_driver_ra8_xspi.h"
#include "tx_api.h"
#endif

/**
 * @brief Compile-time settings for the FileX-on-LevelX FAT demo.
 */
typedef enum : uint32_t {
  /** @brief SCI8 baud (matches uart_hello / threadx_filex_demo). */
  k_demo_baud = 115200U,

  /** @brief Worker-thread stack size in bytes. */
  k_demo_thread_stack = 4096U,

  /** @brief FileX media memory buffer size in bytes (one sector). */
  k_demo_media_buf_size = 512U,

  /** @brief Read-back chunk size used when dumping the file to SCI8. */
  k_demo_file_chunk = 64U,

  /** @brief FAT volume name (8 chars + NUL). */
  k_demo_volume_label_max = 12U,

  /** @brief LevelX bytes per logical sector. */
  k_demo_sector_bytes = 512U,
} demo_config_t;

/** @brief Test message written into /levelx_test.txt. */
static char s_demo_test_message[] = "Hello from wear-leveled FAT!";

/** @brief LevelX flash name (mutable so ThreadX/LevelX can take CHAR*). */
static char s_lx_flash_name[] = "ra8_xspi_nor";

/** @brief FileX volume name (mutable so FileX can take CHAR*). */
static char s_fx_volume_name[] = "FXLX";

/** @brief Path under the FAT root that holds the demo payload. */
static char s_demo_file_path[] = "levelx_test.txt";

/** @brief Worker-thread name (mutable so ThreadX can take CHAR*). */
static char s_demo_thread_name[] = "fxlx_demo";

#ifndef RA8_OFF_TARGET
/* LevelX state. ThreadX requires statically-allocated control blocks
 * (NASA Power of 10 Rule 3 -- no dynamic memory). */
static LX_NOR_FLASH s_nor_flash;

/* FileX state. */
static FX_MEDIA s_media;
static FX_FILE  s_file;
static UCHAR    s_media_memory[k_demo_media_buf_size];

/* ThreadX worker thread. */
static TX_THREAD s_demo_thread;
static UCHAR     s_demo_stack[k_demo_thread_stack];
#endif /* !RA8_OFF_TARGET */

/**
 * @brief Halt forever in WFI, after draining the SCI8 TX shift register.
 *
 * @details
 * Calls ``ra8_board_uart_console_flush`` so any panic message previously
 * queued via ``ra8_board_uart_console_write`` finishes clocking onto the
 * wire before WFI gates the SCI clock. Without the flush, only the first
 * 1-3 bytes of the failure log reach the host UART because WFI silences
 * the peripheral mid-frame. Return code is intentionally discarded -- if
 * the flush times out we still want to halt rather than spin.
 *
 * @pre Called only after a fatal error.
 * @post Pending SCI8 TX has drained (or the flush budget expired) and
 *       the CPU is parked.
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
 * @brief Bring CGC + SCI8 up. Panic-halts on any failure.
 *
 * @pre Reset_Handler / SystemInit complete.
 * @post On success SCI8 is sending at 115200 8N1.
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
 * @brief Convenience wrapper to write a NUL-terminated string to SCI8.
 *
 * @param[in] s NUL-terminated ASCII string. Must not be NULL.
 *
 * @pre s != NULL.
 * @post On success the bytes are queued in the SCI8 TX FIFO.
 *
 * @since 0.1.0
 */
static void demo_print(const char* s)
{
  if (s == (const char*)0) {
    return;
  }
  uint32_t len = (uint32_t)strlen(s);
  (void)ra8_board_uart_console_write((const uint8_t*)s, (size_t)len);
}

#ifndef RA8_OFF_TARGET
/**
 * @brief Format + open the LevelX partition. Panics on any failure.
 *
 * @pre ``ra8_xspi_init`` succeeded for instance 0.
 * @post On success ``s_nor_flash`` is open and ready for sector I/O.
 *
 * @since 0.1.0
 */
static void demo_lx_open_or_panic(void)
{
  UINT status =
    lx_nor_flash_format(&s_nor_flash, s_lx_flash_name, lx_nor_driver_ra8_xspi_initialize, LX_NULL);
  if (status != LX_SUCCESS) {
    demo_print("[fxlx] lx_nor_flash_format failed\r\n");
    demo_panic_halt();
  }
  status = lx_nor_flash_open(&s_nor_flash, s_lx_flash_name, lx_nor_driver_ra8_xspi_initialize);
  if (status != LX_SUCCESS) {
    demo_print("[fxlx] lx_nor_flash_open failed\r\n");
    demo_panic_halt();
  }
}

/**
 * @brief Format + open the FAT volume that lives on top of LevelX.
 *
 * @details
 * Calls ``lx_filex_adapter_bind`` first so the adapter knows which
 * LevelX flash to dispatch into, then ``fx_media_format`` to lay
 * down a fresh FAT12 superblock, then ``fx_media_open`` to mount it.
 *
 * @pre ``s_nor_flash`` was successfully opened.
 *
 * @post On success ``s_media`` is mounted FAT and ready for file I/O.
 *
 * @since 0.1.0
 */
static void demo_fx_format_or_panic(void)
{
  UINT status = lx_filex_adapter_bind(&s_nor_flash);
  if (status != FX_SUCCESS) {
    demo_print("[fxlx] lx_filex_adapter_bind failed\r\n");
    demo_panic_halt();
  }
  ULONG total_sectors = lx_filex_adapter_get_total_sectors();
  if (total_sectors == 0U) {
    demo_print("[fxlx] adapter reports zero sectors\r\n");
    demo_panic_halt();
  }
  status = fx_media_format(&s_media,
                           fx_media_driver_ra8_levelx,
                           LX_NULL,
                           s_media_memory,
                           (UINT)sizeof(s_media_memory),
                           s_fx_volume_name,
                           1U,                        /* number of FATs         */
                           32U,                       /* root directory entries */
                           0U,                        /* hidden sectors         */
                           total_sectors,             /* total logical sectors  */
                           (UINT)k_demo_sector_bytes, /* bytes per sector       */
                           1U,                        /* sectors per cluster    */
                           1U,                        /* heads (non-rotating)   */
                           1U);                       /* sectors per track      */
  if (status != FX_SUCCESS) {
    demo_print("[fxlx] fx_media_format failed\r\n");
    demo_panic_halt();
  }
  status = fx_media_open(&s_media,
                         s_fx_volume_name,
                         fx_media_driver_ra8_levelx,
                         LX_NULL,
                         s_media_memory,
                         (ULONG)sizeof(s_media_memory));
  if (status != FX_SUCCESS) {
    demo_print("[fxlx] fx_media_open failed\r\n");
    demo_panic_halt();
  }
}

/**
 * @brief Create + write /levelx_test.txt with the demo test message.
 *
 * @pre ``s_media`` is mounted FAT.
 *
 * @post On success the file exists on the FAT volume containing
 *       exactly ``s_demo_test_message`` bytes.
 *
 * @since 0.1.0
 */
static void demo_write_test_file(void)
{
  UINT status = fx_file_create(&s_media, s_demo_file_path);
  if (status != FX_SUCCESS && status != FX_ALREADY_CREATED) {
    demo_print("[fxlx] fx_file_create failed\r\n");
    return;
  }
  status = fx_file_open(&s_media, &s_file, s_demo_file_path, FX_OPEN_FOR_WRITE);
  if (status != FX_SUCCESS) {
    demo_print("[fxlx] fx_file_open(write) failed\r\n");
    return;
  }
  ULONG msg_bytes = (ULONG)strlen(s_demo_test_message);
  status          = fx_file_write(&s_file, s_demo_test_message, msg_bytes);
  if (status != FX_SUCCESS) {
    demo_print("[fxlx] fx_file_write failed\r\n");
  } else {
    demo_print("[fxlx] wrote /levelx_test.txt: ");
    demo_print(s_demo_test_message);
    demo_print("\r\n");
  }
  (void)fx_file_close(&s_file);
  (void)fx_media_flush(&s_media);
}

/**
 * @brief Open + read /levelx_test.txt back to SCI8.
 *
 * @pre ``s_media`` is mounted FAT and the file exists.
 *
 * @post The file's contents have been streamed to SCI8.
 *
 * @since 0.1.0
 */
static void demo_read_test_file(void)
{
  UINT status = fx_file_open(&s_media, &s_file, s_demo_file_path, FX_OPEN_FOR_READ);
  if (status != FX_SUCCESS) {
    demo_print("[fxlx] fx_file_open(read) failed\r\n");
    return;
  }
  demo_print("[fxlx] readback: ");
  CHAR  buf[k_demo_file_chunk];
  ULONG actual = 0U;
  do {
    actual = 0U;
    status = fx_file_read(&s_file, buf, (ULONG)k_demo_file_chunk, &actual);
    if (actual > 0U) {
      (void)ra8_board_uart_console_write((const uint8_t*)buf, (size_t)actual);
    }
  } while (status == FX_SUCCESS && actual > 0U);
  demo_print("\r\n");
  (void)fx_file_close(&s_file);
}

/**
 * @brief ThreadX worker entry: bring up LevelX + FileX, exercise the file.
 *
 * @param[in] thread_input Unused.
 *
 * @pre ``tx_application_define`` has scheduled this thread.
 * @post On success the test file has been written + read back.
 *
 * @since 0.1.0
 */
static void demo_thread_entry(ULONG thread_input)
{
  (void)thread_input;

  demo_print("[fxlx] booting xSPI flash\r\n");
  /* The LevelX NOR driver owns OCTA bus bring-up: lx_nor_flash_format ->
   * lx_nor_driver_ra8_xspi_initialize -> priv_bus_init_once routes the
   * pins, runs the 8D/1S software-reset recovery, calls ra8_xspi_init,
   * and probes RDID exactly once. Doing it here as well double-routes
   * the PFS pins (the validator rejects the second route with
   * k_ra8_err_gpio_conflict), so the driver's initialize bails before
   * wiring the sector buffer and format returns LX_NO_MEMORY. Let the
   * driver be the single owner. */
  demo_print("[fxlx] formatting + opening LevelX partition\r\n");
  demo_lx_open_or_panic();

  demo_print("[fxlx] formatting + opening FAT volume on LevelX\r\n");
  demo_fx_format_or_panic();

  demo_write_test_file();
  demo_read_test_file();

  (void)fx_media_close(&s_media);
  (void)lx_nor_flash_close(&s_nor_flash);
  demo_print("[fxlx] done\r\n");
}

/**
 * @brief ThreadX system-define hook: build the worker + FileX + LevelX cores.
 *
 * @param[in] first_unused_memory Pointer to the start of free RAM
 *   provided by the ThreadX port; unused -- we statically allocate.
 *
 * @pre ``tx_kernel_enter()`` has been called.
 * @post One worker thread is created and FileX + LevelX are initialized.
 *
 * @since 0.1.0
 */
void tx_application_define(void* first_unused_memory)
{
  (void)first_unused_memory;

  fx_system_initialize();
  lx_nor_flash_initialize();

  (void)tx_thread_create(&s_demo_thread,
                         s_demo_thread_name,
                         demo_thread_entry,
                         0U,
                         s_demo_stack,
                         (ULONG)sizeof(s_demo_stack),
                         8U, /* priority          */
                         8U, /* preempt threshold */
                         TX_NO_TIME_SLICE,
                         TX_AUTO_START);
}
#endif /* !RA8_OFF_TARGET */

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
  demo_print("[fxlx] booting ThreadX + FileX-on-LevelX...\r\n");

#ifndef RA8_OFF_TARGET
  /* Hands control over to ThreadX permanently. */
  tx_kernel_enter();
#endif

  /* Should never return. */
  demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
