/**
 * @file examples/ek_ra8d2/hw_validated/hil/threadx_filex_demo/main.c
 * @brief ThreadX + FileX file-operations demo on the on-board OSPI flash
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * A FileX FAT file-operations exerciser backed by the EK-RA8D2's on-board
 * Octo-SPI NOR flash (via LevelX wear-levelling), NOT an SD card: the board's
 * microSD is on Pmod2 / SCI0 Simple-SPI, which the FileX SDHI driver cannot
 * reach, so this demo targets the always-present OSPI flash instead. It is the
 * file-ops counterpart to ``threadx_filex_levelx_demo`` (which proves the
 * LevelX integration); this one drives the FileX FAT API itself.
 *
 * Brings the chip up like ``uart_hello`` (CGC -> SCI8 @ 115200 8N1), then hands
 * control to ThreadX. ``tx_application_define`` spawns one worker that:
 *
 *   1. Formats + opens a LevelX NOR partition on the OSPI flash
 *      (``lx_nor_driver_ra8_xspi``).
 *   2. Lays down a fresh FAT volume on top via the LevelX<->FileX adapter
 *      (``lx_filex_adapter_bind`` -> ``fx_media_format`` -> ``fx_media_open``).
 *   3. Creates two files, lists the root, reads one back and verifies its
 *      bytes match, deletes the other and confirms it is gone.
 *   4. Prints ``[filex] ospi FAT roundtrip ok`` on success.
 *
 * No card, no jumpers -- the flash is soldered on the board, so this runs
 * unattended on the HIL bench.
 *
 * @author Brighton Sikarskie
 * @date 2026-06-15
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
 * The host unit-test build (RA8_OFF_TARGET) does not link the ThreadX /
 * FileX / LevelX vendor trees, so their headers are unreachable when clang-tidy
 * walks this file. Pull them in only on the cross-compile target.
 */
#ifndef RA8_OFF_TARGET
#include "fx_api.h"
#include "lx_api.h"
#include "lx_filex_adapter.h"
#include "lx_nor_driver_ra8_xspi.h"
#include "tx_api.h"
#endif

/** @brief Compile-time settings (console + FileX geometry). */
typedef enum : uint32_t {
  k_demo_baud           = 115200U, /**< SCI8 baud (matches uart_hello).   */
  k_demo_thread_stack   = 4096U,   /**< Worker-thread stack bytes.        */
  k_demo_media_buf_size = 512U,    /**< FileX media buffer (one sector).  */
  k_demo_file_chunk     = 64U,     /**< Read-back chunk for verification. */
  k_demo_sector_bytes   = 512U,    /**< LevelX logical sector size.       */
} demo_config_t;

#ifndef RA8_OFF_TARGET
/* Statically-allocated control blocks (NASA P10 Rule 3 -- no dynamic memory). */
static LX_NOR_FLASH s_nor_flash;
static FX_MEDIA     s_media;
static FX_FILE      s_file;
static UCHAR        s_media_memory[k_demo_media_buf_size];

static TX_THREAD s_demo_thread;
static UCHAR     s_demo_stack[k_demo_thread_stack];

/* Mutable names/paths -- the ThreadX/FileX/LevelX APIs take non-const CHAR*. */
static char s_lx_flash_name[]  = "ra8_xspi_nor";
static char s_fx_volume_name[] = "FXOSPI";
static char s_readme_path[]    = "readme.txt";
static char s_scratch_path[]   = "scratch.txt";
static char s_readme_text[]    = "FileX FAT on OSPI flash via LevelX.\r\n";
#endif /* !RA8_OFF_TARGET */

/**
 * @brief Halt forever in WFI.
 *
 * @pre Called only after a fatal error.
 * @post CPU is parked; only a debugger or reset recovers.
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
 * @brief Write a NUL-terminated ASCII string to the console.
 *
 * @param[in] s NUL-terminated ASCII string (NULL is ignored).
 * @post On success the bytes are queued in the BSP console TX FIFO.
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

#ifndef RA8_OFF_TARGET
/**
 * @brief Format + open the LevelX NOR partition on the OSPI flash.
 *
 * @details The LevelX NOR driver owns the OCTA bus bring-up (pin routing, the
 * 8D/1S software-reset recovery, ra8_xspi_init, RDID probe) inside
 * ::lx_nor_driver_ra8_xspi_initialize, so this must NOT also call ra8_xspi_init
 * (a second PFS route is rejected and format then fails). Let the driver own it.
 *
 * @return true on success; false (with a printed reason) on any LevelX error.
 * @pre LevelX has been initialised (::lx_nor_flash_initialize in the define hook).
 * @post On true ::s_nor_flash is open and ready for sector I/O.
 * @since 0.1.0
 */
