/**
 * @file examples/ek_ra8d2/hw_validated/hil/drw_dlist_demo/main.c
 * @brief DRW display-list (DLR) clear+fill demo -- loop-stable (EK-RA8D2, #247)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The D/AVE 2D engine's DISPLAY-LIST route: a list built once in SRAM clears a
 * 32x32 ARGB8888 framebuffer to 0x00000000 and fills a 16x16 green box at
 * (8,8), and the display-list reader (DLR) executes it end to end when kicked
 * via DLISTSTART. The CPU never writes the framebuffer inside the loop, so
 * there is no CPU/engine write race to latch STATUS.BUSERRMFB -- the
 * loop-stable path that closes issue #247.
 *
 * Once a second the loop kicks the list, waits for the DLR to go idle, hashes
 * the whole framebuffer FNV-1a-32, and prints
 * ``"drw: dlist crc=E6B215C5 PASS\r\n"`` (or ``FAIL`` with the observed hash) on
 * the J-Link OB CDC channel. LED1 toggles on a match, LED2 on a mismatch. The
 * golden ``0xE6B215C5`` is a real EK-RA8D2 capture: a 16x16 green box
 * (``0xFF00FF00``) at (8,8) on a zero background, byte-identical to the
 * register-mode ::ra8_drw_fill_rect result and reproduced by ``tools/ra8_emulator``
 * (its DLR model executes the same list), so this app is SIM == HIL.
 *
 * The framebuffer is poisoned once up front so the very first display-list
 * clear must zero it; every later pass re-renders from the DRW alone.
 *
 * Bare EK-RA8D2 only -- no shields or external transceivers. The DRW FB cache
 * is left off (``enable_caches = false``) so the CPU reads the freshly
 * rasterized pixels directly.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_check.h"
#include "ra8_drw.h"
#include "ra8_drw_dlist.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_time.h"

/** @brief Diagnostic / log tag. */
static const char* s_tag = "drw_dlist";

/** @brief Compile-time settings. */
typedef enum : uint32_t {
  k_demo_baud        = 115200U,     /**< SCI console baud.                */
  k_demo_period_ms   = 1000U,       /**< Delay between display-list runs. */
  k_demo_fill_argb   = 0xFF00FF00U, /**< Opaque green box colour.         */
  k_demo_poison      = 0xDEADBEEFU, /**< Pre-clear framebuffer poison.    */
  k_demo_idle_budget = 500000U,     /**< Bounded DLR-idle poll budget.    */
  k_demo_golden_crc  = 0xE6B215C5U, /**< Bench FNV-1a-32 of the result.   */
  k_demo_fnv_offset  = 2166136261U, /**< FNV-1a-32 offset basis.          */
  k_demo_fnv_prime   = 16777619U,   /**< FNV-1a-32 prime.                 */
} demo_config_t;

/** @brief Framebuffer + rectangle geometry (pixels). */
typedef enum : uint16_t {
  k_demo_fb_dim = 32U, /**< 32 x 32 framebuffer.          */
  k_demo_box_xy = 8U,  /**< Box top-left (8, 8).          */
  k_demo_box_wh = 16U, /**< Box 16 x 16 -> covers [8,24). */
} demo_geom_t;

/** @brief Banner-formatting constants. */
typedef enum : uint32_t {
  k_demo_hex_digits  = 8U,    /**< Hex digits in a 32-bit CRC. */
  k_demo_nibble_bits = 4U,    /**< Bits per hex digit.         */
  k_demo_nibble_mask = 0x0FU, /**< Low nibble mask.            */
  k_demo_dlist_words = 32U,   /**< Display-list buffer words.  */
} demo_fmt_t;

/** @brief Fixed banner text around the formatted CRC. */
static const uint8_t k_demo_prefix[]  = "drw: dlist crc=";
static const uint8_t k_demo_ok_msg[]  = " PASS\r\n";
static const uint8_t k_demo_bad_msg[] = " FAIL\r\n";
static const uint8_t k_demo_hex_lut[] = "0123456789ABCDEF";

