//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Parity and execution verification test for `tests/zig_abi_pairs/ra8_box_pair`.
//! Proves that `vectors.json` and `inc/ra8_box_pair_vectors.h` express one
//! identical specification, and executes `vectors.json` directly against the
//! compiled `libra8_box.a` C ABI exports.

const std = @import("std");

const Rect = extern struct {
    x: i32,
    y: i32,
    w: i32,
    h: i32,
};

const BoxNode = extern struct {
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

const BoxTree = extern struct {
    nodes: ?[*]BoxNode,
    cap: u16,
    count: u16,
};

extern "c" fn ra8_box_tree_init(tree: ?*BoxTree, storage: ?[*]BoxNode, cap: u16) callconv(.c) u16;
extern "c" fn ra8_box_add(tree: ?*BoxTree, parent: i16, node: ?*const BoxNode) callconv(.c) i16;
extern "c" fn ra8_box_layout(tree: ?*BoxTree, root: i16, frame: ?*const Rect) callconv(.c) u16;

const Constants = struct {
    k_ra8_box_none: i32,
    k_ra8_box_no_colour: i32,
    k_ra8_box_stack_v: u8,
    k_ra8_box_stack_h: u8,
    k_ra8_box_grid: u8,
    k_ra8_box_leaf: u8,
    k_ra8_ok: u16,
    k_ra8_err_invalid_arg: u16,
    k_ra8_err_null_ptr: u16,
    sizeof_ra8_ui_rect_t: usize,
    sizeof_ra8_box_t: usize,
    max_nodes_per_case: u16,
};

const ErrorVector = struct {
    id: []const u8,
    op: []const u8,
    null_tree: bool,
    null_arg: bool,
    cap: u16,
    seed_count: u16,
    index_arg: i16,
    expected_err: u16,
    expected_idx: i16,
};

const JsonRect = struct {
    x: i32,
    y: i32,
    w: i32,
    h: i32,
};

const NodeVector = struct {
    parent: i16,
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
    expected_index: i16,
    expected_first_child: i32,
    expected_next: i32,
    expected_rect: JsonRect,
};

const LayoutVector = struct {
    id: []const u8,
    frame: JsonRect,
    root: i16,
    nodes: []const NodeVector,
};

const VectorSpec = struct {
    schema: u32,
    library: []const u8,
    header: []const u8,
    constants: Constants,
    error_vectors: []const ErrorVector,
    layout_vectors: []const LayoutVector,
};

const vectors_bytes = @embedFile("vectors_json");
const c_vectors_bytes = @embedFile("c_vectors_json");

fn parseSpec(allocator: std.mem.Allocator, bytes: []const u8) !std.json.Parsed(VectorSpec) {
    return std.json.parseFromSlice(VectorSpec, allocator, bytes, .{});
}

test "C projection inc/ra8_box_pair_vectors.h matches vectors.json 1:1" {
    const canonical = try parseSpec(std.testing.allocator, vectors_bytes);
    defer canonical.deinit();
    const projected = try parseSpec(std.testing.allocator, c_vectors_bytes);
    defer projected.deinit();

    const a = canonical.value;
    const b = projected.value;

    try std.testing.expectEqual(a.schema, b.schema);
    try std.testing.expectEqualStrings(a.library, b.library);
    try std.testing.expectEqualStrings(a.header, b.header);
    try std.testing.expectEqual(a.constants, b.constants);

    try std.testing.expectEqual(a.error_vectors.len, b.error_vectors.len);
    for (a.error_vectors, b.error_vectors) |ea, eb| {
        try std.testing.expectEqualStrings(ea.id, eb.id);
        try std.testing.expectEqualStrings(ea.op, eb.op);
        try std.testing.expectEqual(ea.null_tree, eb.null_tree);
        try std.testing.expectEqual(ea.null_arg, eb.null_arg);
        try std.testing.expectEqual(ea.cap, eb.cap);
        try std.testing.expectEqual(ea.seed_count, eb.seed_count);
        try std.testing.expectEqual(ea.index_arg, eb.index_arg);
        try std.testing.expectEqual(ea.expected_err, eb.expected_err);
        try std.testing.expectEqual(ea.expected_idx, eb.expected_idx);
    }

    try std.testing.expectEqual(a.layout_vectors.len, b.layout_vectors.len);
    for (a.layout_vectors, b.layout_vectors) |la, lb| {
        try std.testing.expectEqualStrings(la.id, lb.id);
        try std.testing.expectEqual(la.frame, lb.frame);
        try std.testing.expectEqual(la.root, lb.root);
        try std.testing.expectEqual(la.nodes.len, lb.nodes.len);
        for (la.nodes, lb.nodes) |na, nb| {
            try std.testing.expectEqual(na, nb);
        }
    }
}

test "vectors.json executes cleanly against libra8_box.a C ABI" {
    const parsed = try parseSpec(std.testing.allocator, vectors_bytes);
    defer parsed.deinit();
    const spec = parsed.value;

    try std.testing.expectEqual(spec.constants.sizeof_ra8_ui_rect_t, @sizeOf(Rect));
    try std.testing.expectEqual(spec.constants.sizeof_ra8_box_t, @sizeOf(BoxNode));
    try std.testing.expect(@sizeOf(BoxTree) >= @sizeOf(usize));

    const max_nodes: usize = 8;
    try std.testing.expectEqual(max_nodes, spec.constants.max_nodes_per_case);

    for (spec.error_vectors) |ev| {
        var storage: [max_nodes]BoxNode = @splat(std.mem.zeroes(BoxNode));
        var tree = std.mem.zeroes(BoxTree);
        const leaf = BoxNode{
            .kind = spec.constants.k_ra8_box_leaf,
            .grid_cols = 0,
            .fixed = 0,
            .flex = 0,
            .pad = 0,
            .gap = 0,
            .fill = 0,
            .border = 0,
            .border_w = 0,
            .tag = 0,
            .first_child = 0,
            .next = 0,
            .rect = .{ .x = 0, .y = 0, .w = 0, .h = 0 },
        };
        const frame = Rect{ .x = 0, .y = 0, .w = 10, .h = 10 };

        if (!std.mem.eql(u8, ev.op, "tree_init")) {
            try std.testing.expectEqual(
                spec.constants.k_ra8_ok,
                ra8_box_tree_init(&tree, &storage, ev.cap),
            );
            var seeded: u16 = 0;
            while (seeded < ev.seed_count) : (seeded += 1) {
                try std.testing.expect(ra8_box_add(&tree, -1, &leaf) >= 0);
            }
        }

        const tree_arg: ?*BoxTree = if (ev.null_tree) null else &tree;
        if (std.mem.eql(u8, ev.op, "tree_init")) {
            const buf_arg: ?[*]BoxNode = if (ev.null_arg) null else &storage;
            try std.testing.expectEqual(ev.expected_err, ra8_box_tree_init(tree_arg, buf_arg, ev.cap));
        } else if (std.mem.eql(u8, ev.op, "add")) {
            const node_arg: ?*const BoxNode = if (ev.null_arg) null else &leaf;
            const before = tree.count;
            try std.testing.expectEqual(ev.expected_idx, ra8_box_add(tree_arg, ev.index_arg, node_arg));
            if (!ev.null_tree) try std.testing.expectEqual(before, tree.count);
        } else if (std.mem.eql(u8, ev.op, "layout")) {
            const frame_arg: ?*const Rect = if (ev.null_arg) null else &frame;
            try std.testing.expectEqual(ev.expected_err, ra8_box_layout(tree_arg, ev.index_arg, frame_arg));
        } else {
            return error.UnknownErrorVectorOp;
        }
    }

    for (spec.layout_vectors) |lv| {
        var storage: [max_nodes]BoxNode = @splat(std.mem.zeroes(BoxNode));
        var tree = std.mem.zeroes(BoxTree);
        try std.testing.expectEqual(
            spec.constants.k_ra8_ok,
            ra8_box_tree_init(&tree, &storage, spec.constants.max_nodes_per_case),
        );

        for (lv.nodes) |nv| {
            const node = BoxNode{
                .kind = nv.kind,
                .grid_cols = nv.grid_cols,
                .fixed = nv.fixed,
                .flex = nv.flex,
                .pad = nv.pad,
                .gap = nv.gap,
                .fill = nv.fill,
                .border = nv.border,
                .border_w = nv.border_w,
                .tag = nv.tag,
                .first_child = 0,
                .next = 0,
                .rect = .{ .x = 0, .y = 0, .w = 0, .h = 0 },
            };
            try std.testing.expectEqual(nv.expected_index, ra8_box_add(&tree, nv.parent, &node));
        }

        const frame = Rect{ .x = lv.frame.x, .y = lv.frame.y, .w = lv.frame.w, .h = lv.frame.h };
        try std.testing.expectEqual(spec.constants.k_ra8_ok, ra8_box_layout(&tree, lv.root, &frame));

        for (lv.nodes, 0..) |nv, idx| {
            const actual = storage[idx];
            try std.testing.expectEqual(nv.kind, actual.kind);
            try std.testing.expectEqual(nv.grid_cols, actual.grid_cols);
            try std.testing.expectEqual(nv.fixed, actual.fixed);
            try std.testing.expectEqual(nv.flex, actual.flex);
            try std.testing.expectEqual(nv.pad, actual.pad);
            try std.testing.expectEqual(nv.gap, actual.gap);
            try std.testing.expectEqual(nv.fill, actual.fill);
            try std.testing.expectEqual(nv.border, actual.border);
            try std.testing.expectEqual(nv.border_w, actual.border_w);
            try std.testing.expectEqual(nv.tag, actual.tag);
            try std.testing.expectEqual(nv.expected_first_child, actual.first_child);
            try std.testing.expectEqual(nv.expected_next, actual.next);
            try std.testing.expectEqual(nv.expected_rect.x, actual.rect.x);
            try std.testing.expectEqual(nv.expected_rect.y, actual.rect.y);
            try std.testing.expectEqual(nv.expected_rect.w, actual.rect.w);
            try std.testing.expectEqual(nv.expected_rect.h, actual.rect.h);
        }
    }
}
