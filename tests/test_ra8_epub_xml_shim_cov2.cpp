/**
 * @file test_ra8_epub_xml_shim_cov2.cpp
 * @brief Complementary line-coverage tests for libs/ra8_epub/src/ra8_epub_xml_shim.cpp.
 *
 * @par Tag
 * [Ring 4 / EPUB] {World: NS}
 *
 * @details
 * This TU completes the aggregate line coverage of the XML shim's four public
 * C-linkage entry points. It is a companion to `test_ra8_epub_xml_shim_cov.cpp`;
 * gcovr unions the two `.gcda` sets against the same source file.
 *
 * Unlike the `_cov` sibling, this TU is deliberately BLACK-BOX: it does NOT
 * `#include` the shim source and does NOT rename the entry points. It links the
 * genuine production symbols out of `ra8_core_hal` (the same object the firmware
 * ships) and drives them with hand-built fixtures. This matters for coverage
 * accounting: when a source file is compiled a second time with the entry
 * points renamed (the `_cov` white-box trick), gcov attributes the renamed
 * function's body lines to a distinct function occupying the same source lines,
 * and gcovr cannot union the entry-point-body lines that only the renamed copy
 * reaches. Calling the real, un-renamed functions here lands those lines in the
 * shared production object, so they merge cleanly into the aggregate.
 *
 * What was dark before this TU: the container / OPF / NCX / nav entry points'
 * length, parse-result, root-null, and (for container) rootfile-resolution
 * legs, plus the OPF spine `idref`/`href` skips, the chapter-count cap, the
 * legacy `<meta name="cover">` fall-through, the `find_descendant` deep-recurse
 * return, and the `nav_walk` non-`<li>` skip. The `name == nullptr` guards in
 * `find_child` / `find_cover_by_meta` / `elem_local_is` stay dark because
 * tinyxml2 never yields a named element with a null `Name()`; they are left
 * uncovered rather than excluded (the file clears the 90% floor without them).
 *
 * The prototypes below mirror the internal cross-TU declarations in
 * `ra8_epub_open.c` (the shim entry points are not exposed in a public header).
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
#include <string>

#include "ra8_epub.h"
#include "ra8_epub_xml_shim_internal.h"

namespace {

/** @brief Reinterpret a C-string literal as the shim's byte-pointer input. */
const uint8_t* bytes_of(const char* s)
{
  return reinterpret_cast<const uint8_t*>(s);
}

/**
 * @test test_cov2_container_error_paths
 *
 * @par MC/DC:
 * Decision A: `if (xml_bytes == nullptr || out == nullptr)` (2 conditions).
 *  - V1: xml_bytes=valid, out=valid -> C1=F,C2=F. Decision F (proceed; the
 *        success test below supplies this control vector).
 *  - V2: xml_bytes=NULL,  out=valid -> C1=T short. Decision T -> null_ptr.
 *  - V3: xml_bytes=valid, out=NULL  -> C1=F,C2=T. Decision T -> null_ptr.
 * V1+V2 isolate C1; V1+V3 isolate C2. N+1 = 3 vectors for N=2: minimal MC/DC.
 *
 * Decision B: `if (full_path == nullptr || full_path[0] == '\0')` (2
 * conditions), reached only once the document parses and a `<rootfile>` is
 * found.
 *  - V1: full_path="OEBPS/c.opf" -> C1=F,C2=F. Decision F (copy + k_ra8_ok;
 *        supplied by the success test).
 *  - V2: no `full-path` attribute  -> C1=T short. Decision T -> validation.
 *  - V3: `full-path=""` (empty)     -> C1=F,C2=T. Decision T -> validation.
 * V1+V2 isolate C1; V1+V3 isolate C2. N+1 = 3 vectors: minimal MC/DC.
 *
 * The remaining legs are sequential single-condition guards: `xml_len == 0`
 * (invalid_size), the tinyxml2 parse-result, `root == nullptr` (comment-only
 * document), and `rootfile == nullptr` (a root with no rootfile element).
 */
