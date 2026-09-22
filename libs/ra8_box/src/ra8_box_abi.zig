//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `libs/ra8_box/inc/ra8_box.h`. The layout engine itself
//! lives in `internal/root.zig`; this file owns only argument validation,
//! `ra8_err_t` mapping, and the diagnostic log calls the C implementation
//! made through `RA8_CHECK_NULL_PTR`.

const std = @import("std");
const implementation = @import("internal/root.zig");

/// Rectangle type of the published ABI (`ra8_ui_rect_t`).
pub const Rect = implementation.Rect;
/// Node type of the published ABI (`ra8_box_t`).
pub const Node = implementation.Node;
/// Tree builder type of the published ABI (`ra8_box_tree_t`).
pub const Tree = implementation.Tree;
/// Invalid-index sentinel of the published ABI (`k_ra8_box_none`).
pub const none = implementation.none;

/// Subset of `ra8_err_t` this library returns.
pub const BoxError = enum(u16) {
    ok = 0,
    invalid_arg = 0x103,
    null_ptr = 0x504,
};

/// Component tag for diagnostic logging, matching the C `s_tag`.
const tag: [*:0]const u8 = "ra8_box";

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;

comptime {
    if (@sizeOf(BoxError) != 2) @compileError("ra8_box error width");
    if (@intFromEnum(BoxError.ok) != 0) @compileError("ra8_box success value");
    if (@intFromEnum(BoxError.invalid_arg) != 0x103) @compileError("ra8_box invalid-arg value");
    if (@intFromEnum(BoxError.null_ptr) != 0x504) @compileError("ra8_box null-pointer value");

    if (@sizeOf(implementation.Rect) != 16) @compileError("ra8_ui_rect_t size");
    if (@alignOf(implementation.Rect) != 4) @compileError("ra8_ui_rect_t alignment");
    if (@offsetOf(implementation.Rect, "x") != 0) @compileError("ra8_ui_rect_t x offset");
    if (@offsetOf(implementation.Rect, "y") != 4) @compileError("ra8_ui_rect_t y offset");
    if (@offsetOf(implementation.Rect, "w") != 8) @compileError("ra8_ui_rect_t w offset");
    if (@offsetOf(implementation.Rect, "h") != 12) @compileError("ra8_ui_rect_t h offset");

    if (@sizeOf(implementation.Node) != 48) @compileError("ra8_box_t size");
    if (@alignOf(implementation.Node) != 4) @compileError("ra8_box_t alignment");
    if (@offsetOf(implementation.Node, "kind") != 0) @compileError("ra8_box_t kind offset");
    if (@offsetOf(implementation.Node, "grid_cols") != 1) @compileError("ra8_box_t grid_cols offset");
    if (@offsetOf(implementation.Node, "fixed") != 2) @compileError("ra8_box_t fixed offset");
    if (@offsetOf(implementation.Node, "flex") != 4) @compileError("ra8_box_t flex offset");
    if (@offsetOf(implementation.Node, "pad") != 6) @compileError("ra8_box_t pad offset");
    if (@offsetOf(implementation.Node, "gap") != 8) @compileError("ra8_box_t gap offset");
    if (@offsetOf(implementation.Node, "fill") != 12) @compileError("ra8_box_t fill offset");
    if (@offsetOf(implementation.Node, "border") != 16) @compileError("ra8_box_t border offset");
    if (@offsetOf(implementation.Node, "border_w") != 20) @compileError("ra8_box_t border_w offset");
    if (@offsetOf(implementation.Node, "tag") != 22) @compileError("ra8_box_t tag offset");
    if (@offsetOf(implementation.Node, "first_child") != 24) @compileError("ra8_box_t first_child offset");
    if (@offsetOf(implementation.Node, "next") != 28) @compileError("ra8_box_t next offset");
    if (@offsetOf(implementation.Node, "rect") != 32) @compileError("ra8_box_t rect offset");

    if (@sizeOf(implementation.Tree) != std.mem.alignForward(usize, @sizeOf(usize) + 4, @alignOf(usize))) {
        @compileError("ra8_box_tree_t size");
    }
    if (@offsetOf(implementation.Tree, "nodes") != 0) @compileError("ra8_box_tree_t nodes offset");
    if (@offsetOf(implementation.Tree, "cap") != @sizeOf(usize)) @compileError("ra8_box_tree_t cap offset");
    if (@offsetOf(implementation.Tree, "count") != @sizeOf(usize) + 2) @compileError("ra8_box_tree_t count offset");
}

/// Bind a tree builder to caller-owned node storage.
pub export fn ra8_box_tree_init(
    tree: ?*implementation.Tree,
    storage: ?[*]implementation.Node,
    cap: u16,
) callconv(.c) BoxError {
    const target = tree orelse {
        ra8_log_emit_error(tag, "tree must not be nullptr");
        return .null_ptr;
    };
    const nodes = storage orelse {
        ra8_log_emit_error(tag, "storage must not be nullptr");
        return .null_ptr;
    };
    if (!implementation.treeInit(target, nodes, cap)) return .invalid_arg;
    return .ok;
}

/// Append a node and link it as a child of `parent`.
pub export fn ra8_box_add(
    tree: ?*implementation.Tree,
    parent: i16,
    node: ?*const implementation.Node,
) callconv(.c) i16 {
    const target = tree orelse return @intCast(implementation.none);
    const template = node orelse return @intCast(implementation.none);
    return implementation.add(target, parent, template.*);
}

/// Lay out the tree, filling every reachable node's rectangle.
pub export fn ra8_box_layout(
    tree: ?*implementation.Tree,
    root: i16,
    frame: ?*const implementation.Rect,
) callconv(.c) BoxError {
    const target = tree orelse {
        ra8_log_emit_error(tag, "tree must not be nullptr");
        return .null_ptr;
    };
    const outer = frame orelse {
        ra8_log_emit_error(tag, "frame must not be nullptr");
        return .null_ptr;
    };
    if (!implementation.layout(target, root, outer.*)) return .invalid_arg;
    return .ok;
}
