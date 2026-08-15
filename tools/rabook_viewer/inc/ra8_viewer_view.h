/**
 * @file ra8_viewer_view.h
 * @brief Caller-owned C ABI for the Cocoa scrolling JOF view.
 * @details First-party C state, layout arrays, and pixel conversion scratch are
 * bound from caller storage. AppKit and CoreGraphics object ownership remains an
 * explicit platform/SOUP dependency and is not represented as first-party heap.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"
#include "ra8_viewer_reader.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Opaque view state bound inside caller workspace. */
typedef struct ra8_viewer_view ra8_viewer_view_t;

/** @brief Exact first-party workspace required by one platform view. */
typedef struct {
  size_t   required_bytes;     /**< Complete view workspace extent.       */
  size_t   required_alignment; /**< Required base alignment.              */
  size_t   pixel_bytes;        /**< In-place RGB565-to-ARGB tile scratch. */
  size_t   layout_bytes;       /**< Two per-tile layout arrays.           */
  uint32_t tile_count;         /**< Reader tile count used for sizing.    */
  uint32_t layout_version;     /**< ABI guard consumed by open.           */
} ra8_viewer_view_requirements_t;

/** @brief Calculate the exact view workspace for @p reader. */
[[nodiscard]] ra8_err_t ra8_viewer_view_requirements(const ra8_viewer_reader_t*      reader,
                                                     ra8_viewer_view_requirements_t* out);

/**
 * @brief Open a scrolling window into caller-owned first-party workspace.
 * @param[out] out Receives the bound view on success.
 * @param[in] reader Open reader that outlives the view.
 * @param[in] title Optional NUL-terminated title.
 * @param[in,out] workspace Caller-owned backing.
 * @param[in] workspace_bytes Accessible backing extent.
 * @param[in] requirements Unmodified requirements result.
 * @param[out] report Exact required/supplied evidence.
 * @return ::k_ra8_ok on success or a platform/capacity error.
 */
[[nodiscard]] ra8_err_t ra8_viewer_view_open(ra8_viewer_view_t**                   out,
                                             ra8_viewer_reader_t*                  reader,
                                             const char*                           title,
                                             void*                                 workspace,
                                             size_t                                workspace_bytes,
                                             const ra8_viewer_view_requirements_t* requirements,
                                             ra8_viewer_workspace_report_t*        report);

/** @brief Drain pending events; true after the window closes. */
[[nodiscard]] bool ra8_viewer_view_pump(ra8_viewer_view_t* view);

/**
 * @brief Close the window and reset borrowed view state without freeing it.
 * @details Removes notifications, closes AppKit objects, and clears C references.
 * @param[in,out] view Bound view, or NULL.
 * @pre @p view is NULL or came from a successful open.
 * @pre No pump call is in progress.
 * @post The platform window is closed when non-NULL.
 * @post Caller workspace and reader ownership remain unchanged.
 * @note Must execute on the creating thread; NULL is safe.
 * @since 0.1.0
 */
void ra8_viewer_view_close(ra8_viewer_view_t* view);

#ifdef __cplusplus
}
#endif
