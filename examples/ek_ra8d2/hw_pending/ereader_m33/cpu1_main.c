/**
 * @file examples/ek_ra8d2/hw_pending/ereader_m33/cpu1_main.c
 * @brief CPU1 (Cortex-M33 secondary core) image: render the e-reader page
 *
 * @par Tag
 * [Ring1/app] {World: NS}
 *
 * @details
 * This is the firmware that runs on the RA8D2's *second* core, the Cortex-M33,
 * for the #150 power-saving demo. It is compiled as a wholly separate ELF
 * (`-mcpu=cortex-m33`) and embedded into the M85 ELF as a `.cpu1_image` blob;
 * the M85 hands the reader to this core via `ra_cpu1_release` (HUM Ch 2.9.1)
 * and PARKS. From then on the M33 is the live core running the e-reader.
 *
 * The M33 runs the reader's data path AND renders each page to real pixels:
 *
 *   1. Stamp ::k_erm33_m33_sig into the shared mailbox so the parked M85 can
 *      prove the M33 left reset and is executing user code.
 *   2. Validate the baked, already-inflated `RABOOK1` flat blob
 *      (`rabook_fixture.h`) -- magic, version, size, table counts -- the same
 *      first step the real loader takes before walking a book.
 *   3. For each chapter, walk its DOM subtree iteratively (NASA Rule 1: an
 *      explicit stack, no recursion) with the pure inline `ra_book.h`
 *      accessors, appending the chapter's text runs into a page accumulator.
 *      Every ::k_erm33_page_chars characters is one page: the M33 RENDERS those
 *      characters into the shared 1-bpp framebuffer (::erm33_framebuffer) with a
 *      compact baked-font mono blit (`m33_font.h`), bumps `page_idx` +
 *      `heartbeat`, publishes `frame_seq`, and HOLDS the rendered page until the
 *      M85 acknowledges it (`frame_ack`) -- exactly the idle / hold / turn loop
 *      an e-reader spends almost all its time in.
 *   4. On the last page publish `total_pages`, set `status = ok`, `done = 1`.
 *   5. Spin forever (the M85 reads each framebuffer, logs its CRC, and reports
 *      the verdict on the M33's behalf).
 *
 * @note Only the header-only `ra_book.h` inline accessors and the baked
 *       `m33_font.h` glyph table are used (pure offset arithmetic, no heap, no
 *       decompression, no logging), so this freestanding M33 image links nothing
 *       beyond its own code -- the same clean link as the sibling dual-core
 *       examples.
 * @note The M33 deliberately does NOT call `ra_log`: board_sim echoes only the
 *       primary core's ITM, so an M33 log line would be invisible. Its
 *       proof-of-life is the mailbox the M85 narrates and the framebuffer the
 *       M85 CRCs.
 * @note The framebuffer is the M33's own 1-bpp text plane in shared SRAM, not
 *       yet the GLCDC scan-out plane; swapping this compact blit for a full
 *       M33-side `ra_gfx` + panel path is the next increment (#150).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ereader_m33.h"
#include "m33_font.h"
#include "ra_attributes.h"
#include "ra_book.h"
#include "rabook_fixture.h"

/** @brief CPU1 stack top (slot 0 of the M33 vector table). */
extern uint32_t g_ra_ls_cpu1_stack_top;
/** @brief CPU1 `.data` run-region start (in SRAM_CPU1). */
extern uint32_t g_ra_ls_cpu1_data_start;
/** @brief CPU1 `.data` run-region end. */
extern uint32_t g_ra_ls_cpu1_data_end;
/** @brief CPU1 `.data` load image (in MRAM_CPU1). */
extern uint32_t g_ra_ls_cpu1_data_load;
/** @brief CPU1 `.bss` start (in SRAM_CPU1). */
extern uint32_t g_ra_ls_cpu1_bss_start;
/** @brief CPU1 `.bss` end. */
extern uint32_t g_ra_ls_cpu1_bss_end;

