/**
 * @file examples/ek_ra8d2/hw_pending/compile_on_m33/src/cpu1_main.c
 * @brief CPU1 (Cortex-M33 secondary core) image: full text/CSS/SVG EPUB->RABOOK1 compile
 *
 * @par Tag
 * [Ring 6 / APP] {World: NS}
 *
 * @details
 * This is the firmware that runs on the RA8D2's *second* core, the Cortex-M33,
 * for the #149 compiler offload. It is compiled as a wholly separate ELF
 * (`-mcpu=cortex-m33`) and embedded into the M85 ELF as a `.cpu1_image` blob;
 * the M85 hands the work to this core via `ra8_cpu1_release` (HUM Ch 2.9.1) and
 * PARKS while the slow core compiles the book.
 *
 * The M33 runs the FULL text/CSS/SVG EPUB->`.rabook` compile over a baked parity
 * fixture `.epub` -- the exact data path the on-device import offloads:
 *
 *   1. Stamp ::k_com33_m33_sig into the shared mailbox so the parked M85 can
 *      prove the M33 left reset and is executing user code, then read the job the
 *      M85 posted (the staged .epub base/len + output buffer) from the request.
 *   2. `epub_open` the M85-staged .epub (shared SRAM, ::k_com33_epub_addr) as
 *      an in-memory media: miniz unzips the container and the bounded XML reader
 *      parses the OPF spine/manifest. The M33 compiles whatever the M85 staged,
 *      so the same image serves any book (not a baked-in fixture).
 *   3. `rabook_compile_from_epub_to_buffer` runs the same pipeline the M85
 *      proved byte-identical to the desktop tool -- stylesheets, SVG images
 *      (verbatim; raster is compiled out via `RA8_RABOOK_NO_RASTER`, so no
 *      stb_image), the chapter DOM walk, metadata, then `ra8_rabook_finalize`
 *      into the shared output buffer. The builder arenas + `epub_book_t` live
 *      in external SDRAM (`.sdram_bss`); SRAM_CPU1 holds only small state + stack.
 *   4. Publish `blob_base` / `blob_len` / `blob_crc` / `chapter_count`, set
 *      `status = ok`, `done = 1`. The M85 validates the blob AND byte-compares it
 *      to the same golden the desktop/M85 path produced -- proving the compile is
 *      byte-identical on the secondary core.
 *
 * @note Zero dynamic allocation reaching real malloc (NASA Rule 3): miniz uses
 *       the bounded epub arena and XML uses explicit caller workspace. The
 *       freestanding M33 image links the front end and back end (miniz,
 *       xml, epub, rabook_compile/pipeline/gray4) -- no stb_image, no
 *       ra8_fs (the M85 owns the filesystem; this core finalizes into a buffer).
 * @note The M33 deliberately does NOT call `ra8_log`: ra8_emulator echoes only the
 *       primary core's ITM, so an M33 log line would be invisible. Its
 *       proof-of-life is the mailbox the M85 narrates.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "book.h"
#include "compile_on_m33.h"
#include "epub.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_ipc.h"
#include "ra8_rabook_pipeline.h"
#include "rabook_compile.h"

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

/**
 * @enum m33_arena_cap_t
 * @brief Capacities of the M33 text/CSS/SVG compile's caller-owned arenas.
 * @details Sized for the parity fixture (one chapter, a stylesheet, an SVG
 *          cover) with comfortable slack. The builder tables/pools, the EPUB
 *          parse scratch and @ref epub_book_t all live in external SDRAM
 *          (`.sdram_bss`): @ref epub_book_t alone exceeds the 64 KiB
 *          SRAM_CPU1. The finalized blob is laid into the shared output buffer
 *          (see @ref com33_blob), never here.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_arena_chapters = 8U,          /**< Chapter-table entries.               */
  k_arena_nodes    = 256U,        /**< Node-table entries.                  */
  k_arena_attrs    = 64U,         /**< Attribute-table entries.             */
  k_arena_styles   = 4U,          /**< Stylesheet-table entries.            */
  k_arena_images   = 8U,          /**< Image-table entries.                 */
  k_arena_string   = 8U * 1024U,  /**< String-pool capacity (bytes).        */
  k_arena_imgpool  = 64U * 1024U, /**< Image-pool capacity (bytes).         */
  k_arena_xhtml    = 16U * 1024U, /**< Chapter XHTML parse scratch (bytes). */
  k_arena_imgraw   = 16U * 1024U, /**< SVG/resource load scratch (bytes).   */
  k_arena_css      = 16U * 1024U, /**< Stylesheet load scratch (bytes).     */
} m33_arena_cap_t;

