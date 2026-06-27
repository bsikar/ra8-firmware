/**
 * @file examples/ek_ra8d2/hw_pending/compile_on_m33/cpu1_main.c
 * @brief CPU1 (Cortex-M33 secondary core) image: run the RABOOK1 emitter
 *
 * @par Tag
 * [Ring 6 / APP] {World: NS}
 *
 * @details
 * This is the firmware that runs on the RA8D2's *second* core, the Cortex-M33,
 * for the #149(b) compiler-back-end offload. It is compiled as a wholly separate
 * ELF (`-mcpu=cortex-m33`) and embedded into the M85 ELF as a `.cpu1_image` blob;
 * the M85 hands the work to this core via `ra_cpu1_release` (HUM Ch 2.9.1) and
 * PARKS.
 *
 * The M33 runs the `.rabook` compiler's *back-end* -- the RABOOK1 emitter
 * (`libs/ra_rabook_compile`) -- on a hand-built tiny DOM, exactly the data path
 * the on-device EPUB->`.rabook` import would offload to the slow core:
 *
 *   1. Stamp ::k_com33_m33_sig into the shared mailbox so the parked M85 can
 *      prove the M33 left reset and is executing user code.
 *   2. Bind the zero-heap emitter to caller-owned arenas (scratch tables in the
 *      M33's own SRAM_CPU1 `.bss`; the output buffer in shared SRAM so the M85
 *      reads the finished blob with no copy).
 *   3. Drive the emitter API over a 2-chapter book DOM: intern strings, add
 *      `<body>/<h1>/<p>` element + text nodes, link them, add the spine chapters
 *      and the title/author/language/identifier metadata.
 *   4. `ra_rabook_finalize` lays the tables + pools, fills the 100-byte header
 *      and the body CRC-32 straight into the shared output buffer.
 *   5. Publish `blob_base` / `blob_len` / `blob_crc` / `chapter_count`, set
 *      `status = ok`, `done = 1`. The M85 then validates the blob.
 *
 * @note The emitter is zero-allocation (NASA Rule 3): it only appends into the
 *       fixed arenas this file owns, so the freestanding M33 image links nothing
 *       beyond its own code plus `ra_rabook_compile.c` and newlib's
 *       `memcpy`/`memset`/`strlen`. Logging is compiled out (`RA_LOG_LEVEL=0` on
 *       this image) so no `ra_log` backend is pulled into the second core.
 * @note The M33 deliberately does NOT call `ra_log`: board_sim echoes only the
 *       primary core's ITM, so an M33 log line would be invisible. Its
 *       proof-of-life is the mailbox the M85 narrates.
 * @note A full `ra_epub_open` (unzip + XHTML parse + image transcode) on the M33
 *       is the *next*, heavier #149(b) increment: it needs miniz, tinyxml2 and
 *       stb_image linked into the second core. This increment proves the emitter
 *       back-end runs there.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "compile_on_m33.h"
#include "ra_attributes.h"
#include "ra_book.h"
#include "ra_err.h"
#include "ra_rabook_compile.h"

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
 * @enum m33_arena_cap_t
 * @brief Fixed capacities of the emitter's caller-owned scratch arenas.
 * @details Sized for the tiny 2-chapter book this demo builds, with comfortable
 *          slack: a handful of chapters, a few dozen DOM nodes, and a small
 *          string pool. The image and stylesheet arenas exist only because
 *          @ref ra_rabook_compile_init requires every member pointer to be
 *          non-NULL; this demo emits neither. All of these live in the M33
 *          image's own SRAM_CPU1 `.bss` (~1.5 KiB total).
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_arena_chapters = 4U,   /**< Chapter-table arena entries.    */
  k_arena_nodes    = 32U,  /**< Node-table arena entries.       */
  k_arena_attrs    = 8U,   /**< Attribute-table arena entries.  */
  k_arena_styles   = 2U,   /**< Stylesheet-table arena entries. */
  k_arena_images   = 2U,   /**< Image-table arena entries.      */
  k_arena_string   = 512U, /**< String-pool capacity in bytes.  */
  k_arena_imgpool  = 16U,  /**< Image-pool capacity in bytes.   */
} m33_arena_cap_t;

