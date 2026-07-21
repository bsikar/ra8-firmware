# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""The two read-only surfaces: resources (documents) and prompts (canned tasks).

Neither runs anything. Resources hand back repository documents by URI, and
prompts hand back a pre-written instruction with the caller's arguments
substituted -- so both are grouped apart from :mod:`mcp_tools`, whose entries
build firmware, run tests and touch hardware.
"""

from __future__ import annotations

from collections.abc import Callable
from typing import Any

from mcp_tools import tool_list_apps
from mcp_util import REPO_ROOT, read_text


# ---------------------------------------------------------------------------
# Resource registry -- read-only project context exposed by stable URIs
# ---------------------------------------------------------------------------
def _resource_doc(rel_path: str) -> Callable[[], str]:
    def reader() -> str:
        path = REPO_ROOT / rel_path
        if not path.is_file():
            return f"{rel_path} not found in this tree."
        return read_text(path)

    return reader


def _resource_catalogue() -> str:
    return tool_list_apps({})


RESOURCES: list[dict[str, Any]] = [
    {
        "uri": "ra8d2://doc/claude-md",
        "name": "CLAUDE.md",
        "description": "Project rules for AI assistants (the most-violated rules).",
        "mimeType": "text/markdown",
        "reader": _resource_doc("CLAUDE.md"),
    },
    {
        "uri": "ra8d2://doc/style-guide",
        "name": "docs/STYLE_GUIDE.md",
        "description": "Authoritative C23 + Doxygen style guide.",
        "mimeType": "text/markdown",
        "reader": _resource_doc("docs/STYLE_GUIDE.md"),
    },
    {
        "uri": "ra8d2://doc/ring-and-world",
        "name": "docs/RING_AND_WORLD.md",
        "description": "Architectural-ring + TrustZone-world tagging system.",
        "mimeType": "text/markdown",
        "reader": _resource_doc("docs/RING_AND_WORLD.md"),
    },
    {
        "uri": "ra8d2://doc/contributing",
        "name": "CONTRIBUTING.md",
        "description": "Contributor workflow and gate reference.",
        "mimeType": "text/markdown",
        "reader": _resource_doc("CONTRIBUTING.md"),
    },
    {
        "uri": "ra8d2://reference/chapter-map",
        "name": "HUM chapter map",
        "description": "Hardware User's Manual chapter-to-page ranges for citations.",
        "mimeType": "text/markdown",
        "reader": _resource_doc("docs/reference/CHAPTER_MAP.md"),
    },
    {
        "uri": "ra8d2://doc/hil",
        "name": "docs/HIL_SUITE.md",
        "description": "Hardware-in-the-loop suite + how each app is verified in CI.",
        "mimeType": "text/markdown",
        "reader": _resource_doc("docs/HIL_SUITE.md"),
    },
    {
        "uri": "ra8d2://doc/ai-attribution",
        "name": "docs/AI_ATTRIBUTION_POLICY.md",
        "description": "The zero-AI-attribution policy enforced across the tree.",
        "mimeType": "text/markdown",
        "reader": _resource_doc("docs/AI_ATTRIBUTION_POLICY.md"),
    },
    {
        "uri": "ra8d2://apps/catalogue",
        "name": "Firmware app catalogue",
        "description": "Live list of every discovered firmware app.",
        "mimeType": "text/plain",
        "reader": _resource_catalogue,
    },
]

RESOURCE_INDEX: dict[str, dict[str, Any]] = {r["uri"]: r for r in RESOURCES}

# ---------------------------------------------------------------------------
# Prompt registry -- reusable review flows the assistant can invoke by name.
# Each ``template`` is filled with the call's arguments by handle_prompts_get.
# ---------------------------------------------------------------------------
PROMPTS: list[dict[str, Any]] = [
    {
        "name": "audit_register_access",
        "description": "Audit a direct MMIO register access for a valid HUM citation + style.",
        "arguments": [
            {"name": "code", "description": "the register read/write line(s)", "required": True}
        ],
        "template": (
            "Audit this RA8D2 register access against the project rules: every direct "
            "register read/write MUST be immediately preceded by a HUM citation comment "
            '`/* HUM Ch X.Y "section" p NNNN */`, must go through an inline accessor '
            "(never a macro address), and must be pure 7-bit ASCII. Report each violation "
            "with a concrete fix.\n\n```c\n{code}\n```"
        ),
    },
    {
        "name": "mcdc_vectors",
        "description": "Write minimal MC/DC test vectors for a compound boolean decision.",
        "arguments": [
            {"name": "decision", "description": "the C boolean decision", "required": True}
        ],
        "template": (
            "Write the minimal (N+1) MC/DC test vectors for this decision, demonstrating "
            "that each condition independently affects the outcome, formatted as the "
            "project's `@par MC/DC:` Doxygen block.\n\n```c\n{decision}\n```"
        ),
    },
]

PROMPT_INDEX: dict[str, dict[str, Any]] = {p["name"]: p for p in PROMPTS}


def handle_prompts_list() -> dict[str, Any]:
    """Answer MCP `prompts/list` with the catalogue's public fields.

    Deliberately projects each entry rather than returning it whole: `template`
    is server-side detail, and shipping it would leak the prompt text to every
    client that merely enumerates.

    Returns:
        `{"prompts": [...]}` with name, description and arguments per entry, in
        PROMPTS order.
    """
    listed = [
        {"name": p["name"], "description": p["description"], "arguments": p["arguments"]}
        for p in PROMPTS
    ]
    return {"prompts": listed}


def handle_prompts_get(params: dict[str, Any]) -> dict[str, Any]:
    """Answer MCP `prompts/get` by substituting arguments into a template.

    Every declared argument is substituted, and a missing one becomes "" rather
    than an error -- so a partially-filled prompt renders with a gap instead of
    failing the call. Only declared arguments are passed to `format()`, so an
    undeclared extra in `params` is ignored, and a `{placeholder}` in the
    template with no matching declared argument raises.

    Args:
        params: JSON-RPC params; "name" selects the prompt, "arguments" is an
            optional name -> value mapping whose values are coerced to str.

    Returns:
        `{"description": ..., "messages": [...]}` with one user-role text
        message.

    Raises:
        ValueError: No prompt by that name, or the template referenced a
            placeholder that is not a declared argument. `dispatch` converts
            this into a JSON-RPC error response.
    """
    name = str(params.get("name", ""))
    prompt = PROMPT_INDEX.get(name)
    if prompt is None:
        msg = f"unknown prompt: {name}"
        raise ValueError(msg)
    arguments = params.get("arguments") or {}
    try:
        text = prompt["template"].format(
            **{a["name"]: str(arguments.get(a["name"], "")) for a in prompt["arguments"]}
        )
    except (KeyError, IndexError) as exc:
        msg = f"bad prompt arguments: {exc}"
        raise ValueError(msg) from exc
    return {
        "description": prompt["description"],
        "messages": [{"role": "user", "content": {"type": "text", "text": text}}],
    }
