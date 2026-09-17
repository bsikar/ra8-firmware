/**
 * @file test_alphabet_soup.c
 * @brief Unit tests for alphabet-soup word search solver and POSIX port operations.
 *
 * @par Tag
 * [Ring 4 / Test] {World: Host}
 *
 * @details
 * Tests the architecture-neutral filesystem interface (fw_if_fs), byte-stream
 * facade (ra8_io_stream), POSIX adapters (fw_if_fs_posix and ra8_io_stream_posix),
 * and the Alphabet Soup word search solver on horizontal, vertical, diagonal,
 * forward, reverse, and space-containing words, along with invalid inputs and edge cases.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "alphabet_soup.h"
#include "alphabet_soup_cli_internal.h"
#include "fw_if_fs.h"
#include "fw_if_fs_posix.h"
#include "fw_if_fs_types.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_io_stream.h"
#include "ra8_io_stream_posix.h"
#include "ra8_io_stream_ram.h"
#include "ra8_test_output.h"
#include "unity_minimal.h"

/** @brief Numeric bounds and limits for alphabet-soup test vectors. */
typedef enum : uint32_t {
  k_alphabet_count     = 26U,  /**< Total letters in the English alphabet.   */
  k_file_work_capacity = 64U,  /**< File handle workspace size.              */
  k_path_buf_capacity  = 64U,  /**< Local path formatting buffer capacity.   */
  k_payload_capacity   = 32U,  /**< Payload buffer capacity per letter file. */
  k_read_buf_capacity  = 64U,  /**< Read buffer capacity.                    */
  k_max_list_entries   = 64U,  /**< Maximum directory entries to list.       */
  k_seek_offset_middle = 13U,  /**< Seek offset for partial read tests.      */
  k_capture_buf_cap    = 512U, /**< RAM stream test capture buffer capacity. */
} alphabet_test_limits_t;

/** @brief Context for directory enumeration callback. */
typedef struct {
  uint32_t entry_count; /**< Count of enumerated entries. */
} list_context_t;

/** @brief Static test solver context in .bss. */
static soup_context_t s_test_ctx;

/**
 * @brief Directory listing callback that increments entry count.
 *
 * @details
 * Invoked by fw_fs_listdir for each directory entry found, updating the
 * accumulated count in the passed list_context_t structure.
 *
 * @param[in,out] ctx Pointer to caller-allocated list_context_t.
 * @param[in] entry Directory entry metadata.
 * @param[out] out_continue Set to true to continue enumeration.
 *
 * @return ra8_err_t Status code.
 * @retval k_ra8_ok Entry processed successfully.
 * @retval k_ra8_err_null_ptr Required pointer was null.
 *
 * @pre ctx is non-null and points to list_context_t.
 * @pre entry is non-null and valid.
 * @pre out_continue is non-null.
 * @post ctx->entry_count is incremented.
 * @post *out_continue is set to true.
 * @note Reentrant across distinct contexts.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_list_callback(void* ctx, const fw_fs_dirent_t* entry, bool* out_continue)
{
  if ((ctx == nullptr) || (entry == nullptr) || (out_continue == nullptr)) {
    return k_ra8_err_null_ptr;
  }

  list_context_t* list_ctx = (list_context_t*)ctx;
  list_ctx->entry_count++;
  *out_continue = true;
  return k_ra8_ok;
}

/**
 * @brief Test POSIX stream binding and writing.
 *
 * @details
 * Validates error checking on invalid arguments and verifies successful
 * write/flush operations through the POSIX stream facade adapter.
 *
 * @pre Standard output is open and writable.
 * @pre Process memory is available for stream initialization.
 * @post Stream write and flush operations succeed.
 * @post Memory state is preserved.
 * @note Test case function.
 * @since 0.1.0
 */
RA8_INTERNAL static void test_posix_stream(void)
{
  ra8_io_stream_t             stream = {};
  ra8_io_stream_posix_state_t state  = {};

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_io_stream_posix_init(&stream, &state, -1));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_stream_posix_init(nullptr, &state, STDOUT_FILENO));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_stream_posix_init(&stream, nullptr, STDOUT_FILENO));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_posix_init(&stream, &state, STDOUT_FILENO));

  const char msg[]   = "[test_alphabet_soup] POSIX stream binding verified\n";
  uint32_t   written = 0U;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_io_stream_write(&stream, (const uint8_t*)msg, (uint32_t)strlen(msg), &written));
  TEST_ASSERT_EQ((uint32_t)strlen(msg), written);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_stream_flush(&stream));
}

