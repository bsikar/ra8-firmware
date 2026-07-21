#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Pre-commit gate: ban AI attribution anywhere in the codebase.

Policy (see ``docs/AI_ATTRIBUTION_POLICY.md`` and ``CLAUDE.md``):

The literal token ``Claude`` is reserved EXCLUSIVELY for the project's own
``CLAUDE.md`` filename. No file in ``libs/``, ``src/``, ``tests/``,
``examples/``, ``port/``, ``scripts/``, ``docs/`` -- nor any other tracked
file -- may attribute code, tests, docs, or any other artifact to an AI tool
(Claude, GPT, Anthropic, OpenAI, Copilot, etc.).

Per-line opt-out: append ``AI-OK: <reason>`` (case-sensitive) to the line
that legitimately needs to mention the banned token (e.g. policy text in
``CLAUDE.md`` itself or ``CONTRIBUTING.md``).

Exit status:
  * 0 -- no violations found
  * 1 -- one or more violations (printed in ``<path>:<line>: ...`` form)
"""

from __future__ import annotations

import os
import re
import sys
from collections.abc import Iterable
from pathlib import Path

# Repo root = two parents up from this file (scripts/checks/<this>).
REPO_ROOT = Path(__file__).resolve().parents[2]

# Directories scanned. Anything outside these is ignored.
SCAN_DIRS = ("libs", "src", "tests", "examples", "port", "scripts", "docs")

# Plus a few top-level files that should also be policed.
EXTRA_TOP_LEVEL_FILES = ("CLAUDE.md", "CONTRIBUTING.md", "README.md")

# File extensions we inspect. Anything else (binaries, PDFs) is skipped.
TEXT_EXTS = {
    ".c",
    ".h",
    ".cpp",
    ".hpp",
    ".cc",
    ".md",
    ".txt",
    ".rst",
    ".py",
    ".sh",
    ".bash",
    ".cmake",
    ".yml",
    ".yaml",
    ".json",
    ".toml",
    ".cfg",
    ".conf",
    ".ini",
    ".tex",
    ".css",
    ".html",
    ".xml",
    ".ld",
    ".s",
    ".S",
}

# Always-skip path fragments.
SKIP_FRAGMENTS = (
    "/build/",
    "/_deps/",
    "/third_party/",
    "/node_modules/",
    "/.git/",
    "/__pycache__/",
    "/docs/doxygen/html/",
    "/docs/doxygen/xml/",
    "/docs/doxygen/latex/",
)

OPT_OUT_TAG = "AI-OK:"

EXCERPT_MAX_LEN = 160  # Maximum characters shown from a violating line before truncation.
EXCERPT_TRUNCATE_LEN = 157  # Length of truncated body (leaves room for "...").

# Self-exempt files that legitimately spell the banned tokens.
SELF_EXEMPT_FILES = {
    REPO_ROOT / "scripts" / "utils" / "check_no_ai_attribution.py",
    REPO_ROOT / "docs" / "AI_ATTRIBUTION_POLICY.md",
}

# ---------------------------------------------------------------------------
# Detection rules
# ---------------------------------------------------------------------------

# 1. ``claude`` -- case-insensitive word match, BUT allow:
#    - the literal filename ``CLAUDE.md`` (any case)
#    - paths that include ``CLAUDE.md``
#    - the ``.claude`` config directory (e.g. ``.claude``, ``./.claude/``)
RX_CLAUDE = re.compile(r"\bclaude\b", re.IGNORECASE)
RX_CLAUDE_MD_OK = re.compile(r"\bclaude\.md\b", re.IGNORECASE)
RX_DOT_CLAUDE_OK = re.compile(r"(?<![A-Za-z0-9_])\.claude(?![A-Za-z0-9_])", re.IGNORECASE)

# 2. ``gpt`` -- only flag suspicious AI-style usages. Renesas RA8D2 has
#    a GPT (general PWM timer) peripheral with channels ``GPT0``..``GPT13``
#    plus PDG sub-channels ``GPT320``..``GPT323``, so bare ``GPT``,
#    ``GPTn`` and ``ra8_gpt*`` are all legitimate hardware references.
#    Flag only the OpenAI product family (``ChatGPT``, ``GPT-3``,
#    ``GPT-4``, ``GPT-3.5``, ``GPT-4o``, ``OpenAI GPT``). Keep the dash
#    requirement -- ``GPT3`` without a separator is the Renesas channel.
RX_GPT_BAD = re.compile(
    r"\b(?:chatgpt|openai\s+gpt|gpt-[0-9](?:\.[0-9]+)?[a-z]*)\b",
    re.IGNORECASE,
)

# 3. ``anthropic`` -- always banned (no legitimate use).
RX_ANTHROPIC = re.compile(r"\banthropic\b", re.IGNORECASE)

# 4. ``openai`` -- always banned.
RX_OPENAI = re.compile(r"\bopenai\b", re.IGNORECASE)

# 5. ``copilot`` -- always banned.
RX_COPILOT = re.compile(r"\bcopilot\b", re.IGNORECASE)

# 5b. Other AI-coding-tool brands -- always banned.
#     Tightened to require word boundaries OR explicit domain to avoid
#     unrelated false positives (e.g. ``cursor`` as a database term in
#     SQL docs, ``cody`` as a person's name, ``continue`` as a C
#     keyword).
RX_OTHER_BRANDS = re.compile(
    r"\b("
    r"cursor\.(?:sh|com|so)|cursor\s+composer|"
    r"codeium|windsurf|"
    r"aider|aider\.chat|"
    r"continue\.dev|"
    r"cody\.dev|sourcegraph\s+cody|"
    r"tabnine|"
    r"devin\.ai|cognition\s+ai|"
    r"replit\s+ghostwriter|ghostwriter\s+ai|"
    r"codewhisperer|amazon\s+q\s+developer|"
    r"cline|roo\s+cline|roo\s+code|"
    r"jetbrains\s+ai|"
    r"google\s+gemini|gemini\s+code\s+assist|"
    r"google\s+bard|"
    r"meta\s+llama|llama\s+code|code\s+llama|"
    r"mistral\s+ai|codestral|"
    r"deepseek\s+coder|"
    r"qwen\s+coder|"
    r"phind\.com|"
    r"perplexity\.ai|"
    r"pieces\.app|"
    r"sweep\.dev|"
    r"smol\s+agents|smolagents|"
    r"phi-[0-9]|"
    r"x\.ai|grok\s+ai|"
    r"character\.ai|"
    r"hugging\s*face|"
    r"ollama|lm\s+studio|llama\.cpp|"
    r"stability\s+ai|"
    r"midjourney|"
    r"dall-?e-?[0-9]?|sora\s+ai|runway\s+ml|suno\s+ai"
    r")\b",
    re.IGNORECASE,
)

# 6. ``Co-Authored-By: <ai-name>`` lines. Brand list.
_AI_BRAND_LIST = (
    r"claude|anthropic|openai|chatgpt|codex|copilot|gpt|"
    r"cursor(?:\s+composer)?|aider|continue|cody|sourcegraph\s+cody|"
    r"tabnine|codeium|windsurf|devin|cognition|replit|ghostwriter|"
    r"codewhisperer|q\s+developer|cline|roo\s+(?:code|cline)|"
    r"jetbrains\s+ai|gemini|bard|llama|mistral|mixtral|deepseek|qwen|"
    r"phind|perplexity|pieces|sweep|smol(?:agents)?|phi|grok|xai|"
    r"inflection|character\.ai|hugging\s*face|ollama|lm\s*studio|"
    r"llama\.cpp|stability\s+ai|midjourney|dall-?e|sora|runway|suno"
)
RX_COAUTH = re.compile(
    rf"co-?authored-by:.*\b(?:{_AI_BRAND_LIST})\b",
    re.IGNORECASE,
)

# 6b. AI-bot email-address domains and usernames. Matches both
#     ``Author: ... <foo@anthropic.com>`` and bare email mentions in
#     prose / commit bodies. The ``[bot]`` user-suffix is the GitHub
#     convention for app-installed bots (``copilot[bot]``, etc.) and is
#     a strong signal that the commit was authored by an AI assistant.
RX_AI_EMAIL = re.compile(
    r"<?[a-zA-Z0-9._+-]+@("
    r"anthropic\.com|"
    r"openai\.com|"
    r"cursor\.(?:sh|com|so)|"
    r"codeium\.com|"
    r"tabnine\.com|"
    r"aider\.chat|"
    r"continue\.dev|"
    r"sourcegraph\.com|"
    r"perplexity\.ai|"
    r"mistral\.ai|"
    r"huggingface\.co|"
    r"deepseek\.com|"
    r"x\.ai|"
    r"character\.ai|"
    r"stability\.ai|"
    r"midjourney\.com|"
    r"runwayml\.com|"
    r"suno\.ai|"
    r"replit\.com|"
    r"jetbrains-ai\.com"
    r")>?",
    re.IGNORECASE,
)
# Match ANY ``[bot]`` suffix. The GitHub convention is that app-
# installed accounts append ``[bot]`` to their login name; legitimate
# CI bots (``github-actions[bot]``, ``dependabot[bot]``) and AI bots
# (``claude[bot]``, ``copilot[bot]``, ``cursor[bot]``) both use this
# suffix. Project policy: zero bot authorship. Per-line ``AI-OK:``
# opt-out is the escape hatch when a bot commit is genuinely desired.
RX_AI_BOT_USER = re.compile(r"\[bot\]\b")

# 7. ``<verb> by/with/using <AI>`` and similar.
RX_GENERATED = re.compile(
    rf"\b(?:generated|authored|created|written|refactored|reviewed|coded|implemented)\s+(?:by|with|using)\s+(?:{_AI_BRAND_LIST}|ai)\b",
    re.IGNORECASE,
)


def _line_has_opt_out(line: str) -> bool:
    return OPT_OUT_TAG in line


def _claude_violation(line: str) -> str | None:
    """Return matched token or None.

    A line containing ``claude`` only via ``CLAUDE.md`` (filename) or the
    ``.claude`` config directory is allowed.
    """
    if not RX_CLAUDE.search(line):
        return None
    # Strip the allowed forms and re-check.
    stripped = RX_CLAUDE_MD_OK.sub("", line)
    stripped = RX_DOT_CLAUDE_OK.sub("", stripped)
    m = RX_CLAUDE.search(stripped)
    return m.group(0) if m else None


def _scan_line(line: str) -> list[str]:
    """Return list of offending tokens found on this line."""
    hits: list[str] = []
    tok = _claude_violation(line)
    if tok:
        hits.append(tok)
    for rx in (
        RX_GPT_BAD,
        RX_ANTHROPIC,
        RX_OPENAI,
        RX_COPILOT,
        RX_OTHER_BRANDS,
        RX_AI_EMAIL,
        RX_AI_BOT_USER,
        RX_COAUTH,
        RX_GENERATED,
    ):
        m = rx.search(line)
        if m:
            hits.append(m.group(0))
    return hits


def _iter_files() -> Iterable[Path]:
    for d in SCAN_DIRS:
        root = REPO_ROOT / d
        if not root.is_dir():
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            # Prune skip dirs in-place so os.walk doesn't descend.
            dirnames[:] = [
                dn
                for dn in dirnames
                if dn not in ("_deps", "third_party", "node_modules", ".git", "__pycache__")
                and not dn.startswith("build")
            ]
            for fn in filenames:
                p = Path(dirpath) / fn
                rel = "/" + str(p.relative_to(REPO_ROOT)) + "/"
                if any(frag in rel for frag in SKIP_FRAGMENTS):
                    continue
                if p.suffix.lower() not in TEXT_EXTS:
                    continue
                yield p
    for fn in EXTRA_TOP_LEVEL_FILES:
        p = REPO_ROOT / fn
        if p.is_file():
            yield p


def _scan_file(path: Path) -> list[tuple[int, str, str]]:
    """Return list of ``(lineno, token, excerpt)`` violations."""
    out: list[tuple[int, str, str]] = []
    try:
        with path.open("r", encoding="utf-8", errors="replace") as fh:
            for i, line in enumerate(fh, start=1):
                if _line_has_opt_out(line):
                    continue
                hits = _scan_line(line)
                out.extend((i, tok, line.rstrip("\n")) for tok in hits)
    except OSError:
        return []
    return out


def main() -> int:
    # Self-exempt files legitimately spell the banned tokens; they are
    # matched explicitly so every internal line need not carry AI-OK.
    self_exempt_resolved = {q.resolve() for q in SELF_EXEMPT_FILES}

    violations = 0
    for p in _iter_files():
        if p.resolve() in self_exempt_resolved:
            continue
        for lineno, tok, excerpt in _scan_file(p):
            rel = p.relative_to(REPO_ROOT)
            # Trim long excerpts.
            display = (
                excerpt
                if len(excerpt) <= EXCERPT_MAX_LEN
                else excerpt[:EXCERPT_TRUNCATE_LEN] + "..."
            )
            print(f"{rel}:{lineno}: AI attribution found ('{tok}'): {display}")
            violations += 1

    if violations:
        print(f"\n[FAIL] {violations} AI-attribution violation(s).", file=sys.stderr)
        print("       See docs/AI_ATTRIBUTION_POLICY.md. Use a per-line", file=sys.stderr)
        print("       'AI-OK: <reason>' tag only when quoting policy text.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
