/**
 * @file main.c
 * @brief Bounded host composition root for the JOF/comic viewer.
 * @details One named 32-MiB backing holds the reader plus the mutually
 * exclusive headless-tile or Cocoa-view scratch. Exact requirements are checked
 * before every bind; larger atlases fail visibly instead of reaching an
 * allocator or acquiring an implicit mapping.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <signal.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ra8_arena.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_imgdec_name.h"
#include "ra8_io_stream.h"
#include "ra8_io_stream_posix.h"
#include "ra8_log.h"
#include "ra8_viewer_output_internal.h"
#include "ra8_viewer_reader.h"
#include "ra8_viewer_view.h"

/** @brief Composition limits and CLI constants. */
typedef enum : int32_t {
  k_viewer_frame_ns  = 16000000, /**< Cooperative pump period.       */
  k_viewer_min_args  = 2,        /**< argv count with an input path. */
  k_viewer_radix_dec = 10,       /**< Decimal option radix.          */
  /** @brief Entire first-party byte budget. */
  k_viewer_composition_bytes = 32 * 1024 * 1024,
} viewer_main_config_t;

/** @brief Parsed command-line options. */
typedef struct {
  const char* path;      /**< Input document path.    */
  const char* dump_ppm;  /**< Output path, or NULL.   */
  uint32_t    page;      /**< Headless page index.    */
  int64_t     dump_tile; /**< Tile index, or -1.      */
  bool        headless;  /**< Do not create a window. */
} viewer_options_t;

/** @brief Explicit host composition storage; another target may replace it. */
alignas(max_align_t) static uint8_t s_viewer_composition[k_viewer_composition_bytes];

