#!/usr/bin/env python3
"""Every completed bench/silicon verification claim must say where its evidence lives.

A verification claim that names no evidence cannot go stale, because there is
nothing to check it against. That is not a hypothetical: #710 found
``coprocessor/esp32c6/build.sh`` asserting the recipe had been "built, flashed
and booted on the bench" in a comment written before the media component
existed. The claim was true when it was written, silently widened to cover code
added 19 days later, and nothing in the tree could tell the difference.

So this checker asks one mechanical question of every completed hardware
verification claim in first-party sources: does its own comment or prose block
name a LOCATOR -- a date, a commit, an issue, a file, or a backticked app,
symbol or artifact -- that a reader can go and check? A claim that names one can
be audited and can be found stale. A claim that names none cannot.

What is a claim: a verification verb in the completed sense bound to hardware
(``bench-proven``, ``silicon-validated``, ``verified on the bench``,
``measured on silicon``, ``HIL-validated``). "On the bench" alone is not one --
the tree uses it constantly as a location ("the bench host", "the bench Pi") --
and neither is a negated or forward-looking statement ("has not been captured
on the bench", "needs a bench run", "until it is re-run on the bench"), which
is the honest shape this gate exists to encourage rather than punish.

What is a locator, in the block the claim sits in (not merely its own line, so
a multi-line Doxygen comment qualifies as a unit the way a reader reads it):

  - an ISO date              ``BENCH-PROVEN 2026-07-27``
  - a commit                 ``verified at 83f2d1e67``
  - an issue or PR           ``#490 disproved it on the bench``
  - a backticked name        ``the silicon-validated `dtc_transfer_demo```
  - a file path              ``recorded in coprocessor/esp32c6/pins.env``
  - ``project memory``       the repo's own evidence store

Usage:
  scripts/checks/check_bench_claims.py [--selftest] [PATHS...]

Exit codes: 0 clean, 1 findings, 2 usage/internal error.
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

# A completed hardware-verification assertion. Deliberately NOT "on the bench"
# on its own: that is a location in most of this tree, not a claim.
CLAIM_RE = re.compile(
    r"\b("
    r"bench[- ](?:verified|validated|qualified|proven|tested)"
    r"|silicon[- ](?:verified|validated|proven|qualified)"
    r"|HIL[- ](?:verified|validated|proven)"
    r"|(?:verified|validated|proven|confirmed|qualified|measured|captured)"
    r"\s+(?:\w+\s+){0,3}?on\s+(?:the\s+)?(?:bench|silicon|hardware|board|target)\b"
    r")",
    re.IGNORECASE,
)

# Negated, hedged or forward-looking: "has not been captured on the bench",
# "needs a bench run", "until it is re-run on the bench". These are the honest
# shape; they assert no completed verification, so they need no locator.
HEDGE_RE = re.compile(
    r"\b(not|never|no longer|cannot|can't|until|needs?|still|when|if|would|should"
    r"|must|pending|unproven|refus\w*|yet|re-?run|re-?valid\w*|awaiting|blocked"
    r"|before|once|assume\w*|would-be)\b",
    re.IGNORECASE,
)

# Where the evidence lives. Any one of these makes the claim checkable.
LOCATOR_RE = re.compile(
    r"(20\d\d-\d\d-\d\d"
    r"|\b[0-9a-f]{8,40}\b"
    r"|#\d+"
    r"|``[^`]+``"
    r"|`[^`]+`"
    r"|\b[\w./-]+\.(?:c|h|py|sh|env|md|conf|cmake|yml|yaml|json|toml|txt)\b"
    r"|project memory"
    r")",
    re.IGNORECASE,
)

# Vendored, generated and other-lane trees are not ours to word.
SKIP_PREFIXES = (
    "libs/third_party/",
    "docs/sbom/",
    "tools/ra8_emulator",
    "tools/ra8ci/",
)

TEXT_SUFFIXES = {
    ".c", ".h", ".cpp", ".hpp", ".py", ".sh", ".bash", ".md", ".cmake", ".conf",
    ".env", ".yml", ".yaml", ".just", ".txt", ".toml", ".json", ".ld", ".s",
}

# A collapsed read (a bad path argument, a changed layout) must not pass as
# clean. The tree held 90 claims when this gate was written.
CLAIM_FLOOR = 40


def repo_root() -> Path:
    here = Path(__file__).resolve()
    return here.parent.parent.parent


def tracked_files(root: Path) -> list[str]:
    out = subprocess.run(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
        cwd=root, capture_output=True, text=True, check=False,
    ).stdout
    return [p for p in out.split("\0") if p]


def is_candidate(rel: str) -> bool:
    if rel.startswith(SKIP_PREFIXES):
        return False
    suffix = Path(rel).suffix
    if suffix:
        return suffix in TEXT_SUFFIXES
    # extensionless: only the handful of prose files we actually word
    return Path(rel).name in {"justfile", "Dockerfile"}


def block_bounds(lines: list[str], idx: int) -> tuple[int, int]:
    """The contiguous non-blank run around lines[idx], 0-based inclusive.

    A blank line ends a comment block in C, a paragraph in Markdown and a
    stanza in shell or Kconfig alike, so one rule covers every file we read.
    """
    start = idx
    while start > 0 and lines[start - 1].strip():
        start -= 1
    end = idx
    while end + 1 < len(lines) and lines[end + 1].strip():
        end += 1
    return start, end


def scan_text(rel: str, text: str) -> tuple[int, list[tuple[str, int, str]]]:
    """Return (claims seen, findings) for one file's contents."""
    lines = text.splitlines()
    seen = 0
    findings: list[tuple[str, int, str]] = []
    for idx, line in enumerate(lines):
        if not CLAIM_RE.search(line):
            continue
        seen += 1
        if HEDGE_RE.search(line):
            continue
        start, end = block_bounds(lines, idx)
        block = "\n".join(lines[start:end + 1])
        if LOCATOR_RE.search(block):
            continue
        findings.append((rel, idx + 1, line.strip()))
    return seen, findings


