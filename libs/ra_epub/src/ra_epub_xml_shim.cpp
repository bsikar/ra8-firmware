/**
 * @file ra_epub_xml_shim.cpp
 * @brief C++ -> C shim that wraps tinyxml2 for the ra_epub parser.
 *
 * @details
 * The rest of `libs/ra_epub` is plain C. We reach for the vendored
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
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / EPUB]
 * {World: NS}
 */

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "ra_epub.h"
#include "tinyxml2.h"

extern "C" {

/**
 * @brief Out-parameter struct returned by `ra_epub_xml_parse_container()`.
 *
 * @details The OPF path is always non-empty on success; the caller
 *          uses `opf_path_buf` to feed the second pass
 *          (`ra_epub_xml_parse_opf()`).
 */
typedef struct {
  char opf_path[k_ra_epub_max_path_len];
} ra_epub_container_result_t;

/**
 * @brief Parse `META-INF/container.xml` and copy the rootfile path into
 *        `out`.
 */
ra_err_t ra_epub_xml_parse_container(const uint8_t*              xml_bytes,
                                     size_t                      xml_len,
                                     ra_epub_container_result_t* out);

/**
 * @brief Parse the OPF document and fill in metadata, cover, and spine.
 *
 * `book` is mutated in-place: `chapter_count`, `chapter_paths`,
 * `title`, `author`, `language`, `cover_path` are written.
 */
ra_err_t ra_epub_xml_parse_opf(const uint8_t* xml_bytes, size_t xml_len, ra_epub_book_t* book);

} /* extern "C" */

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
    if (meta_name != nullptr && meta_content != nullptr && std::strcmp(meta_name, "cover") == 0) {
      const char* href = manifest_href_by_id(manifest, meta_content);
      if (href != nullptr) {
        return href;
      }
    }
  }
  return nullptr;
}

} /* namespace */

extern "C" ra_err_t ra_epub_xml_parse_container(const uint8_t*              xml_bytes,
                                                size_t                      xml_len,
                                                ra_epub_container_result_t* out)
{
  if (xml_bytes == nullptr || out == nullptr) {
    return k_ra_err_null_ptr;
  }
  if (xml_len == 0U) {
    return k_ra_err_invalid_size;
  }

  out->opf_path[0] = '\0';

  XMLDocument doc;
  if (doc.Parse(reinterpret_cast<const char*>(xml_bytes), xml_len) != tinyxml2::XML_SUCCESS) {
    return k_ra_err_validation_failed;
  }

  const XMLElement* root = doc.RootElement();
  if (root == nullptr) {
    return k_ra_err_validation_failed;
  }

  const XMLElement* rootfiles = find_descendant(root, "rootfiles");
  const XMLElement* rootfile =
    (rootfiles != nullptr) ? find_child(rootfiles, "rootfile") : find_descendant(root, "rootfile");
  if (rootfile == nullptr) {
    return k_ra_err_validation_failed;
  }

  const char* full_path = rootfile->Attribute("full-path");
  if (full_path == nullptr || full_path[0] == '\0') {
    return k_ra_err_validation_failed;
  }
  copy_bounded(out->opf_path, k_ra_epub_max_path_len, full_path);
  return k_ra_ok;
}

extern "C" ra_err_t
ra_epub_xml_parse_opf(const uint8_t* xml_bytes, size_t xml_len, ra_epub_book_t* book)
{
  if (xml_bytes == nullptr || book == nullptr) {
    return k_ra_err_null_ptr;
  }
  if (xml_len == 0U) {
    return k_ra_err_invalid_size;
  }

  XMLDocument doc;
  if (doc.Parse(reinterpret_cast<const char*>(xml_bytes), xml_len) != tinyxml2::XML_SUCCESS) {
    return k_ra_err_validation_failed;
  }
  const XMLElement* package = doc.RootElement();
  if (package == nullptr) {
    return k_ra_err_validation_failed;
  }

  /* ---- metadata block (Dublin Core) ----------------------------------- */
  const XMLElement* metadata = find_child(package, "metadata");
  if (metadata != nullptr) {
    const XMLElement* title = find_child(metadata, "title");
    if (title != nullptr) {
      copy_bounded(book->title, k_ra_epub_meta_len, title->GetText());
    }
    const XMLElement* creator = find_child(metadata, "creator");
    if (creator != nullptr) {
      copy_bounded(book->author, k_ra_epub_meta_len, creator->GetText());
    }
    const XMLElement* language = find_child(metadata, "language");
    if (language != nullptr) {
      copy_bounded(book->language, k_ra_epub_meta_len, language->GetText());
    }
  }

  /* ---- manifest ------------------------------------------------------- */
  const XMLElement* manifest = find_child(package, "manifest");
  const XMLElement* spine    = find_child(package, "spine");
  if (manifest == nullptr || spine == nullptr) {
    return k_ra_err_validation_failed;
  }

  /* Cover: prefer EPUB3 properties="cover-image"; fall back to legacy
   * <meta name="cover" content="ID"> + manifest lookup. */
  const char* cover_href = find_cover_by_properties(manifest);
  if (cover_href == nullptr) {
    cover_href = find_cover_by_meta(metadata, manifest);
  }
  if (cover_href != nullptr) {
    copy_bounded(book->cover_path, k_ra_epub_max_path_len, cover_href);
  }

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
    if (count >= k_ra_epub_max_chapters) {
      return k_ra_err_no_mem;
    }
    copy_bounded(book->chapter_paths[count], k_ra_epub_max_path_len, href);
    ++count;
  }
  book->chapter_count = count;
  return k_ra_ok;
}
