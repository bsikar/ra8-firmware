const std = @import("std");
const graph = @import("build_graph");
const manager = graph.m85_txm_manager;
const mw = graph.middleware;
const m85 = graph.m85_threadx_modules;

test "an app that is not a Module Manager keeps its middleware set unchanged" {
    const mws = [_]mw.Middleware{mw.threadx};
    const out = manager.kernelFor(std.testing.allocator, &mws, false);
    try std.testing.expectEqual(@as(usize, 1), out.len);
    try std.testing.expectEqualStrings("threadx", out[0].name);
    try std.testing.expectEqual(@intFromPtr(&mws), @intFromPtr(out.ptr));
}

test "an M85 Module Manager app links threadx_m85_modules in threadx's place" {
    const mws = [_]mw.Middleware{mw.threadx};
    const out = manager.kernelFor(std.testing.allocator, &mws, true);
    defer std.testing.allocator.free(out);
    try std.testing.expectEqual(@as(usize, 1), out.len);
    try std.testing.expectEqualStrings(m85.threadx_m85_modules.name, out[0].name);
    try std.testing.expectEqualStrings("threadx", mws[0].name);
}

test "the swap keeps every other middleware in its position" {
    var other = mw.threadx;
    other.name = "probe_other";
    const mws = [_]mw.Middleware{ other, mw.threadx, other };
    const out = manager.kernelFor(std.testing.allocator, &mws, true);
    defer std.testing.allocator.free(out);
    try std.testing.expectEqualStrings("probe_other", out[0].name);
    try std.testing.expectEqualStrings("threadx_m85_modules", out[1].name);
    try std.testing.expectEqualStrings("probe_other", out[2].name);
}