/**
 * @struct m33_arenas_t
 * @brief All of the emitter's caller-owned scratch arenas in one block.
 * @details Bundling the per-table arenas into a single struct gives them one
 *          documented home in the M33 image's `.bss`. @ref bind_buffers points
 *          a @ref ra_rabook_buffers_t at these members; the emitter only appends
 *          into them and never allocates. The finalized blob is NOT here -- it is
 *          laid into the shared output buffer (see @ref com33_blob).
 * @invariant Each array holds at least its matching ::m33_arena_cap_t capacity.
 * @since 0.1.0
 */
typedef struct {
  ra_book_chapter_t    chapters[k_arena_chapters];  /**< Chapter-table arena.    */
  ra_book_node_t       nodes[k_arena_nodes];        /**< Node-table arena.       */
  ra_book_attr_t       attrs[k_arena_attrs];        /**< Attribute-table arena.  */
  ra_book_stylesheet_t styles[k_arena_styles];      /**< Stylesheet-table arena. */
  ra_book_image_t      images[k_arena_images];      /**< Image-table arena.      */
  char                 string_pool[k_arena_string]; /**< String-pool arena.      */
  uint8_t              image_pool[k_arena_imgpool]; /**< Image-pool arena.       */
} m33_arenas_t;

/**
 * @var s_arenas
 * @brief The single static instance of the emitter's scratch arenas.
 * @details Lives in the M33 image's SRAM_CPU1 `.bss`; zeroed by
 *          @ref cpu1_reset_handler before any C code runs.
 * @note Single-owner: only the M33 emitter path touches it.
 * @warning Not shared with the M85; do not place cross-core state here.
 * @since 0.1.0
 */
static m33_arenas_t s_arenas;

/**
 * @brief Point a builder buffer set at the static scratch arenas + shared output.
 *
 * @param[out] buf Buffer descriptor to fill (never NULL).
 *
 * @return Whether the buffers were bound.
 * @retval true  @p buf now references every arena plus the shared output buffer.
 * @retval false @p buf is NULL, or the shared output address resolved to NULL.
 *
 * @pre @p buf is an uninitialised (or zeroed) buffer descriptor.
 * @pre ::s_arenas is zeroed (the reset handler cleared `.bss`).
 * @post On true, every `*` member of @p buf is non-NULL and each `_cap` is set.
 * @post On true, `buf->out` addresses the shared blob buffer at ::k_com33_blob_addr.
 *
 * @note Single-threaded; runs in M33 thread mode with no RTOS.
 * @since 0.1.0
 */
static bool bind_buffers(ra_rabook_buffers_t* buf)
{
  if (buf == nullptr) {
    return false;
  }
  uint8_t* out = com33_blob();
  if (out == nullptr) {
    return false;
  }
  buf->chapters       = s_arenas.chapters;
  buf->chapter_cap    = (uint32_t)k_arena_chapters;
  buf->nodes          = s_arenas.nodes;
  buf->node_cap       = (uint32_t)k_arena_nodes;
  buf->attrs          = s_arenas.attrs;
  buf->attr_cap       = (uint32_t)k_arena_attrs;
  buf->stylesheets    = s_arenas.styles;
  buf->stylesheet_cap = (uint32_t)k_arena_styles;
  buf->images         = s_arenas.images;
  buf->image_cap      = (uint32_t)k_arena_images;
  buf->string_pool    = s_arenas.string_pool;
  buf->string_cap     = (uint32_t)k_arena_string;
  buf->image_pool     = s_arenas.image_pool;
  buf->image_pool_cap = (uint32_t)k_arena_imgpool;
  buf->out            = out;
  buf->out_cap        = (uint32_t)k_com33_blob_cap;
  return true;
}

