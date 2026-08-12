/**
 * @file test_ra8_epub_xml_shim_cov.cpp
 * @brief White-box line-coverage tests for libs/ra8_epub/src/ra8_epub_xml_shim.cpp.
 *
 * @par Tag
 * [Ring 4 / EPUB] {World: NS}
 *
 * @details
 * The MC/DC vector suite in `test_ra8_epub_xml_shim.cpp` drives the four
 * public C-linkage entry points and lands the reachable short-circuit
 * decisions. What it cannot reach are the TU-local (anonymous-namespace)
 * helpers' defensive guards: every public entry point validates its
 * pointers up front, so `copy_bounded`, `find_descendant`, `find_child`,
 * `manifest_href_by_id`, `find_cover_by_properties`, `find_cover_by_meta`,
 * `elem_local_is`, `find_nav_manifest_href`, `media_type_is_font`,
 * `collect_font_items`, and `collect_manifest_items` are only ever invoked
 * with non-NULL arguments through the public surface -- their NULL / empty
 * guard arms, their static caps, and the `find_identifier` matcher (no
 * fixture in the public suite ships a `<dc:identifier>`) stay dark.
 *
 * To reach that logic this TU compiles a private instrumented copy of the
 * shim: it `#include`s the C++ source with the four exported symbols
 * renamed (`ra8_epub_xml_parse_*` -> `ra8_epub_xml_parse_*_cov`) so they do
 * not collide with the production copy linked from `ra8_core_hal`, then
 * calls the anonymous-namespace helpers directly with the edge inputs the
 * public API filters out. Only the wire is faked -- the real production
 * logic runs line-by-line; no line is bypassed by an exclusion marker.
 * `tinyxml2` itself is linked from `ra8_core_hal` (only the header is
 * included here), so parsing is the genuine vendored parser.
 *
 * The same white-box `#include`-the-source pattern is used by
 * `test_ra8_usb_hmsc_enum_cov.c`.
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
#include <string>

/* Rename the four extern "C" entry points so the instrumented copy does not
 * clash with the production `ra8_epub_xml_parse_*` linked from ra8_core_hal.
 * Both the declarations (top of the TU) and the definitions are renamed
 * consistently by these macros. The macro name on each left-hand side is
 * dictated by the symbol it renames, so it cannot take the UPPER_CASE
 * spelling readability-identifier-naming would otherwise demand. */
// NOLINTBEGIN(readability-identifier-naming) -- names fixed by the renamed symbols
#define ra8_epub_xml_parse_container ra8_epub_xml_parse_container_cov
#define ra8_epub_xml_parse_opf       ra8_epub_xml_parse_opf_cov
#define ra8_epub_xml_parse_ncx       ra8_epub_xml_parse_ncx_cov
#define ra8_epub_xml_parse_nav       ra8_epub_xml_parse_nav_cov
// NOLINTEND(readability-identifier-naming)

/* White-box: pull the shim source (and its anonymous-namespace helpers) into
 * this TU so the defensive guards can be driven directly. tinyxml2's symbols
 * resolve from ra8_core_hal (only the header is included transitively). */
#include "ra8_epub_xml_shim.cpp" // NOLINT(bugprone-suspicious-include) -- white-box copy

