/**
 * @file examples/ek_ra8d2/sdhi_card_demo/main.c
 * @brief Native 4-bit SDHI microSD bring-up + FAT round-trip demo (EK-RA8D2)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The native-SDHI analogue of the SPI-mode SD apps (`tz_secure_only_sd`,
 * `sd_font_render`): instead of clocking the card byte-by-byte over an SCI
 * Simple-SPI bus, it drives the on-board microSD through the dedicated **SDHI**
 * 4-bit controller. The full SD Physical Layer identification
 * (CMD0 -> CMD8 -> ACMD41 -> CMD2 -> CMD3 -> CMD9 -> CMD7) and the polled
 * 512-byte block read/write live in `ra_sdcard` + `ra_sdhi`; this app wires
 * `ra_sdcard` into an `ra_fs` block-device backend, mounts the FAT volume, and
 * round-trips a file.
 *
 * Flow:
 *  1. CGC + SysTick + SCI8 + LEDs + MSTP.
 *  2. Route the eight SDHI pins (port 4, pins 0..7: CMD/CLK/DAT0..3/WP/CD) to
 *     `PSEL = k_ra_psel_sdhi`.
 *  3. `ra_sdhi_init(0)` + `ra_sdcard_init({.instance = 0})` -- card ident +
 *     clock step-up to default speed.
 *  4. Bind `ra_sdcard_read_blocks` / `ra_sdcard_write_blocks` /
 *     `ra_sdcard_get_capacity` into an `ra_fs_backend_t` and `ra_fs_mount`.
 *  5. Write a fixed-seed payload to `TEST.TXT`, read it back, compare.
 *
 * On success: `sdhi: roundtrip ok`. LED1 toggles healthy, LED2 on a fault.
 * `g_sdhi_stage` / `g_sdhi_ok` / `g_sdhi_blocks` / `g_sdhi_heartbeat` mirror the
 * result for headless probing.
 *
 * @note **Headless-emulator status.** `tools/board_sim` does not model the SDHI
 * controller or a card on it, so the identification sequence times out on the
 * emulator (no `SD_INFO1.RSPEND`) and the demo stops at `g_sdhi_stage =
 * cardinit` -- it runs without faulting but cannot reach `roundtrip ok` without
 * a real card. The round-trip therefore needs silicon; the app lives in
 * `hw_pending/`. See `README.md` for the bench plan.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_fs.h"
#include "ra_gpio_constants.h"
#include "ra_isr.h"
#include "ra_mstp.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_sci.h"
#include "ra_sdcard.h"
#include "ra_sdhi.h"
#include "ra_time.h"

/** @brief Diagnostic / log tag. */
static const char* s_tag = "sdhi_demo";

/** @brief Compile-time settings. */
typedef enum : uint32_t {
  k_sdhi_demo_baud        = 115200U,     /**< SCI8 baud rate.                 */
  k_sdhi_demo_sci_channel = 8U,          /**< J-Link OB CDC is on SCI8.       */
  k_sdhi_demo_payload     = 512U,        /**< Round-trip payload bytes.       */
  k_sdhi_demo_pat_xor     = 0xA5A5A5A5U, /**< Per-word payload pattern XOR.   */
  k_sdhi_demo_block_size  = 512U,        /**< SD block size (bytes).          */
  k_sdhi_demo_period_ms   = 500U,        /**< Heartbeat LED toggle period.    */
} sdhi_demo_config_t;

/** @enum sdhi_demo_mask_t @brief Bit masks for payload byte extraction. */
typedef enum : uint32_t {
  k_sdhi_demo_byte_mask = 0xFFU, /**< Low-byte mask for a pattern word. */
} sdhi_demo_mask_t;

/** @brief SDHI controller instance + pin geometry. */
typedef enum : uint8_t {
  k_sdhi_demo_instance  = 0U, /**< SDHI0 drives the on-board microSD.   */
  k_sdhi_demo_pin_count = 8U, /**< CMD/CLK/DAT0..3/WP/CD on port 4.     */
} sdhi_demo_geom_t;

/** @brief Bring-up stage reached -- mirrored to ::g_sdhi_stage for probes. */
typedef enum : uint32_t {
  k_sdhi_stage_boot      = 0U, /**< Pre-init.                            */
  k_sdhi_stage_pins      = 1U, /**< SDHI pins routed.                    */
  k_sdhi_stage_cardinit  = 2U, /**< ra_sdcard_init reached (may fail).   */
  k_sdhi_stage_mount     = 3U, /**< ra_fs_mount succeeded.               */
  k_sdhi_stage_roundtrip = 4U, /**< Write + read-back + compare OK.     */
} sdhi_demo_stage_t;

/** @brief Pinout for SCI8 on the J-Link OB CDC channel (PD02 / PD03). */
static const ra_port_pin_t k_sdhi_demo_pin_txd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);
static const ra_port_pin_t k_sdhi_demo_pin_rxd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);

