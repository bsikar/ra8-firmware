/**
 * @file fuzz_ra8_epub.c
 * @brief libFuzzer harness for ra8_epub_open() (zip / EPUB parser).
 *
 * @details
 * EPUB files are ZIP archives plus an OPF/NCX manifest. The host
 * implementation hands the raw bytes to miniz inside a fixed-size
 * `ra8_epub_book_t`. This harness wraps the input as an
 * `ra8_epub_mem_media_t` and tries to open it. The parser must reject
 * every malformed input without crashing, leaking, or reading out of
 * bounds. ASan / UBSan diagnose any violation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "fuzz_entry.h"
#include "ra8_epub.h"
#include "ra8_err.h"

/**
 * @enum epub_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_fuzz_input_cap_log2 =
    20, /**< Log2 of the largest input this harness accepts; longer cases are dropped so a run stays bounded. */
} epub_uint8_const_t;

static ra8_epub_book_t s_book;

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
  if (size == 0U || size > (1U << k_fuzz_input_cap_log2)) {
    return 0;
  }
  ra8_epub_mem_media_t media = {.data = data, .size = size};
  memset(&s_book, 0, sizeof s_book);
  ra8_err_t e = ra8_epub_open(&media, "fuzz.epub", &s_book);
  if (e == k_ra8_ok) {
    (void)ra8_epub_close(&s_book);
  }
  return 0;
}