namespace {

using tinyxml2::XMLDocument;
using tinyxml2::XMLElement;

/* -----------------------------------------------------------------------
 * Small parse helpers: the direct-call tests need real tinyxml2 element
 * pointers to feed the anonymous-namespace helpers.
 * -----------------------------------------------------------------------
 */

/** @brief Root element of a parsed document, or nullptr on a parse error. */
const XMLElement* root_of(XMLDocument& doc, const char* xml)
{
  return (doc.Parse(xml, std::strlen(xml)) == tinyxml2::XML_SUCCESS) ? doc.RootElement() : nullptr;
}

/**
 * @test test_cov_copy_bounded_guards
 *
 * @par MC/DC:
 * Decision: `if (dst == nullptr || cap == 0U)` inside `copy_bounded`
 * (xml_shim.cpp; 2 conditions). Called directly because every public entry
 * point only ever hands `copy_bounded` a real destination buffer.
 *  - V1: dst=valid, cap>0  -> C1=F,C2=F. Decision F (proceed, copy runs).
 *  - V2: dst=NULL,  cap>0  -> C1=T short. Decision T (return, no write).
 *  - V3: dst=valid, cap=0  -> C1=F,C2=T. Decision T (return, no write).
 * V1+V2 isolate C1; V1+V3 isolate C2. The `src == nullptr` arm (a separate
 * single-condition guard) is exercised too: it must leave `dst[0] == '\0'`.
 */
void test_cov_copy_bounded_guards(void)
{
  std::printf("test_cov_copy_bounded_guards: ");

  char buf[8] = {};

  /* V1: normal copy -- both conditions false, loop runs to the NUL. */
  copy_bounded(buf, sizeof buf, "hello");
  assert(std::strcmp(buf, "hello") == 0);

  /* V2: dst NULL -> early return, no crash. */
  copy_bounded(nullptr, sizeof buf, "x");

  /* V3: cap 0 -> early return, destination left untouched. */
  buf[0] = 'Z';
  copy_bounded(buf, 0U, "x");
  assert(buf[0] == 'Z');

  /* src NULL guard: writes an empty string. */
  copy_bounded(buf, sizeof buf, nullptr);
  assert(buf[0] == '\0');

  std::printf("ok\n");
}

/**
 * @test test_cov_find_null_guards
 *
 * @par MC/DC:
 * Two structurally identical 2-condition guards, each driven directly:
 *  - `if (root == nullptr || local_name == nullptr)` in `find_descendant`.
 *  - `if (parent == nullptr || local_name == nullptr)` in `find_child`.
 * For each: V1 both non-NULL -> F,F (found/searched, covered by the public
 * suite); V2 first arg NULL -> C1=T short -> nullptr; V3 name NULL ->
 * C1=F,C2=T -> nullptr. V1+V2 isolate the pointer condition, V1+V3 isolate
 * the name condition. The public suite supplies V1 for both helpers.
 */
void test_cov_find_null_guards(void)
{
  std::printf("test_cov_find_null_guards: ");

  XMLDocument       doc;
  const XMLElement* root = root_of(doc, "<r><a/><b/></r>");
  assert(root != nullptr);

  /* find_descendant: NULL root, then NULL name. */
  assert(find_descendant(nullptr, "a") == nullptr);
  assert(find_descendant(root, nullptr) == nullptr);
  /* Positive control: a real descendant still resolves. */
  assert(find_descendant(root, "a") != nullptr);

  /* find_child: NULL parent, then NULL name. */
  assert(find_child(nullptr, "a") == nullptr);
  assert(find_child(root, nullptr) == nullptr);
  assert(find_child(root, "b") != nullptr);

  std::printf("ok\n");
}

/**
 * @test test_cov_lookup_null_guards
 *
 * @par MC/DC:
 * The manifest / cover / nav lookup helpers each open with a NULL guard the
 * public API can never trip (parse_opf validates `manifest`/`metadata`
 * first). Each is driven directly:
 *  - `manifest_href_by_id`: `if (manifest == nullptr || id == nullptr)` --
 *    C1=T short (NULL manifest) and C1=F,C2=T (NULL id) both return nullptr;
 *    the public suite supplies the F,F control.
 *  - `find_cover_by_properties(nullptr)` -- single-condition manifest guard.
 *  - `find_cover_by_meta(nullptr, ...)` -- single-condition metadata guard.
 *  - `elem_local_is`: `if (elem == nullptr || local == nullptr)` -- C1=T
 *    (NULL elem) and C1=F,C2=T (NULL local) both return false.
 *  - `find_nav_manifest_href(nullptr)` -- single-condition manifest guard.
 *  - `media_type_is_font(nullptr)` -- single-condition media-type guard.
 */
void test_cov_lookup_null_guards(void)
{
  std::printf("test_cov_lookup_null_guards: ");

  XMLDocument       doc;
  const XMLElement* root = root_of(doc, "<manifest><item id=\"x\" href=\"x.html\"/></manifest>");
  assert(root != nullptr);

  /* manifest_href_by_id: both short-circuit arms. */
  assert(manifest_href_by_id(nullptr, "x") == nullptr);
  assert(manifest_href_by_id(root, nullptr) == nullptr);
  /* Positive control through the non-guard path. */
  assert(manifest_href_by_id(root, "x") != nullptr);

  /* Single-condition NULL guards. */
  assert(find_cover_by_properties(nullptr) == nullptr);
  assert(find_cover_by_meta(nullptr, root) == nullptr);
  assert(find_nav_manifest_href(nullptr) == nullptr);
  assert(!media_type_is_font(nullptr));

  /* elem_local_is: NULL element, then NULL local name. */
  assert(!elem_local_is(nullptr, "item"));
  assert(!elem_local_is(root, nullptr));
  assert(elem_local_is(root, "manifest"));

  std::printf("ok\n");
}

/**
 * @test test_cov_collect_guards_and_caps
 *
 * @par MC/DC:
 * `collect_font_items` / `collect_manifest_items` each open with a
 * single-condition `manifest == nullptr` guard (they zero the count first,
 * then bail) and each has a single-condition static-cap guard:
 *  - NULL manifest -> count stays 0 (guard true).
 *  - A manifest with more items than the cap -> the cap guard fires and the
 *    stored count clamps to the cap (::k_ra8_epub_max_fonts /
 *    ::k_ra8_epub_max_manifest).
 *  - `collect_manifest_items` also skips non-`<item>` children
 *    (`if (!elem_local_is(item, "item")) continue;`).
 * `toc_emit`'s cap guard (`toc_count >= k_ra8_epub_max_toc`) is driven by
 * pre-seeding the count to the cap and asserting the emit is dropped.
 */
/* Font cap: feeding more font items than the cap clamps the count. */
void cov_check_font_cap(void)
{
  std::string xml = "<manifest>";
  for (int i = 0; i < static_cast<int>(k_ra8_epub_max_fonts) + 3; ++i) {
    xml += "<item id=\"f";
    xml += std::to_string(i);
    xml += "\" href=\"f";
    xml += std::to_string(i);
    xml += ".ttf\" media-type=\"font/ttf\"/>";
  }
  xml += "</manifest>";
  XMLDocument       doc;
  const XMLElement* manifest = root_of(doc, xml.c_str());
  assert(manifest != nullptr);
  ra8_epub_book_t book = {};
  collect_font_items(manifest, &book);
  assert(book.embedded_font_count == static_cast<uint16_t>(k_ra8_epub_max_fonts));
}

/* Manifest cap + non-<item> child skip: one stray element then an over-cap
 * run of items; the stray is skipped, the count clamps. */
void cov_check_manifest_cap(void)
{
  std::string xml = "<manifest><stray/>";
  for (int i = 0; i < static_cast<int>(k_ra8_epub_max_manifest) + 2; ++i) {
    xml += "<item id=\"i";
    xml += std::to_string(i);
    xml += "\" href=\"h";
    xml += std::to_string(i);
    xml += "\" media-type=\"application/xhtml+xml\"/>";
  }
  xml += "</manifest>";
  XMLDocument       doc;
  const XMLElement* manifest = root_of(doc, xml.c_str());
  assert(manifest != nullptr);
  ra8_epub_book_t book = {};
  collect_manifest_items(manifest, &book);
  assert(book.manifest_count == static_cast<uint16_t>(k_ra8_epub_max_manifest));
}

void test_cov_collect_guards_and_caps(void)
{
  std::printf("test_cov_collect_guards_and_caps: ");

  ra8_epub_book_t book = {};

  /* NULL manifest guards -> counts pinned at zero. */
  collect_font_items(nullptr, &book);
  assert(book.embedded_font_count == 0U);
  collect_manifest_items(nullptr, &book);
  assert(book.manifest_count == 0U);

  cov_check_font_cap();
  cov_check_manifest_cap();

  /* toc_emit cap: pre-seed the count to the cap, the emit is dropped. */
  std::memset(&book, 0, sizeof(book));
  book.toc_count = static_cast<uint16_t>(k_ra8_epub_max_toc);
  toc_emit(&book, "dropped", "dropped.xhtml", 0U);
  assert(book.toc_count == static_cast<uint16_t>(k_ra8_epub_max_toc));

  std::printf("ok\n");
}

/* -----------------------------------------------------------------------
 * find_identifier fixtures: no fixture in the public MC/DC suite ships a
 * <dc:identifier>, so the whole matcher (id/uid compare + first-fallback)
 * is dark. Each OPF below is well-formed (manifest + spine) so parse_opf
 * reaches the identifier copy and returns k_ra8_ok.
 * -----------------------------------------------------------------------
 */

/* Two identifiers; the second's id matches the package unique-identifier,
 * so find_identifier returns it. A leading <dc:title> exercises the
 * non-identifier `continue`. */
constexpr const char* k_opf_id_match =
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
  "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"pub-id\">"
  "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
  "<dc:title>T</dc:title>"
  "<dc:identifier id=\"other\">ID-OTHER</dc:identifier>"
  "<dc:identifier id=\"pub-id\">ID-MATCH</dc:identifier>"
  "</metadata>"
  "<manifest><item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/></manifest>"
  "<spine><itemref idref=\"c1\"/></spine></package>";

/* A single identifier with no `id` attribute; find_identifier falls back to
 * the first identifier's text. */
constexpr const char* k_opf_id_no_attr =
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
  "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"pub-id\">"
  "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
  "<dc:identifier>ID-NOATTR</dc:identifier>"
  "</metadata>"
  "<manifest><item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/></manifest>"
  "<spine><itemref idref=\"c1\"/></spine></package>";

/* No package unique-identifier attribute at all, so `uid == nullptr`;
 * find_identifier falls back to the first identifier's text. */
constexpr const char* k_opf_no_uid =
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
  "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\">"
  "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
  "<dc:identifier id=\"x\">ID-NOUID</dc:identifier>"
  "</metadata>"
  "<manifest><item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/></manifest>"
  "<spine><itemref idref=\"c1\"/></spine></package>";

/**
 * @test test_cov_find_identifier_variants
 *
 * @par MC/DC:
 * Covers `find_identifier`'s sequential guards (no compound `&&`/`||`):
 *  - `if (!elem_local_is(m, "identifier")) continue;` -- the leading
 *    `<dc:title>` in k_opf_id_match takes the true (skip) arm; identifiers
 *    take the false arm.
 *  - `if (first == nullptr) first = m;` -- true on the first identifier,
 *    false on the second (k_opf_id_match).
 *  - `if (id == nullptr) continue;` -- true for the attribute-less
 *    identifier (k_opf_id_no_attr).
 *  - `if (uid == nullptr) continue;` -- true when the package omits
 *    unique-identifier (k_opf_no_uid).
 *  - `if (std::strcmp(id, uid) == 0) return m->GetText();` -- false for the
 *    non-matching id, true for the matching one (k_opf_id_match).
 *  - the `first != nullptr ? first->GetText() : nullptr` fallback -- taken
 *    by both fallback fixtures.
 */
void test_cov_find_identifier_variants(void)
{
  std::printf("test_cov_find_identifier_variants: ");

  ra8_epub_book_t book = {};

  const auto* match = reinterpret_cast<const uint8_t*>(k_opf_id_match);
  assert(ra8_epub_xml_parse_opf_cov(match, std::strlen(k_opf_id_match), &book) == k_ra8_ok);
  assert(std::strcmp(book.identifier, "ID-MATCH") == 0);

  std::memset(&book, 0, sizeof(book));
  const auto* noattr = reinterpret_cast<const uint8_t*>(k_opf_id_no_attr);
  assert(ra8_epub_xml_parse_opf_cov(noattr, std::strlen(k_opf_id_no_attr), &book) == k_ra8_ok);
  assert(std::strcmp(book.identifier, "ID-NOATTR") == 0);

  std::memset(&book, 0, sizeof(book));
  const auto* nouid = reinterpret_cast<const uint8_t*>(k_opf_no_uid);
  assert(ra8_epub_xml_parse_opf_cov(nouid, std::strlen(k_opf_no_uid), &book) == k_ra8_ok);
  assert(std::strcmp(book.identifier, "ID-NOUID") == 0);

  std::printf("ok\n");
}

/**
 * @test test_cov_entry_error_paths
 *
 * @par MC/DC:
 * Each of the four entry points guards on three sequential single-condition
 * checks after the (already-covered) NULL-pointer decision: `xml_len == 0`,
 * the tinyxml2 parse-result, and `root == nullptr`. No compound decision.
 *  - zero length -> k_ra8_err_invalid_size.
 *  - malformed XML (mismatched tags) -> parse fails -> k_ra8_err_validation_failed.
 *  - a comment-only document parses but exposes no root element ->
 *    k_ra8_err_validation_failed. (Should tinyxml2 instead treat it as a
 *    parse error, the return is identical, so the assertion is stable.)
 */
void test_cov_entry_error_paths(void)
{
  std::printf("test_cov_entry_error_paths: ");

  const auto*                 nonnull = reinterpret_cast<const uint8_t*>("x");
  ra8_epub_container_result_t res     = {};
  ra8_epub_book_t             book    = {};

  /* Zero length -> invalid_size on every entry point. */
  assert(ra8_epub_xml_parse_container_cov(nonnull, 0U, &res) == k_ra8_err_invalid_size);
  assert(ra8_epub_xml_parse_opf_cov(nonnull, 0U, &book) == k_ra8_err_invalid_size);
  assert(ra8_epub_xml_parse_ncx_cov(nonnull, 0U, &book) == k_ra8_err_invalid_size);
  assert(ra8_epub_xml_parse_nav_cov(nonnull, 0U, &book) == k_ra8_err_invalid_size);

  /* Malformed XML -> parse failure. */
  constexpr const char* k_bad = "<a><b></c>";
  const auto*           bad   = reinterpret_cast<const uint8_t*>(k_bad);
  assert(ra8_epub_xml_parse_container_cov(bad, std::strlen(k_bad), &res) ==
         k_ra8_err_validation_failed);
  assert(ra8_epub_xml_parse_opf_cov(bad, std::strlen(k_bad), &book) == k_ra8_err_validation_failed);
  assert(ra8_epub_xml_parse_ncx_cov(bad, std::strlen(k_bad), &book) == k_ra8_err_validation_failed);
  assert(ra8_epub_xml_parse_nav_cov(bad, std::strlen(k_bad), &book) == k_ra8_err_validation_failed);

  /* Comment only -> parses, but no root element. */
  constexpr const char* k_noroot = "<!-- only a comment -->";
  const auto*           noroot   = reinterpret_cast<const uint8_t*>(k_noroot);
  assert(ra8_epub_xml_parse_container_cov(noroot, std::strlen(k_noroot), &res) ==
         k_ra8_err_validation_failed);
  assert(ra8_epub_xml_parse_opf_cov(noroot, std::strlen(k_noroot), &book) ==
         k_ra8_err_validation_failed);
  assert(ra8_epub_xml_parse_ncx_cov(noroot, std::strlen(k_noroot), &book) ==
         k_ra8_err_validation_failed);
  assert(ra8_epub_xml_parse_nav_cov(noroot, std::strlen(k_noroot), &book) ==
         k_ra8_err_validation_failed);

  std::printf("ok\n");
}

/* Spine with an idref-less itemref, an unresolvable idref, then a good one:
 * exercises the `idref == nullptr` and `href == nullptr` continue arms of the
 * parse_opf spine loop. */
constexpr const char* k_opf_spine_edges =
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
  "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\"><metadata/>"
  "<manifest><item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/></manifest>"
  "<spine><itemref/><itemref idref=\"ghost\"/><itemref idref=\"c1\"/></spine></package>";

/* NCX whose first navPoint carries neither <navLabel> nor <content>:
 * exercises the (label == nullptr), (text == nullptr) and (content == nullptr)
 * ternary arms of ncx_walk. */
constexpr const char* k_ncx_bare_navpoint =
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
  "<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\">"
  "<navMap>"
  "<navPoint id=\"bare\"/>"
  "<navPoint id=\"np1\"><navLabel><text>Chapter One</text></navLabel>"
  "<content src=\"c1.xhtml\"/></navPoint>"
  "</navMap></ncx>";

/* nav.xhtml whose first <li> has a <span> label but no <a>: exercises the
 * (anchor == nullptr) fallback to find_child(li, "span") and the
 * (anchor == nullptr) href arm of nav_walk. */
constexpr const char* k_nav_span_li =
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
  "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\">"
  "<body><nav epub:type=\"toc\"><ol>"
  "<li><span>Span Label</span></li>"
  "<li><a href=\"c1.xhtml\">Chapter One</a></li>"
  "</ol></nav></body></html>";

/**
 * @test test_cov_spine_and_nav_edges
 *
 * @par MC/DC:
 * All arms below are sequential single-condition guards / ternaries (no
 * compound decision):
 *  - parse_opf spine loop: `if (idref == nullptr) continue;` (idref-less
 *    itemref) and `if (href == nullptr) continue;` (unresolvable idref); the
 *    trailing valid itemref resolves, so exactly one chapter is stored.
 *  - parse_opf chapter cap: a spine longer than ::k_ra8_epub_max_chapters
 *    returns k_ra8_err_no_mem.
 *  - ncx_walk ternaries: a bare navPoint drives the (label/text/content ==
 *    nullptr) arms, emitting an empty-title/empty-href entry.
 *  - nav_walk: a <li> with a <span> and no <a> drives the anchor-NULL label
 *    fallback and the anchor-NULL href arm.
 */
void test_cov_spine_and_nav_edges(void)
{
  std::printf("test_cov_spine_and_nav_edges: ");

  ra8_epub_book_t book = {};

  /* Spine edges: only the last itemref resolves. */
  const auto* spine = reinterpret_cast<const uint8_t*>(k_opf_spine_edges);
  assert(ra8_epub_xml_parse_opf_cov(spine, std::strlen(k_opf_spine_edges), &book) == k_ra8_ok);
  assert(book.chapter_count == 1U);
  assert(std::strcmp(book.chapter_paths[0], "c1.xhtml") == 0);

  /* Chapter cap: a spine of (cap + 1) itemrefs all resolving to one item
   * trips the k_ra8_epub_max_chapters guard. */
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
    const auto* over = reinterpret_cast<const uint8_t*>(xml.c_str());
    assert(ra8_epub_xml_parse_opf_cov(over, xml.size(), &book) == k_ra8_err_no_mem);
  }