/* The full compile working set lives in external SDRAM (.sdram_bss, NOLOAD):
 * epub_book_t alone is ~65 KiB and the builder/scratch arenas push well past
 * the 64 KiB SRAM_CPU1. The M85 brings up the SDRAM controller before releasing
 * the M33. NOLOAD needs no startup zeroing -- the builder appends into its
 * arenas before reading, the EPUB scratch is filled before each read, and
 * s_book is memset in compile_fixture before epub_open. The finalized blob
 * goes to the shared SRAM2 output buffer (com33_blob), not here. */
[[gnu::section(".sdram_bss"), gnu::aligned(8)]] static epub_book_t    s_book;
[[gnu::section(".sdram_bss"), gnu::aligned(8)]] static book_chapter_t s_chapters[k_arena_chapters];
[[gnu::section(".sdram_bss"), gnu::aligned(8)]] static book_node_t    s_nodes[k_arena_nodes];
[[gnu::section(".sdram_bss"), gnu::aligned(8)]] static book_attr_t    s_attrs[k_arena_attrs];
[[gnu::section(".sdram_bss"), gnu::aligned(8)]] static book_stylesheet_t s_styles[k_arena_styles];
[[gnu::section(".sdram_bss"), gnu::aligned(8)]] static book_image_t      s_images[k_arena_images];
[[gnu::section(".sdram_bss"), gnu::aligned(8)]] static char    s_string_pool[k_arena_string];
[[gnu::section(".sdram_bss"), gnu::aligned(8)]] static uint8_t s_image_pool[k_arena_imgpool];
[[gnu::section(".sdram_bss"), gnu::aligned(8)]] static uint8_t s_xhtml[k_arena_xhtml];
[[gnu::section(".sdram_bss"), gnu::aligned(8)]] static ra8_rabook_xml_workspace_t s_xml_workspace;
[[gnu::section(".sdram_bss"), gnu::aligned(8)]] static uint8_t s_image_raw[k_arena_imgraw];
[[gnu::section(".sdram_bss"), gnu::aligned(8)]] static char    s_css[k_arena_css];

/**
 * @brief Point a builder + pipeline-scratch view at the SDRAM arenas + output.
 *
 * @param[out] buf Builder buffer descriptor to fill (never NULL).
 * @param[out] scr Pipeline scratch descriptor to fill (never NULL).
 *
 * @return Whether the views were bound.
 * @retval true  @p buf and @p scr reference the SDRAM arenas + shared output.
 * @retval false @p buf or @p scr is NULL, or the output address resolved to NULL.
 *
 * @pre @p buf and @p scr are writable descriptors.
 * @pre The SDRAM arenas are linked into the `.sdram_bss` region.
 * @post On true, `buf->out` addresses the shared blob buffer at ::k_com33_blob_addr
 *       and every other `*` member is non-NULL with its `_cap` set.
 * @post On true, @p scr carries xhtml / image_raw / css; img_arena and gray are
 *       NULL (raster is excluded -- the image is built RA8_RABOOK_NO_RASTER).
 *
 * @note Single-threaded; runs in M33 thread mode with no RTOS.
 * @since 0.1.0
 */
static bool bind_compile(ra8_rabook_buffers_t* buf, ra8_rabook_pipeline_scratch_t* scr)
{
  if (buf == nullptr) {
    return false;
  }
  if (scr == nullptr) {
    return false;
  }
  uint8_t* out = com33_blob();
  if (out == nullptr) {
    return false;
  }
  *buf = (ra8_rabook_buffers_t){
    .chapters       = s_chapters,
    .nodes          = s_nodes,
    .attrs          = s_attrs,
    .stylesheets    = s_styles,
    .images         = s_images,
    .string_pool    = s_string_pool,
    .image_pool     = s_image_pool,
    .out            = out,
    .chapter_cap    = (uint32_t)k_arena_chapters,
    .node_cap       = (uint32_t)k_arena_nodes,
    .attr_cap       = (uint32_t)k_arena_attrs,
    .stylesheet_cap = (uint32_t)k_arena_styles,
    .image_cap      = (uint32_t)k_arena_images,
    .string_cap     = (uint32_t)k_arena_string,
    .image_pool_cap = (uint32_t)k_arena_imgpool,
    .out_cap        = (uint32_t)k_com33_blob_cap,
  };
  *scr = (ra8_rabook_pipeline_scratch_t){
    .xhtml         = s_xhtml,
    .xhtml_cap     = sizeof(s_xhtml),
    .image_raw     = s_image_raw,
    .image_cap     = sizeof(s_image_raw),
    .img_arena     = nullptr,
    .gray          = nullptr,
    .gray_cap      = 0U,
    .css           = s_css,
    .css_cap       = sizeof(s_css),
    .xml_workspace = &s_xml_workspace,
  };
  return true;
}

