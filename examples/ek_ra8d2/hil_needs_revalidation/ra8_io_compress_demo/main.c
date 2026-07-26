/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/ra8_io_compress_demo/main.c
 * @brief Transparent compress-on-write / decompress-on-read over the VFS (#161).
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Demonstrates that DEFLATE compression is a TRANSPARENT property of the fabric:
 * the app hands a plain payload to ::ra8_io_vfs_write_compressed and reads it
 * back with ::ra8_io_vfs_read_compressed -- it NEVER touches the codec
 * (`ra8_io_compress` / `ra8_io_decompress`) or `ra8_fs_write_file` directly. The
 * bytes on the backing store are the compressed blob; the API hides the codec.
 *
 * Because the compression seam sits ABOVE the VFS, it composes over WHATEVER
 * backend is mounted. To prove that, this demo mounts TWO backends and runs the
 * SAME compressed round-trip through the fabric on each:
 *   1. A RAM block device over an in-SRAM buffer, registered as `"ram"`.
 *   2. The external 64 MiB SDRAM as a volatile ramdisk, registered as `"dr"`.
 * The only per-backend difference is the block-device bind line and the VFS
 * prefix -- the write/read/verify code is identical and backend-agnostic.
 *
 * For each backend the demo:
 *   a. Formats + mounts a FAT12 volume and registers it in the VFS.
 *   b. ::ra8_io_vfs_write_compressed -- compresses + stores `STORY.RBK`.
 *   c. ::ra8_io_vfs_read_compressed -- reads + inflates the blob.
 *   d. memcmp against the original payload.
 *
 * This mirrors the firmware's real `.rabook` RBKC compress-on-write path. The
 * board_sim emulator captures the SCI8 console, so the PASS line is observable
 * headlessly: a successful run ends with
 * `ra8_io_compress_demo: ram+dr 4096 -> N bytes -> 4096 round-trip PASS`.
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
#include "ra8_io_vfs_compress.h"
#include "ra8_log.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "ra8_sci.h"
#include "ra8_time.h"

/** @enum demo_const_t @brief Console + volume + payload knobs (no magic numbers). */
typedef enum : uint32_t {
  k_demo_uart_chan   = 8U,      /**< SCI8 J-Link OB console.                */
  k_demo_uart_baud   = 115200U, /**< Console baud.                          */
  k_demo_disk_blocks = 512U,    /**< 256 KiB per volume (FAT12).            */
  k_demo_payload     = 4096U,   /**< Source bytes (compressible).           */
  k_demo_blob_cap    = 8192U,   /**< Compressed-blob staging capacity.      */
  k_demo_pin_shift   = 8U,      /**< Port byte position in ra8_port_pin_t.  */
  k_demo_seed_mul    = 31U,     /**< Test-pattern multiplier.               */
  k_demo_pattern_mod = 7U,      /**< Small alphabet -> highly compressible. */
} demo_const_t;

/** @brief SCI8 console TXD = PD02. */
static const ra8_port_pin_t k_demo_txd =
  (ra8_port_pin_t)(((uint16_t)k_ra8_port_13 << (uint16_t)k_demo_pin_shift) | (uint16_t)k_ra8_pin_2);
/** @brief SCI8 console RXD = PD03. */
static const ra8_port_pin_t k_demo_rxd =
  (ra8_port_pin_t)(((uint16_t)k_ra8_port_13 << (uint16_t)k_demo_pin_shift) | (uint16_t)k_ra8_pin_3);

/** @brief 256 KiB RAM-disk backing buffer (in SRAM .bss) for the `"ram"` mount. */
static uint8_t s_disk[(size_t)k_demo_disk_blocks * (size_t)k_ra8_io_block_size_bytes];

/** @brief RAM block-device handle + its backend state. */
static ra8_io_blockdev_t           s_bd_ram;
static ra8_io_blockdev_ram_state_t s_bstate_ram;
/** @brief SDRAM block-device handle + its (reused RAM-backend) state. */
static ra8_io_blockdev_t           s_bd_dr;
static ra8_io_blockdev_ram_state_t s_bstate_dr;
/** @brief ra8_fs backends bridged onto each block device. */
static ra8_fs_backend_t s_be_ram;
static ra8_fs_backend_t s_be_dr;

/** @brief UART output stream + its sink state. */
static ra8_io_stream_t            s_uart;
static ra8_io_stream_uart_state_t s_ust;

/** @brief Original payload, compressed-blob staging, and the inflated copy. */
static uint8_t s_payload[(size_t)k_demo_payload];
static uint8_t s_blob[(size_t)k_demo_blob_cap];
static uint8_t s_restored[(size_t)k_demo_payload];
/** @brief Compressor scratch (one miniz tdefl_compressor) in SRAM. */
static uint8_t s_scratch[(size_t)k_ra8_io_compress_scratch_bytes];

/** @brief Module log tag. */
static const char* const s_tag = "ra8_io_compress_demo";

/**
 * @brief Print a NUL-terminated string on the demo's UART stream.
 *
 * @details Thin wrapper over ::ra8_io_stream_puts bound to ::s_uart so call sites
 *          do not repeat the sink handle.
 *
 * @param[in] msg NUL-terminated ASCII string (CR/LF supplied by the caller).
 *
 * @return None.
 *
 * @pre ::s_uart was initialised by ::ra8_io_stream_uart_init.
 * @pre @p msg is non-NULL and NUL-terminated.
 * @post The bytes of @p msg are queued on the UART stream.
 * @post No other state changes.
 *
 * @note Blocking polled TX; not interrupt-safe.
 * @since 0.1.0
 */
static void demo_print(const char* msg)
{
  (void)ra8_io_stream_puts(&s_uart, msg);
}

/**
 * @brief Bring up CGC + SysTick + the SCI8 console; halt forever on any failure.
 *
 * @details Initialises clocks, reads CPU/PCLKA rates, starts the millisecond
 *          time base, routes the SCI8 console pins, and opens the SCI8 UART. Any
 *          failure spins forever so a debugger can inspect the halt.
 *
 * @return None.
 *
 * @pre SystemInit configured VTOR / FPU / priority grouping.
 * @pre Runs single-threaded during early boot.
 * @post On success SCI8 is open at ::k_demo_uart_baud and the time base runs.
 * @post On any failure the function never returns (infinite halt loop).
 *
 * @note Not thread-safe; boot-context only.
 * @since 0.1.0
 */
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

/**
 * @brief Format + mount a fresh FAT12 volume and register it in the VFS.
 *
 * @details Bridges an already-bound block device to ra8_fs, formats a FAT12
 *          volume, mounts it, and registers the mount under @p name in the VFS.
 *          The block device and backend storage are caller-owned.
 *
 * @param[in]  bd   Bound block device (RAM or SDRAM).
 * @param[out] be   Caller-owned ra8_fs backend to populate.
 * @param[in]  name VFS mount name (e.g. `"ram"` or `"dr"`).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Volume formatted, mounted, and registered.
 * @retval k_ra8_err_null_ptr @p bd, @p be, or @p name was NULL.
 * @retval (other)           The first failing fabric step's code.
 *
 * @pre @p bd was bound by a block-device init call.
 * @pre @p be and @p name out-live the registration.
 * @post On success `"<name>:/..."` paths resolve to the new volume.
 * @post On any non-ok return no volume is registered under @p name.
 *
 * @note Not thread-safe; single-caller boot context.
 * @since 0.1.0
 */
static ra8_err_t demo_mount(ra8_io_blockdev_t* bd, ra8_fs_backend_t* be, const char* name)
{
  RA8_CHECK_NULL_PTR(bd, s_tag, "bd");
  RA8_CHECK_NULL_PTR(be, s_tag, "be");
  RA8_CHECK_NULL_PTR(name, s_tag, "name");
  RA8_RETURN_ON_ERROR(ra8_io_blockdev_as_fs_backend(bd, be), s_tag, "fs bridge");
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_fat12;
  opts.label                = "RAIO";
  RA8_RETURN_ON_ERROR(ra8_fs_format(be, &opts), s_tag, "format");
  ra8_fs_mount_t* mnt = nullptr;
  RA8_RETURN_ON_ERROR(ra8_fs_mount(be, &mnt), s_tag, "mount");
  RA8_RETURN_ON_ERROR(ra8_io_vfs_mount(name, mnt), s_tag, "vfs mount");
  return k_ra8_ok;
}

/**
 * @brief Run the transparent compressed round-trip on one mounted volume.
 *
 * @details Calls ONLY the fabric's transparent compression API: it writes the
 *          shared payload to `"<prefix>STORY.RBK"` with
 *          ::ra8_io_vfs_write_compressed (compress-on-write), reads it back with
 *          ::ra8_io_vfs_read_compressed (decompress-on-read), and byte-compares
 *          against the original. The codec is never invoked directly. This body
 *          is backend-agnostic: the only thing that varies between volumes is
 *          @p prefix.
 *
 * @param[in]  prefix       VFS path prefix incl. the trailing slash (`"ram:/"`).
 * @param[out] out_blob_len Compressed blob length stored on success.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                    The payload round-tripped intact.
 * @retval k_ra8_err_null_ptr          @p prefix or @p out_blob_len was NULL.
 * @retval k_ra8_err_invalid_size      The inflated length differed.
 * @retval k_ra8_err_checksum_mismatch The inflated bytes differed.
 * @retval (other)                    The first failing fabric step's code.
 *
 * @pre The volume named by @p prefix is mounted in the VFS.
 * @pre ::s_payload holds the deterministic source pattern.
 * @post On success `"<prefix>STORY.RBK"` holds the compressed blob.
 * @post No file handle is left open on any return path.
 *
 * @note Not thread-safe; single-caller boot context.
 * @since 0.1.0
 */
static ra8_err_t demo_roundtrip(const char* prefix, uint32_t* out_blob_len)
{
  RA8_CHECK_NULL_PTR(prefix, s_tag, "prefix");
  RA8_CHECK_NULL_PTR(out_blob_len, s_tag, "out_blob_len");

  char path[(size_t)k_ra8_io_vfs_name_max + sizeof("STORY.RBK") + 1U] = {};
  (void)strncpy(path, prefix, sizeof(path) - 1U);
  (void)strncat(path, "STORY.RBK", sizeof(path) - strlen(path) - 1U);

  uint32_t blob_len = 0;
  RA8_RETURN_ON_ERROR(ra8_io_vfs_write_compressed(path,
                                                  s_payload,
                                                  (uint32_t)k_demo_payload,
                                                  s_scratch,
                                                  (uint32_t)k_ra8_io_compress_scratch_bytes,
                                                  s_blob,
                                                  (uint32_t)k_demo_blob_cap,
                                                  &blob_len),
                      s_tag,
                      "write compressed");

  uint32_t out_len = 0;
  RA8_RETURN_ON_ERROR(ra8_io_vfs_read_compressed(path,
                                                 s_blob,
                                                 (uint32_t)k_demo_blob_cap,
                                                 s_restored,
                                                 (uint32_t)k_demo_payload,
                                                 &out_len),
                      s_tag,
                      "read compressed");
  if (out_len != (uint32_t)k_demo_payload) {
    return k_ra8_err_invalid_size;
  }
  if (memcmp(s_restored, s_payload, sizeof(s_payload)) != 0) {
    return k_ra8_err_checksum_mismatch;
  }
  *out_blob_len = blob_len;
  return k_ra8_ok;
}

/**
 * @brief Bind both backends and run the transparent round-trip on each.
 *
 * @details Builds the deterministic compressible payload once, then for each
 *          backend (RAM then SDRAM) binds the block device, mounts a FAT12
 *          volume in the VFS, and runs ::demo_roundtrip through the fabric. The
 *          per-backend difference is exactly the bind line plus the VFS prefix.
 *
 * @param[out] out_blob_len Compressed blob length from the RAM round-trip.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Both backends round-tripped intact.
 * @retval k_ra8_err_null_ptr @p out_blob_len was NULL.
 * @retval (other)           The first failing backend's failing-step code.
 *
 * @pre ::demo_setup_or_halt has run (clocks + console up).
 * @pre The SDRAM pins/clocks allow `ra8_io_blockdev_sdram_init` to succeed.
 * @post On success both `ram:/STORY.RBK` and `dr:/STORY.RBK` hold the blob.
 * @post No file handle is left open on any return path.
 *
 * @note Not thread-safe; single-caller boot context.
 * @since 0.1.0
 */
static ra8_err_t demo_run(uint32_t* out_blob_len)
{
  RA8_CHECK_NULL_PTR(out_blob_len, s_tag, "out_blob_len");

  for (uint32_t i = 0; i < (uint32_t)k_demo_payload; ++i) {
    s_payload[i] = (uint8_t)((i * (uint32_t)k_demo_seed_mul) % (uint32_t)k_demo_pattern_mod);
  }

  /* Backend 1: RAM disk over an in-SRAM buffer, mounted as "ram". */
  RA8_RETURN_ON_ERROR(
    ra8_io_blockdev_ram_init(&s_bd_ram, &s_bstate_ram, s_disk, (uint32_t)k_demo_disk_blocks, false),
    s_tag,
    "ram blockdev init");
  RA8_RETURN_ON_ERROR(demo_mount(&s_bd_ram, &s_be_ram, "ram"), s_tag, "ram mount");
  uint32_t ram_blob_len = 0;
  RA8_RETURN_ON_ERROR(demo_roundtrip("ram:/", &ram_blob_len), s_tag, "ram round-trip");

  /* Backend 2: external SDRAM as a volatile ramdisk, mounted as "dr". */
  RA8_RETURN_ON_ERROR(
    ra8_io_blockdev_sdram_init(&s_bd_dr, &s_bstate_dr, (uint32_t)k_demo_disk_blocks),
    s_tag,
    "sdram blockdev init");
  RA8_RETURN_ON_ERROR(demo_mount(&s_bd_dr, &s_be_dr, "dr"), s_tag, "dr mount");
  uint32_t dr_blob_len = 0;
  RA8_RETURN_ON_ERROR(demo_roundtrip("dr:/", &dr_blob_len), s_tag, "dr round-trip");

  *out_blob_len = ram_blob_len;
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
  (void)ra8_io_log_attach(&s_uart);
  demo_print("ra8_io_compress_demo: boot\r\n");

  uint32_t        blob_len = 0;
  const ra8_err_t e        = demo_run(&blob_len);
  if (e == k_ra8_ok) {
    demo_print("ra8_io_compress_demo: ram+dr 4096 -> ");
    (void)ra8_io_stream_put_u32(&s_uart, blob_len);
    demo_print(" bytes -> 4096 round-trip PASS\r\n");
  } else {
    demo_print("ra8_io_compress_demo: FAIL\r\n");
  }
  (void)ra8_sci_flush((uint8_t)k_demo_uart_chan);
  while (true) {
  }
}
