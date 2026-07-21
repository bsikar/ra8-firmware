#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: every authored diagram block must actually render in the built HTML.

An authored Graphviz block is written as ``@dot`` ... ``@enddot`` inside a
Markdown page or a Doxygen comment.  Doxygen renders each one by shelling out
to ``dot`` and emitting ``<img src="dot_inline_dotgraph_<md5>.svg">`` into the
page.  Nothing in this repository ever checked that the second half happened.

That gap is not hypothetical.  Three separate failure modes drop a diagram
while leaving the page HTTP 200, the prose intact and every existing gate
green:

* ``HAVE_DOT=NO`` -- ``build_docs.sh`` degrades to text-only output when
  ``dot`` is absent from ``PATH``.  Doxygen then ignores every authored block,
  the published site loses every diagram, and nothing fails.
* ``PLANTUML_JAR_PATH`` unset -- doxygen ignored every ``@startuml`` block and
  emitted one warning per block, which ``scripts/ci.sh`` explicitly filtered
  out.  24 state-machine diagrams rendered nowhere for the entire life of the
  tree while ``CLAUDE.md`` mandated the construct that produced them.
* A block doxygen parses but ``dot`` fails to lay out yields an SVG file with
  no graph content in it -- present, referenced, and empty.

So this gate reads the *generated output*, not the configuration.
Configuration is what everyone checked; output is what was broken.

Counting model
--------------
Comparing raw embed counts does not work: doxygen renders one comment block on
both the file page and the topic page it belongs to, so a single ``@dot``
legitimately produces several ``<img>`` embeds.  Doxygen also names each
rendered SVG after a hash of the block body, so two byte-identical blocks share
one file.  The invariant that survives both effects is:

    distinct authored block bodies  ==  distinct rendered SVGs referenced

Both sides are deduplicated by content, which is the identity doxygen itself
uses.

Usage::

    check_doc_diagrams.py --html build/docs-gate/html   # the gate
    check_doc_diagrams.py --selftest                    # both-direction proof

Exit 0 when clean, 1 on findings, 2 on a selftest failure or a missing ``dot``.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import shutil
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

#: Roots scanned for authored diagram blocks.  Mirrors the CLAUDE.md scope
#: note: every first-party file, not just the firmware.
SCAN_ROOTS = ("docs", "libs", "src", "port", "examples", "tools", "tests")

#: Suffixes that can carry a doxygen comment or a doxygen-rendered page.
SCAN_SUFFIXES = (".md", ".c", ".h", ".cpp", ".hpp", ".dox")

#: Path fragments marking vendored or generated trees, which are out of scope.
EXCLUDED = ("third_party", "/build/", "/generated/", "doxygen_theme")

#: Files allowed to name the banned construct while documenting the rule.
#: They describe the policy; they do not carry a renderable block.
STARTUML_DOC_ALLOWLIST = (
    "docs/STYLE_GUIDE.md",
    "docs/DOCS.md",
    "docs/formats/BINARY_FORMATS.md",
)

#: Written into the generated HTML tree by build_docs.sh once a build finishes.
#: Its contents are the fingerprint of the authored diagram set that build saw,
#: which is what lets this gate tell "output built from the tree under test"
#: apart from "whatever HTML was lying around".
STAMP_NAME = ".ra8-diagram-stamp"

#: Index of the optional stamp-mode field in a selftest case tuple.
STAMP_MODE_FIELD = 4

DOT_OPEN = re.compile(r"^\s*(?:\*\s?)?@dot\s*$")
DOT_CLOSE = re.compile(r"^\s*(?:\*\s?)?@enddot\s*$")
SVG_REF = re.compile(r"dot_inline_dotgraph_[0-9a-f]+\.svg")
SVG_TEXT = re.compile(r"<text[^>]*>([^<]*)</text>")
COMMENT_LEADER = re.compile(r"^\s*\*\s?")


def in_scope(path: Path) -> bool:
    posix = path.as_posix()
    if any(frag in posix for frag in EXCLUDED):
        return False
    return path.suffix in SCAN_SUFFIXES


def iter_sources(root: Path):
    for top in SCAN_ROOTS:
        base = root / top
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.is_file() and not path.is_symlink() and in_scope(path):
                yield path