/**
 * @brief Compile the baked parity fixture EPUB into the shared output blob.
 *
 * @details Opens @c s_m33_parity_epub in memory, binds the SDRAM builder +
 * scratch arenas, and runs @ref rabook_compile_from_epub_to_buffer -- the same
 * byte-identical text/CSS/SVG pipeline the M85 path proved -- finalizing the
 * RABOOK1 blob into the shared SRAM2 output buffer (no filesystem). @c s_book is
 * memset first because it lives in NOLOAD SDRAM the reset handler does not zero.
 * Raster is excluded (RA8_RABOOK_NO_RASTER): the fixture cover is an SVG stored
 * verbatim, so no stb_image is linked into the M33 image.
 *
 * @param[out] out_blob Receives the finalized blob base (in the shared buffer).
 * @param[out] out_len  Receives the finalized blob length in bytes.
 *
 * @return Whether the EPUB compiled to a finalized blob.
 * @retval true  The blob was finalized into the shared output buffer.
 * @retval false @p out_blob / @p out_len NULL, epub_open failed, or a compile
 *               stage overflowed an arena.
 *
 * @pre @p out_blob and @p out_len are non-NULL.
 * @pre The M85 brought up the SDRAM controller before releasing this core.
 * @post On true the shared output buffer holds a finalized RABOOK1 blob.
 * @post On false the shared output buffer must not be treated as a valid blob.
 *
 * @note Single-threaded; runs in M33 thread mode with no RTOS.
 * @since 0.1.0
 */
static bool compile_fixture(const void** out_blob, uint32_t* out_len)
{
  if (out_blob == nullptr) {
    return false;
  }
  if (out_len == nullptr) {
    return false;
  }
  /* The M85 staged the source .epub into shared SRAM and posted its base/len in
   * the request; the M33 compiles whatever was staged (not a baked-in fixture),
   * so the same image serves any book the M85 dispatches. */
  const volatile com33_mailbox_t* mb = com33_mailbox();
  memset(&s_book, 0, sizeof(s_book));
  const epub_mem_media_t media = {
    .data = (const uint8_t*)(uintptr_t)mb->epub_base,
    .size = (size_t)mb->epub_len,
  };
  if (epub_open(&media, "rabook.epub", &s_book) != k_ra8_ok) {
    return false;
  }

  ra8_rabook_buffers_t          buf = {};
  ra8_rabook_pipeline_scratch_t scr = {};
  if (!bind_compile(&buf, &scr)) {
    (void)epub_close(&s_book);
    return false;
  }

  const ra8_err_t rc = rabook_compile_from_epub_to_buffer(&s_book, &buf, &scr, out_blob, out_len);
  (void)epub_close(&s_book);
  return rc == k_ra8_ok;
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
    const book_header_t* hdr = book_header(blob);
    mb->blob_base            = (uint32_t)(uintptr_t)blob;
    mb->blob_len             = len;
    mb->blob_crc             = hdr->crc32_val;
    mb->chapter_count        = hdr->chapter_count;
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
 * @note The core spins in place.
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
 * @enum cpu1_ipc_t
 * @brief IPC channel the M33 pokes to wake the parked M85.
 * @details IPC0 channel 0 is the CPU1 -> CPU0 direction; writing its IPC0ISET0
 *          raises the IPC0 receive interrupt the M85 armed before release, so
 *          the M85 leaves WFI as soon as the result is published.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_cpu1_ipc_wake_channel = 0U, /**< IPC0 channel 0 (CPU1 -> CPU0). */
} cpu1_ipc_t;

