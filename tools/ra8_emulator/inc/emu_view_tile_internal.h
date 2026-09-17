/**
 * @file emu_view_tile_internal.h
 * @brief Internal bounded RGB565 tile rotation/writer
 * @details Declares the cross-translation-unit tile seam used by the streamed
 * surface builder to rotate caller-supplied RGB565 spans directly into the
 * checked presentation workspace.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include "emu_presentation.h"
#include "ra8_attributes.h"

/**
 * @brief Rotate and write one native-panel tile into its exact display location.
 * @details Rotate and write one native-panel tile into its exact display location; this step is contained within the emu view tile model and uses bounded caller or module-owned storage.
 * @param[in,out] presentation Descriptor-backed presentation workspace accessed by the operation.
 * @param[in] source Source input used by the operation.
 * @param[in] source_x Source x input used by the operation.
 * @param[in] source_y Source y input used by the operation.
 * @param[in] width Width of the affected region in pixels.
 * @param[in] height Height of the affected region in pixels.
 * @return The emu view tile write result produced by the emu view tile model.
 * @retval true The emu view tile write condition holds or completed successfully; false otherwise.
 * @pre Arguments satisfy the ranges documented for emu view tile write. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu view tile model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_PRIV bool priv_emu_view_tile_write(emu_presentation_workspace_t* presentation,
                                       const uint16_t*               source,
                                       uint16_t                      source_x,
                                       uint16_t                      source_y,
                                       uint16_t                      width,
                                       uint16_t                      height);