/**
 * @brief Pin null and invalid-handle behavior of the CLI file helpers.
 * @pre Local output buffers are writable.
 * @post Every required-pointer guard and the read-error propagation arm ran.
 * @note Test-only and synchronous.
 * @since 0.1.0
 */
RA8_INTERNAL static void test_cli_file_helper_guards(void)
{
  char root[8] = {};
  char leaf[8] = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 priv_alphabet_soup_split_path(nullptr, root, sizeof(root), leaf, sizeof(leaf)));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 priv_alphabet_soup_split_path("a", nullptr, sizeof(root), leaf, sizeof(leaf)));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 priv_alphabet_soup_split_path("a", root, sizeof(root), nullptr, sizeof(leaf)));

  fw_fs_file_t file = {};
  uint8_t      byte = 0U;
  uint32_t     size = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, priv_alphabet_soup_read_all(nullptr, &byte, 1U, &size));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, priv_alphabet_soup_read_all(&file, nullptr, 1U, &size));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, priv_alphabet_soup_read_all(&file, &byte, 1U, nullptr));
  TEST_ASSERT(priv_alphabet_soup_read_all(&file, &byte, 1U, &size) != k_ra8_ok);
}

/**
 * @brief Write and verify one alphabet letter file.
 *
 * @details
 * Opens a letter file for truncation, writes formatted letter text, closes,
 * re-opens for reading, and asserts that the content read matches the payload.
 *
 * @param[in,out] fs Filesystem handle.
 * @param[in] letter Letter character.
 *
 * @pre fs is an initialized filesystem instance.
 * @pre letter is an ASCII character.
 * @post The letter file is written, verified, and closed.
 * @post No file handles leak.
 * @note Helper function.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_write_and_check_letter(fw_fs_t* fs, char letter)
{
  char path[k_path_buf_capacity] = {};
  (void)snprintf(path, sizeof(path), "/soup/letter_%c.txt", letter);

  char payload[k_payload_capacity] = {};
  (void)snprintf(payload, sizeof(payload), "Letter: %c\n", letter);
  const uint32_t payload_len = (uint32_t)strlen(payload);

  alignas(max_align_t) uint8_t file_work[k_file_work_capacity] = {};
  fw_fs_file_t                 file                            = {};

  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_fs_open(&fs->streams,
                            path,
                            k_fw_fs_open_write_truncate,
                            &file,
                            file_work,
                            sizeof(file_work)));

  uint32_t written = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_write(&file, (const uint8_t*)payload, payload_len, &written));
  TEST_ASSERT_EQ(payload_len, written);
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_close(&file));

  alignas(max_align_t) uint8_t read_work[k_file_work_capacity] = {};
  fw_fs_file_t                 read_file                       = {};
  TEST_ASSERT_EQ(
    k_ra8_ok,
    fw_fs_open(&fs->streams, path, k_fw_fs_open_read, &read_file, read_work, sizeof(read_work)));

  uint8_t  read_buf[k_read_buf_capacity] = {};
  uint32_t bytes_read                    = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_read(&read_file, read_buf, sizeof(read_buf), &bytes_read));
  TEST_ASSERT_EQ(payload_len, bytes_read);
  for (uint32_t index = 0U; index < payload_len; ++index) {
    TEST_ASSERT_EQ((uint8_t)payload[index], read_buf[index]);
  }
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_close(&read_file));
}

/**
 * @brief Test alphabet file creation, writing, reading, and deletion lifecycle.
 *
 * @details
 * Creates 26 files inside a temporary directory, asserts all are enumerated
 * via listdir, and cleanly unlinks and removes the directory tree.
 *
 * @pre Temp directory creation succeeds.
 * @pre Filesystem adapter initializes cleanly.
 * @post All 26 files are created, verified, and unlinked.
 * @post The temp directory is cleaned up and deleted.
 * @note Test case function.
 * @since 0.1.0
 */
