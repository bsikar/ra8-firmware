//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Private box-model layout engine. Geometry only: no allocation, no
//! recursion, no framebuffer. The public C ABI membrane lives in
//! `../ra8_box_abi.zig`; this module owns the layout arithmetic and is
//! exercised directly by the Zig unit tests.

const std = @import("std");

/// No child / no sibling link, and the invalid-index sentinel.
pub const none: i32 = -1;

/// Box layout kind, matching `ra8_box_kind_t`.
pub const Kind = enum(u8) {
    stack_v = 0,
    stack_h = 1,
    grid = 2,
    leaf = 3,
};

/// Axis-aligned rectangle, matching `ra8_ui_rect_t`.
pub const Rect = extern struct {
    x: i32,
    y: i32,
    w: i32,
    h: i32,
};

/// One node in a box tree, matching `ra8_box_t`.
pub const Node = extern struct {
    kind: u8,
    grid_cols: u8,
    fixed: i16,
    flex: u16,
    pad: i16,
    gap: i16,
    fill: u32,
    border: u32,
    border_w: i16,
    tag: i16,
    first_child: i32,
    next: i32,
    rect: Rect,
};

/// Append-only builder over caller-owned node storage, matching
/// `ra8_box_tree_t`.
pub const Tree = extern struct {
    nodes: ?[*]Node,
    cap: u16,
    count: u16,
};

/// Inset `outer` by a uniform padding, flooring extent at zero.
pub fn inset(outer: Rect, pad: i32) Rect {
    var r = Rect{
        .x = outer.x +% pad,
        .y = outer.y +% pad,
        .w = outer.w -% (2 *% pad),
        .h = outer.h -% (2 *% pad),
    };
    if (r.w < 0) r.w = 0;
    if (r.h < 0) r.h = 0;
    return r;
}

/// Child-walk loop guard: the link is live and the iteration bound has not
/// been reached. The bound arm is a Power-of-10 Rule 2 defensive cycle guard;
/// no public-API sequence builds a cyclic chain.
pub fn iterLive(link: i32, guard: u16, count: u16) bool {
    return link != none and guard < count;
}

/// Direct-child summary used to distribute flex space.
pub const Tally = struct {
    count: u16,
    fixed: i32,
    flex: u32,
};

fn nodesOf(tree: *Tree) [*]Node {
    return tree.nodes.?;
}

/// Sum child count, fixed main-axis extent, and flex weight of a container.
pub fn tally(tree: *Tree, parent: u16) Tally {
    const nodes = nodesOf(tree);
    var result = Tally{ .count = 0, .fixed = 0, .flex = 0 };
    var c: i32 = nodes[parent].first_child;
    var guard: u16 = 0;
    while (iterLive(c, guard, tree.count)) : (guard += 1) {
        const child = &nodes[@intCast(c)];
        if (child.fixed > 0) {
            result.fixed +%= @as(i32, child.fixed);
        } else {
            result.flex +%= @as(u32, child.flex);
        }
        result.count += 1;
        c = child.next;
    }
    return result;
}

/// Lay out a stack container's children along one axis.
pub fn layoutStack(tree: *Tree, parent: u16, horizontal: bool) void {
    const nodes = nodesOf(tree);
    const cont = inset(nodes[parent].rect, @as(i32, nodes[parent].pad));
    const gap: i32 = @as(i32, nodes[parent].gap);
    const summary = tally(tree, parent);

    const main_extent: i32 = if (horizontal) cont.w else cont.h;
    const gaps: i32 = if (summary.count > 0)
        @as(i32, @intCast(summary.count - 1)) *% gap
    else
        0;
    var flex_space: i32 = main_extent -% summary.fixed -% gaps;
    if (flex_space < 0) flex_space = 0;

    var cursor: i32 = if (horizontal) cont.x else cont.y;
    var c: i32 = nodes[parent].first_child;
    var guard: u16 = 0;
    while (iterLive(c, guard, tree.count)) : (guard += 1) {
        const child = &nodes[@intCast(c)];
        var main_sz: i32 = 0;
        if (child.fixed > 0) {
            main_sz = @as(i32, child.fixed);
        } else if (summary.flex > 0) {
            main_sz = @divTrunc(flex_space *% @as(i32, child.flex), @as(i32, @intCast(summary.flex)));
        }
        if (horizontal) {
            child.rect = .{ .x = cursor, .y = cont.y, .w = main_sz, .h = cont.h };
        } else {
            child.rect = .{ .x = cont.x, .y = cursor, .w = cont.w, .h = main_sz };
        }
        cursor +%= main_sz +% gap;
        c = child.next;
    }
}

