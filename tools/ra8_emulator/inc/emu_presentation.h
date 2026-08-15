/**
 * @file emu_presentation.h
 * @brief Bounded raw-descriptor presentation surface and scratch workspace
 * @details Plans exact disk and caller-scratch requirements, creates one
 * unlinked raw backing descriptor, and exposes checked rectangle/read access.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "board_overlay.h"
#include "ra8_attributes.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Geometry and bounded tile limits for streamed presentation. */
typedef enum : size_t {
  k_emu_presentation_max_panel_px = 4096U, /**< Largest panel dimension.   */
  k_emu_presentation_tile_px      = 64U,   /**< Square rotation tile side. */
  k_emu_presentation_max_scratch_bytes =
    2U * k_emu_presentation_tile_px * k_emu_presentation_tile_px * sizeof(uint16_t),
  /**< Exact maximum caller scratch: source plus rotated 64x64 RGB565 tiles. */
} emu_presentation_limit_t;

/** @brief Presentation planning, creation, and transfer status. */
typedef enum : uint8_t {
  k_emu_presentation_ok = 0U,  /**< Operation completed exactly.                */
  k_emu_presentation_invalid,  /**< Geometry, rotation, pointer, or fd invalid. */
  k_emu_presentation_overflow, /**< A checked size/offset operation overflowed. */
  k_emu_presentation_capacity, /**< Caller scratch is smaller than required.    */
  k_emu_presentation_io,       /**< Raw descriptor create/size/I/O failed.      */
} emu_presentation_status_t;

/** @brief Inputs controlling exact presentation requirements. */
typedef struct {
  size_t   panel_width;  /**< Native firmware panel width.                     */
  size_t   panel_height; /**< Native firmware panel height.                    */
  uint32_t rotate_deg;   /**< Display rotation: 0, 90, 180, or 270 degrees.    */
  bool     active;       /**< A view, snapshot, record, or click needs pixels. */
} emu_presentation_spec_t;

/** @brief Exact result of a requirements or workspace operation. */
typedef struct {
  emu_presentation_status_t status;                 /**< Semantic result.        */
  size_t                    required_surface_bytes; /**< Exact raw-fd length.    */
  size_t                    required_scratch_bytes; /**< Exact caller scratch.   */
  size_t                    supplied_scratch_bytes; /**< Caller capacity.        */
  int                       os_error;               /**< Captured errno for I/O. */
} emu_presentation_result_t;

/** @brief One independent owned surface plus non-owning bounded scratch. */
typedef struct {
  int      fd;               /**< Owned unlinked descriptor, or -1.    */
  uint16_t panel_width;      /**< Native panel width.                  */
  uint16_t panel_height;     /**< Native panel height.                 */
  uint16_t display_width;    /**< Rotation-adjusted panel width.       */
  uint16_t display_height;   /**< Rotation-adjusted panel height.      */
  uint16_t composite_width;  /**< Display plus sidebar width.          */
  uint16_t composite_height; /**< Display/sidebar maximum height.      */
  uint32_t rotate_deg;       /**< Bound display rotation.              */
  uint8_t* scratch;          /**< Borrowed caller scratch, or nullptr. */
  size_t   scratch_bytes;    /**< Exact bound scratch prefix.          */
  size_t   surface_bytes;    /**< Exact raw-fd RGB565 length.          */
} emu_presentation_workspace_t;

/**
 * @brief Compute exact raw-fd and scratch requirements without mutation.
 * @param[in] spec Presentation geometry and mode.
 * @return Exact requirements or validation/overflow status.
 * @pre @p spec is non-null.
 * @post No caller memory, descriptor, filesystem name, or global changes.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] emu_presentation_result_t
emu_presentation_requirements(const emu_presentation_spec_t* spec);

/**
 * @brief Create and bind one unlinked raw-fd presentation workspace.
 * @details Validates exact capacity before creating an anonymous temporary;
 * failure preserves @p workspace and every scratch byte.
 * @param[in] spec Presentation geometry and mode.
 * @param[in,out] scratch Caller-owned bounded scratch, or nullptr if inactive.
 * @param[in] supplied_scratch_bytes Writable scratch capacity.
 * @param[out] workspace Receives one independent owned descriptor on success.
 * @return Exact requirements, supplied capacity, and captured I/O error.
 * @pre @p spec and @p workspace are non-null.
 * @pre Non-null @p scratch spans @p supplied_scratch_bytes writable bytes.
 * @pre Active @p scratch is aligned to @c alignof(uint16_t).
 * @post Success leaves no named temporary path and owns exactly one descriptor.
 * @post Failure leaves @p workspace and scratch unchanged with no descriptor leak.
 * @note Distinct workspaces are independently usable and closeable.
 * @since 0.1.0
 */