  /* NCX bare navPoint -> empty first entry, real second entry. */
  std::memset(&book, 0, sizeof(book));
  const auto* ncx = reinterpret_cast<const uint8_t*>(k_ncx_bare_navpoint);
  assert(ra8_epub_xml_parse_ncx_cov(ncx, std::strlen(k_ncx_bare_navpoint), &book) == k_ra8_ok);
  assert(book.toc_count == 2U);
  assert(book.toc[0].title[0] == '\0');
  assert(book.toc[0].href[0] == '\0');
  assert(std::strcmp(book.toc[1].title, "Chapter One") == 0);

  /* nav <li> with a <span> label and no <a>. */
  std::memset(&book, 0, sizeof(book));
  const auto* nav = reinterpret_cast<const uint8_t*>(k_nav_span_li);
  assert(ra8_epub_xml_parse_nav_cov(nav, std::strlen(k_nav_span_li), &book) == k_ra8_ok);
  assert(book.toc_count == 2U);
  assert(std::strcmp(book.toc[0].title, "Span Label") == 0);
  assert(book.toc[0].href[0] == '\0');
  assert(std::strcmp(book.toc[1].title, "Chapter One") == 0);

  std::printf("ok\n");
}

/* Nested NCX: a top-level <navPoint> ("Chapter One") whose only child is a
 * childless nested <navPoint> ("Section 1.1") with no navPoint sibling,
 * followed by a top-level sibling ("Chapter Two"). Descending into the
 * childless nested navPoint and finding no sibling forces ncx_walk's inner
 * backtrack loop to run with depth > base_depth. */
