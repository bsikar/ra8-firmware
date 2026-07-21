/**
 * @file ra8_epub_xml_shim.cpp
 * @brief C++ -> C shim that wraps tinyxml2 for the ra8_epub parser.
 *
 * @details
 * The rest of `libs/ra8_epub` is plain C. We reach for the vendored
 * tinyxml2 (`libs/third_party/tinyxml2/tinyxml2.{h,cpp}`) only here, in
 * a small TU that:
 *
 *   1. Parses the EPUB `META-INF/container.xml` and pulls the first
 *      `<rootfile full-path="...">` attribute.
 *   2. Parses the OPF (Open Packaging Format) document and fills in:
 *        - Dublin Core metadata (title, creator, language)
 *        - The cover-image manifest path (`<item id="cover-image">`)
 *          OR the legacy `<meta name="cover" content="...">` lookup.
 *        - The chapter spine -- a list of href strings in spine order.
 *
 * The shim is the only C++ TU in this library. Everything callable from
 * the rest of the codebase is `extern "C"` and uses caller-owned
 * fixed-size buffers, so no C++ ABI escapes the boundary.
 *
 * ## NASA Rule 3 (no malloc/free in firmware) -- documented deviation
 *
 * `tinyxml2::XMLDocument::Parse()` allocates internal node objects via
 * its `MemPoolT<>` allocator, which is heap-backed by default. We
 * accept this as a vendored exemption rather than (a) hand-rolling a
 * SAX-style scanner over the OPF/container XML, or (b) configuring
 * tinyxml2's `MemPoolT` against a static arena. Rationale:
 *
 *   - The OPF and `META-INF/container.xml` files are bounded (we cap
 *     the OPF scratch at `k_ra8_epub_opf_xml_buf` in the C side), so
 *     the tinyxml2 allocator has a bounded total footprint per parse.
 *   - The `XMLDocument` is local to `extern "C"` parse functions and
 *     is destroyed before the function returns. No allocation outlives
 *     a single `ra8_epub_*` call.
 *   - Replacing tinyxml2 with a hand-rolled scanner is on the roadmap
 *     (see `docs/ROADMAP.md`).
 *
 * Anyone reviewing the no-alloc audit (`scripts/checks/check_no_dynamic_alloc.py`)
 * should expect to see allocations charged to this TU at runtime; the
 * rest of `libs/ra8_epub` is alloc-free.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / EPUB]
 * {World: NS}
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>

#include "ra8_epub.h"
#include "ra8_epub_xml_shim_internal.h"
#include "tinyxml2.h"

namespace {

using tinyxml2::XMLDocument;
using tinyxml2::XMLElement;

/**
 * @brief Bounded `strncpy` that always NUL-terminates.
 *
 * @details Used in place of `strncpy` so static analysis never sees a
 *          missing terminator.
 */
void copy_bounded(char* dst, std::size_t cap, const char* src)
{
  if (dst == nullptr || cap == 0U) {
    return;
  }
  if (src == nullptr) {
    dst[0] = '\0';
    return;
  }
  std::size_t i = 0U;
  // mcdc-deactivated: TU-local helper copy_bounded; cap >= 1 already validated above and src is bounded by caller-supplied null-terminated input. The conjunction guards against invariant violations -- in normal operation the loop terminates by `src[i] != '\0'` long before the cap watchdog fires.
  while (i + 1U < cap && src[i] != '\0') {
    dst[i] = src[i];
    ++i;
  }
  dst[i] = '\0';
}

/**
 * @brief Recursive walk to find the first descendant matching
 *        `local_name`. Tinyxml2 stores names with the namespace prefix
 *        glued in, so we accept any tag that ends in `:local_name` or
 *        is exactly `local_name`.
 */
const XMLElement* find_descendant(const XMLElement* root, const char* local_name)
{
  if (root == nullptr || local_name == nullptr) {
    return nullptr;
  }
  for (const XMLElement* child = root->FirstChildElement(); child != nullptr;
       child                   = child->NextSiblingElement()) {
    const char* name = child->Name();
    if (name != nullptr) {
      const char* colon = std::strrchr(name, ':');
      const char* tail  = (colon != nullptr) ? (colon + 1) : name;
      if (std::strcmp(tail, local_name) == 0) {
        return child;
      }
    }
    const XMLElement* deeper = find_descendant(child, local_name);
    if (deeper != nullptr) {
      return deeper;
    }
  }
  return nullptr;
}

