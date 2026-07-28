/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/ra8_io_cache_demo/main.c
 * @brief ra8_io caching block device demo (Phase 5, #160).
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Exercises the caching block device -- an LRU sector cache decorator that wraps
 * any backend so repeated reads of the same blocks (filesystem metadata, a
 * re-read page) skip the slow medium. The cache sits between the filesystem/VFS
 * and the media:
 *   1. Build a RAM block device as the slow backend (Phase 1, #156).
 *   2. Wrap it with `ra8_io_blockdev_cache_init` over a fixed set of cached
 *      sectors (Phase 5, #160).
 *   3. Bridge the *cached* device to ra8_fs, format/mount a FAT12 volume, and
 *      register it in the VFS as `"ram"` (Phase 3, #158) -- so every FAT access
 *      now flows through the cache.
 *   4. Write a file, then read it back repeatedly through `"ram:/HELLO.TXT"`.
 *      The first pass fills the cache; the re-reads touch the same metadata and
 *      data sectors and are served as cache hits.
 *   5. Read the hit/miss counters with `ra8_io_blockdev_cache_stats` and require
 *      a non-zero hit count, proving the cache served repeated reads.
 *   6. Report progress on the SCI8 console through a ra8_io UART stream sink.
 *
 * The board_sim emulator captures the SCI8 console, so the PASS line and the
 * hit/miss counts are observable headlessly: a successful run prints
 * `ra8_io_cache_demo: re-read x8 hits=H misses=M ram:/HELLO.TXT PASS`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_cgc.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_io.h"
#include "ra8_io_blockdev_cache.h"
#include "ra8_log.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "ra8_sci.h"
#include "ra8_time.h"

/** @enum demo_const_t @brief Console + volume + cache knobs (no magic numbers). */
typedef enum : uint32_t {
  k_demo_uart_chan   = 8U,      /**< SCI8 J-Link OB console.               */
  k_demo_uart_baud   = 115200U, /**< Console baud.                         */
  k_demo_disk_blocks = 512U,    /**< 256 KiB RAM-disk (FAT12).             */
  k_demo_cache_slots = 32U,     /**< Cached 512-byte sectors (16 KiB).     */
  k_demo_payload     = 128U,    /**< Bytes written + read back.            */
  k_demo_reads       = 8U,      /**< Re-read passes over the same file.    */
  k_demo_pin_shift   = 8U,      /**< Port byte position in ra8_port_pin_t. */
  k_demo_seed_mul    = 7U,      /**< Test-pattern multiplier.              */
} demo_const_t;

/** @brief SCI8 console TXD = PD02. */
static const ra8_port_pin_t k_demo_txd =
  (ra8_port_pin_t)(((uint16_t)k_ra8_port_13 << (uint16_t)k_demo_pin_shift) | (uint16_t)k_ra8_pin_2);
/** @brief SCI8 console RXD = PD03. */
static const ra8_port_pin_t k_demo_rxd =
  (ra8_port_pin_t)(((uint16_t)k_ra8_port_13 << (uint16_t)k_demo_pin_shift) | (uint16_t)k_ra8_pin_3);

/** @brief 256 KiB RAM-disk backing buffer for the slow backend (in SRAM .bss). */
static uint8_t s_disk[(size_t)k_demo_disk_blocks * (size_t)k_ra8_io_block_size_bytes];
/** @brief Slow backend block-device handle + its RAM backend state. */
static ra8_io_blockdev_t           s_under;
static ra8_io_blockdev_ram_state_t s_ustate;
/** @brief Caching decorator over the backend + its caller-owned cache storage. */
static ra8_io_blockdev_t             s_cached;
static ra8_io_blockdev_cache_state_t s_cstate;
static uint8_t s_cache_data[(size_t)k_demo_cache_slots * (size_t)k_ra8_io_block_size_bytes];
static ra8_io_blockdev_cache_slot_t s_cache_slots[(size_t)k_demo_cache_slots];
/** @brief ra8_fs backend bridged onto the *cached* block device. */
static ra8_fs_backend_t s_be;
/** @brief UART output stream + its sink state. */
static ra8_io_stream_t            s_uart;
static ra8_io_stream_uart_state_t s_ust;

/** @brief Module log tag. */
static const char* const s_tag = "ra8_io_cache_demo";

/** @brief Print a NUL-terminated string on the UART stream. */
static void demo_print(const char* msg)
{
  (void)ra8_io_stream_puts(&s_uart, msg);
}

/** @brief Bring up CGC + SysTick + the SCI8 console; halt on any failure. */
static void demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;
  if ((ra8_cgc_init() != k_ra8_ok) ||
      (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) ||
      (ra8_cgc_get_clock_hz(k_ra8_clock_id_pclka, &pclka_hz) != k_ra8_ok) ||
      (ra8_time_init(cpuclk0_hz) != k_ra8_ok) ||
      (ra8_pfs_route_peripheral(k_demo_txd, k_ra8_psel_sci_async, "demo.txd") != k_ra8_ok) ||
      (ra8_pfs_route_peripheral(k_demo_rxd, k_ra8_psel_sci_async, "demo.rxd") != k_ra8_ok)) {
    while (true) {
    }
  }
  const ra8_sci_cfg_t sci_cfg = {.baud      = (uint32_t)k_demo_uart_baud,
                                 .data_bits = k_ra8_sci_data_8,
                                 .parity    = k_ra8_sci_parity_none,
                                 .stop_bits = k_ra8_sci_stop_1,
                                 .pclk_hz   = pclka_hz};
  if (ra8_sci_init((uint8_t)k_demo_uart_chan, &sci_cfg) != k_ra8_ok) {
    while (true) {
    }
  }
}

/** @brief Bridge the cached device to ra8_fs, format FAT12, and register the VFS. */
static ra8_err_t demo_mount(ra8_fs_mount_t** out_mnt)
{
  RA8_CHECK_NULL_PTR(out_mnt, s_tag, "out_mnt");
  RA8_RETURN_ON_ERROR(
    ra8_io_blockdev_ram_init(&s_under, &s_ustate, s_disk, (uint32_t)k_demo_disk_blocks, false),
    s_tag,
    "blockdev init");
  RA8_RETURN_ON_ERROR(ra8_io_blockdev_cache_init(&s_cached,
                                                 &s_cstate,
                                                 &s_under,
                                                 s_cache_data,
                                                 s_cache_slots,
                                                 (uint32_t)k_demo_cache_slots),
                      s_tag,
                      "cache init");
  RA8_RETURN_ON_ERROR(ra8_io_blockdev_as_fs_backend(&s_cached, &s_be), s_tag, "fs bridge");
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_fat12;
  opts.label                = "RAIO";
  RA8_RETURN_ON_ERROR(ra8_fs_format(&s_be, &opts), s_tag, "format");
  RA8_RETURN_ON_ERROR(ra8_fs_mount(&s_be, out_mnt), s_tag, "mount");
  RA8_RETURN_ON_ERROR(ra8_io_vfs_mount("ram", *out_mnt), s_tag, "vfs mount");
  return k_ra8_ok;
}

/** @brief Read `ram:/HELLO.TXT` once and byte-compare it against `expect`. */
static ra8_err_t demo_read_once(const uint8_t* expect)
{
  RA8_CHECK_NULL_PTR(expect, s_tag, "expect");
  ra8_fs_file_t* f = nullptr;
  RA8_RETURN_ON_ERROR(ra8_io_vfs_open("ram:/HELLO.TXT", k_ra8_fs_mode_read, &f), s_tag, "open");
  uint8_t  got[(size_t)k_demo_payload] = {};
  uint32_t got_len                     = 0;
  RA8_RETURN_ON_ERROR(ra8_fs_read(f, got, (uint32_t)k_demo_payload, &got_len), s_tag, "read");
  RA8_RETURN_ON_ERROR(ra8_fs_close(f), s_tag, "close");
  if (got_len != (uint32_t)k_demo_payload) {
    return k_ra8_err_invalid_size;
  }
  if (memcmp(got, expect, (size_t)k_demo_payload) != 0) {
    return k_ra8_err_checksum_mismatch;
  }
  return k_ra8_ok;
}

/** @brief Write the file once, re-read it `k_demo_reads` times through the cache. */
static ra8_err_t demo_run(uint32_t* out_hits, uint32_t* out_misses)
{
  RA8_CHECK_NULL_PTR(out_hits, s_tag, "out_hits");
  RA8_CHECK_NULL_PTR(out_misses, s_tag, "out_misses");
  ra8_fs_mount_t* mnt = nullptr;
  RA8_RETURN_ON_ERROR(demo_mount(&mnt), s_tag, "mount stage");

  uint8_t data[(size_t)k_demo_payload];
  for (uint32_t i = 0; i < (uint32_t)k_demo_payload; ++i) {
    data[i] = (uint8_t)(i * (uint32_t)k_demo_seed_mul + 1U);
  }
  RA8_RETURN_ON_ERROR(ra8_fs_write_file(mnt, "HELLO.TXT", data, (uint32_t)k_demo_payload),
                      s_tag,
                      "write");

  for (uint32_t pass = 0; pass < (uint32_t)k_demo_reads; ++pass) {
    RA8_RETURN_ON_ERROR(demo_read_once(data), s_tag, "reread");
  }

  RA8_RETURN_ON_ERROR(ra8_io_blockdev_cache_stats(&s_cstate, out_hits, out_misses), s_tag, "stats");
  if (*out_hits == 0U) {
    return k_ra8_err_checksum_mismatch;
  }
  return k_ra8_ok;
}

/**
 * @brief Firmware entry point.
 *
 * @pre SystemInit set VTOR / FPU / priority grouping.
 * @return Never returns.
 */
int main(void)
{
  ra8_log_init();
  demo_setup_or_halt();
  (void)ra8_io_stream_uart_init(&s_uart, &s_ust, (uint8_t)k_demo_uart_chan);
  (void)ra8_io_log_attach(&s_uart); /* route ra8_log into the UART stream too */
  demo_print("ra8_io_cache_demo: boot\r\n");

  uint32_t        hits   = 0;
  uint32_t        misses = 0;
  const ra8_err_t e      = demo_run(&hits, &misses);
  if (e == k_ra8_ok) {
    demo_print("ra8_io_cache_demo: re-read x8 hits=");
    (void)ra8_io_stream_put_u32(&s_uart, hits);
    demo_print(" misses=");
    (void)ra8_io_stream_put_u32(&s_uart, misses);
    demo_print(" ram:/HELLO.TXT PASS\r\n");
  } else {
    demo_print("ra8_io_cache_demo: FAIL\r\n");
  }
  (void)ra8_sci_flush((uint8_t)k_demo_uart_chan);
  while (true) {
  }
}
