/**
 * @file test_ra8_epub_xml_shim_cov2.cpp
 * @brief Complementary line-coverage tests for libs/ra8_epub/src/ra8_epub_xml_shim.c.
 *
 * @par Tag
 * [Ring 4 / EPUB] {World: NS}
 *
 * @details
 * Complements ``test_ra8_epub_xml_shim.cpp`` with black-box malformed-input,
 * missing-element, fallback, and capacity edges against the genuine production
 * symbols in ``ra8_core_hal``. It never includes or renames the implementation.
 * Both executables therefore exercise the same pure-C bounded pull-reader
 * consumer that ships in firmware.
 *
 * The private entry points are declared by
 * ``ra8_epub_xml_shim_internal.h``. Every fixture is immutable and every parse
 * uses caller-owned ``ra8_epub_xml_workspace_t`` state.
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

// NOLINTBEGIN(misc-use-anonymous-namespace) -- the project's RA8_INTERNAL
// contract (scripts/checks/annot_walk.py) requires the declaration to spell
// literal `static`; an anonymous namespace's internal linkage is a distinct
// mechanism and does not satisfy that contract.
static ra8_epub_xml_workspace_t s_xml_workspace = {};

/** @brief Reinterpret a C-string literal as the shim's byte-pointer input. */
RA8_INTERNAL static const uint8_t* internal_bytes_of(const char* s)
{
  return reinterpret_cast<const uint8_t*>(s);
}

/**
 * @test internal_test_cov2_container_error_paths
 *
 * @par MC/DC:
 * Decision A: `xml_bytes == nullptr || out == nullptr || workspace == nullptr`.
 *  - V1: xml_bytes=valid, out=valid -> C1=F,C2=F. Decision F (proceed; the
 *        success test below supplies this control vector).
 *  - V2: xml_bytes=NULL,  out=valid -> C1=T short. Decision T -> null_ptr.
 *  - V3: xml_bytes=valid, out=NULL  -> C1=F,C2=T. Decision T -> null_ptr.
 * V1+V2 isolate C1; V1+V3 isolate C2. The companion suite independently
 * selects C3 with a null workspace.
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
 * The remaining legs cover zero length, validation failure, absent rootfile,
 * and a rootfile nested below (rather than directly under) rootfiles. @brief Verify cov2 container error paths behavior. @details Executes the cov2 container error paths scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_cov2_container_error_paths(void)
{

  ra8_epub_container_result_t res = {};

  /* Decision A, V2 / V3: the two short-circuit arms of the NULL guard. */
  assert(priv_ra8_epub_xml_parse_container(nullptr, 1U, &res, &s_xml_workspace) ==
         k_ra8_err_null_ptr);
  assert(priv_ra8_epub_xml_parse_container(internal_bytes_of("x"), 1U, nullptr, &s_xml_workspace) ==
         k_ra8_err_null_ptr);

  /* Zero length -> invalid_size. */
  assert(priv_ra8_epub_xml_parse_container(internal_bytes_of("x"), 0U, &res, &s_xml_workspace) ==
         k_ra8_err_invalid_size);

  /* Comment-only document -> parses, but exposes no root element. */
  constexpr const char* k_comment = "<!-- only a comment -->";
  assert(priv_ra8_epub_xml_parse_container(internal_bytes_of(k_comment),
                                           std::strlen(k_comment),
                                           &res,
                                           &s_xml_workspace) == k_ra8_err_validation_failed);

  /* A root with neither <rootfiles> nor <rootfile> -> rootfile NULL. */
  constexpr const char* k_no_rootfile = "<container><other/></container>";
  assert(priv_ra8_epub_xml_parse_container(internal_bytes_of(k_no_rootfile),
                                           std::strlen(k_no_rootfile),
                                           &res,
                                           &s_xml_workspace) == k_ra8_err_validation_failed);

  /* A selected rootfiles only accepts a direct rootfile child. */
  constexpr const char* k_nested_rootfile =
    "<container><rootfiles><wrap><rootfile full-path='bad.opf'/></wrap>"
    "</rootfiles></container>";
  assert(priv_ra8_epub_xml_parse_container(internal_bytes_of(k_nested_rootfile),
                                           std::strlen(k_nested_rootfile),
                                           &res,
                                           &s_xml_workspace) == k_ra8_err_validation_failed);

  /* Decision B, V2: <rootfile> present but no full-path attribute. */
  constexpr const char* k_no_attr = "<container><rootfiles><rootfile/></rootfiles></container>";
  assert(priv_ra8_epub_xml_parse_container(internal_bytes_of(k_no_attr),
                                           std::strlen(k_no_attr),
                                           &res,
                                           &s_xml_workspace) == k_ra8_err_validation_failed);

  /* Decision B, V3: full-path present but empty. */
  constexpr const char* k_empty_attr =
    "<container><rootfiles><rootfile full-path=\"\"/></rootfiles></container>";
  assert(priv_ra8_epub_xml_parse_container(internal_bytes_of(k_empty_attr),
                                           std::strlen(k_empty_attr),
                                           &res,
                                           &s_xml_workspace) == k_ra8_err_validation_failed);
}

