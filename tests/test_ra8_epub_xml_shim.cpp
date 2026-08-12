/**
 * @file test_ra8_epub_xml_shim.cpp
 * @brief MC/DC vector tests for libs/ra8_epub/src/ra8_epub_xml_shim.cpp.
 *
 * @par Tag
 * [Ring 4 / EPUB] {World: NS}
 *
 * @details
 * Drives the two C-linkage public entry points of the XML shim
 * (``ra8_epub_xml_parse_container`` and ``ra8_epub_xml_parse_opf``)
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
 * @since 0.1.0
 */

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" {
#include "ra8_epub.h"
#include "ra8_err.h"
}

#include "ra8_epub_xml_shim_internal.h"

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

  ra8_epub_container_result_t res = {};

  /* Decision 1, V1: both non-NULL -> success. */
  const auto* xml_v1 = reinterpret_cast<const uint8_t*>(k_valid_container);
  assert(ra8_epub_xml_parse_container(xml_v1, std::strlen(k_valid_container), &res) == k_ra8_ok);
  assert(std::strcmp(res.opf_path, "OEBPS/content.opf") == 0);

  /* Decision 1, V2: xml_bytes NULL -> null_ptr. */
  assert(ra8_epub_xml_parse_container(nullptr, 16U, &res) == k_ra8_err_null_ptr);

  /* Decision 1, V3: out NULL -> null_ptr. */
  assert(ra8_epub_xml_parse_container(xml_v1, std::strlen(k_valid_container), nullptr) ==
         k_ra8_err_null_ptr);

  /* Decision 2, V5: full-path attr missing -> validation_failed. */
  const auto* xml_v5 = reinterpret_cast<const uint8_t*>(k_container_no_full_path);
  assert(ra8_epub_xml_parse_container(xml_v5, std::strlen(k_container_no_full_path), &res) ==
         k_ra8_err_validation_failed);

  /* Decision 2, V6: full-path="" -> validation_failed. */
  const auto* xml_v6 = reinterpret_cast<const uint8_t*>(k_container_empty_full_path);
  assert(ra8_epub_xml_parse_container(xml_v6, std::strlen(k_container_empty_full_path), &res) ==
         k_ra8_err_validation_failed);

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

  ra8_epub_book_t book = {};

  /* Decision 1, V1 + Decision 2, V4: well-formed OPF -> ok. */
  const auto* xml_v1 = reinterpret_cast<const uint8_t*>(k_valid_opf);
  assert(ra8_epub_xml_parse_opf(xml_v1, std::strlen(k_valid_opf), &book) == k_ra8_ok);

  /* Decision 1, V2: xml_bytes NULL -> null_ptr. */
  assert(ra8_epub_xml_parse_opf(nullptr, 16U, &book) == k_ra8_err_null_ptr);

  /* Decision 1, V3: book NULL -> null_ptr. */
  assert(ra8_epub_xml_parse_opf(xml_v1, std::strlen(k_valid_opf), nullptr) == k_ra8_err_null_ptr);

  /* Decision 2, V5: well-formed XML but missing <manifest> -> validation_failed. */
  std::memset(&book, 0, sizeof(book));
  const auto* xml_v5 = reinterpret_cast<const uint8_t*>(k_opf_no_manifest);
  assert(ra8_epub_xml_parse_opf(xml_v5, std::strlen(k_opf_no_manifest), &book) ==
         k_ra8_err_validation_failed);

  /* Decision 2, V6: well-formed XML but missing <spine> -> validation_failed. */
  std::memset(&book, 0, sizeof(book));
  const auto* xml_v6 = reinterpret_cast<const uint8_t*>(k_opf_no_spine);
  assert(ra8_epub_xml_parse_opf(xml_v6, std::strlen(k_opf_no_spine), &book) ==
         k_ra8_err_validation_failed);

  std::printf("ok\n");
}

