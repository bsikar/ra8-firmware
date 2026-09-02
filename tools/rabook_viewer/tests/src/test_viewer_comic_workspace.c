/**
 * @file test_viewer_comic_workspace.c
 * @brief Bare-comic caller-workspace and pixel-content regression test.
 * @details Proves exact one-byte-short reader/tile refusal, opens the committed
 * CBZ through the production descriptor-backed engine, and renders non-white
 * pixels without heap, streams, mappings, or hidden storage.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_viewer_reader.h"

/** @brief Fixed test-only caller storage capacities. */
typedef enum : uint32_t {
  k_test_comic_reader_bytes = 16U * 1024U * 1024U, /**< Complete comic reader. */
  k_test_comic_tile_bytes   = 2U * 1024U * 1024U,  /**< One 600x900 tile.      */
  k_test_page_count         = 3U,                  /**< Committed CBZ pages.   */
  k_test_page_width         = 600U,                /**< Committed page width.  */
  k_test_page_height        = 900U,                /**< Committed page height. */
  k_test_sentinel           = 0xA5U,               /**< Short-bind fill byte.  */
  k_test_white_rgb565       = 0xFFFFU,             /**< Empty white pixel.     */
} viewer_comic_test_limit_t;

alignas(max_align_t) static uint8_t s_reader[k_test_comic_reader_bytes];
alignas(max_align_t) static uint8_t s_tile[k_test_comic_tile_bytes];