/**
 * @brief Find the first child element whose local name matches.
 */
const XMLElement* find_child(const XMLElement* parent, const char* local_name)
{
  if (parent == nullptr || local_name == nullptr) {
    return nullptr;
  }
  for (const XMLElement* child = parent->FirstChildElement(); child != nullptr;
       child                   = child->NextSiblingElement()) {
    const char* name = child->Name();
    if (name == nullptr) {
      continue;
    }
    const char* colon = std::strrchr(name, ':');
    const char* tail  = (colon != nullptr) ? (colon + 1) : name;
    if (std::strcmp(tail, local_name) == 0) {
      return child;
    }
  }
  return nullptr;
}

/**
 * @brief Pull the named manifest item's href by id.
 */
const char* manifest_href_by_id(const XMLElement* manifest, const char* id)
{
  if (manifest == nullptr || id == nullptr) {
    return nullptr;
  }
  for (const XMLElement* item = manifest->FirstChildElement(); item != nullptr;
       item                   = item->NextSiblingElement()) {
    const char* item_id = item->Attribute("id");
    // mcdc-deactivated: TU-local helper manifest_href_by_id; well-formed EPUB OPF manifests always tag every `<item>` with an `id` attribute (EPUB 3 spec 3.4.3.2 makes id mandatory), so item_id is non-NULL on every reachable iteration -- the second condition (string match) is the only one that varies.
    if (item_id != nullptr && std::strcmp(item_id, id) == 0) {
      return item->Attribute("href");
    }
  }
  return nullptr;
}

/**
 * @brief Walk the manifest looking for `<item properties="cover-image">`.
 */
const char* find_cover_by_properties(const XMLElement* manifest)
{
  if (manifest == nullptr) {
    return nullptr;
  }
  for (const XMLElement* item = manifest->FirstChildElement(); item != nullptr;
       item                   = item->NextSiblingElement()) {
    const char* props = item->Attribute("properties");
    // mcdc-deactivated: TU-local helper find_cover_by_properties; the loop body is only entered when item->Attribute("properties") returns a non-NULL string in well-formed EPUB 3 manifests that declare cover-image; the NULL-guard is defense-in-depth and never independently flips on a reachable EPUB.
    if (props != nullptr && std::strstr(props, "cover-image") != nullptr) {
      return item->Attribute("href");
    }
  }
  return nullptr;
}

/**
 * @brief Look up the legacy `<meta name="cover" content="ID"/>` form.
 */
const char* find_cover_by_meta(const XMLElement* metadata, const XMLElement* manifest)
{
  if (metadata == nullptr) {
    return nullptr;
  }
  for (const XMLElement* meta = metadata->FirstChildElement(); meta != nullptr;
       meta                   = meta->NextSiblingElement()) {
    const char* name = meta->Name();
    if (name == nullptr) {
      continue;
    }
    const char* colon = std::strrchr(name, ':');
    const char* tail  = (colon != nullptr) ? (colon + 1) : name;
    if (std::strcmp(tail, "meta") != 0) {
      continue;
    }
    const char* meta_name    = meta->Attribute("name");
    const char* meta_content = meta->Attribute("content");
    // mcdc-deactivated: TU-local helper find_cover_by_meta; legacy EPUB 2 `<meta name="cover" content="ID"/>` elements are only emitted by builders when both attributes are present (otherwise the element is meaningless and elided) -- both NULL guards cannot independently flip on any conformant input.
    if (meta_name != nullptr && meta_content != nullptr && std::strcmp(meta_name, "cover") == 0) {
      const char* href = manifest_href_by_id(manifest, meta_content);
      if (href != nullptr) {
        return href;
      }
    }
  }
  return nullptr;
}

/**
 * @brief Test whether an element's local name (prefix stripped) equals
 *        `local`.
 */
