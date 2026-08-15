/**
 * @file test_ra8_epub_xml_shim_cov2.cpp
 * @brief Complementary line-coverage tests for libs/ra8_epub/src/ra8_epub_xml_shim.c.
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
 * bounded XML reader never yields a named element with a null `Name()`; they are left
 * uncovered rather than excluded (the file clears the 90% floor without them).
 *
 * The prototypes below mirror the internal cross-TU declarations in
 * `ra8_epub_open.c` (the shim entry points are not exposed in a public header).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "ra8_epub.h"
#include "ra8_epub_xml_shim_internal.h"

namespace {

static ra8_epub_xml_workspace_t s_xml_workspace = {};

/** @brief Provide the file-local parse container test helper. @details Implements the parse container fixture operation used only by this focused test executable. @param[in] bytes Fixture argument governed by the exercised interface contract. @param[in] length Fixture argument governed by the exercised interface contract. @param[out] result Fixture argument governed by the exercised interface contract. @return RA8 status from the exercised fixture operation. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static ra8_err_t
internal_parse_container(const uint8_t* bytes, size_t length, ra8_epub_container_result_t* result)
{
  return priv_ra8_epub_xml_parse_container(bytes, length, result, &s_xml_workspace);
}

/** @brief Route container parsing through the test-owned XML workspace wrapper. */
#define priv_ra8_epub_xml_parse_container internal_parse_container

/** @brief Reinterpret a C-string literal as the shim's byte-pointer input. */
RA8_INTERNAL static const uint8_t* internal_bytes_of(const char* s)
{
  return reinterpret_cast<const uint8_t*>(s);
}

/**
 * @test internal_test_cov2_container_error_paths
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
 * (invalid_size), the bounded-reader parse result, `root == nullptr` (comment-only
 * document), and `rootfile == nullptr` (a root with no rootfile element). @brief Verify cov2 container error paths behavior. @details Executes the cov2 container error paths scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_cov2_container_error_paths(void)
{

  ra8_epub_container_result_t res = {};

  /* Decision A, V2 / V3: the two short-circuit arms of the NULL guard. */
  assert(priv_ra8_epub_xml_parse_container(nullptr, 1U, &res) == k_ra8_err_null_ptr);
  assert(priv_ra8_epub_xml_parse_container(internal_bytes_of("x"), 1U, nullptr) ==
         k_ra8_err_null_ptr);

  /* Zero length -> invalid_size. */
  assert(priv_ra8_epub_xml_parse_container(internal_bytes_of("x"), 0U, &res) ==
         k_ra8_err_invalid_size);

  /* Comment-only document -> parses, but exposes no root element. */
  constexpr const char* k_comment = "<!-- only a comment -->";
  assert(priv_ra8_epub_xml_parse_container(internal_bytes_of(k_comment),
                                           std::strlen(k_comment),
                                           &res) == k_ra8_err_validation_failed);

  /* A root with neither <rootfiles> nor <rootfile> -> rootfile NULL. */
  constexpr const char* k_no_rootfile = "<container><other/></container>";
  assert(priv_ra8_epub_xml_parse_container(internal_bytes_of(k_no_rootfile),
                                           std::strlen(k_no_rootfile),
                                           &res) == k_ra8_err_validation_failed);

  /* Decision B, V2: <rootfile> present but no full-path attribute. */
  constexpr const char* k_no_attr = "<container><rootfiles><rootfile/></rootfiles></container>";
  assert(priv_ra8_epub_xml_parse_container(internal_bytes_of(k_no_attr),
                                           std::strlen(k_no_attr),
                                           &res) == k_ra8_err_validation_failed);

  /* Decision B, V3: full-path present but empty. */
  constexpr const char* k_empty_attr =
    "<container><rootfiles><rootfile full-path=\"\"/></rootfiles></container>";
  assert(priv_ra8_epub_xml_parse_container(internal_bytes_of(k_empty_attr),
                                           std::strlen(k_empty_attr),
                                           &res) == k_ra8_err_validation_failed);
}

