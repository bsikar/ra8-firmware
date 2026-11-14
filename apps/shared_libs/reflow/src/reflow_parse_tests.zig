//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for the reflow parse entry (RA8FW-1212). The tests stand in
//! for the C tokenizer with a stub `priv_reflow_xml_walk` export, drive
//! the argument gates of `reflow_parse_xhtml` and check the pool reset
//! lands before the forward.

const std = @import("std");
const abi = @import("reflow_parse_abi.zig");

const ReflowEngine = abi.ReflowEngine;

const k_ra8_err_invalid_size: c_int = 0x105;
const k_ra8_err_not_initialized: c_int = 0x10F;
const k_ra8_err_null_ptr: c_int = 0x504;

pub var walk_calls: usize = 0;
var walk_engine: usize = 0;
var walk_len: usize = 0;

export fn priv_reflow_xml_walk(engine: *ReflowEngine, xhtml_buf: [*]const u8, xhtml_len: usize) c_int {
    walk_calls += 1;
    walk_engine = @intFromPtr(engine);
    walk_len = xhtml_len;
    _ = xhtml_buf;
    return 0;
}

test "argument gates" {
    const engine = try std.heap.page_allocator.create(ReflowEngine);
    defer std.heap.page_allocator.destroy(engine);
    engine.* = std.mem.zeroes(ReflowEngine);

    try std.testing.expectEqual(k_ra8_err_null_ptr, abi.reflow_parse_xhtml(null, "x", 1));
    try std.testing.expectEqual(k_ra8_err_null_ptr, abi.reflow_parse_xhtml(engine, null, 1));
    try std.testing.expectEqual(k_ra8_err_not_initialized, abi.reflow_parse_xhtml(engine, "x", 1));
    engine.in_use = 1;
    try std.testing.expectEqual(k_ra8_err_invalid_size, abi.reflow_parse_xhtml(engine, "x", 0));
    try std.testing.expectEqual(@as(usize, 0), walk_calls);
}

test "pool reset then forward" {
    const engine = try std.heap.page_allocator.create(ReflowEngine);
    defer std.heap.page_allocator.destroy(engine);
    engine.* = std.mem.zeroes(ReflowEngine);
    engine.in_use = 1;
    engine.token_count = 5;
    engine.text_pool_used = 9;
    engine.link_target_count = 3;

    try std.testing.expectEqual(@as(c_int, 0), abi.reflow_parse_xhtml(engine, "x", 1));
    try std.testing.expectEqual(@as(usize, 1), walk_calls);
    try std.testing.expectEqual(@intFromPtr(engine), walk_engine);
    try std.testing.expectEqual(@as(usize, 1), walk_len);
    try std.testing.expectEqual(@as(u32, 0), engine.token_count);
    try std.testing.expectEqual(@as(u32, 0), engine.text_pool_used);
    try std.testing.expectEqual(@as(u32, 0), engine.link_target_count);
}
