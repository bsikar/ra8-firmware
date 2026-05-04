/**
 * @file examples/ek_ra8d2/ereader/main.c
 * @brief Skeleton ePub e-reader application for the EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S} -- application-layer code that runs in the
 * Secure world.
 *
 * @details
 * / Sweep 15 / Phase 6: end-to-end skeleton tying ThreadX,
 * FileX, GUIX, ``libs/ra_epub`` and (when available)
 * ``libs/ra_reflow`` together into a working e-reader running on the
 * EK-RA8D2's on-board parallel TFT panel. The E-Ink panel + custom
 * board are deferred to Phase 6.2.1.
 *
 * ## Bring-up sequence
 *
 *   1. ``ra_cgc_init`` brings PLLs / dividers up to the canonical
 *      RA8D2 1 GHz CPUCLK0 / 100 MHz PCLKA layout.
 *   2. ``ra_pfs_route_peripheral`` routes the GLCDC (TFT panel)
 *      pins, the SCI8 J-Link OB CDC pins, and the SDHI SD-card pins.
 *   3. ``ra_sci_init`` brings SCI8 up at 115200 8N1 for the diagnostic
 *      console (``[ereader] ...`` lines).
 *   4. ``ra_gpio_output_init`` configures LED1 (P6_00) -- toggled per
 *      page render -- and LED2 (P3_03) -- lit while a book is loaded.
 *   5. ``tx_kernel_enter`` hands control to ThreadX.
 *
 * ## Thread architecture
 *
 *   - **library_thread** (priority 5): walks the SD card via FileX,
 *     reads ``/books/`` `*.epub` entries, opens each with
 *     ``ra_epub_open``, pulls Dublin Core metadata via
 *     ``ra_epub_get_metadata`` and fills an in-memory book list. Once
 *     the catalogue is built it posts ``k_ereader_msg_library_ready``
 *     to the UI queue.
 *   - **ui_thread** (priority 4, highest of the three): drives GUIX.
 *     Two windows live on the same canvas:
 *       * "Library View" -- a vertical list of book titles + authors.
 *         A SW1 press moves the highlight; a SW2 press selects the
 *         highlighted book and the UI sends
 *         ``k_ereader_msg_load_book`` to ``reader_thread``.
 *       * "Reader View" -- shows the current page. Left/right arrow
 *         widgets are simulated by SW1 / SW2 in reader mode (page-
 *         prev / page-next).
 *   - **reader_thread** (priority 6): on receipt of a "load this
 *     book" message, calls ``ra_epub_open`` against the SD-resident
 *     ``.epub`` blob, ``ra_epub_load_chapter(0)``, then -- when
 *     ``RA_HAS_REFLOW`` is defined -- calls
 *     ``ra_reflow_layout_chapter`` and ``ra_reflow_render_page``.
 *     Without reflow the thread paints a fallback "Hello world"
 *     prompt on the canvas so the demo still flashes.
 *
 * ## Message-passing pattern
 *
 * Two ThreadX queues couple the threads:
 *
 *   - ``s_ui_queue``       (ui_thread receives): library_thread,
 *                          reader_thread, and the SW1/SW2 ICU IRQs
 *                          all post ``ereader_msg_t`` records here.
 *   - ``s_reader_queue``   (reader_thread receives): ui_thread posts
 *                          ``k_ereader_msg_load_book`` /
 *                          ``k_ereader_msg_page_prev`` /
 *                          ``k_ereader_msg_page_next`` here.
 *
 * The message body is a single ``ULONG`` (a packed enum + payload)
 * because GUIX widgets and ``ra_epub`` indices fit comfortably in 32
 * bits and ThreadX's smallest queue word is one ULONG.
 *
 * ## Framebuffer flow
 *
 * ``s_framebuffer`` lives in ``.bss`` (SRAM, 64-byte aligned). GUIX
 * and ra_reflow both write into it directly. The GLCDC (when wired
 * to the on-board panel by Phase 6.2.1) scans it out as RGB565. For
 * this skeleton the GLCDC is left disabled -- writing the buffer
 * still exercises the GUIX pipeline, and a follow-up flash on real
 * hardware can light the panel by adding ``ra_glcdc_init`` /
 * ``ra_glcdc_start`` once the EK board manual entries land.
 *
 * ## Hardware-accelerated rendering
 *
 * Sweep 17 routes the page-render hot-path
 * (``ra_gfx_blit`` / ``ra_gfx_rect`` / ``ra_gfx_text_out``) through
 * ``ra_drw`` (Renesas D/AVE 2D). The CMakeLists.txt for this app
 * defines ``RA_GFX_USE_DRW=1``; ``libs/ra_gfx/src/ra_gfx_text.c``
 * picks that up at compile time and replaces the software pixel
 * pusher's rect / blit fast paths with D/AVE 2D commands. The public
 * ra_gfx API is unchanged, so no call site in this file needed
 * modification.
 *
 * Touch input is intentionally deferred: the EK-RA8D2 LCD-connector
 * touch IC is not wired to a chip-side controller in the v1 board.
 * The README documents the SW1 / SW2 fallback wiring instead.
 *
 * @author Brighton Sikarskie
 * @date 2026-04-29
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_gpio_constants.h"
#include "ra_isr.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_sci.h"
#include "ra_time.h"

/*
 * The host unit-test build (RA_SIMULATOR_MODE) does not link the
 * ThreadX, FileX or GUIX vendor trees, so guard those headers in the
 * same way the other ThreadX-aware examples do.
 */
#ifndef RA_SIMULATOR_MODE
#include "fx_api.h"
#include "fx_media_driver_ra_sdhi.h"
#include "gx_api.h"
#include "gx_display.h"
#include "gx_display_driver_ra_glcdc.h"
#include "ra_epub.h"
#include "tx_api.h"
#endif

#ifdef RA_HAS_REFLOW
#include "ra_reflow.h"
#endif

#ifndef RA_SIMULATOR_MODE
#include "power.h"
#endif

/* =============================================================================
 * Compile-time configuration -- typed enums per the no-magic-number rule.
 * =============================================================================
 */

/**
 * @brief Common scalar configuration shared by every component below.
 *
 * @details
 * The values are the same baud / SCI channel as ``uart_hello`` and
 * ``threadx_filex_demo`` so a single SEGGER-CDC terminal session
 * spans the whole sweep. Stack sizes are sized small but generous --
 * the deepest chain (FileX + ra_epub + miniz) needs a little under
 * 4 KiB.
 */
typedef enum : uint32_t {
  k_ereader_baud           = 115200U, /**< SCI8 console baud rate. */
  k_ereader_sci_channel    = 8U,      /**< SCI channel number.     */
  k_ereader_thread_stack   = 6144U,   /**< Per-thread stack bytes. */
  k_ereader_media_buf_size = 512U,    /**< FileX scratch buffer.   */
} ereader_config_t;