[[noreturn]] void cpu1_reset_handler(void);

/**
 * @enum m33_reader_bound_t
 * @brief Static iteration / delay bounds for the M33 reader (NASA Rule 2).
 * @details Each loop in this file is bounded by one of these compile-time
 *          constants so the M33 can never run unbounded on a corrupt blob or
 *          stall forever waiting on the M85.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_walk_iter_max = 4096U,     /**< Max DOM-walk pops per chapter.            */
  k_max_run_len   = 1024U,     /**< Bounded text-run length cap, bytes.       */
  k_ack_wait_max  = 20000000U, /**< Bounded spins holding a page for the M85. */
} m33_reader_bound_t;

/**
 * @enum m33_reader_size_t
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
} m33_reader_size_t;

/**
 * @struct m33_reader_ctx_t
 * @brief Running state threaded through the M33 reader / render path.
 *
 * @details Holds the shared mailbox + framebuffer pointers, the count of pages
 * emitted so far, the number of characters already buffered for the current
 * page, and the per-page text accumulator the renderer blits. One instance
 * lives on the reader's stack; it is passed by pointer to the walk / render
 * helpers so they never reach for module-global state.
 *
 * @invariant `col <= k_erm33_page_chars` at every call boundary.
 * @invariant `page_text[0 .. col-1]` are the characters of the page in flight.
 * @see cpu1_run_reader
 * @since 0.1.0
 */
typedef struct {
  volatile erm33_mailbox_t* mb;       /**< Shared cross-core progress mailbox.     */
  volatile uint8_t*         fb;       /**< Shared 1-bpp page framebuffer base.     */
  uint32_t                  page_ctr; /**< Pages emitted so far (1-based once >0). */
  uint32_t                  col;      /**< Characters buffered for the next page.  */
  char page_text[k_erm33_page_chars]; /**< Current page's text, `col` valid.       */
} m33_reader_ctx_t;

/**
 * @brief Zero a 1-bpp framebuffer span (clear it to all-background).
 *
 * @details Writes @p len background bytes to @p fb, clamped to the static
 * ::k_erm33_fb_bytes so a wrong length can never run off the plane. Called once
 * per page before the glyphs are blitted in.
 *
 * @param[out] fb  Framebuffer base (never NULL).
 * @param[in]  len Bytes to clear; expected to equal ::k_erm33_fb_bytes.
 *
 * @return Nothing.
 *
 * @pre @p fb addresses the shared framebuffer (::erm33_framebuffer).
 * @pre @p len is the true plane size in bytes.
 * @post The cleared span reads back as 0 (all background pixels).
 * @post Iteration bounded by ::k_erm33_fb_bytes (NASA Rule 2).
 *
 * @note Single-threaded; runs in M33 thread mode with no RTOS.
 * @since 0.1.0
 */
static void fb_clear(volatile uint8_t* fb, uint32_t len)
{
  if (fb == nullptr) {
    return;
  }
  if (len == 0U) {
    return;
  }
  RA_BOUNDED_LOOP(k_erm33_fb_bytes);
  for (uint32_t i = 0U; i < len; i++) {
    if (i >= (uint32_t)k_erm33_fb_bytes) {
      break;
    }
    fb[i] = 0U;
  }
}

/**
 * @brief Blit one 8x16 baked glyph into a framebuffer cell.
 *
 * @details Copies the 16 row-bytes of the glyph for @p ch out of the baked
 * `m33_font.h` table into the framebuffer at glyph cell (@p cell_col,
 * @p cell_row). Because each glyph is exactly 8 pixels wide (one byte) and the
 * plane is 1 bpp, the destination byte column equals the glyph column and the
 * blit is a plain per-row byte copy -- no bit shifting. A non-printable
 * codepoint is drawn as the blank space glyph.
 *
 * @param[in,out] fb        Framebuffer base (never NULL).
 * @param[in]     ch        Character codepoint to draw.
 * @param[in]     cell_col  Glyph column (0 .. ::k_erm33_fb_cols - 1).
 * @param[in]     cell_row  Glyph row (0 .. ::k_erm33_fb_rows - 1).
 *
 * @return Nothing.
 *
 * @pre @p fb addresses the shared framebuffer.
 * @pre The font table is the baked, immutable ::k_m33_font_8x16.
 * @post Out-of-range cells are dropped; nothing outside the plane is written.
 * @post Iteration bounded by ::k_m33_font_glyph_h (NASA Rule 2).
 *
 * @note Single-threaded; runs in M33 thread mode with no RTOS.
 * @since 0.1.0
 */