def scan(root: Path, rels: list[str] | None = None) -> tuple[int, list[tuple[str, int, str]]]:
    if rels is None:
        rels = [r for r in tracked_files(root) if is_candidate(r)]
    total = 0
    findings: list[tuple[str, int, str]] = []
    for rel in rels:
        path = root / rel
        try:
            text = path.read_text(encoding="utf-8", errors="ignore")
        except OSError:
            continue
        seen, found = scan_text(rel, text)
        total += seen
        findings.extend(found)
    return total, findings


# ---------------------------------------------------------------- selftest

_CASES: list[tuple[str, str, str, int]] = [
    # (name, filename, body, expected finding count)
    (
        "a dated marker is quiet",
        "pins.env",
        "# BENCH-PROVEN 2026-07-27: the link came up at SPI mode 3.\n",
        0,
    ),
    (
        "a bare claim fires",
        "main.c",
        "/* The CEU keeps the bench-proven 640x480 configuration. */\n",
        1,
    ),
    (
        "a backticked app is a locator",
        "README.md",
        "The activation path is byte-identical to the\n"
        "silicon-validated `dtc_transfer_demo`.\n",
        0,
    ),
    (
        "an issue reference is a locator",
        "notes.md",
        "It was never a protobuf-free piece of work; #490 disproved that on the bench.\n",
        0,
    ),
    (
        "a commit is a locator",
        "notes.md",
        "The image was verified on the bench at 83f2d1e67 before the split.\n",
        0,
    ),
    (
        "a file path is a locator",
        "pins.h",
        " * Both ends of the link are bench-proven and both are recorded in\n"
        " * coprocessor/esp32c6/pins.env as C6_PIN_ entries.\n",
        0,
    ),
    (
        "the locator may sit elsewhere in the same block",
        "main.c",
        "/* The standby entry is the bench-proven sequence.\n"
        " * See `lpm_ulpt_standby` for the captured trace. */\n",
        0,
    ),
    (
        "a blank line ends the block, so a later locator does not count",
        "main.c",
        "/* The standby entry is the bench-proven sequence. */\n"
        "\n"
        "/* See `lpm_ulpt_standby` for the captured trace. */\n",
        1,
    ),
    (
        "a negated claim needs no locator",
        "README.md",
        "The xSPI leg has not been captured on the bench.\n",
        0,
    ),
    (
        "a forward-looking claim needs no locator",
        "README.md",
        "The app must move until it is re-run on the bench.\n",
        0,
    ),
    (
        "the bench host is a location, not a claim",
        "bench.sh",
        "# The witness's home on the bench host, sampled from /proc.\n",
        0,
    ),
    (
        "the bench Pi is a location, not a claim",
        "rig_env.sh",
        "#   PI_REPO  path to the checkout on the bench Pi.\n",
        0,
    ),
    (
        "measured on silicon is a claim",
        "main.c",
        "/* The 0.29 s release was measured on silicon. */\n",
        1,
    ),
    (
        "HIL-validated is a claim",
        "hil.conf",
        "# The scrape verdict is HIL-validated.\n",
        1,
    ),
    (
        "two bare claims in one block report once each",
        "main.c",
        "/* Bench-proven rate.\n"
        " * Silicon-verified window. */\n",
        2,
    ),
]


