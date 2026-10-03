//! Tests for a middleware archive's Zig sources (RA8FW-526): how the
//! archive's -D flags reach translate-c, and that no archive has grown one
//! by accident, so every existing archive keeps its command lines.
const std = @import("std");
const graph = @import("build_graph");
const middleware = graph.middleware;
const m85_modules = graph.m85_threadx_modules;
const cpu1_modules = graph.cpu1_threadx_modules;

test "a define with a value splits at the first equals sign" {
    const d = middleware.splitDefine("-DTX_TIMER_TICKS_PER_SECOND=1000").?;
    try std.testing.expectEqualStrings("TX_TIMER_TICKS_PER_SECOND", d.name);
    try std.testing.expectEqualStrings("1000", d.value);
    const nested = middleware.splitDefine("-DA=B=C").?;
    try std.testing.expectEqualStrings("A", nested.name);
    try std.testing.expectEqualStrings("B=C", nested.value);
}

test "an empty value stays empty, the way TX_SINGLE_MODE_SECURE= is meant" {
    const d = middleware.splitDefine("-DTX_SINGLE_MODE_SECURE=").?;
    try std.testing.expectEqualStrings("TX_SINGLE_MODE_SECURE", d.name);
    try std.testing.expectEqualStrings("", d.value);
}

test "a bare name is 1, as gcc defines it" {
    const d = middleware.splitDefine("-DRA8_THREADX_MODULES").?;
    try std.testing.expectEqualStrings("RA8_THREADX_MODULES", d.name);
    try std.testing.expectEqualStrings("1", d.value);
}

test "anything that is not a -D flag is not a define" {
    try std.testing.expect(middleware.splitDefine("-D") == null);
    try std.testing.expect(middleware.splitDefine("-D=1") == null);
    try std.testing.expect(middleware.splitDefine("-UFOO") == null);
    try std.testing.expect(middleware.splitDefine("-mcpu=cortex-m85") == null);
}

test "a Zig source's object is named after its file, like the C ones" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const source: middleware.ZigSource = .{
        .path = "port/threadx/src/cortex_m85_modules/external_memory_enable.zig",
        .cpu = &std.Target.arm.cpu.cortex_m85,
    };
    const name = try std.fmt.allocPrint(arena.allocator(), "{s}.o", .{std.fs.path.stem(source.path)});
    try std.testing.expectEqualStrings("external_memory_enable.o", name);
}

test "only the M85 Module Manager carries a Zig source (RA8FW-527)" {
    try std.testing.expectEqual(@as(usize, 1), m85_modules.threadx_m85_modules.zig_sources.len);
    try std.testing.expectEqual(@as(usize, 0), cpu1_modules.threadx_m33_modules.zig_sources.len);
}