def normalise(body_lines: list[str]) -> str:
    """Reduce a block body to the text doxygen hashes.

    Strips the Doxygen comment leader (`` * ``) and trailing whitespace, so the
    same diagram written in a ``.h`` comment and in a ``.md`` page normalises
    identically -- which is what makes them collide into one rendered SVG.
    """
    out = [COMMENT_LEADER.sub("", ln).rstrip() for ln in body_lines]
    while out and not out[0]:
        out.pop(0)
    while out and not out[-1]:
        out.pop()
    return "\n".join(out)


class Finding:
    """One rule violation, rendered as ``where: message``."""

    def __init__(self, where: str, message: str):
        self.where = where
        self.message = message

    def __str__(self) -> str:
        return f"{self.where}: {self.message}"


def _scan_startuml(rel: str, lines: list[str]) -> list[Finding]:
    """Reject the construct that renders nowhere in this tree."""
    if rel in STARTUML_DOC_ALLOWLIST:
        return []
    return [
        Finding(
            f"{rel}:{idx}",
            "@startuml renders nothing (PLANTUML_JAR_PATH is unset and no "
            "JVM is provisioned) -- use @dot",
        )
        for idx, line in enumerate(lines, 1)
        if "@startuml" in line
    ]


def _scan_dot_blocks(rel: str, lines: list[str], blocks: dict[str, list[str]]):
    """Collect ``@dot`` bodies into ``blocks``; return marker findings."""
    findings: list[Finding] = []
    open_at: int | None = None
    body: list[str] = []

    for idx, line in enumerate(lines, 1):
        if DOT_OPEN.match(line):
            if open_at is not None:
                findings.append(Finding(f"{rel}:{idx}", "nested @dot: previous block never closed"))
            open_at, body = idx, []
        elif DOT_CLOSE.match(line):
            if open_at is None:
                findings.append(Finding(f"{rel}:{idx}", "@enddot without @dot"))
                continue
            key = normalise(body)
            if not key:
                findings.append(Finding(f"{rel}:{open_at}", "empty @dot block"))
            blocks.setdefault(key, []).append(f"{rel}:{open_at}")
            open_at = None
        elif open_at is not None:
            body.append(line)

    if open_at is not None:
        findings.append(Finding(f"{rel}:{open_at}", "@dot block never closed"))
    return findings


def scan_sources(root: Path, files=None):
    """Return ``(blocks, findings)``; ``blocks`` maps normalised body -> origins."""
    blocks: dict[str, list[str]] = {}
    findings: list[Finding] = []
    sources = files if files is not None else list(iter_sources(root))

    for path in sources:
        try:
            text = path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        rel = path.relative_to(root).as_posix() if path.is_absolute() else path.as_posix()
        lines = text.splitlines()
        findings += _scan_startuml(rel, lines)
        findings += _scan_dot_blocks(rel, lines, blocks)

    return blocks, findings


def scan_html(html_dir: Path):
    """Return ``(referenced, live, findings)`` for diagrams under ``html_dir``."""
    findings: list[Finding] = []
    referenced: set[str] = set()

    for page in sorted(html_dir.rglob("*.html")):
        try:
            text = page.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        referenced.update(SVG_REF.findall(text))

    live: set[str] = set()
    for name in sorted(referenced):
        svg = html_dir / name
        if not svg.is_file():
            findings.append(Finding(name, "referenced by a page but not generated"))
            continue
        content = svg.read_text(encoding="utf-8", errors="replace")
        # A laid-out graphviz SVG always carries at least one node group.
        if 'class="node"' not in content:
            findings.append(
                Finding(name, "rendered SVG contains no graph nodes (dot produced an empty layout)")
            )
            continue
        # A diagram can render and still be wrong. `\\n` in a dot label is an
        # escaped backslash followed by 'n', so graphviz draws the characters
        # "\n" instead of breaking the line -- the label reads as one run of
        # text with literal escapes in it. This has shipped twice, so catch it
        # in the rendered text rather than trusting the source spelling.
        literal = [t for t in SVG_TEXT.findall(content) if "\\n" in t]
        if literal:
            findings.append(
                Finding(
                    name,
                    "rendered label contains a literal '\\n' -- the dot source "
                    "double-escaped a line break (use \\n, not \\\\n): "
                    + "; ".join(sorted(literal)[:3]),
                )
            )
            continue
        live.add(name)

    return referenced, live, findings


