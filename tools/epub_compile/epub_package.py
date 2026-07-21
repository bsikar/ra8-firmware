"""Reading the EPUB package: the OPF manifest/spine and the navigation document.

Namespace-tolerant on purpose. Real EPUBs in the wild bind the OPF and NCX
namespaces inconsistently (and some omit them), so every element is matched on
its LOCAL name -- a book that will not open because of a namespace prefix is a
bug in the tool, not in the book.

@copyright Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
"""

from __future__ import annotations

import posixpath
from typing import TYPE_CHECKING
from xml.etree import ElementTree as ET

if TYPE_CHECKING:
    from zipfile import ZipFile


# --- EPUB unpacking -----------------------------------------------------------
def opf_localname(tag: str | None) -> str | None:
    """Strip the `{namespace}` prefix ElementTree prepends to a tag name.

    OPF, NCX and XHTML documents all declare namespaces, and publishers use
    different prefixes and even different namespace URIs for the same vocabulary.
    Matching on the local name alone is what lets the parsers below accept real
    EPUBs instead of only spec-perfect ones.

    Args:
        tag: An ElementTree tag, namespaced or not. None and "" pass through.

    Returns:
        The local name, or `tag` unchanged when it carries no namespace.
    """
    return tag.split("}", 1)[1] if tag and tag[0] == "{" else tag


def parse_opf(
    zf: ZipFile,
) -> tuple[str, dict[str, str], dict[str, tuple[str, str, str]], list[str], str | None]:
    """Read container.xml and the OPF package it points at.

    Walks the OPF once with `iter()` rather than following its schema, so
    elements in unexpected parents are still found. Metadata fields take the
    FIRST value seen and ignore later ones, which is how a book with several
    `<dc:creator>` entries yields one author.

    Cover resolution follows the device's precedence exactly: an EPUB3 manifest
    item whose `properties` contains "cover-image" wins, and the legacy EPUB2
    `<meta name="cover">` is only the fallback. The substring test mirrors
    `ra8_epub_xml_shim.cpp` `find_cover_by_properties()` byte for byte, so the
    host `.rabook` and an on-device compile agree on the cover -- which matters
    for EPUB3-only fixed-layout comics that ship no legacy meta (issue #196).

    Args:
        zf: Open ZipFile for the EPUB.

    Returns:
        Tuple of (opf_dir, meta, manifest, spine, cover_id): the OPF's directory
        for resolving relative hrefs, the four metadata strings (each "" if
        absent), manifest id -> (href, media_type, properties), spine idrefs in
        reading order, and the cover's manifest id or None.

    Raises:
        KeyError: The EPUB has no META-INF/container.xml.
        xml.etree.ElementTree.ParseError: container.xml or the OPF is not
            well-formed XML.
    """
    container = ET.fromstring(zf.read("META-INF/container.xml"))  # noqa: S314  # trusted local EPUB input
    rootfile = None
    for el in container.iter():
        if opf_localname(el.tag) == "rootfile":
            rootfile = el.get("full-path")
            break
    opf = ET.fromstring(zf.read(rootfile))  # noqa: S314  # trusted local EPUB input
    opf_dir = posixpath.dirname(rootfile)

    meta = {"title": "", "author": "", "language": "", "identifier": ""}
    manifest = {}  # id -> (href, media_type, properties)
    spine = []  # list of idref
    cover_id_meta = None  # legacy EPUB2 <meta name="cover" content="ID">
    cover_id_props = None  # EPUB3 manifest <item properties="cover-image">
    for el in opf.iter():
        tag = opf_localname(el.tag)
        if tag == "title" and not meta["title"]:
            meta["title"] = (el.text or "").strip()
        elif tag == "creator" and not meta["author"]:
            meta["author"] = (el.text or "").strip()
        elif tag == "language" and not meta["language"]:
            meta["language"] = (el.text or "").strip()
        elif tag == "identifier" and not meta["identifier"]:
            meta["identifier"] = (el.text or "").strip()
        elif tag == "meta" and el.get("name") == "cover":
            cover_id_meta = el.get("content")
        elif tag == "item":
            props = el.get("properties", "") or ""
            manifest[el.get("id")] = (el.get("href"), el.get("media-type", ""), props)
            # First manifest item carrying the EPUB3 cover-image property. The
            # substring test mirrors ra8_epub_xml_shim.cpp find_cover_by_properties()
            # (a strstr over the space-separated properties list) byte-for-byte, so
            # an EPUB3-only book -- no legacy meta, how modern fixed-layout comics
            # ship (issue #196) -- resolves the same cover host-side and on-device.
            if (cover_id_props is None) and ("cover-image" in props):
                cover_id_props = el.get("id")
        elif tag == "itemref":
            spine.append(el.get("idref"))
    # EPUB3 properties="cover-image" wins; the legacy <meta name="cover"> is the
    # fallback -- the exact precedence ra8_epub_xml_shim.cpp uses on device
    # (find_cover_by_properties() then find_cover_by_meta()), so the desktop
    # .rabook and the on-device compile agree on the cover image index.
    cover_id = cover_id_props if cover_id_props is not None else cover_id_meta
    return opf_dir, meta, manifest, spine, cover_id


def parse_toc(
    zf: ZipFile, opf_dir: str, manifest: dict[str, tuple[str, str, str]]
) -> dict[str, str]:
    """Map a normalized doc path -> TOC label, from the EPUB3 nav or EPUB2 NCX."""
    labels = {}

    def resolve(href: str) -> str:
        return posixpath.normpath(posixpath.join(opf_dir, href.split("#", 1)[0]))

    nav_href = next((h for (h, _m, p) in manifest.values() if "nav" in p), None)
    ncx_href = next(
        (h for (h, m, _p) in manifest.values() if m == "application/x-dtbncx+xml"), None
    )
    try:
        if nav_href:
            tree = ET.fromstring(zf.read(resolve(nav_href)))  # noqa: S314  # trusted local EPUB input
            base = posixpath.dirname(resolve(nav_href))
            for a in tree.iter():
                if opf_localname(a.tag) == "a" and a.get("href"):
                    tgt = posixpath.normpath(posixpath.join(base, a.get("href").split("#", 1)[0]))
                    labels.setdefault(tgt, "".join(a.itertext()).strip())
        elif ncx_href:
            tree = ET.fromstring(zf.read(resolve(ncx_href)))  # noqa: S314  # trusted local EPUB input
            base = posixpath.dirname(resolve(ncx_href))
            for nav in tree.iter():
                if opf_localname(nav.tag) != "navPoint":
                    continue
                label = ""
                href = None
                for sub in nav.iter():
                    ln = opf_localname(sub.tag)
                    if ln == "text" and not label:
                        label = (sub.text or "").strip()
                    elif ln == "content":
                        href = sub.get("src")
                if href:
                    tgt = posixpath.normpath(posixpath.join(base, href.split("#", 1)[0]))
                    labels.setdefault(tgt, label)
    except (ET.ParseError, KeyError):
        pass
    return labels
