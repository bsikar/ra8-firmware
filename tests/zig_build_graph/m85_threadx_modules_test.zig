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

test "the M85 Module Manager schedules with the copy that keeps the board MPU map" {
    const m = m85.threadx_m85_modules;
    try std.testing.expect(mw.isReplaced(m, "tx_thread_schedule.S"));
    try std.testing.expect(mentions(m.project_sources, m85.schedule_source));
    try std.testing.expect(!mw.isReplaced(mw.threadx, "tx_thread_schedule.S"));
    for (mw.threadx.project_sources) |source| {
        try std.testing.expect(mentions(m.project_sources, source));
    }
}

test "the M85 Module Manager carries the Module Manager defines" {
    const defines = m85.threadx_m85_modules.public_defines;
    try std.testing.expect(mentions(defines, "-DTX_INCLUDE_USER_DEFINE_FILE"));
    try std.testing.expect(mentions(defines, "-DRA8_THREADX_MODULES"));
    try std.testing.expect(mentions(defines, "-DTX_SINGLE_MODE_SECURE="));
}

test "the M85 module MPU budget leaves one region for the board (RA8FW-484)" {
    try std.testing.expectEqual(@as(u32, 8), m85.module_entries + m85.board_entries);
    try std.testing.expectEqual(@as(u32, m85.dregion), m85.module_entries + m85.board_entries);
    // Kernel entry, code, data, then the shared grants fill the module's part.
    try std.testing.expectEqual(@as(u32, m85.module_entries), 3 + m85.shared_entries);
    const modules = m85.threadx_m85_modules;
    try std.testing.expectEqual(@as(usize, 1), modules.patched_headers.len);
    try std.testing.expectEqual(@as(usize, 2), modules.patched_headers[0].rewrites.len);
    try std.testing.expect(std.mem.endsWith(u8, modules.patched_headers[0].rewrites[0].new, " 7"));
    try std.testing.expect(std.mem.endsWith(u8, modules.patched_headers[0].rewrites[1].new, " 4"));
}

test "the M85 scheduler programs the module's 7 regions and the board's shared one (RA8FW-484)" {
    const schedule = try std.fs.cwd().readFileAlloc(
        std.testing.allocator,
        "port/threadx/src/cortex_m85_modules/tx_thread_schedule.S",
        1 << 20,
    );
    defer std.testing.allocator.free(schedule);
    const expected = [_][]const u8{
        "LDR     r0, [r1, #0x90]", // module instance pointer
        "LDR     r2, [r0, #0x74]", // data region RBAR
        "ADD     r0, r0, #0x64", // MPU registers in the instance
        "LDM     r0, {r2-r7}", // module entries 4..6
        "LDRD    r4, r5, [r6, #32]", // board slot 4 = shared_board_region * 8
        "MOV     r2, #7",
    };
    for (expected) |line| {
        if (std.mem.indexOf(u8, schedule, line) == null) {
            std.debug.print("missing from the M85 scheduler: {s}\n", .{line});
            return error.TestUnexpectedResult;
        }
    }
    try std.testing.expectEqual(@as(u32, 32), m85.shared_board_region * 8);
    try std.testing.expect(std.mem.indexOf(u8, schedule, "LDM     r0, {r2-r9}") == null);
}