/**
 * @brief Framebuffer dimensions. Chosen to match the threadx_guix_demo
 *        smoke test so the GUIX+GLCDC shim does not have to be
 *        re-validated for this app.
 */
typedef enum : uint16_t {
  k_ereader_fb_width  = 256U, /**< RGB565 framebuffer width.  */
  k_ereader_fb_height = 128U, /**< RGB565 framebuffer height. */
} ereader_fb_dim_t;

/**
 * @brief Bytes per RGB565 pixel.
 */
typedef enum : uint8_t {
  k_ereader_bpp_rgb565 = 2U, /**< 2 bytes per RGB565 pixel. */
} ereader_bpp_t;

/**
 * @brief Memory-pool / alignment constants.
 */
typedef enum : uint32_t {
  k_ereader_gx_pool_bytes = 16384U, /**< GUIX bump-pool size.        */
  k_ereader_fb_align      = 64U,    /**< AXI-burst alignment (bytes).*/
  k_ereader_pool_align    = 8U,     /**< GUIX-pool alignment (bytes).*/
} ereader_pool_t;

/**
 * @brief Thread priorities. Lower number == higher priority.
 *
 * @details
 * UI gets the highest priority so it can repaint promptly. The
 * library walker runs once at startup, so priority is mostly
 * irrelevant. The reader does the heavy text layout work and
 * deliberately runs lower than the UI.
 */
typedef enum : uint8_t {
  k_ereader_priority_ui      = 4U, /**< ui_thread priority.       */
  k_ereader_priority_library = 5U, /**< library_thread priority.  */
  k_ereader_priority_reader  = 6U, /**< reader_thread priority.   */
} ereader_priority_t;

/**
 * @brief Static caps for the in-memory book catalogue.
 */
typedef enum : uint8_t {
  k_ereader_max_books     = 16U, /**< Most books we will discover.   */
  k_ereader_book_name_len = 96U, /**< Bytes of title we cache.       */
} ereader_lib_caps_t;

/**
 * @brief Per-book scratch buffer caps. ``ra_epub`` reads everything
 *        out of an in-memory blob, so we have to budget enough RAM
 *        to load the largest book we plan to handle. Skeletons keep
 *        this small until ePubs are demoed against real hardware.
 */
typedef enum : uint32_t {
  k_ereader_zip_blob_bytes = 256U * 1024U, /**< 256 KiB max book size.    */
  k_ereader_chapter_bytes  = 32U * 1024U,  /**< 32 KiB per-chapter scratch.*/
} ereader_scratch_caps_t;

/**
 * @brief Inter-thread message identifiers (packed into a ULONG).
 *
 * @details
 * The wire format is a single ``ULONG`` consisting of:
 *
 *   - bits [7..0]  : message kind (``ereader_msg_kind_t``).
 *   - bits [31..8] : payload (book index, chapter index, etc.).
 */
typedef enum : uint8_t {
  k_ereader_msg_none          = 0U, /**< Unused slot.                   */
  k_ereader_msg_library_ready = 1U, /**< library_thread -> ui_thread.   */
  k_ereader_msg_load_book     = 2U, /**< ui_thread -> reader_thread.    */
  k_ereader_msg_page_prev     = 3U, /**< ui_thread -> reader_thread.    */
  k_ereader_msg_page_next     = 4U, /**< ui_thread -> reader_thread.    */
  k_ereader_msg_rendered      = 5U, /**< reader_thread -> ui_thread.    */
  k_ereader_msg_sw1           = 6U, /**< SW1 IRQ -> ui_thread.          */
  k_ereader_msg_sw2           = 7U, /**< SW2 IRQ -> ui_thread.          */
} ereader_msg_kind_t;

/**
 * @brief Queue message capacity (in ULONGs). TX_1_ULONG = 1.
 */
typedef enum : uint8_t {
  k_ereader_queue_msg_size  = 1U, /**< 1 ULONG per message (matches TX_1_ULONG). */
  k_ereader_queue_msg_count = 8U, /**< Slots per queue.                         */
} ereader_queue_caps_t;

/* =============================================================================
 * Pin routing tables
 * =============================================================================
 */

/**
 * @brief SCI8 J-Link OB CDC pin pair (PD_02 = TXD, PD_03 = RXD).
 *
 * @details
 * Same wiring as the ``uart_hello`` and ``threadx_filex_demo``
 * examples; reused verbatim so the diagnostic console line stays
 * consistent across the test matrix.
 */
static const ra_port_pin_t k_ereader_pin_txd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);
static const ra_port_pin_t k_ereader_pin_rxd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);

/**
 * @brief Number of GLCDC pins to route (24 data + HSYNC + VSYNC + DE + CLK).
 */
typedef enum : uint8_t {
  k_ereader_glcdc_pin_count = 28U,
} ereader_glcdc_pin_count_t;

/**
 * @brief GLCDC pin list. Each pin is set to ``k_ra_psel_glcdc``.
 *
 * @details
 * Mirrors the table in ``examples/lcd_demo/main.c``. The EK-RA8D2 v1
 * board manual is still TODO; the pads below are the GLCDC-capable
 * pins on the 289-BGA per datasheet R01DS0493EJ "Pin Functions"
 * section "GLCDC" but they have not yet been confirmed against the
 * board-side connector wiring.
 */
#define EREADER_PP(port, pin) ((ra_port_pin_t)(((uint16_t)(port) << 8) | (uint16_t)(pin)))

static const ra_port_pin_t k_ereader_glcdc_pins[k_ereader_glcdc_pin_count] = {
  /* LCDD0..LCDD7  -> Port 6. */
  EREADER_PP(6U, 0U),
  EREADER_PP(6U, 1U),
  EREADER_PP(6U, 2U),
  EREADER_PP(6U, 3U),
  EREADER_PP(6U, 4U),
  EREADER_PP(6U, 5U),
  EREADER_PP(6U, 6U),
  EREADER_PP(6U, 7U),
  /* LCDD8..LCDD15 -> Port 7. */
  EREADER_PP(7U, 0U),
  EREADER_PP(7U, 1U),
  EREADER_PP(7U, 2U),
  EREADER_PP(7U, 3U),
  EREADER_PP(7U, 4U),
  EREADER_PP(7U, 5U),
  EREADER_PP(7U, 6U),
  EREADER_PP(7U, 7U),
  /* LCDD16..LCDD23 -> Port 8. */
  EREADER_PP(8U, 0U),
  EREADER_PP(8U, 1U),
  EREADER_PP(8U, 2U),
  EREADER_PP(8U, 3U),
  EREADER_PP(8U, 4U),
  EREADER_PP(8U, 5U),
  EREADER_PP(8U, 6U),
  EREADER_PP(8U, 7U),
  /* HSYNC / VSYNC / DE / CLK -> Port 9. */
  EREADER_PP(9U, 0U),
  EREADER_PP(9U, 1U),
  EREADER_PP(9U, 2U),
  EREADER_PP(9U, 3U),
};

