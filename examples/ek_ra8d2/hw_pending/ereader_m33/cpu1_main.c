/**
 * @file examples/ek_ra8d2/hw_pending/ereader_m33/cpu1_main.c
 * @brief CPU1 (Cortex-M33 secondary core) image: run the e-reader page loop
 *
 * @par Tag
 * [Ring 1 / app] {World: NS}
 *
 * @details
 * This is the firmware that runs on the RA8D2's *second* core, the Cortex-M33,
 * for the #150 power-saving demo. It is compiled as a wholly separate ELF
 * (`-mcpu=cortex-m33`) and embedded into the M85 ELF as a `.cpu1_image` blob;
 * the M85 hands the reader to this core via `ra_cpu1_release` (HUM Ch 2.9.1)
 * and PARKS. From then on the M33 is the live core running the e-reader.
 *
 * The M33 runs the reader's actual *data* path on a real compiled book:
 *
 *   1. Stamp ::k_erm33_m33_sig into the shared mailbox so the parked M85 can
 *      prove the M33 left reset and is executing user code.
 *   2. Validate the baked, already-inflated `RABOOK1` flat blob
 *      (`rabook_fixture.h`) -- magic, version, size, table counts -- the same
 *      first step the real loader takes before walking a book.
 *   3. For each chapter, walk its DOM subtree iteratively (NASA Rule 1: an
 *      explicit stack, no recursion) with the pure inline `ra_book.h`
 *      accessors, summing the lengths of the chapter's text runs. Every
 *      ::k_erm33_page_chars characters is one rendered page: the M33 "turns"
 *      the page -- bumping `page_idx` + `heartbeat` in the mailbox and holding
 *      it for a bounded busy-delay, exactly the idle / hold / turn loop an
 *      e-reader spends almost all its time in.
 *   4. On the last page publish `total_pages`, set `status = ok`, `done = 1`.
 *   5. Spin forever (the M85 logs the verdict on the M33's behalf).
 *
 * @note Only the header-only `ra_book.h` inline accessors are used (pure offset
 *       arithmetic, no heap, no decompression, no logging), so this freestanding
 *       M33 image links nothing beyond its own code -- the same clean link as
 *       the sibling dual-core examples.
 * @note The M33 deliberately does NOT call `ra_log`: board_sim echoes only the
 *       primary core's ITM, so an M33 log line would be invisible. Its
 *       proof-of-life is the mailbox the M85 narrates.
 * @note Pixel rendering on the M33 is the *next* increment (#150): it needs the
 *       display plane + framebuffer handed to the M33 and an M33-side `ra_gfx`
 *       path. This increment proves the secondary core runs the reader's data
 *       path; the page count it walks is real, the framebuffer is not yet.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ereader_m33.h"
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
 *          constants so the M33 can never run unbounded on a corrupt blob.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_walk_iter_max = 4096U,  /**< Max DOM-walk pops per chapter.          */
  k_max_run_len   = 1024U,  /**< Bounded text-run length cap, bytes.     */
  k_hold_spins    = 40000U, /**< Bounded busy-delay spins per page turn. */
} m33_reader_bound_t;

/**
 * @enum m33_reader_size_t
 * @brief Small table / validation sizes for the M33 reader.
 * @details The DOM-walk stack depth, per-run page cap, and the validation caps
 *          that reject an implausible blob before the walk begins.
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_walk_stack_depth  = 64U,   /**< Explicit DOM-walk stack depth.        */
  k_max_pages_per_run = 64U,   /**< Max pages a single text run may turn. */
  k_max_chapters      = 32U,   /**< Validation cap on `chapter_count`.    */
  k_max_nodes         = 4096U, /**< Validation cap on `node_count`.       */
  k_magic_len         = 7U,    /**< "RABOOK1" magic length (no NUL).      */
} m33_reader_size_t;

/**
 * @brief Bounded string length of an interned text run.
 *
 * @details Counts bytes up to the first NUL or @p cap, whichever comes first,
 * so a corrupt (unterminated) run can never run the walk off the blob.
 *
 * @param[in] s   NUL-terminated text run from the blob (may be NULL).
 * @param[in] cap Maximum bytes to scan (the static ::k_max_run_len cap).
 *
 * @return Number of bytes before the NUL, clamped to @p cap.
 * @retval 0 @p s is NULL or @p cap is 0.
 *
 * @pre @p s, when non-NULL, points inside the validated blob's string pool.
 * @pre @p cap equals ::k_max_run_len (the bound the loop is proven against).
 * @post The result is at most @p cap.
 * @post @p s is not modified.
 *
 * @note Pure; safe from any context. Avoids a libc `strlen` dependency.
 * @since 0.1.0
 */
static uint32_t m33_strnlen(const char* s, uint32_t cap)
{
  if (s == nullptr) {
    return 0U;
  }
  if (cap == 0U) {
    return 0U;
  }
  uint32_t i = 0U;
  RA_BOUNDED_LOOP(k_max_run_len);
  for (i = 0U; i < cap; i++) {
    if (s[i] == '\0') {
      break;
    }
  }
  return i;
}

/**
 * @brief Turn one page: advance the counter, publish it, hold the page.
 *
 * @details Increments the running page counter, mirrors it into the mailbox
 * `page_idx`, bumps the liveness `heartbeat`, drains the write buffer with a
 * `dsb` so the parked M85 sees the new page, then holds the page for a bounded
 * busy-delay -- the slow core's "hold a rendered page" posture.
 *
 * @param[in,out] mb       Shared mailbox (never NULL).
 * @param[in,out] page_ctr Running page counter advanced by one (never NULL).
 *
 * @return Nothing.
 *
 * @pre @p mb is the fixed-address mailbox pointer.
 * @pre @p page_ctr addresses the caller's page accumulator.
 * @post `*page_ctr` increased by 1 and is mirrored in `mb->page_idx`.
 * @post `mb->heartbeat` increased by 1; a `dsb` has published both.
 *
 * @note Single-threaded; runs in M33 thread mode with no RTOS.
 * @since 0.1.0
 */
