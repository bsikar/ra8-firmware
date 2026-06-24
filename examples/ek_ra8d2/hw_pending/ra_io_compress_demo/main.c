/**
 * @file examples/ek_ra8d2/hw_pending/ra_io_compress_demo/main.c
 * @brief End-to-end ra_io fabric + DEFLATE compression demo (Phase 6, #161).
 *
 * @details
 * Extends the plain ra_io_demo with the opt-in compression layer, exercising
 * the whole stack in one app with no external hardware:
 *   1. Build a RAM block device over an in-SRAM buffer (Phase 1, #156).
 *   2. Bridge it to ra_fs, format/mount a FAT12 volume, and register it in the
 *      VFS as `"ram"` (Phase 3, #158).
 *   3. Generate a compressible payload and DEFLATE it with ra_io_compress,
 *      using a caller-provided scratch buffer in SRAM (Phase 6, #161).
 *   4. Write the *compressed* blob to `ram:/STORY.RBK` through the VFS.
 *   5. Read the blob back, inflate it with ra_io_decompress, and verify the
 *      result is byte-identical to the original payload.
 *   6. Report progress on the SCI8 console through a UART stream sink.
 *
 * This mirrors the firmware's real compress-on-write / decompress-on-read path
 * (the `.rabook` RBKZ blobs) but over the unified ra_io fabric. The board_sim
 * emulator captures the SCI8 console, so the PASS line and the byte counts are
 * observable headlessly: a successful run prints
 * `ra_io_compress_demo: 4096 -> N -> 4096 bytes ram:/STORY.RBK PASS`.
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
#include "ra_io_compress.h"
#include "ra_log.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_sci.h"
#include "ra_time.h"

/** @enum demo_const_t @brief Console + volume + payload knobs (no magic numbers). */
typedef enum : uint32_t {
  k_demo_uart_chan   = 8U,      /**< SCI8 J-Link OB console.            */
  k_demo_uart_baud   = 115200U, /**< Console baud.                      */
  k_demo_disk_blocks = 512U,    /**< 256 KiB RAM-disk (FAT12).          */
  k_demo_payload     = 4096U,   /**< Source bytes (compressible).       */
  k_demo_blob_cap    = 8192U,   /**< Compressed-blob staging capacity.  */
  k_demo_pin_shift   = 8U,      /**< Port byte position in ra_port_pin_t.*/
  k_demo_seed_mul    = 31U,     /**< Test-pattern multiplier.           */
  k_demo_pattern_mod = 7U,      /**< Small alphabet -> highly compressible. */
} demo_const_t;

/** @brief SCI8 console TXD = PD02. */
static const ra_port_pin_t k_demo_txd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << (uint16_t)k_demo_pin_shift) | (uint16_t)k_ra_pin_2);
/** @brief SCI8 console RXD = PD03. */
static const ra_port_pin_t k_demo_rxd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << (uint16_t)k_demo_pin_shift) | (uint16_t)k_ra_pin_3);

/** @brief 256 KiB RAM-disk backing buffer (in SRAM .bss). */
static uint8_t s_disk[(size_t)k_demo_disk_blocks * (size_t)k_ra_io_block_size_bytes];
/** @brief Block-device handle + its RAM backend state. */
static ra_io_blockdev_t           s_bd;
static ra_io_blockdev_ram_state_t s_bstate;
/** @brief ra_fs backend bridged onto the block device. */
static ra_fs_backend_t s_be;
/** @brief UART output stream + its sink state. */
static ra_io_stream_t            s_uart;
static ra_io_stream_uart_state_t s_ust;

/** @brief Original payload, the compressed blob, and the inflated copy. */
static uint8_t s_payload[(size_t)k_demo_payload];
static uint8_t s_blob[(size_t)k_demo_blob_cap];
static uint8_t s_restored[(size_t)k_demo_payload];
/** @brief Compressor scratch (one miniz tdefl_compressor) in SRAM. */
static uint8_t s_scratch[(size_t)k_ra_io_compress_scratch_bytes];

/** @brief Module log tag. */
static const char* const s_tag = "ra_io_compress_demo";

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

