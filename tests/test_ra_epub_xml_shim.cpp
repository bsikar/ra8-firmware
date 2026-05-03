/**
 * @file test_ra_epub_xml_shim.cpp
 * @brief MC/DC vector tests for libs/ra_epub/src/ra_epub_xml_shim.cpp.
 *
 * @par Tag
 * [Ring 4 / EPUB] {World: NS}
 *
 * @details
 * Drives the two C-linkage public entry points of the XML shim
 * (``ra_epub_xml_parse_container`` and ``ra_epub_xml_parse_opf``)
 * directly from C++ host code so MC/DC vectors land on the production
 * decisions at xml_shim.cpp lines 241, 268, 278, 314 -- the four
 * uncovered short-circuit OR guards listed in
 * ``docs/MCDC_GAPS.csv``.
 *
 * The TU-local ``static`` helpers (lines 101, 124, 150, 173 -- which
 * are NULL-defensive copies of contracts already enforced by these
 * public APIs) are documented as deactivated under DO-178C 6.4.4.3 in
 * ``docs/MCDC_DEACTIVATIONS.md``; they are not re-tested here.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" {
#include "ra_epub.h"
#include "ra_err.h"
}

/* -----------------------------------------------------------------------
 * The XML-shim out-parameter struct and entry-point declarations are
 * defined inside the C++ TU itself (they are not in any public header
 * because no external consumer needs them outside ra_epub). Mirror the
 * declarations here so the test can reach them via C linkage.
 * -----------------------------------------------------------------------
 */
extern "C" {

typedef struct {
  char opf_path[k_ra_epub_max_path_len];
} ra_epub_container_result_t;

ra_err_t ra_epub_xml_parse_container(const uint8_t*              xml_bytes,
                                     std::size_t                 xml_len,
                                     ra_epub_container_result_t* out);

ra_err_t ra_epub_xml_parse_opf(const uint8_t* xml_bytes, std::size_t xml_len, ra_epub_book_t* book);

} // extern "C"

