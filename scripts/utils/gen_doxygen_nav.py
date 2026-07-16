#!/usr/bin/env python3
# gen_doxygen_nav.py -- generate the Doxygen navigation trees for ra8-firmware.
#
# This script is the single generator behind the two curated navigation
# sections of the Doxygen site:
#
#   1. "Guides & Reference" -- the hand-written docs under docs/*.md, grouped
#      into a small, sensible section tree (Architecture, Conventions & Policy,
#      Safety & Certification, Hardware & Bring-up, Project & Reference) instead
#      of a flat heap. Directory sub-trees (docs/adr, docs/SOUP, ...) are picked
#      up automatically as sub-sections.
#
#   2. "Example Applications" -- every example app under examples/, organized by
#      the exact directory tiers the repo already uses (hw_validated/hil,
#      hw_validated/manual, hw_pending, _unsupported, ra8p1_foundation, ...),
#      each app row carrying the one-line description from its own README.
#
# It is deterministic and directory-driven: adding a new example app or a new
# docs/*.md page needs no edit here -- the app appears in its tier automatically,
# and an unclassified top-level doc lands in "Project & Reference" with a
# warning printed to stderr so it can be curated. Only a genuinely new top-level
# docs section or example board tier needs a one-line entry in the small maps
# below.
#
# The generated .dox files are written under docs/generated/ (gitignored) and
# consumed by the main Doxyfile. build_docs.sh runs this before doxygen, so the
# navigation can never drift from the tree.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT

from __future__ import annotations

import html
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
DOCS_DIR = ROOT / "docs"
EXAMPLES_DIR = ROOT / "examples"
OUT_DIR = DOCS_DIR / "generated"

# --- Doxygen id manglers -----------------------------------------------------
# These reproduce Doxygen 1.17's deterministic naming so the generated @subpage
# / link targets resolve without editing any source file. If Doxygen ever
# changed the scheme, the warnings gate would flag the unresolved references.


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


def doxy_file_id(relpath: str) -> str:
    """Doxygen file-page id for any input file (case preserved)."""
    out = []
    for ch in relpath:
        if ch == "/":
            out.append("_2")
        elif ch == "_":
            out.append("__")
        elif ch == ".":
            out.append("_8")
        else:
            out.append(ch)
    return "".join(out)


# --- narrative-docs taxonomy -------------------------------------------------
# Ordered list of (section id, title, brief). Order here == order in the tree.
DOC_SECTIONS: list[tuple[str, str, str]] = [
    (
        "architecture",
        "Architecture",
        "How the firmware is structured: rings/worlds, dual core, the memory "
        "map, the module tour, and driver status.",
    ),
    (
        "policy",
        "Conventions & Policy",
        "The rules every change is held to: style, annotations, citations, "
        "licensing, and the AI-attribution policy.",
    ),
    (
        "safety",
        "Safety & Certification",
        "The DO-178C Level B / IEC 61508 SIL 3 evidence: MC/DC, MISRA, "
        "coverage, static analysis, stack bounding, and the qualification kit.",
    ),
    (
        "hardware",
        "Hardware & Bring-up",
        "Board bring-up, the HIL bench rig, debugging, the toolchain, and "
        "performance / size reports.",
    ),
    (
        "reference",
        "Project & Reference",
        "Acronym glossary, the roadmap, and other quick-reference material.",
    ),
]

