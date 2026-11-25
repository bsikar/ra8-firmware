/**
 * @file ra8_viewer_reader.h
 * @brief Caller-owned host JOF/comic reader and RGB565 render surface.
 *
 * @details The reader core is storage-, allocator-, and OS-agnostic below its
 * POSIX composition edge. A format-aware requirements query calculates the
 * exact bytes and alignment needed by one JOF, bare CBZ/CBR/CBT, EPUB, or
 * RABOOK document; bind partitions caller-owned storage; open attaches either an
 * injected ::ra8_fmt_source_t positioned-read callback or a host file path. No
 * core reader function owns dynamic memory or exposes a hosted stream type.
 * Comic decoding uses explicit bounded slices for its page index, name arena,
 * resident encoded page, and stb decode arena. Gzip/XZ-wrapped comics remain
 * unsupported because the shared wrapper API still requires the complete
 * unwrapped archive to be resident at once.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fmt_io.h"
#include "ra8_imgdec_name.h"

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

/** @brief Recognised container format passed to the portable reader core. */
typedef enum : uint32_t {
  k_ra8_viewer_format_unknown    = 0U, /**< Unrecognised format or extension.    */
  k_ra8_viewer_format_jof        = 1U, /**< Streamed JOF long-strip atlas.       */
  k_ra8_viewer_format_comic      = 2U, /**< Bare CBZ/CBR/CBT archive.            */
  k_ra8_viewer_format_comic_wrap = 3U, /**< Gzip/XZ-wrapped comic (unsupported). */
  k_ra8_viewer_format_epub       = 4U, /**< Streamed EPUB reflowable book.       */
  k_ra8_viewer_format_rabook     = 5U, /**< Compiled RBKC/RABOOK1 reflow book.   */
} ra8_viewer_format_t;

/** @brief Reader engine selected by the requirements query. */
typedef enum : uint32_t {
  k_ra8_viewer_engine_jof    = 1U, /**< Streamed JOF long-strip engine. */
  k_ra8_viewer_engine_comic  = 2U, /**< Bare CBZ/CBR/CBT comic engine.  */
  k_ra8_viewer_engine_reflow = 3U, /**< EPUB/RABOOK reflow engine.      */
} ra8_viewer_reader_engine_t;

/**
 * @struct ra8_viewer_reader_requirements_t
 * @brief Exact immutable layout returned for one document.
 * @details Callers may inspect the public totals but must pass the complete
 * object unchanged to ::ra8_viewer_reader_bind.
 */
typedef struct {
  size_t   required_bytes;      /**< Complete workspace extent.                */
  size_t   required_alignment;  /**< Required base-address alignment.          */
  size_t   framebuffer_bytes;   /**< Resident fixed RGB565 framebuffer.        */
  size_t   dimensions_bytes;    /**< Two resident per-tile dimension arrays.   */
  size_t   cell_bytes;          /**< One resident decoded JOF band.            */
  size_t   scratch_bytes;       /**< One bounded compressed-band staging area. */
  size_t   comic_pages_bytes;   /**< Comic page-index storage, or zero.        */
  size_t   comic_names_bytes;   /**< Comic member-name arena, or zero.         */
  size_t   comic_page_bytes;    /**< Resident encoded-page storage, or zero.   */
  size_t   comic_arena_bytes;   /**< stb decode arena storage, or zero.        */
  size_t   reflow_epub_bytes;   /**< EPUB archive reader storage, or zero.     */
  size_t   reflow_engine_bytes; /**< Reflow pagination engine state, or zero.  */
  size_t   reflow_pages_bytes;  /**< Book-to-chapter page map storage, or 0.   */
  size_t   reflow_xhtml_bytes;  /**< Per-chapter XHTML staging, or zero.       */
  size_t   reflow_file_bytes;   /**< Raw .rabook container buffer, or zero.    */
  size_t   reflow_book_bytes;   /**< Inflated .rabook blob buffer, or zero.    */
  uint32_t tile_count;          /**< Number of viewport tiles in the strip.    */
  uint32_t engine;              /**< ::ra8_viewer_reader_engine_t selection.   */
  uint32_t layout_version;      /**< ABI guard consumed by bind.               */
} ra8_viewer_reader_requirements_t;

/** @brief Required-versus-supplied evidence from a bind attempt. */
typedef struct {
  size_t required_bytes; /**< Exact bytes required by the requested layout. */
  size_t supplied_bytes; /**< Bytes supplied by the caller.                 */
} ra8_viewer_workspace_report_t;

