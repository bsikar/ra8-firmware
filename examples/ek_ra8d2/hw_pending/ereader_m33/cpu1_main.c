/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hw_pending/ereader_m33/cpu1_main.c
 * @brief CPU1 (Cortex-M33 secondary core) image: render a held e-reader page
 *
 * @par Tag
 * [Ring1/app] {World: NS}
 *
 * @details
 * This is the firmware that runs on the RA8D2's *second* core, the Cortex-M33,
 * for the #150 power-saving demo. It is compiled as a wholly separate ELF
 * (`-mcpu=cortex-m33`) and embedded into the M85 ELF as a `.cpu1_image` blob;
 * the M85 hands the reader to this core via `ra8_cpu1_release` (HUM Ch 2.9.1)
 * and PARKS. From then on the M33 is the live core holding the page.
 *
 * The M33 runs the reader's data path AND renders one page to real pixels through
 * the production `ra8_gfx` text stack -- the same scalar (no-Helium) renderer the
 * M85 e-reader uses, here driving a framebuffer that lives in external SDRAM:
 *
 *   1. Stamp ::k_erm33_m33_sig into the shared mailbox so the parked M85 can
 *      prove the M33 left reset and is executing user code.
 *   2. Validate the baked, already-inflated `RABOOK1` flat blob
 *      (`rabook_fixture.h`) -- magic, version, size, table counts -- the same
 *      first step the real loader takes before walking a book.
 *   3. Walk chapter 0's DOM subtree iteratively (NASA Rule 1: an explicit stack,
 *      no recursion) with the pure inline `ra8_book.h` accessors, collecting the
 *      opening ::k_erm33_page_chars characters of extracted text.
 *   4. Bind the SDRAM RGB565 framebuffer (`s_framebuffer`, placed in `.sdram_bss`
 *      at 0x68000000) with `ra8_gfx_init`, clear it to paper white, and blit the
 *      collected text line by line with `ra8_gfx_text_out`.
 *   5. Fold a CRC-32 over the rendered pixels -- reading them back out of SDRAM is
 *      itself the proof real pixels landed -- and PUBLISH the framebuffer base,
 *      geometry, format, glyph count and CRC into the shared mailbox.
 *   6. Set `status = ok`, `done = 1`, then enter the #150 MODE-SWITCH hold loop:
 *      the M33 holds the page and polls a (fake) touch input. On each
 *      page turn it bumps `turn_req`, POKES the parked M85 over IPC0
 *      (`ra8_ipc_send_event`, HUM Ch 3.2.11 p 215) to wake it, waits for the
 *      M85's heavy-work ack (`turn_ack`), RE-RENDERS the held page (re-folding
 *      the same deterministic CRC), and signals `turn_done`. After
 *      ::k_erm33_max_turns turns it holds forever. A bad blob or render sets a
 *      failure status, then `done`.
 *
 * @note Only `ra8_book.h` (header-only inline accessors), `ra8_gfx` (three
 *       dependency-clean, zero-heap, scalar TUs) and the `ra8_ipc` send path (for
 *       the page-turn wake poke) are linked -- no decompression, no logging
 *       backend (RA8_LOG_LEVEL=0 collapses ra8_ipc's log calls), no panel driver --
 *       so this freestanding M33 image keeps a clean link.
 * @note The M33 deliberately does NOT call `ra8_log`: ra8_emulator echoes only the
 *       primary core's ITM, so an M33 log line would be invisible. Its
 *       proof-of-life is the mailbox the M85 narrates and the CRC it publishes.
 * @note The touch is *fake* (a bounded page-dwell spin): on real hardware
 *       this is replaced by a GT911 touch-controller poll, a HIL follow-up.
 * @note The framebuffer is the M33's own RGB565 plane in external SDRAM, not yet
 *       the GLCDC scan-out plane; wiring the held plane to the live panel + the
 *       display-plane handoff is a later increment (#150).
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "ereader_m33.h"
#include "ra8_attributes.h"
#include "ra8_book.h"
#include "ra8_gfx.h"
#include "ra8_ipc.h"
#include "rabook_fixture.h"

/** @brief CPU1 stack top (slot 0 of the M33 vector table). */
extern uint32_t g_ra8_ls_cpu1_stack_top;
/** @brief CPU1 `.data` run-region start (in SRAM_CPU1). */
extern uint32_t g_ra8_ls_cpu1_data_start;
/** @brief CPU1 `.data` run-region end. */
extern uint32_t g_ra8_ls_cpu1_data_end;
/** @brief CPU1 `.data` load image (in MRAM_CPU1). */
extern uint32_t g_ra8_ls_cpu1_data_load;
/** @brief CPU1 `.bss` start (in SRAM_CPU1). */
extern uint32_t g_ra8_ls_cpu1_bss_start;
/** @brief CPU1 `.bss` end. */
extern uint32_t g_ra8_ls_cpu1_bss_end;

[[noreturn]] void cpu1_reset_handler(void);

/* The published format tag must equal the real ra8_gfx RGB565 enumerator. */
static_assert((uint32_t)k_erm33_fb_format_rgb565 == (uint32_t)k_ra8_gfx_format_rgb565,
              "published fb_format must equal ra8_gfx RGB565");
/* The geometry the mailbox advertises must match the baked plane size. */
static_assert((uint32_t)k_erm33_fb_bytes ==
                ((uint32_t)k_erm33_fb_width * (uint32_t)k_erm33_fb_height *
                 (uint32_t)k_erm33_fb_bpp),
              "framebuffer byte size must equal width*height*bpp");

/**
 * @var s_framebuffer
 * @brief The M33's RGB565 page framebuffer, resident in external SDRAM.
 * @details Placed in the `.sdram_bss` (NOLOAD) section the CPU1 linker script
 *          pins at the SDRAM base (0x68000000); `ra8_gfx_clear` paints every byte
 *          before the page is published, so it needs no startup zeroing. The M33
 *          publishes its address (`&s_framebuffer`) in the mailbox's `fb_base`.
 * @warning Written only through `ra8_gfx`; do not modify directly.
 * @since 0.1.0
 */
[[gnu::section(".sdram_bss"), gnu::aligned(8)]] static uint8_t s_framebuffer[k_erm33_fb_bytes];

/**
 * @enum m33_walk_bound_t
 * @brief Static iteration bounds for the M33 DOM walk (NASA Rule 2).
 * @details Each loop in the reader is bounded by one of these compile-time
 *          constants so the M33 can never run unbounded on a corrupt blob.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_walk_iter_max = 4096U, /**< Max DOM-walk pops in the chapter.   */
  k_max_run_len   = 1024U, /**< Bounded text-run length cap, bytes. */
} m33_walk_bound_t;

/**
 * @enum m33_walk_size_t
 * @brief Small table / validation sizes for the M33 reader.
 * @details The DOM-walk stack depth and the validation caps that reject an
 *          implausible blob before the walk begins.
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_walk_stack_depth = 64U,   /**< Explicit DOM-walk stack depth.     */
  k_max_chapters     = 32U,   /**< Validation cap on `chapter_count`. */
  k_max_nodes        = 4096U, /**< Validation cap on `node_count`.    */
  k_magic_len        = 7U,    /**< "RABOOK1" magic length (no NUL).   */
} m33_walk_size_t;