constexpr const char* k_ncx_nested_backtrack =
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
  "<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\">"
  "<navMap>"
  "<navPoint id=\"p1\"><navLabel><text>Chapter One</text></navLabel>"
  "<content src=\"c1.xhtml\"/>"
  "<navPoint id=\"p1a\"><navLabel><text>Section 1.1</text></navLabel>"
  "<content src=\"c1.xhtml#s1\"/></navPoint>"
  "</navPoint>"
  "<navPoint id=\"p2\"><navLabel><text>Chapter Two</text></navLabel>"
  "<content src=\"c2.xhtml\"/></navPoint>"
  "</navMap></ncx>";

/* Nested nav.xhtml mirroring k_ncx_nested_backtrack: a top-level <li>
 * ("Chapter One") carrying a nested <ol> whose single <li> ("Section 1.1")
 * has no <li> sibling, followed by a top-level sibling <li> ("Chapter Two").
 * The childless nested <li> forces nav_walk's two-level backtrack loop to run
 * with depth > base_depth. */
constexpr const char* k_nav_nested_backtrack =
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
  "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\">"
  "<body><nav epub:type=\"toc\"><ol>"
  "<li><a href=\"c1.xhtml\">Chapter One</a>"
  "<ol><li><a href=\"c1.xhtml#s1\">Section 1.1</a></li></ol></li>"
  "<li><a href=\"c2.xhtml\">Chapter Two</a></li>"
  "</ol></nav></body></html>";

