/**
 * @file test_ra8_vfs_compress.c
 * @brief Unit tests for the app-owned transparent VFS compression seam plus the
 *        UART / USB-CDC stream sink data paths (ra8_io fabric, issues #157/#161).
 *
 * @details
 * Three uncovered ra8_io modules are exercised host-side over RAM-backed models,
 * so no real hardware is required:
 *
 * - ::ra8_vfs_compress_write / ::ra8_vfs_compress_read over a FAT16
 *   volume on a RAM block device: a compress-on-write then decompress-on-read
 *   round trip is byte-identical, the on-disk blob is smaller than the source,
 *   and every guard path (NULL args on both directions, undersized scratch,
 *   too-small staging / output buffers, missing `name:` prefix, absent mount,
 *   absent file) returns the documented error. This drives both private helpers
 *   (`ra8_vfs_compress_store_blob` / `ra8_vfs_compress_load_blob`) on both
 *   their success and error legs.
 *
 * - The USB-CDC stream sink data path through the heap-free ra8_usb_pal ring
 *   model: a bound stream write splits into PAL transfers, the bytes land in the
 *   endpoint ring and read back identically, and NULL / unopened-endpoint guards
 *   return the documented error.
 *
 * - The UART stream sink data path: NULL guards plus error propagation from
 *   `ra8_sci_write_polling` / `ra8_sci_flush` when the bound channel index is out
 *   of range (the host build has no live SCI, so the invalid-channel leg is the
 *   clean, spin-free way to drive the sink's error branch).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_compress.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_io_blockdev.h"
#include "ra8_io_blockdev_ram.h"
#include "ra8_io_stream.h"
#include "ra8_io_stream_uart.h"
#include "ra8_io_stream_usbcdc.h"
#include "ra8_io_vfs.h"
#include "ra8_usb_pal.h"
#include "ra8_vfs_compress.h"
#include "unity_minimal.h"

/**
 * @enum t_vfscz_const_t
 * @brief Fixture sizes.
 */
typedef enum : uint32_t {
  k_t_disk_blocks = 16384, /**< 8 MiB backing buffer -- comfortably FAT16.    */
  k_t_payload     = 4096,  /**< Plain payload byte count (repetitive).        */
  k_t_blob_cap    = 8192,  /**< Staging buffer capacity for the blob.         */
  k_t_seed_mul    = 31,    /**< Pattern generator multiplier.                 */
  k_t_pattern_mod = 7,     /**< Small alphabet so the payload compresses.     */
  k_t_bad_channel = 200,   /**< SCI channel index past k_ra8_sci_channel_max. */
} t_vfscz_const_t;

/** @brief Plain payload, compressor scratch, staging blob, and restore buffer. */
static uint8_t s_payload[(size_t)k_t_payload];
static uint8_t s_scratch[(size_t)k_ra8_compress_scratch_bytes];
static uint8_t s_blob[(size_t)k_t_blob_cap];
static uint8_t s_restored[(size_t)k_t_payload];

/** @brief Fill the payload with a small-alphabet repetitive (compressible) pattern. @details Exercises the fill payload path with bounded caller-owned fixture state and verifies its documented result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_fill_payload(void)
{
  for (uint32_t i = 0; i < (uint32_t)k_t_payload; ++i) {
    s_payload[i] = (uint8_t)((i * (uint32_t)k_t_seed_mul) % (uint32_t)k_t_pattern_mod);
  }
}

/** @brief Build a fresh empty FAT16 volume on the RAM disk and mount it as "ram". @details Exercises the setup ram mount path with bounded caller-owned fixture state and verifies its documented result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_setup_ram_mount(void)
{
  static uint8_t s_disk[(size_t)k_t_disk_blocks * (size_t)k_ra8_io_block_size_bytes];
  static ra8_io_blockdev_ram_state_t s_bstate;
  static ra8_io_blockdev_t           s_bd;
  static ra8_fs_backend_t            s_be;
  static ra8_fs_mount_t*             s_mnt;

  if (s_mnt != nullptr) {
    (void)ra8_fs_unmount(s_mnt); /* ra8_fs has only 2 mount slots -- free the prior one */
    s_mnt = nullptr;
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_init());
  (void)ra8_io_blockdev_ram_init(&s_bd, &s_bstate, s_disk, k_t_disk_blocks, false);
  (void)ra8_io_blockdev_as_fs_backend(&s_bd, &s_be);
  ra8_fs_format_opts_t opts = {.type = k_ra8_fs_type_fat16, .label = "RBKZ"};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_be, &opts));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_be, &s_mnt));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mount("ram", s_mnt));
}

/* =============================================================================
 * VFS transparent compression
 * =============================================================================
 */

