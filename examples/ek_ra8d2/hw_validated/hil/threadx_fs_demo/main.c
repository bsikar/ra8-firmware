/**
 * @file examples/ek_ra8d2/hw_validated/hil/threadx_fs_demo/main.c
 * @brief ThreadX + ra8_fs file-operations demo on the on-board OSPI flash
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * An ra8_fs FAT file-operations exerciser backed by the EK-RA8D2's on-board
 * Octo-SPI NOR flash (via LevelX wear-levelling), NOT an SD card: the board's
 * microSD is on Pmod2 / SCI0 Simple-SPI, and the always-present OSPI flash
 * needs no card at all. It is the file-ops counterpart to
 * ``threadx_fs_levelx_demo`` (which proves the LevelX integration); this one
 * drives the ra8_fs FAT API itself, from an RTOS world through the
 * ``ra8_fs_set_lock()`` seam bound to a ThreadX mutex (#608).
 *
 * Brings the chip up like ``uart_hello`` (CGC -> SCI8 @ 115200 8N1), then hands
 * control to ThreadX. ``tx_application_define`` spawns one worker that:
 *
 *   1. Formats + opens a LevelX NOR partition on the OSPI flash
 *      (``lx_nor_driver_ra8_xspi``).
 *   2. Lays down a fresh FAT volume on top via the LevelX<->ra8_fs backend
 *      (``lx_fs_backend_bind`` -> ``ra8_fs_format`` -> ``ra8_fs_mount``).
 *   3. Creates two files, lists the root, reads one back and verifies its
 *      bytes match, deletes the other and confirms it is gone.
 *   4. Prints ``[fs] ospi FAT roundtrip ok`` on success.
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
#include "ra8_fs.h"
#include "ra8_isr.h"
#include "ra8_time.h"

/*
 * The host unit-test build (RA8_OFF_TARGET) does not link the ThreadX /
 * LevelX vendor trees, so their headers are unreachable when clang-tidy
 * walks this file. Pull them in only on the cross-compile target.
 */
#ifndef RA8_OFF_TARGET
#include "lx_api.h"
#include "lx_fs_backend.h"
#include "lx_nor_driver_ra8_xspi.h"
#include "tx_api.h"
#endif

/** @brief Compile-time settings (console + worker geometry). */
typedef enum : uint32_t {
  k_demo_baud         = 115200U, /**< SCI8 baud (matches uart_hello).   */
  k_demo_thread_stack = 8192U,   /**< Worker-thread stack bytes.        */
  k_demo_file_chunk   = 64U,     /**< Read-back chunk for verification. */
} demo_config_t;

#ifndef RA8_OFF_TARGET
/** @brief Test payload written into both demo files. */
static const char s_readme_text[] = "ra8_fs FAT on OSPI flash via LevelX.\r\n";

/** @brief Path of the file that is read back and verified. */
static const char s_readme_path[] = "/readme.txt";

/** @brief Path of the file that is deleted and confirmed gone. */
static const char s_scratch_path[] = "/scratch.txt";

/* Statically-allocated control blocks (NASA P10 Rule 3 -- no dynamic memory). */
static LX_NOR_FLASH s_nor_flash;

static TX_THREAD s_demo_thread;
static UCHAR     s_demo_stack[k_demo_thread_stack];

/** @brief ThreadX mutex the ra8_fs lock seam is bound to (#608). */
static TX_MUTEX s_fs_mutex;

/* Mutable name -- the ThreadX/LevelX APIs take non-const CHAR*. */
static char s_lx_flash_name[] = "ra8_xspi_nor";
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
 * @brief ra8_fs lock-seam acquire: block on the ThreadX mutex.
 *
 * @param[in] ctx The bound ``TX_MUTEX*``.
 * @pre The mutex was created; the seam contract forbids failure, so wait forever.
 * @post The calling thread holds the mutex.
 * @since 0.1.0
 */
static void demo_fs_lock_acquire(void* ctx)
{
  (void)tx_mutex_get((TX_MUTEX*)ctx, TX_WAIT_FOREVER);
}

/**
 * @brief ra8_fs lock-seam release: hand the ThreadX mutex back.
 *
 * @param[in] ctx The bound ``TX_MUTEX*``.
 * @pre The calling thread holds the mutex (acquire/release pair up).
 * @post The mutex is released.
 * @since 0.1.0
 */
