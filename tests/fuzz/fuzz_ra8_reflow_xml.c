/**
 * @file fuzz_ra8_reflow_xml.c
 * @brief libFuzzer harness for the tinyxml2-backed EPUB manifest parsers.
 *
 * @details
 * The EPUB reader parses the OPF package document, the EPUB 2 NCX table of
 * contents, and the EPUB 3 nav document with tinyxml2 (see
 * libs/ra8_epub/src/ra8_epub_xml_shim.cpp, driven from ra8_epub_open.c). Every one
 * of those XML documents comes straight out of the untrusted book file, so
 * tinyxml2::XMLDocument::Parse() runs on fully attacker-controlled bytes. The
 * existing fuzz_ra8_epub harness only reaches this XML layer after miniz has
 * successfully inflated a well-formed ZIP, so most inputs never exercise the
 * parser. This harness feeds the fuzz input directly to each of the three
 * XML parse entries the reader uses, so tinyxml2 is driven on every input. The
 * parsers must reject malformed XML with an error code, never crash, leak, or
 * read out of bounds; ASan / UBSan diagnose any violation. tinyxml2's node
 * allocations are served by the firmware's fixed operator-new arena
 * (ra8_epub_cpp_alloc.cpp), so no host heap is involved.
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

#include "fuzz_entry.h"
#include "ra8_epub.h"
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

/*
 * The tinyxml2-backed manifest parsers are declared extern "C" inside
 * ra8_epub_xml_shim.cpp (definition) and ra8_epub_open.c (consumer); there is no
 * shared public header. Mirror those prototypes here so the harness can drive
 * tinyxml2::XMLDocument::Parse() directly. Each mutates a caller-owned
 * ra8_epub_book_t in place and returns an ra8_err_t.
 */
ra8_err_t ra8_epub_xml_parse_opf(const uint8_t* xml_bytes, size_t xml_len, ra8_epub_book_t* book);
ra8_err_t ra8_epub_xml_parse_ncx(const uint8_t* xml_bytes, size_t xml_len, ra8_epub_book_t* book);
ra8_err_t ra8_epub_xml_parse_nav(const uint8_t* xml_bytes, size_t xml_len, ra8_epub_book_t* book);

static ra8_epub_book_t s_book;

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
  if (size == 0U || size > (1U << k_reflow_xml_u_20)) {
    return 0;
  }
  /* Each parser is independent: reset the book between passes so every entry
   * point sees the same input from a clean state. */
  memset(&s_book, 0, sizeof s_book);
  (void)ra8_epub_xml_parse_opf(data, size, &s_book);
  memset(&s_book, 0, sizeof s_book);
  (void)ra8_epub_xml_parse_ncx(data, size, &s_book);
  memset(&s_book, 0, sizeof s_book);
  (void)ra8_epub_xml_parse_nav(data, size, &s_book);
  return 0;
}