/**
 * @var g_dlist_fb
 * @brief 32 x 32 ARGB8888 framebuffer the display list rasterizes into.
 * @note 4-byte aligned (ARGB8888 invariant). Read externally (HIL).
 * @since 0.1.0
 */
[[gnu::aligned(4)]] static uint32_t g_dlist_fb[(uint32_t)k_demo_fb_dim * (uint32_t)k_demo_fb_dim];

/**
 * @var g_dlist_buf
 * @brief SRAM display-list buffer the DLR fetches. Built once at start-up.
 * @note 4-byte aligned; in SRAM so the DRW bus initiator can read it.
 * @since 0.1.0
 */
[[gnu::aligned(4)]] static uint32_t g_dlist_buf[(uint32_t)k_demo_dlist_words];

/**
 * @var g_dlist_crc
 * @brief FNV-1a-32 of the framebuffer after the most recent display-list run.
 * @note Read externally only (HIL / board emulator).
 * @since 0.1.0
 */
volatile uint32_t g_dlist_crc = 0U;

/**
 * @var g_dlist_match
 * @brief 1 when ::g_dlist_crc equals the golden hash.
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_dlist_match = 0U;

/**
 * @var g_dlist_rev
 * @brief DRW HWREVISION snapshot (0x0FBE0107 once the graphics domain is on).
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_dlist_rev = 0U;

/**
 * @var g_dlist_buserr
 * @brief OR of every STATUS bus-error bit ever observed (0 == clean loop).
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_dlist_buserr = 0U;

/**
 * @var g_dlist_heartbeat
 * @brief Bumps once per loop pass -- liveness for headless probes.
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_dlist_heartbeat = 0U;

/** @brief Park forever after a fatal init failure. */
static void demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief Bring CGC + SysTick + SCI console + LEDs + MSTP up. */
static void demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    demo_panic_halt();
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    demo_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_demo_baud) != k_ra8_ok) {
    demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led2) != k_ra8_ok) {
    demo_panic_halt();
  }
}

/** @brief Configure the DRW pointed at ``g_dlist_fb`` and snapshot HWREVISION. */
[[nodiscard]] static ra8_err_t demo_configure(void)
{
  const ra8_drw_config_t cfg = {
    .framebuffer_addr       = (uintptr_t)g_dlist_fb,
    .pitch_px               = (uint16_t)k_demo_fb_dim,
    .format                 = k_ra8_drw_writefmt_argb8888,
    .enable_caches          = false,
    .enable_buffered_writes = false,
  };
  const ra8_err_t err = ra8_drw_init(&cfg);
  if (err != k_ra8_ok) {
    return err;
  }
  uint32_t rev = 0U;
  if (ra8_drw_get_hwrevision(&rev) == k_ra8_ok) {
    g_dlist_rev = rev;
  }
  return k_ra8_ok;
}

/**
 * @brief Build the clear+fill display list into ``g_dlist_buf``.
 *
 * @param[out] out_dl Builder to leave bound and terminated for the run loop.
 *
 * @par MC/DC:
 * Each ``err != k_ra8_ok`` guard is a single decision -- no compound condition.
 *
 * @return ``ra8_err_t`` from the first failing builder call, else ``k_ra8_ok``.
 * @pre ::demo_configure succeeded.
 * @post ``*out_dl`` is terminated and runnable on success.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t demo_build_dlist(ra8_drw_dlist_t* out_dl)
{
  RA8_CHECK_NULL_PTR(out_dl, s_tag, "out_dl must not be nullptr");
  ra8_err_t err = ra8_drw_dlist_begin(out_dl, g_dlist_buf, (uint32_t)k_demo_dlist_words);
  if (err != k_ra8_ok) {
    return err;
  }
  const ra8_drw_rect_t clear = {
    .x              = 0,
    .y              = 0,
    .width_px       = (uint16_t)k_demo_fb_dim,
    .height_px      = (uint16_t)k_demo_fb_dim,
    .color_argb8888 = 0x00000000U,
  };
  const ra8_drw_rect_t box = {
    .x              = (int16_t)k_demo_box_xy,
    .y              = (int16_t)k_demo_box_xy,
    .width_px       = (uint16_t)k_demo_box_wh,
    .height_px      = (uint16_t)k_demo_box_wh,
    .color_argb8888 = (uint32_t)k_demo_fill_argb,
  };
  err = ra8_drw_dlist_add_fill(out_dl, &clear);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_drw_dlist_add_fill(out_dl, &box);
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_drw_dlist_end(out_dl);
}

/** @brief FNV-1a-32 over the whole framebuffer, byte by byte. */
static uint32_t demo_fb_hash(void)
{
  const uint8_t* bytes = (const uint8_t*)g_dlist_fb;
  uint32_t       hash  = (uint32_t)k_demo_fnv_offset;
  for (uint32_t i = 0U; i < sizeof(g_dlist_fb); ++i) {
    hash ^= (uint32_t)bytes[i];
    hash *= (uint32_t)k_demo_fnv_prime;
  }
  return hash;
}

