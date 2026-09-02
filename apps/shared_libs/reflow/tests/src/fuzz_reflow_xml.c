/**
 * @file fuzz_reflow_xml.c
 * @brief libFuzzer harness for the bounded EPUB XML consumers.
 *
 * @details
 * The EPUB reader parses the OPF package document, the EPUB 2 NCX table of
 * contents, and the EPUB 3 nav document through the caller-owned bounded pull
 * reader in `apps/shared_libs/epub/src/epub_xml_shim.c`. Every one of those XML
 * documents comes straight out of the untrusted book file. The existing
 * fuzz_epub harness only reaches this XML layer after miniz has
 * successfully inflated a well-formed ZIP, so most inputs never exercise the
 * parser. This harness feeds the fuzz input directly to each of the three
 * XML parse entries the reader uses, so the strict lexer and consumers see
 * every input. They must reject malformed XML with an error code, never crash,
 * leak, mutate the source, or read out of bounds; ASan / UBSan diagnose any
 * violation. Parser state lives in each book's fixed XML workspace.
 *
 * Build via tests/fuzz/CMakeLists.txt with -DRA8_FUZZ=ON.
 * Run for a longer fuzz session via scripts/checks/run_fuzz.sh.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "epub.h"
#include "epub_xml_shim_internal.h"
#include "fuzz_entry.h"
#include "ra8_err.h"

/**
 * @enum reflow_xml_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_reflow_xml_u_20 = 20, /**< Log2 of the fuzz input-size cap (1 MiB). */
} reflow_xml_uint8_const_t;

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
  static epub_book_t s_book;
  if ((size == 0U) || (size > (1U << k_reflow_xml_u_20))) {
    return 0;
  }
  /* Each parser is independent: reset the book between passes so every entry
   * point sees the same input from a clean state. */
  (void)memset(&s_book, 0, sizeof s_book);
  (void)priv_epub_xml_parse_opf(data, size, &s_book);
  (void)memset(&s_book, 0, sizeof s_book);
  (void)priv_epub_xml_parse_ncx(data, size, &s_book);
  (void)memset(&s_book, 0, sizeof s_book);
  (void)priv_epub_xml_parse_nav(data, size, &s_book);
  return 0;
}
