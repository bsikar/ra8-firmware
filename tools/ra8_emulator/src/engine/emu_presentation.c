/**
 * @file emu_presentation.c
 * @brief Checked raw-fd presentation workspace implementation
 * @details Plans overflow-safe surface geometry and manages unlinked temporary
 * descriptors for the raw composite and bounded scratch storage, with exact
 * injected I/O and explicit ownership transfer on every lifecycle path.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "emu_presentation.h"

#include <errno.h>
#include <fcntl.h>
#include <stdalign.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "emu_host_io_internal.h"
#include "emu_view.h"

/** @brief Compile-time invariant values for presentation storage. */
typedef enum : size_t {
  k_presentation_expected_scratch_bytes = 16384U, /**< Two maximum RGB565 tiles. */
} presentation_invariant_t;

/**
 * @brief Create, unlink, and exactly size one anonymous temporary descriptor.
 * @details Create, unlink, and exactly size one anonymous temporary descriptor; this step is contained within the emu presentation model and uses bounded caller or module-owned storage.
 * @param[in] size Size of the requested region or access in bytes.
 * @param[out] os_error Storage receiving the host error code on failure.
 * @return The anonymous descriptor result produced by the emu presentation model.
 * @retval value The operation-specific anonymous descriptor value.
 * @pre Arguments satisfy the ranges documented for anonymous descriptor. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu presentation model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_anonymous_fd(size_t size, int* os_error)
{
  char      temporary[] = "/tmp/ra8-emu-present.XXXXXX";
  const int fd          = mkstemp(temporary);
  if (fd < 0) {
    *os_error = errno;
    return -1;
  }
  const int unlink_result = unlink(temporary);
  const int unlink_error  = errno;
  if ((unlink_result != 0) || (ftruncate(fd, (off_t)size) != 0)) {
    *os_error = (unlink_result != 0) ? unlink_error : errno;
    (void)close(fd);
    if (unlink_result != 0) {
      (void)unlink(temporary);
    }
    return -1;
  }
  return fd;
}

/** @brief Complete internal geometry and storage plan. */
typedef struct {
  size_t panel_width;      /**< Native width.                    */
  size_t panel_height;     /**< Native height.                   */
  size_t display_width;    /**< Rotation-adjusted width.         */
  size_t display_height;   /**< Rotation-adjusted height.        */
  size_t composite_width;  /**< Display plus sidebar.            */
  size_t composite_height; /**< Display/sidebar maximum.         */
  size_t surface_bytes;    /**< Exact raw RGB565 surface length. */
  size_t scratch_bytes;    /**< Exact tile/row scratch length.   */
} emu_presentation_plan_t;