/**
 * @brief Number of SDHI pins to route (CMD + CLK + DAT0..3 + WP + CD).
 */
typedef enum : uint8_t {
  k_ereader_sdhi_pin_count = 8U,
} ereader_sdhi_pin_count_t;

/**
 * @brief SDHI pin list. Each pin is set to ``k_ra_psel_sdhi``.
 *
 * @details
 * RA8D2 datasheet R01DS0493EJ "Pin Functions" -> "SDHI" lists the
 * default mapping at port 4. Same caveat as the GLCDC table -- the
 * board-side wiring should be confirmed against EK-RA8D2 v1 docs
 * once they are committed to ``docs/reference/``.
 */
static const ra_port_pin_t k_ereader_sdhi_pins[k_ereader_sdhi_pin_count] = {
  EREADER_PP(4U, 0U), /* SD_CMD  */
  EREADER_PP(4U, 1U), /* SD_CLK  */
  EREADER_PP(4U, 2U), /* SD_DAT0 */
  EREADER_PP(4U, 3U), /* SD_DAT1 */
  EREADER_PP(4U, 4U), /* SD_DAT2 */
  EREADER_PP(4U, 5U), /* SD_DAT3 */
  EREADER_PP(4U, 6U), /* SD_WP   */
  EREADER_PP(4U, 7U), /* SD_CD   */
};

/* =============================================================================
 * Cross-build state -- only present on the firmware target.
 * =============================================================================
 */

#ifndef RA_SIMULATOR_MODE

/**
 * @struct ereader_book_entry_t
 * @brief In-memory record for one ``.epub`` discovered on the SD card.
 *
 * @details
 * Populated by ``library_thread``. The reader thread later opens the
 * file by ``filename`` (relative to ``/books/``) when the user picks
 * it from the GUIX list.
 */
typedef struct {
  char    filename[k_ra_epub_max_path_len]; /**< Path under /books/.       */
  char    title[k_ereader_book_name_len];   /**< Dublin-Core title.        */
  char    author[k_ereader_book_name_len];  /**< Dublin-Core creator.      */
  uint8_t valid;                            /**< 1 if entry has been filled.*/
} ereader_book_entry_t;

/**
 * @var s_framebuffer
 * @brief Shared RGB565 framebuffer drawn by GUIX and (later) ra_reflow.
 */
static uint16_t s_framebuffer[k_ereader_fb_height][k_ereader_fb_width]
  __attribute__((aligned(k_ereader_fb_align)));

/**
 * @var s_gx_pool
 * @brief Backing memory for ``demo_gx_alloc``.
 */
static uint8_t s_gx_pool[k_ereader_gx_pool_bytes] __attribute__((aligned(8)));

/**
 * @var s_book_blob
 * @brief Caller-owned blob handed to ``ra_epub_open``.
 *
 * @details
 * 256 KiB of SRAM dedicated to holding one open EPUB. The skeleton
 * loads at most one book at a time; future Phase-6 work will move
 * this into SDRAM once the SDRAM controller has been brought up.
 */
static uint8_t s_book_blob[k_ereader_zip_blob_bytes] __attribute__((aligned(16)));

/**
 * @var s_chapter_buf
 * @brief Per-chapter scratch buffer for ``ra_epub_load_chapter``.
 */
static uint8_t s_chapter_buf[k_ereader_chapter_bytes] __attribute__((aligned(8)));

/* FileX state. */
static FX_MEDIA s_sd_media;
static FX_FILE  s_book_file;
static UCHAR    s_media_memory[k_ereader_media_buf_size];

/**
 * @var s_epub_scratch
 * @brief Single shared ``ra_epub_book_t`` slot.
 *
 * @details
 * The book handle is several KiB (the spine path table dominates),
 * so we keep one at file scope to avoid blowing per-thread stacks.
 * Both ``library_thread`` (during catalogue building) and
 * ``reader_thread`` (during page rendering) use it -- they never
 * run concurrently against the same book because the catalogue is
 * complete before any reader request is accepted.
 */
static ra_epub_book_t s_epub_scratch;

/* GUIX control blocks. Two windows share the same canvas and toggle
 * visibility when the user navigates between Library and Reader. */
static GX_DISPLAY     s_gx_display;
static GX_WINDOW_ROOT s_gx_root;
static GX_WINDOW      s_gx_library_win;
static GX_WINDOW      s_gx_reader_win;
static GX_PROMPT      s_gx_library_label;
static GX_PROMPT      s_gx_reader_label;
static GX_CANVAS      s_gx_canvas;

/* The two static prompt strings the skeleton paints. The library view
 * is rewritten each time the catalogue grows; the reader view falls
 * back to "Hello world" until ``ra_reflow_render_page`` is wired up
 * (sibling sweep). */
static GX_CHAR       s_library_text[k_ereader_book_name_len * k_ereader_max_books];
static const GX_CHAR s_reader_fallback_text[] = "Hello world (reflow pending)";

/* ThreadX threads + stacks. */
static TX_THREAD s_ui_thread_cb;
static TX_THREAD s_library_thread_cb;
static TX_THREAD s_reader_thread_cb;
static uint8_t   s_ui_stack[k_ereader_thread_stack] __attribute__((aligned(8)));
static uint8_t   s_library_stack[k_ereader_thread_stack] __attribute__((aligned(8)));
static uint8_t   s_reader_stack[k_ereader_thread_stack] __attribute__((aligned(8)));

/* Inter-thread queues. */
static TX_QUEUE s_ui_queue;
static TX_QUEUE s_reader_queue;
static ULONG    s_ui_queue_storage[k_ereader_queue_msg_count];
static ULONG    s_reader_queue_storage[k_ereader_queue_msg_count];

/**
 * @var s_books
 * @brief Catalogue built by ``library_thread``.
 *
 * @note Written exclusively by ``library_thread`` before posting
 *       ``k_ereader_msg_library_ready``; thereafter the reader/UI
 *       threads only read the table. Reads are atomic on Cortex-M85
 *       because the entries are small structs accessed one field at
 *       a time and writes complete before any messages are sent.
 */
static ereader_book_entry_t s_books[k_ereader_max_books];
static uint8_t              s_book_count;

/**
 * @var s_active_book
 * @brief Index into ``s_books`` of the book currently displayed.
 */
static volatile uint8_t s_active_book;

/**
 * @var s_active_page
 * @brief Page index within the currently rendered chapter.
 */
static volatile uint16_t s_active_page;

/* =============================================================================
 * SysTick override -- routes the 1 ms tick into ThreadX.
 * =============================================================================
 */

/* NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,readability-identifier-naming) */
extern void _tx_timer_interrupt(void);
/* NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,readability-identifier-naming) */

