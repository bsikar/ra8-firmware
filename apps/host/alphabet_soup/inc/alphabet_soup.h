/**
 * @file alphabet_soup.h
 * @brief Word search puzzle solver API and bounded workspace definitions.
 * @ingroup grp_alphabet_soup
 *
 * @par Tag
 * [Ring 4 / App] {World: Host}
 *
 * @details
 * Declares data structures and solver routines for parsing word search character grids
 * and locating words along 8-way directional rays (horizontal, vertical, diagonal,
 * forward, and backward) with strict bounds checking and zero dynamic allocation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"
#include "ra8_io_stream.h"

/** @brief Fixed sizing and bounds for Alphabet Soup word search solver. */
typedef enum : uint32_t {
  k_soup_max_grid_rows     = 128U,   /**< Maximum board row dimension.           */
  k_soup_max_grid_cols     = 128U,   /**< Maximum board column dimension.        */
  k_soup_max_word_chars    = 128U,   /**< Maximum characters per target word.    */
  k_soup_max_file_capacity = 65536U, /**< Maximum supported puzzle file size.    */
  k_soup_max_parse_steps   = 65536U, /**< Upper bound on file parse iterations.  */
  k_soup_max_line_steps    = 1024U,  /**< Upper bound on line parse iterations.  */
  k_soup_max_dim_digits    = 8U,     /**< Maximum digits in dimension specifier. */
  k_soup_direction_count   = 8U,     /**< 8-way directional navigation rays.     */
} soup_limits_t;

/** @brief In-memory word search board matrix. */
typedef struct {
  uint32_t row_count;                                         /**< Active row dimension.    */
  uint32_t col_count;                                         /**< Active column dimension. */
  char     cells[k_soup_max_grid_rows][k_soup_max_grid_cols]; /**< Character grid storage.  */
} soup_grid_t;

/**
 * @struct soup_context_t
 * @brief Caller-owned bounded workspace for puzzle solving (zero-heap).
 */
typedef struct {
  soup_grid_t grid;                                     /**< Board grid storage.    */
  char        word_buffer[k_soup_max_word_chars];       /**< Current word buffer.   */
  char        search_key_buffer[k_soup_max_word_chars]; /**< Normalized key buffer. */
} soup_context_t;

/**
 * @brief Initialize a caller-owned puzzle solver context.
 *
 * @param[out] ctx Solver context to zero-initialize.
 *
 * @return ra8_err_t Result status.
 * @retval k_ra8_ok Context initialized.
 * @retval k_ra8_err_null_ptr @p ctx was null.
 *
 * @pre ctx is non-null.
 * @post ctx is zero-initialized and ready for use.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t soup_init(soup_context_t* ctx);

/**
 * @brief Search a grid for a normalized word along all 8 compass directions.
 *
 * @param[in] grid Pointer to populated board.
 * @param[in] search_key Normalized uppercase/stripped word string.
 * @param[in] key_len Length of normalized word.
 * @param[out] out_start_row Start row coordinate.
 * @param[out] out_start_col Start column coordinate.
 * @param[out] out_end_row End row coordinate.
 * @param[out] out_end_col End column coordinate.
 *
 * @return bool True if word was found, false otherwise.
 *
 * @pre grid, search_key, out_start_row, out_start_col, out_end_row, out_end_col are non-null.
 * @pre key_len > 0.
 *
 * @post Coordinates populated when true is returned.
 *
 * @note First-character candidate filtering provides optimal zero-allocation embedded
 * search efficiency. For dynamic systems with massive dictionaries (10,000+ words),
 * a multi-string Trie / Aho-Corasick automaton would be asymptotically preferred.
 * @since 0.1.0
 */
[[nodiscard]] bool soup_find_word(const soup_grid_t* grid,
                                  const char*        search_key,
                                  uint32_t           key_len,
                                  uint32_t*          out_start_row,
                                  uint32_t*          out_start_col,
                                  uint32_t*          out_end_row,
                                  uint32_t*          out_end_col);

/**
 * @brief Parse board and words from puzzle text and emit solutions to the output stream.
 *
 * @param[in,out] ctx Caller-owned solver context.
 * @param[in] text Null-terminated input file content.
 * @param[in] text_len Byte length of @p text.
 * @param[in,out] out_stream Bound destination stream for answer key emission.
 *
 * @return ra8_err_t Status of parsing and emission.
 * @retval k_ra8_ok Puzzle parsed and output emitted.
 * @retval k_ra8_err_null_ptr A required pointer was null.
 * @retval k_ra8_err_invalid_size Text exceeded bounds or invalid dimensions.
 * @retval k_ra8_err_range_check_failed Dimensions or coordinates out of range.
 *
 * @pre ctx, text, out_stream are non-null.
 * @pre text_len <= k_soup_max_file_capacity.
 * @pre out_stream is initialized and writable.
 *
 * @post Answer key lines are written and flushed to out_stream.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
soup_solve(soup_context_t* ctx, const char* text, uint32_t text_len, ra8_io_stream_t* out_stream);
