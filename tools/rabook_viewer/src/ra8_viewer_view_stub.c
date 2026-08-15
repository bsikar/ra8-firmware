/**
 * @file ra8_viewer_view_stub.c
 * @brief Non-Apple platform alternative for the Cocoa viewer ABI.
 * @details Reports the absence of a desktop view without allocating a fake
 * handle, while preserving the caller-workspace API on headless hosts.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_viewer_view.h"

ra8_err_t ra8_viewer_view_requirements(const ra8_viewer_reader_t*      reader,
                                       ra8_viewer_view_requirements_t* out)
{
  if ((reader == nullptr) || (out == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *out = (ra8_viewer_view_requirements_t){};
  return k_ra8_err_not_supported;
}

ra8_err_t ra8_viewer_view_open(ra8_viewer_view_t**                   out,
                               ra8_viewer_reader_t*                  reader,
                               const char*                           title,
                               void*                                 workspace,
                               size_t                                workspace_bytes,
                               const ra8_viewer_view_requirements_t* requirements,
                               ra8_viewer_workspace_report_t*        report)
{
  if ((out == nullptr) || (reader == nullptr) || (requirements == nullptr) || (report == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *out    = nullptr;
  *report = (ra8_viewer_workspace_report_t){.required_bytes = requirements->required_bytes,
                                            .supplied_bytes = workspace_bytes};
  (void)title;
  (void)workspace;
  return k_ra8_err_not_supported;
}

bool ra8_viewer_view_pump(ra8_viewer_view_t* view)
{
  (void)view;
  return true;
}

void ra8_viewer_view_close(ra8_viewer_view_t* view)
{
  (void)view;
}