/// Lay out a grid container's children row-major across `grid_cols`.
pub fn layoutGrid(tree: *Tree, parent: u16) void {
    const nodes = nodesOf(tree);
    const cont = inset(nodes[parent].rect, @as(i32, nodes[parent].pad));
    const gap: i32 = @as(i32, nodes[parent].gap);
    const cols: i32 = if (nodes[parent].grid_cols >= 1) @as(i32, nodes[parent].grid_cols) else 1;
    const summary = tally(tree, parent);

    const rows: i32 = @divTrunc(@as(i32, @intCast(summary.count)) +% cols -% 1, cols);
    const row_count: i32 = if (rows > 0) rows else 1;
    var cell_w: i32 = @divTrunc(cont.w -% ((cols -% 1) *% gap), cols);
    if (cell_w < 0) cell_w = 0;
    var cell_h: i32 = @divTrunc(cont.h -% ((row_count -% 1) *% gap), row_count);
    if (cell_h < 0) cell_h = 0;

    var idx: i32 = 0;
    var c: i32 = nodes[parent].first_child;
    var guard: u16 = 0;
    while (iterLive(c, guard, tree.count)) : (guard += 1) {
        const child = &nodes[@intCast(c)];
        const col = @rem(idx, cols);
        const row = @divTrunc(idx, cols);
        child.rect.x = cont.x +% (col *% (cell_w +% gap));
        child.rect.y = cont.y +% (row *% (cell_h +% gap));
        child.rect.w = cell_w;
        child.rect.h = if (child.fixed > 0) @as(i32, child.fixed) else cell_h;
        idx +%= 1;
        c = child.next;
    }
}

/// Bind a tree builder to caller-owned node storage.
pub fn treeInit(tree: *Tree, storage: [*]Node, cap: u16) bool {
    if (cap == 0) return false;
    tree.nodes = storage;
    tree.cap = cap;
    tree.count = 0;
    return true;
}

/// Append `node` and link it as a child of `parent`, returning its index or
/// `none` when the tree is full or `parent` is not an earlier valid index.
pub fn add(tree: *Tree, parent: i16, node: Node) i16 {
    if (tree.nodes == null) return @intCast(none);
    if (tree.count >= tree.cap) return @intCast(none);
    if (parent != none and (parent < 0 or parent >= @as(i16, @bitCast(tree.count)))) {
        return @intCast(none);
    }

    const nodes = nodesOf(tree);
    const idx: i16 = @bitCast(tree.count);
    nodes[tree.count] = node;
    nodes[tree.count].first_child = none;
    nodes[tree.count].next = none;
    tree.count += 1;

    if (parent != none) {
        const par = &nodes[@intCast(parent)];
        if (par.first_child == none) {
            par.first_child = @as(i32, idx);
        } else {
            var sib: i32 = par.first_child;
            var guard: u16 = 0;
            while (iterLive(nodes[@intCast(sib)].next, guard, tree.count)) : (guard += 1) {
                sib = nodes[@intCast(sib)].next;
            }
            nodes[@intCast(sib)].next = @as(i32, idx);
        }
    }
    return idx;
}

/// Lay out the whole tree from `root`, filling every reachable `rect`.
pub fn layout(tree: *Tree, root: i16, frame: Rect) bool {
    if (tree.nodes == null) return false;
    if (tree.count == 0 or root < 0 or root >= @as(i16, @bitCast(tree.count))) return false;

    const nodes = nodesOf(tree);
    nodes[@intCast(root)].rect = frame;
    var i: u16 = 0;
    while (i < tree.count) : (i += 1) {
        switch (nodes[i].kind) {
            @intFromEnum(Kind.stack_v) => layoutStack(tree, i, false),
            @intFromEnum(Kind.stack_h) => layoutStack(tree, i, true),
            @intFromEnum(Kind.grid) => layoutGrid(tree, i),
            else => {},
        }
    }
    return true;
}