static void blit_glyph(volatile uint8_t* fb, uint8_t ch, uint32_t cell_col, uint32_t cell_row)
{
  if (fb == nullptr) {
    return;
  }
  if (cell_col >= (uint32_t)k_erm33_fb_cols) {
    return;
  }
  if (cell_row >= (uint32_t)k_erm33_fb_rows) {
    return;
  }
  uint8_t cp = ch;
  if (cp < (uint8_t)k_m33_font_first_cp) {
    cp = (uint8_t)k_m33_font_first_cp;
  }
  if (cp > (uint8_t)k_m33_font_last_cp) {
    cp = (uint8_t)k_m33_font_first_cp;
  }
  const uint32_t gi = ((uint32_t)cp - (uint32_t)k_m33_font_first_cp) * (uint32_t)k_m33_font_bytes;
  const uint32_t y0 = cell_row * (uint32_t)k_m33_font_glyph_h;
  RA_BOUNDED_LOOP(k_m33_font_glyph_h);
  for (uint32_t gy = 0U; gy < (uint32_t)k_m33_font_glyph_h; gy++) {
    fb[((y0 + gy) * (uint32_t)k_erm33_fb_stride) + cell_col] = k_m33_font_8x16[gi + gy];
  }
}

/**
 * @brief Render the buffered page text into the shared framebuffer.
 *
 * @details Clears the plane, then lays the @c ctx->col buffered characters out
 * left-to-right, wrapping to the next glyph row every ::k_erm33_fb_cols cells.
 * The layout loop is bounded by ::k_erm33_page_chars, the most a page can hold.
 *
 * @param[in,out] ctx Reader context with the buffered page (never NULL).
 *
 * @return Nothing.
 *
 * @pre @p ctx and @c ctx->fb are non-NULL.
 * @pre @c ctx->col is the count of valid characters in @c ctx->page_text.
 * @post The framebuffer shows the page's glyphs over a cleared background.
 * @post Iteration bounded by ::k_erm33_page_chars (NASA Rule 2).
 *
 * @note Single-threaded; runs in M33 thread mode with no RTOS.
 * @since 0.1.0
 */
static void render_page(m33_reader_ctx_t* ctx)
{
  if (ctx == nullptr) {
    return;
  }
  if (ctx->fb == nullptr) {
    return;
  }
  fb_clear(ctx->fb, (uint32_t)k_erm33_fb_bytes);
  const uint32_t n = ctx->col;
  RA_BOUNDED_LOOP(k_erm33_page_chars);
  for (uint32_t i = 0U; i < (uint32_t)k_erm33_page_chars; i++) {
    if (i >= n) {
      break;
    }
    const uint32_t cc = i % (uint32_t)k_erm33_fb_cols;
    const uint32_t cr = i / (uint32_t)k_erm33_fb_cols;
    blit_glyph(ctx->fb, (uint8_t)ctx->page_text[i], cc, cr);
  }
}