/**
 * @enum m33_render_t
 * @brief Glyph cell height and page colours for the `ra8_gfx` blit.
 * @details The bundled `ra8_gfx` font is 8x16, so glyph rows advance by 16 px.
 *          The page is rendered as black ink on a white paper background, the
 *          conventional e-reader palette.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_erm33_glyph_h = 16U,       /**< `ra8_gfx` 8x16 font cell height, pixels. */
  k_erm33_paper   = 0xFFFFFFU, /**< Page background colour (white).          */
  k_erm33_ink     = 0x000000U, /**< Glyph foreground colour (black).         */
} m33_render_t;

/**
 * @enum m33_crc_t
 * @brief Constants for the standard reflected CRC-32 over the rendered plane.
 * @details The M33 folds every byte of its SDRAM framebuffer through the IEEE
 *          802.3 / zlib CRC-32 (reflected polynomial, pre/post-inverted), so the
 *          published value is a stable fingerprint of the exact pixels drawn.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_crc32_init = 0xFFFFFFFFU, /**< Pre/post-inversion seed.     */
  k_crc32_poly = 0xEDB88320U, /**< Reflected CRC-32 polynomial. */
} m33_crc_t;

/**
 * @enum m33_crc_bits_t
 * @brief Bit-fold count for the bitwise CRC-32 inner loop.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_crc32_bits = 8U, /**< Bits folded per input byte. */
} m33_crc_bits_t;

/**
 * @enum cpu1_ipc_t
 * @brief IPC channel the M33 pokes to wake the parked M85 on a page turn.
 * @details IPC0 channel 0 is the CPU1 -> CPU0 direction; writing its IPC0ISET0
 *          raises the IPC0 receive interrupt the M85 armed before release, so the
 *          M85 leaves WFI as soon as a page-turn request is published.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_cpu1_ipc_wake_channel = 0U, /**< IPC0 channel 0 (CPU1 -> CPU0). */
} cpu1_ipc_t;

/**
 * @enum m33_cycle_bound_t
 * @brief Static bound for the M33's wait-for-ack poll (NASA Rule 2).
 * @details The M85 acknowledges a page turn within a few ra8_emulator interleaves;
 *          this bound is the backstop so a never-arriving ack still terminates
 *          the loop instead of hanging the held-page cycle.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_m33_ack_budget = 50000000UL, /**< Max iters waiting for the M85's turn_ack. */
} m33_cycle_bound_t;

