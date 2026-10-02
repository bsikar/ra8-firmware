const std = @import("std");
const graph = @import("build_graph");
const m85 = graph.m85_threadx_modules;
const mw = graph.middleware;

fn mentions(items: []const []const u8, needle: []const u8) bool {
    for (items) |item| {
        if (std.mem.indexOf(u8, item, needle) != null) return true;
    }
    return false;
}

test "the M85 Module Manager is the whole kernel built from the cortex_m33 module port" {
    const m = m85.threadx_m85_modules;
    try std.testing.expect(mentions(m.soup_c_dirs, "threadx/common/src"));
    try std.testing.expect(mentions(m.soup_c_dirs, "common_modules/module_manager/src"));
    try std.testing.expect(mentions(m.soup_c_dirs, "ports_module/cortex_m33/gnu/module_manager/src"));
    try std.testing.expect(mentions(m.public_system_include_dirs, "ports_module/cortex_m33/gnu/inc"));
    for ([_][]const []const u8{ m.soup_c_dirs, m.soup_asm_dirs, m.soup_cpp_asm_dirs, m.public_system_include_dirs }) |dirs| {
        try std.testing.expect(!mentions(dirs, "threadx/ports/"));
    }
    try std.testing.expectEqual(@as(usize, 0), m.requires.len);
}

test "the M85 Module Manager keeps the M85 kernel's low-level init and SysTick glue" {
    const m = m85.threadx_m85_modules;
    try std.testing.expect(mentions(m.replaced_basenames, "tx_initialize_low_level.S"));
    try std.testing.expect(mentions(m.project_sources, "port/threadx/src/cortex_m85/tx_initialize_low_level.S"));
    try std.testing.expectEqual(mw.threadx.link_options.len, m.link_options.len);
}

test "the M85 Module Manager carries the Module Manager defines" {
    const defines = m85.threadx_m85_modules.public_defines;
    try std.testing.expect(mentions(defines, "-DTX_INCLUDE_USER_DEFINE_FILE"));
    try std.testing.expect(mentions(defines, "-DRA8_THREADX_MODULES"));
    try std.testing.expect(mentions(defines, "-DTX_SINGLE_MODE_SECURE="));
}