void test_cov2_container_error_paths(void)
{
  std::printf("test_cov2_container_error_paths: ");

  ra8_epub_container_result_t res = {};

  /* Decision A, V2 / V3: the two short-circuit arms of the NULL guard. */
  assert(ra8_epub_xml_parse_container(nullptr, 1U, &res) == k_ra8_err_null_ptr);
  assert(ra8_epub_xml_parse_container(bytes_of("x"), 1U, nullptr) == k_ra8_err_null_ptr);

  /* Zero length -> invalid_size. */
  assert(ra8_epub_xml_parse_container(bytes_of("x"), 0U, &res) == k_ra8_err_invalid_size);

  /* Comment-only document -> parses, but exposes no root element. */
  constexpr const char* k_comment = "<!-- only a comment -->";
  assert(ra8_epub_xml_parse_container(bytes_of(k_comment), std::strlen(k_comment), &res) ==
         k_ra8_err_validation_failed);

  /* A root with neither <rootfiles> nor <rootfile> -> rootfile NULL. */
  constexpr const char* k_no_rootfile = "<container><other/></container>";
  assert(ra8_epub_xml_parse_container(bytes_of(k_no_rootfile), std::strlen(k_no_rootfile), &res) ==
         k_ra8_err_validation_failed);

  /* Decision B, V2: <rootfile> present but no full-path attribute. */
  constexpr const char* k_no_attr = "<container><rootfiles><rootfile/></rootfiles></container>";
  assert(ra8_epub_xml_parse_container(bytes_of(k_no_attr), std::strlen(k_no_attr), &res) ==
         k_ra8_err_validation_failed);

  /* Decision B, V3: full-path present but empty. */
  constexpr const char* k_empty_attr =
    "<container><rootfiles><rootfile full-path=\"\"/></rootfiles></container>";
  assert(ra8_epub_xml_parse_container(bytes_of(k_empty_attr), std::strlen(k_empty_attr), &res) ==
         k_ra8_err_validation_failed);

  std::printf("ok\n");
}

/**
 * @test test_cov2_container_success
 *
 * @par MC/DC:
 * Both fixtures take the success path (Decision A V1 and Decision B V1 from
 * `test_cov2_container_error_paths`). They also cover the sequential
 * `rootfiles`-resolution ternary and the `find_descendant` deep-recurse return:
 *  - Nested `<rootfiles>` under a wrapper element: the top-level scan misses it,
 *    so `find_descendant` recurses and returns the grandchild match (the
 *    `if (deeper != nullptr) return deeper;` leg), then `find_child` resolves
 *    the `<rootfile>` (the `rootfiles != nullptr` ternary arm).
 *  - A direct `<rootfile>` child with no `<rootfiles>` wrapper: `find_descendant`
 *    for `rootfiles` returns NULL, so the ternary falls back to
 *    `find_descendant(root, "rootfile")`.
 */
void test_cov2_container_success(void)
{
  std::printf("test_cov2_container_success: ");

  ra8_epub_container_result_t res = {};

  /* Nested rootfiles -> deep-recurse return + rootfiles-present ternary arm. */
  constexpr const char* k_nested = "<container><wrap><rootfiles>"
                                   "<rootfile full-path=\"OEBPS/content.opf\"/>"
                                   "</rootfiles></wrap></container>";
  assert(ra8_epub_xml_parse_container(bytes_of(k_nested), std::strlen(k_nested), &res) == k_ra8_ok);
  assert(std::strcmp(res.opf_path, "OEBPS/content.opf") == 0);

  /* Direct rootfile, no rootfiles wrapper -> fallback ternary arm. */
  std::memset(&res, 0, sizeof(res));
  constexpr const char* k_direct =
    "<container><rootfile full-path=\"OEBPS/direct.opf\"/></container>";
  assert(ra8_epub_xml_parse_container(bytes_of(k_direct), std::strlen(k_direct), &res) == k_ra8_ok);
  assert(std::strcmp(res.opf_path, "OEBPS/direct.opf") == 0);

  std::printf("ok\n");
}