RA8_INTERNAL static void test_alphabet_files_roundtrip(void)
{
  static const char root_template_source[] = "/tmp/ra8_alphabet_soup_XXXXXX";
  char              root_template[sizeof(root_template_source)];
  (void)memcpy(root_template, root_template_source, sizeof(root_template));
  char* temp_root = mkdtemp(root_template);
  TEST_ASSERT_NOT_NULL(temp_root);

  fw_fs_t                 fs          = {};
  fw_fs_posix_state_t     posix_state = {.root_fd = -1};
  const fw_fs_posix_cfg_t cfg         = {
    .root_path       = root_template,
    .removable_media = false,
  };

  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_posix_init(&fs, &posix_state, &cfg));

  const char* soup_dir = "/soup";
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_mkdir(&fs.names, soup_dir));

  for (uint32_t i = 0U; i < k_alphabet_count; ++i) {
    internal_write_and_check_letter(&fs, (char)('A' + i));
  }

  list_context_t list_ctx     = {.entry_count = 0U};
  uint32_t       out_count    = 0U;
  bool           out_complete = false;
  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_fs_listdir(&fs.names,
                               soup_dir,
                               k_max_list_entries,
                               internal_list_callback,
                               &list_ctx,
                               &out_count,
                               &out_complete));
  TEST_ASSERT_EQ(k_alphabet_count, out_count);
  TEST_ASSERT_EQ(k_alphabet_count, list_ctx.entry_count);
  TEST_ASSERT(out_complete);

  for (uint32_t i = 0U; i < k_alphabet_count; ++i) {
    char letter                    = (char)('A' + i);
    char path[k_path_buf_capacity] = {};
    (void)snprintf(path, sizeof(path), "/soup/letter_%c.txt", letter);
    TEST_ASSERT_EQ(k_ra8_ok, fw_fs_unlink(&fs.names, path));
  }

  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_rmdir(&fs.names, soup_dir));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_posix_deinit(&posix_state));
  TEST_ASSERT_EQ(0, rmdir(root_template));
}

/**
 * @brief Test file seek and position querying.
 *
 * @details
 * Writes a full alphabet file, reads file size, seeks to the midpoint,
 * queries offset with tell, and asserts the remaining data read matches.
 *
 * @param[in,out] fs Filesystem handle.
 *
 * @pre fs is an initialized filesystem instance.
 * @pre Write permissions are available in fs root.
 * @post File seeking and reading match expected offsets.
 * @post Test file is closed and unlinked.
 * @note Helper function.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_check_file_seeking(fw_fs_t* fs)
{
  const char*    alphabet_file   = "/alphabet.txt";
  const char     alphabet_data[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  const uint32_t data_len        = (uint32_t)strlen(alphabet_data);

  alignas(max_align_t) uint8_t file_work[k_file_work_capacity] = {};
  fw_fs_file_t                 file                            = {};

  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_fs_open(&fs->streams,
                            alphabet_file,
                            k_fw_fs_open_write_truncate,
                            &file,
                            file_work,
                            sizeof(file_work)));

  uint32_t written = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_write(&file, (const uint8_t*)alphabet_data, data_len, &written));
  TEST_ASSERT_EQ(data_len, written);
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_close(&file));

  alignas(max_align_t) uint8_t read_work[k_file_work_capacity] = {};
  fw_fs_file_t                 read_file                       = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_fs_open(&fs->streams,
                            alphabet_file,
                            k_fw_fs_open_read,
                            &read_file,
                            read_work,
                            sizeof(read_work)));

  uint64_t file_size = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_file_size(&read_file, &file_size));
  TEST_ASSERT_EQ((uint64_t)data_len, file_size);

  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_seek(&read_file, (uint64_t)k_seek_offset_middle));

  uint64_t current_offset = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_tell(&read_file, &current_offset));
  TEST_ASSERT_EQ((uint64_t)k_seek_offset_middle, current_offset);

  uint8_t  read_buf[k_read_buf_capacity] = {};
  uint32_t bytes_read                    = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_read(&read_file, read_buf, sizeof(read_buf), &bytes_read));
  TEST_ASSERT_EQ(data_len - k_seek_offset_middle, bytes_read);
  for (uint32_t index = 0U; index < bytes_read; ++index) {
    TEST_ASSERT_EQ((uint8_t)alphabet_data[k_seek_offset_middle + index], read_buf[index]);
  }

  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_close(&read_file));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_unlink(&fs->names, alphabet_file));
}

/**
 * @brief Test filesystem space querying and file seek/tell contracts.
 *
 * @details
 * Validates space metrics on a temporary POSIX filesystem mount, and exercises
 * seek and tell offsets using internal_check_file_seeking.
 *
 * @pre Temp directory creation succeeds.
 * @pre POSIX filesystem initializes cleanly.
 * @post Total and free byte counts are greater than zero.
 * @post Temp directory is deleted before return.
 * @note Test case function.
 * @since 0.1.0
 */