/* -----------------------------------------------------------------------
 * Table-of-contents fixtures (#74): NCX (EPUB 2) and nav.xhtml (EPUB 3).
 * Both encode the same logical TOC -- a top-level "Chapter One" with a
 * nested "Section 1.1", then a top-level "Chapter Two" -- so the two
 * parsers can be asserted against one shared expectation.
 * -----------------------------------------------------------------------
 */
constexpr const char* k_ncx_doc =
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
  "<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\">"
  "<head/><docTitle><text>Book</text></docTitle>"
  "<navMap>"
  "<navPoint id=\"np1\" playOrder=\"1\"><navLabel><text>Chapter One</text></navLabel>"
  "<content src=\"c1.xhtml\"/>"
  "<navPoint id=\"np1a\" playOrder=\"2\"><navLabel><text>Section 1.1</text></navLabel>"
  "<content src=\"c1.xhtml#s1\"/></navPoint>"
  "</navPoint>"
  "<navPoint id=\"np2\" playOrder=\"3\"><navLabel><text>Chapter Two</text></navLabel>"
  "<content src=\"c2.xhtml\"/></navPoint>"
  "</navMap></ncx>";

/* The landmarks <nav> deliberately comes first (and also carries an
 * <ol>) so the test proves we pick the epub:type="toc" nav, not the
 * first one. */
constexpr const char* k_nav_doc =
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
  "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\">"
  "<head><title>TOC</title></head><body>"
  "<nav epub:type=\"landmarks\"><ol><li><a href=\"c1.xhtml\">Start Reading</a></li></ol></nav>"
  "<nav epub:type=\"toc\" id=\"toc\"><h1>Contents</h1><ol>"
  "<li><a href=\"c1.xhtml\">Chapter One</a>"
  "<ol><li><a href=\"c1.xhtml#s1\">Section 1.1</a></li></ol></li>"
  "<li><a href=\"c2.xhtml\">Chapter Two</a></li>"
  "</ol></nav></body></html>";

/* OPF whose manifest declares an EPUB 3 nav document. */
constexpr const char* k_opf_with_nav =
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
  "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"id\">"
  "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>T</dc:title></metadata>"
  "<manifest>"
  "<item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>"
  "<item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/>"
  "</manifest><spine><itemref idref=\"c1\"/></spine></package>";

/* OPF whose spine references an EPUB 2 NCX via the `toc` attribute. */
constexpr const char* k_opf_with_ncx =
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
  "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"2.0\" unique-identifier=\"id\">"
  "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>T</dc:title></metadata>"
  "<manifest>"
  "<item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>"
  "<item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/>"
  "</manifest><spine toc=\"ncx\"><itemref idref=\"c1\"/></spine></package>";

/* Assert the shared 3-entry expectation against a parsed book. */
void assert_shared_toc(const ra8_epub_book_t& book)
{
  assert(book.toc_count == 3U);
  assert(std::strcmp(book.toc[0].title, "Chapter One") == 0);
  assert(std::strcmp(book.toc[0].href, "c1.xhtml") == 0);
  assert(book.toc[0].depth == 0U);
  assert(std::strcmp(book.toc[1].title, "Section 1.1") == 0);
  assert(std::strcmp(book.toc[1].href, "c1.xhtml#s1") == 0);
  assert(book.toc[1].depth == 1U);
  assert(std::strcmp(book.toc[2].title, "Chapter Two") == 0);
  assert(std::strcmp(book.toc[2].href, "c2.xhtml") == 0);
  assert(book.toc[2].depth == 0U);
}

/**
 * @test test_parse_ncx_and_nav
 *
 * Both navigation-document parsers must flatten the same hierarchical
 * TOC depth-first into identical title/href/depth triples.
 */