/**
 * @brief Append an element node carrying a single text child; return its index.
 *
 * @details Interns @p tag and @p text, adds the element and the text node, then
 * links the text node as the element's only child. The element has no attributes.
 *
 * @param[in,out] ctx  Initialised builder context (never NULL).
 * @param[in]     tag  Element tag name to intern (never NULL).
 * @param[in]     text Text-run content to intern (never NULL).
 *
 * @return The element node index, or @ref k_ra_book_nil on any failure.
 * @retval k_ra_book_nil A pointer argument was NULL or an arena overflowed.
 *
 * @pre @p ctx was initialised by @ref ra_rabook_compile_init.
 * @pre @p tag and @p text are NUL-terminated strings.
 * @post On success the element node has the interned tag and one text child.
 * @post On failure the builder's sticky `failed` flag reflects any overflow.
 *
 * @note Single-threaded; runs in M33 thread mode with no RTOS.
 * @since 0.1.0
 */
static uint32_t add_text_el(ra_rabook_ctx_t* ctx, const char* tag, const char* text)
{
  if (ctx == nullptr) {
    return (uint32_t)k_ra_book_nil;
  }
  if (tag == nullptr) {
    return (uint32_t)k_ra_book_nil;
  }
  if (text == nullptr) {
    return (uint32_t)k_ra_book_nil;
  }
  const uint32_t el = ra_rabook_add_element(ctx, ra_rabook_intern(ctx, tag), nullptr, 0U);
  const uint32_t tx = ra_rabook_add_text(ctx, ra_rabook_intern(ctx, text));
  if (el == (uint32_t)k_ra_book_nil) {
    return (uint32_t)k_ra_book_nil;
  }
  if (tx == (uint32_t)k_ra_book_nil) {
    return (uint32_t)k_ra_book_nil;
  }
  (void)ra_rabook_link_child(ctx, el, tx);
  return el;
}

/**
 * @brief Build one chapter body: a `<body>` with an `<h1>` then a `<p>` child.
 *
 * @param[in,out] ctx     Initialised builder context (never NULL).
 * @param[in]     heading Heading text for the chapter's `<h1>` (never NULL).
 * @param[in]     para    Body text for the chapter's `<p>` (never NULL).
 *
 * @return The `<body>` root node index, or @ref k_ra_book_nil on any failure.
 * @retval k_ra_book_nil A pointer argument was NULL or an arena overflowed.
 *
 * @pre @p ctx was initialised by @ref ra_rabook_compile_init.
 * @pre @p heading and @p para are NUL-terminated strings.
 * @post On success the body's child chain is `<h1> -> <p>`.
 * @post On failure the builder's sticky `failed` flag reflects any overflow.
 *
 * @note Single-threaded; runs in M33 thread mode with no RTOS.
 * @since 0.1.0
 */
static uint32_t build_chapter_body(ra_rabook_ctx_t* ctx, const char* heading, const char* para)
{
  if (ctx == nullptr) {
    return (uint32_t)k_ra_book_nil;
  }
  if (heading == nullptr) {
    return (uint32_t)k_ra_book_nil;
  }
  const uint32_t body = ra_rabook_add_element(ctx, ra_rabook_intern(ctx, "body"), nullptr, 0U);
  const uint32_t h1   = add_text_el(ctx, "h1", heading);
  const uint32_t p    = add_text_el(ctx, "p", para);
  if (body == (uint32_t)k_ra_book_nil) {
    return (uint32_t)k_ra_book_nil;
  }
  if (h1 == (uint32_t)k_ra_book_nil) {
    return (uint32_t)k_ra_book_nil;
  }
  if (p == (uint32_t)k_ra_book_nil) {
    return (uint32_t)k_ra_book_nil;
  }
  (void)ra_rabook_link_child(ctx, body, h1);
  (void)ra_rabook_link_sibling(ctx, h1, p);
  return body;
}

