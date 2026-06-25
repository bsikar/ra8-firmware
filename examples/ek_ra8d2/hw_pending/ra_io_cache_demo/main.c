/**
 * @file examples/ek_ra8d2/hw_pending/ra_io_cache_demo/main.c
 * @brief ra_io caching block device demo (Phase 5, #160).
 *
 * @details
 * Exercises the caching block device -- an LRU sector cache decorator that wraps
 * any backend so repeated reads of the same blocks (filesystem metadata, a
 * re-read page) skip the slow medium. The cache sits between the filesystem/VFS
 * and the media:
 *   1. Build a RAM block device as the slow backend (Phase 1, #156).
 *   2. Wrap it with `ra_io_blockdev_cache_init` over a fixed set of cached
 *      sectors (Phase 5, #160).
 *   3. Bridge the *cached* device to ra_fs, format/mount a FAT12 volume, and
 *      register it in the VFS as `"ram"` (Phase 3, #158) -- so every FAT access
 *      now flows through the cache.
 *   4. Write a file, then read it back repeatedly through `"ram:/HELLO.TXT"`.
 *      The first pass fills the cache; the re-reads touch the same metadata and
 *      data sectors and are served as cache hits.
 *   5. Read the hit/miss counters with `ra_io_blockdev_cache_stats` and require
 *      a non-zero hit count, proving the cache served repeated reads.
 *   6. Report progress on the SCI8 console through a ra_io UART stream sink.
 *
 * The board_sim emulator captures the SCI8 console, so the PASS line and the
 * hit/miss counts are observable headlessly: a successful run prints
 * `ra_io_cache_demo: re-read x8 hits=H misses=M ram:/HELLO.TXT PASS`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra_cgc.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_fs.h"
#include "ra_io.h"
#include "ra_io_blockdev_cache.h"
#include "ra_log.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_sci.h"
#include "ra_time.h"

/** @enum demo_const_t @brief Console + volume + cache knobs (no magic numbers). */
typedef enum : uint32_t {
  k_demo_uart_chan   = 8U,      /**< SCI8 J-Link OB console.            */
  k_demo_uart_baud   = 115200U, /**< Console baud.                      */
  k_demo_disk_blocks = 512U,    /**< 256 KiB RAM-disk (FAT12).          */
  k_demo_cache_slots = 32U,     /**< Cached 512-byte sectors (16 KiB).  */
  k_demo_payload     = 128U,    /**< Bytes written + read back.         */
  k_demo_reads       = 8U,      /**< Re-read passes over the same file. */
  k_demo_pin_shift   = 8U,      /**< Port byte position in ra_port_pin_t.*/
  k_demo_seed_mul    = 7U,      /**< Test-pattern multiplier.           */
} demo_const_t;

/** @brief SCI8 console TXD = PD02. */
static const ra_port_pin_t k_demo_txd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << (uint16_t)k_demo_pin_shift) | (uint16_t)k_ra_pin_2);
/** @brief SCI8 console RXD = PD03. */
static const ra_port_pin_t k_demo_rxd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << (uint16_t)k_demo_pin_shift) | (uint16_t)k_ra_pin_3);

/** @brief 256 KiB RAM-disk backing buffer for the slow backend (in SRAM .bss). */
static uint8_t s_disk[(size_t)k_demo_disk_blocks * (size_t)k_ra_io_block_size_bytes];
/** @brief Slow backend block-device handle + its RAM backend state. */
static ra_io_blockdev_t           s_under;
static ra_io_blockdev_ram_state_t s_ustate;
/** @brief Caching decorator over the backend + its caller-owned cache storage. */
static ra_io_blockdev_t             s_cached;
static ra_io_blockdev_cache_state_t s_cstate;
static uint8_t s_cache_data[(size_t)k_demo_cache_slots * (size_t)k_ra_io_block_size_bytes];
static ra_io_blockdev_cache_slot_t s_cache_slots[(size_t)k_demo_cache_slots];
/** @brief ra_fs backend bridged onto the *cached* block device. */
static ra_fs_backend_t s_be;
/** @brief UART output stream + its sink state. */
static ra_io_stream_t            s_uart;
static ra_io_stream_uart_state_t s_ust;

/** @brief Module log tag. */
static const char* const s_tag = "ra_io_cache_demo";

/** @brief Print a NUL-terminated string on the UART stream. */
static void demo_print(const char* msg)
{
  (void)ra_io_stream_puts(&s_uart, msg);
}

/** @brief Bring up CGC + SysTick + the SCI8 console; halt on any failure. */
static void demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;
  if ((ra_cgc_init() != k_ra_ok) ||
      (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) ||
      (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz) != k_ra_ok) ||
      (ra_time_init(cpuclk0_hz) != k_ra_ok) ||
      (ra_pfs_route_peripheral(k_demo_txd, k_ra_psel_sci_async, "demo.txd") != k_ra_ok) ||
      (ra_pfs_route_peripheral(k_demo_rxd, k_ra_psel_sci_async, "demo.rxd") != k_ra_ok)) {
    while (true) {
    }
  }
  const ra_sci_cfg_t sci_cfg = {.baud      = (uint32_t)k_demo_uart_baud,
                                .data_bits = k_ra_sci_data_8,
                                .parity    = k_ra_sci_parity_none,
                                .stop_bits = k_ra_sci_stop_1,
                                .pclk_hz   = pclka_hz};
  if (ra_sci_init((uint8_t)k_demo_uart_chan, &sci_cfg) != k_ra_ok) {
    while (true) {
    }
  }
}