RA8_INTERNAL static void test_alphabet_space_and_seek(void)
{
  static const char root_template_source[] = "/tmp/ra8_alphabet_seek_XXXXXX";
  char              root_template[sizeof(root_template_source)];
  (void)memcpy(root_template, root_template_source, sizeof(root_template));
  char* temp_root = mkdtemp(root_template);
  TEST_ASSERT_NOT_NULL(temp_root);

  fw_fs_t                 fs          = {};
  fw_fs_posix_state_t     posix_state = {.root_fd = -1};
  const fw_fs_posix_cfg_t cfg         = {
    .root_path       = root_template,
    .removable_media = false,
  };

  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_posix_init(&fs, &posix_state, &cfg));

  fw_fs_space_t space = {};
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_space(&fs.names, &space));
  TEST_ASSERT(space.total_bytes > 0U);
  TEST_ASSERT(space.free_bytes > 0U);

  internal_check_file_seeking(&fs);

  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_posix_deinit(&posix_state));
  TEST_ASSERT_EQ(0, rmdir(root_template));
}

/**
 * @brief Test 3x3 word search puzzle solving.
 *
 * @details
 * Solves a 3x3 grid with horizontal and diagonal word targets, capturing output
 * in a memory stream and asserting exact coordinate matches.
 *
 * @pre s_test_ctx is allocated and valid.
 * @pre Output buffer is initialized.
 * @post Solved coordinates match expected string output.
 * @post Memory state is preserved.
 * @note Helper test function.
 * @since 0.1.0
 */
RA8_INTERNAL static void test_solver_sample_3x3(void)
{
  const char puzzle_3x3[] = "3x3\n"
                            "A B C\n"
                            "D E F\n"
                            "G H I\n"
                            "ABC\n"
                            "AEI\n";

  char                      buf_3x3[k_capture_buf_cap] = {};
  ra8_io_stream_t           stream_3x3                 = {};
  ra8_io_stream_ram_state_t ram_state_3x3              = {};

  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_io_stream_ram_init(&stream_3x3, &ram_state_3x3, (uint8_t*)buf_3x3, sizeof(buf_3x3) - 1U));
  TEST_ASSERT_EQ(k_ra8_ok,
                 soup_solve(&s_test_ctx, puzzle_3x3, (uint32_t)strlen(puzzle_3x3), &stream_3x3));
  buf_3x3[ram_state_3x3.len] = '\0';

  const char expected_3x3[] = "ABC 0:0 0:2\n"
                              "AEI 0:0 2:2\n";
  TEST_ASSERT_EQ(0, strcmp(expected_3x3, buf_3x3));
}

/**
 * @brief Test 5x5 word search puzzle solving.
 *
 * @details
 * Solves a 5x5 grid with reverse horizontal and diagonal matches, asserting
 * exact string output against expected target coordinates.
 *
 * @pre s_test_ctx is allocated and valid.
 * @pre Output RAM stream is initialized.
 * @post Output string matches expected 5x5 coordinates.
 * @post Stream state is properly terminated.
 * @note Helper test function.
 * @since 0.1.0
 */
RA8_INTERNAL static void test_solver_sample_5x5(void)
{
  const char puzzle_5x5[] = "5x5\n"
                            "H A S D F\n"
                            "G E Y B H\n"
                            "J K L Z X\n"
                            "C V B L N\n"
                            "G O O D O\n"
                            "HELLO\n"
                            "GOOD\n"
                            "BYE\n";

  char                      buf_5x5[k_capture_buf_cap] = {};
  ra8_io_stream_t           stream_5x5                 = {};
  ra8_io_stream_ram_state_t ram_state_5x5              = {};

  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_io_stream_ram_init(&stream_5x5, &ram_state_5x5, (uint8_t*)buf_5x5, sizeof(buf_5x5) - 1U));
  TEST_ASSERT_EQ(k_ra8_ok,
                 soup_solve(&s_test_ctx, puzzle_5x5, (uint32_t)strlen(puzzle_5x5), &stream_5x5));
  buf_5x5[ram_state_5x5.len] = '\0';

  const char expected_5x5[] = "HELLO 0:0 4:4\n"
                              "GOOD 4:0 4:3\n"
                              "BYE 1:3 1:1\n";
  TEST_ASSERT_EQ(0, strcmp(expected_5x5, buf_5x5));
}

