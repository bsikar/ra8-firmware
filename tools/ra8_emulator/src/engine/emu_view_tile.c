/**
 * @file emu_view_tile.c
 * @brief Pure bounded tile rotation plus checked raw-surface row writes
 * @details Maps native panel coordinates through each supported display
 * rotation and writes exact RGB565 row spans into the descriptor-backed
 * composite while rejecting overflow and out-of-range placements.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "emu_view.h"
#include "emu_view_tile_internal.h"

/**
 * @brief Return the raw-surface byte offset of one display row.
 * @details Return the raw-surface byte offset of one display row; this step is contained within the emu view tile model and uses bounded caller or module-owned storage.
 * @param[in] presentation Descriptor-backed presentation workspace accessed by the operation.
 * @param[in] x Horizontal coordinate in pixels.
 * @param[in] y Vertical coordinate in pixels.
 * @return The row offset result produced by the emu view tile model.
 * @retval value The operation-specific row offset value.
 * @pre Arguments satisfy the ranges documented for row offset. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu view tile model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static size_t
internal_row_offset(const emu_presentation_workspace_t* presentation, uint16_t x, uint16_t y)
{
  return (((size_t)y * presentation->composite_width) + x) * sizeof(uint16_t);
}

/** @brief Rotated tile placement and dimensions. */
typedef struct {
  uint16_t x;      /**< Composite destination column. */
  uint16_t y;      /**< Composite destination row.    */
  uint16_t width;  /**< Rotated tile width.           */
  uint16_t height; /**< Rotated tile height.          */
} tile_destination_t;

/**
 * @brief Write one contiguous tile into checked raw-surface rows.
 * @details Write one contiguous tile into checked raw-surface rows; this step is contained within the emu view tile model and uses bounded caller or module-owned storage.
 * @param[in,out] presentation Descriptor-backed presentation workspace accessed by the operation.
 * @param[in] pixels Pixels input used by the operation.
 * @param[in] destination Destination input used by the operation.
 * @return The write rows result produced by the emu view tile model.
 * @retval true The write rows condition holds or completed successfully; false otherwise.
 * @pre Arguments satisfy the ranges documented for write rows. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu view tile model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_write_rows(emu_presentation_workspace_t* presentation,
                                             const uint16_t*               pixels,
                                             const tile_destination_t*     destination)
{
  for (uint16_t row = 0U; row < destination->height; row++) {
    if (!emu_presentation_write(
          presentation,
          internal_row_offset(presentation, destination->x, (uint16_t)(destination->y + row)),
          &pixels[(size_t)row * destination->width],
          (size_t)destination->width * sizeof(uint16_t))) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Resolve one source tile's placement after panel rotation.
 * @details Resolve one source tile's placement after panel rotation; this step is contained within the emu view tile model and uses bounded caller or module-owned storage.
 * @param[in] presentation Descriptor-backed presentation workspace accessed by the operation.
 * @param[in] source_x Source x input used by the operation.
 * @param[in] source_y Source y input used by the operation.
 * @param[in] width Width of the affected region in pixels.
 * @param[in] height Height of the affected region in pixels.
 * @return The tile destination result produced by the emu view tile model.
 * @retval value The operation-specific tile destination value.
 * @pre Arguments satisfy the ranges documented for tile destination. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu view tile model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static tile_destination_t
internal_tile_destination(const emu_presentation_workspace_t* presentation,
                          uint16_t                            source_x,
                          uint16_t                            source_y,
                          uint16_t                            width,
                          uint16_t                            height)
{
  tile_destination_t destination = {.x = source_x, .y = source_y, .width = width, .height = height};
  if ((presentation->rotate_deg == (uint32_t)k_rotate_90) ||
      (presentation->rotate_deg == (uint32_t)k_rotate_270)) {
    destination.width  = height;
    destination.height = width;
  }
  if (presentation->rotate_deg == (uint32_t)k_rotate_90) {
    destination.x = (uint16_t)(presentation->panel_height - source_y - height);
    destination.y = source_x;
  } else if (presentation->rotate_deg == (uint32_t)k_rotate_180) {
    destination.x = (uint16_t)(presentation->panel_width - source_x - width);
    destination.y = (uint16_t)(presentation->panel_height - source_y - height);
  } else if (presentation->rotate_deg == (uint32_t)k_rotate_270) {
    destination.x = source_y;
    destination.y = (uint16_t)(presentation->panel_width - source_x - width);
  }
  return destination;
}

/**
 * @brief Rotate one bounded RGB565 tile into caller scratch.
 * @details Rotate one bounded rgb565 tile into caller scratch; this step is contained within the emu view tile model and uses bounded caller or module-owned storage.
 * @param[in] presentation Descriptor-backed presentation workspace accessed by the operation.
 * @param[in] source Source input used by the operation.
 * @param[in,out] rotated Rotated state or storage updated in place by the operation.
 * @param[in] width Width of the affected region in pixels.
 * @param[in] height Height of the affected region in pixels.
 * @param[in] output_width Output width input used by the operation.
 * @pre Arguments satisfy the ranges documented for rotate tile. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu view tile model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_rotate_tile(const emu_presentation_workspace_t* presentation,
                                              const uint16_t*                     source,
                                              uint16_t*                           rotated,
                                              uint16_t                            width,
                                              uint16_t                            height,
                                              uint16_t                            output_width)
{
  for (uint16_t y = 0U; y < height; y++) {
    for (uint16_t x = 0U; x < width; x++) {
      /* The caller rejects 0 degrees, so 270 is the remaining default. */
      uint16_t destination_x = y;
      uint16_t destination_y = (uint16_t)(width - 1U - x);
      if (presentation->rotate_deg == (uint32_t)k_rotate_90) {
        destination_x = (uint16_t)(height - 1U - y);
        destination_y = x;
      } else if (presentation->rotate_deg == (uint32_t)k_rotate_180) {
        destination_x = (uint16_t)(width - 1U - x);
        destination_y = (uint16_t)(height - 1U - y);
      } else {
        /* 270 degrees: the seeded destination above already describes it. */
      }
      rotated[((size_t)destination_y * output_width) + destination_x] =
        source[((size_t)y * width) + x];
    }
  }
}

bool priv_emu_view_tile_write(emu_presentation_workspace_t* presentation,
                              const uint16_t*               source,
                              uint16_t                      source_x,
                              uint16_t                      source_y,
                              uint16_t                      width,
                              uint16_t                      height)
{
  if ((presentation == nullptr) || (source == nullptr) || (width == 0U) || (height == 0U) ||
      ((size_t)source_x + width > presentation->panel_width) ||
      ((size_t)source_y + height > presentation->panel_height)) {
    return false;
  }
  if (presentation->rotate_deg == 0U) {
    const tile_destination_t destination = {.x      = source_x,
                                            .y      = source_y,
                                            .width  = width,
                                            .height = height};
    return internal_write_rows(presentation, source, &destination);
  }
  const size_t    max_tile_width  = (presentation->panel_width < k_emu_presentation_tile_px)
                                      ? presentation->panel_width
                                      : k_emu_presentation_tile_px;
  const size_t    max_tile_height = (presentation->panel_height < k_emu_presentation_tile_px)
                                      ? presentation->panel_height
                                      : k_emu_presentation_tile_px;
  uint16_t* const rotated =
    &((uint16_t*)(void*)presentation->scratch)[max_tile_width * max_tile_height];
  const tile_destination_t destination =
    internal_tile_destination(presentation, source_x, source_y, width, height);
  internal_rotate_tile(presentation, source, rotated, width, height, destination.width);
  return internal_write_rows(presentation, rotated, &destination);
}
