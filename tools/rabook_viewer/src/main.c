/**
 * @file main.c
 * @brief Entry point for the RA8 desktop reader viewer (direct-call, no emulation).
 *
 * @details
 * Opens a comic/e-book file with the host-linked reader core (ra8_viewer_reader)
 * and either shows it in a resizable, fit-to-width, continuously-scrolling Cocoa
 * window (ra8_viewer_view) or, headless, dumps a single page/tile for a rendering
 * proof. This is a viewer, not an emulator: it links the firmware's
 * platform-agnostic reader libraries and calls them directly on the host.
 *
 * Usage:
 * @code
 * ra8_viewer <file.cbz|.cbr|.cbt[.gz|.xz]|.jof>            # scrolling window
 * ra8_viewer <file> --headless --dump-ppm P [--page N | --dump-tile N]
 * @endcode
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 *
 *

 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ra8_host_arena.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_viewer_reader.h"
#include "ra8_viewer_view.h"

/**
 * @enum viewer_main_cfg_t
 * @brief Fixed run-loop and argument constants (no magic numbers).
 * @since 0.1.0
 */
typedef enum : int32_t {
  k_viewer_frame_ns  = 16000000, /**< Cooperative pump period, ~60 Hz (ns). */
  k_viewer_min_args  = 2,        /**< argv count with just the file path.   */
  k_viewer_radix_dec = 10,       /**< Base for parsing --page (decimal).    */
  k_arena_bytes      = 16U * 1024U * 1024U,
} viewer_main_cfg_t;

/**
 * @struct viewer_opts_t
 * @brief Parsed command-line options.
 * @since 0.1.0
 */
typedef struct {
  const char* path;      /**< Input document path (required).         */
  const char* dump_ppm;  /**< PPM dump path, or NULL.                 */
  uint32_t    page;      /**< Page to render for --dump-ppm.          */
  int64_t     dump_tile; /**< Tile to dump via the scroll API, or -1. */
  bool        headless;  /**< Skip opening a window when true.        */
} viewer_opts_t;

/**
 * @brief Print the one-line usage banner to stderr.
 * @details Lists the accepted container extensions and the headless dump flags,
 *          and names the scroll bindings the window honours. Printed on a usage
 *          error just before the process exits with code 2.
 * @param[in] argv0 Program name (argv[0]).
 * @pre @p argv0 is a NUL-terminated string.
 * @pre stderr is open.
 * @post The banner has been written to stderr.
 * @post No state other than the stream is mutated.
 * @note Not thread-safe; the tool is single-threaded.
 * @since 0.1.0
 */
static void viewer_usage(const char* argv0)
{
  (void)fprintf(stderr,
                "usage: %s <file.cbz|.cbr|.cbt[.gz|.xz]|.jof> [--headless]\n"
                "         [--dump-ppm PATH [--page N | --dump-tile N]]\n"
                "  window: resizable, fit-to-width, continuous scroll "
                "(wheel/trackpad/PageUp-Dn/Home/End)\n",
                argv0);
}

/**
 * @brief Parse argv into @p opts.
 * @details Takes argv[1] as the document path, then walks the remaining flags:
 *          `--page N`, `--dump-ppm PATH`, `--dump-tile N` and `--headless`. An
 *          unknown flag, or a value-taking flag with no value, is rejected so
 *          the caller prints usage. @p opts is zeroed first and dump_tile
 *          defaults to -1 (no tile dump).
 * @param[in]  argc Argument count.
 * @param[in]  argv Argument vector.
 * @param[out] opts Receives the parsed options.
 * @return true on a valid command line; false to print usage and exit.
 * @retval true  Every argument was recognised; @p opts is populated.
 * @retval false Too few arguments, or an unrecognised / incomplete flag.
 * @pre @p argv holds @p argc entries and @p opts is writable.
 * @pre @p argc reflects the length of @p argv.
 * @post On success @p opts->path names the document.
 * @post On failure the caller prints usage and exits.
 * @note Not thread-safe (writes @p opts).
 * @since 0.1.0
 */
static bool viewer_parse_args(int argc, char** argv, viewer_opts_t* opts)
{
  if (argc < k_viewer_min_args) {
    return false;
  }
  memset(opts, 0, sizeof(*opts));
  opts->path      = argv[1];
  opts->dump_tile = -1;
  for (int i = 2; i < argc; ++i) {
    if ((strcmp(argv[i], "--page") == 0) && ((i + 1) < argc)) {
      opts->page = (uint32_t)strtoul(argv[++i], nullptr, (int)k_viewer_radix_dec);
    } else if ((strcmp(argv[i], "--dump-ppm") == 0) && ((i + 1) < argc)) {
      opts->dump_ppm = argv[++i];
    } else if ((strcmp(argv[i], "--dump-tile") == 0) && ((i + 1) < argc)) {
      opts->dump_tile = (int64_t)strtoll(argv[++i], nullptr, (int)k_viewer_radix_dec);
    } else if (strcmp(argv[i], "--headless") == 0) {
      opts->headless = true;
    } else {
      return false;
    }
  }
  return true;
}

