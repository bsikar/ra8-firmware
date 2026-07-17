/**
 * @file main.c
 * @brief Entry point for the RA8 desktop reader viewer (direct-call, no emulation).
 *
 * @details
 * Opens a comic/e-book file with the host-linked reader core (ra8_viewer_reader),
 * renders a page into an RGB565 framebuffer, and either dumps it for a headless
 * rendering proof or shows it in a Cocoa window (ra8_viewer_view) with Left/Right
 * arrow page turns. This is a viewer, not an emulator: it links the firmware's
 * platform-agnostic reader libraries and calls them directly on the host.
 *
 * Usage:
 *   ra8_viewer <file.cbz|.cbr|.cbt> [--page N] [--dump-ppm PATH] [--dump-png PATH]
 *              [--headless]
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
} viewer_main_cfg_t;

/**
 * @struct viewer_opts_t
 * @brief Parsed command-line options.
 * @since 0.1.0
 */
typedef struct {
  const char* path;     /**< Input document path (required).       */
  const char* dump_ppm; /**< PPM dump path, or NULL.               */
  const char* dump_png; /**< PNG dump path, or NULL.               */
  uint32_t    page;     /**< Initial page index.                   */
  bool        headless; /**< Skip opening a window when true.      */
} viewer_opts_t;

/**
 * @brief Print the one-line usage banner to stderr.
 * @param[in] argv0 Program name (argv[0]).
 * @since 0.1.0
 */
static void viewer_usage(const char* argv0)
{
  (void)fprintf(stderr,
                "usage: %s <file.cbz|.cbr|.cbt> [--page N] [--dump-ppm PATH]"
                " [--dump-png PATH] [--headless]\n",
                argv0);
}

/**
 * @brief Parse argv into @p opts.
 * @param[in]  argc Argument count.
 * @param[in]  argv Argument vector.
 * @param[out] opts Receives the parsed options.
 * @return true on a valid command line; false to print usage and exit.
 * @since 0.1.0
 */
static bool viewer_parse_args(int argc, char** argv, viewer_opts_t* opts)
{
  if (argc < k_viewer_min_args) {
    return false;
  }
  memset(opts, 0, sizeof(*opts));
  opts->path = argv[1];
  for (int i = 2; i < argc; ++i) {
    if ((strcmp(argv[i], "--page") == 0) && ((i + 1) < argc)) {
      opts->page = (uint32_t)strtoul(argv[++i], nullptr, (int)k_viewer_radix_dec);
    } else if ((strcmp(argv[i], "--dump-ppm") == 0) && ((i + 1) < argc)) {
      opts->dump_ppm = argv[++i];
    } else if ((strcmp(argv[i], "--dump-png") == 0) && ((i + 1) < argc)) {
      opts->dump_png = argv[++i];
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
 * @param[in] page  Requested page.
 * @param[in] count Page count.
 * @return A valid page index (0 when @p count is 0).
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
 * @brief Render @p page and write any requested PPM/PNG dumps.
 * @param[in]  reader Open reader.
 * @param[in]  opts   Parsed options.
 * @param[in]  page   Page to render.
 * @return true on success.
 * @since 0.1.0
 */
static bool
viewer_render_and_dump(ra8_viewer_reader_t* reader, const viewer_opts_t* opts, uint32_t page)
{
  const ra8_err_t rc = ra8_viewer_render_page(reader, page);
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
 * @brief Windowed loop: present the current page, turn pages on arrow keys.
 * @param[in] reader Open reader.
 * @param[in] opts   Parsed options.
 * @param[in] start  Initial page.
 * @return 0 on clean exit.
 * @since 0.1.0
 */
static int viewer_run_window(ra8_viewer_reader_t* reader, const viewer_opts_t* opts, uint32_t start)
{
  const uint16_t     w    = ra8_viewer_fb_width(reader);
  const uint16_t     h    = ra8_viewer_fb_height(reader);
  ra8_viewer_view_t* view = ra8_viewer_view_open(w, h, "RA8 Viewer");
  if (view == nullptr) {
    (void)fprintf(stderr, "no window (headless host?); use --dump-ppm/--dump-png\n");
    return 1;
  }
  const uint32_t count = ra8_viewer_page_count(reader);
  uint32_t       page  = start;
  ra8_viewer_view_present(view, ra8_viewer_framebuffer(reader), w, h);
  if (opts->dump_png != nullptr) {
    (void)ra8_viewer_view_save_png(view, opts->dump_png);
    (void)fprintf(stderr, "wrote %s\n", opts->dump_png);
  }

  const struct timespec frame = {.tv_sec = 0, .tv_nsec = k_viewer_frame_ns};
  while (!ra8_viewer_view_pump(view)) {
    const int32_t nav = ra8_viewer_view_poll_nav(view);
    if (nav != 0) {
      const int64_t  want    = (int64_t)page + nav;
      const int64_t  clamped = (want < 0) ? 0 : want;
      const uint32_t next    = viewer_clamp_page((uint32_t)clamped, count);
      if ((next != page) && (ra8_viewer_render_page(reader, next) == k_ra8_ok)) {
        page = next;
        ra8_viewer_view_present(view, ra8_viewer_framebuffer(reader), w, h);
      }
    }
    (void)ra8_viewer_view_poll_scroll(view); /* TODO: webtoon vertical scroll. */
    (void)nanosleep(&frame, nullptr);
  }
  ra8_viewer_view_close(view);
  return 0;
}

/**
 * @brief Program entry point.
 * @param[in] argc Argument count.
 * @param[in] argv Argument vector.
 * @return 0 on success, non-zero on error.
 * @since 0.1.0
 */
/**
 * @brief ra8_log byte sink -- routes firmware log bytes to stderr.
 * @details Registering any sink makes the logger's `internal_itm_ready()` skip
 *          the ITM debug-register read, which is an unmapped MMIO address on the
 *          host (a bus fault) -- the host-safe path the tests also use.
 * @param[in] ctx  Unused cookie.
 * @param[in] byte Log byte to emit.
 * @since 0.1.0
 */
static void viewer_log_sink(void* ctx, uint8_t byte)
{
  (void)ctx;
  (void)fputc((int)byte, stderr);
}

int main(int argc, char** argv)
{
  ra8_log_set_byte_sink(viewer_log_sink, nullptr);

  viewer_opts_t opts = {};
  if (!viewer_parse_args(argc, argv, &opts)) {
    viewer_usage(argv[0]);
    return 2;
  }

  ra8_viewer_reader_t* reader = nullptr;
  const ra8_err_t      rc     = ra8_viewer_open(&reader, opts.path);
  if (rc != k_ra8_ok) {
    (void)fprintf(stderr, "open '%s' failed: 0x%x\n", opts.path, (unsigned)rc);
    return 1;
  }
  const uint32_t count = ra8_viewer_page_count(reader);
  const uint32_t page  = viewer_clamp_page(opts.page, count);
  (void)fprintf(stderr, "opened '%s': %u page(s)\n", opts.path, count);

  int status = 0;
  if (!viewer_render_and_dump(reader, &opts, page)) {
    status = 1;
  } else if (!opts.headless) {
    status = viewer_run_window(reader, &opts, page);
  }
  ra8_viewer_close(reader);
  return status;
}