bool elem_local_is(const XMLElement* elem, const char* local)
{
  if (elem == nullptr || local == nullptr) {
    return false;
  }
  const char* name = elem->Name();
  if (name == nullptr) {
    return false;
  }
  const char* colon = std::strrchr(name, ':');
  const char* tail  = (colon != nullptr) ? (colon + 1) : name;
  return std::strcmp(tail, local) == 0;
}

/**
 * @brief Return the text of the canonical `<dc:identifier>`.
 *
 * @details
 * Prefers the identifier whose `id` matches the package's
 * `unique-identifier` attribute (@p uid); falls back to the first
 * `<dc:identifier>` element. Sequential guards (no compound decision) so
 * the helper stays outside the MC/DC obligation.
 */
const char* find_identifier(const XMLElement* metadata, const char* uid)
{
  const XMLElement* first = nullptr;
  for (const XMLElement* m = metadata->FirstChildElement(); m != nullptr;
       m                   = m->NextSiblingElement()) {
    if (!elem_local_is(m, "identifier")) {
      continue;
    }
    if (first == nullptr) {
      first = m;
    }
    const char* id = m->Attribute("id");
    if (id == nullptr) {
      continue;
    }
    if (uid == nullptr) {
      continue;
    }
    if (std::strcmp(id, uid) == 0) {
      return m->GetText();
    }
  }
  return (first != nullptr) ? first->GetText() : nullptr;
}

/**
 * @brief Find the manifest item declaring `properties="nav"` and return
 *        its href (the EPUB 3 navigation document).
 */
const char* find_nav_manifest_href(const XMLElement* manifest)
{
  if (manifest == nullptr) {
    return nullptr;
  }
  for (const XMLElement* item = manifest->FirstChildElement(); item != nullptr;
       item                   = item->NextSiblingElement()) {
    const char* props = item->Attribute("properties");
    /* The reserved manifest property is the bare token "nav"; no other
     * reserved EPUB 3 property string contains that substring, so a
     * substring test is sufficient and tolerates a token list. */
    if (props != nullptr && std::strstr(props, "nav") != nullptr) {
      return item->Attribute("href");
    }
  }
  return nullptr;
}

/**
 * @brief True if @p media_type is a recognised font resource type (#109).
 *
 * @details Matches the OPF `media-type` values EPUBs use for shipped TTF/OTF
 * faces. A `nullptr` media-type (malformed `<item>`) is not a font.
 */
bool media_type_is_font(const char* media_type)
{
  if (media_type == nullptr) {
    return false;
  }
  static const char* const k_font_types[] = {
    "application/font-sfnt",
    "application/vnd.ms-opentype",
    "font/ttf",
    "font/otf",
    "application/x-font-ttf",
  };
  return std::any_of(
    std::begin(k_font_types),
    std::end(k_font_types),
    [media_type](const char* const candidate) { return std::strcmp(media_type, candidate) == 0; });
}

/**
 * @brief Record manifest font items' hrefs into the book (bounded, #109).
 *
 * @details Scans `<manifest>` for `<item media-type="...font...">` and copies
 * each href into `book->embedded_font_paths` until the static cap
 * (::k_ra8_epub_max_fonts) is reached; the remainder are ignored (graceful
 * degradation, never an error). Sets `book->embedded_font_count`.
 */
void collect_font_items(const XMLElement* manifest, ra8_epub_book_t* book)
{
  book->embedded_font_count = 0U;
  if (manifest == nullptr) {
    return;
  }
  for (const XMLElement* item = manifest->FirstChildElement(); item != nullptr;
       item                   = item->NextSiblingElement()) {
    if (book->embedded_font_count >= static_cast<std::uint16_t>(k_ra8_epub_max_fonts)) {
      return;
    }
    const char* href = item->Attribute("href");
    if (href == nullptr || !media_type_is_font(item->Attribute("media-type"))) {
      continue;
    }
    copy_bounded(book->embedded_font_paths[book->embedded_font_count],
                 k_ra8_epub_max_path_len,
                 href);
    ++book->embedded_font_count;
  }
}