static bool demo_lx_open(void)
{
  if (lx_nor_flash_format(&s_nor_flash,
                          s_lx_flash_name,
                          lx_nor_driver_ra8_xspi_initialize,
                          LX_NULL) != LX_SUCCESS) {
    demo_print("[filex] lx_nor_flash_format failed\r\n");
    return false;
  }
  if (lx_nor_flash_open(&s_nor_flash, s_lx_flash_name, lx_nor_driver_ra8_xspi_initialize) !=
      LX_SUCCESS) {
    demo_print("[filex] lx_nor_flash_open failed\r\n");
    return false;
  }
  return true;
}

/**
 * @brief Lay down + mount a FAT volume on top of the LevelX partition.
 *
 * @return true on success; false (with a printed reason) on any FileX error.
 * @pre ::demo_lx_open returned true.
 * @post On true ::s_media is mounted FAT and ready for file I/O.
 * @since 0.1.0
 */
static bool demo_fx_format(void)
{
  if (lx_filex_adapter_bind(&s_nor_flash) != FX_SUCCESS) {
    demo_print("[filex] lx_filex_adapter_bind failed\r\n");
    return false;
  }
  ULONG total_sectors = lx_filex_adapter_get_total_sectors();
  if (total_sectors == 0U) {
    demo_print("[filex] adapter reports zero sectors\r\n");
    return false;
  }
  UINT status = fx_media_format(&s_media,
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
    demo_print("[filex] fx_media_format failed\r\n");
    return false;
  }
  if (fx_media_open(&s_media,
                    s_fx_volume_name,
                    fx_media_driver_ra8_levelx,
                    LX_NULL,
                    s_media_memory,
                    (ULONG)sizeof(s_media_memory)) != FX_SUCCESS) {
    demo_print("[filex] fx_media_open failed\r\n");
    return false;
  }
  return true;
}

/**
 * @brief Create @p path and write @p text (NUL excluded) into it.
 *
 * @param[in] path NUL-terminated FAT path at the root.
 * @param[in] text NUL-terminated payload to write.
 * @return true if the file was created, written, and flushed.
 * @pre ::s_media is mounted FAT.
 * @post On true @p path exists holding exactly strlen(@p text) bytes.
 * @since 0.1.0
 */
static bool demo_write_file(char* path, char* text)
{
  UINT status = fx_file_create(&s_media, path);
  if (status != FX_SUCCESS && status != FX_ALREADY_CREATED) {
    demo_print("[filex] fx_file_create failed\r\n");
    return false;
  }
  if (fx_file_open(&s_media, &s_file, path, FX_OPEN_FOR_WRITE) != FX_SUCCESS) {
    demo_print("[filex] fx_file_open(write) failed\r\n");
    return false;
  }
  bool ok = fx_file_write(&s_file, text, (ULONG)strlen(text)) == FX_SUCCESS;
  (void)fx_file_close(&s_file);
  (void)fx_media_flush(&s_media);
  if (!ok) {
    demo_print("[filex] fx_file_write failed\r\n");
  }
  return ok;
}

/**
 * @brief Read @p path back and verify its bytes equal @p expected.
 *
 * @param[in] path     NUL-terminated FAT path at the root.
 * @param[in] expected NUL-terminated bytes the file must contain.
 * @return true if the readback matched @p expected exactly.
 * @pre ::s_media is mounted FAT and @p path exists.
 * @post The file is closed; the readback is echoed to the console.
 * @since 0.1.0
 */
static bool demo_read_verify(char* path, const char* expected)
{
  if (fx_file_open(&s_media, &s_file, path, FX_OPEN_FOR_READ) != FX_SUCCESS) {
    demo_print("[filex] fx_file_open(read) failed\r\n");
    return false;
  }
  CHAR     buf[k_demo_file_chunk];
  ULONG    actual = 0U;
  uint32_t off    = 0U;
  bool     ok     = true;
  do {
    actual      = 0U;
    UINT status = fx_file_read(&s_file, buf, (ULONG)k_demo_file_chunk, &actual);
    if (actual > 0U) {
      if (memcmp(buf, &expected[off], (size_t)actual) != 0) {
        ok = false;
      }
      off += (uint32_t)actual;
    }
    if (status != FX_SUCCESS) {
      break;
    }
  } while (actual > 0U);
  (void)fx_file_close(&s_file);
  ok = ok && (off == (uint32_t)strlen(expected));
  return ok;
}