# Top-level docs/*.md basename (without extension) -> section id. This is the
# one curated map; anything missing defaults to "reference" with a warning.
DOC_CLASSIFY: dict[str, str] = {
    "ARCHITECTURE": "architecture",
    "RING_AND_WORLD": "architecture",
    "DUAL_CORE": "architecture",
    "MEMORY_MAP": "architecture",
    "MODULES": "architecture",
    "DRIVER_STATUS": "architecture",
    "STYLE_GUIDE": "policy",
    "ANNOTATIONS": "policy",
    "AI_ATTRIBUTION_POLICY": "policy",
    "CITATION_POLICY": "policy",
    "CONTENT_LICENSING": "policy",
    "VENDOR_BLOBS": "policy",
    "DOCS": "policy",
    "CERTIFICATION_SCOPE": "safety",
    "QUALIFICATION_ROADMAP": "safety",
    "MCDC": "safety",
    "MCDC_GAPS": "safety",
    "MCDC_DEACTIVATIONS": "safety",
    "MISRA": "safety",
    "COVERAGE": "safety",
    "STATIC_ANALYSIS": "safety",
    "STACK_USAGE": "safety",
    "FUZZING": "safety",
    "DOXYGEN_GAPS": "safety",
    "INIT_ORDER_AUDIT": "safety",
    "ROOT_OF_TRUST": "safety",
    "HARDWARE_BRINGUP": "hardware",
    "HIL_SUITE": "hardware",
    "HIL_DEVELOPER_WORKFLOW": "hardware",
    "DEBUG": "hardware",
    "TOOLCHAIN": "hardware",
    "PERFORMANCE": "hardware",
    "APP_SIZES": "hardware",
    "RA8D2_VS_RA8P1": "hardware",
    "EPUB_CONFORMANCE": "hardware",
    "ACRONYMS": "reference",
    "ROADMAP": "reference",
    "ROADMAP_DASHBOARD": "reference",
}

# docs/<subdir>/*.md become an automatic sub-section. Title + parent section is
# looked up here; an unknown subdir defaults to a prettified name under
# "reference" (still shown, just uncurated).
SUBDIR_TITLES: dict[str, str] = {
    "adr": "Architecture Decision Records",
    "SOUP": "SOUP component justifications",
    "qualification": "Qualification kit",
    "reference": "Datasheet reference",
}
SUBDIR_PARENT: dict[str, str] = {
    "adr": "safety",
    "SOUP": "safety",
    "qualification": "safety",
    "reference": "hardware",
}

# docs subdirs that are not narrative pages (vendored/generated/binary assets).
DOCS_SKIP_DIRS = {"generated", "doxygen", "doxygen_theme", "badges", "sbom"}

# --- example-tier taxonomy ---------------------------------------------------
# Pretty titles for the example directory tiers. Unknown tiers fall back to a
# prettified directory name so a new board tier still renders.
TIER_TITLES: dict[str, str] = {
    "ek_ra8d2": "EK-RA8D2 (stock evaluation kit)",
    "ek_ra8d2/hw_validated": "Hardware-validated",
    "ek_ra8d2/hw_validated/hil": "Hardware-in-the-loop (HIL)",
    "ek_ra8d2/hw_validated/manual": "Manual (jumper / button steps)",
    "ek_ra8d2/hw_pending": "Hardware-pending",
    "ek_ra8d2/hw_pending/manual": "Manual (jumper / button steps)",
    "_unsupported": "Needs external hardware",
    "ra8p1_foundation": "RA8P1 foundation",
}
TIER_BRIEFS: dict[str, str] = {
    "ek_ra8d2": "Apps that run on a stock EK-RA8D2 v1 kit with no added parts.",
    "_unsupported": "Apps that need hardware not on the stock board "
    "(motor driver, audio CODEC, external radios, ...).",
    "ra8p1_foundation": "Foundation apps for the RA8P1 variant (RA8D2 + Ethos-U55 NPU).",
}


def prettify(name: str) -> str:
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
    def __init__(self, path: str) -> None:
        self.path = path  # relative to examples/, "" for root
        self.apps: list[tuple[str, str, str]] = []  # (name, file_id, desc)
        self.children: dict[str, TierNode] = {}

    def title(self) -> str:
        if self.path in TIER_TITLES:
            return TIER_TITLES[self.path]
        return prettify(self.path.split("/")[-1])

    def page_id(self) -> str:
        if not self.path:
            return "ra8_examples"
        safe = self.path.replace("/", "_")
        safe = "".join(c if (c.isalnum() or c == "_") else "_" for c in safe)
        return "ra8_ex_" + safe