static void turn_page(volatile erm33_mailbox_t* mb, uint32_t* page_ctr)
{
  if (mb == nullptr) {
    return;
  }
  if (page_ctr == nullptr) {
    return;
  }
  *page_ctr += 1U;
  mb->page_idx  = *page_ctr;
  mb->heartbeat = mb->heartbeat + 1U;
  __asm volatile("dsb" ::: "memory");
  RA_BOUNDED_LOOP(k_hold_spins);
  for (volatile uint32_t d = 0U; d < (uint32_t)k_hold_spins; d++) {
    __asm volatile("nop");
  }
}

/**
 * @brief Fold a text run into the page accumulator, turning full pages.
 *
 * @details Adds @p run_len characters to @p acc, then turns one page for every
 * whole ::k_erm33_page_chars it now holds, carrying the remainder for the next
 * run. The page-turn loop is bounded by ::k_max_pages_per_run so even a very
 * long run cannot spin unbounded.
 *
 * @param[in,out] mb       Shared mailbox (never NULL).
 * @param[in,out] page_ctr Running page counter (never NULL).
 * @param[in,out] acc      Carry of leftover characters not yet paged (never NULL).
 * @param[in]     run_len  Length of the text run just walked, in characters.
 *
 * @return Nothing.
 *
 * @pre @p mb, @p page_ctr and @p acc are non-NULL.
 * @pre @p acc holds the leftover characters from prior runs in this chapter.
 * @post Each ::k_erm33_page_chars folded in turned exactly one page.
 * @post `*acc` is the new sub-page remainder (`< ::k_erm33_page_chars`).
 *
 * @note Single-threaded; runs in M33 thread mode with no RTOS.
 * @since 0.1.0
 */
static void
account_text(volatile erm33_mailbox_t* mb, uint32_t* page_ctr, uint32_t* acc, uint32_t run_len)
{
  if (mb == nullptr) {
    return;
  }
  if (acc == nullptr) {
    return;
  }
  *acc += run_len;
  RA_BOUNDED_LOOP(k_max_pages_per_run);
  for (uint32_t k = 0U; k < (uint32_t)k_max_pages_per_run; k++) {
    if (*acc < (uint32_t)k_erm33_page_chars) {
      break;
    }
    *acc -= (uint32_t)k_erm33_page_chars;
    turn_page(mb, page_ctr);
  }
}

/**
 * @brief Walk one chapter's DOM subtree, paging its text runs as they appear.
 *
 * @details Iterative pre-order walk (NASA Rule 1: an explicit index stack, no
 * recursion) rooted at the chapter's `root_node`. The root's own siblings are
 * never followed, so the walk stays inside this chapter. Each text node folds
 * its run length into @p acc via ::account_text, which turns pages as whole
 * ::k_erm33_page_chars accumulate. The pop loop is bounded by ::k_walk_iter_max.
 *
 * @param[in]     base       Validated `RABOOK1` blob base (never NULL).
 * @param[in]     root       Chapter root node index, or ::k_ra_book_nil.
 * @param[in]     node_count Node-table length (an out-of-range index is dropped).
 * @param[in,out] mb         Shared mailbox (never NULL).
 * @param[in,out] page_ctr   Running page counter (never NULL).
 * @param[in,out] acc        Leftover-character carry for this chapter (never NULL).
 *
 * @return Nothing.
 *
 * @pre @p base was accepted by ::book_is_valid.
 * @pre @p mb, @p page_ctr and @p acc are non-NULL.
 * @post Every text run in the chapter has been folded into the page accounting.
 * @post Iteration is bounded by ::k_walk_iter_max (NASA Rule 2).
 *
 * @note Single-threaded; runs in M33 thread mode with no RTOS.
 * @since 0.1.0
 */
static void walk_chapter(const void*               base,
                         uint32_t                  root,
                         uint32_t                  node_count,
                         volatile erm33_mailbox_t* mb,
                         uint32_t*                 page_ctr,
                         uint32_t*                 acc)
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
      const char* txt = ra_book_node_text(base, node);
      account_text(mb, page_ctr, acc, m33_strnlen(txt, (uint32_t)k_max_run_len));
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
 * @brief CPU1 reader entry: validate the baked book, page through it, report.
 *
 * @details Stamps the boot signature, validates the baked `RABOOK1` blob, then
 * walks every chapter -- paging the extracted text and turning pages through
 * the mailbox -- and finally publishes the total page count and `done`. A bad
 * blob sets `status = bad_book`, `done = 1`, and parks.
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
  uint32_t                 page_ctr = 0U;
  RA_BOUNDED_LOOP(k_max_chapters);
  for (uint32_t ci = 0U; ci < hdr->chapter_count; ci++) {
    mb->chapter_idx = ci;
    uint32_t acc    = 0U;
    walk_chapter(base, chapters[ci].root_node, hdr->node_count, mb, &page_ctr, &acc);
    if (acc > 0U) {
      turn_page(mb, &page_ctr); /* flush the chapter's partial last page */
    }
  }

  mb->total_pages = page_ctr;
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
__attribute__((used, section(".cpu1_vectors"))) const uintptr_t g_cpu1_vector_table[] = {
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