/**
 * @brief Render, publish, and hold one page until the M85 consumes it.
 *
 * @details Renders the buffered page into the framebuffer, advances the page
 * counter, mirrors it into `page_idx`, bumps the liveness `heartbeat`, drains
 * the write buffer with a `dsb` so the parked M85 sees stable pixels, then
 * publishes `frame_seq`. It then HOLDS the page -- a bounded spin until the M85
 * writes `frame_ack` back equal to this page -- so the framebuffer is never
 * overwritten before the M85 has CRC'd it. Finally it resets the accumulator.
 *
 * @param[in,out] ctx Reader context (never NULL).
 *
 * @return Nothing.
 *
 * @pre @p ctx and @c ctx->mb are non-NULL.
 * @pre @c ctx->col holds the characters of the page to emit.
 * @post `page_idx == frame_seq == ctx->page_ctr`, all published with a `dsb`.
 * @post @c ctx->col is 0; the hold loop is bounded by ::k_ack_wait_max.
 *
 * @note Single-threaded; runs in M33 thread mode with no RTOS.
 * @since 0.1.0
 */
static void emit_page(m33_reader_ctx_t* ctx)
{
  if (ctx == nullptr) {
    return;
  }
  if (ctx->mb == nullptr) {
    return;
  }
  volatile erm33_mailbox_t* mb = ctx->mb;
  render_page(ctx);
  ctx->page_ctr += 1U;
  mb->page_idx  = ctx->page_ctr;
  mb->heartbeat = mb->heartbeat + 1U;
  __asm volatile("dsb" ::: "memory");
  mb->frame_seq = ctx->page_ctr;
  __asm volatile("dsb" ::: "memory");
  RA_BOUNDED_LOOP(k_ack_wait_max);
  for (uint32_t w = 0U; w < (uint32_t)k_ack_wait_max; w++) {
    if (mb->frame_ack == ctx->page_ctr) {
      break;
    }
    __asm volatile("nop");
  }
  ctx->col = 0U;
}

/**
 * @brief Append one text run into the page accumulator, emitting full pages.
 *
 * @details Scans @p txt up to its NUL or the static ::k_max_run_len cap,
 * appending each character into @c ctx->page_text. Whenever the accumulator
 * reaches ::k_erm33_page_chars the page is rendered + emitted via ::emit_page
 * and the accumulator resets, so a long run spills across as many pages as it
 * needs. The scan is bounded by ::k_max_run_len.
 *
 * @param[in,out] ctx Reader context (never NULL).
 * @param[in]     txt NUL-terminated text run from the validated blob (may be NULL).
 *
 * @return Nothing.
 *
 * @pre @p ctx is non-NULL; @c ctx->page_text has room for ::k_erm33_page_chars.
 * @pre @p txt, when non-NULL, points inside the validated blob's string pool.
 * @post Every whole ::k_erm33_page_chars folded in emitted exactly one page.
 * @post @c ctx->col is the new sub-page remainder (`< ::k_erm33_page_chars`).
 *
 * @note Single-threaded; runs in M33 thread mode with no RTOS.
 * @since 0.1.0
 */
static void append_run(m33_reader_ctx_t* ctx, const char* txt)
{
  if (ctx == nullptr) {
    return;
  }
  if (txt == nullptr) {
    return;
  }
  RA_BOUNDED_LOOP(k_max_run_len);
  for (uint32_t i = 0U; i < (uint32_t)k_max_run_len; i++) {
    const char c = txt[i];
    if (c == '\0') {
      break;
    }
    if (ctx->col < (uint32_t)k_erm33_page_chars) {
      ctx->page_text[ctx->col] = c;
      ctx->col += 1U;
    }
    if (ctx->col >= (uint32_t)k_erm33_page_chars) {
      emit_page(ctx);
    }
  }
}