/**
 * @brief SysTick exception handler routed to ThreadX.
 *
 * @details
 * Each 1 ms SysTick fires ``_tx_timer_interrupt``, which advances
 * the kernel tick counter and pre-empts the running thread when a
 * higher-priority thread becomes ready.
 *
 * @pre Called from exception context (IPSR == 15).
 * @pre ``_tx_initialize_low_level`` has programmed SysTick.
 * @post One ThreadX tick has been credited.
 *
 * @note Not thread-safe; runs only from the SysTick exception.
 *
 * @since 0.1.0
 */
void SysTick_Handler(void);
void SysTick_Handler(void)
{
  _tx_timer_interrupt();
}

/* =============================================================================
 * Diagnostic helpers
 * =============================================================================
 */

/**
 * @brief Halt forever in WFI.
 *
 * @pre Called only after a fatal error in init.
 * @post CPU is parked.
 *
 * @note Not thread-safe; called from a single failing context.
 *
 * @since 0.1.0
 */
static void ereader_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Write a NUL-terminated string to SCI8.
 *
 * @param[in] s NUL-terminated ASCII; may be NULL (early exit).
 *
 * @pre SCI8 has been brought up by ``ereader_setup_or_halt``.
 * @post On success the bytes are queued in the SCI8 TX FIFO.
 *
 * @note Not thread-safe relative to ``ra_sci_write_polling``; we
 *       only call it from one thread at a time after init.
 *
 * @since 0.1.0
 */
static void ereader_print(const char* s)
{
  if (s == (const char*)0) {
    return;
  }
  uint32_t len = (uint32_t)strlen(s);
  (void)ra_sci_write_polling((uint8_t)k_ereader_sci_channel, (const uint8_t*)s, len);
}

/**
 * @brief Print a small unsigned integer in decimal to SCI8.
 *
 * @param[in] value Value to print, 0..65535.
 *
 * @pre SCI8 is up.
 * @post 1..5 ASCII digits are queued in the SCI8 TX FIFO.
 *
 * @note Not thread-safe; called only from logging paths.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ereader_dec_digits = 5U,
} ereader_dec_digits_t;

static void ereader_print_uint(uint16_t value)
{
  char    buf[k_ereader_dec_digits + 1U];
  uint8_t pos = (uint8_t)k_ereader_dec_digits;
  buf[pos]    = '\0';
  if (value == 0U) {
    pos--;
    buf[pos] = '0';
  } else {
    while ((value > 0U) && (pos > 0U)) {
      pos--;
      buf[pos] = (char)('0' + (char)(value % 10U));
      value    = (uint16_t)(value / 10U);
    }
  }
  ereader_print(&buf[pos]);
}

/* =============================================================================
 * Boot-time init helpers
 * =============================================================================
 */

/**
 * @brief Route SCI8 + GLCDC + SDHI pins.
 *
 * @retval k_ra_ok                       All pins routed.
 * @retval k_ra_err_gpio_invalid_port    PFS port out of range.
 * @retval k_ra_err_gpio_invalid_pin     PFS pin out of range.
 * @retval k_ra_err_gpio_conflict        Pin already claimed.
 *
 * @pre ``ra_cgc_init`` has been called (IOPORT clock is up).
 * @post On success every pin in the three tables is in its peripheral
 *       mode.
 *
 * @note Not thread-safe; called once during single-threaded init.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t ereader_pins_init(void)
{
  ra_err_t err = ra_pfs_route_peripheral(k_ereader_pin_txd, k_ra_psel_sci_async, "ereader.txd8");
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_pfs_route_peripheral(k_ereader_pin_rxd, k_ra_psel_sci_async, "ereader.rxd8");
  if (err != k_ra_ok) {
    return err;
  }
  for (uint8_t i = 0U; i < (uint8_t)k_ereader_glcdc_pin_count; i++) {
    err = ra_pfs_route_peripheral(k_ereader_glcdc_pins[i], k_ra_psel_glcdc, "ereader.glcdc");
    if (err != k_ra_ok) {
      return err;
    }
  }
  for (uint8_t i = 0U; i < (uint8_t)k_ereader_sdhi_pin_count; i++) {
    err = ra_pfs_route_peripheral(k_ereader_sdhi_pins[i], k_ra_psel_sdhi, "ereader.sdhi");
    if (err != k_ra_ok) {
      return err;
    }
  }
  return k_ra_ok;
}

/**
 * @brief Bring up CGC + time + SCI8 + LEDs. Halts on any failure.
 *
 * @pre Reset_Handler has run; SystemInit is complete.
 * @post On success SCI8 is sending at 115200 8N1; LEDs are output.
 *
 * @note Not thread-safe; runs from main() before tx_kernel_enter.
 *
 * @since 0.1.0
 */
static void ereader_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;

  if (ra_cgc_init() != k_ra_ok) {
    ereader_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    ereader_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz) != k_ra_ok) {
    ereader_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    ereader_panic_halt();
  }
  if (ereader_pins_init() != k_ra_ok) {
    ereader_panic_halt();
  }

  const ra_sci_cfg_t sci_cfg = {
    .baud      = k_ereader_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_ereader_sci_channel, &sci_cfg) != k_ra_ok) {
    ereader_panic_halt();
  }

  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    ereader_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led2) != k_ra_ok) {
    ereader_panic_halt();
  }
}

/* =============================================================================
 * GUIX memory allocator wrappers (bump-pool, no free)
 * =============================================================================
 */

/**
 * @brief Local malloc-on-pool implementation for GUIX.
 *
 * @details
 * GUIX wants two function pointers (alloc + free). NASA Power-of-10
 * Rule 3 forbids runtime malloc, so the "allocator" is a simple bump
 * pointer over ``s_gx_pool``. ``free`` is a no-op.
 *
 * @param[in] size Bytes requested.
 * @return Aligned pointer into the pool, or NULL on exhaustion.
 *
 * @pre ``size > 0``.
 * @post Returned pointer (when non-NULL) is 8-byte aligned.
 *
 * @note Not thread-safe; GUIX serialises all allocator calls from
 *       its own internal thread.
 *
 * @since 0.1.0
 */
static VOID* ereader_gx_alloc(ULONG size)
{
  static ULONG s_used = 0U;
  const ULONG  aligned =
    (size + (ULONG)(k_ereader_pool_align - 1U)) & ~(ULONG)(k_ereader_pool_align - 1U);
  if ((s_used + aligned) > (ULONG)k_ereader_gx_pool_bytes) {
    return GX_NULL;
  }
  VOID* ptr = &s_gx_pool[s_used];
  s_used += aligned;
  return ptr;
}

/**
 * @brief No-op free callback paired with ``ereader_gx_alloc``.
 *
 * @param[in] mem Block to free (ignored).
 *
 * @note Not thread-safe; only called from GUIX-internal teardown.
 *
 * @since 0.1.0
 */
