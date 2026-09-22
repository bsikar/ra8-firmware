/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/ra8_io_cache_demo/src/main.c
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
 * The ra8_emulator captures the SCI8 console, so the PASS line and the
 * hit/miss counts are observable headlessly: a successful run prints
 * `ra8_io_cache_demo: re-read x8 hits=H misses=M ram:/HELLO.TXT PASS`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_board_ek_ra8d2_console_stream.h"
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_io.h"
#include "ra8_io_blockdev_cache.h"
#include "ra8_log.h"
#include "ra8_time.h"

/** @enum demo_const_t @brief Console + volume + cache knobs (no magic numbers). */
typedef enum : uint32_t {
  k_demo_uart_baud   = 115200U, /**< Console baud.                      */
  k_demo_disk_blocks = 512U,    /**< 256 KiB RAM-disk (FAT12).          */
  k_demo_cache_slots = 32U,     /**< Cached 512-byte sectors (16 KiB).  */
  k_demo_payload     = 128U,    /**< Bytes written + read back.         */
  k_demo_reads       = 8U,      /**< Re-read passes over the same file. */
  k_demo_seed_mul    = 7U,      /**< Test-pattern multiplier.           */
} demo_const_t;

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
/** @brief Console output stream; the board owns the sink behind it. */
static ra8_io_stream_t s_uart;

/** @brief Module log tag. */
static const char* const s_tag = "ra8_io_cache_demo";

