/**
 * @file examples/ek_ra8d2/hw_pending/ereader_m33/ereader_m33.h
 * @brief Shared contract for the "render a held e-reader page on the M33" demo
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * This header pins the cross-core contract for the #150 power-saving model: the
 * Cortex-M85 (primary, "CPU0") does the one-time heavy work -- it owns the baked
 * book and could open / compile it at 1 GHz -- then hands the *reader* to the
 * Cortex-M33 (secondary, "CPU1") and PARKS. The slow core renders one held
 * e-reader page into a real RGB565 framebuffer in external SDRAM through the
 * production `ra8_gfx` text path, then HOLDS that page -- exactly the idle posture
 * an e-reader spends almost all its time in, now at a fraction of the power while
 * the M85 @ 1 GHz sleeps in WFI. "Power saving = drop to the slow core."
 *
 * Two shared regions both core images must agree on:
 *
 *   1. The progress mailbox (::erm33_mailbox_t) at a fixed on-chip SRAM address
 *      (::k_erm33_mailbox_addr). Each core is a separate compiled image with its
 *      own linker script, so a `static` global in one image is invisible to the
 *      other; the one name both images resolve identically is a hard-coded
 *      address. Per HUM Ch 58.1 "SRAM" Table 58.1 p 3527 the on-chip data SRAM is
 *      SRAM0..SRAM2 (512 KiB each) plus SRAM3 (128 KiB), spanning 0x22000000..
 *      0x221A0000. The M85 image owns SRAM0+SRAM1 (0x22000000..0x22100000) and
 *      the M33 image owns the top 64 KiB of SRAM3 (0x22190000..0x221A0000); the
 *      mailbox at 0x22100000 (the start of SRAM2) is left unclaimed by both, so
 *      it is backed by the same physical SRAM on both sides with no overlap.
 *
 *   2. The page framebuffer in external SDRAM (::k_erm33_sdram_base, 0x68000000).
 *      Unlike the mailbox the framebuffer is NOT at a fixed shared address: the
 *      M33 places it in its own image's `.sdram_bss` and PUBLISHES the base +
 *      geometry into the mailbox (`fb_base` / `fb_width` / ...), the same way the
 *      sibling `compile_on_m33` publishes its emitted blob's base + length. The
 *      M33 also publishes a CRC-32 it computed over those rendered pixels.
 *
 * Why the M33 CRCs its own framebuffer: on the ra8_emulator host emulator the two
 * cores share only the on-chip SRAM host buffer; each core's external-SDRAM
 * window is a separate mapping, so the parked M85 cannot read back the bytes the
 * M33 wrote at 0x68000000. The M33 therefore reads its own SDRAM framebuffer
 * back to fold a CRC-32 -- which is itself the proof that real pixels landed in
 * SDRAM -- and publishes that value through the shared mailbox. The M85 reports
 * the published CRC; the ra8_emulator gate asserts it against a golden. On silicon
 * the single physical SDRAM is shared, so an M85 re-read would match.
 *
 * Coherency: this app's `system_init.c` leaves the M85 data cache OFF, so a store
 * from one core is visible to the other once a `dsb` has drained the write
 * buffer; no cache clean / invalidate dance is needed. The mailbox fields are
 * `volatile` so the compiler emits a real load / store on every access.
 *
 * Protocol (the reader handoff + the held-page handshake + the #150 mode-switch):
 *   1. M85 zeros the mailbox, stamps ::k_erm33_magic, `dsb`.
 *   2. M85 arms the IPC0 receive IRQ (its wake-from-WFI source), configures the
 *      LPM block, releases the M33, and waits for the first held page.
 *   3. M33 stamps ::k_erm33_m33_sig, validates the baked book, lays its opening
 *      page text out with `ra8_gfx_text_out`, renders it into the SDRAM RGB565
 *      framebuffer, folds a CRC-32 over those pixels, publishes `fb_base` /
 *      `fb_width` / `fb_height` / `fb_stride` / `fb_format` / `fb_crc` /
 *      `glyph_count`, sets `status = ok` and `done = 1`, then HOLDS the page.
 *   4. M85 waits for `done`, validates the descriptor, logs the page-0 verdict
 *      "ereader_m33 ... crc=<hex> PASS", then enters the MODE-SWITCH cycle.
 *   5. M85 PARKS: writes the CGC clock-gate (down-clocks an idle oscillator via
 *      the LPM clock-stop matrix) and drops into Sleep-mode WFI. The slow M33 is
 *      now the only running core, holding the page and polling a (simulated)
 *      touch input -- the e-reader's steady-state idle posture.
 *   6. On a page-turn touch the M33 bumps `turn_req` and POKES the M85 over IPC0
 *      (`ra8_ipc_send_event`), waking it from WFI -- the #149 wake mechanism.
 *   7. The woken M85 restores its clocks, does the "heavy" work (the next-page
 *      decision the 1 GHz core owns), acknowledges via `turn_ack`, and re-parks.
 *   8. The M33 observes the ack, RE-RENDERS the held page (re-folding the same
 *      deterministic CRC), publishes `turn_done`, and holds again. Steps 5..8
 *      repeat ::k_erm33_max_turns times, then both cores park for good.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum erm33_mailbox_addr_t
 * @brief Fixed shared-SRAM base address of the progress mailbox.
 *
 * @details The start of data-SRAM bank SRAM2 (0x22100000), the first word above
 * the M85 image's 1 MiB SRAM0+SRAM1 allocation and well below the M33 image's
 * SRAM3 block at 0x22190000, so both linker scripts leave it free. Declared
 * `uintptr_t` so the same constant casts to a pointer correctly on the 32-bit
 * target and the 64-bit unit-test host.
 *
 * @invariant The address is 16-byte aligned (cache-line safe).
 * @see erm33_mailbox()
 * @since 0.1.0
 */