static void demo_fs_lock_release(void* ctx)
{
  (void)tx_mutex_put((TX_MUTEX*)ctx);
}

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
    demo_print("[fs] lx_nor_flash_format failed\r\n");
    return false;
  }
  if (lx_nor_flash_open(&s_nor_flash, s_lx_flash_name, lx_nor_driver_ra8_xspi_initialize) !=
      LX_SUCCESS) {
    demo_print("[fs] lx_nor_flash_open failed\r\n");
    return false;
  }
  return true;
}

/**
 * @brief Bind the backend, lay down + mount a FAT volume on the LevelX partition.
 *
 * @details ``lx_fs_backend_bind`` wires the three ::ra8_fs_backend_t callbacks
 * onto ::s_nor_flash; ``ra8_fs_format`` then writes a FAT12 superfloppy (the
 * xSPI driver exposes a 64-block LevelX window -- 384 usable 512-byte
 * sectors -- so FAT12 is the band that fits, exactly as the retired FileX
 * format auto-selected) and ``ra8_fs_mount`` parses it back.
 *
 * @param[out] out_backend Receives the bound backend (must outlive the mount).
 * @param[out] out_mount   Receives the mount handle.
 * @return true on success; false (with a printed reason) on any failure.
 * @pre ::demo_lx_open returned true.
 * @post On true ``*out_mount`` is a mounted FAT volume ready for file I/O.
 * @since 0.1.0
 */
static bool demo_fs_format_mount(ra8_fs_backend_t* out_backend, ra8_fs_mount_t** out_mount)
{
  if (lx_fs_backend_bind(&s_nor_flash, out_backend) != k_ra8_ok) {
    demo_print("[fs] lx_fs_backend_bind failed\r\n");
    return false;
  }
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_fat12;
  opts.label                = "RA8FS";
  if (ra8_fs_format(out_backend, &opts) != k_ra8_ok) {
    demo_print("[fs] ra8_fs_format failed\r\n");
    return false;
  }
  if (ra8_fs_mount(out_backend, out_mount) != k_ra8_ok) {
    demo_print("[fs] ra8_fs_mount failed\r\n");
    return false;
  }
  return true;
}

/**
 * @brief Read @p path back and verify its bytes equal @p expected.
 *
 * @param[in,out] mount    Mounted FAT volume.
 * @param[in]     path     NUL-terminated path at the root.
 * @param[in]     expected NUL-terminated bytes the file must contain.
 * @return true if the readback matched @p expected exactly.
 * @pre @p mount is mounted and @p path exists.
 * @post The file is closed again; no volume state is modified.
 * @since 0.1.0
 */
static bool demo_read_verify(ra8_fs_mount_t* mount, const char* path, const char* expected)
{
  ra8_fs_file_t* file = nullptr;
  if (ra8_fs_open(mount, path, k_ra8_fs_mode_read, &file) != k_ra8_ok) {
    demo_print("[fs] ra8_fs_open(read) failed\r\n");
    return false;
  }
  uint8_t  buf[k_demo_file_chunk];
  uint32_t off = 0U;
  bool     ok  = true;
  uint32_t got = 0U;
  do {
    got = 0U;
    if (ra8_fs_read(file, buf, (uint32_t)k_demo_file_chunk, &got) != k_ra8_ok) {
      ok = false;
      break;
    }
    if (got > 0U) {
      if (memcmp(buf, &expected[off], (size_t)got) != 0) {
        ok = false;
      }
      off += got;
    }
  } while (got > 0U);
  (void)ra8_fs_close(file);
  ok = ok && (off == (uint32_t)strlen(expected));
  return ok;
}

/**
 * @brief ra8_fs_listdir callback: print one root-directory entry name.
 *
 * @param[in] name Entry name (NUL-terminated UTF-8).
 * @param[in] attr FAT attribute bits (unused).
 * @param[in] size Entry size in bytes (unused).
 * @param[in] ctx  Unused cookie.
 * @pre Invoked by ::ra8_fs_listdir only.
 * @post One indented name line is queued on the console.
 * @since 0.1.0
 */
static void demo_list_entry(const char* name, uint8_t attr, uint64_t size, void* ctx)
{
  (void)attr;
  (void)size;
  (void)ctx;
  demo_print("  ");
  demo_print(name);
  demo_print("\r\n");
}

