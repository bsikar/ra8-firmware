//! The hello-world ThreadX module for CPU1, the RA8D2's Cortex-M33 (RA8FW-430,
//! under RA8FW-290): the first module image a CPU1 Module Manager can load.
//!
//! It is built the way upstream's GNU module sample is
//! (`ports_module/cortex_m3/gnu/example_build/build_threadx_module_sample.bat`):
//! the module preamble and `gcc_setup` assembled position independent, the
//! module's own code, and the module library, linked with the module linker
//! script and entered at the thread shell. The M33 port ships the preamble
//! and `gcc_setup` but no script, so the linker script is the M3 sample's; its
//! FLASH/RAM origins are link addresses only, since `gcc_setup` rebases the
//! GOT and data on the addresses the manager loads the module at.
//!
//! The module's code is Zig, not C. Zig emits an `.ARM.exidx` cantunwind
//! entry the module script never places, so it is stripped from the entry
//! object before the link. `zig build txm-hello-m33` installs
//! `arm/txm_hello_m33.{elf,bin,map}`. A CPU1 image that names a module in
//! `txm_module` links that module's binary packed into `.txm_module` (`pack`,
//! RA8FW-431). Every module builds the same way and differs only in its name
//! and its Zig start thread (`Module`, RA8FW-458).

const std = @import("std");
const middleware = @import("middleware.zig");
const pkg_path = @import("pkg_path.zig");
const cpu1_image = @import("cpu1_image.zig");
const cpu1_txm_lib = @import("cpu1_txm_lib.zig");
const module_object = @import("txm_module_object.zig");

pub const step_description = "Build the hello-world ThreadX module for CPU1 (Cortex-M33) as arm/txm_hello_m33.elf";

/// One CPU1 module: the name its outputs carry and its Zig start thread.
pub const Module = struct {
    name: []const u8,
    entry_source: []const u8,
    /// Build the start thread through Zig's C backend and gcc with the
    /// module flags, and link the rebase pass and table (RA8FW-539). A
    /// module that keeps any address in its data needs this; one that keeps
    /// none, like the two below, is built natively as before.
    through_c: bool = false,
    /// The core the module runs on. Only a module built through C can be
    /// for the M85 (RA8FW-820); the native recipe is M33 only.
    core: module_object.Core = .cortex_m33,
};

/// The hello-world module, and the one the `txm-hello-m33` step installs.
pub const hello_world = Module{
    .name = "txm_hello_m33",
    .entry_source = "examples/ek_ra8d2/hw_pending/txm_hello_m33/module_start.zig",
};

/// The negative case: a module whose start thread stores outside its MPU
/// regions, for txm_fault_cpu1 (RA8FW-459).
pub const fault = Module{
    .name = "txm_fault_m33",
    .entry_source = "examples/ek_ra8d2/hw_pending/txm_fault_m33/module_start.zig",
};

/// The module that keeps a table of function pointers in its data and
/// reports what it computes through it, for txm_table_cpu1 (RA8FW-539).
pub const table = Module{
    .name = "txm_table_m33",
    .entry_source = "tests/zig_build_graph/txm_module_probes/table.zig",
    .through_c = true,
};

/// The module that calls a resident-image service through ra8_rpc over
/// ThreadX queues, for txm_rpc_cpu1 (RA8FW-544). Its client and transport
/// reach their queues through vtables in its data, so it goes through C.
pub const rpc = Module{
    .name = "txm_rpc_m33",
    .entry_source = "examples/ek_ra8d2/hw_pending/txm_rpc_cpu1/src/module_start.zig",
    .through_c = true,
};

/// The M85 module with MVE that checks its own Q0-Q7 and VPR across
/// preemption, for txm_helium_m85 (RA8FW-820, under RA8FW-428).
pub const helium = Module{
    .name = "txm_helium_m85",
    .entry_source = "examples/ek_ra8d2/hw_pending/txm_helium_m85/module_start.zig",
    .through_c = true,
    .core = .cortex_m85,
};