def source_fingerprint(blocks: dict[str, list[str]]) -> str:
    """Fingerprint the authored diagram set, order-independently."""
    digest = hashlib.sha256()
    for key in sorted(blocks):
        digest.update(key.encode("utf-8"))
        digest.update(b"\0")
    return digest.hexdigest()


def check_stamp(html_dir: Path, blocks: dict[str, list[str]]) -> list[Finding]:
    """Refuse to judge output that was not built from the tree under test.

    Doxygen never deletes what it no longer produces, so a directory can hold a
    mixture of two builds. That is dangerous in both directions: an orphan
    render can mask a diagram the current tree drops, and a diagram added since
    the last build looks dropped. Rather than trusting the caller to have
    rebuilt, compare the authored diagram set against the fingerprint the build
    recorded.
    """
    stamp = html_dir / STAMP_NAME
    if not stamp.is_file():
        return [
            Finding(
                str(html_dir),
                "no build stamp -- this HTML was not produced by scripts/build_docs.sh, "
                "so it cannot be trusted to reflect the current tree. Rebuild with "
                "'bash scripts/build_docs.sh --gate'.",
            )
        ]
    recorded = stamp.read_text(encoding="utf-8").strip()
    current = source_fingerprint(blocks)
    if recorded != current:
        return [
            Finding(
                str(html_dir),
                "stale output -- the authored diagram set has changed since this "
                f"HTML was built (stamp {recorded[:12]}, tree {current[:12]}). "
                "Rebuild with 'bash scripts/build_docs.sh --gate'; do not judge "
                "the diagrams from a mixture of two builds.",
            )
        ]
    return []


def check(root: Path, html_dir: Path) -> list[Finding]:
    """Run every rule and return the accumulated findings."""
    blocks, findings = scan_sources(root)

    # Freshness first: every count below is meaningless against stale output,
    # and reporting a bogus count mismatch is how a gate teaches people to
    # `rm -rf` and re-run until green.
    stale = check_stamp(html_dir, blocks)
    if stale:
        return findings + stale

    _referenced, live, html_findings = scan_html(html_dir)
    findings += html_findings

    n_src, n_live = len(blocks), len(live)
    if n_src != n_live:
        detail = [
            "",
            f"  distinct authored @dot blocks : {n_src}",
            f"  distinct rendered  SVGs       : {n_live}",
            "",
        ]
        if n_src > n_live:
            # Report SOURCE files, never rendered-SVG filenames. Doxygen names
            # each SVG after a hash of its own normalised copy of the block, and
            # that normalisation is not reproducible from the source text, so an
            # SVG name cannot be attributed back to the block that produced it.
            # Printing one anyway names a file the author never touched, which
            # is exactly what made this hard to read the first time it fired.
            per_file: dict[str, int] = {}
            for origins in blocks.values():
                for origin in origins:
                    per_file[origin.rsplit(":", 1)[0]] = (
                        per_file.get(origin.rsplit(":", 1)[0], 0) + 1
                    )
            detail.append(f"  {n_src - n_live} authored diagram(s) did not reach the HTML.")
            detail.append("  Authored @dot blocks by source file:")
            detail.extend(f"    {count:3d}  {path}" for path, count in sorted(per_file.items()))
            detail.append("")
            detail.append("  The build is fresh (stamp matched), so a block in one of these")
            detail.append("  files was parsed but not rendered. Check it for a dot syntax")
            detail.append("  error, or a node count over DOT_GRAPH_MAX_NODES.")
        else:
            detail.append(
                "  More rendered diagrams than authored blocks -- a scan root "
                "or suffix is missing from this gate."
            )
        findings.append(
            Finding(
                "diagram-count", "authored and rendered diagram counts disagree" + "\n".join(detail)
            )
        )
    return findings


# ---------------------------------------------------------------------------
# Selftest
# ---------------------------------------------------------------------------


def _svg(*, nodes: bool, label: str = "Idle") -> str:
    body = f'<g class="node"><title>A</title><text x="0" y="0">{label}</text></g>' if nodes else ""
    return f'<?xml version="1.0"?><svg xmlns="http://www.w3.org/2000/svg">{body}</svg>'


def _md(*bodies: str) -> str:
    return "Prose paragraph.\n\n" + "\n".join("@dot\n" + b + "\n@enddot\n" for b in bodies)


def _page(*svgs: str) -> str:
    imgs = "".join(f'<div class="dotgraph"><img src="{s}"/></div>' for s in svgs)
    return f"<html><body>{imgs}</body></html>"