/* An OPF with NO properties="cover-image" item but a legacy
 * <meta name="cover" content="ghostid"/> whose id is absent from the manifest:
 * find_cover_by_properties returns NULL, so find_cover_by_meta runs, matches the
 * cover meta, then manifest_href_by_id fails to resolve "ghostid" (href NULL) and
 * the matcher falls through its inner block. The spine mixes an idref-less
 * itemref, an unresolvable idref, and a good one so the two spine `continue`
 * legs both fire. */
constexpr const char* k_opf_cover_meta_spine_edges =
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
  "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\"><metadata>"
  "<meta name=\"cover\" content=\"ghostid\"/></metadata>"
  "<manifest><item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/></manifest>"
  "<spine><itemref/><itemref idref=\"ghost\"/><itemref idref=\"c1\"/></spine></package>";

/**
 * @test test_cov2_opf_edges
 *
 * @par MC/DC:
 * Sequential single-condition legs only (no compound `&&`/`||` in the arms
 * exercised here):
 *  - `if (xml_len == 0U)` -> invalid_size.
 *  - the tinyxml2 parse-result / `package == nullptr` guard (comment-only
 *    document) -> validation_failed.
 *  - spine loop `if (idref == nullptr) continue;` (idref-less itemref) and
 *    `if (href == nullptr) continue;` (unresolvable idref); the trailing valid
 *    itemref resolves, so exactly one chapter is stored.
 *  - chapter cap `if (count >= k_ra8_epub_max_chapters) return k_ra8_err_no_mem;`
 *    driven by a spine longer than the cap.
 * The legacy-cover matcher's inner fall-through (cover id unresolved) is also
 * taken, leaving `cover_path` empty.
 */
void test_cov2_opf_edges(void)
{
  std::printf("test_cov2_opf_edges: ");

  ra8_epub_book_t book = {};

  /* Zero length -> invalid_size. */
  assert(ra8_epub_xml_parse_opf(bytes_of("x"), 0U, &book) == k_ra8_err_invalid_size);

  /* Comment-only document -> no package root element. */
  constexpr const char* k_comment = "<!-- only a comment -->";
  assert(ra8_epub_xml_parse_opf(bytes_of(k_comment), std::strlen(k_comment), &book) ==
         k_ra8_err_validation_failed);

  /* Legacy cover meta with an unresolved id + spine skip legs. */
  std::memset(&book, 0, sizeof(book));
  assert(ra8_epub_xml_parse_opf(bytes_of(k_opf_cover_meta_spine_edges),
                                std::strlen(k_opf_cover_meta_spine_edges),
                                &book) == k_ra8_ok);
  assert(book.chapter_count == 1U);
  assert(std::strcmp(book.chapter_paths[0], "c1.xhtml") == 0);
  assert(book.cover_path[0] == '\0');

  /* Chapter cap: a spine of (cap + 1) resolving itemrefs -> no_mem. */
  {
    std::string xml =
      "<?xml version=\"1.0\"?>"
      "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\"><metadata/>"
      "<manifest><item id=\"c\" href=\"c.xhtml\" media-type=\"application/xhtml+xml\"/></manifest>"
      "<spine>";
    for (int i = 0; i < static_cast<int>(k_ra8_epub_max_chapters) + 1; ++i) {
      xml += "<itemref idref=\"c\"/>";
    }
    xml += "</spine></package>";
    std::memset(&book, 0, sizeof(book));
    assert(ra8_epub_xml_parse_opf(bytes_of(xml.c_str()), xml.size(), &book) == k_ra8_err_no_mem);
  }

  std::printf("ok\n");
}

/**
 * @test test_cov2_ncx_edges
 *
 * @par MC/DC:
 * Sequential single-condition legs of `ra8_epub_xml_parse_ncx`:
 *  - the tinyxml2 parse-result guard (malformed, mismatched tags) ->
 *    validation_failed.
 *  - `root == nullptr` (comment-only document) -> validation_failed.
 * The NULL-pointer and zero-length legs are already covered by the MC/DC suite,
 * so this test targets the parse / root legs specifically.
 */