/* Shared 3-entry expectation for the nested TOC fixtures above. */
void cov_assert_nested_toc(const ra8_epub_book_t& book)
{
  assert(book.toc_count == 3U);
  assert(std::strcmp(book.toc[0].title, "Chapter One") == 0);
  assert(book.toc[0].depth == 0U);
  assert(std::strcmp(book.toc[1].title, "Section 1.1") == 0);
  assert(book.toc[1].depth == 1U);
  assert(std::strcmp(book.toc[2].title, "Chapter Two") == 0);
  assert(book.toc[2].depth == 0U);
}

/**
 * @test test_cov_nested_toc_backtrack
 *
 * @par MC/DC:
 * Completes the two-condition backtrack guards that #438's recursion-to-
 * iteration refactor introduced, driven through the white-box `_cov` entry
 * points so THIS translation unit's instantiation of ncx_walk / nav_walk
 * (compiled from the `#include`d source) reaches them. The production
 * instantiation is already covered by the nested fixtures in
 * test_ra8_epub_xml_shim.cpp; the `_cov` copy's existing flat fixtures never
 * descend a level, leaving its backtrack loops at 0% MC/DC.
 *
 * Decision A: `while (sibling == nullptr && depth > base_depth)` in ncx_walk
 * (2 conditions), via ra8_epub_xml_parse_ncx_cov(k_ncx_nested_backtrack).
 *  - V1: after descending into the childless nested navPoint (depth 1) with no
 *    navPoint sibling -> C1=T (sibling NULL), C2=T (1 > 0). Decision T (enter,
 *    backtrack one level).
 *  - V2: the nested navPoint's parent ("Chapter One") has a following navPoint
 *    sibling ("Chapter Two") -> C1=F short. Decision F (stop backtracking).
 *  - V3: the final top-level navPoint ("Chapter Two") has no sibling at base
 *    depth -> C1=T, C2=F (0 > 0 is false). Decision F (walk complete).
 * V1+V2 isolate C1 (sibling); V1+V3 isolate C2 (depth). N+1 = 3 vectors.
 *
 * Decision B: `while (sibling == nullptr && depth > base_depth)` in nav_walk
 * (2 conditions), via ra8_epub_xml_parse_nav_cov(k_nav_nested_backtrack) over
 * the identical nested shape, landing the same V1/V2/V3 legs on the two-level
 * (`<li>` -> `<ol>` -> parent `<li>`) backtrack.
 */