/**
 * @brief Print usage through the standard-error descriptor.
 * @details Uses one descriptor-formatted write so no hosted stream is owned.
 * @param[in,out] output Bound diagnostic byte stream.
 * @param[in] executable NUL-terminated argv[0] spelling.
 * @pre @p executable is non-NULL.
 * @pre Standard error may accept a best-effort diagnostic.
 * @post One complete usage message was attempted.
 * @post Composition storage and options remain unchanged.
 * @note The diagnostic failure is intentionally non-fatal.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_usage(ra8_io_stream_t* output, const char* executable)
{
  (void)priv_viewer_output_usage(output, executable);
}

/**
 * @brief Parse the bounded viewer command line.
 * @details Accepts only the documented value-taking flags and headless switch.
 * @param[in] argc Argument count.
 * @param[in] argv Argument vector.
 * @param[out] options Parsed options.
 * @return Whether every argument was valid.
 * @retval true Options were populated.
 * @retval false Arguments were incomplete or unknown.
 * @pre @p argv spans @p argc entries.
 * @pre @p options is writable.
 * @post Success publishes a document path and defaults tile to -1.
 * @post Failure causes no external I/O.
 * @note Pure apart from @p options.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_parse_arguments(int argc, char** argv, viewer_options_t* options)
{
  if (argc < (int)k_viewer_min_args) {
    return false;
  }
  *options = (viewer_options_t){.path = argv[1], .dump_tile = -1};
  for (int index = 2; index < argc; ++index) {
    if ((strcmp(argv[index], "--page") == 0) && ((index + 1) < argc)) {
      options->page = (uint32_t)strtoul(argv[++index], nullptr, (int)k_viewer_radix_dec);
    } else if ((strcmp(argv[index], "--dump-ppm") == 0) && ((index + 1) < argc)) {
      options->dump_ppm = argv[++index];
    } else if ((strcmp(argv[index], "--dump-tile") == 0) && ((index + 1) < argc)) {
      options->dump_tile = strtoll(argv[++index], nullptr, (int)k_viewer_radix_dec);
    } else if (strcmp(argv[index], "--headless") == 0) {
      options->headless = true;
    } else {
      return false;
    }
  }
  return true;
}

/**
 * @brief Clamp a requested page into the document.
 * @details Empty documents map to zero; over-range requests map to the last
 * page.
 * @param[in] page Requested index.
 * @param[in] count Document page count.
 * @return A bounded page index.
 * @retval 0 The document is empty or page zero was requested.
 * @pre @p count is the reader-reported count.
 * @pre @p page is an unsigned command-line value.
 * @post A non-empty result is below @p count.
 * @post No state is mutated.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_clamp_page(uint32_t page, uint32_t count)
{
  if (count == 0U) {
    return 0U;
  }
  return (page >= count) ? (count - 1U) : page;
}

/**
 * @brief Report exact capacity evidence.
 * @details Emits the subject plus exact required and supplied byte counts.
 * @param[in,out] output Bound diagnostic byte stream.
 * @param[in] subject NUL-terminated workspace name.
 * @param[in] report Completed capacity report.
 * @pre @p subject is non-NULL.
 * @pre @p report is non-NULL.
 * @post A diagnostic write was attempted.
 * @post Workspace bytes remain unchanged.
 * @note Descriptor diagnostics are best-effort.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_report_capacity(ra8_io_stream_t*                     output,
                                                  const char*                          subject,
                                                  const ra8_viewer_workspace_report_t* report)
{
  (void)
    priv_viewer_output_capacity(output, subject, report->required_bytes, report->supplied_bytes);
}

/**
 * @brief Name the container a failed page holds, when it can be named.
 * @details A render refusal says the page did not open; it does not say what
 * the page was. ::ra8_viewer_page_container answers that from the one shared
 * naming table, so a GIF the bound decoder will not open reads as a GIF rather
 * than as an anonymous unsupported status. A page with no recognised signature
 * adds no line: there is nothing truthful to add.
 * @param[in,out] diagnostic Bound diagnostic byte stream.
 * @param[in,out] reader Open reader.
 * @param[in] page Page index that failed.
 * @pre @p reader is open and @p page is in range.
 * @post At most one naming line was attempted.
 * @post No render state is published.
 * @note Best-effort; a naming failure is silent by design.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_report_container(ra8_io_stream_t* diagnostic, ra8_viewer_reader_t* reader, uint32_t page)
{
  ra8_imgdec_name_t name = {};
  if (ra8_viewer_page_container(reader, page, &name) == k_ra8_ok) {
    (void)priv_viewer_output_container(diagnostic, page, name.ext, name.mime);
  }
}

/**
 * @brief Render and optionally write one fixed-framebuffer page.
 * @details Drives the fixed target and publishes it only when a path was given.
 * @param[in,out] reader Open reader.
 * @param[in] options Parsed options.
 * @param[in] page Bounded page index.
 * @param[in,out] diagnostic Bound diagnostic byte stream.
 * @return Whether render and optional publication succeeded.
 * @retval true All requested work completed.
 * @retval false A render or descriptor publication failed.
 * @pre @p reader is open.
 * @pre @p options is populated.
 * @post Success leaves the framebuffer rendered.
 * @post Failure is reported on standard error.
 * @note Not thread-safe; it drives shared reader state.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_render_page(ra8_viewer_reader_t*    reader,
                                              const viewer_options_t* options,
                                              uint32_t                page,
                                              ra8_io_stream_t*        diagnostic)
{
  const ra8_err_t error = ra8_viewer_render_page(reader, page);
  if (error != k_ra8_ok) {
    (void)priv_viewer_output_index_error(diagnostic, "render page ", page, error);
    internal_report_container(diagnostic, reader, page);
    return false;
  }
  if ((options->dump_ppm != nullptr) &&
      (ra8_viewer_dump_ppm(reader, options->dump_ppm) != k_ra8_ok)) {
    (void)priv_viewer_output_text(diagnostic, "dump ppm failed\n");
    return false;
  }
  if (options->dump_ppm != nullptr) {
    (void)priv_viewer_output_wrote(diagnostic, options->dump_ppm);
  }
  return true;
}

/**
 * @brief Render and publish one caller-buffered scroll tile.
 * @details Renders into the supplied remainder and writes the exact RGB565
 * tile.
 * @param[in,out] reader Open reader.
 * @param[in] tile Tile index.
 * @param[in] path NUL-terminated output path.
 * @param[in,out] workspace Caller tile backing.
 * @param[in] workspace_bytes Accessible tile backing extent.
 * @param[in,out] diagnostic Bound diagnostic byte stream.
 * @return Whether render and publication succeeded.
 * @retval true The PPM was written.
 * @retval false Capacity, render, or publication failed.
 * @pre @p reader is open and @p path is non-NULL.
 * @pre @p workspace spans @p workspace_bytes bytes.
 * @post Success writes one complete PPM.
 * @post Failure reports the exact capacity when applicable.
 * @note Not thread-safe; it drives shared reader state.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_dump_tile(ra8_viewer_reader_t* reader,
                                            uint32_t             tile,
                                            const char*          path,
                                            void*                workspace,
                                            size_t               workspace_bytes,
                                            ra8_io_stream_t*     diagnostic)
{
  uint32_t                      width  = 0U;
  uint32_t                      height = 0U;
  uint16_t*                     pixels = nullptr;
  ra8_viewer_workspace_report_t report = {};
  const ra8_err_t               error  = ra8_viewer_render_tile565(reader,
                                                                   tile,
                                                                   workspace,
                                                                   workspace_bytes,
                                                                   &width,
                                                                   &height,
                                                                   &pixels,
                                                                   &report);
  if (error != k_ra8_ok) {
    if (error == k_ra8_err_invalid_size) {
      internal_report_capacity(diagnostic, "tile", &report);
    }
    (void)priv_viewer_output_index_error(diagnostic, "render tile ", tile, error);
    internal_report_container(diagnostic, reader, tile);
    return false;
  }
  const ra8_err_t write_error = ra8_viewer_write_ppm565(pixels, width, height, path);
  if (write_error != k_ra8_ok) {
    (void)priv_viewer_output_error(diagnostic, "write tile ppm", write_error);
    return false;
  }
  (void)priv_viewer_output_tile(diagnostic, tile, width, height, path);
  return true;
}

/**
 * @brief Open and cooperatively pump the caller-workspace Cocoa view.
 * @details Sizes, binds, pumps, and closes the platform view in the caller
 * slice.
 * @param[in,out] reader Open reader borrowed by the view.
 * @param[in,out] workspace Caller view backing.
 * @param[in] workspace_bytes Accessible backing extent.
 * @param[in,out] diagnostic Bound diagnostic byte stream.
 * @return Process-style status.
 * @retval 0 The window was opened and closed normally.
 * @retval 1 The platform or workspace could not provide a view.
 * @pre @p reader remains open throughout the pump.
 * @pre @p workspace spans @p workspace_bytes bytes.
 * @post Any successfully opened view is closed.
 * @post The reader remains open and caller-owned.
 * @note Must execute on the Cocoa main thread.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_run_window(ra8_viewer_reader_t* reader,
                                            void*                workspace,
                                            size_t               workspace_bytes,
                                            ra8_io_stream_t*     diagnostic)
{
  ra8_viewer_view_requirements_t requirements = {};
  if (ra8_viewer_view_requirements(reader, &requirements) != k_ra8_ok) {
    (void)priv_viewer_output_text(diagnostic,
                                  "no window (headless host?); use --headless --dump-ppm PATH\n");
    return 1;
  }
  ra8_viewer_view_t*            view   = nullptr;
  ra8_viewer_workspace_report_t report = {};
  const ra8_err_t               error  = ra8_viewer_view_open(&view,
                                                              reader,
                                                              "RA8 Viewer",
                                                              workspace,
                                                              workspace_bytes,
                                                              &requirements,
                                                              &report);
  if (error != k_ra8_ok) {
    if (error == k_ra8_err_invalid_size) {
      internal_report_capacity(diagnostic, "view", &report);
    }
    (void)priv_viewer_output_text(diagnostic,
                                  "no window (headless host?); use --headless --dump-ppm PATH\n");
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
 * @brief Route firmware log bytes through the standard-error descriptor.
 * @details Delegates the byte to the bound portable stream and intentionally
 * ignores its best-effort diagnostic status.
 * @param[in,out] context Bound ::ra8_io_stream_t logger cookie.
 * @param[in] byte Log byte.
 * @pre The logger installed this callback.
 * @pre Standard error may accept a best-effort byte.
 * @post One byte write was attempted.
 * @post Reader and composition state remain unchanged.
 * @note The single-threaded tool serialises calls.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_log_sink(void* context, uint8_t byte)
{
  (void)ra8_io_stream_putc((ra8_io_stream_t*)context, (char)byte);
}

/**
 * @brief Bind standard error through the raw POSIX stream adapter.
 * @details Ignores SIGPIPE so a closed diagnostic consumer becomes EPIPE and
 * can be mapped to a normal stream error rather than terminating the viewer.
 * @param[out] output Stream handle to bind.
 * @param[out] state Caller-owned POSIX adapter state.
 * @return Canonical binding or signal-configuration status.
 * @retval k_ra8_ok Standard error is bound and ready.
 * @retval k_ra8_err_comm_error SIGPIPE disposition setup failed.
 * @pre @p output and @p state are writable.
 * @pre Standard error remains process-owned for the binding lifetime.
 * @post Success leaves SIGPIPE ignored for the process lifetime.
 * @post Failure leaves @p output unbound.
 * @note This process-wide policy belongs at the composition boundary.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_bind_diagnostic(ra8_io_stream_t*             output,
                                                       ra8_io_stream_posix_state_t* state)
{
  struct sigaction action = {.sa_handler = SIG_IGN};
  if ((sigemptyset(&action.sa_mask) != 0) || (sigaction(SIGPIPE, &action, nullptr) != 0)) {
    return k_ra8_err_comm_error;
  }
  return ra8_io_stream_posix_init(output, state, STDERR_FILENO);
}

/**
 * @brief Execute the selected viewer mode over the composition remainder.
 * @details Carves the composition into the reader's bound span and the scratch
 * tail through the platform arena, then dispatches to tile, fixed-page, or
 * interactive-window rendering without acquiring storage.
 * @param[in,out] reader Open reader bound in composition storage.
 * @param[in] requirements Exact reader workspace requirements.
 * @param[in] options Parsed viewer options.
 * @param[in] page Bounded page index.
 * @param[in,out] diagnostic Bound diagnostic stream.
 * @return Process-style command status.
 * @retval 0 The selected mode completed successfully.
 * @retval 1 The composition carve or the selected mode failed.
 * @pre @p reader is open and @p requirements describes its binding.
 * @pre @p options and @p diagnostic remain valid throughout the call.
 * @post The reader remains open and caller-owned.
 * @post Composition bytes outside the selected scratch slice remain unchanged.
 * @note Not thread-safe; it drives the caller's reader and shared composition.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_execute(ra8_viewer_reader_t*                    reader,
                                         const ra8_viewer_reader_requirements_t* requirements,
                                         const viewer_options_t*                 options,
                                         uint32_t                                page,
                                         ra8_io_stream_t*                        diagnostic)
{
  if (requirements->required_bytes > sizeof(s_viewer_composition)) {
    return 1;
  }
  ra8_arena_t composition = {};
  void*       reader_span = nullptr;
  void*       scratch     = nullptr;
  uint32_t    scratch_bytes = 0U;
  if (ra8_arena_init(&composition,
                     s_viewer_composition,
                     (uint32_t)sizeof(s_viewer_composition)) != k_ra8_ok) {
    return 1;
  }
  if (ra8_arena_carve(&composition,
                      (uint32_t)requirements->required_bytes,
                      (uint32_t)alignof(max_align_t),
                      &reader_span) != k_ra8_ok) {
    return 1;
  }
  if (reader_span != (void*)s_viewer_composition) {
    return 1;
  }
  if (ra8_arena_carve_remaining(&composition,
                                (uint32_t)alignof(max_align_t),
                                &scratch,
                                &scratch_bytes) != k_ra8_ok) {
    return 1;
  }
  if (options->dump_tile >= 0) {
    const char* output = (options->dump_ppm != nullptr) ? options->dump_ppm : "/tmp/ra8_tile.ppm";
    return internal_dump_tile(reader,
                              (uint32_t)options->dump_tile,
                              output,
                              scratch,
                              scratch_bytes,
                              diagnostic)
             ? 0
             : 1;
  }
  if (options->headless || (options->dump_ppm != nullptr)) {
    return internal_render_page(reader, options, page, diagnostic) ? 0 : 1;
  }
  return internal_run_window(reader, scratch, scratch_bytes, diagnostic);
}

/** @brief Program entry point. */
int main(int argc, char** argv)
{
  ra8_io_stream_t             diagnostic       = {};
  ra8_io_stream_posix_state_t diagnostic_state = {};
  if (internal_bind_diagnostic(&diagnostic, &diagnostic_state) != k_ra8_ok) {
    return 1;
  }
  ra8_log_set_byte_sink(internal_log_sink, &diagnostic);
  viewer_options_t options = {};
  if (!internal_parse_arguments(argc, argv, &options)) {
    internal_usage(&diagnostic, argv[0]);
    return 2;
  }
  ra8_viewer_reader_requirements_t requirements = {};
  ra8_err_t error = ra8_viewer_reader_requirements(options.path, &requirements);
  if (error != k_ra8_ok) {
    (void)priv_viewer_output_open_error(&diagnostic, options.path, error);
    return 1;
  }
  ra8_viewer_reader_t*          reader = nullptr;
  ra8_viewer_workspace_report_t report = {};
  error                                = ra8_viewer_reader_bind(&reader,
                                                                s_viewer_composition,
                                                                sizeof(s_viewer_composition),
                                                                &requirements,
                                                                &report);
  if (error != k_ra8_ok) {
    internal_report_capacity(&diagnostic, "reader", &report);
    (void)priv_viewer_output_open_error(&diagnostic, options.path, error);
    return 1;
  }
  error = ra8_viewer_open(reader, options.path);
  if (error != k_ra8_ok) {
    (void)priv_viewer_output_open_error(&diagnostic, options.path, error);
    return 1;
  }
  const uint32_t count = ra8_viewer_page_count(reader);
  const uint32_t page  = internal_clamp_page(options.page, count);
  (void)priv_viewer_output_opened(&diagnostic, options.path, count);
  const int status = internal_execute(reader, &requirements, &options, page, &diagnostic);
  ra8_viewer_close(reader);
  return status;
}