/**
 * @brief Run the file-ops sequence; print the pass banner only if all pass.
 *
 * @details create two files -> list root -> read+verify readme -> delete
 * scratch -> confirm the delete. Any failure returns without the banner.
 *
 * @param[in,out] mount Mounted FAT volume.
 * @pre @p mount is mounted for read/write.
 * @post On success ``[fs] ospi FAT roundtrip ok`` is queued.
 * @since 0.1.0
 */
static void demo_file_ops(ra8_fs_mount_t* mount)
{
  const uint8_t* payload = (const uint8_t*)s_readme_text;
  uint32_t       len     = (uint32_t)strlen(s_readme_text);
  if (ra8_fs_write_file(mount, s_readme_path, payload, len) != k_ra8_ok) {
    demo_print("[fs] ra8_fs_write_file failed\r\n");
    return;
  }
  if (ra8_fs_write_file(mount, s_scratch_path, payload, len) != k_ra8_ok) {
    demo_print("[fs] ra8_fs_write_file failed\r\n");
    return;
  }
  demo_print("[fs] root listing:\r\n");
  if (ra8_fs_listdir(mount, "/", demo_list_entry, nullptr) != k_ra8_ok) {
    demo_print("[fs] ra8_fs_listdir failed\r\n");
    return;
  }
  if (!demo_read_verify(mount, s_readme_path, s_readme_text)) {
    demo_print("[fs] readback mismatch\r\n");
    return;
  }
  demo_print("[fs] readback verified\r\n");
  if (ra8_fs_unlink(mount, s_scratch_path) != k_ra8_ok) {
    demo_print("[fs] ra8_fs_unlink failed\r\n");
    return;
  }
  ra8_fs_stat_t st = {};
  if (ra8_fs_stat(mount, s_scratch_path, &st) != k_ra8_err_not_found) {
    demo_print("[fs] delete did not remove the file\r\n");
    return;
  }
  demo_print("[fs] ospi FAT roundtrip ok\r\n");
}

/**
 * @brief ThreadX worker entry: bring up LevelX + FAT, run the file ops.
 *
 * @param[in] thread_input Unused.
 * @pre ::tx_application_define scheduled this thread.
 * @post LevelX + the mount are closed; the verdict has been printed.
 * @since 0.1.0
 */
static void demo_thread_entry(ULONG thread_input)
{
  (void)thread_input;

  demo_print("[fs] formatting + opening LevelX on OSPI flash\r\n");
  if (!demo_lx_open()) {
    return;
  }
  demo_print("[fs] formatting + mounting FAT volume\r\n");
  ra8_fs_backend_t backend = {};
  ra8_fs_mount_t*  mount   = nullptr;
  if (!demo_fs_format_mount(&backend, &mount)) {
    (void)lx_nor_flash_close(&s_nor_flash);
    return;
  }
  demo_file_ops(mount);
  (void)ra8_fs_unmount(mount);
  (void)lx_nor_flash_close(&s_nor_flash);
  demo_print("[fs] done\r\n");
}

/**
 * @brief ThreadX system-define hook: build the worker + the ra8_fs lock seam.
 *
 * @details Creates the mutex first and installs it through
 * ``ra8_fs_set_lock()`` before the worker can issue a filesystem call, per the
 * seam's init-time contract (#608). LevelX's core is initialised here too.
 *
 * @param[in] first_unused_memory Free-RAM sentinel from the port (unused;
 *   control blocks are static).
 * @pre ::tx_kernel_enter has been called.
 * @post One worker thread is created; LevelX and the lock seam are initialised.
 * @since 0.1.0
 */
void tx_application_define(void* first_unused_memory)
{
  (void)first_unused_memory;

  lx_nor_flash_initialize();

  (void)tx_mutex_create(&s_fs_mutex, "ra8_fs", TX_NO_INHERIT);
  const ra8_fs_lock_t lock = {
    .acquire = demo_fs_lock_acquire,
    .release = demo_fs_lock_release,
    .ctx     = &s_fs_mutex,
  };
  if (ra8_fs_set_lock(&lock) != k_ra8_ok) {
    demo_print("[fs] ra8_fs_set_lock failed\r\n");
  }

  (void)tx_thread_create(&s_demo_thread,
                         "fs_demo",
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
  demo_print("[fs] booting ThreadX + ra8_fs on OSPI flash...\r\n");

#ifndef RA8_OFF_TARGET
  tx_kernel_enter();
#endif

  demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