static VOID ereader_gx_free(VOID* mem)
{
  (void)mem;
}

/* =============================================================================
 * Library thread
 * =============================================================================
 */

/**
 * @brief Append a discovered ePub entry to the catalogue.
 *
 * @param[in] filename SD-relative path (under /books/).
 * @param[in] title    Book title from Dublin Core; may be empty.
 * @param[in] author   Book author from Dublin Core; may be empty.
 *
 * @pre Called only from ``library_thread_entry`` while it owns the
 *      catalogue.
 * @post On success, ``s_book_count`` has incremented by one.
 *
 * @note Not thread-safe; only called by library_thread.
 *
 * @since 0.1.0
 */
static void ereader_catalogue_append(const char* filename, const char* title, const char* author)
{
  if (s_book_count >= (uint8_t)k_ereader_max_books) {
    return;
  }
  ereader_book_entry_t* slot = &s_books[s_book_count];
  (void)strncpy(slot->filename, filename, sizeof(slot->filename) - 1U);
  slot->filename[sizeof(slot->filename) - 1U] = '\0';
  (void)strncpy(slot->title, title, sizeof(slot->title) - 1U);
  slot->title[sizeof(slot->title) - 1U] = '\0';
  (void)strncpy(slot->author, author, sizeof(slot->author) - 1U);
  slot->author[sizeof(slot->author) - 1U] = '\0';
  slot->valid                             = 1U;
  s_book_count++;
}

/**
 * @brief Try to open ``s_book_blob`` as an EPUB and fill the catalogue.
 *
 * @param[in] filename SD-relative path (under /books/).
 * @param[in] blob_len Bytes loaded into ``s_book_blob``.
 *
 * @pre ``s_book_blob`` already populated with ``blob_len`` bytes.
 * @post On success, the catalogue has been appended to.
 *
 * @note Not thread-safe; only called by library_thread.
 *
 * @since 0.1.0
 */
static void ereader_catalogue_try_book(const char* filename, size_t blob_len)
{
  ra_epub_mem_media_t media = {.data = s_book_blob, .size = blob_len};

  if (ra_epub_open(&media, filename, &s_epub_scratch) != k_ra_ok) {
    return;
  }

  ra_epub_metadata_t meta;
  const char*        title  = "";
  const char*        author = "";
  if (ra_epub_get_metadata(&s_epub_scratch, &meta) == k_ra_ok) {
    title  = meta.title;
    author = meta.author;
  }
  ereader_catalogue_append(filename, title, author);
  (void)ra_epub_close(&s_epub_scratch);
}

/**
 * @brief library_thread entry: walk /books/ and build the catalogue.
 *
 * @param[in] thread_input Unused (ThreadX cookie).
 *
 * @pre fx_system_initialize() has been called (by tx_application_define).
 * @post ``s_books`` is populated, ``s_book_count`` is final, and
 *       ``k_ereader_msg_library_ready`` has been posted to
 *       ``s_ui_queue``.
 *
 * @note Not thread-safe; runs as a single-instance thread.
 *
 * @since 0.1.0
 */
static void library_thread_entry(ULONG thread_input)
{
  (void)thread_input;

  UINT status = fx_media_open(&s_sd_media,
                              (CHAR*)"SD",
                              fx_media_driver_ra_sdhi,
                              FX_NULL,
                              s_media_memory,
                              (ULONG)sizeof(s_media_memory));
  if (status != FX_SUCCESS) {
    ereader_print("[ereader] SD mount failed\r\n");
    /* Still post the ready message so the UI can show "no books". */
    ULONG msg = (ULONG)k_ereader_msg_library_ready;
    (void)tx_queue_send(&s_ui_queue, &msg, TX_NO_WAIT);
    while (1) {
      (void)tx_thread_sleep(1000U);
    }
  }

  /* Walk /books/ -- look for *.epub entries. We use the simple flat
   * iterator since the skeleton does not need to recurse. */
  CHAR  name[FX_MAX_LONG_NAME_LEN];
  UINT  attributes = 0U;
  ULONG size       = 0U;

  (void)fx_directory_default_set(&s_sd_media, "/books");
  status = fx_directory_first_full_entry_find(&s_sd_media,
                                              name,
                                              &attributes,
                                              &size,
                                              FX_NULL,
                                              FX_NULL,
                                              FX_NULL,
                                              FX_NULL,
                                              FX_NULL,
                                              FX_NULL);
  while (status == FX_SUCCESS) {
    /* Match suffix ".epub" (case-insensitive). */
    const size_t name_len = strlen(name);
    if ((name_len > 5U) && (size > 0UL) && (size <= (ULONG)k_ereader_zip_blob_bytes)) {
      const char* tail = &name[name_len - 5U];
      const bool  is_epub =
        ((tail[0] == '.') && ((tail[1] == 'e') || (tail[1] == 'E')) &&
         ((tail[2] == 'p') || (tail[2] == 'P')) && ((tail[3] == 'u') || (tail[3] == 'U')) &&
         ((tail[4] == 'b') || (tail[4] == 'B')));
      if (is_epub) {
        if (fx_file_open(&s_sd_media, &s_book_file, name, FX_OPEN_FOR_READ) == FX_SUCCESS) {
          ULONG actual = 0UL;
          if (fx_file_read(&s_book_file, s_book_blob, size, &actual) == FX_SUCCESS) {
            ereader_catalogue_try_book(name, (size_t)actual);
          }
          (void)fx_file_close(&s_book_file);
        }
      }
    }
    status = fx_directory_next_full_entry_find(&s_sd_media,
                                               name,
                                               &attributes,
                                               &size,
                                               FX_NULL,
                                               FX_NULL,
                                               FX_NULL,
                                               FX_NULL,
                                               FX_NULL,
                                               FX_NULL);
  }
  (void)fx_media_close(&s_sd_media);

  ereader_print("[ereader] loaded ");
  ereader_print_uint((uint16_t)s_book_count);
  ereader_print(" books from SD card\r\n");

  ULONG msg = (ULONG)k_ereader_msg_library_ready;
  (void)tx_queue_send(&s_ui_queue, &msg, TX_NO_WAIT);

  while (1) {
    (void)tx_thread_sleep(10000U);
  }
}

/* =============================================================================
 * Reader thread
 * =============================================================================
 */

