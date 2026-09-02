/**
 * @file alphabet_soup.c
 * @brief Word search puzzle solver implementation with NASA Power-of-10 safety guarantees.
 * @ingroup grp_alphabet_soup
 *
 * @par Tag
 * [Ring 4 / App] {World: Host}
 *
 * @details
 * Implements bounded grid parsing, space-stripped search key normalization, and
 * 8-way directional ray matching within caller-owned context memory. Every loop
 * enforces static iteration bounds, every parameter is bounds-checked, and stack
 * frames remain minimal (< 256 bytes).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "alphabet_soup.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_io_stream.h"

/** @brief Direction vector for 2D grid navigation. */
typedef struct {
  int32_t delta_row; /**< Row offset step.    */
  int32_t delta_col; /**< Column offset step. */
} soup_dir_t;

/** @brief Numeric constants for parsing and search navigation. */
typedef enum : uint32_t {
  k_decimal_base         = 10U,        /**< Base 10 decimal multiplier.      */
  k_decimal_overflow_cap = 429496729U, /**< (UINT32_MAX - 9) / 10 threshold. */
} soup_numeric_constants_t;

ra8_err_t soup_init(soup_context_t* ctx)
{
  if (ctx == nullptr) {
    return k_ra8_err_null_ptr;
  }
  (void)memset(ctx, 0, sizeof(soup_context_t));
  return k_ra8_ok;
}

/**
 * @brief Match word characters along a single directional ray.
 *
 * @details
 * Validates the ray endpoint bounds against the grid boundaries, then
 * checks each cell along the ray direction up to the specified span length.
 *
 * @param[in] grid Board state.
 * @param[in] key Target characters.
 * @param[in] span Ray length minus 1.
 * @param[in] r Start row.
 * @param[in] c Start column.
 * @param[in] dir Direction vector.
 *
 * @return bool True if all characters along ray match.
 * @retval true All characters match along the ray.
 * @retval false One or more characters mismatch or ray exits grid.
 *
 * @pre grid and key are non-null.
 * @pre span is non-negative and within grid bounds.
 * @post Grid memory is unmodified.
 * @post Scan terminates within span steps.
 * @note Helper function with strict bounded loop.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_match_ray(const soup_grid_t* grid,
                                            const char*        key,
                                            int32_t            span,
                                            int32_t            r,
                                            int32_t            c,
                                            soup_dir_t         dir)
{
  int32_t end_r = r + (dir.delta_row * span);
  int32_t end_c = c + (dir.delta_col * span);
  int32_t rows  = (int32_t)grid->row_count;
  int32_t cols  = (int32_t)grid->col_count;

  if ((end_r < 0) || (end_r >= rows) || (end_c < 0) || (end_c >= cols)) {
    return false;
  }

  for (int32_t k = 0; k <= span; ++k) {
    int32_t cur_r = r + (dir.delta_row * k);
    int32_t cur_c = c + (dir.delta_col * k);
    if (grid->cells[cur_r][cur_c] != key[k]) {
      return false;
    }
  }

  return true;
}

/**
 * @brief Check all directions starting at a specific cell.
 *
 * @details
 * Iterates through all 8 compass search directions starting from (r, c).
 * Upon finding a complete match, populates out_end_row and out_end_col.
 *
 * @param[in] grid Board state.
 * @param[in] key Target characters.
 * @param[in] span Ray length minus 1.
 * @param[in] r Start row.
 * @param[in] c Start column.
 * @param[out] out_end_row End row.
 * @param[out] out_end_col End col.
 *
 * @return bool True if word matched from this cell.
 * @retval true A matching ray was found in one of the 8 directions.
 * @retval false No matching ray found from this cell.
 *
 * @pre grid, key, out_end_row, out_end_col are non-null.
 * @pre (r, c) coordinates are within grid bounds.
 * @post Output coordinates are updated if match is found.
 * @post Grid state is preserved.
 * @note Helper function.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_check_cell(const soup_grid_t* grid,
                                             const char*        key,
                                             int32_t            span,
                                             int32_t            r,
                                             int32_t            c,
                                             uint32_t*          out_end_row,
                                             uint32_t*          out_end_col)
{
  /** @brief Direction delta lookup table for 8-way word search. */
  static const soup_dir_t s_search_directions[k_soup_direction_count] = {
    {.delta_row = 0, .delta_col = 1},   /**< East       */
    {.delta_row = 1, .delta_col = 1},   /**< South-East */
    {.delta_row = 1, .delta_col = 0},   /**< South      */
    {.delta_row = 1, .delta_col = -1},  /**< South-West */
    {.delta_row = 0, .delta_col = -1},  /**< West       */
    {.delta_row = -1, .delta_col = -1}, /**< North-West */
    {.delta_row = -1, .delta_col = 0},  /**< North      */
    {.delta_row = -1, .delta_col = 1},  /**< North-East */
  };

  for (uint32_t d = 0U; d < k_soup_direction_count; ++d) {
    soup_dir_t dir = s_search_directions[d];
    if (internal_match_ray(grid, key, span, r, c, dir)) {
      const int32_t end_row = r + (dir.delta_row * span);
      const int32_t end_col = c + (dir.delta_col * span);
      *out_end_row          = (uint32_t)end_row;
      *out_end_col          = (uint32_t)end_col;
      return true;
    }
  }
  return false;
}

