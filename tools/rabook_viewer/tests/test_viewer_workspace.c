/**
 * @file test_viewer_workspace.c
 * @brief Independent binding, exact shortfall, and caller-tile regression test.
 * @details Exercises two disjoint reader workspaces, failure-atomic one-byte
 * short binds, caller tile output, and post-close independence.
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

/** @brief Test backing caps for the generated 32x32 JOF. */
typedef enum : uint32_t {
  k_test_reader_bytes = 2U * 1024U * 1024U, /**< Per-reader backing.       */
  k_test_tile_bytes   = 4096U,              /**< 32x32 RGB565 plus margin. */
} viewer_test_budget_t;

alignas(max_align_t) static uint8_t s_first_reader[k_test_reader_bytes];
alignas(max_align_t) static uint8_t s_second_reader[k_test_reader_bytes];
alignas(max_align_t) static uint8_t s_short_reader[k_test_reader_bytes];
alignas(max_align_t) static uint8_t s_first_tile[k_test_tile_bytes];
alignas(max_align_t) static uint8_t s_second_tile[k_test_tile_bytes];

/**
 * @brief Prove one-byte-short bind rejection is exact and failure-atomic.
 * @details Fills a sentinel backing, attempts the short bind, and checks edges.
 * @param[in] need Valid requirements for the generated JOF.
 * @return Whether all shortfall assertions passed.
 * @retval true Required/supplied evidence and sentinels matched.
 * @retval false Any assertion failed.
 * @pre @p need is non-NULL.
 * @pre @p need fits the test backing.
 * @post No successful reader is published.
 * @post Sentinel edge bytes remain unchanged.
 * @note Test-only and single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_short_bind(const ra8_viewer_reader_requirements_t* need)
{
  if ((need->required_bytes == 0U) || (need->required_bytes > sizeof(s_short_reader))) {
    return false;
  }
  memset(s_short_reader, 0xA5, sizeof(s_short_reader));
  ra8_viewer_reader_t*          reader = (ra8_viewer_reader_t*)(uintptr_t)1U;
  ra8_viewer_workspace_report_t report = {};
  const ra8_err_t               error =
    ra8_viewer_reader_bind(&reader, s_short_reader, need->required_bytes - 1U, need, &report);
  return (error == k_ra8_err_invalid_size) && (reader == nullptr) &&
         (report.required_bytes == need->required_bytes) &&
         (report.supplied_bytes == (need->required_bytes - 1U)) && (s_short_reader[0] == 0xA5U) &&
         (s_short_reader[need->required_bytes - 1U] == 0xA5U);
}

/**
 * @brief Bind and open two readers over disjoint caller storage.
 * @details Uses the same immutable requirements with two independent backings.
 * @param[in] path Generated JOF path.
 * @param[in] need Valid requirements.
 * @param[out] first First reader.
 * @param[out] second Second reader.
 * @return Whether both distinct readers opened.
 * @retval true Both bindings and opens succeeded.
 * @retval false Any operation failed or addresses aliased.
 * @pre All pointers are non-NULL.
 * @pre Both backing arrays satisfy @p need.
 * @post Success publishes two open readers.
 * @post Failure is reclaimed by the caller cleanup path.
 * @note Test-only and single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_open_pair(const char*                             path,
                                            const ra8_viewer_reader_requirements_t* need,
                                            ra8_viewer_reader_t**                   first,
                                            ra8_viewer_reader_t**                   second)
{
  ra8_viewer_workspace_report_t first_report  = {};
  ra8_viewer_workspace_report_t second_report = {};
  if (ra8_viewer_reader_bind(first, s_first_reader, sizeof(s_first_reader), need, &first_report) !=
      k_ra8_ok) {
    return false;
  }
  if (ra8_viewer_reader_bind(second,
                             s_second_reader,
                             sizeof(s_second_reader),
                             need,
                             &second_report) != k_ra8_ok) {
    return false;
  }
  return (*first != *second) && (ra8_viewer_open(*first, path) == k_ra8_ok) &&
         (ra8_viewer_open(*second, path) == k_ra8_ok);
}

/**
 * @brief Prove exact tile shortfall and byte-identical independent renders.
 * @details Rejects one-byte-short output, then renders through both readers.
 * @param[in,out] first First open reader.
 * @param[in,out] second Second open reader.
 * @return Whether all tile assertions passed.
 * @retval true Shortfall evidence and rendered bytes matched.
 * @retval false Any query or render assertion failed.
 * @pre @p first is open.
 * @pre @p second is open and independent.
 * @post Both readers remain open.
 * @post Success leaves byte-identical caller tile buffers.
 * @note Test-only and single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_render_pair(ra8_viewer_reader_t* first,
                                              ra8_viewer_reader_t* second)
{
  size_t required  = 0U;
  size_t alignment = 0U;
  if ((ra8_viewer_tile_requirements(first, 0U, &required, &alignment) != k_ra8_ok) ||
      (required > sizeof(s_first_tile))) {
    return false;
  }
  uint32_t                      width        = 0U;
  uint32_t                      height       = 0U;
  uint16_t*                     pixels       = nullptr;
  ra8_viewer_workspace_report_t short_report = {};
  if ((ra8_viewer_render_tile565(first,
                                 0U,
                                 s_first_tile,
                                 required - 1U,
                                 &width,
                                 &height,
                                 &pixels,
                                 &short_report) != k_ra8_err_invalid_size) ||
      (short_report.required_bytes != required) || (pixels != nullptr)) {
    return false;
  }
  ra8_viewer_workspace_report_t first_report  = {};
  ra8_viewer_workspace_report_t second_report = {};
  return (ra8_viewer_render_tile565(first,
                                    0U,
                                    s_first_tile,
                                    sizeof(s_first_tile),
                                    &width,
                                    &height,
                                    &pixels,
                                    &first_report) == k_ra8_ok) &&
         (ra8_viewer_render_tile565(second,
                                    0U,
                                    s_second_tile,
                                    sizeof(s_second_tile),
                                    &width,
                                    &height,
                                    &pixels,
                                    &second_report) == k_ra8_ok) &&
         (memcmp(s_first_tile, s_second_tile, required) == 0);
}

/** @brief Test entry point; argv[1] is the generated legitimate JOF. */
int main(int argc, char** argv)
{
  if (argc != 2) {
    return 2;
  }
  ra8_viewer_reader_requirements_t need = {};
  if ((ra8_viewer_reader_requirements(argv[1], &need) != k_ra8_ok) || !internal_short_bind(&need)) {
    return 1;
  }
  ra8_viewer_reader_t* first  = nullptr;
  ra8_viewer_reader_t* second = nullptr;
  if (!internal_open_pair(argv[1], &need, &first, &second) ||
      !internal_render_pair(first, second)) {
    ra8_viewer_close(first);
    ra8_viewer_close(second);
    return 1;
  }
  ra8_viewer_close(first);
  const bool second_survives = ra8_viewer_render_page(second, 0U) == k_ra8_ok;
  ra8_viewer_close(second);
  return second_survives ? 0 : 1;
}