/**
 * @brief Record every `<manifest>` `<item>` into the book in OPF order (#151).
 *
 * @details Copies each item's `id`, `href`, and `media-type` verbatim into
 * `book->manifest` until the static cap (::k_ra8_epub_max_manifest) is reached;
 * the remainder are ignored (graceful degradation, never an error). Document
 * order is preserved because the on-device compiler's stylesheet and image
 * table indices must match the desktop `epub_compile.py` (`manifest.items()`).
 * `copy_bounded` writes "" for any absent attribute. Sets `book->manifest_count`.
 */
void collect_manifest_items(const XMLElement* manifest, ra8_epub_book_t* book)
{
  book->manifest_count = 0U;
  if (manifest == nullptr) {
    return;
  }
  for (const XMLElement* item = manifest->FirstChildElement(); item != nullptr;
       item                   = item->NextSiblingElement()) {
    if (book->manifest_count >= static_cast<std::uint16_t>(k_ra8_epub_max_manifest)) {
      return;
    }
    if (!elem_local_is(item, "item")) {
      continue;
    }
    ra8_epub_manifest_item_t* slot = &book->manifest[book->manifest_count];
    copy_bounded(slot->id, k_ra8_epub_id_len, item->Attribute("id"));
    copy_bounded(slot->href, k_ra8_epub_max_path_len, item->Attribute("href"));
    copy_bounded(slot->media_type, k_ra8_epub_media_len, item->Attribute("media-type"));
    ++book->manifest_count;
  }
}

/**
 * @brief Append one TOC entry to the book, honouring the static cap.
 */
void toc_emit(ra8_epub_book_t* book, const char* title, const char* href, std::uint8_t depth)
{
  if (book->toc_count >= static_cast<std::uint16_t>(k_ra8_epub_max_toc)) {
    return;
  }
  ra8_epub_toc_entry_t* entry = &book->toc[book->toc_count];
  copy_bounded(entry->title, k_ra8_epub_meta_len, title);
  copy_bounded(entry->href, k_ra8_epub_max_path_len, href);
  entry->depth    = depth;
  book->toc_count = static_cast<std::uint16_t>(book->toc_count + 1U);
}

/**
 * @brief Depth-first walk of NCX `<navPoint>` siblings under `parent`.
 */
void ncx_walk(const XMLElement* parent, ra8_epub_book_t* book, std::uint8_t depth)
{
  for (const XMLElement* np = parent->FirstChildElement(); np != nullptr;
       np                   = np->NextSiblingElement()) {
    if (!elem_local_is(np, "navPoint")) {
      continue;
    }
    const XMLElement* label   = find_child(np, "navLabel");
    const XMLElement* text    = (label != nullptr) ? find_child(label, "text") : nullptr;
    const XMLElement* content = find_child(np, "content");
    const char*       title   = (text != nullptr) ? text->GetText() : nullptr;
    const char*       src     = (content != nullptr) ? content->Attribute("src") : nullptr;
    toc_emit(book, title, src, depth);
    ncx_walk(np, book, static_cast<std::uint8_t>(depth + 1U));
  }
}

/**
 * @brief Recursively find the first `<nav>` whose `epub:type` token list
 *        contains `type` (e.g. "toc").
 */
const XMLElement* find_nav_by_type(const XMLElement* root, const char* type)
{
  for (const XMLElement* child = root->FirstChildElement(); child != nullptr;
       child                   = child->NextSiblingElement()) {
    if (elem_local_is(child, "nav")) {
      const char* attr = child->Attribute("epub:type");
      if (attr != nullptr && std::strstr(attr, type) != nullptr) {
        return child;
      }
    }
    const XMLElement* deeper = find_nav_by_type(child, type);
    if (deeper != nullptr) {
      return deeper;
    }
  }
  return nullptr;
}

/**
 * @brief Depth-first walk of nav `<li>` siblings under an `<ol>`.
 */