void test_cov_nested_toc_backtrack(void)
{
  std::printf("test_cov_nested_toc_backtrack: ");

  ra8_epub_book_t book = {};
  const auto*     ncx  = reinterpret_cast<const uint8_t*>(k_ncx_nested_backtrack);
  assert(ra8_epub_xml_parse_ncx_cov(ncx, std::strlen(k_ncx_nested_backtrack), &book) == k_ra8_ok);
  cov_assert_nested_toc(book);

  std::memset(&book, 0, sizeof(book));
  const auto* nav = reinterpret_cast<const uint8_t*>(k_nav_nested_backtrack);
  assert(ra8_epub_xml_parse_nav_cov(nav, std::strlen(k_nav_nested_backtrack), &book) == k_ra8_ok);
  cov_assert_nested_toc(book);

  std::printf("ok\n");
}

/**
 * @test test_cov_entry_and_structural_guards
 *
 * @par MC/DC:
 * Completes the compound decisions in the whitebox copy of the four entry points
 * plus parse_opf's manifest/spine and parse_container's full-path guards. The
 * (false,false) control legs are supplied by the parse-success tests
 * (test_cov_find_identifier_variants, and the container success below); this test
 * adds the missing short-circuit and second-condition-true legs. N+1 vectors each.
 *  - `xml_bytes == nullptr || out/book == nullptr` (parse_container / _opf / _ncx
 *    / _nav): V2 xml_bytes NULL -> C1 true short; V3 out/book NULL -> C1 false,
 *    C2 true. Both return null_ptr.
 *  - `manifest == nullptr || spine == nullptr` (parse_opf): C1 true (no
 *    `<manifest>`) and C1 false, C2 true (manifest present, no `<spine>`); both
 *    return validation_failed.
 *  - `full_path == nullptr || full_path[0] == '\0'` (parse_container): C1 true
 *    (rootfile without full-path) and C1 false, C2 true (empty full-path); both
 *    return validation_failed, with a valid full-path as the F,F control.
 */
