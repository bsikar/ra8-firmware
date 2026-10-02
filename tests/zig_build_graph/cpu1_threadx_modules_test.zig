const std = @import("std");
const graph = @import("build_graph");
const modules = graph.cpu1_threadx_modules;
const cpu1_threadx = graph.cpu1_threadx;
const mw = graph.middleware;

fn mentions(items: []const []const u8, needle: []const u8) bool {
    for (items) |item| {
        if (std.mem.indexOf(u8, item, needle) != null) return true;
    }
    return false;
}

test "the Module Manager reads the cortex_m33 module port and never a plain or M85 port" {
    const m = modules.threadx_m33_modules;
    try std.testing.expect(mentions(m.soup_c_dirs, "common_modules/module_manager/src"));
    try std.testing.expect(mentions(m.soup_c_dirs, "ports_module/cortex_m33/gnu/module_manager/src"));
    try std.testing.expect(mentions(m.public_system_include_dirs, "ports_module/cortex_m33/gnu/inc"));
    for ([_][]const []const u8{ m.soup_c_dirs, m.soup_asm_dirs, m.soup_cpp_asm_dirs, m.public_system_include_dirs }) |dirs| {
        try std.testing.expect(!mentions(dirs, "ports/cortex_m33"));
        try std.testing.expect(!mentions(dirs, "cortex_m85"));
    }
}

test "the port's preprocessed lowercase assembly is collected" {
    try std.testing.expectEqual(@as(usize, 1), modules.threadx_m33_modules.soup_cpp_asm_dirs.len);
}

test "the Module Manager is the whole kernel, not a layer on threadx_m33" {
    const m = modules.threadx_m33_modules;
    try std.testing.expect(mentions(m.soup_c_dirs, "threadx/common/src"));
    try std.testing.expectEqual(@as(usize, 0), m.requires.len);
}

test "a CPU1 image can name the Module Manager, and it gets the glue" {
    try std.testing.expect(cpu1_threadx.find(modules.threadx_m33_modules.name) != null);
    try std.testing.expect(cpu1_threadx.wantsGlue(&.{"threadx_m33_modules"}));
    try std.testing.expect(cpu1_threadx.wantsGlue(&.{"threadx_m33"}));
    try std.testing.expect(!cpu1_threadx.wantsGlue(&.{}));
}

test "the two CPU1 kernels are counted so naming both can be refused" {
    try std.testing.expectEqual(@as(usize, 1), cpu1_threadx.kernelCount(&.{"threadx_m33_modules"}));
    try std.testing.expectEqual(@as(usize, 2), cpu1_threadx.kernelCount(&.{ "threadx_m33", "threadx_m33_modules" }));
    try std.testing.expectEqual(@as(usize, 0), cpu1_threadx.kernelCount(&.{"other"}));
}

test "resolving the opt-in hands back the Module Manager archive alone" {
    const got = cpu1_threadx.resolve(std.testing.allocator, &.{"threadx_m33_modules"});
    defer std.testing.allocator.free(got);
    try std.testing.expectEqual(@as(usize, 1), got.len);
    try std.testing.expectEqualStrings("threadx_m33_modules", got[0].name);
}

test "only a lowercase .s unit gets the preprocessor switch" {
    const plain = mw.Unit{ .path = "a.S", .language = .assembly };
    const cpp = mw.Unit{ .path = "a.s", .language = .assembly_cpp };
    const c = mw.Unit{ .path = "a.c", .language = .c };
    try std.testing.expectEqual(@as(usize, 0), mw.languageFlags(plain).len);
    try std.testing.expectEqual(@as(usize, 0), mw.languageFlags(c).len);
    try std.testing.expectEqualStrings("-x", mw.languageFlags(cpp)[0]);
    try std.testing.expectEqualStrings("assembler-with-cpp", mw.languageFlags(cpp)[1]);
}

test "the Module Manager keeps notify callbacks and tells the assembly it is single-mode secure" {
    const defines = modules.threadx_m33_modules.public_defines;
    try std.testing.expect(mentions(defines, "-DRA8_THREADX_MODULES"));
    try std.testing.expect(mentions(defines, "-DTX_SINGLE_MODE_SECURE="));
}