/**
 * @brief Prove an exact one-byte-short reader bind is failure-atomic.
 * @details Fills the backing with a sentinel, attempts the short bind, and
 * checks exact required/supplied evidence plus both affected edge bytes.
 * @param[in] requirements Canonical comic requirements.
 * @return Whether every rejection assertion held.
 * @retval true The bind failed exactly and mutated no observed byte.
 * @retval false Capacity, status, publication, or sentinel evidence differed.
 * @pre @p requirements is non-NULL and fits the reader backing.
 * @pre Required bytes are non-zero.
 * @post No reader is published.
 * @post The shared reader backing remains caller-owned.
 * @note Test-only and single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_short_reader(const ra8_viewer_reader_requirements_t* requirements)
{
  if ((requirements->required_bytes == 0U) || (requirements->required_bytes > sizeof(s_reader))) {
    return false;
  }
  memset(s_reader, (int)k_test_sentinel, sizeof(s_reader));
  ra8_viewer_reader_t*          reader = (ra8_viewer_reader_t*)(uintptr_t)1U;
  ra8_viewer_workspace_report_t report = {};
  const ra8_err_t               error  = ra8_viewer_reader_bind(&reader,
                                                                s_reader,
                                                                requirements->required_bytes - 1U,
                                                                requirements,
                                                                &report);
  return (error == k_ra8_err_invalid_size) && (reader == nullptr) &&
         (report.required_bytes == requirements->required_bytes) &&
         (report.supplied_bytes == (requirements->required_bytes - 1U)) &&
         (s_reader[0] == (uint8_t)k_test_sentinel) &&
         (s_reader[requirements->required_bytes - 1U] == (uint8_t)k_test_sentinel);
}

/**
 * @brief Check that the rendered tile contains actual page pixels.
 * @details Scans the exact RGB565 output and requires at least one non-white
 * pixel, preventing a successful clear-only render from satisfying the test.
 * @param[in] pixels Rendered RGB565 tile.
 * @param[in] pixel_count Exact pixel count.
 * @return Whether any pixel differs from white.
 * @retval true The decoder produced visible content.
 * @retval false The entire tile is an untouched white field.
 * @pre @p pixels spans @p pixel_count readable entries.
 * @pre @p pixel_count is positive and bounded by the test tile.
 * @post No state is mutated.
 * @post Every examined pixel lies inside the caller buffer.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_has_content(const uint16_t* pixels, size_t pixel_count)
{
  for (size_t index = 0U; index < pixel_count; ++index) {
    if (pixels[index] != (uint16_t)k_test_white_rgb565) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Open and render the committed comic through caller-owned storage.
 * @details Binds after the shortfall proof, checks page geometry, rejects a
 * one-byte-short tile, then renders exact non-white output and closes cleanly.
 * @param[in] path NUL-terminated committed CBZ path.
 * @param[in] requirements Canonical comic requirements.
 * @return Whether every lifecycle and rendering assertion held.
 * @retval true The production reader completed the complete proof.
 * @retval false Any bind, open, sizing, rendering, or close assertion failed.
 * @pre Both pointers are non-NULL.
 * @pre Reader and tile arrays meet the requirements.
 * @post Any published reader is closed before return.
 * @post Caller arrays remain statically owned.
 * @note Test-only and single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_render_comic(const char*                             path,
                                               const ra8_viewer_reader_requirements_t* requirements)
{
  ra8_viewer_reader_t*          reader      = nullptr;
  ra8_viewer_workspace_report_t bind_report = {};
  if ((ra8_viewer_reader_bind(&reader, s_reader, sizeof(s_reader), requirements, &bind_report) !=
       k_ra8_ok) ||
      (ra8_viewer_open(reader, path) != k_ra8_ok) ||
      (ra8_viewer_page_count(reader) != (uint32_t)k_test_page_count) ||
      (ra8_viewer_render_page(reader, 0U) != k_ra8_ok)) {
    ra8_viewer_close(reader);
    return false;
  }
  size_t required  = 0U;
  size_t alignment = 0U;
  if ((ra8_viewer_tile_requirements(reader, 0U, &required, &alignment) != k_ra8_ok) ||
      (required > sizeof(s_tile)) || (alignment > alignof(max_align_t))) {
    ra8_viewer_close(reader);
    return false;
  }
  uint32_t                      width        = 0U;
  uint32_t                      height       = 0U;
  uint16_t*                     pixels       = nullptr;
  ra8_viewer_workspace_report_t short_report = {};
  if ((ra8_viewer_render_tile565(reader,
                                 0U,
                                 s_tile,
                                 required - 1U,
                                 &width,
                                 &height,
                                 &pixels,
                                 &short_report) != k_ra8_err_invalid_size) ||
      (pixels != nullptr) || (short_report.required_bytes != required)) {
    ra8_viewer_close(reader);
    return false;
  }
  ra8_viewer_workspace_report_t render_report = {};
  const bool rendered = (ra8_viewer_render_tile565(reader,
                                                   0U,
                                                   s_tile,
                                                   sizeof(s_tile),
                                                   &width,
                                                   &height,
                                                   &pixels,
                                                   &render_report) == k_ra8_ok) &&
                        (pixels == (uint16_t*)s_tile) && (width == (uint32_t)k_test_page_width) &&
                        (height == (uint32_t)k_test_page_height) &&
                        internal_has_content(pixels, (size_t)width * (size_t)height);
  ra8_viewer_close(reader);
  return rendered && (ra8_viewer_page_count(reader) == 0U);
}

/**
 * @brief Test entry point; argv[1] names the committed sample CBZ.
 * @details Runs requirements, exact shortfall, production open, and pixel proof.
 * @param[in] argc Argument count.
 * @param[in] argv Argument vector.
 * @return Process-style test status.
 * @retval 0 Every assertion passed.
 * @retval 1 A contract assertion failed.
 * @retval 2 The fixture argument was missing.
 * @pre @p argv spans @p argc entries.
 * @pre `argv[1]` is a readable regular CBZ when argc is two.
 * @post No descriptor remains open.
 * @post No heap or hosted stream is acquired.
 * @note Test process is single-threaded.
 * @since 0.1.0
 */
int main(int argc, char** argv)
{
  if (argc != 2) {
    return 2;
  }
  ra8_viewer_reader_requirements_t requirements = {};
  return (ra8_viewer_reader_requirements(argv[1], &requirements) == k_ra8_ok) &&
             (requirements.engine == (uint32_t)k_ra8_viewer_engine_comic) &&
             internal_short_reader(&requirements) && internal_render_comic(argv[1], &requirements)
           ? 0
           : 1;
}
