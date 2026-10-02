const std = @import("std");
const graph = @import("build_graph");
const cpu1_threadx = graph.cpu1_threadx;
const mw = graph.middleware;

const base = mw.Toolchain{
    .gcc = "arm-none-eabi-gcc",
    .ar = "arm-none-eabi-ar",
    .global_defines = &.{"-DRA8_FREESTANDING"},
    .c_flags = &.{ "-mcpu=cortex-m85", "-fdata-sections", "-O0", "-g3", "-std=gnu2x" },
    .asm_flags = &.{ "-mcpu=cortex-m85", "-g3" },
};

fn lastMcpu(flags: []const []const u8) []const u8 {
    var found: []const u8 = "";
    for (flags) |flag| {
        if (std.mem.startsWith(u8, flag, "-mcpu=")) found = flag;
    }
    return found;
}

fn contains(items: []const []const u8, needle: []const u8) bool {
    for (items) |item| {
        if (std.mem.indexOf(u8, item, needle) != null) return true;
    }
    return false;
}

test "the CPU1 kernel reads the cortex_m33 port and never the M85 one" {
    const k = cpu1_threadx.threadx_m33;
    const dirs = [_][]const []const u8{ k.soup_c_dirs, k.soup_asm_dirs, k.public_system_include_dirs };
    for (dirs) |set| {
        try std.testing.expect(contains(set, "cortex_m33"));
        try std.testing.expect(!contains(set, "cortex_m85"));
    }
    try std.testing.expect(contains(k.soup_c_dirs, "pkg:threadx/common/src"));
    try std.testing.expectEqual(@as(usize, 0), k.project_sources.len);
    try std.testing.expect(contains(k.public_include_dirs, "port/threadx/inc"));
    try std.testing.expect(contains(k.public_defines, "TX_INCLUDE_USER_DEFINE_FILE"));
}

test "the CPU1 kernel compiles C and assembly for the M33, CPU1 defines first" {
    const tc = cpu1_threadx.toolchain(std.testing.allocator, base);
    defer std.testing.allocator.free(tc.c_flags);
    defer std.testing.allocator.free(tc.asm_flags);
    try std.testing.expectEqualStrings("-mcpu=cortex-m33", lastMcpu(tc.c_flags));
    try std.testing.expectEqualStrings("-mcpu=cortex-m33", lastMcpu(tc.asm_flags));
    try std.testing.expectEqualStrings("-DRA8_BUILD_FOR_CPU1", tc.c_flags[0]);
    try std.testing.expectEqualStrings("-DRA8_BUILD_FOR_CPU1", tc.asm_flags[0]);
    try std.testing.expect(contains(tc.c_flags, "-std=gnu2x"));
    try std.testing.expect(contains(tc.asm_flags, "-mfpu=fpv5-sp-d16"));
}

test "the CPU1 kernel is not middleware any app can name yet" {
    try std.testing.expect(mw.find("threadx_m33") == null);
}

test "the CPU1 graph finds threadx_m33 by name and knows nothing else" {
    try std.testing.expectEqualStrings("threadx_m33", cpu1_threadx.find("threadx_m33").?.name);
    try std.testing.expect(cpu1_threadx.find("threadx") == null);
}