/**
 * @par MC/DC:
 * (no compound decisions under test -- a compress-on-write then
 * decompress-on-read reproduces the input exactly and the on-disk blob is
 * strictly smaller than the source payload) @brief Verify vfs compress round trip behavior. @details Executes the vfs compress round trip scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_vfs_compress_round_trip(void)
{
  TEST_BEGIN("vfs compress round-trip");
  internal_fill_payload();
  internal_setup_ram_mount();

  uint32_t blob_len = 0;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_vfs_compress_write("ram:/STORY.RBK",
                                        s_payload,
                                        k_t_payload,
                                        s_scratch,
                                        (uint32_t)sizeof(s_scratch),
                                        s_blob,
                                        (uint32_t)sizeof(s_blob),
                                        &blob_len));
  TEST_ASSERT(blob_len > 0U);
  TEST_ASSERT(blob_len < (uint32_t)k_t_payload); /* repetitive -> shrinks on disk */

  /* stat confirms the file holds the compressed blob, not the plain payload */
  ra8_io_vfs_stat_t st = {.size_bytes = 0U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_stat("ram:/STORY.RBK", &st));
  TEST_ASSERT(st.exists);
  TEST_ASSERT_EQ(blob_len, st.size_bytes);

  uint32_t out_len = 0;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_vfs_compress_read("ram:/STORY.RBK",
                                       s_blob,
                                       (uint32_t)sizeof(s_blob),
                                       s_restored,
                                       (uint32_t)sizeof(s_restored),
                                       &out_len));
  TEST_ASSERT_EQ(k_t_payload, out_len);
  TEST_ASSERT_EQ(0, memcmp(s_payload, s_restored, (size_t)k_t_payload));
  TEST_END("vfs compress round-trip");
}

/**
 * @brief Wrapper over `ra8_vfs_compress_write` (payload len + blob cap fixed).
 * @param[in]  path    VFS destination path (nullptr exercises the path guard).
 * @param[in]  payload Source payload (nullptr exercises the payload guard).
 * @param[in]  scratch Compressor scratch (nullptr exercises the scratch guard).
 * @param[in]  scap    Scratch capacity in bytes.
 * @param[out] blob    Output blob buffer (nullptr exercises the blob guard).
 * @param[out] out     Blob-length sink (nullptr exercises the length guard).
 * @return The `ra8_vfs_compress_write` result code.
 * @pre A RAM mount is set up.
 * @post No state beyond @p blob / @p out and the mount is modified.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0 @details Exercises the vc write path with bounded caller-owned fixture state and verifies its documented result. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @post Documented outputs contain the exercised result when the operation succeeds. */
