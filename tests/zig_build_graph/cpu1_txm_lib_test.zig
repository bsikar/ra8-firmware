const std = @import("std");
const graph = @import("build_graph");
const txm = graph.cpu1_txm_lib;
const mw = graph.middleware;

fn mentions(items: []const []const u8, needle: []const u8) bool {
    for (items) |item| {
        if (std.mem.indexOf(u8, item, needle) != null) return true;
    }
    return false;
}

test "the module library reads the module sources and the cortex_m33 module port only" {
    const m = txm.txm_m33;
    try std.testing.expect(mentions(m.soup_c_dirs, "common_modules/module_lib/src"));
    try std.testing.expect(mentions(m.soup_c_dirs, "ports_module/cortex_m33/gnu/module_lib/src"));
    for ([_][]const []const u8{ m.soup_c_dirs, m.public_system_include_dirs }) |dirs| {
        try std.testing.expect(!mentions(dirs, "module_manager"));
        try std.testing.expect(!mentions(dirs, "threadx/common/src"));
        try std.testing.expect(!mentions(dirs, "cortex_m85"));
    }
    try std.testing.expectEqual(@as(usize, 0), m.soup_asm_dirs.len);
}

test "the module library is built with the kernel's configuration" {
    const defines = txm.txm_m33.public_defines;
    try std.testing.expect(mentions(defines, "-DTX_INCLUDE_USER_DEFINE_FILE"));
    try std.testing.expect(mentions(defines, "-DRA8_THREADX_MODULES"));
}

test "every C unit is position independent with data through r9" {
    const base = mw.Toolchain{
        .gcc = "gcc",
        .ar = "ar",
        .global_defines = &.{},
        .c_flags = &.{"-std=gnu2x"},
        .asm_flags = &.{},
    };
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const tc = txm.toolchain(arena.allocator(), base);
    for (txm.pic_flags) |flag| try std.testing.expect(mentions(tc.c_flags, flag));
    try std.testing.expect(mentions(tc.c_flags, "-mcpu=cortex-m33"));
    try std.testing.expect(!mentions(tc.c_flags, "-mpic-data-is-text-relative"));
}