def build_example_tree() -> tuple[TierNode, int]:
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
        rel = app_dir.relative_to(ROOT).as_posix() + "/README.md"
        node.apps.append((app_dir.name, doxy_file_id(rel), read_app_desc(app_dir)))
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
    root, total = build_example_tree()
    out: list[str] = [
        "// GENERATED by scripts/utils/gen_doxygen_nav.py -- do not edit.",
        "",
        "/**",
        " * @page ra8_examples Example Applications",
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
def gen_docs() -> str:
    section_pages: dict[str, list[str]] = {sid: [] for sid, _, _ in DOC_SECTIONS}
    # sub-sections keyed by parent section id -> list of (subid, title, [ids])
    subsections: dict[str, list[tuple[str, str, list[str]]]] = {
        sid: [] for sid, _, _ in DOC_SECTIONS
    }

    # top-level docs/*.md
    top = sorted(p for p in DOCS_DIR.glob("*.md") if p.name.lower() != "readme.md")
    for md in top:
        base = md.stem
        section = DOC_CLASSIFY.get(base)
        if section is None:
            section = "reference"
            print(
                f"gen_doxygen_nav: docs/{md.name} is not classified -- "
                f"placing it under 'Project & Reference'.",
                file=sys.stderr,
            )
        rel = md.relative_to(ROOT).as_posix()
        section_pages[section].append(doxy_page_id(rel))

    # docs/<subdir>/*.md -> sub-sections
    for sub in sorted(p for p in DOCS_DIR.iterdir() if p.is_dir()):
        if sub.name in DOCS_SKIP_DIRS:
            continue
        md_files = sorted(p for p in sub.glob("*.md") if p.name.lower() != "readme.md")
        if not md_files:
            continue
        parent = SUBDIR_PARENT.get(sub.name, "reference")
        title = SUBDIR_TITLES.get(sub.name, prettify(sub.name))
        # Distinct "docsub" prefix so a subdir named like a section id (e.g.
        # docs/reference vs the "reference" section) cannot collide.
        subid = "ra8_docsub_" + "".join(
            c if (c.isalnum() or c == "_") else "_" for c in sub.name.lower()
        )
        ids = [doxy_page_id(p.relative_to(ROOT).as_posix()) for p in md_files]
        subsections[parent].append((subid, title, ids))

    out: list[str] = [
        "// GENERATED by scripts/utils/gen_doxygen_nav.py -- do not edit.",
        "",
        "/**",
        " * @page ra8_guides Guides & Reference",
        " * @brief The hand-written documentation, grouped into a few sensible",
        " *        sections. (The API reference lives under Topics; the source",
        " *        tree lives under Files.)",
        " *",
    ]
    emit_subpage_list(out, [(f"ra8_docs_{sid}", "") for sid, _t, _b in DOC_SECTIONS])
    out.append(" */")
    out.append("")

    for sid, title, brief in DOC_SECTIONS:
        out.append("/**")
        out.append(f" * @page ra8_docs_{sid} {title}")
        out.append(f" * @brief {brief}")
        out.append(" *")
        entries = [(pid, "") for pid in section_pages[sid]]
        entries += [(subid, "") for subid, _st, _ids in subsections[sid]]
        emit_subpage_list(out, entries)
        out.append(" */")
        out.append("")
        for subid, st, ids in subsections[sid]:
            out.append("/**")
            out.append(f" * @page {subid} {st}")
            out.append(" *")
            emit_subpage_list(out, [(pid, "") for pid in ids])
            out.append(" */")
            out.append("")

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
    out = ["// GENERATED by scripts/utils/gen_doxygen_nav.py -- do not edit.", ""]
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
    mods = python_module_symbols()
    lines = [
        "# GENERATED by scripts/utils/gen_doxygen_nav.py -- do not edit.",
        "# Drop first-party Python tooling module symbols from the API so the",
        "# C firmware's Data Structures / Topics lists stay clean. The .py files",
        "# themselves remain listed and browsable under Files.",
        "EXCLUDE_SYMBOLS = " + " \\\n                  ".join(mods),
    ]
    return "\n".join(lines) + "\n"


def main() -> int:
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
