/**
 * @file fuzz_ra_epub.c
 * @brief libFuzzer harness for ra_epub_open() (zip / EPUB parser).
 *
 * @details
 * EPUB files are ZIP archives plus an OPF/NCX manifest. The host
 * implementation hands the raw bytes to miniz inside a fixed-size
 * `ra_epub_book_t`. This harness wraps the input as an
 * `ra_epub_mem_media_t` and tries to open it. The parser must reject
 * every malformed input without crashing, leaking, or reading out of
 * bounds. ASan / UBSan diagnose any violation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra_epub.h"
#include "ra_err.h"

static ra_epub_book_t s_book;

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
  if (size == 0U || size > (1U << 20)) {
    return 0;
  }
  ra_epub_mem_media_t media = {.data = data, .size = size};
  memset(&s_book, 0, sizeof s_book);
  ra_err_t e = ra_epub_open(&media, "fuzz.epub", &s_book);
  if (e == k_ra_ok) {
    (void)ra_epub_close(&s_book);
  }
  return 0;
}
