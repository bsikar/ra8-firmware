/**
 * @file fuzz_epub.c
 * @brief libFuzzer harness for epub_open() (zip / EPUB parser).
 *
 * @details
 * EPUB files are ZIP archives plus an OPF/NCX manifest. The host
 * implementation hands the raw bytes to miniz inside a fixed-size
 * `epub_book_t`. This harness wraps the input as an
 * `epub_mem_media_t` and tries to open it. The parser must reject
 * every malformed input without crashing, leaking, or reading out of
 * bounds. ASan / UBSan diagnose any violation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "epub.h"
#include "fuzz_entry.h"
#include "ra8_err.h"

/**
 * @enum epub_fuzz_bound_t
 * @brief Log2 of the largest input this harness accepts, so a run stays bounded.
 */
typedef enum : uint8_t {
  k_fuzz_input_cap_mib = 1U, /**< MiB accepted; longer cases drop so a run stays bounded. */
} epub_fuzz_bound_t;

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
  static epub_book_t s_book;
  if ((size == 0U) || (size > ((size_t)k_fuzz_input_cap_mib * 1024U * 1024U))) {
    return 0;
  }
  epub_mem_media_t media = {.data = data, .size = size};
  (void)memset(&s_book, 0, sizeof s_book);
  ra8_err_t e = epub_open(&media, "fuzz.epub", &s_book);
  if (e == k_ra8_ok) {
    (void)epub_close(&s_book);
  }
  return 0;
}