void test_parse_ncx_and_nav(void)
{
  std::printf("test_parse_ncx_and_nav: ");

  ra8_epub_book_t book = {};
  const auto*     ncx  = reinterpret_cast<const uint8_t*>(k_ncx_doc);
  assert(ra8_epub_xml_parse_ncx(ncx, std::strlen(k_ncx_doc), &book) == k_ra8_ok);
  assert_shared_toc(book);

  std::memset(&book, 0, sizeof(book));
  const auto* nav = reinterpret_cast<const uint8_t*>(k_nav_doc);
  assert(ra8_epub_xml_parse_nav(nav, std::strlen(k_nav_doc), &book) == k_ra8_ok);
  assert_shared_toc(book);

  std::printf("ok\n");
}

/**
 * @test test_parse_opf_toc_source
 *
 * `ra8_epub_xml_parse_opf` must record which navigation document to load:
 * `properties="nav"` -> nav kind, spine `toc=` -> NCX kind, neither ->
 * none.
 */
void test_parse_opf_toc_source(void)
{
  std::printf("test_parse_opf_toc_source: ");

  ra8_epub_book_t book = {};
  const auto*     nav  = reinterpret_cast<const uint8_t*>(k_opf_with_nav);
  assert(ra8_epub_xml_parse_opf(nav, std::strlen(k_opf_with_nav), &book) == k_ra8_ok);
  assert(book.toc_kind == static_cast<uint8_t>(k_ra8_epub_toc_nav));
  assert(std::strcmp(book.toc_path, "nav.xhtml") == 0);

  std::memset(&book, 0, sizeof(book));
  const auto* ncx = reinterpret_cast<const uint8_t*>(k_opf_with_ncx);
  assert(ra8_epub_xml_parse_opf(ncx, std::strlen(k_opf_with_ncx), &book) == k_ra8_ok);
  assert(book.toc_kind == static_cast<uint8_t>(k_ra8_epub_toc_ncx));
  assert(std::strcmp(book.toc_path, "toc.ncx") == 0);

  /* The plain fixture has neither a nav item nor a spine toc attr. */
  std::memset(&book, 0, sizeof(book));
  const auto* plain = reinterpret_cast<const uint8_t*>(k_valid_opf);
  assert(ra8_epub_xml_parse_opf(plain, std::strlen(k_valid_opf), &book) == k_ra8_ok);
  assert(book.toc_kind == static_cast<uint8_t>(k_ra8_epub_toc_none));
  assert(book.toc_path[0] == '\0');

  std::printf("ok\n");
}

/**
 * @test test_parse_toc_guards
 *
 * NULL / zero-length / malformed inputs are rejected without touching
 * the caller's TOC beyond the documented reset.
 */
void test_parse_toc_guards(void)
{
  std::printf("test_parse_toc_guards: ");

  ra8_epub_book_t book = {};
  const auto*     ncx  = reinterpret_cast<const uint8_t*>(k_ncx_doc);

  assert(ra8_epub_xml_parse_ncx(nullptr, 16U, &book) == k_ra8_err_null_ptr);
  assert(ra8_epub_xml_parse_ncx(ncx, 0U, &book) == k_ra8_err_invalid_size);
  assert(ra8_epub_xml_parse_nav(nullptr, 16U, &book) == k_ra8_err_null_ptr);

  /* Well-formed XML but no navMap / no nav element -> validation_failed. */
  constexpr const char* k_not_ncx = "<?xml version=\"1.0\"?><ncx><head/></ncx>";
  const auto*           bad_ncx   = reinterpret_cast<const uint8_t*>(k_not_ncx);
  assert(ra8_epub_xml_parse_ncx(bad_ncx, std::strlen(k_not_ncx), &book) ==
         k_ra8_err_validation_failed);

  constexpr const char* k_not_nav = "<?xml version=\"1.0\"?><html><body><p>x</p></body></html>";
  const auto*           bad_nav   = reinterpret_cast<const uint8_t*>(k_not_nav);
  assert(ra8_epub_xml_parse_nav(bad_nav, std::strlen(k_not_nav), &book) ==
         k_ra8_err_validation_failed);

  std::printf("ok\n");
}