/**
 * @test internal_test_cov2_container_success
 *
 * @par MC/DC:
 * Both fixtures take the success path. A nested rootfiles element exercises the
 * global selection followed by parent-scoped direct-child lookup. A direct
 * rootfile with no rootfiles wrapper exercises the defined global fallback. @brief Verify cov2 container success behavior. @details Executes the cov2 container success scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_cov2_container_success(void)
{

  ra8_epub_container_result_t res = {};

  /* Nested rootfiles -> deep-recurse return + rootfiles-present ternary arm. */
  constexpr const char* k_nested = "<container><wrap><rootfiles>"
                                   "<rootfile full-path=\"OEBPS/content.opf\"/>"
                                   "</rootfiles></wrap></container>";
  assert(priv_ra8_epub_xml_parse_container(internal_bytes_of(k_nested),
                                           std::strlen(k_nested),
                                           &res,
                                           &s_xml_workspace) == k_ra8_ok);
  assert(std::strcmp(res.opf_path, "OEBPS/content.opf") == 0);

  /* Direct rootfile, no rootfiles wrapper -> fallback ternary arm. */
  std::memset(&res, 0, sizeof(res));
  constexpr const char* k_direct =
    "<container><rootfile full-path=\"OEBPS/direct.opf\"/></container>";
  assert(priv_ra8_epub_xml_parse_container(internal_bytes_of(k_direct),
                                           std::strlen(k_direct),
                                           &res,
                                           &s_xml_workspace) == k_ra8_ok);
  assert(std::strcmp(res.opf_path, "OEBPS/direct.opf") == 0);
}

/* An OPF with a legacy cover id absent from the manifest. The spine mixes an
 * idref-less itemref, an unresolvable idref, and a good one. */
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
 *  - bounded validation and OPF-shape rejection for a comment-only document.
 *  - idref-less and unresolvable spine entries are skipped; the trailing valid
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
 *  - element lookup on a comment-only document -> validation_failed.
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

/* A nav document whose ordered list has a stray element before one real li. */
constexpr const char* s_nav_stray_then_li =
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
  "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\">"
  "<body><nav epub:type=\"toc\"><ol>"
  "<stray/>"
  "<li><a href=\"c1.xhtml\">Chapter One</a></li>"
  "</ol></nav></body></html>";

/* A typed toc nav with no direct ordered-list child is invalid. */
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
 *  - selection on a comment-only document -> validation_failed.
 *  - a selected nav with no direct ordered-list child -> validation_failed.
 *  - a stray element ahead of a real li is ignored, leaving one TOC entry. @brief Verify cov2 nav edges behavior. @details Executes the cov2 nav edges scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
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

/* A container whose rootfiles wrapper is preceded by a sibling at the same
 * depth, and by a global rootfile that the parent-scoped search must skip. */
constexpr const char* s_container_sibling_first =
  "<container><other/><rootfiles><rootfile full-path='x.opf'/></rootfiles></container>";
constexpr const char* s_container_global_first =
  "<container><rootfile full-path='a.opf'/><rootfiles>"
  "<rootfile full-path='b.opf'/></rootfiles></container>";

