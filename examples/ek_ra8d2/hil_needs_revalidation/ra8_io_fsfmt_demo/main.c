/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/ra8_io_fsfmt_demo/main.c
 * @brief ra8_io pluggable filesystem-format registry demo (Phase 4, #159).
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Exercises the format registry that lets the fabric recognise the on-disk
 * filesystem without the upper layers hard-coding a `switch` over FAT vs exFAT:
 *   1. Build a RAM block device, bridge it to ra8_fs, and format a FAT12 volume
 *      on it (Phase 1/3, #156/#158).
 *   2. Register the built-in formats (`ra8_io_fsfmt_init`) and probe the FAT
 *      volume: it must resolve to the "fat" format and report FAT capabilities
 *      (writable, streaming-write, 8.3 name length).
 *   3. Demonstrate the foreign-format seam: register a stub format whose probe
 *      claims any volume bearing a magic byte in block 0, write that byte onto a
 *      second tiny RAM device, and probe it -- it must resolve to "stub" with
 *      the stub's (read-only, case-sensitive) capabilities, proving a new format
 *      plugs in with no change to the registry's built-ins.
 *   4. Report progress on the SCI8 console through a ra8_io UART stream sink.
 *
 * The ra8_emulator captures the SCI8 console, so the PASS line is
 * observable headlessly: a successful run prints
 * `ra8_io_fsfmt_demo: probed fat maxname=12 + foreign stub seam PASS`.
 */

#include <stdint.h>
#include <string.h>

#include "ra8_cgc.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_io.h"
#include "ra8_io_fsfmt.h"
#include "ra8_log.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "ra8_sci.h"
#include "ra8_time.h"

/** @enum demo_const_t @brief Console + volume knobs (no magic numbers). */
typedef enum : uint32_t {
  k_demo_uart_chan    = 8U,      /**< SCI8 J-Link OB console.               */
  k_demo_uart_baud    = 115200U, /**< Console baud.                         */
  k_demo_disk_blocks  = 512U,    /**< 256 KiB RAM-disk (FAT12).             */
  k_demo_stub_blocks  = 8U,      /**< 4 KiB tiny disk for the stub vol.     */
  k_demo_pin_shift    = 8U,      /**< Port byte position in ra8_port_pin_t. */
  k_demo_fat_maxname  = 12U,     /**< Expected FAT 8.3 name length.         */
  k_demo_stub_maxname = 16U,     /**< Stub format max name length.          */
  k_demo_stub_magic   = 0x5AU,   /**< Stub block-0 signature byte.          */
} demo_const_t;

/** @brief SCI8 console TXD = PD02. */
static const ra8_port_pin_t k_demo_txd =
  (ra8_port_pin_t)(((uint16_t)k_ra8_port_13 << (uint16_t)k_demo_pin_shift) | (uint16_t)k_ra8_pin_2);
/** @brief SCI8 console RXD = PD03. */
static const ra8_port_pin_t k_demo_rxd =
  (ra8_port_pin_t)(((uint16_t)k_ra8_port_13 << (uint16_t)k_demo_pin_shift) | (uint16_t)k_ra8_pin_3);

/** @brief 256 KiB RAM-disk backing buffer for the FAT volume (in SRAM .bss). */
static uint8_t s_disk[(size_t)k_demo_disk_blocks * (size_t)k_ra8_io_block_size_bytes];
/** @brief 4 KiB RAM-disk backing buffer for the stub-signature volume. */
static uint8_t s_stub_disk[(size_t)k_demo_stub_blocks * (size_t)k_ra8_io_block_size_bytes];
/** @brief Block-device handle + its RAM backend state (FAT volume). */
static ra8_io_blockdev_t           s_bd;
static ra8_io_blockdev_ram_state_t s_bstate;
/** @brief ra8_fs backend bridged onto the FAT block device. */
static ra8_fs_backend_t s_be;
/** @brief UART output stream + its sink state. */
static ra8_io_stream_t            s_uart;
static ra8_io_stream_uart_state_t s_ust;

/** @brief Module log tag. */
static const char* const s_tag = "ra8_io_fsfmt_demo";

/** @brief Print a NUL-terminated string on the UART stream. */
static void demo_print(const char* msg)
{
  (void)ra8_io_stream_puts(&s_uart, msg);
}

/** @brief Foreign-format probe: block 0 begins with the stub magic byte. */
static bool stub_probe(const ra8_fs_backend_t* be)
{
  if (be == nullptr) {
    return false;
  }
  if (be->read_block == nullptr) {
    return false;
  }
  uint8_t blk[(size_t)k_ra8_io_block_size_bytes];
  if (be->read_block(be->ctx, 0, 1, blk) != k_ra8_ok) {
    return false;
  }
  return blk[0] == (uint8_t)k_demo_stub_magic;
}

/** @brief Foreign stub format descriptor (read-only, case-sensitive). */
static const ra8_io_fsfmt_t k_demo_stub = {
  .name = "stub",
  .caps =
    {
      .max_name_len             = (uint16_t)k_demo_stub_maxname,
      .read_only                = true,
      .supports_mkdir           = false,
      .supports_streaming_write = false,
      .case_sensitive           = true,
    },
  .probe = stub_probe,
};

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

/** @brief Format a FAT12 RAM volume and probe it; require the "fat" format. */
static ra8_err_t demo_probe_fat(const ra8_io_fsfmt_t** out_fmt)
{
  RA8_CHECK_NULL_PTR(out_fmt, s_tag, "out_fmt");
  RA8_RETURN_ON_ERROR(
    ra8_io_blockdev_ram_init(&s_bd, &s_bstate, s_disk, (uint32_t)k_demo_disk_blocks, false),
    s_tag,
    "blockdev init");
  RA8_RETURN_ON_ERROR(ra8_io_blockdev_as_fs_backend(&s_bd, &s_be), s_tag, "fs bridge");
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_fat12;
  opts.label                = "RAIO";
  RA8_RETURN_ON_ERROR(ra8_fs_format(&s_be, &opts), s_tag, "format");

  RA8_RETURN_ON_ERROR(ra8_io_fsfmt_init(), s_tag, "fsfmt init");
  RA8_RETURN_ON_ERROR(ra8_io_fsfmt_probe(&s_be, out_fmt), s_tag, "probe fat");
  if (strcmp((*out_fmt)->name, "fat") != 0) {
    return k_ra8_err_not_found;
  }
  if ((*out_fmt)->caps.read_only || !(*out_fmt)->caps.supports_streaming_write) {
    return k_ra8_err_not_supported;
  }
  if ((*out_fmt)->caps.max_name_len != (uint16_t)k_demo_fat_maxname) {
    return k_ra8_err_invalid_size;
  }
  return k_ra8_ok;
}

/** @brief Register the foreign stub format, craft its volume, and require it wins. */
static ra8_err_t demo_probe_foreign(void)
{
  RA8_RETURN_ON_ERROR(ra8_io_fsfmt_register(&k_demo_stub), s_tag, "register stub");

  ra8_io_blockdev_t           bd    = {};
  ra8_io_blockdev_ram_state_t state = {};
  RA8_RETURN_ON_ERROR(
    ra8_io_blockdev_ram_init(&bd, &state, s_stub_disk, (uint32_t)k_demo_stub_blocks, false),
    s_tag,
    "stub blockdev");
  uint8_t blk[(size_t)k_ra8_io_block_size_bytes] = {};
  blk[0]                                         = (uint8_t)k_demo_stub_magic;
  RA8_RETURN_ON_ERROR(ra8_io_blockdev_write(&bd, 0, 1, blk), s_tag, "stub mark");

  ra8_fs_backend_t be = {};
  RA8_RETURN_ON_ERROR(ra8_io_blockdev_as_fs_backend(&bd, &be), s_tag, "stub bridge");
  const ra8_io_fsfmt_t* fmt = nullptr;
  RA8_RETURN_ON_ERROR(ra8_io_fsfmt_probe(&be, &fmt), s_tag, "probe stub");
  if (strcmp(fmt->name, "stub") != 0) {
    return k_ra8_err_not_found;
  }
  if (!fmt->caps.read_only || !fmt->caps.case_sensitive) {
    return k_ra8_err_not_supported;
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
  demo_print("ra8_io_fsfmt_demo: boot\r\n");

  const ra8_io_fsfmt_t* fat = nullptr;
  if ((demo_probe_fat(&fat) == k_ra8_ok) && (demo_probe_foreign() == k_ra8_ok)) {
    demo_print("ra8_io_fsfmt_demo: probed ");
    demo_print(fat->name);
    demo_print(" maxname=");
    (void)ra8_io_stream_put_u32(&s_uart, (uint32_t)fat->caps.max_name_len);
    demo_print(" + foreign stub seam PASS\r\n");
  } else {
    demo_print("ra8_io_fsfmt_demo: FAIL\r\n");
  }
  (void)ra8_sci_flush((uint8_t)k_demo_uart_chan);
  while (true) {
  }
}