/**
 * @brief Walk one chapter's DOM subtree, rendering its text runs as they appear.
 *
 * @details Iterative pre-order walk (NASA Rule 1: an explicit index stack, no
 * recursion) rooted at the chapter's `root_node`. The root's own siblings are
 * never followed, so the walk stays inside this chapter. Each text node's run is
 * appended via ::append_run, which renders + emits pages as whole
 * ::k_erm33_page_chars accumulate. The pop loop is bounded by ::k_walk_iter_max.
 *
 * @param[in]     base       Validated `RABOOK1` blob base (never NULL).
 * @param[in]     root       Chapter root node index, or ::k_ra_book_nil.
 * @param[in]     node_count Node-table length (an out-of-range index is dropped).
 * @param[in,out] ctx        Reader context (never NULL).
 *
 * @return Nothing.
 *
 * @pre @p base was accepted by ::book_is_valid.
 * @pre @p ctx is non-NULL with a live mailbox + framebuffer.
 * @post Every text run in the chapter has been appended into the page render.
 * @post Iteration is bounded by ::k_walk_iter_max (NASA Rule 2).
 *
 * @note Single-threaded; runs in M33 thread mode with no RTOS.
 * @since 0.1.0
 */
static void
walk_chapter(const void* base, uint32_t root, uint32_t node_count, m33_reader_ctx_t* ctx)
{
  if (base == nullptr) {
    return;
  }
  if (root == (uint32_t)k_ra_book_nil) {
    return;
  }
  const ra_book_node_t* nodes = ra_book_nodes(base);
  uint32_t              stack[k_walk_stack_depth];
  uint32_t              sp = 0U;
  if (nodes[root].first_child != (uint32_t)k_ra_book_nil) {
    stack[sp++] = nodes[root].first_child;
  }
  RA_BOUNDED_LOOP(k_walk_iter_max);
  for (uint32_t it = 0U; it < (uint32_t)k_walk_iter_max; it++) {
    if (sp == 0U) {
      break;
    }
    const uint32_t n = stack[--sp];
    if (n >= node_count) {
      continue;
    }
    const ra_book_node_t* node = &nodes[n];
    if (node->next_sibling != (uint32_t)k_ra_book_nil) {
      if (sp < (uint32_t)k_walk_stack_depth) {
        stack[sp++] = node->next_sibling;
      }
    }
    if (node->kind == (uint8_t)k_ra_book_node_text) {
      append_run(ctx, ra_book_node_text(base, node));
    } else if (node->first_child != (uint32_t)k_ra_book_nil) {
      if (sp < (uint32_t)k_walk_stack_depth) {
        stack[sp++] = node->first_child;
      }
    }
  }
}

/**
 * @brief Validate the baked `RABOOK1` flat blob before walking it.
 *
 * @details Checks the "RABOOK1" magic, the format version, that the header's
 * `total_size` matches the baked length, and that the chapter / node counts are
 * non-zero and within sane caps. Mirrors the real loader's gate so the M33
 * never walks a malformed blob (the inline accessors do no bounds checking).
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
  if (size < (uint32_t)k_ra_book_sizeof_header) {
    return false;
  }
  const ra_book_header_t* hdr      = ra_book_header(base);
  static const char       expect[] = {'R', 'A', 'B', 'O', 'O', 'K', '1'};
  RA_BOUNDED_LOOP(k_magic_len);
  for (uint32_t i = 0U; i < (uint32_t)k_magic_len; i++) {
    if (hdr->magic[i] != expect[i]) {
      return false;
    }
  }
  if (hdr->format_version != (uint32_t)k_ra_book_format_version) {
    return false;
  }
  if (hdr->total_size != size) {
    return false;
  }
  if (hdr->chapter_count == 0U) {
    return false;
  }
  if (hdr->chapter_count > (uint32_t)k_max_chapters) {
    return false;
  }
  if (hdr->node_count == 0U) {
    return false;
  }
  if (hdr->node_count > (uint32_t)k_max_nodes) {
    return false;
  }
  return true;
}

/**
 * @brief Park the M33 forever once the reader has reported its verdict.
 *
 * @return This function never returns.
 * @retval (none) The core spins in place.
 *
 * @pre The mailbox already carries the final `status` / `done`.
 * @pre Entered only after the reader loop completes (or fails validation).
 * @post The M33 makes no further forward progress.
 * @post The published mailbox state is stable for the M85 to read.
 *
 * @note Mirrors the sibling dual-core park loops.
 * @since 0.1.0
 */
