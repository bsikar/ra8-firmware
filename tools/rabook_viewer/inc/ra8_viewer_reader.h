/**
 * @file ra8_viewer_reader.h
 * @brief Caller-owned host JOF/comic reader and RGB565 render surface.
 *
 * @details The reader is deliberately storage- and allocator-agnostic below its
 * POSIX composition edge. A format-aware requirements query reports the exact
 * bytes and alignment needed by one JOF or bare CBZ/CBR/CBT document; bind
 * partitions caller-owned storage; open attaches a raw descriptor behind a
 * positional-read callback. No reusable reader function owns dynamic memory or
 * exposes a hosted stream type. Comic decoding uses explicit bounded slices for
 * its page index, name arena, resident encoded page, and stb decode arena.
 * Gzip/XZ-wrapped comics remain unsupported because the shared wrapper API still
 * requires the complete unwrapped archive to be resident at once.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Fixed framebuffer geometry used by headless page rendering. */
typedef enum : uint16_t {
  k_ra8_viewer_fb_width  = 720U,  /**< Framebuffer width in pixels.  */
  k_ra8_viewer_fb_height = 1080U, /**< Framebuffer height in pixels. */
} ra8_viewer_geom_t;

/** @brief Opaque state bound inside caller-owned workspace. */
typedef struct ra8_viewer_reader ra8_viewer_reader_t;

/** @brief Reader engine selected by the requirements query. */
typedef enum : uint32_t {
  k_ra8_viewer_engine_jof   = 1U, /**< Streamed JOF long-strip engine. */
  k_ra8_viewer_engine_comic = 2U, /**< Bare CBZ/CBR/CBT comic engine.  */
} ra8_viewer_reader_engine_t;

/**
 * @struct ra8_viewer_reader_requirements_t
 * @brief Exact immutable layout returned for one document.
 * @details Callers may inspect the public totals but must pass the complete
 * object unchanged to ::ra8_viewer_reader_bind.
 */
typedef struct {
  size_t   required_bytes;     /**< Complete workspace extent.                */
  size_t   required_alignment; /**< Required base-address alignment.          */
  size_t   framebuffer_bytes;  /**< Resident fixed RGB565 framebuffer.        */
  size_t   dimensions_bytes;   /**< Two resident per-tile dimension arrays.   */
  size_t   cell_bytes;         /**< One resident decoded JOF band.            */
  size_t   scratch_bytes;      /**< One bounded compressed-band staging area. */
  size_t   comic_pages_bytes;  /**< Comic page-index storage, or zero.        */
  size_t   comic_names_bytes;  /**< Comic member-name arena, or zero.         */
  size_t   comic_page_bytes;   /**< Resident encoded-page storage, or zero.   */
  size_t   comic_arena_bytes;  /**< stb decode arena storage, or zero.        */
  uint32_t tile_count;         /**< Number of viewport tiles in the strip.    */
  uint32_t engine;             /**< ::ra8_viewer_reader_engine_t selection.   */
  uint32_t layout_version;     /**< ABI guard consumed by bind.               */
} ra8_viewer_reader_requirements_t;

/** @brief Required-versus-supplied evidence from a bind attempt. */
typedef struct {
  size_t required_bytes; /**< Exact bytes required by the requested layout. */
  size_t supplied_bytes; /**< Bytes supplied by the caller.                 */
} ra8_viewer_workspace_report_t;

/**
 * @brief Inspect @p path and calculate its exact reader workspace.
 * @param[in] path NUL-terminated host path.
 * @param[out] out Exact requirements on success.
 * @return ::k_ra8_ok on success; an error for I/O, unsupported format, or an
 * invalid document geometry.
 * @retval k_ra8_ok Requirements are complete.
 * @retval k_ra8_err_null_ptr A required pointer was NULL.
 * @retval k_ra8_err_not_found The input is absent, empty, or not regular.
 * @retval k_ra8_err_not_supported The format is wrapped, reflow, or unknown.
 * @post On failure @p out is zeroed.
 * @note The sizing descriptor opened internally is closed before return.
 */
[[nodiscard]] ra8_err_t ra8_viewer_reader_requirements(const char*                       path,
                                                       ra8_viewer_reader_requirements_t* out);

/**
 * @brief Bind one reader state to caller-owned bytes.
 * @param[out] out Receives the bound reader.
 * @param[in,out] workspace Aligned caller-owned backing.
 * @param[in] workspace_bytes Accessible backing extent.
 * @param[in] requirements Unmodified successful requirements result.
 * @param[out] report Exact required/supplied sizes, including on failure.
 * @return ::k_ra8_ok on success.
 * @retval k_ra8_ok The workspace was partitioned and @p out published.
 * @retval k_ra8_err_null_ptr A required pointer was NULL.
 * @retval k_ra8_err_invalid_size The layout, alignment, or capacity is invalid.
 * @post Failure leaves @p out NULL and does not mutate workspace bytes.
 * @post Success publishes a closed reader borrowing @p workspace.
 */