/**
 * @brief Construct a complete operation result.
 * @details Construct a complete operation result; this step is contained within the emu presentation model and uses bounded caller or module-owned storage.
 * @param[in] status Status value consumed or published by the operation.
 * @param[in] surface Descriptor-backed presentation surface to access.
 * @param[in] required_scratch Required scratch input used by the operation.
 * @param[in] supplied_scratch Supplied scratch input used by the operation.
 * @param[in] os_error Storage receiving the host error code on failure.
 * @return The result result produced by the emu presentation model.
 * @retval value The operation-specific result value.
 * @pre Arguments satisfy the ranges documented for result. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu presentation model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static emu_presentation_result_t internal_result(emu_presentation_status_t status,
                                                              size_t                    surface,
                                                              size_t required_scratch,
                                                              size_t supplied_scratch,
                                                              int    os_error)
{
  return (emu_presentation_result_t){.status                 = status,
                                     .required_surface_bytes = surface,
                                     .required_scratch_bytes = required_scratch,
                                     .supplied_scratch_bytes = supplied_scratch,
                                     .os_error               = os_error};
}

/**
 * @brief Checked size addition.
 * @details Checked size addition; this step is contained within the emu presentation model and uses bounded caller or module-owned storage.
 * @param[in] left Left input used by the operation.
 * @param[in] right Right input used by the operation.
 * @param[out] result Storage receiving the computed operation result.
 * @return The add result produced by the emu presentation model.
 * @retval true The add condition holds or completed successfully; false otherwise.
 * @pre Arguments satisfy the ranges documented for add. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu presentation model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_add(size_t left, size_t right, size_t* result)
{
  if ((result == nullptr) || (right > (SIZE_MAX - left))) {
    return false;
  }
  *result = left + right;
  return true;
}

/**
 * @brief Checked size multiplication.
 * @details Checked size multiplication; this step is contained within the emu presentation model and uses bounded caller or module-owned storage.
 * @param[in] left Left input used by the operation.
 * @param[in] right Right input used by the operation.
 * @param[out] result Storage receiving the computed operation result.
 * @return The multiply result produced by the emu presentation model.
 * @retval true The multiply condition holds or completed successfully; false otherwise.
 * @pre Arguments satisfy the ranges documented for multiply. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu presentation model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_multiply(size_t left, size_t right, size_t* result)
{
  if ((result == nullptr) || ((left != 0U) && (right > (SIZE_MAX / left)))) {
    return false;
  }
  *result = left * right;
  return true;
}

/**
 * @brief True for one supported clockwise rotation.
 * @details True for one supported clockwise rotation; this step is contained within the emu presentation model and uses bounded caller or module-owned storage.
 * @param[in] rotation Rotation input used by the operation.
 * @return The rotation valid result produced by the emu presentation model.
 * @retval true The rotation valid condition holds or completed successfully; false otherwise.
 * @pre Arguments satisfy the ranges documented for rotation valid. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu presentation model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_rotation_valid(uint32_t rotation)
{
  return (rotation == (uint32_t)k_rotate_0) || (rotation == (uint32_t)k_rotate_90) ||
         (rotation == (uint32_t)k_rotate_180) || (rotation == (uint32_t)k_rotate_270);
}

/**
 * @brief Compute all checked geometry and storage values.
 * @details Compute all checked geometry and storage values; this step is contained within the emu presentation model and uses bounded caller or module-owned storage.
 * @param[in] spec Spec input used by the operation.
 * @param[in,out] plan Plan state or storage updated in place by the operation.
 * @return The plan result produced by the emu presentation model.
 * @retval value The operation-specific plan value.
 * @pre Arguments satisfy the ranges documented for plan. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu presentation model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static emu_presentation_status_t internal_plan(const emu_presentation_spec_t* spec,
                                                            emu_presentation_plan_t*       plan)
{
  if ((spec == nullptr) || (plan == nullptr) || (spec->panel_width == 0U) ||
      (spec->panel_height == 0U) || !internal_rotation_valid(spec->rotate_deg)) {
    return k_emu_presentation_invalid;
  }
  const bool swap =
    (spec->rotate_deg == (uint32_t)k_rotate_90) || (spec->rotate_deg == (uint32_t)k_rotate_270);
  const size_t display_width   = swap ? spec->panel_height : spec->panel_width;
  const size_t display_height  = swap ? spec->panel_width : spec->panel_height;
  size_t       composite_width = 0U;
  if (!internal_add(display_width, (size_t)k_board_overlay_sidebar_width_px, &composite_width)) {
    return k_emu_presentation_overflow;
  }
  const size_t composite_height = (display_height > (size_t)k_board_overlay_min_height_px)
                                    ? display_height
                                    : (size_t)k_board_overlay_min_height_px;
  size_t       composite_pixels = 0U;
  size_t       surface_bytes    = 0U;
  if (!internal_multiply(composite_width, composite_height, &composite_pixels) ||
      !internal_multiply(composite_pixels, sizeof(uint16_t), &surface_bytes)) {
    return k_emu_presentation_overflow;
  }
  const size_t tile_width  = (spec->panel_width < k_emu_presentation_tile_px)
                               ? spec->panel_width
                               : k_emu_presentation_tile_px;
  const size_t tile_height = (spec->panel_height < k_emu_presentation_tile_px)
                               ? spec->panel_height
                               : k_emu_presentation_tile_px;
  size_t       tile_pixels = 0U;
  size_t       tile_bytes  = 0U;
  size_t       row_bytes   = 0U;
  if (!internal_multiply(tile_width, tile_height, &tile_pixels) ||
      !internal_multiply(tile_pixels, sizeof(uint16_t), &tile_bytes) ||
      !internal_multiply(composite_width, sizeof(uint16_t), &row_bytes)) {
    return k_emu_presentation_overflow;
  }
  size_t rotation_bytes = tile_bytes;
  if ((spec->rotate_deg != 0U) && !internal_multiply(rotation_bytes, 2U, &rotation_bytes)) {
    return k_emu_presentation_overflow;
  }
  const size_t scratch_bytes = (row_bytes > rotation_bytes) ? row_bytes : rotation_bytes;
  if ((spec->panel_width > k_emu_presentation_max_panel_px) ||
      (spec->panel_height > k_emu_presentation_max_panel_px) || (composite_width > UINT16_MAX) ||
      (composite_height > UINT16_MAX)) {
    return k_emu_presentation_invalid;
  }
  *plan = (emu_presentation_plan_t){.panel_width      = spec->panel_width,
                                    .panel_height     = spec->panel_height,
                                    .display_width    = display_width,
                                    .display_height   = display_height,
                                    .composite_width  = composite_width,
                                    .composite_height = composite_height,
                                    .surface_bytes    = spec->active ? surface_bytes : 0U,
                                    .scratch_bytes    = spec->active ? scratch_bytes : 0U};
  return k_emu_presentation_ok;
}

emu_presentation_result_t emu_presentation_requirements(const emu_presentation_spec_t* spec)
{
  emu_presentation_plan_t         plan   = {};
  const emu_presentation_status_t status = internal_plan(spec, &plan);
  return internal_result(status,
                         (status == k_emu_presentation_ok) ? plan.surface_bytes : 0U,
                         (status == k_emu_presentation_ok) ? plan.scratch_bytes : 0U,
                         0U,
                         0);
}

emu_presentation_result_t emu_presentation_open(const emu_presentation_spec_t* spec,
                                                void*                          scratch,
                                                size_t supplied_scratch_bytes,
                                                emu_presentation_workspace_t* workspace)
{
  emu_presentation_plan_t   plan   = {};
  emu_presentation_status_t status = internal_plan(spec, &plan);
  if ((status == k_emu_presentation_ok) && (plan.scratch_bytes > supplied_scratch_bytes)) {
    status = k_emu_presentation_capacity;
  }
  if ((status == k_emu_presentation_ok) && (plan.scratch_bytes > 0U) && (scratch == nullptr)) {
    status = k_emu_presentation_invalid;
  }
  if ((status == k_emu_presentation_ok) && (plan.scratch_bytes > 0U) &&
      (((uintptr_t)scratch % alignof(uint16_t)) != 0U)) {
    status = k_emu_presentation_invalid;
  }
  if ((status != k_emu_presentation_ok) || (workspace == nullptr)) {
    return internal_result((workspace == nullptr) ? k_emu_presentation_invalid : status,
                           plan.surface_bytes,
                           plan.scratch_bytes,
                           supplied_scratch_bytes,
                           0);
  }
  int fd = -1;
  if (plan.surface_bytes > 0U) {
    int os_error = 0;
    fd           = internal_anonymous_fd(plan.surface_bytes, &os_error);
    if (fd < 0) {
      return internal_result(k_emu_presentation_io,
                             plan.surface_bytes,
                             plan.scratch_bytes,
                             supplied_scratch_bytes,
                             os_error);
    }
  }
  const emu_presentation_workspace_t ready = {
    .fd               = fd,
    .panel_width      = (uint16_t)plan.panel_width,
    .panel_height     = (uint16_t)plan.panel_height,
    .display_width    = (uint16_t)plan.display_width,
    .display_height   = (uint16_t)plan.display_height,
    .composite_width  = (uint16_t)plan.composite_width,
    .composite_height = (uint16_t)plan.composite_height,
    .rotate_deg       = spec->rotate_deg,
    .scratch          = (uint8_t*)scratch,
    .scratch_bytes    = plan.scratch_bytes,
    .surface_bytes    = plan.surface_bytes,
  };
  *workspace = ready;
  return internal_result(k_emu_presentation_ok,
                         plan.surface_bytes,
                         plan.scratch_bytes,
                         supplied_scratch_bytes,
                         0);
}

bool emu_presentation_close(emu_presentation_workspace_t* workspace)
{
  if (workspace == nullptr) {
    return false;
  }
  const int fd = workspace->fd;
  *workspace   = (emu_presentation_workspace_t){.fd = -1};
  return (fd < 0) || (close(fd) == 0);
}

bool emu_presentation_read(const emu_presentation_workspace_t* workspace,
                           size_t                              offset,
                           void*                               bytes,
                           size_t                              count)
{
  if ((workspace == nullptr) || (workspace->fd < 0) || (bytes == nullptr) ||
      (offset > workspace->surface_bytes) || (count > (workspace->surface_bytes - offset)) ||
      (offset > (size_t)INT64_MAX)) {
    return false;
  }
  return priv_emu_io_pread_exact(workspace->fd, bytes, count, (off_t)offset).status == k_emu_io_ok;
}

bool emu_presentation_write(emu_presentation_workspace_t* workspace,
                            size_t                        offset,
                            const void*                   bytes,
                            size_t                        count)
{
  if ((workspace == nullptr) || (workspace->fd < 0) || (bytes == nullptr) ||
      (offset > workspace->surface_bytes) || (count > (workspace->surface_bytes - offset)) ||
      (offset > (size_t)INT64_MAX)) {
    return false;
  }
  return priv_emu_io_pwrite_exact(workspace->fd, bytes, count, (off_t)offset).status == k_emu_io_ok;
}

bool emu_presentation_fill(void*    context,
                           uint16_t x,
                           uint16_t y,
                           uint16_t width,
                           uint16_t height,
                           uint16_t color)
{
  emu_presentation_workspace_t* const workspace = (emu_presentation_workspace_t*)context;
  if ((workspace == nullptr) || (width == 0U) || (height == 0U) ||
      ((size_t)x + width > workspace->composite_width) ||
      ((size_t)y + height > workspace->composite_height)) {
    return false;
  }
  const size_t row_bytes = (size_t)width * sizeof(uint16_t);
  if (row_bytes > workspace->scratch_bytes) {
    return false;
  }
  /* One row of RGB565 pixels in the borrowed scratch. Alignment to
   * alignof(uint16_t) is a checked precondition of emu_presentation_open(), so
   * the union names one storage under two views instead of casting a uint8_t*
   * through void*. */
  const union {
    uint8_t*  bytes;  /**< Borrowed scratch as raw bytes.     */
    uint16_t* pixels; /**< The same storage as RGB565 pixels. */
  } scratch           = {.bytes = workspace->scratch};
  uint16_t* const row = scratch.pixels;
  /* The borrowed scratch is documented "or nullptr"; this is the one place the
   * fill path needs it, so the check lives here rather than in the guard above. */
  if (row == nullptr) {
    return false;
  }
  for (uint16_t column = 0U; column < width; column++) {
    row[column] = color;
  }
  for (uint16_t delta = 0U; delta < height; delta++) {
    const size_t pixel_offset =
      ((size_t)(y + delta) * (size_t)workspace->composite_width) + (size_t)x;
    if (!emu_presentation_write(workspace, pixel_offset * sizeof(uint16_t), row, row_bytes)) {
      return false;
    }
  }
  return true;
}