/**
 * @brief Clamp @p page into `[0, count)`, guarding an empty document.
 * @details A `--page` value past the end is pinned to the last page rather than
 *          rejected, and a zero-page document yields page 0 so the caller never
 *          indexes an empty range.
 * @param[in] page  Requested page.
 * @param[in] count Page count.
 * @return A valid page index (0 when @p count is 0).
 * @retval 0 @p count is 0, or @p page is already 0.
 * @pre @p count is the document's true page count.
 * @pre @p page came from the parsed command line.
 * @post The result is less than @p count whenever @p count > 0.
 * @post No state is mutated.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
static uint32_t viewer_clamp_page(uint32_t page, uint32_t count)
{
  if (count == 0U) {
    return 0U;
  }
  return (page >= count) ? (count - 1U) : page;
}

/**
 * @brief Render @p page into the framebuffer and write the requested PPM dump.
 * @details Renders the page through the reader's fixed framebuffer path, then --
 *          when `--dump-ppm` was given -- writes that framebuffer out as a PPM.
 *          A render or write failure is reported on stderr and returns false.
 * @param[in]  reader Open reader.
 * @param[in]  opts   Parsed options.
 * @param[in]  page   Page to render.
 * @param[in] arena Scratch arena.
 * @return true on success.
 * @retval true  The page rendered and any requested dump was written.
 * @retval false Rendering or the PPM write failed (reported on stderr).
 * @pre @p reader is an open document and @p opts is populated.
 * @pre @p page is within the document's page range.
 * @post On success the framebuffer holds @p page and any dump file exists.
 * @post On failure nothing is written to the dump path.
 * @note Not thread-safe (drives the shared framebuffer).
 * @since 0.1.0
 */
static bool viewer_render_and_dump(ra8_viewer_reader_t* reader,
                                   const viewer_opts_t* opts,
                                   uint32_t             page,
                                   ra8_arena_t*         arena)
{
  const ra8_err_t rc = ra8_viewer_render_page(reader, page, arena);
  if (rc != k_ra8_ok) {
    (void)fprintf(stderr, "render page %u failed: 0x%x\n", page, (unsigned)rc);
    return false;
  }
  if (opts->dump_ppm != nullptr) {
    if (ra8_viewer_dump_ppm(reader, opts->dump_ppm) != k_ra8_ok) {
      (void)fprintf(stderr, "dump ppm failed\n");
      return false;
    }
    (void)fprintf(stderr, "wrote %s\n", opts->dump_ppm);
  }
  return true;
}

/**
 * @brief Open the scrolling reader window and pump events until it closes.
 * @details Creates the Cocoa view over @p reader and cooperatively pumps events
 *          at ~60 Hz (::k_viewer_frame_ns), sleeping between frames, until the
 *          user closes the window. A NULL view means no window could be created
 *          (a headless host), reported with the `--headless` hint.
 * @param[in] reader Open reader (the window's tile source).
 * @param[in] arena Scratch arena.
 * @return 0 on clean exit; 1 if no window could be created (headless host).
 * @retval 0 The window opened and was closed by the user.
 * @retval 1 No window could be created on this host.
 * @pre @p reader is an open document.
 * @pre A window system is available for a non-headless run.
 * @post On success the view has been closed and freed.
 * @post On failure no view is left open.
 * @note Not thread-safe; must run on the main thread (Cocoa requirement).
 * @since 0.1.0
 */
static int viewer_run_window(ra8_viewer_reader_t* reader, ra8_arena_t* arena)
{
  ra8_viewer_view_t* view = ra8_viewer_view_open(reader, "RA8 Viewer", arena);
  if (view == nullptr) {
    (void)fprintf(stderr, "no window (headless host?); use --headless --dump-ppm PATH\n");
    return 1;
  }
  const struct timespec frame = {.tv_sec = 0, .tv_nsec = k_viewer_frame_ns};
  while (!ra8_viewer_view_pump(view)) {
    (void)nanosleep(&frame, nullptr);
  }
  ra8_viewer_view_close(view);
  return 0;
}

