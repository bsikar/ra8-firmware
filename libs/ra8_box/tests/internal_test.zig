//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Unit tests for the private box-model layout engine.

const std = @import("std");
const implementation = @import("implementation");

fn leaf(fixed: i16, flex: u16) implementation.Node {
    return .{
        .kind = @intFromEnum(implementation.Kind.leaf),
        .grid_cols = 1,
        .fixed = fixed,
        .flex = flex,
        .pad = 0,
        .gap = 0,
        .fill = 0,
        .border = 0,
        .border_w = 0,
        .tag = -1,
        .first_child = implementation.none,
        .next = implementation.none,
        .rect = .{ .x = 0, .y = 0, .w = 0, .h = 0 },
    };
}

fn container(kind: implementation.Kind, pad: i16, gap: i16, cols: u8) implementation.Node {
    var node = leaf(0, 1);
    node.kind = @intFromEnum(kind);
    node.pad = pad;
    node.gap = gap;
    node.grid_cols = cols;
    return node;
}

test "inset clamps a collapsed content box to zero extent" {
    const outer = implementation.Rect{ .x = 0, .y = 0, .w = 10, .h = 10 };
    const tight = implementation.inset(outer, 20);
    try std.testing.expectEqual(@as(i32, 20), tight.x);
    try std.testing.expectEqual(@as(i32, 0), tight.w);
    try std.testing.expectEqual(@as(i32, 0), tight.h);
}

test "iterLive stops on a none link and on the cycle bound" {
    try std.testing.expect(implementation.iterLive(3, 0, 8));
    try std.testing.expect(!implementation.iterLive(implementation.none, 0, 8));
    try std.testing.expect(!implementation.iterLive(3, 8, 8));
}

test "add rejects a full tree and a forward parent" {
    var storage: [2]implementation.Node = undefined;
    var tree: implementation.Tree = undefined;
    try std.testing.expect(implementation.treeInit(&tree, &storage, 2));

    try std.testing.expectEqual(@as(i16, 0), implementation.add(&tree, -1, leaf(10, 0)));
    try std.testing.expectEqual(@as(i16, -1), implementation.add(&tree, 5, leaf(10, 0)));
    try std.testing.expectEqual(@as(i16, 1), implementation.add(&tree, 0, leaf(10, 0)));
    try std.testing.expectEqual(@as(i16, -1), implementation.add(&tree, 0, leaf(10, 0)));
    try std.testing.expectEqual(@as(u16, 2), tree.count);
}

test "treeInit refuses a zero capacity" {
    var storage: [1]implementation.Node = undefined;
    var tree: implementation.Tree = undefined;
    try std.testing.expect(!implementation.treeInit(&tree, &storage, 0));
}

test "vertical stack gives fixed children their extent and splits the rest" {
    var storage: [8]implementation.Node = undefined;
    var tree: implementation.Tree = undefined;
    try std.testing.expect(implementation.treeInit(&tree, &storage, 8));

    const root = implementation.add(&tree, -1, container(.stack_v, 10, 10, 1));
    const a = implementation.add(&tree, root, leaf(30, 0));
    const b = implementation.add(&tree, root, leaf(0, 1));
    const c = implementation.add(&tree, root, leaf(20, 0));

    try std.testing.expect(implementation.layout(&tree, root, .{ .x = 0, .y = 0, .w = 100, .h = 200 }));
    try std.testing.expectEqual(@as(i32, 10), storage[@intCast(a)].rect.y);
    try std.testing.expectEqual(@as(i32, 30), storage[@intCast(a)].rect.h);
    try std.testing.expectEqual(@as(i32, 80), storage[@intCast(a)].rect.w);
    try std.testing.expectEqual(@as(i32, 110), storage[@intCast(b)].rect.h);
    try std.testing.expectEqual(@as(i32, 170), storage[@intCast(c)].rect.y);
}

test "grid places children row-major and keeps a fixed child height" {
    var storage: [8]implementation.Node = undefined;
    var tree: implementation.Tree = undefined;
    try std.testing.expect(implementation.treeInit(&tree, &storage, 8));

    const root = implementation.add(&tree, -1, container(.grid, 0, 0, 2));
    const a = implementation.add(&tree, root, leaf(0, 1));
    const b = implementation.add(&tree, root, leaf(0, 1));
    const c = implementation.add(&tree, root, leaf(7, 0));

    try std.testing.expect(implementation.layout(&tree, root, .{ .x = 0, .y = 0, .w = 100, .h = 100 }));
    try std.testing.expectEqual(@as(i32, 0), storage[@intCast(a)].rect.x);
    try std.testing.expectEqual(@as(i32, 50), storage[@intCast(b)].rect.x);
    try std.testing.expectEqual(@as(i32, 50), storage[@intCast(c)].rect.y);
    try std.testing.expectEqual(@as(i32, 7), storage[@intCast(c)].rect.h);
}

test "layout rejects an out-of-range root and an empty tree" {
    var storage: [2]implementation.Node = undefined;
    var tree: implementation.Tree = undefined;
    try std.testing.expect(implementation.treeInit(&tree, &storage, 2));
    const frame = implementation.Rect{ .x = 0, .y = 0, .w = 10, .h = 10 };
    try std.testing.expect(!implementation.layout(&tree, 0, frame));
    _ = implementation.add(&tree, -1, leaf(0, 1));
    try std.testing.expect(!implementation.layout(&tree, 1, frame));
    try std.testing.expect(!implementation.layout(&tree, -1, frame));
    try std.testing.expect(implementation.layout(&tree, 0, frame));
}