/* -----------------------------------------------------------------------
 * Additional fixtures driving the remaining short-circuit arms of the
 * compound decisions inside the shim's TU-local helpers. Each helper is
 * exercised through its sole public entry point (parse_opf / parse_ncx /
 * parse_nav); the input is crafted so tinyxml2 still parses it but a
 * single condition flips to its uncovered value.
 * -----------------------------------------------------------------------
 */

/**
 * @brief Truncation length copy_bounded() yields for a `k_ra8_epub_meta_len`
 *        destination: `cap - 1` payload bytes plus the NUL terminator.
 */
constexpr std::size_t k_meta_truncated_len = static_cast<std::size_t>(k_ra8_epub_meta_len) - 1U;

/* NCX whose first navPoint <text> is longer than `k_ra8_epub_meta_len`, so
 * copy_bounded() must stop on its cap watchdog (`i + 1U < cap` false) rather
 * than the source NUL -- exercises the xml_shim while-loop first OR. The
 * title string below is 150 'A' characters (> 127). */
constexpr const char* k_ncx_long_title =
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
  "<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\">"
  "<navMap><navPoint id=\"np1\" playOrder=\"1\"><navLabel><text>"
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
  "</text></navLabel><content src=\"c1.xhtml\"/></navPoint></navMap></ncx>";

/* OPF whose manifest places an attribute-less <item> (no `id`) ahead of the
 * spine target; manifest_href_by_id() then sees `item_id == nullptr` on the
 * first iteration -- exercises the line-203 first AND condition (C1=F). */
constexpr const char* k_opf_manifest_item_no_id =
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
  "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\">"
  "<metadata/>"
  "<manifest>"
  "<item href=\"orphan.xhtml\" media-type=\"application/xhtml+xml\"/>"
  "<item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/>"
  "</manifest><spine><itemref idref=\"c1\"/></spine></package>";

/* OPF with no properties="cover-image" item but a legacy
 * <meta name="cover" content="cov"/> resolving to a manifest href -- drives
 * find_cover_by_meta() down its all-true (TTT) arm. */
constexpr const char* k_opf_meta_cover_ok =
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
  "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"2.0\">"
  "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
  "<dc:title>T</dc:title>"
  "<meta name=\"cover\" content=\"cov\"/>"
  "</metadata>"
  "<manifest>"
  "<item id=\"cov\" href=\"cover.png\" media-type=\"image/png\"/>"
  "<item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/>"
  "</manifest><spine><itemref idref=\"c1\"/></spine></package>";

/* OPF whose <meta> has no `name` attribute -- find_cover_by_meta() sees
 * `meta_name == nullptr` (line-251 first AND condition, C1=F). */
constexpr const char* k_opf_meta_no_name =
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
  "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"2.0\">"
  "<metadata><meta content=\"cov\"/></metadata>"
  "<manifest>"
  "<item id=\"cov\" href=\"cover.png\" media-type=\"image/png\"/>"
  "<item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/>"
  "</manifest><spine><itemref idref=\"c1\"/></spine></package>";

/* OPF whose <meta name="cover"> has no `content` attribute --
 * find_cover_by_meta() sees `meta_content == nullptr` (line-251 second AND
 * condition, C2=F). */
constexpr const char* k_opf_meta_no_content =
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
  "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"2.0\">"
  "<metadata><meta name=\"cover\"/></metadata>"
  "<manifest>"
  "<item id=\"cov\" href=\"cover.png\" media-type=\"image/png\"/>"
  "<item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/>"
  "</manifest><spine><itemref idref=\"c1\"/></spine></package>";

/* OPF whose <meta> carries name + content but the name is not "cover" --
 * find_cover_by_meta() takes the line-251 third AND condition false (C3=F),
 * so no cover is recorded. */
constexpr const char* k_opf_meta_other_name =
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
  "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"2.0\">"
  "<metadata><meta name=\"generator\" content=\"cov\"/></metadata>"
  "<manifest>"
  "<item id=\"cov\" href=\"cover.png\" media-type=\"image/png\"/>"
  "<item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/>"
  "</manifest><spine><itemref idref=\"c1\"/></spine></package>";