[[noreturn]] static void cpu1_park(void)
{
  while (1) {
    __asm volatile("nop");
  }
}

/**
 * @brief CPU1 reader entry: validate the baked book, render each page, report.
 *
 * @details Stamps the boot signature, validates the baked `RABOOK1` blob, then
 * walks every chapter -- rendering the extracted text into the shared
 * framebuffer one page at a time and turning pages through the mailbox -- and
 * finally publishes the total page count and `done`. A bad blob sets
 * `status = bad_book`, `done = 1`, and parks.
 *
 * @return This function never returns.
 * @retval (none) Control ends in ::cpu1_park.
 *
 * @pre `cpu1_reset_handler` has initialised `.data` / `.bss`.
 * @pre The M85 published ::k_erm33_magic and released this core.
 * @post `mb->done == 1` with `status` and `total_pages` reflecting the walk.
 * @post `page_idx` reached `total_pages` (the last page) on success.
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
    mb->status = (uint32_t)k_erm33_status_bad_book;
    __asm volatile("dsb" ::: "memory");
    mb->done = 1U;
    cpu1_park();
  }

  const ra_book_header_t*  hdr      = ra_book_header(base);
  const ra_book_chapter_t* chapters = ra_book_chapters(base);
  m33_reader_ctx_t         ctx      = {};
  ctx.mb                            = mb;
  ctx.fb                            = erm33_framebuffer();
  RA_BOUNDED_LOOP(k_max_chapters);
  for (uint32_t ci = 0U; ci < hdr->chapter_count; ci++) {
    mb->chapter_idx = ci;
    ctx.col         = 0U;
    walk_chapter(base, chapters[ci].root_node, hdr->node_count, &ctx);
    if (ctx.col > 0U) {
      emit_page(&ctx); /* render + flush the chapter's partial last page */
    }
  }

  mb->total_pages = ctx.page_ctr;
  __asm volatile("dsb" ::: "memory");
  mb->status = (uint32_t)k_erm33_status_ok;
  __asm volatile("dsb" ::: "memory");
  mb->done = 1U;
  cpu1_park();
}

/**
 * @brief CPU1 reset handler: minimal C-runtime init, then run the reader.
 *
 * @details The M33 boots with uninitialised RAM, so before any C code runs this
 * copies `.data` from its MRAM_CPU1 load image into SRAM_CPU1 and zeroes `.bss`.
 * The linker exports the region bounds as `g_ra_ls_cpu1_*` symbols.
 *
 * @return This function never returns.
 * @retval (none) Control passes to ::cpu1_run_reader, which loops forever.
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
  uint32_t* dst = &g_ra_ls_cpu1_data_start;
  uint32_t* src = &g_ra_ls_cpu1_data_load;
  while (dst < &g_ra_ls_cpu1_data_end) {
    *dst = *src;
    dst++;
    src++;
  }

  uint32_t* bss = &g_ra_ls_cpu1_bss_start;
  while (bss < &g_ra_ls_cpu1_bss_end) {
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
 * @retval (none) The core spins in place.
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
#ifndef RA_SIMULATOR_MODE
/* The vector table is only meaningful in the cross-compiled M33 image. The host
 * unit-test build compile-checks this TU but never links it as an executable, so
 * dropping the table there costs no coverage. */
[[gnu::used, gnu::section(".cpu1_vectors")]] const uintptr_t g_cpu1_vector_table[] = {
  (uintptr_t)&g_ra_ls_cpu1_stack_top,
  (uintptr_t)&cpu1_reset_handler,
  (uintptr_t)&cpu1_fault_handler,
  (uintptr_t)&cpu1_fault_handler,
  (uintptr_t)&cpu1_fault_handler,
  (uintptr_t)&cpu1_fault_handler,
  (uintptr_t)&cpu1_fault_handler,
  (uintptr_t)&cpu1_fault_handler,
};
#endif