def _hashed(body: str) -> str:
    """Synthesise a doxygen-style SVG filename for a selftest fixture.

    Not a security primitive: this only has to be a stable, unique-per-body
    name so the fixtures can wire a page to its SVG the way doxygen does.
    """
    digest = hashlib.md5(body.encode(), usedforsecurity=False).hexdigest()
    return f"dot_inline_dotgraph_{digest}.svg"


def selftest() -> int:
    """Prove the gate fires when a diagram is dropped and is silent when not."""
    g1 = "digraph a { x -> y; }"
    g2 = "digraph b { p -> q; }"
    s1, s2 = _hashed(g1), _hashed(g2)

    dup_header = (
        "/**\n * @dot\n" + "\n".join(" * " + ln for ln in g1.splitlines()) + "\n * @enddot\n */\n"
    )

    cases = [
        # --- the gate must STAY SILENT on correct output ------------------
        (
            "correct: 2 authored, 2 rendered",
            {"docs/x.md": _md(g1, g2)},
            {"p.html": _page(s1, s2), s1: _svg(nodes=True), s2: _svg(nodes=True)},
            False,
        ),
        (
            "correct: one block embedded on two pages",
            {"docs/x.md": _md(g1)},
            {"a.html": _page(s1), "b.html": _page(s1), s1: _svg(nodes=True)},
            False,
        ),
        (
            "correct: identical blocks share one SVG",
            {"docs/x.md": _md(g1), "libs/y.h": dup_header},
            {"p.html": _page(s1), s1: _svg(nodes=True)},
            False,
        ),
        # --- the gate must FIRE when a diagram is dropped -----------------
        (
            "dropped: 2 authored, 1 rendered",
            {"docs/x.md": _md(g1, g2)},
            {"p.html": _page(s1), s1: _svg(nodes=True)},
            True,
        ),
        (
            "dropped: every diagram missing (HAVE_DOT=NO)",
            {"docs/x.md": _md(g1, g2)},
            {"p.html": "<html><body>no diagrams</body></html>"},
            True,
        ),
        (
            "empty: rendered SVG has no nodes",
            {"docs/x.md": _md(g1)},
            {"p.html": _page(s1), s1: _svg(nodes=False)},
            True,
        ),
        (
            "double-escaped: label renders a literal backslash-n",
            {"docs/x.md": _md(g1)},
            {"p.html": _page(s1), s1: _svg(nodes=True, label="ra8_init\\nstep two")},
            True,
        ),
        (
            "missing: referenced SVG absent from disk",
            {"docs/x.md": _md(g1)},
            {"p.html": _page(s1)},
            True,
        ),
        (
            "startuml: banned construct present",
            {"docs/x.md": "@startuml\n[*] --> A\n@enduml\n"},
            {"p.html": "<html></html>"},
            True,
        ),
        (
            "unbalanced: @dot never closed",
            {"docs/x.md": "@dot\ndigraph z { a -> b; }\n"},
            {"p.html": "<html></html>"},
            True,
        ),
        # --- the gate must REFUSE output it cannot attribute to this tree ----
        # Stale output is dangerous in both directions, and the dangerous one
        # is a leftover render MASKING a diagram the current tree drops. Both
        # of these would otherwise "pass" on the counts alone.
        (
            "stale: leftover render masks a dropped diagram",
            # Two blocks authored; only one is rendered, but a leftover SVG
            # from an older build makes the count add up.
            {"docs/x.md": _md(g1, g2)},
            {"p.html": _page(s1), s1: _svg(nodes=True), s2: _svg(nodes=True)},
            True,
            "wrong",
        ),
        (
            "stale: output predates a newly added diagram",
            {"docs/x.md": _md(g1, g2)},
            {"p.html": _page(s1, s2), s1: _svg(nodes=True), s2: _svg(nodes=True)},
            True,
            "wrong",
        ),
        (
            "unknown provenance: no build stamp at all",
            {"docs/x.md": _md(g1)},
            {"p.html": _page(s1), s1: _svg(nodes=True)},
            True,
            "missing",
        ),
    ]

    failures = 0
    for case in cases:
        name, sources, html, expect_fire = case[:4]
        # How the build stamp is seeded. "good" models a directory that
        # build_docs.sh has just written; the others model output that was not
        # built from the tree under test.
        stamp_mode = case[STAMP_MODE_FIELD] if len(case) > STAMP_MODE_FIELD else "good"
        with tempfile.TemporaryDirectory() as td:
            root = Path(td) / "repo"
            hdir = Path(td) / "html"
            hdir.mkdir(parents=True)
            for rel, text in sources.items():
                path = root / rel
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text, encoding="utf-8")
            for rel, text in html.items():
                (hdir / rel).write_text(text, encoding="utf-8")

            if stamp_mode == "good":
                seen, _ = scan_sources(root)
                (hdir / STAMP_NAME).write_text(source_fingerprint(seen), encoding="utf-8")
            elif stamp_mode == "wrong":
                (hdir / STAMP_NAME).write_text("0" * 64, encoding="utf-8")
            # "missing" writes nothing at all.

            fired = bool(check(root, hdir))
            if fired != expect_fire:
                want = "FIRE" if expect_fire else "stay silent"
                got = "fired" if fired else "stayed silent"
                sys.stderr.write(
                    f"  selftest FAIL [{name}]: expected the gate to {want}, it {got}\n"
                )
                failures += 1
            else:
                verdict = "fires" if expect_fire else "silent"
                sys.stdout.write(f"  ok ({verdict:6s}) {name}\n")

    total = len(cases)
    if failures:
        sys.stderr.write(f"check_doc_diagrams.py: selftest FAILED ({failures}/{total} cases).\n")
        return 2
    fires = sum(1 for case in cases if case[3])
    sys.stdout.write(
        f"check_doc_diagrams.py: selftest PASSED ({total} cases -- "
        f"{fires} assert the gate fires, {total - fires} assert it does not).\n"
    )
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--html", default="build/docs-gate/html", help="generated HTML directory to inspect"
    )
    parser.add_argument(
        "--selftest", action="store_true", help="run synthetic both-direction fixtures and exit"
    )
    parser.add_argument(
        "--write-stamp",
        action="store_true",
        help="record the authored diagram fingerprint into the HTML tree "
        "(called by scripts/build_docs.sh once a build finishes)",
    )
    args = parser.parse_args()

    if args.selftest:
        return selftest()

    # A gate whose dependency is absent must FAIL, never silently pass.
    if shutil.which("dot") is None:
        sys.stderr.write(
            "check_doc_diagrams.py: FATAL -- graphviz 'dot' is not on PATH.\n"
            "Every authored diagram is dropped without it, and this gate "
            "cannot tell that apart from a build that was never run.\n"
        )
        return 2

    html_dir = Path(args.html)
    if not html_dir.is_absolute():
        html_dir = REPO_ROOT / html_dir
    if not html_dir.is_dir():
        sys.stderr.write(
            f"check_doc_diagrams.py: FATAL -- no generated HTML at {html_dir}.\n"
            "Build the docs first (scripts/build_docs.sh --gate).\n"
        )
        return 2

    if args.write_stamp:
        blocks, _ = scan_sources(REPO_ROOT)
        (html_dir / STAMP_NAME).write_text(source_fingerprint(blocks), encoding="utf-8")
        sys.stdout.write(
            f"check_doc_diagrams.py: stamped {len(blocks)} authored diagram(s) "
            f"into {html_dir.relative_to(REPO_ROOT)}.\n"
        )
        return 0

    findings = check(REPO_ROOT, html_dir)
    if findings:
        sys.stderr.write("check_doc_diagrams.py: FAILED\n\n")
        for finding in findings:
            sys.stderr.write(f"  {finding}\n")
        # Keep the closing advice truthful. A provenance failure means "this
        # measurement is not valid yet", not "a diagram is missing" -- telling
        # someone their diagrams are gone when the real answer is "rebuild" is
        # how a gate earns a reputation for crying wolf.
        if any("stale output" in f.message or "no build stamp" in f.message for f in findings):
            sys.stderr.write(
                "\nNothing was measured: the generated HTML does not correspond "
                "to the current tree.\nRebuild and re-run before drawing any "
                "conclusion about the diagrams.\n"
            )
        else:
            sys.stderr.write(
                "\nAn authored diagram that does not reach the HTML is invisible "
                "to every reader of the published site.\n"
            )
        return 1

    blocks, _ = scan_sources(REPO_ROOT)
    sys.stdout.write(
        f"check_doc_diagrams.py: OK -- {len(blocks)} authored diagram(s) all "
        f"render in {html_dir.relative_to(REPO_ROOT)}.\n"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