/* HUM Ch 58.1 "SRAM" Table 58.1 p 3527 -- SRAM2 (data bank 2) begins at 0x22100000 */
typedef enum : uintptr_t {
  k_erm33_mailbox_addr = 0x22100000U, /**< Shared mailbox base (SRAM2 start). */
} erm33_mailbox_addr_t;

/**
 * @enum erm33_sdram_addr_t
 * @brief Base + extent of the external SDRAM window the held page lives in.
 *
 * @details The 64 MiB external SDRAM maps at 0x68000000. The M33 places its page
 * framebuffer in `.sdram_bss` near this base and publishes the exact address in
 * `fb_base`; the M85 only uses these bounds to sanity-check that the published
 * base lies inside the SDRAM window. Declared `uintptr_t` so the constants cast
 * to a pointer correctly on both the 32-bit target and the 64-bit unit-test host.
 *
 * @invariant `k_erm33_sdram_base < k_erm33_sdram_end`.
 * @since 0.1.0
 */
/* HUM Ch 5.1 "Address Space" Table 5.1 p 239 -- external SDRAM window at 0x68000000 */
typedef enum : uintptr_t {
  k_erm33_sdram_base = 0x68000000U, /**< External SDRAM window base.          */
  k_erm33_sdram_end  = 0x6C000000U, /**< External SDRAM window end (+64 MiB). */
} erm33_sdram_addr_t;

/**
 * @enum erm33_fb_geom_t
 * @brief Geometry of the RGB565 page framebuffer both images agree on.
 *
 * @details A small landscape page strip rendered through `ra8_gfx`: RGB565 (two
 * bytes per pixel, the EK-RA8D2 panel's native format), ::k_erm33_fb_width pixels
 * wide by ::k_erm33_fb_height tall. With the 8x16 `ra8_gfx` font that is
 * ::k_erm33_fb_cols glyph columns by ::k_erm33_fb_rows glyph rows, enough for the
 * ::k_erm33_page_chars characters one held page carries. Kept deliberately tiny
 * so the CRC-32 over the whole plane is fast and stable for the ra8_emulator gate.
 *
 * @invariant `k_erm33_fb_stride == k_erm33_fb_width * k_erm33_fb_bpp`.
 * @invariant `k_erm33_fb_bytes  == k_erm33_fb_stride * k_erm33_fb_height`.
 * @invariant `k_erm33_fb_cols * k_erm33_fb_rows == k_erm33_page_chars`.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_erm33_fb_width  = 256U,   /**< Plane width in pixels.                       */
  k_erm33_fb_height = 64U,    /**< Plane height in pixels (4 glyph rows of 16). */
  k_erm33_fb_bpp    = 2U,     /**< Bytes per pixel (RGB565).                    */
  k_erm33_fb_stride = 512U,   /**< Bytes per pixel row (width * bpp).           */
  k_erm33_fb_bytes  = 32768U, /**< Total plane size in bytes (stride * height). */
  k_erm33_fb_cols   = 32U,    /**< Glyph columns (width / 8-px glyph).          */
  k_erm33_fb_rows   = 4U,     /**< Glyph rows (height / 16-px glyph).           */
} erm33_fb_geom_t;

/**
 * @enum erm33_const_t
 * @brief Signatures, status codes, the RGB565 format tag, and the page width
 *        shared by both core images.
 *
 * @details ::k_erm33_magic ("ERM3") lets the M33 confirm the mailbox is live
 * before trusting it; ::k_erm33_m33_sig is the boot sentinel the M33 stamps so
 * the M85 can prove the second core left reset. ::k_erm33_fb_format_rgb565 is the
 * `ra8_gfx_format_t` value the M33 publishes in `fb_format` (cross-checked against
 * the real enum by a `static_assert` in the M33 image). ::k_erm33_page_chars is
 * the fixed character capacity of the held page.
 *
 * @see erm33_mailbox_t
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_erm33_magic              = 0x45524D33U, /**< "ERM3" -- M85 stamps it when ready.   */
  k_erm33_m33_sig            = 0x33524452U, /**< "RDR3" boot sentinel written by M33.  */
  k_erm33_status_running     = 0U,          /**< status: M33 is rendering the page.    */
  k_erm33_status_ok          = 1U,          /**< status: page rendered + published.    */
  k_erm33_status_bad_book    = 2U,          /**< status: baked book failed validation. */
  k_erm33_status_render_fail = 3U,          /**< status: ra8_gfx render path failed.   */
  k_erm33_fb_format_rgb565   = 2U,          /**< Published `fb_format` (RGB565).       */
  k_erm33_page_chars         = 128U,        /**< Characters the held page can carry.   */
} erm33_const_t;