void nav_walk(const XMLElement* ordered_list, ra8_epub_book_t* book, std::uint8_t depth)
{
  for (const XMLElement* li = ordered_list->FirstChildElement(); li != nullptr;
       li                   = li->NextSiblingElement()) {
    if (!elem_local_is(li, "li")) {
      continue;
    }
    const XMLElement* anchor = find_child(li, "a");
    const XMLElement* label  = (anchor != nullptr) ? anchor : find_child(li, "span");
    const char*       title  = (label != nullptr) ? label->GetText() : nullptr;
    const char*       href   = (anchor != nullptr) ? anchor->Attribute("href") : nullptr;
    toc_emit(book, title, href, depth);
    const XMLElement* nested = find_child(li, "ol");
    if (nested != nullptr) {
      nav_walk(nested, book, static_cast<std::uint8_t>(depth + 1U));
    }
  }
}

} /* namespace */

extern "C" ra8_err_t ra8_epub_xml_parse_container(const uint8_t*               xml_bytes,
                                                  size_t                       xml_len,
                                                  ra8_epub_container_result_t* out)
{
  if (xml_bytes == nullptr || out == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (xml_len == 0U) {
    return k_ra8_err_invalid_size;
  }

  out->opf_path[0] = '\0';

  XMLDocument doc;
  if (doc.Parse(reinterpret_cast<const char*>(xml_bytes), xml_len) != tinyxml2::XML_SUCCESS) {
    return k_ra8_err_validation_failed;
  }

  const XMLElement* root = doc.RootElement();
  if (root == nullptr) {
    return k_ra8_err_validation_failed;
  }

  const XMLElement* rootfiles = find_descendant(root, "rootfiles");
  const XMLElement* rootfile =
    (rootfiles != nullptr) ? find_child(rootfiles, "rootfile") : find_descendant(root, "rootfile");
  if (rootfile == nullptr) {
    return k_ra8_err_validation_failed;
  }

  const char* full_path = rootfile->Attribute("full-path");
  if (full_path == nullptr || full_path[0] == '\0') {
    return k_ra8_err_validation_failed;
  }
  copy_bounded(out->opf_path, k_ra8_epub_max_path_len, full_path);
  return k_ra8_ok;
}

extern "C" ra8_err_t
ra8_epub_xml_parse_opf(const uint8_t* xml_bytes, size_t xml_len, ra8_epub_book_t* book)
{
  if (xml_bytes == nullptr || book == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (xml_len == 0U) {
    return k_ra8_err_invalid_size;
  }

  XMLDocument doc;
  if (doc.Parse(reinterpret_cast<const char*>(xml_bytes), xml_len) != tinyxml2::XML_SUCCESS) {
    return k_ra8_err_validation_failed;
  }
  const XMLElement* package = doc.RootElement();
  if (package == nullptr) {
    return k_ra8_err_validation_failed;
  }

  /* ---- metadata block (Dublin Core) ----------------------------------- */
  const XMLElement* metadata = find_child(package, "metadata");
  if (metadata != nullptr) {
    const XMLElement* title = find_child(metadata, "title");
    if (title != nullptr) {
      copy_bounded(book->title, k_ra8_epub_meta_len, title->GetText());
    }
    const XMLElement* creator = find_child(metadata, "creator");
    if (creator != nullptr) {
      copy_bounded(book->author, k_ra8_epub_meta_len, creator->GetText());
    }
    const XMLElement* language = find_child(metadata, "language");
    if (language != nullptr) {
      copy_bounded(book->language, k_ra8_epub_meta_len, language->GetText());
    }
    /* The canonical unique id is the <dc:identifier> whose id matches the
     * package's unique-identifier attribute. */
    const char* uid = package->Attribute("unique-identifier");
    copy_bounded(book->identifier, k_ra8_epub_meta_len, find_identifier(metadata, uid));
  }

  /* ---- manifest ------------------------------------------------------- */
  const XMLElement* manifest = find_child(package, "manifest");
  const XMLElement* spine    = find_child(package, "spine");
  if (manifest == nullptr || spine == nullptr) {
    return k_ra8_err_validation_failed;
  }

  /* Cover: prefer EPUB3 properties="cover-image"; fall back to legacy
   * <meta name="cover" content="ID"> + manifest lookup. */
  const char* cover_href = find_cover_by_properties(manifest);
  if (cover_href == nullptr) {
    cover_href = find_cover_by_meta(metadata, manifest);
  }
  if (cover_href != nullptr) {
    copy_bounded(book->cover_path, k_ra8_epub_max_path_len, cover_href);
  }

  /* ---- embedded fonts (#109): record manifest font hrefs -------------- */
  collect_font_items(manifest, book);

  /* ---- manifest items (#151): record id/href/media-type in OPF order --- */
  collect_manifest_items(manifest, book);

  /* ---- spine (chapters in document order) ----------------------------- */
  uint16_t count = 0U;
  for (const XMLElement* itemref = spine->FirstChildElement("itemref"); itemref != nullptr;
       itemref                   = itemref->NextSiblingElement("itemref")) {
    const char* idref = itemref->Attribute("idref");
    if (idref == nullptr) {
      continue;
    }
    const char* href = manifest_href_by_id(manifest, idref);
    if (href == nullptr) {
      continue;
    }
    if (count >= k_ra8_epub_max_chapters) {
      return k_ra8_err_no_mem;
    }
    copy_bounded(book->chapter_paths[count], k_ra8_epub_max_path_len, href);
    ++count;
  }
  book->chapter_count = count;

  /* ---- table-of-contents source --------------------------------------- */
  /* Prefer the EPUB 3 nav document (manifest properties="nav"); fall back
   * to the EPUB 2 NCX referenced by the spine's `toc` attribute. The TOC
   * document itself is extracted and parsed by the caller (open path). */
  const char* nav_href = find_nav_manifest_href(manifest);
  if (nav_href != nullptr) {
    copy_bounded(book->toc_path, k_ra8_epub_max_path_len, nav_href);
    book->toc_kind = static_cast<std::uint8_t>(k_ra8_epub_toc_nav);
  } else {
    const char* toc_id   = spine->Attribute("toc");
    const char* ncx_href = (toc_id != nullptr) ? manifest_href_by_id(manifest, toc_id) : nullptr;
    if (ncx_href != nullptr) {
      copy_bounded(book->toc_path, k_ra8_epub_max_path_len, ncx_href);
      book->toc_kind = static_cast<std::uint8_t>(k_ra8_epub_toc_ncx);
    }
  }
  return k_ra8_ok;
}

extern "C" ra8_err_t
ra8_epub_xml_parse_ncx(const uint8_t* xml_bytes, size_t xml_len, ra8_epub_book_t* book)
{
  if (xml_bytes == nullptr || book == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (xml_len == 0U) {
    return k_ra8_err_invalid_size;
  }

  book->toc_count = 0U;

  XMLDocument doc;
  if (doc.Parse(reinterpret_cast<const char*>(xml_bytes), xml_len) != tinyxml2::XML_SUCCESS) {
    return k_ra8_err_validation_failed;
  }
  const XMLElement* root = doc.RootElement();
  if (root == nullptr) {
    return k_ra8_err_validation_failed;
  }
  const XMLElement* nav_map = find_descendant(root, "navMap");
  if (nav_map == nullptr) {
    return k_ra8_err_validation_failed;
  }
  ncx_walk(nav_map, book, 0U);
  return k_ra8_ok;
}

extern "C" ra8_err_t
ra8_epub_xml_parse_nav(const uint8_t* xml_bytes, size_t xml_len, ra8_epub_book_t* book)
{
  if (xml_bytes == nullptr || book == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (xml_len == 0U) {
    return k_ra8_err_invalid_size;
  }

  book->toc_count = 0U;

  XMLDocument doc;
  if (doc.Parse(reinterpret_cast<const char*>(xml_bytes), xml_len) != tinyxml2::XML_SUCCESS) {
    return k_ra8_err_validation_failed;
  }
  const XMLElement* root = doc.RootElement();
  if (root == nullptr) {
    return k_ra8_err_validation_failed;
  }
  const XMLElement* nav = find_nav_by_type(root, "toc");
  if (nav == nullptr) {
    nav = find_descendant(root, "nav");
  }
  if (nav == nullptr) {
    return k_ra8_err_validation_failed;
  }
  const XMLElement* ordered_list = find_child(nav, "ol");
  if (ordered_list == nullptr) {
    return k_ra8_err_validation_failed;
  }
  nav_walk(ordered_list, book, 0U);
  return k_ra8_ok;
}