/**
 * @brief Drive the emitter over a hand-built 2-chapter book DOM.
 *
 * @details Interns the metadata strings, builds two chapter bodies, appends the
 * two spine chapters and records the book metadata. Every emitter call is checked
 * so a single arena overflow short-circuits to a clean failure that
 * @ref ra_rabook_finalize would also have reported via its sticky flag.
 *
 * @param[in,out] ctx Initialised builder context (never NULL).
 *
 * @return Whether the full DOM was built and the metadata recorded.
 * @retval true  Two chapters and the metadata were appended without overflow.
 * @retval false @p ctx is NULL or an emitter call failed (arena overflow).
 *
 * @pre @p ctx was initialised by @ref ra_rabook_compile_init.
 * @pre The bound arenas are large enough for this small book.
 * @post On true the builder holds two chapters and a complete metadata record.
 * @post On false the builder must not be finalized into a usable blob.
 *
 * @note Single-threaded; runs in M33 thread mode with no RTOS.
 * @since 0.1.0
 */
static bool build_book(ra_rabook_ctx_t* ctx)
{
  if (ctx == nullptr) {
    return false;
  }
  const uint32_t title  = ra_rabook_intern(ctx, "RA8D2 M33-Compiled Book");
  const uint32_t author = ra_rabook_intern(ctx, "Cortex-M33 Emitter");
  const uint32_t lang   = ra_rabook_intern(ctx, "en");
  const uint32_t ident  = ra_rabook_intern(ctx, "urn:ra8d2:compile-on-m33");

  const uint32_t body0 =
    build_chapter_body(ctx, "Chapter One", "Hello from the Cortex-M33 RABOOK1 emitter.");
  const uint32_t body1 =
    build_chapter_body(ctx, "Chapter Two", "The secondary core built this compiled book.");
  if (body0 == (uint32_t)k_ra_book_nil) {
    return false;
  }
  if (body1 == (uint32_t)k_ra_book_nil) {
    return false;
  }

  const uint32_t c0 = ra_rabook_add_chapter(ctx,
                                            ra_rabook_intern(ctx, "Chapter One"),
                                            ra_rabook_intern(ctx, "ch1.xhtml"),
                                            body0);
  const uint32_t c1 = ra_rabook_add_chapter(ctx,
                                            ra_rabook_intern(ctx, "Chapter Two"),
                                            ra_rabook_intern(ctx, "ch2.xhtml"),
                                            body1);
  if (c0 == (uint32_t)k_ra_book_nil) {
    return false;
  }
  if (c1 == (uint32_t)k_ra_book_nil) {
    return false;
  }

  const ra_err_t rc =
    ra_rabook_set_metadata(ctx, title, author, lang, ident, (uint32_t)k_ra_book_nil);
  return rc == k_ra_ok;
}

/**
 * @brief Publish the emitter result into the shared mailbox.
 *
 * @details On success copies the blob base, length, the blob header's body
 * CRC-32 and the chapter count into the mailbox, drains the write buffer, sets
 * `status = ok` then `done = 1`. On failure sets `status = build_fail` and
 * `done = 1`. The two `dsb`s order the payload before `status`, and `status`
 * before `done`, so the parked M85 reads a settled mailbox.
 *
 * @param[in,out] mb   Shared mailbox (never NULL).
 * @param[in]     blob Finalized blob base, or NULL on a build failure.
 * @param[in]     len  Finalized blob length in bytes (0 on failure).
 * @param[in]     ok   Whether the emitter finalized a blob.
 *
 * @return Nothing.
 *
 * @pre @p mb is the fixed-address mailbox pointer.
 * @pre On @p ok, @p blob points at a finalized RABOOK1 blob of @p len bytes.
 * @post `mb->done == 1` with `status` reflecting success or failure.
 * @post On success the blob base / length / CRC / chapter count are published.
 *
 * @note Single-threaded; runs in M33 thread mode with no RTOS.
 * @since 0.1.0
 */
