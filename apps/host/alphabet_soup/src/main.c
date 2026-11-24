/**
 * @file main.c
 * @brief Standalone Alphabet Soup word search solver host CLI.
 * @par Tag
 * [Ring 4 / App] {World: Host}
 * @details Loads an ASCII word-search puzzle through the bounded POSIX adapter,
 *          solves it without heap allocation, and streams results to stdout.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "alphabet_soup.h"
#include "alphabet_soup_cli_internal.h"
#include "ra8_err.h"
#include "ra8_host.h"
#include "ra8_io_stream.h"

/** @brief Process exit codes. */
typedef enum : int {
  k_exit_code_success = 0, /**< Puzzle solved and output printed.  */
  k_exit_code_error   = 1, /**< Error during argument or file I/O. */
} file_print_exit_code_t;

/** @brief Static application workspace allocated in .bss. */
typedef struct {
  soup_context_t solver_ctx;                            /**< Bounded solver context. */
  uint8_t        file_buffer[k_soup_max_file_capacity]; /**< Bounded file buffer.    */
} soup_app_storage_t;

/**
 * @brief Application entry point.
 * @param[in] argc Argument count.
 * @param[in] argv Argument vector.
 * @return Process exit status.
 * @retval 0 Puzzle solved successfully.
 * @retval 1 Argument, file, or solver failure.
 * @pre Standard output descriptor is open and writable.
 * @post POSIX filesystem resources are closed before return.
 * @since 0.1.0
 */
int main(int argc, char** argv)
{
  static soup_app_storage_t s_app_storage;
  ra8_host_t                host     = {};
  const ra8_host_cfg_t      host_cfg = {.bind_streams = true};
  if (ra8_host_open(&host, &host_cfg) != k_ra8_ok) {
    return k_exit_code_error;
  }
  file_print_exit_code_t exit_code = k_exit_code_error;
  if (argc < 2) {
    (void)ra8_io_stream_puts(host.output, "Usage: alphabet_soup <file>\n");
    (void)ra8_io_stream_flush(host.output);
    (void)ra8_host_close(&host);
    return exit_code;
  }
  uint32_t  file_size = 0U;
  ra8_err_t err =
    priv_alphabet_soup_load_file_contents(argv[1],
                                          s_app_storage.file_buffer,
                                          (uint32_t)(sizeof(s_app_storage.file_buffer) - 1U),
                                          &file_size);
  if (err != k_ra8_ok) {
    (void)ra8_io_stream_puts(host.output, "Error: Could not open or read file\n");
    (void)ra8_io_stream_flush(host.output);
    (void)ra8_host_close(&host);
    return exit_code;
  }
  s_app_storage.file_buffer[file_size] = '\0';
  err                                  = soup_solve(&s_app_storage.solver_ctx,
                                                    (const char*)s_app_storage.file_buffer,
                                                    file_size,
                                                    host.output);
  if (err != k_ra8_ok) {
    (void)ra8_io_stream_puts(host.output, "Error: Failed solving puzzle\n");
    (void)ra8_io_stream_flush(host.output);
    (void)ra8_host_close(&host);
    return exit_code;
  }
  exit_code = k_exit_code_success;
  (void)ra8_host_close(&host);
  return exit_code;
}