/// Every module an image can name in `txm_module`.
pub const modules = [_]Module{ hello_world, fault, table, rpc, helium };

pub const name = hello_world.name;
pub const entry_source = hello_world.entry_source;

/// The module called `wanted`, or null when there is none.
pub fn find(wanted: []const u8) ?Module {
    for (modules) |module| {
        if (std.mem.eql(u8, module.name, wanted)) return module;
    }
    return null;
}
pub const preamble = "pkg:threadx/ports_module/cortex_m33/gnu/example_build/txm_module_preamble.S";
pub const gcc_setup = "pkg:threadx/ports_module/cortex_m33/gnu/example_build/gcc_setup.s";
pub const linker_script = "pkg:threadx/ports_module/cortex_m3/gnu/example_build/sample_threadx_module.ld";

/// The module's entry: the shell that sets up the GOT, then runs the start
/// thread the preamble names.
pub const entry_symbol = "_txm_module_thread_shell_entry";

/// The first word of every module image, upstream's preamble ID ("MODU").
pub const preamble_id: u32 = 0x4D4F4455;

/// The section a CPU1 image's linker script places the packed module in.
pub const module_section = ".txm_module";

/// What objcopy removes from the Zig entry object.
pub const strip_args = [_][]const u8{ "-R", ".ARM.exidx", "-R", ".rel.ARM.exidx" };

/// The module link: the M33 target, no start files or libraries, the shell
/// as the entry.
pub const link_flags = [_][]const u8{
    "-mcpu=cortex-m33",
    "-mthumb",
    "-mfloat-abi=hard",
    "-mfpu=fpv5-sp-d16",
    "-nostdlib",
    "-nostartfiles",
    "-Wl,-e," ++ entry_symbol,
    "-Wl,-z,noexecstack",
    "-Wl,--no-warn-rwx-segments",
};

/// The CPU1 target flags with the module PIC flags appended, for the
/// preamble and `gcc_setup`.
pub fn asmFlags(allocator: std.mem.Allocator) []const []const u8 {
    var flags = std.ArrayList([]const u8).init(allocator);
    flags.appendSlice(&cpu1_image.target_flags) catch @panic("OOM");
    flags.appendSlice(&cpu1_txm_lib.pic_flags) catch @panic("OOM");
    return flags.toOwnedSlice() catch @panic("OOM");
}

/// The module's link outputs. A module built through C has no map.
pub const Artifacts = struct {
    elf: std.Build.LazyPath,
    bin: std.Build.LazyPath,
    map: ?std.Build.LazyPath,
};

/// Builds `module`, installs its ELF, binary and map under `arm/`, and
/// returns them for a later step (txm_ra8app.zig packs the binary).
pub fn add(b: *std.Build, step: *std.Build.Step, base: middleware.Toolchain, objcopy: []const u8, module: Module) Artifacts {
    const built = image(b, base, objcopy, module);
    const outputs = [_]struct { ?std.Build.LazyPath, []const u8 }{
        .{ built.elf, "elf" }, .{ built.bin, "bin" }, .{ built.map, "map" },
    };
    for (outputs) |artifact| {
        const path = artifact[0] orelse continue;
        step.dependOn(&b.addInstallFileWithDir(
            path,
            .{ .custom = "arm" },
            b.fmt("{s}.{s}", .{ module.name, artifact[1] }),
        ).step);
    }
    return built;
}