RA8_INTERNAL static ra8_err_t internal_vc_write(const char*    path,
                                                const uint8_t* payload,
                                                uint8_t*       scratch,
                                                uint32_t       scap,
                                                uint8_t*       blob,
                                                uint32_t*      out)
{
  return ra8_vfs_compress_write(path,
                                payload,
                                k_t_payload,
                                scratch,
                                scap,
                                blob,
                                (uint32_t)sizeof(s_blob),
                                out);
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- each of the five write-path pointer
 * guards is an independent single-condition check, and undersized scratch is a
 * sixth independent single-condition check) @brief Verify vfs compress write validation behavior. @details Executes the vfs compress write validation scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_vfs_compress_write_validation(void)
{
  TEST_BEGIN("vfs compress write validation");
  internal_fill_payload();
  internal_setup_ram_mount();
  uint32_t       blob_len = 0;
  const uint32_t scap     = (uint32_t)sizeof(s_scratch);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 internal_vc_write(nullptr, s_payload, s_scratch, scap, s_blob, &blob_len));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 internal_vc_write("ram:/X.RBK", nullptr, s_scratch, scap, s_blob, &blob_len));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 internal_vc_write("ram:/X.RBK", s_payload, nullptr, scap, s_blob, &blob_len));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 internal_vc_write("ram:/X.RBK", s_payload, s_scratch, scap, nullptr, &blob_len));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 internal_vc_write("ram:/X.RBK", s_payload, s_scratch, scap, s_blob, nullptr));

  /* undersized scratch -> the compressor rejects with invalid_size */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 internal_vc_write("ram:/X.RBK",
                                   s_payload,
                                   s_scratch,
                                   (uint32_t)k_ra8_compress_scratch_bytes - 1U,
                                   s_blob,
                                   &blob_len));
  TEST_END("vfs compress write validation");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- a staging blob too small to hold the
 * compressed stream trips the compressor overflow path, and a path with no
 * `name:` prefix / an absent mount each trip an independent VFS guard) @brief Verify vfs compress write errors behavior. @details Executes the vfs compress write errors scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_vfs_compress_write_errors(void)
{
  TEST_BEGIN("vfs compress write errors");
  internal_fill_payload();
  internal_setup_ram_mount();
  uint32_t blob_len = 0;
  uint8_t  tiny[4]  = {[0] = 0U};

  /* staging blob too small -> compressor reports no_mem (store never runs) */
  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 ra8_vfs_compress_write("ram:/X.RBK",
                                        s_payload,
                                        k_t_payload,
                                        s_scratch,
                                        (uint32_t)sizeof(s_scratch),
                                        tiny,
                                        (uint32_t)sizeof(tiny),
                                        &blob_len));

  /* path with no "name:" prefix -> store_blob's VFS open rejects it */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_vfs_compress_write("noprefix",
                                        s_payload,
                                        k_t_payload,
                                        s_scratch,
                                        (uint32_t)sizeof(s_scratch),
                                        s_blob,
                                        (uint32_t)sizeof(s_blob),
                                        &blob_len));

  /* absent mount name -> store_blob's VFS open reports not_found */
  TEST_ASSERT_EQ(k_ra8_err_not_found,
                 ra8_vfs_compress_write("nope:/X.RBK",
                                        s_payload,
                                        k_t_payload,
                                        s_scratch,
                                        (uint32_t)sizeof(s_scratch),
                                        s_blob,
                                        (uint32_t)sizeof(s_blob),
                                        &blob_len));
  TEST_END("vfs compress write errors");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- each of the four read-path pointer guards
 * is an independent single-condition check) @brief Verify vfs compress read validation behavior. @details Executes the vfs compress read validation scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_vfs_compress_read_validation(void)
{
  TEST_BEGIN("vfs compress read validation");
  internal_fill_payload();
  internal_setup_ram_mount();
  uint32_t out_len = 0;

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_vfs_compress_read(nullptr,
                                       s_blob,
                                       (uint32_t)sizeof(s_blob),
                                       s_restored,
                                       (uint32_t)sizeof(s_restored),
                                       &out_len));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_vfs_compress_read("ram:/X.RBK",
                                       nullptr,
                                       (uint32_t)sizeof(s_blob),
                                       s_restored,
                                       (uint32_t)sizeof(s_restored),
                                       &out_len));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_vfs_compress_read("ram:/X.RBK",
                                       s_blob,
                                       (uint32_t)sizeof(s_blob),
                                       nullptr,
                                       (uint32_t)sizeof(s_restored),
                                       &out_len));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_vfs_compress_read("ram:/X.RBK",
                                       s_blob,
                                       (uint32_t)sizeof(s_blob),
                                       s_restored,
                                       (uint32_t)sizeof(s_restored),
                                       nullptr));
  TEST_END("vfs compress read validation");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- an absent file, an undersized output
 * buffer, and an undersized staging blob each trip an independent guard on the
 * read path) @brief Verify vfs compress read errors behavior. @details Executes the vfs compress read errors scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_vfs_compress_read_errors(void)
{
  TEST_BEGIN("vfs compress read errors");
  internal_fill_payload();
  internal_setup_ram_mount();

  /* seed a real compressed file to read back through the error legs */
  uint32_t blob_len = 0;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_vfs_compress_write("ram:/STORY.RBK",
                                        s_payload,
                                        k_t_payload,
                                        s_scratch,
                                        (uint32_t)sizeof(s_scratch),
                                        s_blob,
                                        (uint32_t)sizeof(s_blob),
                                        &blob_len));

  uint32_t out_len = 0;

  /* absent file -> load_blob's VFS open reports not_found */
  TEST_ASSERT_EQ(k_ra8_err_not_found,
                 ra8_vfs_compress_read("ram:/MISSING.RBK",
                                       s_blob,
                                       (uint32_t)sizeof(s_blob),
                                       s_restored,
                                       (uint32_t)sizeof(s_restored),
                                       &out_len));

  /* output buffer too small to hold the inflated payload -> decompress no_mem */
  uint8_t small_out[8] = {[0] = 0U};
  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 ra8_vfs_compress_read("ram:/STORY.RBK",
                                       s_blob,
                                       (uint32_t)sizeof(s_blob),
                                       small_out,
                                       (uint32_t)sizeof(small_out),
                                       &out_len));

  /* staging blob one byte too small for the on-disk blob -> load_blob reports
   * invalid_size (a full-buffer read cannot be told apart from truncation).
   * Cap the real blob buffer at blob_len-1 so it is provably smaller than the
   * compressed file, whatever miniz produced for this payload. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_vfs_compress_read("ram:/STORY.RBK",
                                       s_blob,
                                       blob_len - 1U,
                                       s_restored,
                                       (uint32_t)sizeof(s_restored),
                                       &out_len));
  TEST_END("vfs compress read errors");
}

/* =============================================================================
 * USB-CDC stream sink data path
 * =============================================================================
 */