bool soup_find_word(const soup_grid_t* grid,
                    const char*        search_key,
                    uint32_t           key_len,
                    uint32_t*          out_start_row,
                    uint32_t*          out_start_col,
                    uint32_t*          out_end_row,
                    uint32_t*          out_end_col)
{
  if ((grid == nullptr) || (search_key == nullptr) || (out_start_row == nullptr) ||
      (out_start_col == nullptr) || (out_end_row == nullptr) || (out_end_col == nullptr) ||
      (key_len == 0U) || (key_len > k_soup_max_word_chars) || (grid->row_count == 0U) ||
      (grid->row_count > k_soup_max_grid_rows) || (grid->col_count == 0U) ||
      (grid->col_count > k_soup_max_grid_cols)) {
    return false;
  }

  int32_t rows = (int32_t)grid->row_count;
  int32_t cols = (int32_t)grid->col_count;
  int32_t span = (int32_t)key_len - 1;
  char    lead = search_key[0];

  for (int32_t r = 0; r < rows; ++r) {
    for (int32_t c = 0; c < cols; ++c) {
      if (grid->cells[r][c] != lead) {
        continue;
      }
      if (internal_check_cell(grid, search_key, span, r, c, out_end_row, out_end_col)) {
        *out_start_row = (uint32_t)r;
        *out_start_col = (uint32_t)c;
        return true;
      }
    }
  }

  return false;
}

