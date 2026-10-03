const std = @import("std");
const graph = @import("build_graph");
const grant = graph.m85_shared_grant;
const m85 = graph.m85_threadx_modules;
const mw = graph.middleware;

test "a grant wholly below or above the board's shared SRAM is allowed" {
    try std.testing.expectEqual(grant.Verdict.allowed, grant.check(0x6800_0000, 0x1000));
    try std.testing.expectEqual(grant.Verdict.allowed, grant.check(grant.shared_base - 0x20, 0x20));
    try std.testing.expectEqual(grant.Verdict.allowed, grant.check(grant.shared_end, 0x20));
}

test "a grant inside or touching either edge of the shared SRAM is refused" {
    const refused = grant.Verdict.overlaps_shared;
    try std.testing.expectEqual(refused, grant.check(grant.shared_base, 0x20));
    try std.testing.expectEqual(refused, grant.check(grant.shared_base - 0x20, 0x21));
    try std.testing.expectEqual(refused, grant.check(grant.shared_end - 0x20, 0x20));
    try std.testing.expectEqual(refused, grant.check(0x2200_0000, 0x0020_0000));
}

test "an empty or wrapping grant is refused" {
    try std.testing.expectEqual(grant.Verdict.empty, grant.check(0x6800_0000, 0));
    try std.testing.expectEqual(grant.Verdict.wraps, grant.check(0xFFFF_FFE0, 0x40));
    try std.testing.expectEqual(grant.Verdict.allowed, grant.check(0xFFFF_FFE0, 0x20));
}

test "the shared SRAM bounds still match the board header" {
    const header = try std.fs.cwd().readFileAlloc(
        std.testing.allocator,
        "libs/ra8_board_ek_ra8d2/inc/ra8_board_ek_ra8d2_dualcore.h",
        1 << 20,
    );
    defer std.testing.allocator.free(header);
    const expected = [_][]const u8{
        "k_ra8_board_shared_ram_base = 0x22100000UL",
        "k_ra8_board_cpu1_sram_base  = 0x22190000UL",
        "k_ra8_board_cpu1_sram_size_bytes  = 0x10000UL",
    };
    for (expected) |line| try std.testing.expect(std.mem.indexOf(u8, header, line) != null);
    try std.testing.expectEqual(@as(u32, 0x221A_0000), grant.shared_end);
}

test "the M85 Module Manager takes the shared grant from Zig, not the port's C" {
    const m = m85.threadx_m85_modules;
    try std.testing.expect(mw.isReplaced(m, "txm_module_manager_external_memory_enable.c"));
    try std.testing.expectEqual(@as(usize, 1), m.zig_sources.len);
    try std.testing.expectEqualStrings(m85.external_memory_source, m.zig_sources[0].path);
    try std.testing.expect(!mw.isReplaced(graph.cpu1_threadx_modules.threadx_m33_modules, "txm_module_manager_external_memory_enable.c"));
}