[[nodiscard]] ra8_err_t ra8_viewer_reader_bind(ra8_viewer_reader_t** out,
                                               void*                 workspace,
                                               size_t                workspace_bytes,
                                               const ra8_viewer_reader_requirements_t* requirements,
                                               ra8_viewer_workspace_report_t*          report);

/**
 * @brief Open the document class used to size and bind @p reader.
 * @param[in,out] reader Bound, closed reader.
 * @param[in] path NUL-terminated JOF or bare comic path.
 * @return ::k_ra8_ok on success or a propagated parse/open error.
 * @retval k_ra8_ok The descriptor and selected reader engine are open.
 * @retval k_ra8_err_null_ptr A required pointer was NULL.
 * @retval k_ra8_err_invalid_state The reader is unbound or already open.
 * @retval k_ra8_err_not_supported @p path does not match the bound engine.
 * @post Failure leaves @p reader closed.
 * @note The reader owns only its raw descriptor; all memory remains borrowed.
 */
[[nodiscard]] ra8_err_t ra8_viewer_open(ra8_viewer_reader_t* reader, const char* path);

/** @brief Number of viewport pages in @p reader, or zero for NULL/closed. */
[[nodiscard]] uint32_t ra8_viewer_page_count(const ra8_viewer_reader_t* reader);

/**
 * @brief Render one page into the reader's fixed RGB565 framebuffer.
 * @param[in,out] reader Open reader.
 * @param[in] page Page index.
 * @return Render status.
 */
[[nodiscard]] ra8_err_t ra8_viewer_render_page(ra8_viewer_reader_t* reader, uint32_t page);

/** @brief Number of scroll tiles in @p reader, or zero for NULL/closed. */
[[nodiscard]] uint32_t ra8_viewer_tile_count(const ra8_viewer_reader_t* reader);

/** @brief Read native dimensions for tile @p index. */
[[nodiscard]] ra8_err_t ra8_viewer_tile_size(const ra8_viewer_reader_t* reader,
                                             uint32_t                   index,
                                             uint32_t*                  width,
                                             uint32_t*                  height);

/**
 * @brief Report exact caller storage needed to render tile @p index.
 * @param[in] reader Open reader.
 * @param[in] index Tile index.
 * @param[out] out_bytes Required RGB565 bytes.
 * @param[out] out_alignment Required base alignment.
 * @return ::k_ra8_ok on success.
 */
[[nodiscard]] ra8_err_t ra8_viewer_tile_requirements(const ra8_viewer_reader_t* reader,
                                                     uint32_t                   index,
                                                     size_t*                    out_bytes,
                                                     size_t*                    out_alignment);

/**
 * @brief Render tile @p index into caller-owned RGB565 storage.
 * @param[in,out] reader Open reader.
 * @param[in] index Tile index.
 * @param[in,out] workspace Caller output backing.
 * @param[in] workspace_bytes Accessible backing extent.
 * @param[out] width Rendered width.
 * @param[out] height Rendered height.
 * @param[out] out_pixels Aliases @p workspace on success.
 * @param[out] report Exact required/supplied sizes.
 * @return Render status.
 * @post Failure leaves @p out_pixels NULL and restores the fixed target.
 */
[[nodiscard]] ra8_err_t ra8_viewer_render_tile565(ra8_viewer_reader_t*           reader,
                                                  uint32_t                       index,
                                                  void*                          workspace,
                                                  size_t                         workspace_bytes,
                                                  uint32_t*                      width,
                                                  uint32_t*                      height,
                                                  uint16_t**                     out_pixels,
                                                  ra8_viewer_workspace_report_t* report);

/** @brief Write the current fixed framebuffer as binary PPM. */
[[nodiscard]] ra8_err_t ra8_viewer_dump_ppm(const ra8_viewer_reader_t* reader, const char* path);

/** @brief Write arbitrary caller-owned RGB565 pixels as binary PPM. */
[[nodiscard]] ra8_err_t
ra8_viewer_write_ppm565(const uint16_t* pixels, uint32_t width, uint32_t height, const char* path);

/** @brief Pack 8-bit RGB channels into one RGB565 word. */
[[nodiscard]] uint16_t ra8_viewer_pack565(uint8_t red, uint8_t green, uint8_t blue);

/** @brief Reassemble one little-endian RGB565 word. */
[[nodiscard]] uint16_t ra8_viewer_pack565_le_pair(uint8_t low, uint8_t high);

/**
 * @brief Close the descriptor and reset reader state without freeing workspace.
 * @details Releases only the owned raw descriptor and restores the fixed target.
 * @param[in,out] reader Bound reader, or NULL.
 * @pre @p reader is NULL or came from a successful bind.
 * @pre No render operation is in progress.
 * @post Borrowed workspace remains caller-owned and reusable.
 * @post A non-NULL reader is closed and reports zero pages.
 * @note Safe on NULL; not thread-safe for a shared reader.
 * @since 0.1.0
 */
void ra8_viewer_close(ra8_viewer_reader_t* reader);

#ifdef __cplusplus
}
#endif