/**
 * @brief Encode ``(kind, payload)`` into a single ULONG queue word.
 *
 * @details
 * The wire layout puts ``kind`` in bits [7..0] and ``payload`` in
 * bits [31..8]. ``payload`` is a book index, page index, or 0.
 *
 * @param[in] kind    Message identifier.
 * @param[in] payload Book / page index (0..0x00FFFFFF).
 *
 * @return Packed ULONG.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ereader_payload_shift = 8U,
} ereader_payload_layout_t;

static inline ULONG ereader_msg_pack(ereader_msg_kind_t kind, uint32_t payload)
{
  return (ULONG)((uint32_t)kind | (payload << (uint32_t)k_ereader_payload_shift));
}

/**
 * @brief Render the current chapter / page into ``s_framebuffer``.
 *
 * @param[in] book_idx Index into ``s_books``.
 * @param[in] page_idx Page within chapter 0 (skeleton only renders
 *                     chapter 0 -- chapter selection is Phase 6.2).
 *
 * @retval k_ra_ok Page has been rendered.
 *
 * @pre book_idx < s_book_count.
 *
 * @note Not thread-safe; called only from reader_thread.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t ereader_render_page(uint8_t book_idx, uint16_t page_idx)
{
  ereader_print("[ereader] rendering chapter 0 page ");
  ereader_print_uint(page_idx);
  ereader_print("\r\n");

  /* Open the EPUB blob currently sitting in s_book_blob. We rely on
   * the ui_thread / reader_thread to have re-loaded the bytes via
   * FileX before calling this -- the skeleton keeps the blob across
   * page renders so left/right arrow keys are cheap. */
  ra_epub_mem_media_t media = {.data = s_book_blob, .size = k_ereader_zip_blob_bytes};
  if (ra_epub_open(&media, s_books[book_idx].filename, &s_epub_scratch) != k_ra_ok) {
    /* Fallback: keep the previous framebuffer; the UI will paint the
     * "Hello world" prompt instead. */
    return k_ra_err_validation_failed;
  }

  size_t got = 0U;
  (void)ra_epub_load_chapter(&s_epub_scratch, 0U, s_chapter_buf, sizeof(s_chapter_buf), &got);

#ifdef RA_HAS_REFLOW
  /* Real text layout path -- sibling sweep produces ra_reflow.
   *
   * Phase 6.2 will wire this in fully (font blob from the EPUB,
   * ra_gfx_init against ``s_framebuffer`` first, etc.). The skeleton
   * here only validates the call shape: the engine struct is
   * static so it does not blow the per-thread stack. */
  static ra_reflow_t s_reflow_engine;
  uint32_t           total_pages = 0U;
  if (ra_reflow_layout_chapter(&s_reflow_engine, s_chapter_buf, got, &total_pages) == k_ra_ok) {
    (void)ra_reflow_render_page(&s_reflow_engine, (uint32_t)page_idx, &s_framebuffer[0][0]);
  }
#else
  /* Stub path -- the GUIX prompt already painted "Hello world", so
   * the framebuffer has something visible. Use ``got`` so the
   * compiler does not warn about an unused result. */
  (void)got;
  (void)page_idx;
#endif

  (void)ra_epub_close(&s_epub_scratch);
  return k_ra_ok;
}

/**
 * @brief reader_thread entry: react to load / page-prev / page-next.
 *
 * @param[in] thread_input Unused (ThreadX cookie).
 *
 * @pre tx_application_define has created the queues.
 * @post For each accepted message a ``k_ereader_msg_rendered``
 *       reply is queued back to the UI thread.
 *
 * @note Not thread-safe; runs as a single-instance thread.
 *
 * @since 0.1.0
 */
static void reader_thread_entry(ULONG thread_input)
{
  (void)thread_input;

  while (1) {
    ULONG msg = 0UL;
    if (tx_queue_receive(&s_reader_queue, &msg, TX_WAIT_FOREVER) != TX_SUCCESS) {
      continue;
    }
    const uint8_t  kind    = (uint8_t)(msg & 0xFFUL);
    const uint32_t payload = (uint32_t)(msg >> (uint32_t)k_ereader_payload_shift);

    if (kind == (uint8_t)k_ereader_msg_load_book) {
      s_active_book = (uint8_t)payload;
      s_active_page = 0U;
      (void)ra_board_led_on(k_ra_board_led2);
    } else if (kind == (uint8_t)k_ereader_msg_page_next) {
      s_active_page = (uint16_t)(s_active_page + 1U);
    } else if (kind == (uint8_t)k_ereader_msg_page_prev) {
      if (s_active_page > 0U) {
        s_active_page = (uint16_t)(s_active_page - 1U);
      }
    } else {
      continue;
    }

    if (s_active_book < s_book_count) {
      (void)ereader_render_page(s_active_book, s_active_page);
      /* LED1 toggles per page-render. */
      (void)ra_board_led_toggle(k_ra_board_led1);
    }

    ULONG ack = ereader_msg_pack(k_ereader_msg_rendered, (uint32_t)s_active_page);
    (void)tx_queue_send(&s_ui_queue, &ack, TX_NO_WAIT);

    /* Page-turn power flow: the reader thread is idle until the
     * next button press, so put the CPU into Sleep mode and let
     * the ICU SW1/SW2 IRQs pull us out. ``out_woken_irq`` is the
     * IRQ that woke us; we ignore the value here because the IRQ
     * handler in power.c also re-posts the message into s_ui_queue,
     * but a future power-aware scheduler can use the wake-source
     * to skip the GUIX redraw when not needed. */
    uint8_t        woke = (uint8_t)k_ereader_power_irq_none;
    const ra_err_t pwr  = ereader_power_sleep_until_button(&woke);
    if (pwr != k_ra_ok) {
      /* Sleep failed -- continue without it. */
      (void)pwr;
    }
    (void)woke;
  }
}

/* =============================================================================
 * UI thread
 * =============================================================================
 */

/**
 * @brief Build a multi-line string listing every book in the catalogue.
 *
 * @details
 * The result is written into ``s_library_text`` and then handed to
 * the GUIX prompt via ``gx_prompt_text_set_ext``. Each line is
 * "title -- author".
 *
 * @pre Called only from ui_thread after library_thread is done.
 * @post ``s_library_text`` ends with a NUL.
 *
 * @note Not thread-safe; ui_thread is the only writer.
 *
 * @since 0.1.0
 */
static void ereader_build_library_text(void)
{
  size_t pos = 0U;
  for (uint8_t i = 0U; i < s_book_count; i++) {
    const char*  title   = s_books[i].title[0] != '\0' ? s_books[i].title : s_books[i].filename;
    const char*  author  = s_books[i].author[0] != '\0' ? s_books[i].author : "(unknown)";
    const size_t cap     = sizeof(s_library_text) - pos - 1U;
    int          written = 0;
    /* strncat-style assemble; avoid snprintf (newlib pulls heavy code). */
    const size_t tlen = strlen(title);
    const size_t alen = strlen(author);
    if ((tlen + alen + 6U) > cap) {
      break;
    }
    (void)memcpy(&s_library_text[pos], title, tlen);
    pos += tlen;
    (void)memcpy(&s_library_text[pos], " -- ", 4U);
    pos += 4U;
    (void)memcpy(&s_library_text[pos], author, alen);
    pos += alen;
    s_library_text[pos++] = '\n';
    (void)written;
  }
  if (pos == 0U) {
    static const char* k_empty = "(no books found on /books)";
    const size_t       elen    = strlen(k_empty);
    if (elen < sizeof(s_library_text)) {
      (void)memcpy(s_library_text, k_empty, elen);
      pos = elen;
    }
  }
  s_library_text[pos] = '\0';
}