/** @brief Mount a fresh FAT12 RAM volume and register it as `"ram"`. */
static ra_err_t demo_mount(ra_fs_mount_t** out_mnt)
{
  RA_CHECK_NULL_PTR(out_mnt, s_tag, "out_mnt");
  RA_RETURN_ON_ERROR(
    ra_io_blockdev_ram_init(&s_bd, &s_bstate, s_disk, (uint32_t)k_demo_disk_blocks, false),
    s_tag,
    "blockdev init");
  RA_RETURN_ON_ERROR(ra_io_blockdev_as_fs_backend(&s_bd, &s_be), s_tag, "fs bridge");
  ra_fs_format_opts_t opts = {};
  opts.type                = k_ra_fs_type_fat12;
  opts.label               = "RAIO";
  RA_RETURN_ON_ERROR(ra_fs_format(&s_be, &opts), s_tag, "format");
  RA_RETURN_ON_ERROR(ra_fs_mount(&s_be, out_mnt), s_tag, "mount");
  RA_RETURN_ON_ERROR(ra_io_vfs_mount("ram", *out_mnt), s_tag, "vfs mount");
  return k_ra_ok;
}

/** @brief Read `ram:/STORY.RBK`, inflate it, and confirm it matches the source. */
static ra_err_t demo_read_back_and_verify(uint32_t blob_len)
{
  ra_fs_file_t* f = nullptr;
  RA_RETURN_ON_ERROR(ra_io_vfs_open("ram:/STORY.RBK", k_ra_fs_mode_read, &f), s_tag, "open");
  uint32_t got_len = 0;
  RA_RETURN_ON_ERROR(ra_fs_read(f, s_blob, (uint32_t)k_demo_blob_cap, &got_len), s_tag, "read");
  RA_RETURN_ON_ERROR(ra_fs_close(f), s_tag, "close");
  if (got_len != blob_len) {
    return k_ra_err_invalid_size;
  }
  uint32_t out_len = 0;
  RA_RETURN_ON_ERROR(
    ra_io_decompress(s_blob, blob_len, s_restored, (uint32_t)k_demo_payload, &out_len),
    s_tag,
    "decompress");
  if (out_len != (uint32_t)k_demo_payload) {
    return k_ra_err_invalid_size;
  }
  if (memcmp(s_restored, s_payload, sizeof(s_payload)) != 0) {
    return k_ra_err_checksum_mismatch;
  }
  return k_ra_ok;
}

/** @brief Run the compress -> store -> inflate round-trip; report the blob size. */
static ra_err_t demo_run(uint32_t* out_blob_len)
{
  RA_CHECK_NULL_PTR(out_blob_len, s_tag, "out_blob_len");
  ra_fs_mount_t* mnt = nullptr;
  RA_RETURN_ON_ERROR(demo_mount(&mnt), s_tag, "mount stage");

  for (uint32_t i = 0; i < (uint32_t)k_demo_payload; ++i) {
    s_payload[i] = (uint8_t)((i * (uint32_t)k_demo_seed_mul) % (uint32_t)k_demo_pattern_mod);
  }
  uint32_t blob_len = 0;
  RA_RETURN_ON_ERROR(ra_io_compress(s_payload, (uint32_t)k_demo_payload, s_blob,
                                    (uint32_t)k_demo_blob_cap, s_scratch,
                                    (uint32_t)k_ra_io_compress_scratch_bytes, &blob_len),
                     s_tag,
                     "compress");
  RA_RETURN_ON_ERROR(ra_fs_write_file(mnt, "STORY.RBK", s_blob, blob_len), s_tag, "write");
  RA_RETURN_ON_ERROR(demo_read_back_and_verify(blob_len), s_tag, "verify");
  *out_blob_len = blob_len;
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
  (void)ra_io_log_attach(&s_uart);
  demo_print("ra_io_compress_demo: boot\r\n");

  uint32_t blob_len = 0;
  const ra_err_t e  = demo_run(&blob_len);
  if (e == k_ra_ok) {
    demo_print("ra_io_compress_demo: 4096 -> ");
    (void)ra_io_stream_put_u32(&s_uart, blob_len);
    demo_print(" -> 4096 bytes ram:/STORY.RBK PASS\r\n");
  } else {
    demo_print("ra_io_compress_demo: FAIL\r\n");
  }
  (void)ra_sci_flush((uint8_t)k_demo_uart_chan);
  while (true) {
  }
}