/// Links `module` and converts it to a raw binary.
pub fn image(b: *std.Build, base: middleware.Toolchain, objcopy: []const u8, module: Module) Artifacts {
    if (module.through_c) return imageThroughC(b, base, objcopy, module);
    if (module.core != .cortex_m33) @panic("a module for the M85 must be built through C");
    const archive = middleware.add(b, cpu1_txm_lib.txm_m33, cpu1_txm_lib.toolchain(b.allocator, base));
    const flags = asmFlags(b.allocator);

    const link = b.addSystemCommand(&.{base.gcc});
    link.addArgs(&link_flags);
    link.addPrefixedFileArg("-T", pkg_path.lazy(b, linker_script));
    const map = link.addPrefixedOutputFileArg("-Wl,--Map=", b.fmt("{s}.map", .{module.name}));
    link.addArg("-o");
    const elf = link.addOutputFileArg(b.fmt("{s}.elf", .{module.name}));
    link.addFileArg(assemble(b, base.gcc, flags, preamble, "txm_module_preamble.o"));
    link.addFileArg(assemble(b, base.gcc, flags, gcc_setup, "gcc_setup.o"));
    link.addFileArg(zigEntry(b, objcopy, module));
    link.addFileArg(archive);

    const to_bin = b.addSystemCommand(&.{ objcopy, "-O", "binary" });
    to_bin.addFileArg(elf);
    return .{ .elf = elf, .bin = to_bin.addOutputFileArg(b.fmt("{s}.bin", .{module.name})), .map = map };
}

/// The same, for a module whose Zig goes through C: txm_module_object.zig's
/// recipe, with the toolchain this graph already drives.
fn imageThroughC(
    b: *std.Build,
    base: middleware.Toolchain,
    objcopy: []const u8,
    module: Module,
) Artifacts {
    const ctx: module_object.Context = .{
        .gnu = .{ .gcc = base.gcc, .ar = base.ar, .objcopy = objcopy },
        .core = module.core,
        .base = base,
    };
    const root = module_object.rootModule(b, ctx.core, module.entry_source);
    const entry = module_object.object(b, ctx, module.name, root);
    const elf = module_object.link(b, ctx, module.name, entry, null);
    const to_bin = b.addSystemCommand(&.{ objcopy, "-O", "binary" });
    to_bin.addFileArg(elf);
    const bin = to_bin.addOutputFileArg(b.fmt("{s}.bin", .{module.name}));
    return .{ .elf = elf, .bin = bin, .map = null };
}

/// `module`'s binary as a relocatable object whose one section is
/// `module_section`, for a CPU1 image to link.
pub fn pack(b: *std.Build, objcopy: []const u8, module: Module, bin: std.Build.LazyPath) std.Build.LazyPath {
    const run = b.addSystemCommand(&.{ objcopy, "-I", "binary", "-O", "elf32-littlearm", "-B", "arm", "--rename-section" });
    run.addArg(".data=" ++ module_section ++ ",alloc,load,readonly,contents");
    run.addFileArg(bin);
    return run.addOutputFileArg(b.fmt("{s}_module.o", .{module.name}));
}

fn assemble(b: *std.Build, gcc: []const u8, flags: []const []const u8, source: []const u8, object: []const u8) std.Build.LazyPath {
    const run = b.addSystemCommand(&.{gcc});
    run.addArgs(flags);
    run.addArg("-c");
    run.addFileArg(pkg_path.lazy(b, source));
    run.addArg("-o");
    return run.addOutputFileArg(object);
}

/// The Zig start thread as one object for the M33, its unwind index removed.
fn zigEntry(b: *std.Build, objcopy: []const u8, module: Module) std.Build.LazyPath {
    const root = b.createModule(.{
        .root_source_file = b.path(module.entry_source),
        .target = b.resolveTargetQuery(cpu1_image.zig_target_query),
        .optimize = .ReleaseSmall,
        .unwind_tables = .none,
    });
    const object = b.addObject(.{ .name = b.fmt("{s}_entry", .{module.name}), .root_module = root });
    const strip = b.addSystemCommand(&.{objcopy});
    strip.addArgs(&strip_args);
    strip.addFileArg(object.getEmittedBin());
    return strip.addOutputFileArg(b.fmt("{s}_entry.o", .{module.name}));
}