/**
 * @brief Build a GX_RECTANGLE inline.
 *
 * @param[in] left   Inclusive left edge.
 * @param[in] top    Inclusive top edge.
 * @param[in] right  Inclusive right edge.
 * @param[in] bottom Inclusive bottom edge.
 *
 * @return Initialised rectangle.
 *
 * @since 0.1.0
 */
static inline GX_RECTANGLE
ereader_rect(GX_VALUE left, GX_VALUE top, GX_VALUE right, GX_VALUE bottom)
{
  GX_RECTANGLE r;
  r.gx_rectangle_left   = left;
  r.gx_rectangle_top    = top;
  r.gx_rectangle_right  = right;
  r.gx_rectangle_bottom = bottom;
  return r;
}

/**
 * @brief Build the Library + Reader windows + their prompts.
 *
 * @pre GUIX has been initialised and a canvas exists.
 * @post Both windows are attached to the root; only the library
 *       window is visible at startup.
 *
 * @note Not thread-safe; only called from ui_thread setup.
 *
 * @since 0.1.0
 */
static void ereader_build_widgets(void)
{
  const GX_RECTANGLE root_rect =
    ereader_rect(0, 0, (GX_VALUE)(k_ereader_fb_width - 1U), (GX_VALUE)(k_ereader_fb_height - 1U));

  if (gx_window_root_create(&s_gx_root,
                            "ereader-root",
                            &s_gx_canvas,
                            GX_STYLE_NONE,
                            0U,
                            &root_rect) != GX_SUCCESS) {
    ereader_panic_halt();
  }

  if (gx_window_create(&s_gx_library_win,
                       "library",
                       GX_NULL,
                       GX_STYLE_BORDER_THIN | GX_STYLE_ENABLED,
                       0U,
                       &root_rect) != GX_SUCCESS) {
    ereader_panic_halt();
  }
  if (gx_window_create(&s_gx_reader_win, "reader", GX_NULL, GX_STYLE_BORDER_THIN, 1U, &root_rect) !=
      GX_SUCCESS) {
    ereader_panic_halt();
  }
  if (gx_widget_attach((GX_WIDGET*)&s_gx_root, (GX_WIDGET*)&s_gx_library_win) != GX_SUCCESS) {
    ereader_panic_halt();
  }
  if (gx_widget_attach((GX_WIDGET*)&s_gx_root, (GX_WIDGET*)&s_gx_reader_win) != GX_SUCCESS) {
    ereader_panic_halt();
  }

  const GX_RECTANGLE label_rect =
    ereader_rect(8, 8, (GX_VALUE)(k_ereader_fb_width - 9U), (GX_VALUE)(k_ereader_fb_height - 9U));
  if (gx_prompt_create(&s_gx_library_label,
                       "lib-text",
                       &s_gx_library_win,
                       0,
                       GX_STYLE_TEXT_LEFT | GX_STYLE_ENABLED,
                       0U,
                       &label_rect) != GX_SUCCESS) {
    ereader_panic_halt();
  }
  if (gx_prompt_create(&s_gx_reader_label,
                       "rd-text",
                       &s_gx_reader_win,
                       0,
                       GX_STYLE_TEXT_LEFT | GX_STYLE_ENABLED,
                       0U,
                       &label_rect) != GX_SUCCESS) {
    ereader_panic_halt();
  }

  GX_STRING reader_string;
  reader_string.gx_string_ptr    = s_reader_fallback_text;
  reader_string.gx_string_length = (UINT)(sizeof(s_reader_fallback_text) - 1U);
  (void)gx_prompt_text_set_ext(&s_gx_reader_label, &reader_string);

  /* Library prompt text starts as the placeholder; library_thread
   * rewrites it after the catalogue is ready. */
  static const char* k_initial_lib = "Loading library...";
  (void)memcpy(s_library_text, k_initial_lib, strlen(k_initial_lib) + 1U);
  GX_STRING lib_string;
  lib_string.gx_string_ptr    = s_library_text;
  lib_string.gx_string_length = (UINT)strlen(s_library_text);
  (void)gx_prompt_text_set_ext(&s_gx_library_label, &lib_string);

  if (gx_widget_show((GX_WIDGET*)&s_gx_root) != GX_SUCCESS) {
    ereader_panic_halt();
  }
  /* Reader window starts hidden -- gx_window_create above was passed
   * a style without GX_STYLE_ENABLED so it does not paint until the
   * user picks a book. */
}

/**
 * @brief Bring up GUIX, the canvas, the widgets, then enter the
 *        message-pumping loop.
 *
 * @pre Framebuffer + GUIX shim are reachable (verified by
 *      ``ra_guix_display_driver_bind``).
 * @post GUIX is repainting the canvas in response to UI events.
 *
 * @note Not thread-safe; only called from ui_thread_entry.
 *
 * @since 0.1.0
 */
