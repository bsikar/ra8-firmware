//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Shared host box and log C-ABI shims for widget render test roots.

/// Registers host log, box-layout, and rect shims against `abi`.
pub fn Shims(comptime abi: type) type {
    return struct {
        comptime {
            _ = &ra8_log_emit_error;
            _ = &ra8_box_tree_init;
            _ = &ra8_box_add;
            _ = &ra8_box_layout;
            _ = &ra8_ui_rect_contains;
        }

        export fn ra8_log_emit_error(_: [*:0]const u8, _: [*:0]const u8) void {}

        export fn ra8_box_tree_init(tree: *abi.core.BoxTree, storage: [*]abi.core.Box, cap: u16) callconv(.c) u16 {
            tree.nodes = storage;
            tree.cap = cap;
            tree.count = 0;
            return 0;
        }

        export fn ra8_box_add(tree: *abi.core.BoxTree, _: i16, node: *const abi.core.Box) callconv(.c) i16 {
            if (tree.count >= tree.cap) return -1;
            const index = tree.count;
            tree.nodes.?[index] = node.*;
            tree.count += 1;
            return @intCast(index);
        }

        export fn ra8_box_layout(tree: *abi.core.BoxTree, root: i16, frame: *const abi.types.Rect) callconv(.c) u16 {
            const nodes = tree.nodes.?[0..tree.count];
            const root_index: usize = @intCast(root);
            nodes[root_index].rect = frame.*;
            const stack = nodes[root_index];
            const children = nodes.len - 1;
            if (children == 0) return 0;
            const vertical = stack.kind == abi.core.box.stack_v;
            const extent = if (vertical) frame.h else frame.w;
            const inner = @max(extent - 2 * @as(i32, stack.pad) - @as(i32, stack.gap) * @as(i32, @intCast(children - 1)), 0);
            const cell = @divTrunc(inner, @as(i32, @intCast(children)));
            for (nodes[1..], 0..) |*child, index| {
                const offset = @as(i32, stack.pad) + @as(i32, @intCast(index)) * (cell + @as(i32, stack.gap));
                child.rect = if (vertical)
                    .{ .x = frame.x + stack.pad, .y = frame.y + offset, .w = frame.w - 2 * stack.pad, .h = cell }
                else
                    .{ .x = frame.x + offset, .y = frame.y + stack.pad, .w = cell, .h = frame.h - 2 * stack.pad };
            }
            return 0;
        }

        export fn ra8_ui_rect_contains(rect: *const abi.types.Rect, x: i32, y: i32) callconv(.c) bool {
            return x >= rect.x and y >= rect.y and x < rect.x + rect.w and y < rect.y + rect.h;
        }
    };
}