/** @brief SDHI pin list (port 4, pins 0..7). */
static const ra_port_pin_t k_sdhi_demo_pins[k_sdhi_demo_pin_count] = {
  (ra_port_pin_t)(((uint16_t)k_ra_port_4 << 8) | (uint16_t)k_ra_pin_0), /* SD_CMD  */
  (ra_port_pin_t)(((uint16_t)k_ra_port_4 << 8) | (uint16_t)k_ra_pin_1), /* SD_CLK  */
  (ra_port_pin_t)(((uint16_t)k_ra_port_4 << 8) | (uint16_t)k_ra_pin_2), /* SD_DAT0 */
  (ra_port_pin_t)(((uint16_t)k_ra_port_4 << 8) | (uint16_t)k_ra_pin_3), /* SD_DAT1 */
  (ra_port_pin_t)(((uint16_t)k_ra_port_4 << 8) | (uint16_t)k_ra_pin_4), /* SD_DAT2 */
  (ra_port_pin_t)(((uint16_t)k_ra_port_4 << 8) | (uint16_t)k_ra_pin_5), /* SD_DAT3 */
  (ra_port_pin_t)(((uint16_t)k_ra_port_4 << 8) | (uint16_t)k_ra_pin_6), /* SD_WP   */
  (ra_port_pin_t)(((uint16_t)k_ra_port_4 << 8) | (uint16_t)k_ra_pin_7), /* SD_CD   */
};

/** @brief Output line tags. */
static const uint8_t k_sdhi_demo_ok_msg[]       = "sdhi: roundtrip ok\r\n";
static const uint8_t k_sdhi_demo_cardinit_msg[] = "sdhi: card init failed (needs silicon+card)\r\n";
static const uint8_t k_sdhi_demo_mount_msg[]    = "sdhi: mount failed\r\n";
static const uint8_t k_sdhi_demo_rw_msg[]       = "sdhi: rw mismatch\r\n";

/**
 * @var g_sdhi_stage
 * @brief Highest bring-up stage reached (::sdhi_demo_stage_t).
 * @note Read externally only (HIL / board emulator).
 * @since 0.1.0
 */
volatile uint32_t g_sdhi_stage = (uint32_t)k_sdhi_stage_boot;

/**
 * @var g_sdhi_ok
 * @brief 1 when the FAT round-trip compared byte-exact.
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_sdhi_ok = 0U;

/**
 * @var g_sdhi_blocks
 * @brief Card capacity in 512-byte blocks (0 until ra_sdcard_init succeeds).
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_sdhi_blocks = 0U;

/**
 * @var g_sdhi_heartbeat
 * @brief Bumps once per main-loop pass -- liveness for headless probes.
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_sdhi_heartbeat = 0U;

/** @brief Round-trip payload + read-back buffers (file-scope, off the stack). */
static uint8_t s_payload[k_sdhi_demo_payload];
static uint8_t s_readback[k_sdhi_demo_payload];

/** @brief Park forever after a fatal init failure. */
static void sdhi_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief Emit a byte run on the SCI8 console. */
static void sdhi_demo_print(const uint8_t* msg, uint32_t len)
{
  (void)ra_sci_write_polling((uint8_t)k_sdhi_demo_sci_channel, msg, len);
}

/** @brief Deterministic payload pattern for word @p i. */
static uint32_t sdhi_demo_pattern(uint32_t i)
{
  return (i * i) ^ (uint32_t)k_sdhi_demo_pat_xor;
}

/** @brief Fill @p buf with the fixed-seed payload pattern. */
static void sdhi_demo_fill(uint8_t* buf, uint32_t len)
{
  for (uint32_t i = 0U; i < len; ++i) {
    buf[i] = (uint8_t)(sdhi_demo_pattern(i) & (uint32_t)k_sdhi_demo_byte_mask);
  }
}

/**
 * @brief ra_fs backend: read @p count blocks at @p lba from the SD card.
 * @details Thin adapter forwarding to ::ra_sdcard_read_blocks.
 * @param[in]  ctx   Unused backend cookie.
 * @param[in]  lba   First 512-byte block.
 * @param[in]  count Block count.
 * @param[out] buf   Destination (>= count*512 bytes).
 * @return ``ra_err_t`` from ::ra_sdcard_read_blocks.
 * @pre ::ra_sdcard_init succeeded.
 * @post On success @p buf holds the card data.
 * @since 0.1.0
 */
static ra_err_t sdhi_demo_read_block(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  (void)ctx;
  return ra_sdcard_read_blocks(lba, buf, count);
}