/**
 * @enum erm33_cycle_t
 * @brief Bounds for the #150 park / wake / re-render mode-switch cycle.
 *
 * @details The M85 parks and the M33 holds the page; ::k_erm33_max_turns is the
 * deterministic number of page-turn handoffs the demo exercises before both
 * cores park for good. ::k_erm33_touch_dwell is the bounded count of hold-loop
 * iterations the M33 spins as a *simulated* touch latency between page turns --
 * a deterministic stand-in for polling the GT911 touch controller (the real
 * touch poll is a HIL follow-up, see the README). Kept small so the ra8_emulator
 * gate completes the whole cycle in a handful of instruction chunks. Both are
 * `uint32_t` so the M33 / M85 loop bounds are a single fixed type.
 *
 * @invariant `k_erm33_max_turns >= 1` (at least one handoff is exercised).
 * @see erm33_mailbox_t
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_erm33_max_turns   = 3U,    /**< Page-turn handoffs the cycle exercises.       */
  k_erm33_touch_dwell = 4096U, /**< M33 hold-loop spins per simulated touch wait. */
} erm33_cycle_t;

/**
 * @struct erm33_mailbox_t
 * @brief Cross-core progress block backed by a fixed shared-SRAM address.
 *
 * @details All fields are `volatile` so each core re-reads memory rather than
 * caching a value in a register. The M85 owns `magic` (writer); the M33 owns
 * every other field (writer). The M85 only reads what it does not own, and only
 * after observing `done == 1` behind a `dsb`.
 *
 * @invariant `magic` is 0 until the M85 stamps ::k_erm33_magic, then holds.
 * @invariant `done` is 0 until the M33 publishes the held page, then 1 forever.
 * @invariant On `status == ok`: `fb_base` is inside the SDRAM window, the
 *            geometry equals ::erm33_fb_geom_t, and `fb_crc` / `glyph_count`
 *            are non-zero.
 * @invariant Cycle handshake is monotone: `turn_ack <= turn_req` and
 *            `turn_done <= turn_ack`, each rising 0..::k_erm33_max_turns.
 *
 * @par Example:
 * @code
 * volatile erm33_mailbox_t* mb = erm33_mailbox();
 * while (mb->done == 0U) {}        // wait for the M33 to render the held page
 * uint32_t crc = mb->fb_crc;       // CRC-32 of the SDRAM framebuffer pixels
 * @endcode
 *
 * @see erm33_mailbox()
 * @since 0.1.0
 */
typedef struct {
  volatile uint32_t magic;       /**< M85 stamps ::k_erm33_magic when ready.          */
  volatile uint32_t m33_sig;     /**< M33 stamps ::k_erm33_m33_sig on boot.           */
  volatile uint32_t status;      /**< Render status (::erm33_const_t status codes).   */
  volatile uint32_t fb_base;     /**< M33-published SDRAM framebuffer base address.   */
  volatile uint32_t fb_width;    /**< M33-published framebuffer width in pixels.      */
  volatile uint32_t fb_height;   /**< M33-published framebuffer height in pixels.     */
  volatile uint32_t fb_stride;   /**< M33-published framebuffer stride in bytes.      */
  volatile uint32_t fb_format;   /**< M33-published `ra8_gfx_format_t` (RGB565).      */
  volatile uint32_t fb_crc;      /**< M33-folded CRC-32 over the rendered pixels.     */
  volatile uint32_t glyph_count; /**< Characters the M33 laid onto the held page.     */
  volatile uint32_t done;        /**< Set to 1 by the M33 once the page is published. */
  volatile uint32_t turn_req;    /**< M33->M85: page-turn request, bumped per touch.  */
  volatile uint32_t turn_ack;    /**< M85->M33: heavy-work ack for the matching turn. */
  volatile uint32_t turn_done;   /**< M33->M85: re-render published for that turn.    */
} erm33_mailbox_t;

/**
 * @brief Typed pointer to the fixed-address shared progress mailbox.
 *
 * @details Inlined so both core images compute the identical address with no
 * shared translation unit. Returns the same physical SRAM location on the M85
 * and the M33.
 *
 * @return Pointer to the mailbox at ::k_erm33_mailbox_addr.
 * @retval non-NULL Always; the address is a compile-time constant.
 *
 * @pre The linker scripts of both images leave the mailbox word unallocated.
 * @pre The M85 data cache is disabled (see file header).
 * @post Returns a valid `volatile` pointer; never NULL.
 * @post No side effects.
 *
 * @note Callable from either core; the pointer arithmetic is identical.
 * @since 0.1.0
 */
static inline volatile erm33_mailbox_t* erm33_mailbox(void)
{
  return (volatile erm33_mailbox_t*)(uintptr_t)k_erm33_mailbox_addr;
}

#ifdef __cplusplus
}
#endif