/* OPF whose only `properties` attribute holds "cover-image" (not "nav") and
 * has no nav item and no spine toc -- find_nav_manifest_href() sees
 * `props != nullptr` but `strstr(props, "nav") == nullptr`, exercising the
 * line-328 second AND condition (C2=F). */
constexpr const char* k_opf_props_not_nav =
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
  "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\">"
  "<metadata/>"
  "<manifest>"
  "<item id=\"cov\" href=\"cover.png\" media-type=\"image/png\" "
  "properties=\"cover-image\"/>"
  "<item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/>"
  "</manifest><spine><itemref idref=\"c1\"/></spine></package>";

/* OPF manifest carrying (a) a font <item> with a recognised font media-type
 * and (b) an <item> with no `href` attribute. collect_font_items() then
 * hits both line-379 arms: C1=F,C2=F for the font (recorded) and C1=T for
 * the href-less item (skipped). */
constexpr const char* k_opf_font_and_no_href =
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
  "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\">"
  "<metadata/>"
  "<manifest>"
  "<item id=\"noh\" media-type=\"application/xhtml+xml\"/>"
  "<item id=\"f1\" href=\"fonts/serif.ttf\" media-type=\"font/ttf\"/>"
  "<item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/>"
  "</manifest><spine><itemref idref=\"c1\"/></spine></package>";

/* nav document whose first <nav> has no epub:type attribute, so
 * find_nav_by_type() sees `attr == nullptr` (line-434 first AND condition,
 * C1=F) before it finds the epub:type="toc" nav. */
constexpr const char* k_nav_untyped_first = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                                            "<html xmlns=\"http://www.w3.org/1999/xhtml\" "
                                            "xmlns:epub=\"http://www.idpf.org/2007/ops\">"
                                            "<head><title>TOC</title></head><body>"
                                            "<nav><ol><li><a href=\"x.xhtml\">X</a></li></ol></nav>"
                                            "<nav epub:type=\"toc\"><ol>"
                                            "<li><a href=\"c1.xhtml\">Chapter One</a></li>"
                                            "</ol></nav></body></html>";

/**
 * @test test_mcdc_copy_bounded_truncation
 *
 * @par MC/DC:
 * Decision: ``while (i + 1U < cap && src[i] != '\0')`` inside
 * ``copy_bounded`` (xml_shim.cpp line 132; 2 conditions). Driven via the
 * NCX title path (``ncx_walk -> toc_emit -> copy_bounded`` with
 * ``cap == k_ra8_epub_meta_len``).
 *  - V1: short title (existing fixtures) -> last iteration has C1=T,C2=F.
 *    Loop ends on the NUL. Decision F (terminate). Isolates C2.
 *  - V2: title 150 chars > cap-1         -> at i == cap-1 we have C1=F,
 *    C2=T. Loop ends on the cap watchdog. Decision F (terminate). Isolates
 *    C1.
 * V1+V2 isolate C1; the body iterations (both conditions true) plus V1
 * isolate C2. Result: the stored title is truncated to exactly
 * ``k_ra8_epub_meta_len - 1`` bytes.
 */
void test_mcdc_copy_bounded_truncation(void)
{
  std::printf("test_mcdc_copy_bounded_truncation: ");

  ra8_epub_book_t book = {};
  const auto*     ncx  = reinterpret_cast<const uint8_t*>(k_ncx_long_title);
  assert(ra8_epub_xml_parse_ncx(ncx, std::strlen(k_ncx_long_title), &book) == k_ra8_ok);
  assert(book.toc_count == 1U);
  assert(std::strlen(book.toc[0].title) == k_meta_truncated_len);

  std::printf("ok\n");
}