/**
 * @brief Walk the FAT root directory and print every entry name.
 *
 * @param[in,out] media Open FileX media.
 * @pre @p media has been successfully ::fx_media_open-ed.
 * @post All root-directory entry names are printed to the console.
 * @since 0.1.0
 */
static void demo_list_root(FX_MEDIA* media)
{
  CHAR name[FX_MAX_LONG_NAME_LEN];
  demo_print("[filex] root listing:\r\n");
  UINT status = fx_directory_first_full_entry_find(media,
                                                   name,
                                                   FX_NULL,
                                                   FX_NULL,
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
                                               FX_NULL,
                                               FX_NULL,
                                               FX_NULL,
                                               FX_NULL,
                                               FX_NULL,
                                               FX_NULL,
                                               FX_NULL,
                                               FX_NULL);
  }
}

/**
 * @brief Run the file-ops sequence; print the pass banner only if all pass.
 *
 * @details create two files -> list root -> read+verify readme -> delete
 * scratch -> confirm the delete. Any failure returns without the banner.
 *
 * @pre ::s_media is mounted FAT.
 * @post On success ``[filex] ospi FAT roundtrip ok`` is queued.
 * @since 0.1.0
 */
static void demo_file_ops(void)
{
  if (!demo_write_file(s_readme_path, s_readme_text)) {
    return;
  }
  if (!demo_write_file(s_scratch_path, s_readme_text)) {
    return;
  }
  demo_list_root(&s_media);
  if (!demo_read_verify(s_readme_path, s_readme_text)) {
    demo_print("[filex] readback mismatch\r\n");
    return;
  }
  demo_print("[filex] readback verified\r\n");
  if (fx_file_delete(&s_media, s_scratch_path) != FX_SUCCESS) {
    demo_print("[filex] fx_file_delete failed\r\n");
    return;
  }
  (void)fx_media_flush(&s_media);
  if (fx_file_open(&s_media, &s_file, s_scratch_path, FX_OPEN_FOR_READ) == FX_SUCCESS) {
    (void)fx_file_close(&s_file);
    demo_print("[filex] delete did not remove the file\r\n");
    return;
  }
  demo_print("[filex] ospi FAT roundtrip ok\r\n");
}

/**
 * @brief ThreadX worker entry: bring up LevelX + FAT, run the file ops.
 *
 * @param[in] thread_input Unused.
 * @pre ::tx_application_define scheduled this thread.
 * @post LevelX + FileX are closed; the verdict has been printed.
 * @since 0.1.0
 */
static void demo_thread_entry(ULONG thread_input)
{
  (void)thread_input;

  demo_print("[filex] formatting + opening LevelX on OSPI flash\r\n");
  if (!demo_lx_open()) {
    return;
  }
  demo_print("[filex] formatting + opening FAT volume\r\n");
  if (!demo_fx_format()) {
    (void)lx_nor_flash_close(&s_nor_flash);
    return;
  }
  demo_file_ops();
  (void)fx_media_close(&s_media);
  (void)lx_nor_flash_close(&s_nor_flash);
  demo_print("[filex] done\r\n");
}

/**
 * @brief ThreadX system-define hook: build the worker + FileX + LevelX cores.
 *
 * @param[in] first_unused_memory Free-RAM sentinel from the port (unused;
 *   control blocks are static).
 * @pre ::tx_kernel_enter has been called.
 * @post One worker thread is created and FileX + LevelX are initialised.
 * @since 0.1.0
 */
void tx_application_define(void* first_unused_memory)
{
  (void)first_unused_memory;

  fx_system_initialize();
  lx_nor_flash_initialize();

  (void)tx_thread_create(&s_demo_thread,
                         "filex_demo",
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
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @post On clean entry the kernel runs the worker thread once.
 * @since 0.1.0
 */
int32_t main(void)
{
  demo_setup_or_halt();
  ra8_isr_globals_enable();
  demo_print("[filex] booting ThreadX + FileX on OSPI flash...\r\n");

#ifndef RA8_OFF_TARGET
  tx_kernel_enter();
#endif

  demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