static void ereader_ui_run(void)
{
  if (ra_guix_display_driver_bind(&s_framebuffer[0][0],
                                  (uint16_t)k_ereader_fb_width,
                                  (uint16_t)k_ereader_fb_height) != k_ra_ok) {
    ereader_panic_halt();
  }
  if (gx_system_memory_allocator_set(ereader_gx_alloc, ereader_gx_free) != GX_SUCCESS) {
    ereader_panic_halt();
  }
  if (gx_system_initialize() != GX_SUCCESS) {
    ereader_panic_halt();
  }
  if (gx_display_create(&s_gx_display,
                        "ra8d2-glcdc",
                        ra_guix_display_driver_setup,
                        (GX_VALUE)k_ereader_fb_width,
                        (GX_VALUE)k_ereader_fb_height) != GX_SUCCESS) {
    ereader_panic_halt();
  }
  if (gx_canvas_create(&s_gx_canvas,
                       "ra8d2-canvas",
                       &s_gx_display,
                       GX_CANVAS_MANAGED,
                       (UINT)k_ereader_fb_width,
                       (UINT)k_ereader_fb_height,
                       (GX_COLOR*)&s_framebuffer[0][0],
                       (ULONG)k_ereader_fb_width * (ULONG)k_ereader_fb_height *
                         (ULONG)k_ereader_bpp_rgb565) != GX_SUCCESS) {
    ereader_panic_halt();
  }

  ereader_build_widgets();

  if (gx_system_start() != GX_SUCCESS) {
    /* gx_system_start may return on an internal scheduler error.
     * Fall through to the message loop instead of halting; the UI
     * still works in "polling repaint" mode. */
  }

  /* Event loop. Receives events from library_thread, reader_thread
   * and the SW1/SW2 IRQs, then asks GUIX for a repaint. */
  while (1) {
    ULONG msg = 0UL;
    if (tx_queue_receive(&s_ui_queue, &msg, TX_WAIT_FOREVER) != TX_SUCCESS) {
      continue;
    }
    const uint8_t  kind    = (uint8_t)(msg & 0xFFUL);
    const uint32_t payload = (uint32_t)(msg >> (uint32_t)k_ereader_payload_shift);
    (void)payload;

    if (kind == (uint8_t)k_ereader_msg_library_ready) {
      ereader_build_library_text();
      GX_STRING lib_string;
      lib_string.gx_string_ptr    = s_library_text;
      lib_string.gx_string_length = (UINT)strlen(s_library_text);
      (void)gx_prompt_text_set_ext(&s_gx_library_label, &lib_string);
    } else if (kind == (uint8_t)k_ereader_msg_sw1) {
      /* SW1 = "next book" if in library, else "page-prev" if reading. */
      if (s_active_book >= s_book_count) {
        ULONG out = ereader_msg_pack(k_ereader_msg_load_book, 0U);
        (void)tx_queue_send(&s_reader_queue, &out, TX_NO_WAIT);
      } else {
        ULONG out = ereader_msg_pack(k_ereader_msg_page_prev, 0U);
        (void)tx_queue_send(&s_reader_queue, &out, TX_NO_WAIT);
      }
    } else if (kind == (uint8_t)k_ereader_msg_sw2) {
      /* SW2 = "page-next" while reading. */
      ULONG out = ereader_msg_pack(k_ereader_msg_page_next, 0U);
      (void)tx_queue_send(&s_reader_queue, &out, TX_NO_WAIT);
    } else if (kind == (uint8_t)k_ereader_msg_rendered) {
      /* Reader_thread says "the framebuffer is fresh"; ask GUIX to
       * redraw the canvas. */
      (void)gx_system_canvas_refresh();
    } else {
      /* Unknown message kind -- ignore. */
    }

    /* Non-blocking LVD poll. If the rail dipped past the
     * configured threshold since the last check, log a warning. The
     * GUIX prompt is not updated here -- a future Phase-6 sweep
     * will surface the warning in a status banner. */
    if (ereader_power_check_battery()) {
      ereader_print("[ereader] WARNING: low battery (LVD trip)\r\n");
    }
  }
}

/**
 * @brief ui_thread entry: build GUIX widgets and enter the event loop.
 *
 * @param[in] thread_input Unused (ThreadX cookie).
 *
 * @pre tx_application_define created the canvas storage.
 * @post GUIX repaints in response to messages.
 *
 * @note Not thread-safe; runs as a single-instance thread.
 *
 * @since 0.1.0
 */
static void ui_thread_entry(ULONG thread_input)
{
  (void)thread_input;
  ereader_ui_run();
}

/* =============================================================================
 * tx_application_define -- spawn the three threads + create the queues.
 * =============================================================================
 */

/**
 * @brief ThreadX system-define hook: build threads + queues + FileX.
 *
 * @param[in] first_unused_memory Pointer to the start of free RAM
 *   provided by the ThreadX port; unused -- we statically allocate.
 *
 * @pre tx_kernel_enter() has been called.
 * @post Three worker threads are running and FileX is initialised.
 *
 * @note Not thread-safe; runs once during scheduler startup.
 *
 * @since 0.1.0
 */
/* NOLINTNEXTLINE(misc-use-internal-linkage) -- exported symbol expected by ThreadX. */
void tx_application_define(void* first_unused_memory)
{
  (void)first_unused_memory;

  fx_system_initialize();

  if (tx_queue_create(&s_ui_queue,
                      "ereader-ui",
                      TX_1_ULONG,
                      s_ui_queue_storage,
                      sizeof(s_ui_queue_storage)) != TX_SUCCESS) {
    ereader_panic_halt();
  }
  if (tx_queue_create(&s_reader_queue,
                      "ereader-rd",
                      TX_1_ULONG,
                      s_reader_queue_storage,
                      sizeof(s_reader_queue_storage)) != TX_SUCCESS) {
    ereader_panic_halt();
  }

  if (tx_thread_create(&s_ui_thread_cb,
                       "ui",
                       ui_thread_entry,
                       0U,
                       s_ui_stack,
                       (ULONG)k_ereader_thread_stack,
                       (UINT)k_ereader_priority_ui,
                       (UINT)k_ereader_priority_ui,
                       TX_NO_TIME_SLICE,
                       TX_AUTO_START) != TX_SUCCESS) {
    ereader_panic_halt();
  }
  if (tx_thread_create(&s_library_thread_cb,
                       "library",
                       library_thread_entry,
                       0U,
                       s_library_stack,
                       (ULONG)k_ereader_thread_stack,
                       (UINT)k_ereader_priority_library,
                       (UINT)k_ereader_priority_library,
                       TX_NO_TIME_SLICE,
                       TX_AUTO_START) != TX_SUCCESS) {
    ereader_panic_halt();
  }
  if (tx_thread_create(&s_reader_thread_cb,
                       "reader",
                       reader_thread_entry,
                       0U,
                       s_reader_stack,
                       (ULONG)k_ereader_thread_stack,
                       (UINT)k_ereader_priority_reader,
                       (UINT)k_ereader_priority_reader,
                       TX_NO_TIME_SLICE,
                       TX_AUTO_START) != TX_SUCCESS) {
    ereader_panic_halt();
  }
}

#endif /* !RA_SIMULATOR_MODE */

/* =============================================================================
 * main()
 * =============================================================================
 */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"

/**
 * @brief Application entry. Bring up clocks + UART + LEDs, then enter ThreadX.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @post On normal entry the kernel runs the three threads.
 *
 * @note Not thread-safe; runs once at boot.
 *
 * @since 0.1.0
 */
int32_t main(void)
{
#ifndef RA_SIMULATOR_MODE
  ereader_setup_or_halt();
  /* Bring up the power surface (ICU button IRQs, LVD low-battery
   * threshold, LPM defaults). Failure here is non-fatal: the
   * ereader still boots without sleep / wake / brown-out support
   * if the chip-side substrate refuses; we just lose deep-sleep
   * page-turn power savings. */
  if (ereader_power_init() != k_ra_ok) {
    ereader_print("[ereader] power_init failed (continuing)\r\n");
  }
  ra_isr_globals_enable();
  ereader_print("[ereader] booting ThreadX + FileX + GUIX + ra_epub...\r\n");
  tx_kernel_enter();
  ereader_panic_halt();
#endif
  return 0;
}

#pragma GCC diagnostic pop