/**
 * @test internal_test_cov2_container_success
 *
 * @par MC/DC:
 * Both fixtures take the success path (Decision A V1 and Decision B V1 from
 * `internal_test_cov2_container_error_paths`). They also cover the sequential
 * `rootfiles`-resolution ternary and the `find_descendant` deep-recurse return:
 *  - Nested `<rootfiles>` under a wrapper element: the top-level scan misses it,
 *    so `find_descendant` recurses and returns the grandchild match (the
 *    `if (deeper != nullptr) return deeper;` leg), then `find_child` resolves
 *    the `<rootfile>` (the `rootfiles != nullptr` ternary arm).
 *  - A direct `<rootfile>` child with no `<rootfiles>` wrapper: `find_descendant`
 *    for `rootfiles` returns NULL, so the ternary falls back to
 *    `find_descendant(root, "rootfile")`. @brief Verify cov2 container success behavior. @details Executes the cov2 container success scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_cov2_container_success(void)
{

  ra8_epub_container_result_t res = {};

  /* Nested rootfiles -> deep-recurse return + rootfiles-present ternary arm. */
  constexpr const char* k_nested = "<container><wrap><rootfiles>"
                                   "<rootfile full-path=\"OEBPS/content.opf\"/>"
                                   "</rootfiles></wrap></container>";
  assert(priv_ra8_epub_xml_parse_container(internal_bytes_of(k_nested),
                                           std::strlen(k_nested),
                                           &res) == k_ra8_ok);
  assert(std::strcmp(res.opf_path, "OEBPS/content.opf") == 0);

  /* Direct rootfile, no rootfiles wrapper -> fallback ternary arm. */
  std::memset(&res, 0, sizeof(res));
  constexpr const char* k_direct =
    "<container><rootfile full-path=\"OEBPS/direct.opf\"/></container>";
  assert(priv_ra8_epub_xml_parse_container(internal_bytes_of(k_direct),
                                           std::strlen(k_direct),
                                           &res) == k_ra8_ok);
  assert(std::strcmp(res.opf_path, "OEBPS/direct.opf") == 0);
}

/* An OPF with NO properties="cover-image" item but a legacy
 * <meta name="cover" content="ghostid"/> whose id is absent from the manifest:
 * find_cover_by_properties returns NULL, so find_cover_by_meta runs, matches the
 * cover meta, then manifest_href_by_id fails to resolve "ghostid" (href NULL) and
 * the matcher falls through its inner block. The spine mixes an idref-less
 * itemref, an unresolvable idref, and a good one so the two spine `continue`
 * legs both fire. */
constexpr const char* s_opf_cover_meta_spine_edges =
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
  "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\"><metadata>"
  "<meta name=\"cover\" content=\"ghostid\"/></metadata>"
  "<manifest><item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/></manifest>"
  "<spine><itemref/><itemref idref=\"ghost\"/><itemref idref=\"c1\"/></spine></package>";

/**
 * @test internal_test_cov2_opf_edges
 *
 * @par MC/DC:
 * Sequential single-condition legs only (no compound `&&`/`||` in the arms
 * exercised here):
 *  - `if (xml_len == 0U)` -> invalid_size.
 *  - the bounded-reader parse result / `package == nullptr` guard (comment-only
 *    document) -> validation_failed.
 *  - spine loop `if (idref == nullptr) continue;` (idref-less itemref) and
 *    `if (href == nullptr) continue;` (unresolvable idref); the trailing valid
 *    itemref resolves, so exactly one chapter is stored.
 *  - chapter cap `if (count >= k_ra8_epub_max_chapters) return k_ra8_err_no_mem;`
 *    driven by a spine longer than the cap.
 * The legacy-cover matcher's inner fall-through (cover id unresolved) is also
 * taken, leaving `cover_path` empty. @brief Verify cov2 opf edges behavior. @details Executes the cov2 opf edges scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_cov2_opf_edges(void)
{

  ra8_epub_book_t book = {};

  /* Zero length -> invalid_size. */
  assert(priv_ra8_epub_xml_parse_opf(internal_bytes_of("x"), 0U, &book) == k_ra8_err_invalid_size);

  /* Comment-only document -> no package root element. */
  constexpr const char* k_comment = "<!-- only a comment -->";
  assert(priv_ra8_epub_xml_parse_opf(internal_bytes_of(k_comment), std::strlen(k_comment), &book) ==
         k_ra8_err_validation_failed);

  /* Legacy cover meta with an unresolved id + spine skip legs. */
  std::memset(&book, 0, sizeof(book));
  assert(priv_ra8_epub_xml_parse_opf(internal_bytes_of(s_opf_cover_meta_spine_edges),
                                     std::strlen(s_opf_cover_meta_spine_edges),
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
    assert(priv_ra8_epub_xml_parse_opf(internal_bytes_of(xml.c_str()), xml.size(), &book) ==
           k_ra8_err_no_mem);
  }
}