/**
 * @brief Test 8x8 space-containing word puzzle solving.
 *
 * @details
 * Solves an 8x8 grid where search targets contain internal whitespace (e.g. "ICE CREAM"),
 * asserting that spaces are stripped for searching but preserved in output.
 *
 * @pre s_test_ctx is allocated and valid.
 * @pre Output buffer is initialized.
 * @post Solved output matches expected diagonal coordinates.
 * @post Stream buffer is flushed.
 * @note Helper test function.
 * @since 0.1.0
 */
RA8_INTERNAL static void test_solver_spaces_8x8(void)
{
  const char puzzle_spaces[] = "8x8\n"
                               "I A B C D E F G\n"
                               "H C J K L M N O\n"
                               "P Q E S T U V W\n"
                               "X Y Z C B A D E\n"
                               "F G H I R K L M\n"
                               "N O P Q R E T U\n"
                               "V W X Y Z A A C\n"
                               "D E F G H I J M\n"
                               "ICE CREAM\n";

  char                      buf_spaces[k_capture_buf_cap] = {};
  ra8_io_stream_t           stream_spaces                 = {};
  ra8_io_stream_ram_state_t ram_state_spaces              = {};

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_io_stream_ram_init(&stream_spaces,
                                        &ram_state_spaces,
                                        (uint8_t*)buf_spaces,
                                        sizeof(buf_spaces) - 1U));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    soup_solve(&s_test_ctx, puzzle_spaces, (uint32_t)strlen(puzzle_spaces), &stream_spaces));
  buf_spaces[ram_state_spaces.len] = '\0';

  const char expected_spaces[] = "ICE CREAM 0:0 7:7\n";
  TEST_ASSERT_EQ(0, strcmp(expected_spaces, buf_spaces));
}

/**
 * @brief Test word-search error handling.
 *
 * @details
 * Passes null output pointers, invalid word lengths, and invalid grid dimensions
 * to soup_find_word, asserting that each call stays quiet.
 *
 * @pre None.
 * @post All invalid invocations return false.
 * @post No memory corruption occurs.
 * @note Helper test function.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_find_word_error_cases(void)
{
  soup_grid_t grid      = {.row_count = 1U, .col_count = 1U, .cells = {{'A'}}};
  uint32_t    start_row = 0U;
  uint32_t    start_col = 0U;
  uint32_t    end_row   = 0U;
  uint32_t    end_col   = 0U;
  TEST_ASSERT(!soup_find_word(nullptr, "A", 1U, &start_row, &start_col, &end_row, &end_col));
  TEST_ASSERT(!soup_find_word(&grid, nullptr, 1U, &start_row, &start_col, &end_row, &end_col));
  TEST_ASSERT(!soup_find_word(&grid, "A", 1U, nullptr, &start_col, &end_row, &end_col));
  TEST_ASSERT(!soup_find_word(&grid, "A", 1U, &start_row, nullptr, &end_row, &end_col));
  TEST_ASSERT(!soup_find_word(&grid, "A", 1U, &start_row, &start_col, nullptr, &end_col));
  TEST_ASSERT(!soup_find_word(&grid, "A", 1U, &start_row, &start_col, &end_row, nullptr));
  TEST_ASSERT(!soup_find_word(&grid, "A", 0U, &start_row, &start_col, &end_row, &end_col));
  TEST_ASSERT(!soup_find_word(&grid,
                              "A",
                              (uint32_t)k_soup_max_word_chars + 1U,
                              &start_row,
                              &start_col,
                              &end_row,
                              &end_col));
  grid.row_count = 0U;
  TEST_ASSERT(!soup_find_word(&grid, "A", 1U, &start_row, &start_col, &end_row, &end_col));
  grid.row_count = (uint32_t)k_soup_max_grid_rows + 1U;
  TEST_ASSERT(!soup_find_word(&grid, "A", 1U, &start_row, &start_col, &end_row, &end_col));
  grid.row_count = 1U;
  grid.col_count = 0U;
  TEST_ASSERT(!soup_find_word(&grid, "A", 1U, &start_row, &start_col, &end_row, &end_col));
  grid.col_count = (uint32_t)k_soup_max_grid_cols + 1U;
  TEST_ASSERT(!soup_find_word(&grid, "A", 1U, &start_row, &start_col, &end_row, &end_col));
  grid.col_count = 1U;
  TEST_ASSERT(!soup_find_word(&grid, "Z", 1U, &start_row, &start_col, &end_row, &end_col));
}

/**
 * @brief Test solver error handling on null arguments and malformed inputs.
 *
 * @details
 * Passes null pointers, zero lengths, dimension overflows, and malformed row
 * dimensions to soup_solve, asserting that canonical error codes are returned.
 *
 * @pre s_test_ctx is allocated.
 * @post All invalid invocations return appropriate ra8_err_t failure codes.
 * @post No memory corruption occurs.
 * @note Helper test function.
 * @since 0.1.0
 */
