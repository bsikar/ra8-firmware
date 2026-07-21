"""XHTML -> DOM, preserving everything.

Fidelity is the rule: every tag, attribute and text run in a spine document
survives into the blob, because the on-device reader never parses XHTML and
cannot recover anything this stage drops. Nothing is filtered to match what
the renderer happens to understand today.

@copyright Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
"""

from html.parser import HTMLParser

VOID_TAGS = {
    "area",
    "base",
    "br",
    "col",
    "embed",
    "hr",
    "img",
    "input",
    "link",
    "meta",
    "param",
    "source",
    "track",
    "wbr",
}


class StringPool:
    """De-duplicating UTF-8 string pool; offset 0 is always the empty string."""

    def __init__(self):
        """Start an empty pool and intern "" so offset 0 is the empty string.

        That first intern is load-bearing, not a convenience: the blob format
        uses offset 0 as its "no string" sentinel (an absent title, a text node
        on an element record). Constructing the pool without it would put real
        text at offset 0 and make every such sentinel read back as that text.
        """
        self.buf = bytearray()
        self._map = {}
        self.intern("")

    def intern(self, text):
        """Return the pool offset of `text`, appending it only if new.

        De-duplication is by exact UTF-8 bytes, so two strings that differ only
        in Unicode normalisation get separate slots. `None` is folded to "" and
        therefore always yields offset 0, which lets callers pass a missing
        metadata field straight through without a guard.

        Offsets are stable for the life of the pool: entries are only appended,
        never moved or removed. Callers may hold an offset across later
        `intern()` calls -- but NOT across a snapshot of `buf`, which is why
        `BlobBuilder.serialize()` interns the metadata strings before it copies
        the buffer.

        Args:
            text: String to intern, or None for the empty string.

        Returns:
            Byte offset of the NUL-terminated UTF-8 copy within `buf`.
        """
        if text is None:
            text = ""
        raw = text.encode("utf-8")
        got = self._map.get(raw)
        if got is not None:
            return got
        off = len(self.buf)
        self.buf += raw + b"\x00"
        self._map[raw] = off
        return off


class DomBuilder(HTMLParser):
    """Lenient HTML/XHTML parser that builds a generic element/text tree.

    convert_charrefs resolves entities to Unicode, and <style>/<script> bodies
    are captured as text (so inline CSS survives). Tag names and attributes are
    preserved exactly; inline <svg> becomes ordinary elements.
    """

    def __init__(self):
        """Seed the tree with a synthetic `#root` element and open the stack.

        The sentinel root exists so `handle_data`/`handle_starttag` never have
        to special-case an empty stack: a document with leading text, or one
        with several top-level elements, still has somewhere to attach. It is
        also the fallback chapter root when `compile_epub` finds no `<body>`.
        """
        super().__init__(convert_charrefs=True)
        self.root = {"tag": "#root", "attrs": [], "children": []}
        self.stack = [self.root]

    def handle_starttag(self, tag, attrs):
        """Append an element to the current parent and descend into it.

        Void tags (`<br>`, `<img>`, ...) are appended but NOT pushed, because
        XHTML in the wild spells them both `<br>` and `<br/>`; pushing them
        would leave the stack permanently deeper on the unclosed spelling and
        nest every following sibling inside the void element.

        Args:
            tag: Lowercased tag name from HTMLParser.
            attrs: HTMLParser's (name, value) pairs, preserved verbatim -- order
                and duplicates included, since the blob stores them as written.
        """
        node = {"tag": tag, "attrs": attrs, "children": []}
        self.stack[-1]["children"].append(node)
        if tag not in VOID_TAGS:
            self.stack.append(node)

    def handle_startendtag(self, tag, attrs):
        """Append a self-closing element (`<foo/>`) without descending.

        HTMLParser routes the self-closed spelling here instead of through
        `handle_starttag` + `handle_endtag`, so this must not push the stack.
        It applies to any tag, not just the void set -- `<div/>` and inline
        `<svg><path/></svg>` both arrive here.

        Args:
            tag: Lowercased tag name.
            attrs: (name, value) pairs, preserved verbatim.
        """
        self.stack[-1]["children"].append({"tag": tag, "attrs": attrs, "children": []})

    def handle_endtag(self, tag):
        """Close the innermost open element with this name.

        Any elements left open inside it are discarded from the stack.
        Scanning inward rather than popping one frame lets malformed markup
        (`<p><b>text</p>`) close correctly: the unclosed `<b>` is dropped from
        the stack along with `<p>`, but its already-attached subtree survives in
        the tree. A stray end tag with no matching open element is ignored
        rather than raising -- index 0 is excluded from the scan so the
        synthetic `#root` can never be popped.

        Args:
            tag: Lowercased tag name being closed.
        """
        for i in range(len(self.stack) - 1, 0, -1):
            if self.stack[i]["tag"] == tag:
                del self.stack[i:]
                return

    def handle_data(self, data):
        """Attach a text run to the current parent, dropping only empty runs.

        Whitespace is deliberately kept: the device-side renderer performs its
        own CSS whitespace collapsing, and stripping here would destroy the
        single significant space between two inline elements. Because
        `convert_charrefs=True`, entities have already been resolved to Unicode
        by the time this is called, and one logical text run may still arrive
        split across several calls -- each becomes its own text node, which the
        renderer treats identically to one merged node.

        Args:
            data: Decoded character data.
        """
        if data:
            self.stack[-1]["children"].append({"text": data})


def find_first(node, tag):
    """Depth-first search for the first element with the given tag name."""
    for child in node.get("children", []):
        if child.get("tag") == tag:
            return child
        hit = find_first(child, tag)
        if hit is not None:
            return hit
    return None