void test_cov2_ncx_edges(void)
{
  std::printf("test_cov2_ncx_edges: ");

  ra8_epub_book_t book = {};

  constexpr const char* k_bad = "<a><b></c>";
  assert(ra8_epub_xml_parse_ncx(bytes_of(k_bad), std::strlen(k_bad), &book) ==
         k_ra8_err_validation_failed);

  constexpr const char* k_comment = "<!-- only a comment -->";
  assert(ra8_epub_xml_parse_ncx(bytes_of(k_comment), std::strlen(k_comment), &book) ==
         k_ra8_err_validation_failed);

  std::printf("ok\n");
}

/* A nav document whose <ol> leads with a stray non-<li> element before a real
 * <li>: nav_walk skips the stray (the `if (!elem_local_is(li, "li")) continue;`
 * leg) then emits the one real entry. */
constexpr const char* k_nav_stray_then_li =
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
  "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\">"
  "<body><nav epub:type=\"toc\"><ol>"
  "<stray/>"
  "<li><a href=\"c1.xhtml\">Chapter One</a></li>"
  "</ol></nav></body></html>";

/* A nav element with the toc type but no <ol> child: find_child(nav, "ol")
 * returns NULL and the parser reports validation_failed. */
constexpr const char* k_nav_no_ol =
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
  "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\">"
  "<body><nav epub:type=\"toc\"></nav></body></html>";

/**
 * @test test_cov2_nav_edges
 *
 * @par MC/DC:
 * Sequential single-condition legs of `ra8_epub_xml_parse_nav`:
 *  - `if (xml_len == 0U)` -> invalid_size.
 *  - the tinyxml2 parse-result guard (malformed) -> validation_failed.
 *  - `root == nullptr` (comment-only) -> validation_failed.
 *  - `ordered_list == nullptr` (a <nav> with no <ol>) -> validation_failed.
 *  - nav_walk `if (!elem_local_is(li, "li")) continue;` (a stray non-<li> child
 *    ahead of a real <li>), leaving exactly one TOC entry.
 */
void test_cov2_nav_edges(void)
{
  std::printf("test_cov2_nav_edges: ");

  ra8_epub_book_t book = {};

  /* Zero length -> invalid_size. */
  assert(ra8_epub_xml_parse_nav(bytes_of("x"), 0U, &book) == k_ra8_err_invalid_size);

  /* Malformed XML -> parse failure. */
  constexpr const char* k_bad = "<a><b></c>";
  assert(ra8_epub_xml_parse_nav(bytes_of(k_bad), std::strlen(k_bad), &book) ==
         k_ra8_err_validation_failed);

  /* Comment-only -> no root element. */
  constexpr const char* k_comment = "<!-- only a comment -->";
  assert(ra8_epub_xml_parse_nav(bytes_of(k_comment), std::strlen(k_comment), &book) ==
         k_ra8_err_validation_failed);

  /* toc nav present but with no <ol> -> validation_failed. */
  std::memset(&book, 0, sizeof(book));
  assert(ra8_epub_xml_parse_nav(bytes_of(k_nav_no_ol), std::strlen(k_nav_no_ol), &book) ==
         k_ra8_err_validation_failed);

  /* Stray non-<li> child in the <ol> is skipped; one real entry remains. */
  std::memset(&book, 0, sizeof(book));
  assert(ra8_epub_xml_parse_nav(bytes_of(k_nav_stray_then_li),
                                std::strlen(k_nav_stray_then_li),
                                &book) == k_ra8_ok);
  assert(book.toc_count == 1U);
  assert(std::strcmp(book.toc[0].title, "Chapter One") == 0);
  assert(std::strcmp(book.toc[0].href, "c1.xhtml") == 0);

  std::printf("ok\n");
}

} /* namespace */

int main(void)
{
  test_cov2_container_error_paths();
  test_cov2_container_success();
  test_cov2_opf_edges();
  test_cov2_ncx_edges();
  test_cov2_nav_edges();
  std::fprintf(stderr, "[OK ] test_ra8_epub_xml_shim_cov2.cpp\n");
  return 0;
}