void test_cov_entry_and_structural_guards(void)
{
  std::printf("test_cov_entry_and_structural_guards: ");

  const auto*                 nn   = reinterpret_cast<const uint8_t*>("<x/>");
  ra8_epub_container_result_t res  = {};
  ra8_epub_book_t             book = {};

  /* Entry-point NULL guards: C1-true (arg1 NULL) and C1-false/C2-true (out NULL). */
  assert(ra8_epub_xml_parse_container_cov(nullptr, 4U, &res) == k_ra8_err_null_ptr);
  assert(ra8_epub_xml_parse_container_cov(nn, 4U, nullptr) == k_ra8_err_null_ptr);
  assert(ra8_epub_xml_parse_opf_cov(nullptr, 4U, &book) == k_ra8_err_null_ptr);
  assert(ra8_epub_xml_parse_opf_cov(nn, 4U, nullptr) == k_ra8_err_null_ptr);
  assert(ra8_epub_xml_parse_ncx_cov(nullptr, 4U, &book) == k_ra8_err_null_ptr);
  assert(ra8_epub_xml_parse_ncx_cov(nn, 4U, nullptr) == k_ra8_err_null_ptr);
  assert(ra8_epub_xml_parse_nav_cov(nullptr, 4U, &book) == k_ra8_err_null_ptr);
  assert(ra8_epub_xml_parse_nav_cov(nn, 4U, nullptr) == k_ra8_err_null_ptr);

  /* parse_container full-path guard: F,F control, then C1-true and C2-true. */
  constexpr const char* k_ok_rootfile =
    "<container><rootfile full-path=\"OEBPS/x.opf\"/></container>";
  assert(ra8_epub_xml_parse_container_cov(reinterpret_cast<const uint8_t*>(k_ok_rootfile),
                                          std::strlen(k_ok_rootfile),
                                          &res) == k_ra8_ok);
  assert(std::strcmp(res.opf_path, "OEBPS/x.opf") == 0);
  constexpr const char* k_no_fullpath = "<container><rootfile/></container>";
  assert(ra8_epub_xml_parse_container_cov(reinterpret_cast<const uint8_t*>(k_no_fullpath),
                                          std::strlen(k_no_fullpath),
                                          &res) == k_ra8_err_validation_failed);
  constexpr const char* k_empty_fullpath = "<container><rootfile full-path=\"\"/></container>";
  assert(ra8_epub_xml_parse_container_cov(reinterpret_cast<const uint8_t*>(k_empty_fullpath),
                                          std::strlen(k_empty_fullpath),
                                          &res) == k_ra8_err_validation_failed);

  /* parse_opf manifest/spine guard: C1-true (no manifest), then C1-false/C2-true
   * (manifest present, no spine). */
  constexpr const char* k_no_manifest =
    "<package xmlns=\"http://www.idpf.org/2007/opf\"><metadata/><spine/></package>";
  assert(ra8_epub_xml_parse_opf_cov(reinterpret_cast<const uint8_t*>(k_no_manifest),
                                    std::strlen(k_no_manifest),
                                    &book) == k_ra8_err_validation_failed);
  constexpr const char* k_no_spine =
    "<package xmlns=\"http://www.idpf.org/2007/opf\"><manifest/></package>";
  assert(ra8_epub_xml_parse_opf_cov(reinterpret_cast<const uint8_t*>(k_no_spine),
                                    std::strlen(k_no_spine),
                                    &book) == k_ra8_err_validation_failed);

  std::printf("ok\n");
}

