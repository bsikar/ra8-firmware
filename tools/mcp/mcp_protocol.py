# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""JSON-RPC 2.0 framing and the MCP method handlers.

The transport-and-envelope half of the server: build a result, build an error,
and route one request to the handler for its method.  Kept apart from the
tools, resources and prompts it dispatches to so that a protocol change and a
capability change cannot be made in the same edit by accident.
"""

from __future__ import annotations

from typing import Any

from mcp_content import (
    RESOURCE_INDEX,
    RESOURCES,
    handle_prompts_get,
    handle_prompts_list,
)
from mcp_tools import TOOL_INDEX, TOOLS
from mcp_util import log

SERVER_NAME = "ra8-firmware"
SERVER_VERSION = "1.0.0"

# Newest spec revision this server is known to interoperate with. When a client
# asks for a specific revision in ``initialize`` we echo theirs back (the spec
# lets the server propose its own, but echoing avoids needless renegotiation).
PROTOCOL_VERSION_DEFAULT = "2024-11-05"

# JSON-RPC 2.0 standard error codes used by this server.
ERR_PARSE = -32700
ERR_INVALID_REQUEST = -32600
ERR_METHOD_NOT_FOUND = -32601
ERR_INVALID_PARAMS = -32602
ERR_INTERNAL = -32603


# ---------------------------------------------------------------------------
# JSON-RPC dispatch
# ---------------------------------------------------------------------------
def _result(request_id: Any, payload: dict[str, Any]) -> dict[str, Any]:
    """Wrap `payload` as a JSON-RPC 2.0 success response.

    Args:
        request_id: The originating request's id, echoed verbatim -- the client
            matches responses by it, so it must not be normalised.
        payload: The method's result object.

    Returns:
        A response envelope carrying `result`.
    """
    return {"jsonrpc": "2.0", "id": request_id, "result": payload}


def _error(request_id: Any, code: int, message: str) -> dict[str, Any]:
    """Wrap a failure as a JSON-RPC 2.0 error response.

    This is protocol-level failure -- an unknown method, malformed JSON. A tool
    that runs and fails is NOT this: `handle_tools_call` returns a successful
    response carrying `isError: true`, so the model sees the failure text
    instead of the transport swallowing it.

    Args:
        request_id: The originating request's id, echoed verbatim. None when
            the request was unparseable enough to have no id.
        code: JSON-RPC error code (-32601 unknown method, -32603 internal).
        message: Human-readable text; reaches the client as-is.

    Returns:
        A response envelope carrying `error`.
    """
    return {"jsonrpc": "2.0", "id": request_id, "error": {"code": code, "message": message}}


def handle_initialize(params: dict[str, Any]) -> dict[str, Any]:
    """Answer the MCP `initialize` handshake and declare server capabilities.

    Echoes the client's requested `protocolVersion` back rather than asserting
    the server's own, which keeps a client on a newer spec revision from being
    refused over a version string alone. The fallback applies only when the
    client omits the field or sends an empty one.

    All three capability objects are advertised empty: this server supports
    tools, resources and prompts but none of their optional sub-features (no
    list-changed notifications, no subscriptions).

    Args:
        params: JSON-RPC params from the client's initialize request.

    Returns:
        Protocol version, capability map, and server name/version.
    """
    requested = str(params.get("protocolVersion") or PROTOCOL_VERSION_DEFAULT)
    return {
        "protocolVersion": requested,
        "capabilities": {"tools": {}, "resources": {}, "prompts": {}},
        "serverInfo": {"name": SERVER_NAME, "version": SERVER_VERSION},
    }


def handle_tools_list() -> dict[str, Any]:
    """Answer MCP `tools/list` with the catalogue's client-visible fields.

    Projects each entry to name, description and inputSchema; `handler` is a
    Python callable and is neither serialisable nor the client's business.

    Returns:
        `{"tools": [...]}` in TOOLS order.
    """
    listed = [
        {"name": t["name"], "description": t["description"], "inputSchema": t["inputSchema"]}
        for t in TOOLS
    ]
    return {"tools": listed}


def handle_tools_call(params: dict[str, Any]) -> dict[str, Any]:
    """Answer MCP `tools/call` by running the named handler.

    Every failure here is reported as a SUCCESSFUL JSON-RPC response carrying
    `isError: true`, never as a protocol error. That is the MCP contract for
    tool failure and it is what puts the message in front of the model: a
    transport-level error would be handled by the client and the model would
    only see that something went wrong, not what.

    Unknown tool, argument rejection (ValueError) and any other handler
    exception are therefore all caught. The catch-all is a deliberate RPC
    boundary -- an unhandled exception would otherwise kill the stdio loop and
    take the whole session down over one bad tool call -- and it logs the repr
    to stderr so the traceback-worthy detail is not lost.

    Args:
        params: JSON-RPC params; "name" selects the tool, "arguments" is
            forwarded to the handler unvalidated (schema enforcement is the
            handler's job).

    Returns:
        `{"content": [{"type": "text", ...}], "isError": bool}`.
    """
    name = str(params.get("name", ""))
    tool = TOOL_INDEX.get(name)
    if tool is None:
        return {"content": [{"type": "text", "text": f"unknown tool: {name}"}], "isError": True}
    arguments = params.get("arguments") or {}
    try:
        text = tool["handler"](arguments)
    except ValueError as exc:
        return {"content": [{"type": "text", "text": f"invalid request: {exc}"}], "isError": True}
    except Exception as exc:  # noqa: BLE001 -- RPC boundary: every handler error becomes a protocol error
        log(f"tool '{name}' raised: {exc!r}")
        return {"content": [{"type": "text", "text": f"tool error: {exc}"}], "isError": True}
    else:
        return {"content": [{"type": "text", "text": text}], "isError": False}


def handle_resources_list() -> dict[str, Any]:
    """Answer MCP `resources/list` with the catalogue's descriptive fields.

    Projects out `reader`, the callable that actually loads each resource's
    text. Listing is therefore cheap and touches no files -- content is only
    read when the client asks for a specific URI.

    Returns:
        `{"resources": [...]}` with uri, name, description and mimeType, in
        RESOURCES order.
    """
    listed = [
        {
            "uri": r["uri"],
            "name": r["name"],
            "description": r["description"],
            "mimeType": r["mimeType"],
        }
        for r in RESOURCES
    ]
    return {"resources": listed}


def handle_resources_read(params: dict[str, Any]) -> dict[str, Any]:
    """Answer MCP `resources/read` by invoking the URI's reader.

    Lookup is exact-match against the registered URI; there is no prefix or
    glob matching, so a client cannot reach a file the catalogue does not name.
    That is the read boundary for this server.

    The reader runs on every call with no caching, so the client always sees
    the file's current contents rather than a snapshot from server start.

    Args:
        params: JSON-RPC params; "uri" selects the resource.

    Returns:
        `{"contents": [{"uri", "mimeType", "text"}]}` -- a single-element list,
        as this server has no multi-part resources.

    Raises:
        ValueError: URI is not in the catalogue; `dispatch` turns it into a
            JSON-RPC error.
        OSError: The reader could not read its backing file -- a registered
            resource whose file has been moved or deleted.
    """
    uri = str(params.get("uri", ""))
    resource = RESOURCE_INDEX.get(uri)
    if resource is None:
        msg = f"unknown resource uri: {uri}"
        raise ValueError(msg)
    text = resource["reader"]()
    return {"contents": [{"uri": uri, "mimeType": resource["mimeType"], "text": text}]}


def dispatch(request: dict[str, Any]) -> dict[str, Any] | None:  # noqa: PLR0911  # JSON-RPC method router, splitting hurts readability
    """Route one JSON-RPC request. Returns a response, or None for notifications."""
    method = request.get("method")
    request_id = request.get("id")
    params = request.get("params") or {}

    # Notifications (no id) get no response.
    if request_id is None and isinstance(method, str) and method.startswith("notifications/"):
        return None

    try:
        if method == "initialize":
            return _result(request_id, handle_initialize(params))
        if method == "ping":
            return _result(request_id, {})
        if method == "tools/list":
            return _result(request_id, handle_tools_list())
        if method == "tools/call":
            return _result(request_id, handle_tools_call(params))
        if method == "resources/list":
            return _result(request_id, handle_resources_list())
        if method == "resources/read":
            return _result(request_id, handle_resources_read(params))
        if method == "prompts/list":
            return _result(request_id, handle_prompts_list())
        if method == "prompts/get":
            return _result(request_id, handle_prompts_get(params))
        if method == "resources/templates/list":
            return _result(request_id, {"resourceTemplates": []})
        return _error(request_id, ERR_METHOD_NOT_FOUND, f"method not found: {method}")
    except ValueError as exc:
        return _error(request_id, ERR_INVALID_PARAMS, str(exc))
    except Exception as exc:  # noqa: BLE001 -- RPC boundary: every handler error becomes a protocol error
        log(f"dispatch error for {method}: {exc!r}")
        return _error(request_id, ERR_INTERNAL, str(exc))