/**
 * @brief Classify a filename or host path by extension without opening it.
 * @param[in] path NUL-terminated path or filename, or NULL.
 * @return Recognised ::ra8_viewer_format_t classification.
 * @retval k_ra8_viewer_format_unknown @p path was NULL or carried no known suffix.
 * @post No descriptor is opened and no state is mutated.
 * @note Pure apart from read-only inspection of @p path.
 * @since 0.1.0
 */
[[nodiscard]] ra8_viewer_format_t ra8_viewer_classify_path(const char* path);

/**
 * @brief Derive exact reader workspace requirements from a portable source.
 * @param[in] format Document container format to prepare for.
 * @param[in] source Caller-owned positioned-read source spanning > 0 bytes.
 * @param[out] out Exact requirements on success.
 * @return ::k_ra8_ok on success; ::k_ra8_err_not_supported for wrapped/unknown
 * formats, ::k_ra8_err_invalid_size for zero-length or over-cap inputs, or a
 * propagated read/parse error.
 * @pre @p source and @p out are non-NULL and `source->read_at` is non-NULL.
 * @post On failure @p out is zeroed.
 * @note Performs no heap allocation and opens no POSIX descriptor.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_viewer_reader_requirements_source(ra8_viewer_format_t               format,
                                      const ra8_fmt_source_t*           source,
                                      ra8_viewer_reader_requirements_t* out);

/**
 * @brief Inspect @p path and calculate its exact reader workspace.
 * @param[in] path NUL-terminated host path.
 * @param[out] out Exact requirements on success.
 * @return ::k_ra8_ok on success; an error for I/O, unsupported format, or an
 * invalid document geometry.
 * @retval k_ra8_ok Requirements are complete.
 * @retval k_ra8_err_null_ptr A required pointer was NULL.
 * @retval k_ra8_err_not_found The input is absent, empty, or not regular.
 * @retval k_ra8_err_not_supported The format is wrapped or unknown.
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
 * @brief Open the document over an injected positioned-read source.
 * @param[in,out] reader Bound, closed reader.
 * @param[in] format Document container format matching the bound workspace.
 * @param[in] source Caller-owned positioned source remaining live until close.
 * @return ::k_ra8_ok on success or a propagated parse/open error.
 * @retval k_ra8_ok The selected reader engine is open and ready to render.
 * @retval k_ra8_err_null_ptr A required pointer or callback was NULL.
 * @retval k_ra8_err_invalid_state @p reader is unbound/open or engine mismatches.
 * @retval k_ra8_err_invalid_size Source length is zero or fails bounds.
 * @post Failure closes any partially initialized engine state.
 * @note Caller retains @p source->ctx ownership; zero POSIX descriptors acquired.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_viewer_open_source(ra8_viewer_reader_t*    reader,
                                               ra8_viewer_format_t     format,
                                               const ra8_fmt_source_t* source);

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

/**
 * @brief Name the image container page @p page actually holds.
 *
 * @details The comic engine receives page bytes it did not produce, and until
 * this call the only thing it could say about a page it failed to decode was
 * the status code: a GIF the bound decoder cannot open and a truncated archive
 * member both surfaced as ::k_ra8_err_not_supported. This reads the page's
 * leading bytes and answers from ::ra8_imgdec_identify, the one naming table
 * this tree publishes, so a caller can report *what* it was handed.
 *
 * Naming is not a decode claim. A page this call names may still be one the
 * bound backend refuses; the two questions are deliberately separate.
 *
 * @param[in,out] reader Open reader.
 * @param[in] page Page index below ::ra8_viewer_page_count.
 * @param[out] out Receives the container and its borrowed canonical names.
 * @return Naming status.
 * @retval k_ra8_ok @p out holds a complete naming record.
 * @retval k_ra8_err_null_ptr @p reader or @p out is NULL.
 * @retval k_ra8_err_invalid_state The reader is closed.
 * @retval k_ra8_err_out_of_range @p page is at or above the page count.
 * @retval k_ra8_err_not_supported The bound engine holds no foreign container.
 * @retval k_ra8_err_not_found The page carries no recognised signature.
 * @pre @p reader came from a successful ::ra8_viewer_open.
 * @post On any non-ok return `*out` is zeroed.
 * @post The resident encoded-page slice may be overwritten with @p page.
 * @note Not thread-safe; it reuses the reader's one resident page slice.
 * @see ra8_imgdec_identify()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_viewer_page_container(ra8_viewer_reader_t* reader, uint32_t page, ra8_imgdec_name_t* out);

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
ra8_viewer_write_ppm565(const uint16_t* px, uint32_t w, uint32_t h, const char* path);

/** @brief Pack 8-bit RGB channels into one RGB565 word. */
[[nodiscard]] uint16_t ra8_viewer_pack565(uint8_t rr, uint8_t gg, uint8_t bb);

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