bool emu_presentation_snapshot(emu_presentation_workspace_t* workspace, int* snapshot_fd)
{
  if ((workspace == nullptr) || (snapshot_fd == nullptr) || (workspace->fd < 0) ||
      (workspace->scratch == nullptr) || (workspace->scratch_bytes == 0U)) {
    return false;
  }
  int       os_error = 0;
  const int fd       = internal_anonymous_fd(workspace->surface_bytes, &os_error);
  (void)os_error;
  if (fd < 0) {
    return false;
  }
  bool ok = true;
  for (size_t offset = 0U; ok && (offset < workspace->surface_bytes);) {
    const size_t count = ((workspace->surface_bytes - offset) < workspace->scratch_bytes)
                           ? (workspace->surface_bytes - offset)
                           : workspace->scratch_bytes;
    ok = emu_presentation_read(workspace, offset, workspace->scratch, count) &&
         (priv_emu_io_pwrite_exact(fd, workspace->scratch, count, (off_t)offset).status ==
          k_emu_io_ok);
    offset += count;
  }
  if (!ok) {
    (void)close(fd);
    return false;
  }
  *snapshot_fd = fd;
  return true;
}

static_assert((size_t)k_emu_presentation_max_scratch_bytes ==
                (size_t)k_presentation_expected_scratch_bytes,
              "maximum presentation scratch budget changed");