/** @brief Bridge the cached device to ra_fs, format FAT12, and register the VFS. */
static ra_err_t demo_mount(ra_fs_mount_t** out_mnt)
{
  RA_CHECK_NULL_PTR(out_mnt, s_tag, "out_mnt");
  RA_RETURN_ON_ERROR(
    ra_io_blockdev_ram_init(&s_under, &s_ustate, s_disk, (uint32_t)k_demo_disk_blocks, false),
    s_tag,
    "blockdev init");
  RA_RETURN_ON_ERROR(ra_io_blockdev_cache_init(&s_cached,
                                               &s_cstate,
                                               &s_under,
                                               s_cache_data,
                                               s_cache_slots,
                                               (uint32_t)k_demo_cache_slots),
                     s_tag,
                     "cache init");
  RA_RETURN_ON_ERROR(ra_io_blockdev_as_fs_backend(&s_cached, &s_be), s_tag, "fs bridge");
  ra_fs_format_opts_t opts = {};
  opts.type                = k_ra_fs_type_fat12;
  opts.label               = "RAIO";
  RA_RETURN_ON_ERROR(ra_fs_format(&s_be, &opts), s_tag, "format");
  RA_RETURN_ON_ERROR(ra_fs_mount(&s_be, out_mnt), s_tag, "mount");
  RA_RETURN_ON_ERROR(ra_io_vfs_mount("ram", *out_mnt), s_tag, "vfs mount");
  return k_ra_ok;
}

/** @brief Read `ram:/HELLO.TXT` once and byte-compare it against `expect`. */
static ra_err_t demo_read_once(const uint8_t* expect)
{
  RA_CHECK_NULL_PTR(expect, s_tag, "expect");
  ra_fs_file_t* f = nullptr;
  RA_RETURN_ON_ERROR(ra_io_vfs_open("ram:/HELLO.TXT", k_ra_fs_mode_read, &f), s_tag, "open");
  uint8_t  got[(size_t)k_demo_payload] = {};
  uint32_t got_len                     = 0;
  RA_RETURN_ON_ERROR(ra_fs_read(f, got, (uint32_t)k_demo_payload, &got_len), s_tag, "read");
  RA_RETURN_ON_ERROR(ra_fs_close(f), s_tag, "close");
  if (got_len != (uint32_t)k_demo_payload) {
    return k_ra_err_invalid_size;
  }
  if (memcmp(got, expect, (size_t)k_demo_payload) != 0) {
    return k_ra_err_checksum_mismatch;
  }
  return k_ra_ok;
}

/** @brief Write the file once, re-read it `k_demo_reads` times through the cache. */
static ra_err_t demo_run(uint32_t* out_hits, uint32_t* out_misses)
{
  RA_CHECK_NULL_PTR(out_hits, s_tag, "out_hits");
  RA_CHECK_NULL_PTR(out_misses, s_tag, "out_misses");
  ra_fs_mount_t* mnt = nullptr;
  RA_RETURN_ON_ERROR(demo_mount(&mnt), s_tag, "mount stage");

  uint8_t data[(size_t)k_demo_payload];
  for (uint32_t i = 0; i < (uint32_t)k_demo_payload; ++i) {
    data[i] = (uint8_t)(i * (uint32_t)k_demo_seed_mul + 1u);
  }
  RA_RETURN_ON_ERROR(ra_fs_write_file(mnt, "HELLO.TXT", data, (uint32_t)k_demo_payload),
                     s_tag,
                     "write");

  for (uint32_t pass = 0; pass < (uint32_t)k_demo_reads; ++pass) {
    RA_RETURN_ON_ERROR(demo_read_once(data), s_tag, "reread");
  }

  RA_RETURN_ON_ERROR(ra_io_blockdev_cache_stats(&s_cstate, out_hits, out_misses), s_tag, "stats");
  if (*out_hits == 0u) {
    return k_ra_err_checksum_mismatch;
  }
  return k_ra_ok;
}

/**
 * @brief Firmware entry point.
 *
 * @pre SystemInit set VTOR / FPU / priority grouping.
 * @return Never returns.
 */
int main(void)
{
  ra_log_init();
  demo_setup_or_halt();
  (void)ra_io_stream_uart_init(&s_uart, &s_ust, (uint8_t)k_demo_uart_chan);
  (void)ra_io_log_attach(&s_uart); /* route ra_log into the UART stream too */
  demo_print("ra_io_cache_demo: boot\r\n");

  uint32_t       hits   = 0;
  uint32_t       misses = 0;
  const ra_err_t e      = demo_run(&hits, &misses);
  if (e == k_ra_ok) {
    demo_print("ra_io_cache_demo: re-read x8 hits=");
    (void)ra_io_stream_put_u32(&s_uart, hits);
    demo_print(" misses=");
    (void)ra_io_stream_put_u32(&s_uart, misses);
    demo_print(" ram:/HELLO.TXT PASS\r\n");
  } else {
    demo_print("ra_io_cache_demo: FAIL\r\n");
  }
  (void)ra_sci_flush((uint8_t)k_demo_uart_chan);
  while (true) {
  }
}