/**
 * @brief Append one NUL-terminated text run into the page accumulator.
 *
 * @details Scans @p txt up to its NUL or the static ::k_max_run_len cap, copying
 * each character into @p out until @p out reaches @p cap. A run that would
 * overflow the page is truncated, never wrapped past the buffer.
 *
 * @param[in,out] out   Page accumulator (never NULL).
 * @param[in,out] plen  In/out count of characters already buffered (never NULL).
 * @param[in]     cap   Capacity of @p out in characters.
 * @param[in]     txt   NUL-terminated run from the validated blob (may be NULL).
 *
 * @return Nothing.
 *
 * @pre @p out and @p plen are non-NULL; `*plen <= cap`.
 * @pre @p txt, when non-NULL, points inside the validated blob's string pool.
 * @post `*plen <= cap` still holds.
 * @post Iteration bounded by ::k_max_run_len (NASA Rule 2).
 *
 * @note Single-threaded; runs in M33 thread mode with no RTOS.
 * @since 0.1.0
 */
static void append_run(char* out, uint32_t* plen, uint32_t cap, const char* txt)
{
  if (out == nullptr) {
    return;
  }
  if (plen == nullptr) {
    return;
  }
  if (txt == nullptr) {
    return;
  }
  RA8_LOOP_BOUND(k_max_run_len);
  for (uint32_t i = 0U; i < (uint32_t)k_max_run_len; i++) {
    const char c = txt[i];
    if (c == '\0') {
      break;
    }
    if (*plen >= cap) {
      break;
    }
    out[*plen] = c;
    *plen += 1U;
  }
}

/**
 * @struct m33_walk_stack_t
 * @brief Explicit DOM-walk stack (NASA Rule 1: iteration, never recursion).
 * @details A fixed ::k_walk_stack_depth array plus a stack pointer; the M33
 *          pushes a node's sibling and first child here instead of recursing.
 * @invariant `sp <= k_walk_stack_depth` at every call boundary.
 * @since 0.1.0
 */
typedef struct {
  uint32_t items[k_walk_stack_depth]; /**< Pending node indices.     */
  uint32_t sp;                        /**< Count of pending entries. */
} m33_walk_stack_t;

/**
 * @brief Push a node index onto the DOM-walk stack if it fits.
 *
 * @param[in,out] st   Walk stack (never NULL).
 * @param[in]     node Node index to push, or ::k_ra8_book_nil to ignore.
 *
 * @return Nothing.
 *
 * @pre @p st is non-NULL with `sp <= k_walk_stack_depth`.
 * @pre @p node is a node index or the nil sentinel.
 * @post A non-nil @p node is pushed unless the stack is full (then dropped).
 * @post `st->sp <= k_walk_stack_depth` still holds.
 *
 * @note Single-threaded; runs in M33 thread mode with no RTOS.
 * @since 0.1.0
 */
static void walk_push(m33_walk_stack_t* st, uint32_t node)
{
  if (st == nullptr) {
    return;
  }
  if (node == (uint32_t)k_ra8_book_nil) {
    return;
  }
  if (st->sp < (uint32_t)k_walk_stack_depth) {
    st->items[st->sp] = node;
    st->sp += 1U;
  }
}

/**
 * @brief Collect a chapter's opening text into the page accumulator.
 *
 * @details Iterative pre-order walk (NASA Rule 1: an explicit index stack, no
 * recursion) rooted at @p root. The root's own siblings are never followed, so
 * the walk stays inside this chapter. Each text node's run is appended via
 * ::append_run; the walk stops once @p cap characters are buffered. The pop loop
 * is bounded by ::k_walk_iter_max.
 *
 * @param[in]  base       Validated `RABOOK1` blob base (never NULL).
 * @param[in]  root       Chapter root node index, or ::k_ra8_book_nil.
 * @param[in]  node_count Node-table length (an out-of-range index is dropped).
 * @param[out] out        Page accumulator (never NULL).
 * @param[in]  cap        Capacity of @p out in characters.
 *
 * @return Count of characters collected (`<= cap`).
 * @retval 0 @p base or @p out was NULL, or the chapter held no text.
 *
 * @pre @p base was accepted by ::book_is_valid.
 * @pre @p out has room for @p cap characters.
 * @post The return value is `<= cap`.
 * @post Iteration is bounded by ::k_walk_iter_max (NASA Rule 2).
 *
 * @note Single-threaded; runs in M33 thread mode with no RTOS.
 * @since 0.1.0
 */
static uint32_t
collect_chapter_text(const void* base, uint32_t root, uint32_t node_count, char* out, uint32_t cap)
{
  if (base == nullptr) {
    return 0U;
  }
  if (out == nullptr) {
    return 0U;
  }
  if (root == (uint32_t)k_ra8_book_nil) {
    return 0U;
  }
  const ra8_book_node_t* nodes = ra8_book_nodes(base);
  m33_walk_stack_t       st    = {};
  uint32_t               len   = 0U;
  if (root != (uint32_t)k_ra8_book_nil) {
    walk_push(&st, nodes[root].first_child);
  }
  RA8_LOOP_BOUND(k_walk_iter_max);
  for (uint32_t it = 0U; it < (uint32_t)k_walk_iter_max; it++) {
    if ((st.sp == 0U) || (len >= cap)) {
      break;
    }
    const uint32_t n = st.items[--st.sp];
    if (n >= node_count) {
      continue;
    }
    const ra8_book_node_t* node = &nodes[n];
    walk_push(&st, node->next_sibling);
    if (node->kind == (uint8_t)k_ra8_book_node_text) {
      append_run(out, &len, cap, ra8_book_node_text(base, node));
    } else {
      walk_push(&st, node->first_child);
    }
  }
  return len;
}