/**
 * @brief ra8_log byte sink -- routes firmware log bytes to stderr.
 * @details Registering any sink makes the logger's `internal_itm_ready()` skip
 *          the ITM debug-register read, which is an unmapped MMIO address on the
 *          host (a bus fault) -- the host-safe path the tests also use.
 * @param[in] ctx  Unused cookie.
 * @param[in] byte Log byte to emit.
 * @pre ra8_log has been pointed at this sink via ::ra8_log_set_byte_sink.
 * @pre stderr is open.
 * @post @p byte has been written to stderr.
 * @post No other state is mutated.
 * @note Not thread-safe; the tool is single-threaded.
 * @since 0.1.0
 */
static void viewer_log_sink(void* ctx, uint8_t byte)
{
  (void)ctx;
  (void)fputc((int)byte, stderr);
}

/**
 * @brief Render one scroll tile via the tile API and write it as a P6 PPM.
 * @details Headless verification of the continuous-scroll render path (the same
 *          tiles the window composites), independent of the fixed framebuffer.
 * @param[in] reader Open reader.
 * @param[in] tile   Tile index.
 * @param[in] path   Output PPM path.
 * @param[in] arena Scratch arena.
 * @return true on success.
 * @retval true  The tile rendered and its PPM was written.
 * @retval false Tile render or the PPM write failed (reported on stderr).
 * @pre @p reader is an open document and @p path is writable.
 * @pre @p tile is a valid scroll-tile index.
 * @post On success @p path holds the tile as a P6 PPM and the buffer is freed.
 * @post On failure the render buffer is still freed.
 * @note Not thread-safe (drives the shared reader).
 * @since 0.1.0
 */
static bool
viewer_dump_tile(ra8_viewer_reader_t* reader, uint32_t tile, const char* path, ra8_arena_t* arena)
{
  uint32_t        w   = 0U;
  uint32_t        h   = 0U;
  uint16_t*       buf = nullptr;
  const ra8_err_t rc  = ra8_viewer_render_tile565(reader, tile, &w, &h, &buf, arena);
  if (rc != k_ra8_ok) {
    (void)fprintf(stderr, "render tile %u failed: 0x%x\n", tile, (unsigned)rc);
    return false;
  }
  const ra8_err_t wrc = ra8_viewer_write_ppm565(buf, w, h, path);
  if (wrc != k_ra8_ok) {
    (void)fprintf(stderr, "write tile ppm failed: 0x%x\n", (unsigned)wrc);
    return false;
  }
  (void)fprintf(stderr, "wrote tile %u (%ux%u) -> %s\n", tile, w, h, path);
  return true;
}

/**
 * @brief Program entry point: parse the command line, open, then render.
 * @details Routes to one of three modes -- a headless single-tile dump, a
 *          headless framebuffer page dump, or the interactive scrolling window.
 * @param[in] argc Argument count.
 * @param[in] argv Argument vector.
 * @return 0 on success, 1 on an open/render failure, 2 on a usage error.
 * @since 0.1.0
 */

int main(int argc, char** argv)
{
  ra8_log_set_byte_sink(viewer_log_sink, nullptr);

  uint8_t*    arena_buf = (uint8_t*)malloc((size_t)k_arena_bytes);
  ra8_arena_t arena;
  ra8_arena_init(&arena, arena_buf, (uint32_t)k_arena_bytes);

  viewer_opts_t opts = {};
  if (!viewer_parse_args(argc, argv, &opts)) {
    viewer_usage(argv[0]);
    return 2;
  }

  ra8_viewer_reader_t* reader = nullptr;
  const ra8_err_t      rc     = ra8_viewer_open(&reader, opts.path, &arena);
  if (rc != k_ra8_ok) {
    (void)fprintf(stderr, "open '%s' failed: 0x%x\n", opts.path, (unsigned)rc);
    return 1;
  }
  const uint32_t count = ra8_viewer_page_count(reader);
  const uint32_t page  = viewer_clamp_page(opts.page, count);
  (void)fprintf(stderr, "opened '%s': %u page(s)\n", opts.path, count);

  /* Headless tile dump: verify the scroll render path independent of a window. */
  if (opts.dump_tile >= 0) {
    const char* out    = (opts.dump_ppm != nullptr) ? opts.dump_ppm : "/tmp/ra8_tile.ppm";
    const int   status = viewer_dump_tile(reader, (uint32_t)opts.dump_tile, out, &arena) ? 0 : 1;
    ra8_viewer_close(reader);
    free(arena_buf);
    return status;
  }

  int status = 0;
  if (opts.headless || (opts.dump_ppm != nullptr)) {
    status = viewer_render_and_dump(reader, &opts, page, &arena) ? 0 : 1;
  } else {
    status = viewer_run_window(reader, &arena);
  }
  ra8_viewer_close(reader);
  free(arena_buf);
  return status;
}