/**
 * @test test_mcdc_manifest_item_missing_id
 *
 * @par MC/DC:
 * Decision: ``if (item_id != nullptr && std::strcmp(item_id, id) == 0)``
 * inside ``manifest_href_by_id`` (xml_shim.cpp line 203; 2 conditions).
 * Driven through ``ra8_epub_xml_parse_opf`` spine resolution.
 *  - V1 (existing valid OPF): first item has id="c1" matching the spine
 *    idref -> C1=T,C2=T. Decision T (return href). Isolates C2 vs V2.
 *  - V2: first manifest item carries no `id` attribute -> C1=F short.
 *    Decision F (skip item, scan on). Isolates C1 vs V1.
 * The href-less first item is skipped and the spine still resolves to the
 * second item, so exactly one chapter is recorded.
 */
void test_mcdc_manifest_item_missing_id(void)
{
  std::printf("test_mcdc_manifest_item_missing_id: ");

  ra8_epub_book_t book = {};
  const auto*     xml  = reinterpret_cast<const uint8_t*>(k_opf_manifest_item_no_id);
  assert(ra8_epub_xml_parse_opf(xml, std::strlen(k_opf_manifest_item_no_id), &book) == k_ra8_ok);
  assert(book.chapter_count == 1U);
  assert(std::strcmp(book.chapter_paths[0], "c1.xhtml") == 0);

  std::printf("ok\n");
}

/**
 * @test test_mcdc_cover_by_meta
 *
 * @par MC/DC:
 * Decision: ``if (meta_name != nullptr && meta_content != nullptr &&
 * std::strcmp(meta_name, "cover") == 0)`` inside ``find_cover_by_meta``
 * (xml_shim.cpp line 251; 3 conditions). Reached because none of these
 * OPFs declare ``properties="cover-image"``, so ``parse_opf`` falls back
 * to the legacy ``<meta name="cover">`` lookup.
 *  - V1: name="cover", content="cov"     -> C1=T,C2=T,C3=T. Decision T
 *    (resolve href). cover_path == "cover.png".
 *  - V2: <meta> has no name attribute     -> C1=F short. Decision F.
 *    cover_path stays empty. Isolates C1 (V1 vs V2).
 *  - V3: <meta name="cover"> no content   -> C1=T,C2=F short. Decision F.
 *    cover_path stays empty. Isolates C2 (V1 vs V3).
 *  - V4: name="generator", content="cov"  -> C1=T,C2=T,C3=F. Decision F.
 *    cover_path stays empty. Isolates C3 (V1 vs V4).
 * N+1 = 4 vectors for N=3 conditions: minimal MC/DC.
 */
void test_mcdc_cover_by_meta(void)
{
  std::printf("test_mcdc_cover_by_meta: ");

  ra8_epub_book_t book = {};

  /* V1: all three conditions true -> cover resolved. */
  const auto* xml_ok = reinterpret_cast<const uint8_t*>(k_opf_meta_cover_ok);
  assert(ra8_epub_xml_parse_opf(xml_ok, std::strlen(k_opf_meta_cover_ok), &book) == k_ra8_ok);
  assert(std::strcmp(book.cover_path, "cover.png") == 0);

  /* V2: meta name attribute missing -> C1=F, no cover. */
  std::memset(&book, 0, sizeof(book));
  const auto* xml_no_name = reinterpret_cast<const uint8_t*>(k_opf_meta_no_name);
  assert(ra8_epub_xml_parse_opf(xml_no_name, std::strlen(k_opf_meta_no_name), &book) == k_ra8_ok);
  assert(book.cover_path[0] == '\0');

  /* V3: meta content attribute missing -> C2=F, no cover. */
  std::memset(&book, 0, sizeof(book));
  const auto* xml_no_content = reinterpret_cast<const uint8_t*>(k_opf_meta_no_content);
  assert(ra8_epub_xml_parse_opf(xml_no_content, std::strlen(k_opf_meta_no_content), &book) ==
         k_ra8_ok);
  assert(book.cover_path[0] == '\0');

  /* V4: meta name is not "cover" -> C3=F, no cover. */
  std::memset(&book, 0, sizeof(book));
  const auto* xml_other = reinterpret_cast<const uint8_t*>(k_opf_meta_other_name);
  assert(ra8_epub_xml_parse_opf(xml_other, std::strlen(k_opf_meta_other_name), &book) == k_ra8_ok);
  assert(book.cover_path[0] == '\0');

  std::printf("ok\n");
}

