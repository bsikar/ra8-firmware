/**
 * @file emu_view_surface_internal.h
 * @brief Internal streamed panel/composite builder seam
 * @details Declares the cross-translation-unit builder that snapshots live
 * GLCDC state into the caller-owned descriptor-backed presentation workspace
 * and composes the board overlay in bounded tiles.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <unicorn/unicorn.h>

#include "board_overlay.h"
#include "emu_presentation.h"
#include "ra8_attributes.h"

/**
 * @brief Build one exact fd-backed composite from live engine and status state.
 * @details Build one exact fd-backed composite from live engine and status state; this step is contained within the emu view surface model and uses bounded caller or module-owned storage.
 * @param[in,out] uc Unicorn engine whose emulated state is read or updated.
 * @param[in,out] presentation Descriptor-backed presentation workspace accessed by the operation.
 * @param[in] status Status value consumed or published by the operation.
 * @return The emu view surface build result produced by the emu view surface model.
 * @retval true The emu view surface build condition holds or completed successfully; false otherwise.
 * @pre Arguments satisfy the ranges documented for emu view surface build. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu view surface model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_PRIV bool priv_emu_view_surface_build(uc_engine*                    uc,
                                          emu_presentation_workspace_t* presentation,
                                          const board_status_t*         status);