/* An OPF whose package carries no unique-identifier, whose metadata holds an
 * identifier with and without an id, whose spine precedes its manifest, and
 * which nests a non-item child under manifest and a grandchild under item. */
constexpr const char* s_opf_operand_matrix =
  "<?xml version=\"1.0\"?>"
  "<package version=\"3.0\">"
  "<metadata><dc:identifier>urn:a</dc:identifier>"
  "<dc:identifier id=\"q\">urn:b</dc:identifier></metadata>"
  "<spine><other/><itemref idref=\"c1\"/></spine>"
  "<manifest><other2/>"
  "<item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"><child/></item>"
  "</manifest>"
  "<guide/>"
  "</package>";

/* An NCX whose navMap carries a label and a content element that no navPoint
 * owns, ahead of one well-formed navPoint. */
constexpr const char* s_ncx_unowned_label =
  "<ncx><navMap>"
  "<navLabel><text>Top</text></navLabel>"
  "<content src=\"top.xhtml\"/>"
  "<navPoint><navLabel><text>One</text></navLabel><content src=\"c1.xhtml\"/></navPoint>"
  "</navMap></ncx>";

/* A nav whose only ordered list sits one level too deep to be its child. */
constexpr const char* s_nav_indirect_ol = "<html><body><nav epub:type=\"toc\"><div><ol>"
                                          "<li><a href=\"c1.xhtml\">One</a></li>"
                                          "</ol></div></nav></body></html>";

/* A nav list holding an unowned anchor, an entry whose span text follows an
 * anchor that already supplied a title, and an entry titled only by CDATA. */
constexpr const char* s_nav_operand_matrix = "<html><body><nav epub:type=\"toc\"><ol>"
                                             "<a href=\"x.xhtml\">Stray</a>"
                                             "<li><a href=\"c1.xhtml\">First<b>x</b>Second</a></li>"
                                             "<li><span>Only</span></li>"
                                             "<li><a href=\"c3.xhtml\"><![CDATA[raw]]></a></li>"
                                             "</ol></nav></body></html>";