/**
 * @test test_cov_compound_helper_legs
 *
 * @par MC/DC:
 * Completes the two-condition guards in the manifest-scanning helpers, each
 * driven directly with one fixture that walks all N+1 legs:
 *  - `props != nullptr && std::strstr(props, "nav")` (find_nav_manifest_href):
 *    a no-properties item (C1 false), a `properties="scripted"` item (C1 true,
 *    C2 false), and a `properties="nav"` item (C1 true, C2 true -> returned).
 *  - `attr != nullptr && std::strstr(attr, type)` (find_nav_by_type): a nav with
 *    no `epub:type` (C1 false), one typed "landmarks" (C1 true, C2 false), and
 *    one typed "toc" (C1 true, C2 true -> returned).
 *  - `href == nullptr || !media_type_is_font(...)` (collect_font_items): an item
 *    with no href (C1 true), an href with a non-font media-type (C1 false, C2
 *    true), and an href with a font media-type (C1 false, C2 false -> collected).
 */
void test_cov_compound_helper_legs(void)
{
  std::printf("test_cov_compound_helper_legs: ");

  /* find_nav_manifest_href: props NULL / props without "nav" / props "nav". */
  XMLDocument       d1;
  const XMLElement* m1 = root_of(d1,
                                 "<manifest><item href=\"a\"/>"
                                 "<item href=\"b\" properties=\"scripted\"/>"
                                 "<item href=\"c\" properties=\"nav\"/></manifest>");
  assert(m1 != nullptr);
  const char* nav_href = find_nav_manifest_href(m1);
  assert(nav_href != nullptr && std::strcmp(nav_href, "c") == 0);

  /* find_nav_by_type: nav without epub:type / typed non-toc / typed toc. */
  XMLDocument       d2;
  const XMLElement* r2 = root_of(d2,
                                 "<root><nav/><nav epub:type=\"landmarks\"/>"
                                 "<nav epub:type=\"toc\"><ol/></nav></root>");
  assert(r2 != nullptr);
  const XMLElement* nav = find_nav_by_type(r2, "toc");
  assert(nav != nullptr);

  /* collect_font_items: no href / href + non-font / href + font. */
  XMLDocument       d3;
  const XMLElement* m3 = root_of(d3,
                                 "<manifest><item media-type=\"font/ttf\"/>"
                                 "<item href=\"s.css\" media-type=\"text/css\"/>"
                                 "<item href=\"f.ttf\" media-type=\"font/ttf\"/></manifest>");
  assert(m3 != nullptr);
  ra8_epub_book_t book = {};
  collect_font_items(m3, &book);
  assert(book.embedded_font_count == 1U);
  assert(std::strcmp(book.embedded_font_paths[0], "f.ttf") == 0);

  std::printf("ok\n");
}

} // namespace

int main(void)
{
  test_cov_copy_bounded_guards();
  test_cov_find_null_guards();
  test_cov_lookup_null_guards();
  test_cov_collect_guards_and_caps();
  test_cov_find_identifier_variants();
  test_cov_entry_error_paths();
  test_cov_spine_and_nav_edges();
  test_cov_nested_toc_backtrack();
  test_cov_entry_and_structural_guards();
  test_cov_compound_helper_legs();
  (void)std::fprintf(stderr, "[OK ] test_ra8_epub_xml_shim_cov.cpp\n");
  return 0;
}