/**
 * @test internal_test_cov2_ncx_edges
 *
 * @par MC/DC:
 * Sequential single-condition legs of `priv_ra8_epub_xml_parse_ncx`:
 *  - the bounded-reader parse result guard (malformed, mismatched tags) ->
 *    validation_failed.
 *  - `root == nullptr` (comment-only document) -> validation_failed.
 * The NULL-pointer and zero-length legs are already covered by the MC/DC suite,
 * so this test targets the parse / root legs specifically. @brief Verify cov2 ncx edges behavior. @details Executes the cov2 ncx edges scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_cov2_ncx_edges(void)
{

  ra8_epub_book_t book = {};

  constexpr const char* k_bad = "<a><b></c>";
  assert(priv_ra8_epub_xml_parse_ncx(internal_bytes_of(k_bad), std::strlen(k_bad), &book) ==
         k_ra8_err_validation_failed);

  constexpr const char* k_comment = "<!-- only a comment -->";
  assert(priv_ra8_epub_xml_parse_ncx(internal_bytes_of(k_comment), std::strlen(k_comment), &book) ==
         k_ra8_err_validation_failed);
}

/* A nav document whose <ol> leads with a stray non-<li> element before a real
 * <li>: nav_walk skips the stray (the `if (!elem_local_is(li, "li")) continue;`
 * leg) then emits the one real entry. */
constexpr const char* s_nav_stray_then_li =
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
  "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\">"
  "<body><nav epub:type=\"toc\"><ol>"
  "<stray/>"
  "<li><a href=\"c1.xhtml\">Chapter One</a></li>"
  "</ol></nav></body></html>";

/* A nav element with the toc type but no <ol> child: find_child(nav, "ol")
 * returns NULL and the parser reports validation_failed. */
constexpr const char* s_nav_no_ol =
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
  "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\">"
  "<body><nav epub:type=\"toc\"></nav></body></html>";

/**
 * @test internal_test_cov2_nav_edges
 *
 * @par MC/DC:
 * Sequential single-condition legs of `priv_ra8_epub_xml_parse_nav`:
 *  - `if (xml_len == 0U)` -> invalid_size.
 *  - the bounded-reader parse result guard (malformed) -> validation_failed.
 *  - `root == nullptr` (comment-only) -> validation_failed.
 *  - `ordered_list == nullptr` (a <nav> with no <ol>) -> validation_failed.
 *  - nav_walk `if (!elem_local_is(li, "li")) continue;` (a stray non-<li> child
 *    ahead of a real <li>), leaving exactly one TOC entry. @brief Verify cov2 nav edges behavior. @details Executes the cov2 nav edges scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_cov2_nav_edges(void)
{

  ra8_epub_book_t book = {};

  /* Zero length -> invalid_size. */
  assert(priv_ra8_epub_xml_parse_nav(internal_bytes_of("x"), 0U, &book) == k_ra8_err_invalid_size);

  /* Malformed XML -> parse failure. */
  constexpr const char* k_bad = "<a><b></c>";
  assert(priv_ra8_epub_xml_parse_nav(internal_bytes_of(k_bad), std::strlen(k_bad), &book) ==
         k_ra8_err_validation_failed);

  /* Comment-only -> no root element. */
  constexpr const char* k_comment = "<!-- only a comment -->";
  assert(priv_ra8_epub_xml_parse_nav(internal_bytes_of(k_comment), std::strlen(k_comment), &book) ==
         k_ra8_err_validation_failed);

  /* toc nav present but with no <ol> -> validation_failed. */
  std::memset(&book, 0, sizeof(book));
  assert(priv_ra8_epub_xml_parse_nav(internal_bytes_of(s_nav_no_ol),
                                     std::strlen(s_nav_no_ol),
                                     &book) == k_ra8_err_validation_failed);

  /* Stray non-<li> child in the <ol> is skipped; one real entry remains. */
  std::memset(&book, 0, sizeof(book));
  assert(priv_ra8_epub_xml_parse_nav(internal_bytes_of(s_nav_stray_then_li),
                                     std::strlen(s_nav_stray_then_li),
                                     &book) == k_ra8_ok);
  assert(book.toc_count == 1U);
  assert(std::strcmp(book.toc[0].title, "Chapter One") == 0);
  assert(std::strcmp(book.toc[0].href, "c1.xhtml") == 0);
}

} /* namespace */

/** @brief Run the focused test cases in this executable. @details Invokes each isolated case once and returns the accumulated assertion status. @return Process status from the accumulated assertions. @retval 0 Every focused assertion passed. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
int main(void)
{
  internal_test_cov2_container_error_paths();
  internal_test_cov2_container_success();
  internal_test_cov2_opf_edges();
  internal_test_cov2_ncx_edges();
  internal_test_cov2_nav_edges();
  return 0;
}
