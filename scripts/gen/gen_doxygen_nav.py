#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Generate the Doxygen navigation trees, mirroring the on-disk layout.

Doxygen renders markdown files (docs/*.md and every example README.md) as
"pages", never as entries in the Files/directory tree -- so the sidebar's
directory view is code-only, and the human-readable docs + per-app READMEs
can only be reached through the "pages" tab. This script builds that pages
tab to MIRROR THE ON-DISK DIRECTORY TREE instead of an invented taxonomy, so
navigation reads the same way everywhere (browse by directory) and can never
drift from the repo:

  1. "Documentation" -- the docs/ subtree. Every docs/*.md is a leaf and
     every docs/<subdir> (adr, SOUP, qualification, reference, ...) is a
     sub-node, recursively, exactly as they sit on disk. No doc-to-section
     map: a new page or a new subdir just appears; a removed one just leaves.

  2. "Examples" -- the examples/ subtree. Every app appears under its real
     hardware-validation tier directory (hw_validated/hil, hw_validated/manual,
     hw_pending, _unsupported, ra8p1_foundation, ...), each carrying the
     one-line description from its own README. Moving an app between tiers on
     disk moves it in the nav with no edit here.

The only hand-maintained data left is a set of PRETTY-TITLE maps (tier and
docs-subdir display names); every one degrades gracefully -- an unmapped
directory falls back to a prettified name and still renders, never breaks and
never lists a removed item.

The generated .dox files are written under docs/generated/ (gitignored) and
consumed by the main Doxyfile. build_docs.sh runs this before doxygen, so the
navigation can never drift from the tree.
"""

from __future__ import annotations

import hashlib
import html
import pathlib

ROOT = pathlib.Path(__file__).resolve().parents[2]
DOCS_DIR = ROOT / "docs"
EXAMPLES_DIR = ROOT / "examples"
OUT_DIR = DOCS_DIR / "generated"

# --- Doxygen id manglers -----------------------------------------------------
# These reproduce the pinned Doxygen 1.16.1's deterministic naming so the
# generated @subpage / link targets resolve without editing any source file. If
# Doxygen ever changed the scheme, the warnings gate would flag the unresolved
# references.


def doxy_page_id(relpath: str) -> str:
    """Doxygen page id for a markdown file.

    Matches the Doxyfile's CASE_SENSE_NAMES=YES scheme: the "md_" prefix, the
    path with '/' -> '_2' and '_' -> '__', and the original case preserved
    (e.g. docs/ACRONYMS.md -> md_docs_2ACRONYMS). The '.md' extension is
    dropped. Under CASE_SENSE_NAMES=NO doxygen would instead escape every
    uppercase letter, so this mangler is coupled to that Doxyfile setting.
    """
    stem = str(pathlib.PurePosixPath(relpath).with_suffix(""))
    out = ["md_"]
    for ch in stem:
        if ch == "/":
            out.append("_2")
        elif ch == "_":
            out.append("__")
        else:
            out.append(ch)  # case preserved
    return "".join(out)


def doxy_dir_id(relpath: str) -> str:
    """Doxygen directory-page id: ``dir_`` + md5 of the directory path.

    The path hashed is the STRIP_FROM_PATH-relative one WITH a trailing slash;
    both details are load-bearing, since doxygen hashes exactly that string and
    dropping the slash yields a valid-looking id that resolves to nothing.

    Doxygen renders a directory's README.md as that directory page's detailed
    description -- so this is where an example app's README actually shows,
    alongside its source-file list (the empty 'README.md File Reference' page
    is a dead end). Because the Doxyfile sets STRIP_FROM_PATH=., the path fed
    to the hash is the plain repo-relative one, so the id is machine-independent
    and matches the same page the Files directory tree links to.
    """
    key = relpath.rstrip("/") + "/"
    # Not security-sensitive: this reproduces doxygen's own directory-page
    # file-naming hash so links resolve. usedforsecurity=False documents that
    # and satisfies the lint.
    digest = hashlib.md5(key.encode("utf-8"), usedforsecurity=False).hexdigest()
    return "dir_" + digest


# --- narrative-docs (directory-mirroring) ------------------------------------
# The "Documentation" tree mirrors docs/ exactly, so there is NO doc-to-section
# classification map to rot. The only hand-maintained data is a set of pretty
# display titles for a few subdirectories; every lookup degrades gracefully --
# an unmapped subdir simply shows its prettified directory name.
SUBDIR_TITLES: dict[str, str] = {
    "adr": "Architecture Decision Records (adr)",
    "formats": "Binary format specifications (formats)",
    "SOUP": "SOUP component justifications (SOUP)",
    "qualification": "Qualification kit (qualification)",
    "reference": "Datasheet reference (reference)",
}

# docs/<subdir> holding no narrative markdown (vendored theme, generated .dox,
# the legacy build tree, and binary/asset dirs). These are excluded from the
# Doxyfile INPUT too, so linking their READMEs would dangle -- skip them. A new
# subdir that DOES carry .md is picked up automatically; nothing here hides
# real narrative content (badges/sbom hold only images/JSON, no *.md).
DOCS_SKIP_DIRS = {"generated", "doxygen", "doxygen_theme", "badges", "sbom"}

# --- example-tier taxonomy ---------------------------------------------------
# Pretty titles for the example directory tiers. Unknown tiers fall back to a
# prettified directory name so a new board tier still renders.
TIER_TITLES: dict[str, str] = {
    "ek_ra8d2": "EK-RA8D2 (stock evaluation kit)",
    "ek_ra8d2/hw_validated": "Hardware-validated",
    "ek_ra8d2/hw_validated/hil": "Hardware-in-the-loop (HIL)",
    "ek_ra8d2/hil_needs_revalidation": "HIL -- needs re-validation",
    "ek_ra8d2/hw_validated/manual": "Manual (jumper / button steps)",
    "ek_ra8d2/hw_pending": "Hardware-pending",
    "ek_ra8d2/hw_pending/manual": "Manual (jumper / button steps)",
    "_unsupported": "Needs external hardware",
    "ra8p1_foundation": "RA8P1 foundation",
}
TIER_BRIEFS: dict[str, str] = {
    "ek_ra8d2": "Apps that run on a stock EK-RA8D2 v1 kit with no added parts.",
    "ek_ra8d2/hil_needs_revalidation": "Apps moved out of the HIL-passing set: "
    "each is blocked by bench config, an SD reseat, absent external hardware, "
    "or is under triage -- see the tier README.",
    "_unsupported": "Apps that need hardware not on the stock board "
    "(motor driver, audio CODEC, external radios, ...).",
    "ra8p1_foundation": "Foundation apps for the RA8P1 variant (RA8D2 + Ethos-U55 NPU).",
}


def prettify(name: str) -> str:
    """Fallback display title for a directory with no entry in the title maps.

    This is what makes the title maps optional rather than authoritative: a
    tier or docs subdirectory added on disk renders as Title Case immediately,
    so forgetting to add a pretty name degrades the label and never drops the
    node out of the navigation.
    """
    return name.replace("_", " ").strip().title()


def clean_desc(text: str) -> str:
    """Reduce a README paragraph to one safe, plain-text line."""
    text = text.replace("`", "").replace("**", "").replace("*", "")
    # collapse markdown links [txt](url) -> txt
    out = []
    i = 0
    while i < len(text):
        if text[i] == "[":
            close = text.find("]", i)
            paren = text.find("(", close) if close != -1 else -1
            if close != -1 and paren == close + 1:
                end = text.find(")", paren)
                if end != -1:
                    out.append(text[i + 1 : close])
                    i = end + 1
                    continue
        out.append(text[i])
        i += 1
    text = "".join(out)
    text = " ".join(text.split())
    limit = 200
    if len(text) > limit:
        cut = text[:limit].rsplit(" ", 1)[0]
        text = cut + " ..."
    # HTML-escape, then neutralize Doxygen-active characters.
    text = html.escape(text, quote=False)
    return text.replace("@", "&#64;").replace("\\", "&#92;")


def read_app_desc(app_dir: pathlib.Path) -> str:
    """Pull the one-line blurb for an example app out of its own README.

    Takes the first prose paragraph AFTER the H1, which is where these READMEs
    put their summary. The scan stops at a heading or a table row so an app
    whose README opens with a status table straight after the title yields an
    empty description rather than a row of pipes rendered as prose.

    A missing README returns "" rather than raising: an app without one still
    belongs in the navigation, just without a blurb.
    """
    readme = app_dir / "README.md"
    if not readme.is_file():
        return ""
    lines = readme.read_text(encoding="utf-8", errors="replace").splitlines()
    # skip to after the first H1
    idx = 0
    while idx < len(lines) and not lines[idx].startswith("# "):
        idx += 1
    idx += 1
    # skip blanks
    while idx < len(lines) and not lines[idx].strip():
        idx += 1
    para = []
    while idx < len(lines) and lines[idx].strip():
        stripped = lines[idx].strip()
        if stripped.startswith(("#", "|")):
            break
        para.append(stripped)
        idx += 1
    return clean_desc(" ".join(para))


# --- example tree ------------------------------------------------------------
class TierNode:
    """One directory level of the examples/ tree, holding its apps and children.

    The tree is built from the app directories found on disk, so a node exists
    only because some app lives at or below it -- there is no declared tier
    list to fall out of step with the repository.
    """

    def __init__(self, path: str) -> None:
        """Create an empty node for the tier at ``path`` relative to examples/.

        The root is spelled "" rather than "." so that child paths concatenate
        cleanly and the root's page id can be special-cased.
        """
        self.path = path  # relative to examples/, "" for root
        self.apps: list[tuple[str, str, str]] = []  # (name, file_id, desc)
        self.children: dict[str, TierNode] = {}

    def title(self) -> str:
        """Display title for this tier, falling back to a prettified leaf name.

        Lookup is on the node's FULL path, so two tiers sharing a leaf name at
        different depths (``hw_validated/manual`` and ``hw_pending/manual``)
        can be titled independently.
        """
        if self.path in TIER_TITLES:
            return TIER_TITLES[self.path]
        return prettify(self.path.split("/")[-1])

    def page_id(self) -> str:
        """Doxygen ``@page`` id for this tier, unique and identifier-safe.

        Every character that is neither alphanumeric nor an underscore is
        replaced, so a tier directory containing a dot or a dash cannot emit an
        id doxygen would silently truncate or reject.
        """
        if not self.path:
            return "ra8_examples"
        safe = self.path.replace("/", "_")
        safe = "".join(c if (c.isalnum() or c == "_") else "_" for c in safe)
        return "ra8_ex_" + safe


def build_example_tree() -> tuple[TierNode, int]:
    """Discover every example app and assemble the tier tree it implies.

    An app is defined as "a directory containing main.c", which is what makes
    the navigation self-maintaining: moving an app between tiers on disk moves
    it in the sidebar with no edit here, and a deleted app simply stops being
    found. Intermediate tier nodes are created on demand as each path is
    walked, so only tiers that actually contain apps appear.

    Returns the root node (whose own ``apps`` list holds any app sitting
    directly under examples/) and the total app count, which the caller prints
    in the page brief rather than recomputing.
    """
    root = TierNode("")
    count = 0
    mains = sorted(EXAMPLES_DIR.rglob("main.c"))
    for main_c in mains:
        app_dir = main_c.parent
        tier_rel = app_dir.parent.relative_to(EXAMPLES_DIR).as_posix()
        if tier_rel == ".":
            tier_rel = ""
        node = root
        acc = ""
        for seg in [s for s in tier_rel.split("/") if s]:
            acc = f"{acc}/{seg}" if acc else seg
            node = node.children.setdefault(acc, TierNode(acc))
        app_rel = app_dir.relative_to(ROOT).as_posix()
        # Link to the app's DIRECTORY page: doxygen renders the app README as
        # that page's description and lists its source files there -- the same
        # page the Files tree reaches. (The README file page itself is empty.)
        node.apps.append((app_dir.name, doxy_dir_id(app_rel), read_app_desc(app_dir)))
        count += 1
    return root, count


def emit_subpage_list(out: list[str], entries: list[tuple[str, str]]) -> None:
    """Emit @subpage links as an HTML bulleted list (one per line).

    entries is a list of (page_id, suffix); suffix is appended after the link
    (e.g. " (12 apps)") or "" for none.
    """
    if not entries:
        return
    out.append(" * <ul>")
    for pid, suffix in entries:
        out.append(f" * <li>@subpage {pid}{suffix}</li>")
    out.append(" * </ul>")


def emit_tier_page(node: TierNode, out: list[str]) -> None:
    """Append this tier's ``@page`` block to ``out``, then recurse into children.

    Appends in place rather than returning, so one list accumulates the whole
    tree in traversal order: a parent's page is emitted before its descendants,
    which is the order doxygen needs to nest the subpage links correctly.

    Apps are rendered as a table and child tiers as a bulleted subpage list, so
    a tier holding both reads as sections rather than one mixed list.
    """
    brief = TIER_BRIEFS.get(node.path, "")
    out.append("/**")
    out.append(f" * @page {node.page_id()} {node.title()}")
    if brief:
        out.append(f" * @brief {brief}")
    out.append(" *")
    child_nodes = [node.children[k] for k in sorted(node.children)]
    emit_subpage_list(
        out,
        [(c.page_id(), f" ({_count_apps(c)} apps)") for c in child_nodes],
    )
    if child_nodes and node.apps:
        out.append(" *")
    if node.apps:
        out.append(f" * @par {len(node.apps)} app(s) in this tier:")
        out.append(" * <table>")
        out.append(" * <tr><th>App</th><th>Description</th></tr>")
        for name, fid, desc in sorted(node.apps):
            link = f'<a href="{fid}.html"><code>{html.escape(name)}</code></a>'
            out.append(f" * <tr><td>{link}</td><td>{desc}</td></tr>")
        out.append(" * </table>")
    out.append(" */")
    out.append("")
    for child in child_nodes:
        emit_tier_page(child, out)


def _count_apps(node: TierNode) -> int:
    return len(node.apps) + sum(_count_apps(c) for c in node.children.values())


def gen_examples() -> str:
    """Render the complete "Examples" navigation tree as one .dox file body.

    Returns the file text, newline-terminated, ready to be written verbatim --
    no caller-side assembly, so the generated file has exactly one author.
    """
    root, total = build_example_tree()
    out: list[str] = [
        "// GENERATED by scripts/gen/gen_doxygen_nav.py -- do not edit.",
        "",
        "/**",
        " * @page ra8_examples Examples",
        f" * @brief All {total} example apps, browsable by the same board and",
        " *        hardware-validation tiers the repository uses on disk.",
        " *",
        " * Each app is a self-contained directory under `examples/` with its",
        " * own `main.c`, boot files, and linker script. Pick a tier below;",
        " * every app links to its README. Build any of them with `make <app>`",
        " * or run it on the emulator with `make sim-<app>`.",
        " *",
    ]
    child_nodes = [root.children[k] for k in sorted(root.children)]
    emit_subpage_list(
        out,
        [(c.page_id(), f" ({_count_apps(c)} apps)") for c in child_nodes],
    )
    out.append(" */")
    out.append("")
    for child in child_nodes:
        emit_tier_page(child, out)
    return "\n".join(out) + "\n"


# --- narrative docs tree -----------------------------------------------------
def _docs_subid(rel_under_docs: str) -> str:
    """Stable @page id for a docs/ subdirectory, keyed by its full sub-path.

    Keying on the full path (not just the leaf name) means two subdirs that
    share a leaf name at different depths cannot collide. The distinct
    "docsub" prefix keeps it clear of the "ra8_docs" root and of any page id.
    """
    safe = "".join(c if (c.isalnum() or c == "_") else "_" for c in rel_under_docs)
    return "ra8_docsub_" + safe


def _walk_docs(dir_path: pathlib.Path, rel_under_docs: str) -> tuple[list[str], str | None, str]:
    """Emit the @page blocks for a docs/ subtree, mirroring it on disk.

    Returns (page_blocks, subid, title). subid is None when the subtree holds
    no narrative markdown at all, so the parent omits it -- a directory of only
    images / JSON never becomes an empty nav node. Sub-directories are listed
    before files, matching doxygen's own Files-tree convention.
    """
    md_files = sorted(
        (p for p in dir_path.glob("*.md") if p.name.lower() != "readme.md"),
        key=lambda p: p.name.lower(),
    )
    child_dirs = sorted(
        (p for p in dir_path.iterdir() if p.is_dir() and p.name not in DOCS_SKIP_DIRS),
        key=lambda p: p.name.lower(),
    )

    blocks: list[str] = []
    child_nodes: list[tuple[str, str]] = []
    for cd in child_dirs:
        crel = f"{rel_under_docs}/{cd.name}" if rel_under_docs else cd.name
        cblocks, csubid, ctitle = _walk_docs(cd, crel)
        if csubid is not None:
            child_nodes.append((csubid, ctitle))
            blocks.extend(cblocks)

    leaf_ids = [doxy_page_id(p.relative_to(ROOT).as_posix()) for p in md_files]
    if not leaf_ids and not child_nodes:
        return [], None, ""

    subid = _docs_subid(rel_under_docs)
    title = SUBDIR_TITLES.get(dir_path.name, prettify(dir_path.name))
    page = ["/**", f" * @page {subid} {title}", " *"]
    entries = [(cid, "") for cid, _t in child_nodes] + [(pid, "") for pid in leaf_ids]
    emit_subpage_list(page, entries)
    page.append(" */")
    page.append("")
    # This directory's page first, then its descendants' pages.
    return page + blocks, subid, title


def _handwritten_doc_page_ids() -> list[str]:
    """Discover hand-written ``@page`` ids in docs/*.dox.

    Without this the hand-written pages would float at the top level of the
    sidebar rather than nesting under Documentation, since doxygen parents a
    page only where something ``@subpage``s it.

    Only the docs/ root .dox files are scanned (never docs/generated/, which
    this script owns). The ids are read straight from the files, so a page that
    is renamed or deleted is picked up or dropped automatically -- nothing is
    hardcoded, nothing dangles.
    """
    ids: list[str] = []
    for dox in sorted(DOCS_DIR.glob("*.dox"), key=lambda p: p.name.lower()):
        for line in dox.read_text(encoding="utf-8", errors="replace").splitlines():
            stripped = line.lstrip(" *")
            for tag in ("@page ", "\\page "):
                if stripped.startswith(tag):
                    pid = stripped[len(tag) :].split()[0]
                    if pid:
                        ids.append(pid)
    return ids


def gen_docs() -> str:
    """Render the complete "Documentation" navigation tree as one .dox file body.

    Both the generated per-file pages and the ids harvested from hand-written
    .dox files are listed as children here, so the two kinds of page appear in
    one tree rather than the hand-written ones floating at the top level.

    Returns the file text, newline-terminated.
    """
    # Top-level docs/*.md become leaves of the "Documentation" root; each
    # docs/<subdir> that carries markdown becomes a sub-node, recursively --
    # the tree is exactly the on-disk docs/ layout, no classification map.
    top_md = sorted(
        (p for p in DOCS_DIR.glob("*.md") if p.name.lower() != "readme.md"),
        key=lambda p: p.name.lower(),
    )
    top_leaf_ids = [doxy_page_id(p.relative_to(ROOT).as_posix()) for p in top_md]
    top_leaf_ids += _handwritten_doc_page_ids()

    subdirs = sorted(
        (p for p in DOCS_DIR.iterdir() if p.is_dir() and p.name not in DOCS_SKIP_DIRS),
        key=lambda p: p.name.lower(),
    )
    child_nodes: list[tuple[str, str]] = []
    sub_blocks: list[str] = []
    for sd in subdirs:
        blocks, subid, _title = _walk_docs(sd, sd.name)
        if subid is not None:
            child_nodes.append((subid, _title))
            sub_blocks.extend(blocks)

    out: list[str] = [
        "// GENERATED by scripts/gen/gen_doxygen_nav.py -- do not edit.",
        "",
        "/**",
        " * @page ra8_docs Documentation",
        " * @brief The hand-written documentation, browsable exactly as it sits",
        " *        under docs/ on disk. (The API reference lives under Topics /",
        " *        Data Structures; the source tree lives under Files.)",
        " *",
    ]
    # Sub-directories first, then the top-level pages -- same order as the
    # Files directory tree.
    entries = [(subid, "") for subid, _t in child_nodes]
    entries += [(pid, "") for pid in top_leaf_ids]
    emit_subpage_list(out, entries)
    out.append(" */")
    out.append("")
    out.extend(sub_blocks)

    return "\n".join(out) + "\n"


# --- directory descriptions (Files tab) --------------------------------------
# A plain "@dir src" in a static .dox is ambiguous: many example apps carry a
# nested src/ (examples/*/src), and doxygen applies the description to the
# alphabetically-first match instead of the repo-root src/. The other top-level
# directories have no same-named nested twin, so their @dir blocks stay in the
# static docs/doxygen_dirs.dox. Here we emit an ABSOLUTE-path @dir for any such
# colliding top-level directory, which matches exactly one directory.
COLLIDING_TOP_DIRS: dict[str, str] = {
    "src": "Shared application internals used by the drivers -- no boot code, "
    "no main(). Includes the Ring 5 secure-side substrate (src/secure_app, "
    "e.g. the key vault and secure-OTA commit).",
}


def gen_dirs() -> str:
    """Emit ``@dir`` blocks for top-level directories with a same-named twin.

    A bare ``@dir src`` is ambiguous because many example apps carry their own
    nested src/, and doxygen silently binds the description to the
    alphabetically-first match rather than the repo-root one. Only the
    colliding names need this treatment; the rest keep their static blocks in
    docs/doxygen_dirs.dox.

    Returns the file text, newline-terminated.
    """
    out = ["// GENERATED by scripts/gen/gen_doxygen_nav.py -- do not edit.", ""]
    # Disambiguate with the "<repo-dir>/src" suffix rather than an absolute
    # path: it is unique (no example app lives under <repo-dir>/), and it avoids
    # a doxygen bug that truncates a \dir path at the first dot-prefixed
    # component (e.g. a ".../.claude/worktrees/.../src" checkout).
    for name, brief in COLLIDING_TOP_DIRS.items():
        out.append("/**")
        out.append(f" * @dir {ROOT.name}/{name}")
        out.append(f" * @brief {brief}")
        out.append(" */")
        out.append("")
    return "\n".join(out) + "\n"


# --- Doxyfile @INCLUDE fragment ----------------------------------------------
# Directories that hold first-party Python/shell tooling but no firmware. Their
# .py modules are parsed by doxygen as "namespaces" whose classes then pollute
# the C firmware's Data Structures list. We keep the files browsable under Files
# but drop their module symbols from the API via a generated EXCLUDE_SYMBOLS.
PY_TOOL_DIRS = ["scripts", "tools", "libs", "src", "examples"]
CONFIG_SKIP = {"third_party", "build", "__pycache__", "_deps", "doxygen"}


def python_module_symbols() -> list[str]:
    """Collect the module STEMS of every first-party Python file, deduplicated.

    Doxygen parses a .py file as a namespace named after its stem, and those
    namespaces' classes then appear in the C firmware's Data Structures list.
    Feeding these to EXCLUDE_SYMBOLS drops the symbols while leaving the files
    themselves browsable under Files.

    Stems, not paths: EXCLUDE_SYMBOLS matches symbol names, so two like-named
    modules in different directories collapse to one entry -- which is correct
    here, since both would produce the same namespace name.
    """
    mods: set[str] = set()
    for top in PY_TOOL_DIRS:
        base = ROOT / top
        if not base.is_dir():
            continue
        for py in base.rglob("*.py"):
            if any(part in CONFIG_SKIP for part in py.parts):
                continue
            mods.add(py.stem)
    return sorted(mods)


def gen_config() -> str:
    """Render the EXCLUDE_SYMBOLS fragment the Doxyfile ``@INCLUDE``s.

    Emitted as a Doxyfile fragment rather than edited into the Doxyfile so the
    list regenerates with the tree; a new tooling module is excluded the next
    time the docs build runs, with nothing to remember to update.

    Returns the file text, newline-terminated.
    """
    mods = python_module_symbols()
    lines = [
        "# GENERATED by scripts/gen/gen_doxygen_nav.py -- do not edit.",
        "# Drop first-party Python tooling module symbols from the API so the",
        "# C firmware's Data Structures / Topics lists stay clean. The .py files",
        "# themselves remain listed and browsable under Files.",
        "EXCLUDE_SYMBOLS = " + " \\\n                  ".join(mods),
    ]
    return "\n".join(lines) + "\n"


def main() -> int:
    """Write all four generated navigation files into docs/generated/.

    build_docs.sh runs this immediately before doxygen, so the four files are
    always regenerated from the current tree rather than read from a previous
    build -- which is what makes stale navigation structurally impossible
    rather than merely unlikely.

    Output is written as ASCII, so a non-ASCII character reaching a README
    blurb fails here loudly instead of producing mojibake in the rendered
    site; the repo is ASCII-only by policy and this is where that is enforced
    for generated navigation.

    Returns 0; failures surface as exceptions rather than a status code, since
    every one of them (unwritable output, non-ASCII input) is a build fault
    with no partial-success reading.
    """
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    (OUT_DIR / "nav_examples.dox").write_text(gen_examples(), encoding="ascii")
    (OUT_DIR / "nav_docs.dox").write_text(gen_docs(), encoding="ascii")
    (OUT_DIR / "nav_dirs.dox").write_text(gen_dirs(), encoding="ascii")
    (OUT_DIR / "nav_config.doxy").write_text(gen_config(), encoding="ascii")
    print(
        f"gen_doxygen_nav: wrote {OUT_DIR}/nav_examples.dox, nav_docs.dox, "
        "nav_dirs.dox, nav_config.doxy"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