/**
 * @test internal_test_cov2_operand_matrix
 *
 * @par MC/DC:
 * Container: a sibling at the parent's own depth varies the markup-identity
 * operand of the parent-selection guard, and a globally-earlier rootfile
 * varies the parent-active operand of the acceptance guard, both against the
 * accepted control.
 * OPF: an identifier without an id and an identifier with an id under a
 * package that declares no unique-identifier independently vary the first two
 * operands of the package-identifier match; a depth-one `guide` varies the
 * name operand of the spine selector; a grandchild under `item` varies the
 * direct-child operand of both the metadata and manifest arms; a non-item
 * direct child of manifest varies the item-name operand; a manifest that
 * follows the spine varies the frame-name operand of the spine-child arms;
 * and a non-itemref direct child of spine varies their name operands.
 * NCX: a content element and a navLabel that no navPoint owns independently
 * vary the marker operand of the href and title stores.
 * Nav: an ordered list one level too deep varies the direct-child operand of
 * the list probe; an anchor directly inside the list varies the marker operand
 * of the anchor store and of the title store; anchor text that follows a
 * nested element varies the empty-title operand; a span-titled entry varies
 * the span-name operand; and CDATA inside an anchor varies the event-kind
 * operand of the closing arm.
 * Decisions:
 * - libs/ra8_epub/src/ra8_epub_xml_shim.c@priv_ra8_epub_xml_find
 * - libs/ra8_epub/src/ra8_epub_xml_shim.c@internal_mark_metadata
 * - libs/ra8_epub/src/ra8_epub_xml_shim.c@internal_opf_first
 * - libs/ra8_epub/src/ra8_epub_xml_shim.c@internal_opf_first_event
 * - libs/ra8_epub/src/ra8_epub_xml_shim.c@internal_collect_spine
 * - libs/ra8_epub/src/ra8_epub_xml_shim.c@internal_opf_shape
 * - libs/ra8_epub/src/ra8_epub_xml_toc.c@internal_ncx_event
 * - libs/ra8_epub/src/ra8_epub_xml_toc.c@internal_nav_has_list
 * - libs/ra8_epub/src/ra8_epub_xml_toc.c@internal_nav_event
 * - libs/ra8_epub/src/ra8_epub_xml_toc.c@internal_nav_event_start
 *
 * @brief Vary the remaining container, OPF, NCX, and nav consumer operands.
 * @details Drives one fixture per uncovered operand through the production
 * private entry points, asserting the committed metadata, spine, and TOC state
 * each fixture must produce.
 * @pre Fixed-capacity fixture storage required by this operation is available.
 * @pre Arguments follow the interface contract exercised by this helper.
 * @post Documented outputs contain the exercised result when the operation
 * succeeds.
 * @post Mutations remain confined to documented outputs and file-local fixture
 * state.
 * @note File-local helper; no ownership escapes this focused test executable.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_test_cov2_operand_matrix(void)
{
  ra8_epub_container_result_t res = {};
  assert(priv_ra8_epub_xml_parse_container(internal_bytes_of(s_container_sibling_first),
                                           std::strlen(s_container_sibling_first),
                                           &res,
                                           &s_xml_workspace) == k_ra8_ok);
  assert(std::strcmp(res.opf_path, "x.opf") == 0);

  std::memset(&res, 0, sizeof(res));
  assert(priv_ra8_epub_xml_parse_container(internal_bytes_of(s_container_global_first),
                                           std::strlen(s_container_global_first),
                                           &res,
                                           &s_xml_workspace) == k_ra8_ok);
  assert(std::strcmp(res.opf_path, "b.opf") == 0);

  static ra8_epub_book_t s_book;
  std::memset(&s_book, 0, sizeof(s_book));
  assert(priv_ra8_epub_xml_parse_opf(internal_bytes_of(s_opf_operand_matrix),
                                     std::strlen(s_opf_operand_matrix),
                                     &s_book) == k_ra8_ok);
  assert(s_book.chapter_count == 1U);
  assert(std::strcmp(s_book.chapter_paths[0], "c1.xhtml") == 0);
  assert(std::strcmp(s_book.identifier, "urn:a") == 0);

  std::memset(&s_book, 0, sizeof(s_book));
  assert(priv_ra8_epub_xml_parse_ncx(internal_bytes_of(s_ncx_unowned_label),
                                     std::strlen(s_ncx_unowned_label),
                                     &s_book) == k_ra8_ok);
  assert(s_book.toc_count == 1U);
  assert(std::strcmp(s_book.toc[0].title, "One") == 0);
  assert(std::strcmp(s_book.toc[0].href, "c1.xhtml") == 0);

  std::memset(&s_book, 0, sizeof(s_book));
  assert(priv_ra8_epub_xml_parse_nav(internal_bytes_of(s_nav_indirect_ol),
                                     std::strlen(s_nav_indirect_ol),
                                     &s_book) == k_ra8_err_validation_failed);

  std::memset(&s_book, 0, sizeof(s_book));
  assert(priv_ra8_epub_xml_parse_nav(internal_bytes_of(s_nav_operand_matrix),
                                     std::strlen(s_nav_operand_matrix),
                                     &s_book) == k_ra8_ok);
  assert(s_book.toc_count == 3U);
  assert(std::strcmp(s_book.toc[0].title, "First") == 0);
  assert(std::strcmp(s_book.toc[0].href, "c1.xhtml") == 0);
  assert(std::strcmp(s_book.toc[1].title, "Only") == 0);
  assert(std::strcmp(s_book.toc[2].href, "c3.xhtml") == 0);
}
// NOLINTEND(misc-use-anonymous-namespace)

/** @brief Run the focused test cases in this executable. @details Invokes each isolated case once and returns the accumulated assertion status. @return Process status from the accumulated assertions. @retval 0 Every focused assertion passed. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
int main(void)
{
  internal_test_cov2_container_error_paths();
  internal_test_cov2_container_success();
  internal_test_cov2_opf_edges();
  internal_test_cov2_ncx_edges();
  internal_test_cov2_nav_edges();
  internal_test_cov2_operand_matrix();
  return 0;
}