def selftest() -> int:
    failures = 0
    checked = 0
    for name, filename, body, expect in _CASES:
        _, found = scan_text(filename, body)
        checked += 1
        if len(found) != expect:
            failures += 1
            print(f"  [FAIL] {name}: expected {expect} finding(s), got {len(found)}")
            for rel, line, text in found:
                print(f"         {rel}:{line}: {text}")
        else:
            print(f"  [ok] {name}")

    # A candidate filter that accepted nothing, or a floor that could never
    # trip, would make the real scan vacuous. Assert both ends here.
    if not is_candidate("libs/ra8_hal/src/ra8_drw_draw.c"):
        failures += 1
        print("  [FAIL] a first-party source is not a candidate")
    elif is_candidate("libs/third_party/usbx/common/ux_device_class_dfu_activate.c"):
        failures += 1
        print("  [FAIL] a vendored source is a candidate")
    else:
        checked += 1
        print("  [ok] the candidate filter takes first-party sources and skips vendored ones")

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "a.c").write_text("/* Bench-proven rate. */\n", encoding="utf-8")
        total, found = scan(root, ["a.c"])
        checked += 1
        if total != 1 or len(found) != 1:
            failures += 1
            print(f"  [FAIL] scan over a path list: total={total} findings={len(found)}")
        else:
            print("  [ok] scan honours an explicit path list")

    print(f"selftest: {checked} assertion(s), {failures} failure(s)")
    return 1 if failures else 0


def main(argv: list[str]) -> int:
    args = argv[1:]
    if "--selftest" in args:
        return selftest()

    root = repo_root()
    rels = [a for a in args if not a.startswith("-")] or None
    if rels is not None:
        rels = [os.path.relpath(Path(r).resolve(), root) for r in rels]

    total, findings = scan(root, rels)

    if rels is None and total < CLAIM_FLOOR:
        print(
            f"{Path(__file__).name}: only {total} verification claim(s) found, "
            f"floor is {CLAIM_FLOOR} -- the scan collapsed rather than the tree "
            f"having got cleaner. Check the candidate filter.",
            file=sys.stderr,
        )
        return 2

    if findings:
        print(
            f"{Path(__file__).name}: {len(findings)} verification claim(s) name no "
            f"evidence:",
            file=sys.stderr,
        )
        for rel, line, text in findings:
            print(f"  {rel}:{line}: {text}", file=sys.stderr)
        print(
            "\nEach asserts a completed bench or silicon verification. Add a locator "
            "to the same comment or prose block -- a date, a commit, an issue, a file "
            "path, or a backticked app, symbol or artifact that holds the evidence -- "
            "or reword the claim to say what is actually still unproven.",
            file=sys.stderr,
        )
        return 1

    print(
        f"{Path(__file__).name}: clean -- {total} verification claim(s), "
        f"each naming its evidence"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