/**
 * @brief ra_fs backend: write @p count blocks at @p lba to the SD card.
 * @details Thin adapter forwarding to ::ra_sdcard_write_blocks.
 * @param[in] ctx   Unused backend cookie.
 * @param[in] lba   First 512-byte block.
 * @param[in] count Block count.
 * @param[in] buf   Source (>= count*512 bytes).
 * @return ``ra_err_t`` from ::ra_sdcard_write_blocks.
 * @pre ::ra_sdcard_init succeeded.
 * @post On success the block range was written.
 * @since 0.1.0
 */
static ra_err_t sdhi_demo_write_block(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  (void)ctx;
  return ra_sdcard_write_blocks(lba, buf, count);
}

/**
 * @brief ra_fs backend: report SD capacity + block size.
 * @param[in]  ctx         Unused backend cookie.
 * @param[out] block_count Total 512-byte blocks.
 * @param[out] block_size  Always 512.
 * @return ``ra_err_t`` error code.
 * @retval k_ra_err_null_ptr ``block_count`` or ``block_size`` is NULL.
 * @pre ::ra_sdcard_init succeeded.
 * @post On success ``*block_size == 512``.
 * @since 0.1.0
 */
static ra_err_t sdhi_demo_get_capacity(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  (void)ctx;
  RA_CHECK_NULL_PTR(block_count, s_tag, "block_count must not be nullptr");
  RA_CHECK_NULL_PTR(block_size, s_tag, "block_size must not be nullptr");
  *block_size = (uint32_t)k_sdhi_demo_block_size;
  return ra_sdcard_get_capacity(block_count);
}

/**
 * @brief Route PD02 / PD03 to SCI8 TXD/RXD via PFS.
 * @return ``ra_err_t`` error code from the underlying PFS routing.
 * @pre IOPORT module reachable.
 * @post On success PD02 + PD03 are in SCI-async mode.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t sdhi_demo_console_pins_init(void)
{
  ra_err_t err = ra_pfs_route_peripheral(k_sdhi_demo_pin_txd, k_ra_psel_sci_async, "sdhi.txd8");
  if (err != k_ra_ok) {
    return err;
  }
  return ra_pfs_route_peripheral(k_sdhi_demo_pin_rxd, k_ra_psel_sci_async, "sdhi.rxd8");
}

/**
 * @brief Route the eight SDHI pins to ``PSEL = k_ra_psel_sdhi``.
 * @return ``ra_err_t`` -- first non-ok PFS routing, else k_ra_ok.
 * @pre IOPORT module reachable.
 * @post On success port-4 pins 0..7 are owned by the SDHI block.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t sdhi_demo_sdhi_pins_init(void)
{
  for (uint8_t i = 0U; i < (uint8_t)k_sdhi_demo_pin_count; ++i) {
    const ra_err_t err = ra_pfs_route_peripheral(k_sdhi_demo_pins[i], k_ra_psel_sdhi, "sdhi.bus");
    if (err != k_ra_ok) {
      return err;
    }
  }
  return k_ra_ok;
}

/** @brief Bring CGC + SysTick + SCI8 + LEDs + MSTP up. */
static void sdhi_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;

  if (ra_cgc_init() != k_ra_ok) {
    sdhi_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    sdhi_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz) != k_ra_ok) {
    sdhi_demo_panic_halt();
  }
  if (ra_mstp_init() != k_ra_ok) {
    sdhi_demo_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    sdhi_demo_panic_halt();
  }
  if (sdhi_demo_console_pins_init() != k_ra_ok) {
    sdhi_demo_panic_halt();
  }
  const ra_sci_cfg_t sci_cfg = {
    .baud      = k_sdhi_demo_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_sdhi_demo_sci_channel, &sci_cfg) != k_ra_ok) {
    sdhi_demo_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    sdhi_demo_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led2) != k_ra_ok) {
    sdhi_demo_panic_halt();
  }
}