[[nodiscard]] emu_presentation_result_t
emu_presentation_open(const emu_presentation_spec_t* spec,
                      void*                          scratch,
                      size_t                         supplied_scratch_bytes,
                      emu_presentation_workspace_t*  workspace);

/**
 * @brief Close one owned presentation descriptor and invalidate the workspace.
 * @param[in,out] workspace Workspace returned by ::emu_presentation_open.
 * @return Whether close completed or the workspace was already inactive.
 * @post @p workspace is reset with fd -1 even when close reports failure.
 * @note Does not modify borrowed scratch bytes.
 * @since 0.1.0
  * @details Close one owned presentation descriptor and invalidate the workspace; this step is contained within the emu presentation model and uses bounded caller or module-owned storage.
 * @retval true The emu presentation close condition holds or completed successfully; false otherwise.
 * @pre Arguments satisfy the ranges documented for emu presentation close. @pre The call executes on the emulator's single owning thread.
 * @post Ownership of caller-supplied storage is unchanged.
 */
bool emu_presentation_close(emu_presentation_workspace_t* workspace);

/**
 * @brief Fill a checked RGB565 rectangle in a raw-fd surface.
 * @details Matches ::board_overlay_fill_fn and uses only bound row scratch.
 * @param[in,out] context An ::emu_presentation_workspace_t.
 * @param[in] x Rectangle column.
 * @param[in] y Rectangle row.
 * @param[in] width Rectangle width.
 * @param[in] height Rectangle height.
 * @param[in] color RGB565 fill color.
 * @return True when every row was written exactly.
 * @pre The rectangle is inside the composite geometry.
 * @post Failure is bounded to the owned raw surface; no external output changes.
 * @since 0.1.0
  * @retval true The emu presentation fill condition holds or completed successfully; false otherwise.
 * @pre The call executes on the emulator's single owning thread.
 * @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 */
bool emu_presentation_fill(void*    context,
                           uint16_t x,
                           uint16_t y,
                           uint16_t width,
                           uint16_t height,
                           uint16_t color);

/**
 * @brief Read exact RGB565 bytes at a checked surface offset.
 * @details Read exact rgb565 bytes at a checked surface offset; this step is contained within the emu presentation model and uses bounded caller or module-owned storage.
 * @param[in] workspace Caller-owned workspace used by the operation.
 * @param[in] offset Byte or register offset at which processing begins.
 * @param[in,out] bytes Byte storage transferred by the operation.
 * @param[in] count Number of elements or bytes to process.
 * @return The emu presentation read result produced by the emu presentation model.
 * @retval true The emu presentation read condition holds or completed successfully; false otherwise.
 * @pre Arguments satisfy the ranges documented for emu presentation read. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu presentation model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
bool emu_presentation_read(const emu_presentation_workspace_t* workspace,
                           size_t                              offset,
                           void*                               bytes,
                           size_t                              count);

/**
 * @brief Write exact RGB565 bytes at a checked surface offset.
 * @details Write exact rgb565 bytes at a checked surface offset; this step is contained within the emu presentation model and uses bounded caller or module-owned storage.
 * @param[in,out] workspace Caller-owned workspace used by the operation.
 * @param[in] offset Byte or register offset at which processing begins.
 * @param[in] bytes Byte storage transferred by the operation.
 * @param[in] count Number of elements or bytes to process.
 * @return The emu presentation write result produced by the emu presentation model.
 * @retval true The emu presentation write condition holds or completed successfully; false otherwise.
 * @pre Arguments satisfy the ranges documented for emu presentation write. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu presentation model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
bool emu_presentation_write(emu_presentation_workspace_t* workspace,
                            size_t                        offset,
                            const void*                   bytes,
                            size_t                        count);

/**
 * @brief Create an immutable unlinked descriptor snapshot of one surface.
 * @param[in,out] workspace Complete surface and bounded transfer scratch.
 * @param[out] snapshot_fd Receives an owned descriptor positioned arbitrarily.
 * @return True only when every source byte was copied exactly.
 * @post Success transfers close ownership of @p snapshot_fd to the caller.
 * @post Failure preserves @p snapshot_fd and leaks no descriptor or path.
 * @note The immutable copy prevents asynchronous display-provider data races.
 * @since 0.1.0
  * @details Create an immutable unlinked descriptor snapshot of one surface; this step is contained within the emu presentation model and uses bounded caller or module-owned storage.
 * @retval true The emu presentation snapshot condition holds or completed successfully; false otherwise.
 * @pre Arguments satisfy the ranges documented for emu presentation snapshot. @pre The call executes on the emulator's single owning thread.
 */
bool emu_presentation_snapshot(emu_presentation_workspace_t* workspace, int* snapshot_fd);

#ifdef __cplusplus
}
#endif