/**
 * @test test_mcdc_nav_manifest_props_not_nav
 *
 * @par MC/DC:
 * Decision: ``if (props != nullptr && std::strstr(props, "nav") != nullptr)``
 * inside ``find_nav_manifest_href`` (xml_shim.cpp line 328; 2 conditions).
 * Driven through ``ra8_epub_xml_parse_opf`` TOC-source selection.
 *  - V1 (k_opf_with_nav): item properties="nav" -> C1=T,C2=T. Decision T
 *    (toc_kind == nav). Already covered by test_parse_opf_toc_source.
 *  - V2 (k_valid_opf): no properties attribute   -> C1=F short. Decision F.
 *    Already covered by test_parse_opf_toc_source.
 *  - V3 (here): properties="cover-image"          -> C1=T,C2=F. Decision F
 *    (no nav). toc_kind stays none. Isolates C2 (V1 vs V3).
 */
void test_mcdc_nav_manifest_props_not_nav(void)
{
  std::printf("test_mcdc_nav_manifest_props_not_nav: ");

  ra8_epub_book_t book = {};
  const auto*     xml  = reinterpret_cast<const uint8_t*>(k_opf_props_not_nav);
  assert(ra8_epub_xml_parse_opf(xml, std::strlen(k_opf_props_not_nav), &book) == k_ra8_ok);
  assert(book.toc_kind == static_cast<uint8_t>(k_ra8_epub_toc_none));
  assert(book.toc_path[0] == '\0');
  /* The cover-image property still resolves the cover via the EPUB 3 path. */
  assert(std::strcmp(book.cover_path, "cover.png") == 0);

  std::printf("ok\n");
}

/**
 * @test test_mcdc_collect_fonts_href_and_type
 *
 * @par MC/DC:
 * Decision: ``if (href == nullptr || !media_type_is_font(item->Attribute(
 * "media-type")))`` inside ``collect_font_items`` (xml_shim.cpp line 379;
 * 2 conditions). Driven through ``ra8_epub_xml_parse_opf``.
 *  - V1: a font <item> (href present, font media-type) -> C1=F,C2=F.
 *    Decision F (record the font). embedded_font_count incremented.
 *  - V2: an <item> with no `href` attribute             -> C1=T short.
 *    Decision T (skip). Isolates C1 (V1 vs V2).
 *  - V3: a non-font <item> (href present, xhtml type)   -> C1=F,C2=T.
 *    Decision T (skip). Isolates C2 (V1 vs V3); the chapter item provides
 *    this vector.
 * Exactly one font href is recorded.
 */
void test_mcdc_collect_fonts_href_and_type(void)
{
  std::printf("test_mcdc_collect_fonts_href_and_type: ");

  ra8_epub_book_t book = {};
  const auto*     xml  = reinterpret_cast<const uint8_t*>(k_opf_font_and_no_href);
  assert(ra8_epub_xml_parse_opf(xml, std::strlen(k_opf_font_and_no_href), &book) == k_ra8_ok);
  assert(book.embedded_font_count == 1U);
  assert(std::strcmp(book.embedded_font_paths[0], "fonts/serif.ttf") == 0);

  std::printf("ok\n");
}

