/**
 * @file examples/ek_ra8d2/hw_validated/hil/threadx_fs_levelx_demo/src/main.c
 * @brief ThreadX + ra8_fs-on-LevelX-on-OSPI wear-levelled FAT demo for EK-RA8D2
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
 *      IS25LX512M octal-SPI flash chip (via the LevelX NOR driver).
 *   2. Calls ``lx_nor_flash_format`` once to lay down a fresh LevelX
 *      partition and ``lx_nor_flash_open`` to mount it.
 *   3. Binds the LevelX flash to the ra8_fs block-device backend
 *      (``lx_fs_backend_bind``) and installs the ``ra8_fs_set_lock()``
 *      seam over a ThreadX mutex (#608).
 *   4. Calls ``ra8_fs_format`` followed by ``ra8_fs_mount`` to lay down
 *      a FAT volume on top of the wear-levelled blocks.
 *   5. Writes ``/levelx_test.txt`` with a known message, then reopens
 *      it for read, reads it back and prints the message to SCI8.
 *   6. Unmounts + closes LevelX cleanly and idles.
 *
 * Recipe (see also the README.md alongside this main.c):
 *
 *   - Connect a USB cable to the J-Link OB CDC port on the EK-RA8D2.
 *   - Open a 115200 8N1 terminal.
 *   - Flash this firmware (``just apps::hardware::flash threadx_fs_levelx_demo``).
 *   - Watch the [fslx] log lines stream out, ending with
 *     ``[fslx] readback: Hello from wear-leveled FAT!`` on success.
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
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_isr.h"
#include "ra8_time.h"

/*
 * The host unit-test build (RA8_OFF_TARGET) does not link the
 * ThreadX / LevelX vendor trees, so `tx_api.h` and `lx_api.h` are
 * unreachable when clang-tidy walks this file. Pull them in only on
 * the cross-compile target.
 */
#ifndef RA8_OFF_TARGET
#include "lx_api.h"
#include "lx_fs_backend.h"
#include "lx_nor_driver_ra8_xspi.h"
#include "tx_api.h"
#endif

/**
 * @brief Compile-time settings for the ra8_fs-on-LevelX FAT demo.
 */
typedef enum : uint32_t {
  /** @brief SCI8 baud (matches uart_hello / threadx_fs_demo). */
  k_demo_baud = 115200U,

  /** @brief Worker-thread stack size in bytes. */
  k_demo_thread_stack = 8192U,

  /** @brief Read-back chunk size used when dumping the file to SCI8. */
  k_demo_file_chunk = 64U,
} demo_config_t;

/** @brief Test message written into /levelx_test.txt. */
static const char s_demo_test_message[] = "Hello from wear-leveled FAT!";

/** @brief Path under the FAT root that holds the demo payload. */
static const char s_demo_file_path[] = "/levelx_test.txt";

#ifndef RA8_OFF_TARGET
/* LevelX state. ThreadX requires statically-allocated control blocks
 * (NASA Power of 10 Rule 3 -- no dynamic memory). */
static LX_NOR_FLASH s_nor_flash;

/* ra8_fs state: the bound backend must outlive the mount. */
static ra8_fs_backend_t s_fs_backend;
static ra8_fs_mount_t*  s_fs_mount;

/** @brief ThreadX mutex the ra8_fs lock seam is bound to (#608). */
static TX_MUTEX s_fs_mutex;

/* ThreadX worker thread. */
static TX_THREAD s_demo_thread;
static UCHAR     s_demo_stack[k_demo_thread_stack];

/** @brief LevelX flash name (mutable so ThreadX/LevelX can take CHAR*). */
static char s_lx_flash_name[] = "ra8_xspi_nor";

/** @brief Worker-thread name (mutable so ThreadX can take CHAR*). */
static char s_demo_thread_name[] = "fslx_demo";
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
 * @brief Format + open the LevelX partition. Panics on any failure.
 *
 * @pre ``ra8_xspi_init`` succeeded for instance 0 (the driver owns it).
 * @post On success ``s_nor_flash`` is open and ready for sector I/O.
 *
 * @since 0.1.0
 */
static void demo_lx_open_or_panic(void)
{
  UINT status =
    lx_nor_flash_format(&s_nor_flash, s_lx_flash_name, lx_nor_driver_ra8_xspi_initialize, LX_NULL);
  if (status != LX_SUCCESS) {
    demo_print("[fslx] lx_nor_flash_format failed\r\n");
    demo_panic_halt();
  }
  status = lx_nor_flash_open(&s_nor_flash, s_lx_flash_name, lx_nor_driver_ra8_xspi_initialize);
  if (status != LX_SUCCESS) {
    demo_print("[fslx] lx_nor_flash_open failed\r\n");
    demo_panic_halt();
  }
}

/**
 * @brief Bind the backend, format + mount the FAT volume on top of LevelX.
 *
 * @details
 * Calls ``lx_fs_backend_bind`` first so ra8_fs has a block device to
 * dispatch into, then ``ra8_fs_format`` to lay down a fresh FAT12
 * superblock (the xSPI driver exposes a 64-block LevelX window -- 384
 * usable 512-byte sectors -- so FAT12 is the band that fits, exactly
 * as the retired FileX format auto-selected), then ``ra8_fs_mount`` to
 * parse it back.
 *
 * @pre ``s_nor_flash`` was successfully opened.
 *
 * @post On success ``s_fs_mount`` is a mounted FAT volume ready for file I/O.
 *
 * @since 0.1.0
 */