/**
 * @brief Bind the ra_sdcard backend and FAT-round-trip a file.
 *
 * @param[out] out_ok 1 when the payload read back byte-exact.
 *
 * @par MC/DC:
 * Verdict ``ok = (read_len == payload_len) && (memcmp == 0)`` -- the compare is
 * host-tested (::sdhi_demo_fill + the byte comparison). Stage reporting via
 * ``g_sdhi_stage`` is a sequence of guarded calls.
 *
 * @return ``ra_err_t`` -- the first non-ok status, or k_ra_ok on a clean
 *         round-trip.
 * @retval k_ra_err_null_ptr ``out_ok`` was NULL.
 * @pre ::ra_sdcard_init succeeded.
 * @post ``g_sdhi_stage`` / ``g_sdhi_ok`` updated.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t sdhi_demo_roundtrip(uint8_t* out_ok)
{
  RA_CHECK_NULL_PTR(out_ok, s_tag, "out_ok must not be nullptr");
  *out_ok = 0U;

  ra_fs_backend_t backend = {};
  backend.read_block      = sdhi_demo_read_block;
  backend.write_block     = sdhi_demo_write_block;
  backend.get_capacity    = sdhi_demo_get_capacity;

  ra_fs_mount_t* mount = nullptr;
  const ra_err_t merr  = ra_fs_mount(&backend, &mount);
  if (merr != k_ra_ok) {
    return merr;
  }
  g_sdhi_stage = (uint32_t)k_sdhi_stage_mount;

  sdhi_demo_fill(s_payload, (uint32_t)k_sdhi_demo_payload);
  ra_fs_file_t* existing = nullptr;
  if (ra_fs_open(mount, "TEST.TXT", k_ra_fs_mode_read, &existing) != k_ra_ok) {
    const ra_err_t werr =
      ra_fs_write_file(mount, "TEST.TXT", s_payload, (uint32_t)k_sdhi_demo_payload);
    if (werr != k_ra_ok) {
      (void)ra_fs_unmount(mount);
      return werr;
    }
  } else {
    (void)ra_fs_close(existing);
  }

  ra_fs_file_t* f    = nullptr;
  ra_err_t      oerr = ra_fs_open(mount, "TEST.TXT", k_ra_fs_mode_read, &f);
  if (oerr != k_ra_ok) {
    (void)ra_fs_unmount(mount);
    return oerr;
  }
  uint32_t       got  = 0U;
  const ra_err_t rerr = ra_fs_read(f, s_readback, (uint32_t)k_sdhi_demo_payload, &got);
  (void)ra_fs_close(f);
  (void)ra_fs_unmount(mount);
  if (rerr != k_ra_ok) {
    return rerr;
  }

  uint8_t match = (got == (uint32_t)k_sdhi_demo_payload) ? 1U : 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_sdhi_demo_payload; ++i) {
    if (s_readback[i] != s_payload[i]) {
      match = 0U;
    }
  }
  *out_ok = match;
  return k_ra_ok;
}

/** @brief Bring up SDHI + the card; on failure report the stage and halt. */
static void sdhi_demo_bringup_or_report(void)
{
  if (sdhi_demo_sdhi_pins_init() != k_ra_ok) {
    sdhi_demo_panic_halt();
  }
  g_sdhi_stage = (uint32_t)k_sdhi_stage_pins;

  if (ra_sdhi_init((uint8_t)k_sdhi_demo_instance) != k_ra_ok) {
    sdhi_demo_panic_halt();
  }
  const ra_sdcard_cfg_t cfg = {.instance = (uint8_t)k_sdhi_demo_instance};
  g_sdhi_stage              = (uint32_t)k_sdhi_stage_cardinit;
  if (ra_sdcard_init(&cfg) != k_ra_ok) {
    /* Expected on tools/board_sim (no SDHI/card model): report + park. */
    sdhi_demo_print(k_sdhi_demo_cardinit_msg, (uint32_t)(sizeof(k_sdhi_demo_cardinit_msg) - 1U));
    (void)ra_board_led_toggle(k_ra_board_led2);
    sdhi_demo_panic_halt();
  }
  uint32_t blocks = 0U;
  if (ra_sdcard_get_capacity(&blocks) == k_ra_ok) {
    g_sdhi_blocks = blocks;
  }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  sdhi_demo_setup_or_halt();
  /* Clear PRIMASK so SysTick can dispatch and ra_delay_ms() uses the SysTick
   * path (board_sim does not advance DWT_CYCCNT). */
  ra_isr_globals_enable();

  sdhi_demo_bringup_or_report();

  uint8_t        ok   = 0U;
  const ra_err_t err  = sdhi_demo_roundtrip(&ok);
  const uint8_t  good = (err == k_ra_ok && ok != 0U) ? 1U : 0U;
  g_sdhi_ok           = (uint32_t)good;
  if (good != 0U) {
    g_sdhi_stage = (uint32_t)k_sdhi_stage_roundtrip;
    sdhi_demo_print(k_sdhi_demo_ok_msg, (uint32_t)(sizeof(k_sdhi_demo_ok_msg) - 1U));
  } else if (g_sdhi_stage < (uint32_t)k_sdhi_stage_mount) {
    sdhi_demo_print(k_sdhi_demo_mount_msg, (uint32_t)(sizeof(k_sdhi_demo_mount_msg) - 1U));
  } else {
    sdhi_demo_print(k_sdhi_demo_rw_msg, (uint32_t)(sizeof(k_sdhi_demo_rw_msg) - 1U));
  }

  while (1) {
    if (good != 0U) {
      (void)ra_board_led_toggle(k_ra_board_led1);
    } else {
      (void)ra_board_led_toggle(k_ra_board_led2);
    }
    ++g_sdhi_heartbeat;
    ra_delay_ms((uint32_t)k_sdhi_demo_period_ms);
  }
  sdhi_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