/**
 * @test test_mcdc_nav_by_type_untyped_first
 *
 * @par MC/DC:
 * Decision: ``if (attr != nullptr && std::strstr(attr, type) != nullptr)``
 * inside ``find_nav_by_type`` (xml_shim.cpp line 434; 2 conditions). Driven
 * through ``ra8_epub_xml_parse_nav``.
 *  - V1 (k_nav_doc landmarks): epub:type="landmarks" -> C1=T,C2=F. Decision
 *    F (keep searching). Covered by test_parse_ncx_and_nav.
 *  - V2 (k_nav_doc toc): epub:type="toc"             -> C1=T,C2=T. Decision
 *    T (return nav). Covered by test_parse_ncx_and_nav.
 *  - V3 (here): first <nav> has no epub:type          -> C1=F short.
 *    Decision F (keep searching). Isolates C1 (V2 vs V3). The
 *    epub:type="toc" nav that follows is still selected.
 */
void test_mcdc_nav_by_type_untyped_first(void)
{
  std::printf("test_mcdc_nav_by_type_untyped_first: ");

  ra8_epub_book_t book = {};
  const auto*     nav  = reinterpret_cast<const uint8_t*>(k_nav_untyped_first);
  assert(ra8_epub_xml_parse_nav(nav, std::strlen(k_nav_untyped_first), &book) == k_ra8_ok);
  assert(book.toc_count == 1U);
  assert(std::strcmp(book.toc[0].title, "Chapter One") == 0);
  assert(std::strcmp(book.toc[0].href, "c1.xhtml") == 0);

  std::printf("ok\n");
}

/**
 * @test test_mcdc_ncx_nav_book_null
 *
 * @par MC/DC:
 * Completes the entry-guard decisions of the two TOC parsers.
 *
 * Decision A: ``if (xml_bytes == nullptr || book == nullptr)`` in
 * ``ra8_epub_xml_parse_ncx`` (xml_shim.cpp line 610; 2 conditions).
 *  - V1 (valid call): xml=valid, book=valid -> C1=F,C2=F. Covered by
 *    test_parse_ncx_and_nav.
 *  - V2 (existing guard test): xml=NULL      -> C1=T short. Covered by
 *    test_parse_toc_guards.
 *  - V3 (here): xml=valid, book=NULL          -> C1=F,C2=T. Decision T
 *    (null_ptr). Isolates C2 (V1 vs V3).
 *
 * Decision B: ``if (xml_bytes == nullptr || book == nullptr)`` in
 * ``ra8_epub_xml_parse_nav`` (xml_shim.cpp line 638; 2 conditions).
 *  - V1 (valid call): xml=valid, book=valid -> C1=F,C2=F. Covered.
 *  - V2 (existing guard test): xml=NULL      -> C1=T short. Covered.
 *  - V3 (here): xml=valid, book=NULL          -> C1=F,C2=T. Decision T
 *    (null_ptr). Isolates C2 (V1 vs V3).
 */
void test_mcdc_ncx_nav_book_null(void)
{
  std::printf("test_mcdc_ncx_nav_book_null: ");

  const auto* ncx = reinterpret_cast<const uint8_t*>(k_ncx_doc);
  assert(ra8_epub_xml_parse_ncx(ncx, std::strlen(k_ncx_doc), nullptr) == k_ra8_err_null_ptr);

  const auto* nav = reinterpret_cast<const uint8_t*>(k_nav_doc);
  assert(ra8_epub_xml_parse_nav(nav, std::strlen(k_nav_doc), nullptr) == k_ra8_err_null_ptr);

  std::printf("ok\n");
}

} // namespace

int main(void)
{
  test_mcdc_parse_container_null_or_zero();
  test_mcdc_parse_opf_null_and_manifest_spine();
  test_parse_ncx_and_nav();
  test_parse_opf_toc_source();
  test_parse_toc_guards();
  test_mcdc_copy_bounded_truncation();
  test_mcdc_manifest_item_missing_id();
  test_mcdc_cover_by_meta();
  test_mcdc_nav_manifest_props_not_nav();
  test_mcdc_collect_fonts_href_and_type();
  test_mcdc_nav_by_type_untyped_first();
  test_mcdc_ncx_nav_book_null();
  (void)std::fprintf(stderr, "[OK ] test_ra8_epub_xml_shim.cpp\n");
  return 0;
}