RA8_INTERNAL static void test_solver_error_cases(void)
{
  char                      buf[k_capture_buf_cap] = {};
  ra8_io_stream_t           stream                 = {};
  ra8_io_stream_ram_state_t ram_state              = {};

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_io_stream_ram_init(&stream, &ram_state, (uint8_t*)buf, sizeof(buf) - 1U));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, soup_init(nullptr));
  internal_test_find_word_error_cases();

  const char sample[] = "3x3\nA B C\nD E F\nG H I\nABC\n";
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, soup_solve(nullptr, sample, 10U, &stream));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, soup_solve(&s_test_ctx, nullptr, 10U, &stream));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, soup_solve(&s_test_ctx, sample, 10U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, soup_solve(&s_test_ctx, sample, 0U, &stream));

  const char invalid_dim[] = "999x999\nA\n";
  TEST_ASSERT_EQ(k_ra8_err_range_check_failed,
                 soup_solve(&s_test_ctx, invalid_dim, (uint32_t)strlen(invalid_dim), &stream));

  static const char* const bad_dimensions[] = {
    "x1\nA\n",
    "1-1\nA\n",
    "1x\nA\n",
    "0x1\nA\n",
    "129x1\nA\n",
    "1x0\nA\n",
    "1x129\nA\n",
  };
  for (size_t index = 0U; index < (sizeof(bad_dimensions) / sizeof(bad_dimensions[0])); ++index) {
    TEST_ASSERT(soup_solve(&s_test_ctx,
                           bad_dimensions[index],
                           (uint32_t)strlen(bad_dimensions[index]),
                           &stream) != k_ra8_ok);
  }

  const char blank_word[] = "1x1\nA\n \nZ\n";
  TEST_ASSERT_EQ(k_ra8_ok,
                 soup_solve(&s_test_ctx, blank_word, (uint32_t)strlen(blank_word), &stream));

  /* Test row with too few columns */
  const char row_too_short[] = "3x3\nA B\nD E F\nG H I\nABC\n";
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 soup_solve(&s_test_ctx, row_too_short, (uint32_t)strlen(row_too_short), &stream));

  /* Test row with too many columns (eagerly rejected) */
  const char row_too_long[] = "3x3\nA B C D\nD E F\nG H I\nABC\n";
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 soup_solve(&s_test_ctx, row_too_long, (uint32_t)strlen(row_too_long), &stream));
}

/**
 * @brief Test suite entry point.
 *
 * @param[in] argc Argument count (unused).
 * @param[in] argv Argument vector (unused).
 *
 * @return int Exit code (0 for pass, non-zero for failure).
 *
 * @note Hosted test runner.
 * @since 0.1.0
 */
int main(int argc, char** argv)
{
  (void)argc;
  (void)argv;

  test_posix_stream();
  test_cli_file_helper_guards();
  test_alphabet_files_roundtrip();
  test_alphabet_space_and_seek();
  test_solver_sample_3x3();
  test_solver_sample_5x5();
  test_solver_spaces_8x8();
  test_solver_error_cases();

  ra8_test_output_t    output = {};
  ra8_test_output_fd_t state  = {};
  if (!internal_test_output_fd_init(&output, &state, STDOUT_FILENO)) {
    return 1;
  }
  return (internal_test_output_text(&output, "test_alphabet_soup: all tests passed.\n") ==
          k_ra8_test_output_ok)
           ? 0
           : 1;
}