static void demo_fs_format_or_panic(void)
{
  if (lx_fs_backend_bind(&s_nor_flash, &s_fs_backend) != k_ra8_ok) {
    demo_print("[fslx] lx_fs_backend_bind failed\r\n");
    demo_panic_halt();
  }
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_fat12;
  opts.label                = "FSLX";
  if (ra8_fs_format(&s_fs_backend, &opts) != k_ra8_ok) {
    demo_print("[fslx] ra8_fs_format failed\r\n");
    demo_panic_halt();
  }
  if (ra8_fs_mount(&s_fs_backend, &s_fs_mount) != k_ra8_ok) {
    demo_print("[fslx] ra8_fs_mount failed\r\n");
    demo_panic_halt();
  }
}

/**
 * @brief Create + write /levelx_test.txt with the demo test message.
 *
 * @pre ``s_fs_mount`` is mounted FAT.
 *
 * @post On success the file exists on the FAT volume containing
 *       exactly ``s_demo_test_message`` bytes.
 *
 * @since 0.1.0
 */
static void demo_write_test_file(void)
{
  uint32_t msg_bytes = (uint32_t)strlen(s_demo_test_message);
  if (ra8_fs_write_file(s_fs_mount,
                        s_demo_file_path,
                        (const uint8_t*)s_demo_test_message,
                        msg_bytes) != k_ra8_ok) {
    demo_print("[fslx] ra8_fs_write_file failed\r\n");
    return;
  }
  demo_print("[fslx] wrote /levelx_test.txt: ");
  demo_print(s_demo_test_message);
  demo_print("\r\n");
}

/**
 * @brief Open + read /levelx_test.txt back to SCI8.
 *
 * @pre ``s_fs_mount`` is mounted FAT and the file exists.
 *
 * @post The file's contents have been streamed to SCI8.
 *
 * @since 0.1.0
 */
static void demo_read_test_file(void)
{
  ra8_fs_file_t* file = nullptr;
  if (ra8_fs_open(s_fs_mount, s_demo_file_path, k_ra8_fs_mode_read, &file) != k_ra8_ok) {
    demo_print("[fslx] ra8_fs_open(read) failed\r\n");
    return;
  }
  demo_print("[fslx] readback: ");
  uint8_t  buf[k_demo_file_chunk];
  uint32_t got = 0U;
  do {
    got = 0U;
    if (ra8_fs_read(file, buf, (uint32_t)k_demo_file_chunk, &got) != k_ra8_ok) {
      break;
    }
    if (got > 0U) {
      (void)ra8_board_uart_console_write(buf, (size_t)got);
    }
  } while (got > 0U);
  demo_print("\r\n");
  (void)ra8_fs_close(file);
}

/**
 * @brief ThreadX worker entry: bring up LevelX + ra8_fs, exercise the file.
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

  demo_print("[fslx] booting xSPI flash\r\n");
  /* The LevelX NOR driver owns OCTA bus bring-up: lx_nor_flash_format ->
   * lx_nor_driver_ra8_xspi_initialize -> priv_bus_init_once routes the
   * pins, runs the 8D/1S software-reset recovery, calls ra8_xspi_init,
   * and probes RDID exactly once. Doing it here as well double-routes
   * the PFS pins (the validator rejects the second route with
   * k_ra8_err_gpio_conflict), so the driver's initialize bails before
   * wiring the sector buffer and format returns LX_NO_MEMORY. Let the
   * driver be the single owner. */
  demo_print("[fslx] formatting + opening LevelX partition\r\n");
  demo_lx_open_or_panic();

  demo_print("[fslx] formatting + mounting FAT volume on LevelX\r\n");
  demo_fs_format_or_panic();

  demo_write_test_file();
  demo_read_test_file();

  (void)ra8_fs_unmount(s_fs_mount);
  (void)lx_nor_flash_close(&s_nor_flash);
  demo_print("[fslx] done\r\n");
}

/**
 * @brief ThreadX system-define hook: build the worker + the ra8_fs lock seam.
 *
 * @details Creates the mutex first and installs it through
 * ``ra8_fs_set_lock()`` before the worker can issue a filesystem call, per
 * the seam's init-time contract (#608). LevelX's core is initialised here
 * too.
 *
 * @param[in] first_unused_memory Pointer to the start of free RAM
 *   provided by the ThreadX port; unused -- we statically allocate.
 *
 * @pre ``tx_kernel_enter()`` has been called.
 * @post One worker thread is created; LevelX and the lock seam are initialised.
 *
 * @since 0.1.0
 */
void tx_application_define(void* first_unused_memory)
{
  static CHAR s_mutex_name[] = "ra8_fs";

  (void)first_unused_memory;

  lx_nor_flash_initialize();

  (void)tx_mutex_create(&s_fs_mutex, s_mutex_name, TX_NO_INHERIT);
  const ra8_fs_lock_t lock = {
    .acquire = demo_fs_lock_acquire,
    .release = demo_fs_lock_release,
    .ctx     = &s_fs_mutex,
  };
  if (ra8_fs_set_lock(&lock) != k_ra8_ok) {
    demo_print("[fslx] ra8_fs_set_lock failed\r\n");
  }

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

/**
 * @brief Application entry. Brings up clocks + UART, then enters ThreadX.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @post On clean entry the kernel runs the worker thread once.
 *
 * @since 0.1.0
 */
void main(void)
{
  demo_setup_or_halt();
  ra8_isr_globals_enable();
  demo_print("[fslx] booting ThreadX + ra8_fs-on-LevelX...\r\n");

#ifndef RA8_OFF_TARGET
  /* Hands control over to ThreadX permanently. */
  tx_kernel_enter();
#endif

  /* Should never return. */
  demo_panic_halt();
}