/**
 * @brief Parse a positive decimal integer with overflow detection.
 *
 * @details
 * Accumulates base-10 digits from the stream buffer until reaching a non-digit
 * or hitting the digit count limit, failing if arithmetic overflow occurs.
 *
 * @param[in,out] text_ptr Stream pointer.
 * @param[in] text_end End of text buffer.
 * @param[out] out_val Destination for parsed integer.
 *
 * @return ra8_err_t Error status.
 * @retval k_ra8_ok Integer parsed successfully.
 * @retval k_ra8_err_invalid_arg No digits found at cursor.
 * @retval k_ra8_err_range_check_failed Arithmetic overflow encountered.
 *
 * @pre text_ptr, text_end, out_val are non-null.
 * @pre *text_ptr is within [text_ptr, text_end).
 * @post *text_ptr advances past all parsed digits.
 * @post *out_val contains the parsed value on success.
 * @note NASA P10 Rule 2 bounded loop.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_parse_uint32(const char** text_ptr, const char* text_end, uint32_t* out_val)
{
  const char* ptr   = *text_ptr;
  uint32_t    val   = 0U;
  bool        found = false;

  for (uint32_t i = 0U; i < k_soup_max_dim_digits; ++i) {
    if ((ptr >= text_end) || (*ptr < '0') || (*ptr > '9')) {
      break;
    }
    uint32_t digit = (uint32_t)(*ptr - '0');
    if (val > k_decimal_overflow_cap) {
      return k_ra8_err_range_check_failed;
    }
    val   = (val * k_decimal_base) + digit;
    found = true;
    ptr++;
  }

  if (!found) {
    return k_ra8_err_invalid_arg;
  }

  *out_val  = val;
  *text_ptr = ptr;
  return k_ra8_ok;
}

/**
 * @brief Parse board header dimensions and validate against capacity limits.
 *
 * @details
 * Parses the "RxC" dimension prefix, validates row and column counts against
 * maximum grid dimensions, and advances the cursor to the first row of cells.
 *
 * @param[in,out] text_ptr In-out pointer to current parse position.
 * @param[in] text_end End of text buffer.
 * @param[out] out_rows Number of rows parsed.
 * @param[out] out_cols Number of columns parsed.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Dimensions successfully parsed and within bounds.
 * @retval k_ra8_err_invalid_arg Format was not "RxC" with decimal numbers.
 * @retval k_ra8_err_range_check_failed Row or column count exceeded limits.
 *
 * @pre text_ptr, text_end, out_rows, out_cols are non-null.
 * @pre *text_ptr points to the start of the grid header line.
 * @post *text_ptr points to the first byte after the header line.
 * @post Output dimensions are within [1, k_soup_max_grid_rows/cols].
 * @note Helper function.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_parse_dimensions(const char** text_ptr,
                                                        const char*  text_end,
                                                        uint32_t*    out_rows,
                                                        uint32_t*    out_cols)
{
  uint32_t  rows = 0U;
  uint32_t  cols = 0U;
  ra8_err_t err  = internal_parse_uint32(text_ptr, text_end, &rows);
  if (err != k_ra8_ok) {
    return err;
  }

  const char* ptr = *text_ptr;
  if ((ptr >= text_end) || ((*ptr != 'x') && (*ptr != 'X'))) {
    return k_ra8_err_invalid_arg;
  }
  ptr++;
  *text_ptr = ptr;

  err = internal_parse_uint32(text_ptr, text_end, &cols);
  if (err != k_ra8_ok) {
    return err;
  }

  ptr = *text_ptr;
  for (uint32_t step = 0U; step < k_soup_max_line_steps; ++step) {
    if ((ptr >= text_end) || ((*ptr != '\r') && (*ptr != '\n'))) {
      break;
    }
    ptr++;
  }

  if ((rows == 0U) || (rows > k_soup_max_grid_rows) || (cols == 0U) ||
      (cols > k_soup_max_grid_cols)) {
    return k_ra8_err_range_check_failed;
  }

  *out_rows = rows;
  *out_cols = cols;
  *text_ptr = ptr;
  return k_ra8_ok;
}

/**
 * @brief Parse a single grid row into grid cells with eager validation.
 *
 * @details
 * Reads space-separated non-whitespace characters for one row into the row
 * buffer, verifying that the column count exactly matches expected_cols.
 *
 * @param[in,out] text_ptr Pointer to parse position.
 * @param[in] text_end End of text buffer.
 * @param[out] row_cells Array of cells for current row.
 * @param[in] expected_cols Number of columns required.
 *
 * @return ra8_err_t Error status.
 * @retval k_ra8_ok Exactly expected_cols were parsed for this row.
 * @retval k_ra8_err_invalid_size Too few or too many columns found.
 *
 * @pre text_ptr, text_end, row_cells are non-null.
 * @pre expected_cols > 0 and <= k_soup_max_grid_cols.
 * @post *text_ptr points to the beginning of the next line.
 * @post row_cells contains expected_cols valid characters on success.
 * @note Helper function with NASA Rule 2 bounded loop and eager failure on column surplus.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_parse_single_row(const char** text_ptr,
                                                        const char*  text_end,
                                                        char*        row_cells,
                                                        uint32_t     expected_cols)
{
  const char* ptr = *text_ptr;
  uint32_t    c   = 0U;

  for (uint32_t step = 0U; step < k_soup_max_line_steps; ++step) {
    if ((ptr >= text_end) || (*ptr == '\0') || (*ptr == '\n') || (*ptr == '\r')) {
      break;
    }
    if ((*ptr != ' ') && (*ptr != '\t')) {
      if (c >= expected_cols) {
        return k_ra8_err_invalid_size;
      }
      row_cells[c] = *ptr;
      c++;
    }
    ptr++;
  }

  if (c != expected_cols) {
    return k_ra8_err_invalid_size;
  }

  for (uint32_t step = 0U; step < k_soup_max_line_steps; ++step) {
    if ((ptr >= text_end) || ((*ptr != '\r') && (*ptr != '\n'))) {
      break;
    }
    ptr++;
  }

  *text_ptr = ptr;
  return k_ra8_ok;
}

/**
 * @brief Parse complete grid character matrix from text stream.
 *
 * @details
 * Loops over each row index in the grid structure, calling internal_parse_single_row
 * to populate every cell in sequence.
 *
 * @param[in,out] text_ptr In-out pointer to current parse position.
 * @param[in] text_end End of text buffer.
 * @param[out] grid Grid structure to populate.
 *
 * @return ra8_err_t Status of parsing.
 * @retval k_ra8_ok Entire grid matrix parsed successfully.
 * @retval k_ra8_err_invalid_size A row had incorrect column count.
 *
 * @pre text_ptr, text_end, grid are non-null.
 * @pre grid->row_count and grid->col_count are pre-populated.
 * @post *text_ptr points to the character following the grid rows.
 * @post grid->cells is fully populated on success.
 * @note Helper function.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_parse_grid(const char** text_ptr, const char* text_end, soup_grid_t* grid)
{
  for (uint32_t r = 0U; r < grid->row_count; ++r) {
    ra8_err_t err = internal_parse_single_row(text_ptr, text_end, grid->cells[r], grid->col_count);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Emit single solved word result line to the stream.
 *
 * @details
 * Formats and writes the output string "<WORD> <START_ROW>:<START_COL> <END_ROW>:<END_COL>\n"
 * into the provided I/O stream.
 *
 * @param[in,out] out_stream Bound destination stream.
 * @param[in] word Original word text.
 * @param[in] start_row Start row.
 * @param[in] start_col Start col.
 * @param[in] end_row End row.
 * @param[in] end_col End col.
 *
 * @pre out_stream and word are non-null.
 * @pre Coordinates are valid non-negative integers.
 * @post Line is written to out_stream.
 * @post Stream state reflects appended bytes.
 * @note Helper function.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_emit_match(ra8_io_stream_t* out_stream,
                                             const char*      word,
                                             uint32_t         start_row,
                                             uint32_t         start_col,
                                             uint32_t         end_row,
                                             uint32_t         end_col)
{
  (void)ra8_io_stream_puts(out_stream, word);
  (void)ra8_io_stream_putc(out_stream, ' ');
  (void)ra8_io_stream_put_u32(out_stream, start_row);
  (void)ra8_io_stream_putc(out_stream, ':');
  (void)ra8_io_stream_put_u32(out_stream, start_col);
  (void)ra8_io_stream_putc(out_stream, ' ');
  (void)ra8_io_stream_put_u32(out_stream, end_row);
  (void)ra8_io_stream_putc(out_stream, ':');
  (void)ra8_io_stream_put_u32(out_stream, end_col);
  (void)ra8_io_stream_putc(out_stream, '\n');
}

/**
 * @brief Parse one target word line into word and search key buffers.
 *
 * @details
 * Reads the raw word text preserving spaces into the word buffer, and extracts
 * a space-stripped uppercase key into the search_key buffer.
 *
 * @param[in,out] text_ptr Current stream pointer.
 * @param[in] text_end End of text buffer.
 * @param[out] word Original word string.
 * @param[in] word_cap Word buffer capacity.
 * @param[out] search_key Normalized word string.
 * @param[in] key_cap Search key buffer capacity.
 * @param[out] out_search_len Output search key length.
 *
 * @pre text_ptr, text_end, word, search_key, out_search_len are non-null.
 * @pre word_cap and key_cap are greater than 0.
 * @post *text_ptr advances to the start of the next line.
 * @post *out_search_len contains the normalized key character count.
 * @note Helper function with NASA Rule 2 bounded loop.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_parse_word_entry(const char** text_ptr,
                                                   const char*  text_end,
                                                   char*        word,
                                                   size_t       word_cap,
                                                   char*        search_key,
                                                   size_t       key_cap,
                                                   uint32_t*    out_search_len)
{
  const char* ptr        = *text_ptr;
  uint32_t    word_len   = 0U;
  uint32_t    search_len = 0U;

  for (uint32_t step = 0U; step < k_soup_max_line_steps; ++step) {
    if ((ptr >= text_end) || (*ptr == '\0') || (*ptr == '\r') || (*ptr == '\n')) {
      break;
    }
    if ((word_len + 1U) < word_cap) {
      word[word_len] = *ptr;
      word_len++;
    }
    if ((*ptr != ' ') && (*ptr != '\t')) {
      if ((search_len + 1U) < key_cap) {
        search_key[search_len] = *ptr;
        search_len++;
      }
    }
    ptr++;
  }

  for (uint32_t step = 0U; step < k_soup_max_line_steps; ++step) {
    if ((ptr >= text_end) || ((*ptr != '\r') && (*ptr != '\n'))) {
      break;
    }
    ptr++;
  }

  *out_search_len = search_len;
  *text_ptr       = ptr;
}

/**
 * @brief Solve all words following the grid in the text buffer.
 *
 * @details
 * Iterates through each remaining line of target words, searching for matches
 * across the pre-parsed grid and streaming match results.
 *
 * @param[in,out] ctx Solver context.
 * @param[in] ptr Text stream cursor at word list.
 * @param[in] text_end End of text buffer.
 * @param[in,out] out_stream Output stream.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok All words solved and output flushed.
 * @retval other Stream flush error status.
 *
 * @pre ctx, ptr, text_end, out_stream are non-null.
 * @pre ctx->grid contains valid parsed puzzle grid.
 * @post Matching word coordinates are emitted to out_stream.
 * @post Out stream is flushed before return.
 * @note Helper function.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_solve_words(soup_context_t*  ctx,
                                                   const char*      ptr,
                                                   const char*      text_end,
                                                   ra8_io_stream_t* out_stream)
{
  for (uint32_t step = 0U; step < k_soup_max_parse_steps; ++step) {
    if ((ptr >= text_end) || (*ptr == '\0')) {
      break;
    }

    (void)memset(ctx->word_buffer, 0, sizeof(ctx->word_buffer));
    (void)memset(ctx->search_key_buffer, 0, sizeof(ctx->search_key_buffer));
    uint32_t search_len = 0U;

    internal_parse_word_entry(&ptr,
                              text_end,
                              ctx->word_buffer,
                              sizeof(ctx->word_buffer),
                              ctx->search_key_buffer,
                              sizeof(ctx->search_key_buffer),
                              &search_len);

    if (search_len == 0U) {
      continue;
    }

    uint32_t start_row = 0U;
    uint32_t start_col = 0U;
    uint32_t end_row   = 0U;
    uint32_t end_col   = 0U;
    if (soup_find_word(&ctx->grid,
                       ctx->search_key_buffer,
                       search_len,
                       &start_row,
                       &start_col,
                       &end_row,
                       &end_col)) {
      internal_emit_match(out_stream, ctx->word_buffer, start_row, start_col, end_row, end_col);
    }
  }

  return ra8_io_stream_flush(out_stream);
}

ra8_err_t
soup_solve(soup_context_t* ctx, const char* text, uint32_t text_len, ra8_io_stream_t* out_stream)
{
  if ((ctx == nullptr) || (text == nullptr) || (out_stream == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if ((text_len == 0U) || (text_len > k_soup_max_file_capacity)) {
    return k_ra8_err_invalid_size;
  }

  ra8_err_t err = soup_init(ctx);
  if (err != k_ra8_ok) {
    return err;
  }

  const char* ptr      = text;
  const char* text_end = &text[text_len];

  err = internal_parse_dimensions(&ptr, text_end, &ctx->grid.row_count, &ctx->grid.col_count);
  if (err != k_ra8_ok) {
    return err;
  }

  err = internal_parse_grid(&ptr, text_end, &ctx->grid);
  if (err != k_ra8_ok) {
    return err;
  }

  return internal_solve_words(ctx, ptr, text_end, out_stream);
}
