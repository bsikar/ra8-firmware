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
//! `arm/txm_hello_m33.{elf,bin,map}`; no other image links it.

const std = @import("std");
const middleware = @import("middleware.zig");
const pkg_path = @import("pkg_path.zig");
const cpu1_image = @import("cpu1_image.zig");
const cpu1_txm_lib = @import("cpu1_txm_lib.zig");

pub const step_description = "Build the hello-world ThreadX module for CPU1 (Cortex-M33) as arm/txm_hello_m33.elf";

pub const name = "txm_hello_m33";
pub const entry_source = "examples/ek_ra8d2/hw_pending/txm_hello_m33/module_start.zig";
pub const preamble = "pkg:threadx/ports_module/cortex_m33/gnu/example_build/txm_module_preamble.S";
pub const gcc_setup = "pkg:threadx/ports_module/cortex_m33/gnu/example_build/gcc_setup.s";
pub const linker_script = "pkg:threadx/ports_module/cortex_m3/gnu/example_build/sample_threadx_module.ld";

/// The module's entry: the shell that sets up the GOT, then runs the start
/// thread the preamble names.
pub const entry_symbol = "_txm_module_thread_shell_entry";

/// The first word of every module image, upstream's preamble ID ("MODU").
pub const preamble_id: u32 = 0x4D4F4455;

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

/// Builds the module and installs its ELF, binary and map under `arm/`.
pub fn add(b: *std.Build, step: *std.Build.Step, base: middleware.Toolchain, objcopy: []const u8) void {
    const archive = middleware.add(b, cpu1_txm_lib.txm_m33, cpu1_txm_lib.toolchain(b.allocator, base));
    const flags = asmFlags(b.allocator);

    const link = b.addSystemCommand(&.{base.gcc});
    link.addArgs(&link_flags);
    link.addPrefixedFileArg("-T", pkg_path.lazy(b, linker_script));
    const map = link.addPrefixedOutputFileArg("-Wl,--Map=", name ++ ".map");
    link.addArg("-o");
    const elf = link.addOutputFileArg(name ++ ".elf");
    link.addFileArg(assemble(b, base.gcc, flags, preamble, "txm_module_preamble.o"));
    link.addFileArg(assemble(b, base.gcc, flags, gcc_setup, "gcc_setup.o"));
    link.addFileArg(zigEntry(b, objcopy));
    link.addFileArg(archive);

    const to_bin = b.addSystemCommand(&.{ objcopy, "-O", "binary" });
    to_bin.addFileArg(elf);
    const bin = to_bin.addOutputFileArg(name ++ ".bin");

    inline for (.{ .{ elf, "elf" }, .{ bin, "bin" }, .{ map, "map" } }) |artifact| {
        step.dependOn(&b.addInstallFileWithDir(
            artifact[0],
            .{ .custom = "arm" },
            name ++ "." ++ artifact[1],
        ).step);
    }
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
fn zigEntry(b: *std.Build, objcopy: []const u8) std.Build.LazyPath {
    const root = b.createModule(.{
        .root_source_file = b.path(entry_source),
        .target = b.resolveTargetQuery(cpu1_image.zig_target_query),
        .optimize = .ReleaseSmall,
        .unwind_tables = .none,
    });
    const object = b.addObject(.{ .name = name ++ "_entry", .root_module = root });
    const strip = b.addSystemCommand(&.{objcopy});
    strip.addArgs(&strip_args);
    strip.addFileArg(object.getEmittedBin());
    return strip.addOutputFileArg(name ++ "_entry.o");
}