namespace {

/* -----------------------------------------------------------------------
 * Static fixtures: minimal but well-formed container.xml / OPF documents.
 * -----------------------------------------------------------------------
 */
constexpr const char* k_valid_container =
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
  "<container xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\" "
  "version=\"1.0\">"
  "<rootfiles><rootfile full-path=\"OEBPS/content.opf\" "
  "media-type=\"application/oebps-package+xml\"/></rootfiles></container>";

constexpr const char* k_valid_opf =
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
  "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"id\">"
  "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
  "<dc:title>T</dc:title><dc:creator>A</dc:creator><dc:language>en</dc:language>"
  "</metadata>"
  "<manifest><item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/></manifest>"
  "<spine><itemref idref=\"c1\"/></spine>"
  "</package>";

/* OPF with no <manifest> element (but spine present) -- exercises
 * line-314 first OR-condition. */
constexpr const char* k_opf_no_manifest =
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
  "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\">"
  "<metadata/><spine/></package>";

/* OPF with manifest but no spine -- exercises line-314 second OR. */
constexpr const char* k_opf_no_spine =
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
  "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\">"
  "<metadata/><manifest/></package>";

/* container.xml whose <rootfile> element is missing the full-path
 * attribute -- exercises line-268 first OR (full_path == nullptr). */
constexpr const char* k_container_no_full_path =
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
  "<container xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">"
  "<rootfiles><rootfile media-type=\"application/oebps-package+xml\"/></rootfiles>"
  "</container>";

/* container.xml whose <rootfile full-path=""> is the empty string --
 * exercises line-268 second OR (full_path[0] == '\0'). */
constexpr const char* k_container_empty_full_path =
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
  "<container xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">"
  "<rootfiles><rootfile full-path=\"\" "
  "media-type=\"application/oebps-package+xml\"/></rootfiles></container>";

/**
 * @test test_mcdc_parse_container_null_or_zero
 *
 * @par MC/DC:
 * Decision 1: ``if (xml_bytes == nullptr || out == nullptr)``
 * (xml_shim.cpp line 241; 2 conditions).
 *  - V1: xml=valid, out=valid  -> C1=F,C2=F. Decision F (proceed).
 *  - V2: xml=NULL,  out=valid  -> C1=T short.  Decision T (null_ptr).
 *  - V3: xml=valid, out=NULL   -> C1=F,C2=T.  Decision T (null_ptr).
 * V1+V2 isolate C1; V1+V3 isolate C2.
 *
 * Decision 2: ``if (full_path == nullptr || full_path[0] == '\0')``
 * (xml_shim.cpp line 268; 2 conditions).
 *  - V4: full-path="OEBPS/content.opf" -> C1=F,C2=F. Decision F (ok).
 *  - V5: full-path attr missing        -> C1=T short. Decision T (validation).
 *  - V6: full-path=""                  -> C1=F,C2=T. Decision T (validation).
 * V4+V5 isolate C1; V4+V6 isolate C2.
 */
void test_mcdc_parse_container_null_or_zero(void)
{
  std::printf("test_mcdc_parse_container_null_or_zero: ");

  ra_epub_container_result_t res = {};

  /* Decision 1, V1: both non-NULL -> success. */
  const auto* xml_v1 = reinterpret_cast<const uint8_t*>(k_valid_container);
  assert(ra_epub_xml_parse_container(xml_v1, std::strlen(k_valid_container), &res) == k_ra_ok);
  assert(std::strcmp(res.opf_path, "OEBPS/content.opf") == 0);

  /* Decision 1, V2: xml_bytes NULL -> null_ptr. */
  assert(ra_epub_xml_parse_container(nullptr, 16U, &res) == k_ra_err_null_ptr);

  /* Decision 1, V3: out NULL -> null_ptr. */
  assert(ra_epub_xml_parse_container(xml_v1, std::strlen(k_valid_container), nullptr) ==
         k_ra_err_null_ptr);

  /* Decision 2, V5: full-path attr missing -> validation_failed. */
  const auto* xml_v5 = reinterpret_cast<const uint8_t*>(k_container_no_full_path);
  assert(ra_epub_xml_parse_container(xml_v5, std::strlen(k_container_no_full_path), &res) ==
         k_ra_err_validation_failed);

  /* Decision 2, V6: full-path="" -> validation_failed. */
  const auto* xml_v6 = reinterpret_cast<const uint8_t*>(k_container_empty_full_path);
  assert(ra_epub_xml_parse_container(xml_v6, std::strlen(k_container_empty_full_path), &res) ==
         k_ra_err_validation_failed);

  std::printf("ok\n");
}

/**
 * @test test_mcdc_parse_opf_null_and_manifest_spine
 *
 * @par MC/DC:
 * Decision 1: ``if (xml_bytes == nullptr || book == nullptr)``
 * (xml_shim.cpp line 278; 2 conditions).
 *  - V1: xml=valid, book=valid -> C1=F,C2=F. Decision F (proceed).
 *  - V2: xml=NULL,  book=valid -> C1=T short. Decision T (null_ptr).
 *  - V3: xml=valid, book=NULL  -> C1=F,C2=T. Decision T (null_ptr).
 * V1+V2 isolate C1; V1+V3 isolate C2.
 *
 * Decision 2: ``if (manifest == nullptr || spine == nullptr)``
 * (xml_shim.cpp line 314; 2 conditions).
 *  - V4: both present  -> C1=F,C2=F. Decision F (proceed).
 *  - V5: no manifest   -> C1=T short. Decision T (validation_failed).
 *  - V6: no spine      -> C1=F,C2=T. Decision T (validation_failed).
 * V4+V5 isolate C1; V4+V6 isolate C2.
 */
void test_mcdc_parse_opf_null_and_manifest_spine(void)
{
  std::printf("test_mcdc_parse_opf_null_and_manifest_spine: ");

  ra_epub_book_t book = {};

  /* Decision 1, V1 + Decision 2, V4: well-formed OPF -> ok. */
  const auto* xml_v1 = reinterpret_cast<const uint8_t*>(k_valid_opf);
  assert(ra_epub_xml_parse_opf(xml_v1, std::strlen(k_valid_opf), &book) == k_ra_ok);

  /* Decision 1, V2: xml_bytes NULL -> null_ptr. */
  assert(ra_epub_xml_parse_opf(nullptr, 16U, &book) == k_ra_err_null_ptr);

  /* Decision 1, V3: book NULL -> null_ptr. */
  assert(ra_epub_xml_parse_opf(xml_v1, std::strlen(k_valid_opf), nullptr) == k_ra_err_null_ptr);

  /* Decision 2, V5: well-formed XML but missing <manifest> -> validation_failed. */
  std::memset(&book, 0, sizeof(book));
  const auto* xml_v5 = reinterpret_cast<const uint8_t*>(k_opf_no_manifest);
  assert(ra_epub_xml_parse_opf(xml_v5, std::strlen(k_opf_no_manifest), &book) ==
         k_ra_err_validation_failed);

  /* Decision 2, V6: well-formed XML but missing <spine> -> validation_failed. */
  std::memset(&book, 0, sizeof(book));
  const auto* xml_v6 = reinterpret_cast<const uint8_t*>(k_opf_no_spine);
  assert(ra_epub_xml_parse_opf(xml_v6, std::strlen(k_opf_no_spine), &book) ==
         k_ra_err_validation_failed);

  std::printf("ok\n");
}

} // namespace

int main(void)
{
  test_mcdc_parse_container_null_or_zero();
  test_mcdc_parse_opf_null_and_manifest_spine();
  std::fprintf(stderr, "[OK ] test_ra_epub_xml_shim.cpp\n");
  return 0;
}