/**
 * @brief Print a NUL-terminated string on the UART stream.
 *
 * @details Delegates bounded string emission to the initialized stream and
 * intentionally ignores diagnostic-output errors in this terminal demo.
 *
 * @param[in] msg NUL-terminated message to emit.
 * @pre @p msg is non-NULL and readable through its terminator.
 * @pre ``s_uart`` has been initialized with its caller-owned UART state.
 * @post The stream has accepted the message or reported an ignored sink error.
 * @post Cache, filesystem, and block-device state remain unchanged.
 * @note This single-threaded diagnostic helper performs no retry.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_demo_print(const char* msg)
{
  (void)ra8_io_stream_puts(&s_uart, msg);
}

/**
 * @brief Bring up CGC, SysTick, and the board console; halt on failure.
 *
 * @details Resolves CPUCLK0, initializes the time base, then hands the console
 * over to the BSP -- which owns the channel, the PD02 / PD03 routing and the
 * live-PCLKA bit-rate solve -- and binds it as an ra8_io stream.
 *
 * @pre Reset startup has initialized data and BSS storage.
 * @pre Peripheral register mappings for clocks, pins, and the console are
 *      accessible.
 * @post On return, the board console is configured for the requested
 *       diagnostic baud and bound into ::s_uart.
 * @post Any required setup failure parks the application before returning.
 * @note This helper is intended for the single-threaded startup path only.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if ((ra8_cgc_init() != k_ra8_ok) ||
      (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) ||
      (ra8_time_init(cpuclk0_hz) != k_ra8_ok) ||
      (ra8_board_uart_console_init((uint32_t)k_demo_uart_baud) != k_ra8_ok) ||
      (ra8_board_console_stream(&s_uart) != k_ra8_ok)) {
    while (true) {
    }
  }
}

/**
 * @brief Bridge the cached device to ra8_fs, format FAT12, and register the VFS.
 *
 * @details Constructs the RAM backend and cache decorator in caller-owned
 * storage, formats a FAT12 volume, mounts it, and registers the ``ram`` prefix.
 *
 * @param[out] out_mnt Receives the mounted FAT filesystem on success.
 * @return Error from the first block-device, format, mount, or VFS operation.
 * @retval k_ra8_ok The cached FAT12 volume was mounted and registered.
 * @pre @p out_mnt addresses writable pointer storage.
 * @pre The static RAM disk and cache buffers retain their full declared capacity.
 * @post On success, @p out_mnt identifies the VFS-registered mount.
 * @post On failure, later construction stages are not attempted.
 * @note The backing storage and cache lifetime equal the firmware lifetime.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_demo_mount(ra8_fs_mount_t** out_mnt)
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

/**
 * @brief Read ``ram:/HELLO.TXT`` once and compare it against @p expect.
 *
 * @details Opens the file through VFS, reads exactly the configured payload,
 * closes the handle, then validates both length and contents.
 *
 * @param[in] expect Expected payload bytes for the comparison.
 * @return Filesystem error or the explicit length/content verdict.
 * @retval k_ra8_ok The complete payload matched @p expect.
 * @retval k_ra8_err_invalid_size The file length differed from the payload size.
 * @retval k_ra8_err_checksum_mismatch At least one byte differed.
 * @pre @p expect addresses at least ``k_demo_payload`` readable bytes.
 * @pre The ``ram`` VFS mount and ``HELLO.TXT`` file already exist.
 * @post The read handle is closed after a successful read operation.
 * @post Cache statistics reflect the block accesses performed by this read.
 * @note The comparison buffer is bounded on the stack by the payload constant.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_demo_read_once(const uint8_t* expect)
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

/**
 * @brief Write the file once and re-read it repeatedly through the cache.
 *
 * @details Mounts the cached volume, creates a deterministic payload, writes it,
 * verifies every configured read pass, and returns cache hit/miss counters.
 *
 * @param[out] out_hits Receives the cache-hit count.
 * @param[out] out_misses Receives the cache-miss count.
 * @return Error from the first mount, write, read, or stats operation.
 * @retval k_ra8_ok Every read matched and at least one cache hit was observed.
 * @retval k_ra8_err_checksum_mismatch Data differed or no hit was recorded.
 * @pre Both output pointers address writable 32-bit storage.
 * @pre The VFS mount table has room for the ``ram`` prefix.
 * @post On success, both output counters contain the final cache statistics.
 * @post The verified file remains present on the mounted FAT12 volume.
 * @note The deterministic payload makes repeated cache reads byte-comparable.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_demo_run(uint32_t* out_hits, uint32_t* out_misses)
{
  RA8_CHECK_NULL_PTR(out_hits, s_tag, "out_hits");
  RA8_CHECK_NULL_PTR(out_misses, s_tag, "out_misses");
  ra8_fs_mount_t* mnt = nullptr;
  RA8_RETURN_ON_ERROR(internal_demo_mount(&mnt), s_tag, "mount stage");

  uint8_t data[(size_t)k_demo_payload];
  for (uint32_t i = 0; i < (uint32_t)k_demo_payload; ++i) {
    data[i] = (uint8_t)(i * (uint32_t)k_demo_seed_mul + 1U);
  }
  RA8_RETURN_ON_ERROR(ra8_fs_write_file(mnt, "HELLO.TXT", data, (uint32_t)k_demo_payload),
                      s_tag,
                      "write");

  for (uint32_t pass = 0; pass < (uint32_t)k_demo_reads; ++pass) {
    RA8_RETURN_ON_ERROR(internal_demo_read_once(data), s_tag, "reread");
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
 */
void main(void)
{
  ra8_log_init();
  internal_demo_setup_or_halt();
  (void)ra8_io_log_attach(&s_uart); /* route ra8_log into the console stream too */
  internal_demo_print("ra8_io_cache_demo: boot\r\n");

  uint32_t        hits   = 0;
  uint32_t        misses = 0;
  const ra8_err_t e      = internal_demo_run(&hits, &misses);
  if (e == k_ra8_ok) {
    internal_demo_print("ra8_io_cache_demo: re-read x8 hits=");
    (void)ra8_io_stream_put_u32(&s_uart, hits);
    internal_demo_print(" misses=");
    (void)ra8_io_stream_put_u32(&s_uart, misses);
    internal_demo_print(" ram:/HELLO.TXT PASS\r\n");
  } else {
    internal_demo_print("ra8_io_cache_demo: FAIL\r\n");
  }
  (void)ra8_board_uart_console_flush();
  while (true) {
  }
}
