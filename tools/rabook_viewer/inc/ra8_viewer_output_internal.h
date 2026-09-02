/**
 * @file ra8_viewer_output_internal.h
 * @brief Private typed diagnostic writers for the host viewer.
 * @details Declares the stream-injected, allocation-free message vocabulary
 * used by the viewer composition root and its focused output tests.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_io_stream.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Write the complete command-line usage message. */
RA8_PRIV [[nodiscard]] ra8_err_t priv_viewer_output_usage(ra8_io_stream_t* output,
                                                          const char*      executable);

/** @brief Write an exact workspace requirement diagnostic. */
RA8_PRIV [[nodiscard]] ra8_err_t priv_viewer_output_capacity(ra8_io_stream_t* output,
                                                             const char*      subject,
                                                             size_t           required,
                                                             size_t           supplied);

/** @brief Write a numbered operation failure and hexadecimal status. */
RA8_PRIV [[nodiscard]] ra8_err_t priv_viewer_output_index_error(ra8_io_stream_t* output,
                                                                const char*      operation,
                                                                uint32_t         index,
                                                                ra8_err_t        error);

/** @brief Write an unnumbered operation failure and hexadecimal status. */
RA8_PRIV [[nodiscard]] ra8_err_t
priv_viewer_output_error(ra8_io_stream_t* output, const char* operation, ra8_err_t error);

/** @brief Write a document-open failure with its path and status. */
RA8_PRIV [[nodiscard]] ra8_err_t
priv_viewer_output_open_error(ra8_io_stream_t* output, const char* path, ra8_err_t error);

/** @brief Write a successful document-open diagnostic. */
RA8_PRIV [[nodiscard]] ra8_err_t
priv_viewer_output_opened(ra8_io_stream_t* output, const char* path, uint32_t page_count);

/** @brief Write a successful path-publication diagnostic. */
RA8_PRIV [[nodiscard]] ra8_err_t priv_viewer_output_wrote(ra8_io_stream_t* output,
                                                          const char*      path);

/** @brief Write a successful tile-publication diagnostic. */
RA8_PRIV [[nodiscard]] ra8_err_t priv_viewer_output_tile(ra8_io_stream_t* output,
                                                         uint32_t         tile,
                                                         uint32_t         width,
                                                         uint32_t         height,
                                                         const char*      path);

/** @brief Write one fixed diagnostic without a terminating byte. */
RA8_PRIV [[nodiscard]] ra8_err_t priv_viewer_output_text(ra8_io_stream_t* output, const char* text);

#ifdef __cplusplus
}
#endif
