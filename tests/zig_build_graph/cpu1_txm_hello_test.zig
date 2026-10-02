const std = @import("std");
const graph = @import("build_graph");
const hello = graph.cpu1_txm_hello;
const txm = graph.cpu1_txm_lib;

fn mentions(items: []const []const u8, needle: []const u8) bool {
    for (items) |item| {
        if (std.mem.indexOf(u8, item, needle) != null) return true;
    }
    return false;
}

test "the preamble and gcc_setup come from the cortex_m33 module port" {
    for ([_][]const u8{ hello.preamble, hello.gcc_setup }) |path| {
        try std.testing.expect(std.mem.startsWith(u8, path, "pkg:threadx/ports_module/cortex_m33/gnu/example_build/"));
    }
    try std.testing.expect(std.mem.endsWith(u8, hello.linker_script, "sample_threadx_module.ld"));
}

test "the module's own code is Zig" {
    try std.testing.expect(std.mem.endsWith(u8, hello.entry_source, ".zig"));
    try std.testing.expect(std.mem.startsWith(u8, hello.entry_source, "examples/"));
}

test "the preamble and gcc_setup are assembled position independent for the M33" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const flags = hello.asmFlags(arena.allocator());
    for (txm.pic_flags) |flag| try std.testing.expect(mentions(flags, flag));
    try std.testing.expect(mentions(flags, "-mcpu=cortex-m33"));
}

test "the link enters at the thread shell with no start files" {
    try std.testing.expect(mentions(&hello.link_flags, "-Wl,-e,_txm_module_thread_shell_entry"));
    try std.testing.expect(mentions(&hello.link_flags, "-nostartfiles"));
    try std.testing.expect(mentions(&hello.link_flags, "-mcpu=cortex-m33"));
}

test "the unwind index and its relocations are both stripped" {
    try std.testing.expect(mentions(&hello.strip_args, ".ARM.exidx"));
    try std.testing.expect(mentions(&hello.strip_args, ".rel.ARM.exidx"));
    try std.testing.expectEqual(@as(u32, 0x4D4F4455), hello.preamble_id);
}