/**
 * @par MC/DC:
 * (no compound decisions under test -- a bound stream write streams every byte
 * through the PAL ring and the bytes read back identically) @brief Verify usbcdc sink write behavior. @details Executes the usbcdc sink write scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_usbcdc_sink_write(void)
{
  TEST_BEGIN("usbcdc sink write");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pal_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_pal_ep_open(1U, k_ra8_usb_pal_ep_dir_in, k_ra8_usb_pal_ep_type_bulk, 64U));

  ra8_io_stream_t              s   = {.iface = nullptr};
  ra8_io_stream_usbcdc_state_t cst = {.ep_addr = 0U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_usbcdc_init(&s, &cst, 0x81)); /* 0x81 -> ep 1 */

  const uint8_t msg[]   = {0xDEU, 0xADU, 0xBEU, 0xEFU, 0x10U};
  uint32_t      written = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_write(&s, msg, (uint32_t)sizeof(msg), &written));
  TEST_ASSERT_EQ(sizeof(msg), written);

  uint8_t  rx[16] = {[0] = 0U};
  uint16_t rx_len = (uint16_t)sizeof(rx);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pal_ep_recv(1U, rx, &rx_len));
  TEST_ASSERT_EQ(sizeof(msg), rx_len);
  TEST_ASSERT_EQ(0, memcmp(rx, msg, sizeof(msg)));
  TEST_END("usbcdc sink write");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- a NULL stream / NULL buffer trip the
 * generic stream guards, and a write to an unopened endpoint trips the PAL
 * error leg of the sink's write callback) @brief Verify usbcdc sink errors behavior. @details Executes the usbcdc sink errors scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_usbcdc_sink_errors(void)
{
  TEST_BEGIN("usbcdc sink errors");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pal_init(k_ra8_usb_speed_fs));
  /* endpoint 2 is deliberately NOT opened */

  ra8_io_stream_t              s   = {.iface = nullptr};
  ra8_io_stream_usbcdc_state_t cst = {.ep_addr = 0U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_usbcdc_init(&s, &cst, 2U));

  const uint8_t msg[]   = {1U, 2U, 3U};
  uint32_t      written = 0;
  /* generic stream-layer guards: NULL handle / NULL source buffer */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_stream_write(nullptr, msg, 3U, &written));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_stream_write(&s, nullptr, 3U, &written));
  /* sink write callback propagates the PAL error for the unopened endpoint */
  const ra8_err_t e = ra8_io_stream_write(&s, msg, (uint32_t)sizeof(msg), &written);
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, e);
  TEST_END("usbcdc sink errors");
}

/* =============================================================================
 * UART stream sink data path
 * =============================================================================
 */

/**
 * @par MC/DC:
 * (no compound decisions under test -- a NULL handle / NULL buffer trip the
 * generic stream guards, while an out-of-range channel makes the SCI driver
 * return invalid_arg, which the UART sink propagates on both write and flush) @brief Verify uart sink errors behavior. @details Executes the uart sink errors scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_uart_sink_errors(void)
{
  TEST_BEGIN("uart sink errors");
  ra8_io_stream_t            s   = {.iface = nullptr};
  ra8_io_stream_uart_state_t ust = {.channel = 0U};
  /* bind to an out-of-range SCI channel: the host build has no live SCI, so the
   * driver's internal_reg() returns NULL and reports invalid_arg without ever
   * spinning on a status flag. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_uart_init(&s, &ust, (uint8_t)k_t_bad_channel));

  const uint8_t msg[]   = {'h', 'i'};
  uint32_t      written = 0;
  /* generic stream-layer guards */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_stream_write(nullptr, msg, 2U, &written));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_stream_write(&s, nullptr, 2U, &written));
  /* sink write callback propagates the SCI invalid-channel error */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_io_stream_write(&s, msg, (uint32_t)sizeof(msg), &written));
  /* sink flush callback propagates the same SCI invalid-channel error */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_io_stream_flush(&s));
  TEST_END("uart sink errors");
}

int main(void)
{
  internal_test_vfs_compress_round_trip();
  internal_test_vfs_compress_write_validation();
  internal_test_vfs_compress_write_errors();
  internal_test_vfs_compress_read_validation();
  internal_test_vfs_compress_read_errors();
  internal_test_usbcdc_sink_write();
  internal_test_usbcdc_sink_errors();
  internal_test_uart_sink_errors();
  return 0;
}