/** @brief Format ``crc`` as 8 upper-case hex digits into ``out[8]``. */
static void demo_fmt_hex(uint32_t crc, uint8_t* out)
{
  for (uint32_t i = 0U; i < (uint32_t)k_demo_hex_digits; ++i) {
    const uint32_t shift = ((uint32_t)k_demo_hex_digits - 1U - i) * (uint32_t)k_demo_nibble_bits;
    out[i]               = k_demo_hex_lut[(crc >> shift) & (uint32_t)k_demo_nibble_mask];
  }
}

/** @brief Print ``drw: dlist crc=XXXXXXXX PASS|FAIL`` for the last render. */
static void demo_report(uint32_t crc, bool match)
{
  uint8_t hex[(uint32_t)k_demo_hex_digits];
  demo_fmt_hex(crc, hex);
  (void)ra8_board_uart_console_write(k_demo_prefix, (size_t)(sizeof(k_demo_prefix) - 1U));
  (void)ra8_board_uart_console_write(hex, (size_t)k_demo_hex_digits);
  if (match) {
    (void)ra8_board_uart_console_write(k_demo_ok_msg, (size_t)(sizeof(k_demo_ok_msg) - 1U));
    (void)ra8_board_led_toggle(k_ra8_board_led1);
  } else {
    (void)ra8_board_uart_console_write(k_demo_bad_msg, (size_t)(sizeof(k_demo_bad_msg) - 1U));
    (void)ra8_board_led_toggle(k_ra8_board_led2);
  }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  demo_setup_or_halt();
  /* Clear PRIMASK so SysTick can dispatch ra8_delay_ms(). No NVIC sources are
   * armed by this demo. */
  ra8_isr_globals_enable();

  if (demo_configure() != k_ra8_ok) {
    demo_panic_halt();
  }

  ra8_drw_dlist_t dl;
  if (demo_build_dlist(&dl) != k_ra8_ok) {
    demo_panic_halt();
  }

  /* Poison the framebuffer ONCE so the first display-list clear must zero it;
   * the DRW owns the framebuffer from here on. */
  for (uint32_t i = 0U; i < (uint32_t)k_demo_fb_dim * (uint32_t)k_demo_fb_dim; ++i) {
    g_dlist_fb[i] = (uint32_t)k_demo_poison;
  }

  while (1) {
    if (ra8_drw_dlist_run(&dl) == k_ra8_ok) {
      (void)ra8_drw_wait_idle((uint32_t)k_demo_idle_budget);
    }
    uint32_t status = 0U;
    if (ra8_drw_get_status(&status) == k_ra8_ok) {
      g_dlist_buserr |= (status & (uint32_t)k_ra8_drw_status_buserr_mask);
    }
    const uint32_t crc   = demo_fb_hash();
    const bool     match = (crc == (uint32_t)k_demo_golden_crc);
    g_dlist_crc          = crc;
    g_dlist_match        = match ? 1U : 0U;
    demo_report(crc, match);
    ++g_dlist_heartbeat;
    ra8_delay_ms((uint32_t)k_demo_period_ms);
  }
  demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