static void publish_result(volatile com33_mailbox_t* mb, const void* blob, uint32_t len, bool ok)
{
  if (mb == nullptr) {
    return;
  }
  bool good = ok;
  if (blob == nullptr) {
    good = false;
  }
  if (len == 0U) {
    good = false;
  }
  if (good) {
    const ra_book_header_t* hdr = ra_book_header(blob);
    mb->blob_base               = (uint32_t)(uintptr_t)blob;
    mb->blob_len                = len;
    mb->blob_crc                = hdr->crc32;
    mb->chapter_count           = hdr->chapter_count;
    __asm volatile("dsb" ::: "memory");
    mb->status = (uint32_t)k_com33_status_ok;
  } else {
    mb->status = (uint32_t)k_com33_status_build_fail;
  }
  __asm volatile("dsb" ::: "memory");
  mb->done = 1U;
}

/**
 * @brief Park the M33 forever once the emitter has reported its result.
 *
 * @return This function never returns.
 * @retval (none) The core spins in place.
 *
 * @pre The mailbox already carries the final `status` / `done`.
 * @pre Entered only after the emitter run completes (or fails to build).
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
 * @brief CPU1 emitter entry: build the book, finalize the blob, report.
 *
 * @details Stamps the boot signature, binds the emitter to its arenas, builds
 * the DOM, finalizes the RABOOK1 blob into the shared output buffer, then
 * publishes the result for the parked M85. Any failure publishes `build_fail`.
 *
 * @return This function never returns.
 * @retval (none) Control ends in ::cpu1_park.
 *
 * @pre `cpu1_reset_handler` has initialised `.data` / `.bss`.
 * @pre The M85 published ::k_com33_magic and released this core.
 * @post `mb->done == 1` with `status` reflecting the emitter outcome.
 * @post On success the shared output buffer holds a finalized RABOOK1 blob.
 *
 * @note Single-threaded; runs in M33 thread mode with no RTOS.
 * @since 0.1.0
 */
[[noreturn]] static void cpu1_run_emitter(void)
{
  volatile com33_mailbox_t* mb = com33_mailbox();
  mb->m33_sig                  = (uint32_t)k_com33_m33_sig;
  __asm volatile("dsb" ::: "memory");

  ra_rabook_buffers_t buf = {};
  bool                ok  = bind_buffers(&buf);

  ra_rabook_ctx_t ctx = {};
  if (ok) {
    ok = (ra_rabook_compile_init(&ctx, &buf) == k_ra_ok);
  }
  if (ok) {
    ok = build_book(&ctx);
  }

  const void* blob = nullptr;
  uint32_t    len  = 0U;
  if (ok) {
    ok = (ra_rabook_finalize(&ctx, &blob, &len) == k_ra_ok);
  }

  publish_result(mb, blob, len, ok);
  cpu1_park();
}

/**
 * @brief CPU1 reset handler: minimal C-runtime init, then run the emitter.
 *
 * @details The M33 boots with uninitialised RAM, so before any C code runs this
 * copies `.data` from its MRAM_CPU1 load image into SRAM_CPU1 and zeroes `.bss`.
 * The linker exports the region bounds as `g_ra_ls_cpu1_*` symbols.
 *
 * @return This function never returns.
 * @retval (none) Control passes to ::cpu1_run_emitter, which loops forever.
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
  RA_BOUNDED_LOOP(g_ra_ls_cpu1_data_end);
  while (dst < &g_ra_ls_cpu1_data_end) {
    *dst = *src;
    dst++;
    src++;
  }

  uint32_t* bss = &g_ra_ls_cpu1_bss_start;
  RA_BOUNDED_LOOP(g_ra_ls_cpu1_bss_end);
  while (bss < &g_ra_ls_cpu1_bss_end) {
    *bss = 0U;
    bss++;
  }

  cpu1_run_emitter();
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
 *       completion is visible to the M85 as a stalled, never-done emitter.
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