/**
 * @brief Render the collected page text into the SDRAM framebuffer via `ra8_gfx`.
 *
 * @details Binds @p fb as an RGB565 canvas, clears it to paper white, then lays
 * the @p len buffered characters out left-to-right, wrapping to the next glyph
 * row every ::k_erm33_fb_cols cells, blitting each row string with one
 * `ra8_gfx_text_out`. The row loop is bounded by ::k_erm33_fb_rows.
 *
 * @param[out] fb   SDRAM framebuffer base (never NULL).
 * @param[in]  text Collected page text (never NULL).
 * @param[in]  len  Count of valid characters in @p text.
 *
 * @return Whether every `ra8_gfx` call succeeded.
 * @retval true  The plane holds the page over a paper-white background.
 * @retval false A NULL argument or an `ra8_gfx` error; the plane is undefined.
 *
 * @pre @p fb is ::k_erm33_fb_bytes bytes of SDRAM.
 * @pre @p text holds at least @p len characters.
 * @post On true every framebuffer byte was written (paper or glyph pixels).
 * @post Iteration bounded by ::k_erm33_fb_rows (NASA Rule 2).
 *
 * @note Single-threaded; runs in M33 thread mode with no RTOS.
 * @since 0.1.0
 */
static bool render_page(uint8_t* fb, const char* text, uint32_t len)
{
  if (fb == nullptr) {
    return false;
  }
  if (text == nullptr) {
    return false;
  }
  if (ra8_gfx_init(fb,
                   (uint16_t)k_erm33_fb_width,
                   (uint16_t)k_erm33_fb_height,
                   k_ra8_gfx_format_rgb565) != k_ra8_ok) {
    return false;
  }
  if (ra8_gfx_clear((uint32_t)k_erm33_paper) != k_ra8_ok) {
    return false;
  }
  RA8_LOOP_BOUND(k_erm33_fb_rows);
  for (uint32_t row = 0U; row < (uint32_t)k_erm33_fb_rows; row++) {
    char     line[(uint32_t)k_erm33_fb_cols + 1U];
    uint32_t cols = 0U;
    RA8_LOOP_BOUND(k_erm33_fb_cols);
    for (uint32_t col = 0U; col < (uint32_t)k_erm33_fb_cols; col++) {
      const uint32_t idx = (row * (uint32_t)k_erm33_fb_cols) + col;
      if (idx >= len) {
        break;
      }
      line[cols] = text[idx];
      cols += 1U;
    }
    line[cols] = '\0';
    if (cols == 0U) {
      continue;
    }
    const int32_t y = (int32_t)(row * (uint32_t)k_erm33_glyph_h);
    if (ra8_gfx_text_out(0,
                         y,
                         line,
                         &ra8_gfx_font_8x16,
                         (uint32_t)k_erm33_ink,
                         (uint32_t)k_erm33_paper) != k_ra8_ok) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Fold a reflected CRC-32 over the rendered framebuffer pixels.
 *
 * @details Standard table-less reflected CRC-32: seed all-ones, fold each byte
 * LSB-first through ::k_crc32_poly, post-invert. Reading every byte back out of
 * SDRAM is what proves the M33's pixels actually landed; a blank plane hashes to
 * one fixed value, so a different CRC demonstrates genuine rendering.
 *
 * @param[in] fb  Framebuffer base (never NULL).
 * @param[in] len Bytes to hash; expected to equal ::k_erm33_fb_bytes.
 *
 * @return CRC-32 of the @p len bytes at @p fb.
 * @retval 0 @p fb is NULL or @p len is 0 (no pixels to hash).
 *
 * @pre @p fb addresses the rendered SDRAM framebuffer.
 * @pre The render completed and a `dsb` drained the pixels to SDRAM.
 * @post No framebuffer byte is modified.
 * @post Both loops are bounded (::k_erm33_fb_bytes, ::k_crc32_bits; NASA Rule 2).
 *
 * @note Single-threaded; runs in M33 thread mode with no RTOS.
 * @since 0.1.0
 */
static uint32_t fb_crc32(const uint8_t* fb, uint32_t len)
{
  if (fb == nullptr) {
    return 0U;
  }
  if (len == 0U) {
    return 0U;
  }
  uint32_t crc = (uint32_t)k_crc32_init;
  RA8_LOOP_BOUND(k_erm33_fb_bytes);
  for (uint32_t i = 0U; i < len; i++) {
    if (i >= (uint32_t)k_erm33_fb_bytes) {
      break;
    }
    crc ^= (uint32_t)fb[i];
    RA8_LOOP_BOUND(k_crc32_bits);
    for (uint8_t b = 0U; b < (uint8_t)k_crc32_bits; b++) {
      const uint32_t mask = (uint32_t)(0U - (crc & 1U));
      crc                 = (crc >> 1U) ^ ((uint32_t)k_crc32_poly & mask);
    }
  }
  return crc ^ (uint32_t)k_crc32_init;
}

/**
 * @brief Publish the rendered page's base, geometry, format and CRC.
 *
 * @details Writes the SDRAM framebuffer address, the agreed RGB565 geometry, the
 * glyph count and the freshly-folded CRC into the shared mailbox, each behind a
 * `dsb` so the parked M85 observes a fully-formed descriptor.
 *
 * @param[out] mb      Shared mailbox (never NULL).
 * @param[in]  crc     CRC-32 the M33 folded over the framebuffer.
 * @param[in]  glyphs  Characters laid onto the held page.
 *
 * @return Nothing.
 *
 * @pre @p mb is the fixed-address mailbox pointer.
 * @pre The framebuffer at `s_framebuffer` holds the rendered page.
 * @post `fb_base` / geometry / `fb_crc` / `glyph_count` are published with a `dsb`.
 * @post No field the M85 owns (`magic`) is touched.
 *
 * @note Single-threaded; runs in M33 thread mode with no RTOS.
 * @since 0.1.0
 */
static void publish_page(volatile erm33_mailbox_t* mb, uint32_t crc, uint32_t glyphs)
{
  if (mb == nullptr) {
    return;
  }
  mb->fb_base     = (uint32_t)(uintptr_t)s_framebuffer;
  mb->fb_width    = (uint32_t)k_erm33_fb_width;
  mb->fb_height   = (uint32_t)k_erm33_fb_height;
  mb->fb_stride   = (uint32_t)k_erm33_fb_stride;
  mb->fb_format   = (uint32_t)k_erm33_fb_format_rgb565;
  mb->glyph_count = glyphs;
  mb->fb_crc      = crc;
  __asm volatile("dsb" ::: "memory");
}

/**
 * @brief Validate the baked `RABOOK1` flat blob before walking it.
 *
 * @details Checks the "RABOOK1" magic, the format version, that the header's
 * `total_size` matches the baked length, and that the chapter / node counts are
 * non-zero and within sane caps. Mirrors the real loader's gate so the M33 never
 * walks a malformed blob (the inline accessors do no bounds checking).
 *
 * @param[in] base Candidate blob base (never NULL in this app).
 * @param[in] size Baked blob length in bytes (::k_rabook_fixture_len).
 *
 * @return Whether the blob is well-formed enough to walk.
 * @retval true  Magic, version, size and table counts are all plausible.
 * @retval false Any check failed; the caller must not walk the blob.
 *
 * @pre @p base points at the baked fixture rodata.
 * @pre @p size is the true baked length.
 * @post No field of the blob is modified.
 * @post On false, no accessor is subsequently called on @p base.
 *
 * @note Pure; reads only the immutable candidate blob.
 * @since 0.1.0
 */
static bool book_is_valid(const void* base, uint32_t size)
{
  if (base == nullptr) {
    return false;
  }
  if (size < (uint32_t)k_ra8_book_sizeof_header) {
    return false;
  }
  const ra8_book_header_t* hdr      = ra8_book_header(base);
  static const char        expect[] = {'R', 'A', 'B', 'O', 'O', 'K', '1'};
  RA8_LOOP_BOUND(k_magic_len);
  for (uint32_t i = 0U; i < (uint32_t)k_magic_len; i++) {
    if (hdr->magic[i] != expect[i]) {
      return false;
    }
  }
  if (hdr->format_version != (uint32_t)k_ra8_book_format_version) {
    return false;
  }
  if (hdr->total_size != size) {
    return false;
  }
  if ((hdr->chapter_count == 0U) || (hdr->chapter_count > (uint32_t)k_max_chapters)) {
    return false;
  }
  if ((hdr->node_count == 0U) || (hdr->node_count > (uint32_t)k_max_nodes)) {
    return false;
  }
  return true;
}

/**
 * @brief Re-render the held page from the validated book and fold its CRC.
 *
 * @details Re-collects chapter 0's opening text, renders it into the SDRAM
 * framebuffer via `ra8_gfx`, drains the writes with a `dsb`, then folds the
 * CRC-32 over the rendered pixels. Pure of the mailbox and fully deterministic --
 * the same immutable blob renders the same bytes -- so every call (the first
 * render and each page-turn re-render) yields the identical CRC, keeping the
 * ra8_emulator gate's golden stable.
 *
 * @param[in]  base       Validated `RABOOK1` blob base (never NULL).
 * @param[out] out_crc    Receives the CRC-32 of the rendered framebuffer.
 * @param[out] out_glyphs Receives the count of characters laid onto the page.
 *
 * @return Whether the render + CRC fold succeeded.
 * @retval true  The page rendered and @p out_crc / @p out_glyphs are set.
 * @retval false @p base / @p out_crc / @p out_glyphs was NULL, or `ra8_gfx` failed.
 *
 * @pre @p base was accepted by ::book_is_valid.
 * @pre @p out_crc and @p out_glyphs are writable.
 * @post On true the SDRAM framebuffer holds the rendered held page.
 * @post On false no published mailbox field should be trusted.
 *
 * @note Single-threaded; runs in M33 thread mode with no RTOS.
 * @since 0.1.0
 */
static bool render_held_page(const void* base, uint32_t* out_crc, uint32_t* out_glyphs)
{
  if (base == nullptr) {
    return false;
  }
  if (out_crc == nullptr) {
    return false;
  }
  if (out_glyphs == nullptr) {
    return false;
  }
  const ra8_book_header_t*  hdr      = ra8_book_header(base);
  const ra8_book_chapter_t* chapters = ra8_book_chapters(base);
  char                      page_text[k_erm33_page_chars];
  const uint32_t            glyphs = collect_chapter_text(base,
                                                          chapters[0].root_node,
                                                          hdr->node_count,
                                                          page_text,
                                                          (uint32_t)k_erm33_page_chars);
  if (!render_page(s_framebuffer, page_text, glyphs)) {
    return false;
  }
  __asm volatile("dsb" ::: "memory");
  *out_crc    = fb_crc32(s_framebuffer, (uint32_t)k_erm33_fb_bytes);
  *out_glyphs = glyphs;
  return true;
}

/**
 * @brief Poke the M85's IPC0 receive line to wake it from a page-turn park.
 *
 * @details Writes IPC0ISET0 for the wake channel (HUM Ch 3.2.11 "IPC0ISET0"
 * p 215), asserting IRQ line 0 on the primary core. `turn_req` is already
 * published and `dsb`-ordered ahead of this call, so the woken M85 observes a
 * settled request. The HAL return value is intentionally discarded: a failed poke
 * only costs the M85 a fall-through to its bounded poll, never correctness.
 *
 * @return Nothing.
 *
 * @pre `turn_req` is published behind a `dsb`.
 * @pre The M85 armed the IPC0 receive IRQ before releasing this core.
 * @post IPC0 channel-0 IRQ line 0 is asserted toward the M85.
 * @post No shared mailbox field is modified.
 *
 * @note Single-threaded; runs in M33 thread mode with no RTOS.
 * @since 0.1.0
 */
static void notify_m85(void)
{
  (void)ra8_ipc_send_event((uint8_t)k_cpu1_ipc_wake_channel, k_ra8_ipc_irq_event_0);
}

/**
 * @brief Spin a bounded page-dwell that stands in for a touch-poll latency.
 *
 * @details The real e-reader would poll the GT911 touch controller while holding
 * the page; here the M33 spins ::k_erm33_touch_dwell times on a volatile counter
 * so ra8_emulator interleaves the cores a few times before the synthetic page turn
 * fires. Deterministic and bounded (NASA Rule 2); replaced by a real touch poll
 * on hardware (a HIL follow-up).
 *
 * @return Nothing.
 *
 * @pre Runs in M33 thread mode while holding the rendered page.
 * @pre ::k_erm33_touch_dwell is the agreed dwell bound.
 * @post No shared state is modified.
 * @post Iteration count bounded by ::k_erm33_touch_dwell (NASA Rule 2).
 *
 * @note Single-threaded; pure busy-wait with no side effects.
 * @since 0.1.0
 */
static void simulate_touch_dwell(void)
{
  volatile uint32_t spin = 0U;
  RA8_LOOP_BOUND(k_erm33_touch_dwell);
  for (uint32_t i = 0U; i < (uint32_t)k_erm33_touch_dwell; i++) {
    spin = spin + 1U;
  }
  (void)spin;
}

/**
 * @brief Poll the mailbox until the M85 acks page turn @p turn (heavy work done).
 *
 * @details After the M33 requests a page turn and pokes the M85, the woken M85
 * does its heavy next-page work and writes `turn_ack`. This bounded poll waits
 * for that ack across ra8_emulator's core interleaves; the bound is the NASA Rule 2
 * backstop for an ack that never arrives.
 *
 * @param[in] mb   Shared mailbox (never NULL).
 * @param[in] turn The page-turn number being acknowledged (1-based).
 *
 * @return Whether `turn_ack` reached @p turn within budget.
 * @retval true  `turn_ack >= turn` within ::k_m33_ack_budget iterations.
 * @retval false @p mb was NULL or the budget was exhausted first.
 *
 * @pre @p mb is the fixed-address mailbox pointer; `turn_req == turn` published.
 * @pre The M85 was poked via ::notify_m85.
 * @post No mailbox field is modified.
 * @post Iteration count bounded by ::k_m33_ack_budget (NASA Rule 2).
 *
 * @note Single-threaded; runs in M33 thread mode with no RTOS.
 * @since 0.1.0
 */
static bool wait_for_ack(volatile erm33_mailbox_t* mb, uint32_t turn)
{
  if (mb == nullptr) {
    return false;
  }
  RA8_LOOP_BOUND(k_m33_ack_budget);
  for (uint32_t i = 0U; i < (uint32_t)k_m33_ack_budget; i++) {
    if (mb->turn_ack >= turn) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Hold the page and run the #150 page-turn handoff loop for the M85.
 *
 * @details For each of ::k_erm33_max_turns turns the M33 dwells on the held page
 * (::simulate_touch_dwell), publishes the page-turn request (`turn_req`) behind a
 * `dsb`, pokes the parked M85 (::notify_m85), waits for the M85's heavy-work ack
 * (::wait_for_ack), RE-RENDERS the held page (::render_held_page -- same content,
 * same CRC), republishes the descriptor, and sets `turn_done`. A missed ack ends
 * the loop. The outer loop is bounded by ::k_erm33_max_turns (NASA Rule 2).
 *
 * @param[in,out] mb   Shared mailbox (never NULL).
 * @param[in]     base Validated `RABOOK1` blob base (never NULL).
 *
 * @return Nothing.
 *
 * @pre @p mb is the fixed-address mailbox pointer with the first page published.
 * @pre @p base was accepted by ::book_is_valid.
 * @post `turn_done` advances to the last completed turn (<= ::k_erm33_max_turns).
 * @post The SDRAM framebuffer holds the (re-rendered) held page on return.
 *
 * @note Single-threaded; runs in M33 thread mode with no RTOS.
 * @since 0.1.0
 */
static void run_page_turns(volatile erm33_mailbox_t* mb, const void* base)
{
  if (mb == nullptr) {
    return;
  }
  if (base == nullptr) {
    return;
  }
  RA8_LOOP_BOUND(k_erm33_max_turns);
  for (uint32_t turn = 1U; turn <= (uint32_t)k_erm33_max_turns; turn++) {
    simulate_touch_dwell();
    mb->turn_req = turn;
    __asm volatile("dsb" ::: "memory");
    notify_m85();
    if (!wait_for_ack(mb, turn)) {
      return;
    }
    uint32_t crc    = 0U;
    uint32_t glyphs = 0U;
    if (render_held_page(base, &crc, &glyphs)) {
      publish_page(mb, crc, glyphs);
    }
    mb->turn_done = turn;
    __asm volatile("dsb" ::: "memory");
  }
}

/**
 * @brief Park the M33 forever once the held page has been published.
 *
 * @return This function never returns.
 * @note The core spins in place.
 *
 * @pre The mailbox already carries the final `status` / `done`.
 * @pre Entered only after the render completes (or fails validation).
 * @post The M33 makes no further forward progress.
 * @post The published mailbox state is stable for the M85 to read.
 *
 * @note Mirrors the sibling dual-core park loops; this is the held-page idle.
 * @since 0.1.0
 */
[[noreturn]] static void cpu1_park(void)
{
  while (1) {
    __asm volatile("nop");
  }
}

/**
 * @brief Mark a failure status, publish `done`, and park the M33.
 *
 * @param[out] mb     Shared mailbox (never NULL).
 * @param[in]  status Failure status code (::erm33_const_t).
 *
 * @return This function never returns.
 * @note Control ends in ::cpu1_park.
 *
 * @pre @p mb is the fixed-address mailbox pointer.
 * @pre A validation or render step failed.
 * @post `mb->status == status` and `mb->done == 1`, both behind a `dsb`.
 * @post The M33 makes no further forward progress.
 *
 * @note Single-threaded; runs in M33 thread mode with no RTOS.
 * @since 0.1.0
 */
[[noreturn]] static void cpu1_fail(volatile erm33_mailbox_t* mb, uint32_t status)
{
  if (mb != nullptr) {
    mb->status = status;
    __asm volatile("dsb" ::: "memory");
    mb->done = 1U;
    __asm volatile("dsb" ::: "memory");
  }
  cpu1_park();
}

/**
 * @brief CPU1 reader entry: validate the book, render the held page, publish.
 *
 * @details Stamps the boot signature, validates the baked `RABOOK1` blob,
 * renders chapter 0's opening page into the SDRAM framebuffer (::render_held_page),
 * publishes the descriptor, and sets `status = ok`, `done = 1`. It then becomes
 * the live core for the #150 mode-switch: it runs the page-turn hold loop
 * (::run_page_turns) -- holding the page, waking the parked M85 on each scripted
 * touch, and re-rendering -- before holding for good. A failure at any step
 * publishes the matching failure status and parks.
 *
 * @return This function never returns.
 * @note Control ends in ::cpu1_park.
 *
 * @pre `cpu1_reset_handler` has initialised `.data` / `.bss`.
 * @pre The M85 published ::k_erm33_magic and released this core.
 * @post `mb->done == 1` with `status`, geometry and `fb_crc` reflecting the run.
 * @post On success the SDRAM framebuffer holds the (re-rendered) held page.
 *
 * @note Single-threaded; runs in M33 thread mode with no RTOS.
 * @since 0.1.0
 */
[[noreturn]] static void cpu1_run_reader(void)
{
  volatile erm33_mailbox_t* mb = erm33_mailbox();
  mb->m33_sig                  = (uint32_t)k_erm33_m33_sig;
  __asm volatile("dsb" ::: "memory");

  const void* base = (const void*)k_rabook_fixture;
  if (!book_is_valid(base, (uint32_t)k_rabook_fixture_len)) {
    cpu1_fail(mb, (uint32_t)k_erm33_status_bad_book);
  }

  uint32_t crc    = 0U;
  uint32_t glyphs = 0U;
  if (!render_held_page(base, &crc, &glyphs)) {
    cpu1_fail(mb, (uint32_t)k_erm33_status_render_fail);
  }
  publish_page(mb, crc, glyphs);

  mb->status = (uint32_t)k_erm33_status_ok;
  __asm volatile("dsb" ::: "memory");
  mb->done = 1U;
  __asm volatile("dsb" ::: "memory");

  /* #150 mode-switch: the M33 is now the live core. Hold the page, poll the
   * simulated touch, and on each page turn wake the parked M85, wait for its
   * heavy-work ack, then re-render + republish the (identical) held page. */
  run_page_turns(mb, base);
  cpu1_park();
}

/**
 * @brief CPU1 reset handler: minimal C-runtime init, then run the reader.
 *
 * @details The M33 boots with uninitialised RAM, so before any C code runs this
 * copies `.data` from its MRAM_CPU1 load image into SRAM_CPU1 and zeroes `.bss`.
 * The linker exports the region bounds as `g_ra8_ls_cpu1_*` symbols. The SDRAM
 * framebuffer is in a NOLOAD section and is painted by `ra8_gfx_clear`, so it is
 * deliberately left out of this init.
 *
 * @return This function never returns.
 * @note Control passes to ::cpu1_run_reader, which holds forever.
 *
 * @pre Hardware loaded the initial SP from `.cpu1_vectors[0]`.
 * @pre The M85 released this core via the CPU1ACTCSR handshake.
 * @post `.data` mirrors its MRAM_CPU1 load image.
 * @post `.bss` is zero-filled.
 *
 * @note Entered only from the CPU1 vector table; runs in M33 thread mode.
 * @since 0.1.0
 */
[[noreturn]] void cpu1_reset_handler(void)
{
  uint32_t* dst = &g_ra8_ls_cpu1_data_start;
  uint32_t* src = &g_ra8_ls_cpu1_data_load;
  while (dst < &g_ra8_ls_cpu1_data_end) {
    *dst = *src;
    dst++;
    src++;
  }

  uint32_t* bss = &g_ra8_ls_cpu1_bss_start;
  while (bss < &g_ra8_ls_cpu1_bss_end) {
    *bss = 0U;
    bss++;
  }

  cpu1_run_reader();
}

/**
 * @brief CPU1 default fault handler: park the core.
 *
 * @details Every M33 exception slot routes here. The core stops making forward
 * progress; on hardware a watchdog (if enabled) eventually resets.
 *
 * @return This function never returns.
 * @note The core spins in place.
 *
 * @pre A hardware fault or unhandled exception occurred.
 * @pre Entered via the M33 exception entry path.
 * @post The M33 makes no further forward progress.
 * @post `done` stays at whatever value it held at fault time, so a fault before
 *       completion is visible to the M85 as a stalled, never-done reader.
 *
 * @note Shared default for all CPU1 exception vectors.
 * @since 0.1.0
 */
[[noreturn]] static void cpu1_fault_handler(void)
{
  while (1) {
    __asm volatile("nop");
  }
}

/**
 * @var g_cpu1_vector_table
 * @brief Minimal Armv8-M (baseline) vector table for the M33 image.
 * @details Slot 0 is the initial SP, slot 1 the reset handler; the remaining
 *          core-exception slots share the fault handler. The M85 points
 *          CPU1INITVTOR at this table when it releases the core.
 * @note Placed in the `.cpu1_vectors` section by `linker_script_cpu1.ld`.
 * @warning Do not modify at runtime.
 * @since 0.1.0
 */
#ifndef RA8_OFF_TARGET
/* The vector table is only meaningful in the cross-compiled M33 image. The host
 * unit-test build compile-checks this TU but never links it as an executable, so
 * dropping the table there costs no coverage. */
[[gnu::used, gnu::section(".cpu1_vectors")]] const uintptr_t g_cpu1_vector_table[] = {
  (uintptr_t)&g_ra8_ls_cpu1_stack_top,
  (uintptr_t)&cpu1_reset_handler,
  (uintptr_t)&cpu1_fault_handler,
  (uintptr_t)&cpu1_fault_handler,
  (uintptr_t)&cpu1_fault_handler,
  (uintptr_t)&cpu1_fault_handler,
  (uintptr_t)&cpu1_fault_handler,
  (uintptr_t)&cpu1_fault_handler,
};
#endif
