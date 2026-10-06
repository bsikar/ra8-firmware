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

test "a CPU1 image finds the packed module in .txm_module" {
    try std.testing.expectEqualStrings(".txm_module", hello.module_section);
}

test "every module a CPU1 image names is one the build knows" {
    for (graph.cross_apps) |app| {
        const image = app.cpu1 orelse continue;
        const wanted = image.txm_module orelse continue;
        const module = hello.find(wanted) orelse return error.UnknownModule;
        try std.testing.expectEqualStrings(wanted, module.name);
        try std.testing.expect(std.mem.endsWith(u8, module.entry_source, ".zig"));
    }
}

test "txm_manager_cpu1 packs the hello-world module and an unknown name finds nothing" {
    try std.testing.expectEqualStrings(hello.hello_world.entry_source, hello.find("txm_hello_m33").?.entry_source);
    try std.testing.expect(hello.find("txm_missing_m33") == null);
    for (graph.cross_apps) |app| {
        if (!std.mem.eql(u8, app.name, "txm_manager_cpu1")) continue;
        try std.testing.expectEqualStrings(hello.hello_world.name, app.cpu1.?.txm_module.?);
    }
}

test "txm_table_cpu1 packs the table module, the one built through C" {
    try std.testing.expectEqualStrings("txm_table_m33", hello.find("txm_table_m33").?.name);
    try std.testing.expect(hello.table.through_c);
    try std.testing.expect(!hello.hello_world.through_c);
    try std.testing.expect(!hello.fault.through_c);
    for (graph.cross_apps) |app| {
        if (!std.mem.eql(u8, app.name, "txm_table_cpu1")) continue;
        try std.testing.expectEqualStrings(hello.table.name, app.cpu1.?.txm_module.?);
    }
}

test "txm_rpc_cpu1 packs the RPC module, built through C, and only the two RPC images import ra8_rpc" {
    try std.testing.expectEqualStrings("txm_rpc_m33", hello.find("txm_rpc_m33").?.name);
    try std.testing.expect(hello.rpc.through_c);
    for (graph.cross_apps) |app| {
        const image = app.cpu1 orelse continue;
        const is_rpc = std.mem.eql(u8, app.name, "txm_rpc_cpu1");
        // txm_dual_mailbox's CPU1 refuses calls itself once its module is
        // gone (RA8FW-842).
        const refuses = std.mem.eql(u8, app.name, "txm_dual_mailbox");
        try std.testing.expectEqual(is_rpc or refuses, image.rpc);
        if (is_rpc) try std.testing.expectEqualStrings(hello.rpc.name, image.txm_module.?);
    }
}

test "txm_dual_mailbox packs the client module for the M85 and the server module for CPU1, both through C" {
    try std.testing.expectEqualStrings("txm_dual_server_m33", hello.find("txm_dual_server_m33").?.name);
    try std.testing.expectEqualStrings("txm_dual_client_m33", hello.find("txm_dual_client_m33").?.name);
    try std.testing.expect(hello.dual_server.through_c);
    try std.testing.expect(hello.dual_client.through_c);
    try std.testing.expect(std.mem.endsWith(u8, hello.dual_server.entry_source, "txm_dual_mailbox/src/server_module.zig"));
    try std.testing.expect(std.mem.endsWith(u8, hello.dual_client.entry_source, "txm_dual_mailbox/src/client_module.zig"));
    var found = false;
    for (graph.cross_apps) |app| {
        if (!std.mem.eql(u8, app.name, "txm_dual_mailbox")) continue;
        found = true;
        try std.testing.expectEqualStrings(hello.dual_server.name, app.cpu1.?.txm_module.?);
        try std.testing.expectEqualStrings(hello.dual_client.name, app.txm_module.?);
    }
    try std.testing.expect(found);
}

test "txm_fault_cpu1 packs the negative module, whose code is Zig beside the hello module's" {
    try std.testing.expectEqualStrings("txm_fault_m33", hello.find("txm_fault_m33").?.name);
    try std.testing.expect(std.mem.endsWith(u8, hello.fault.entry_source, "txm_fault_m33/module_start.zig"));
    for (graph.cross_apps) |app| {
        if (!std.mem.eql(u8, app.name, "txm_fault_cpu1")) continue;
        try std.testing.expectEqualStrings(hello.fault.name, app.cpu1.?.txm_module.?);
    }
}

test "txm_helium_m85 is built through C for the M85, and every other module stays on the M33" {
    const helium = hello.find("txm_helium_m85").?;
    try std.testing.expect(helium.through_c);
    try std.testing.expectEqual(graph.txm_module_target.Core.cortex_m85, helium.core);
    try std.testing.expect(mentions(helium.core.cpuFlags(), "-mcpu=cortex-m85"));
    try std.testing.expect(std.mem.endsWith(u8, helium.entry_source, "txm_helium_m85/module_start.zig"));
    for ([_]hello.Module{ hello.hello_world, hello.fault, hello.table, hello.rpc, hello.dual_server, hello.dual_client }) |module| {
        try std.testing.expectEqual(graph.txm_module_target.Core.cortex_m33, module.core);
    }
}