/**
 * @brief Poke the M85's IPC0 receive line to wake it from WFI.
 *
 * @details Writes IPC0ISET0 for the wake channel (HUM Ch 3.2.11 "IPC0ISET0"
 * p 215), asserting IRQ line 0 on the primary core. The mailbox `done` flag is
 * already published and `dsb`-ordered ahead of this call, so the woken M85
 * observes a settled result. The HAL return value is intentionally discarded: a
 * failed poke only costs the M85 a fall-through to its bounded poll, never
 * correctness.
 *
 * @return Nothing.
 *
 * @pre The result mailbox fields and `done` are published behind a `dsb`.
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
 * @brief CPU1 emitter entry: build the book, finalize the blob, report.
 *
 * @details Stamps the boot signature, binds the emitter to its arenas, builds
 * the DOM, finalizes the RABOOK1 blob into the shared output buffer, then
 * publishes the result for the parked M85. Any failure publishes `build_fail`.
 *
 * @return This function never returns.
 * @note Control ends in ::cpu1_park.
 *
 * @pre `cpu1_reset_handler` has initialised `.data` / `.bss`.
 * @pre The M85 published ::k_com33_magic and released this core.
 * @post `mb->done == 1` with `status` reflecting the emitter outcome.
 * @post On success the shared output buffer holds a finalized RABOOK1 blob.
 *
 * @note Single-threaded; runs in M33 thread mode with no RTOS.
 * @since 0.1.0
 */
[[noreturn]] static void cpu1_run_compile(void)
{
  volatile com33_mailbox_t* mb = com33_mailbox();
  mb->m33_sig                  = (uint32_t)k_com33_m33_sig;
  __asm volatile("dsb" ::: "memory");

  /* The M85 stages the .epub + buffers and posts req_magic BEFORE releasing this
   * core, so the job is ready on the first read; the dmb orders that read after
   * observing the boot handshake. A missing request is reported, not compiled. */
  __asm volatile("dmb" ::: "memory");
  if (mb->req_magic != (uint32_t)k_com33_req_magic) {
    mb->status = (uint32_t)k_com33_status_no_request;
    __asm volatile("dsb" ::: "memory");
    mb->done = 1U;
    __asm volatile("dsb" ::: "memory");
    notify_m85();
    cpu1_park();
  }

  const void* blob = nullptr;
  uint32_t    len  = 0U;
  const bool  ok   = compile_fixture(&blob, &len);

  publish_result(mb, blob, len, ok);
  notify_m85();
  cpu1_park();
}

/**
 * @brief CPU1 reset handler: minimal C-runtime init, then run the emitter.
 *
 * @details The M33 boots with uninitialised RAM, so before any C code runs this
 * copies `.data` from its MRAM_CPU1 load image into SRAM_CPU1 and zeroes `.bss`.
 * The linker exports the region bounds as `g_ra8_ls_cpu1_*` symbols.
 *
 * @return This function never returns.
 * @note Control passes to ::cpu1_run_compile, which loops forever.
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
  RA8_LOOP_BOUND_RUNTIME(g_ra8_ls_cpu1_data_end);
  while (dst < &g_ra8_ls_cpu1_data_end) {
    *dst = *src;
    dst++;
    src++;
  }

  uint32_t* bss = &g_ra8_ls_cpu1_bss_start;
  RA8_LOOP_BOUND_RUNTIME(g_ra8_ls_cpu1_bss_end);
  while (bss < &g_ra8_ls_cpu1_bss_end) {
    *bss = 0U;
    bss++;
  }

  cpu1_run_compile();
}

/**
 * @brief CPU1 default fault handler: publish a fault status, wake the M85, park.
 *
 * @details Every M33 exception slot routes here. Before halting, it stamps
 * ::k_com33_status_fault into the shared mailbox and sets `done`, then pokes the
 * M85 over IPC so the parked primary core wakes, observes `done`, reads the
 * distinct fault status, and reports an explicit secondary-core fault. Without
 * this the M85 -- which WFI-idles waiting for the M33's compile-done poke -- would
 * sleep forever on a `done` that a faulted core never sets. The first `dsb` orders
 * the status ahead of `done`; the second orders `done` ahead of the IPC poke. The
 * core then spins; on hardware a watchdog (if enabled) eventually resets it.
 *
 * @return This function never returns.
 * @note The core spins in place after publishing the fault.
 *
 * @pre A hardware fault or unhandled exception occurred.
 * @pre Entered via the M33 exception entry path.
 * @post `status == k_com33_status_fault` and `done == 1` are published behind a `dsb`.
 * @post The M33 makes no further forward progress.
 *
 * @note Shared default for all CPU1 exception vectors.
 * @since 0.1.0
 */
[[noreturn]] static void cpu1_fault_handler(void)
{
  volatile com33_mailbox_t* mb = com33_mailbox();
  mb->status                   = (uint32_t)k_com33_status_fault;
  __asm volatile("dsb" ::: "memory");
  mb->done = 1U;
  __asm volatile("dsb" ::: "memory");
  notify_m85();
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
