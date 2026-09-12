/**
 * @file jof_worker_main.c
 * @brief Process entry for the cbz2jof page worker.
 * @details Requires exactly two paths after `argv[0]` (encoded source image,
 *          destination JOF atlas) and exits with the
 *          ::jof_worker_result_t code from `jof_worker_convert()`.
 *
 * [Ring 4 / Domain] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "jof_worker.h"

/**
 * @brief Convert one image file into one JOF atlas file.
 * @param[in] argc Argument count (must be 3: program, input, output).
 * @param[in] argv Argument vector.
 * @return The ::jof_worker_result_t code as a process exit status.
 * @since 0.1.0
 */
int main(int argc, char** argv)
{
  if (argc != 3) {
    return (int)k_jof_worker_usage;
  }
  return (int)jof_worker_convert(argv[1], argv[2]);
}
