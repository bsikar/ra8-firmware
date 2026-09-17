//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The second (Cortex-M33 / CPU1) image a dual-core app embeds in its own
//! Cortex-M85 ELF (#1044).
//!
//! `ra8_add_app()` builds the M85 image and stops there. An app that also runs
//! code on the RA8D2's second core carries a hand-rolled `add_executable()` in
//! its OWN CMakeLists: four translation units at `-mcpu=cortex-m33`, its own
//! linker script, a narrow include path, then `objcopy` twice -- once to a raw
//! `.bin`, once to a relocatable object whose `.data` is renamed `.cpu1_image`
//! -- and that object is linked into the M85 ELF, where `linker_script.ld`
//! pins it at `ORIGIN(MRAM_CPU1)` so one flash drops both cores' images.
//!
//! Until this slice the root graph built the M85 ELF WITHOUT that object: it
//! linked, it was the right size for its own sections, and it was simply not
//! the image CMake produces, because the second core's half was missing. That
//! is the failure mode this file exists to close, and the reason the app-local
//! CMake outside `ra8_add_app()` has to be in the graph before #859 can delete
//! anything.
//!
//! Two flag facts are load-bearing and neither is visible in the CMakeLists:
//! the CPU1 target inherits the global `CMAKE_C_FLAGS` (the M85 `-mcpu`,
//! `-fdata-sections`, `-ffunction-sections`, `-O0 -g3 -DDEBUG`, `-std=gnu2x`)
//! and overrides only what it repeats, so the real command carries the M85 cpu
//! flags FIRST and the M33 ones after, last-wins; and it carries none of the
//! first-party warning profile, because those come from `ra8_add_app()`'s
//! target options and this target never calls it. Both are measured against a
//! real configure's `compile_commands.json`, not read off the listfile.

const std = @import("std");

/// The dual-core app this image belongs to, spelled as `CrossApp` spells it.
pub const App = struct {
    name: []const u8,
    dir: []const u8,
    board: []const u8,
};

/// The second image itself.
pub const Cpu1Image = struct {
    /// The M33 entry translation unit, relative to the app directory. It is
    /// also what the app names in `AUX_SRCS`, which is how it stays OUT of the
    /// M85 source set (#1036); the two rules are the same fact seen from both
    /// images.
    entry_source: []const u8,
    /// First-party translation units the M33 image links beside its entry
    /// point, repo-relative. Spelled out rather than globbed: this is a
    /// hand-written `add_executable()` naming four files, and a glob of
    /// `libs/ra8_hal/src` here would build a second 200-TU image.
    shared_sources: []const []const u8,
    /// The M33 linker script, relative to the app directory.
    linker_script: []const u8,
    /// The section the blob is renamed to, and which the M85 linker script
    /// pins at `ORIGIN(MRAM_CPU1)`.
    section: []const u8 = ".cpu1_image",
};

/// The CPU1 target's own compile options, in CMakeLists order. No warning
/// flags: the first-party profile rides on `ra8_add_app()` targets only.
pub const target_flags = [_][]const u8{
    "-mcpu=cortex-m33",
    "-mthumb",
    "-mfloat-abi=hard",
    "-mfpu=fpv5-sp-d16",
    "-ffreestanding",
    "-fno-builtin",
    "-fshort-enums",
    "-Os",
    "-g3",
};

/// `target_compile_definitions()` on the CPU1 target, plus the freestanding
/// define every cross TU in the tree carries.
pub const defines = [_][]const u8{ "-DRA8_BUILD_FOR_CPU1", "-DRA8_FREESTANDING" };

/// The CPU1 target's own link options. `-nostartfiles` (not `-nostdlib`, which
/// it inherits) and no `-lgcc`: the M33 target names no link libraries at all.
pub const link_target_flags = [_][]const u8{
    "-mcpu=cortex-m33",
    "-mthumb",
    "-mfloat-abi=hard",
    "-mfpu=fpv5-sp-d16",
    "-nostartfiles",
};

/// `<app>_cpu1`, the name CMake gives the second executable.
pub fn imageName(allocator: std.mem.Allocator, app: App) []const u8 {
    return std.fmt.allocPrint(allocator, "{s}_cpu1", .{app.name}) catch @panic("OOM");
}

/// Every translation unit the M33 image compiles: its entry point under the
/// app, then the shared first-party units.
pub fn sources(allocator: std.mem.Allocator, app: App, image: Cpu1Image) []const []const u8 {
    var out = std.ArrayList([]const u8).init(allocator);
    out.append(join(allocator, app.dir, image.entry_source)) catch @panic("OOM");
    out.appendSlice(image.shared_sources) catch @panic("OOM");
    return out.toOwnedSlice() catch @panic("OOM");
}

fn join(allocator: std.mem.Allocator, left: []const u8, right: []const u8) []const u8 {
    return std.fs.path.join(allocator, &.{ left, right }) catch @panic("OOM");
}

/// The narrow include path the M33 target repeats from `ra8_add_cpu1_image()`:
/// the app, core, HAL, and the board directory that carries the dual-core
/// memory map. Deliberately NOT the M85 app's path -- only freestanding-clean
/// headers may be reached from a Cortex-M33 TU, and the difference between the
/// two paths is the rule.
pub fn includeDirs(allocator: std.mem.Allocator, app: App) []const []const u8 {
    var out = std.ArrayList([]const u8).init(allocator);
    out.append(join(allocator, app.dir, "inc")) catch @panic("OOM");
    out.append(join(allocator, app.dir, "src")) catch @panic("OOM");
    out.append("libs/ra8_core/inc") catch @panic("OOM");
    out.append("libs/ra8_hal/inc") catch @panic("OOM");
    out.append(join(allocator, app.board, "inc")) catch @panic("OOM");
    return out.toOwnedSlice() catch @panic("OOM");
}

/// The compile flags for one CPU1 translation unit: the target's defines, then
/// the global flags it inherits, then its own options. Order is the whole
/// point -- gcc takes the last `-mcpu` and the last `-O`, so an implementation
/// that passed only `target_flags` would silently drop `-fdata-sections`,
/// `-ffunction-sections` and `-std=gnu2x`, and one that passed them in the
/// other order would build the M33 image for an M85.
pub fn compileFlags(
    allocator: std.mem.Allocator,
    global_flags: []const []const u8,
) []const []const u8 {
    var out = std.ArrayList([]const u8).init(allocator);
    out.appendSlice(&defines) catch @panic("OOM");
    out.appendSlice(global_flags) catch @panic("OOM");
    out.appendSlice(&target_flags) catch @panic("OOM");
    return out.toOwnedSlice() catch @panic("OOM");
}

/// What `add()` needs from the graph around it.
pub const Options = struct {
    gcc: []const u8,
    objcopy: []const u8,
    size: []const u8,
    app: App,
    image: Cpu1Image,
    /// The global `CMAKE_C_FLAGS` a cross TU inherits, compile side.
    global_compile_flags: []const []const u8,
    /// The global link flags, ahead of the CPU1 target's own.
    global_link_flags: []const []const u8,
};

/// Build the M33 image, install its `.elf` / `.hex` / `.bin` / `.map` beside
/// the M85 artifacts, and hand back the relocatable `.cpu1_image` object for
/// the caller to link into the M85 ELF.
pub fn add(b: *std.Build, step: *std.Build.Step, options: Options) std.Build.LazyPath {
    const name = imageName(b.allocator, options.app);
    const flags = compileFlags(b.allocator, options.global_compile_flags);
    const include_dirs = includeDirs(b.allocator, options.app);

    var objects = std.ArrayList(std.Build.LazyPath).init(b.allocator);
    for (sources(b.allocator, options.app, options.image)) |source| {
        const compile = b.addSystemCommand(&.{options.gcc});
        compile.addArgs(flags);
        for (include_dirs) |include_dir| {
            compile.addPrefixedDirectoryArg("-I", b.path(include_dir));
        }
        compile.addArg("-c");
        compile.addFileArg(b.path(source));
        compile.addArg("-o");
        const object_name = b.fmt("{s}.o", .{std.fs.path.basename(source)});
        objects.append(compile.addOutputFileArg(object_name)) catch @panic("OOM");
    }

    const link = b.addSystemCommand(&.{options.gcc});
    link.addArgs(options.global_link_flags);
    link.addArgs(&link_target_flags);
    link.addPrefixedFileArg("-T", b.path(b.pathJoin(&.{
        options.app.dir,
        options.image.linker_script,
    })));
    const map = link.addPrefixedOutputFileArg("-Wl,--Map=", b.fmt("{s}.map", .{name}));
    link.addArg("-o");
    const elf = link.addOutputFileArg(b.fmt("{s}.elf", .{name}));
    for (objects.items) |object| link.addFileArg(object);

    const hex = objcopyTo(b, options.objcopy, "ihex", elf, b.fmt("{s}.hex", .{name}));
    const bin = objcopyTo(b, options.objcopy, "binary", elf, b.fmt("{s}.bin", .{name}));

    // The raw image packed as a relocatable object whose single section is the
    // one the M85 linker script pins. objcopy also emits `_binary_*_start` /
    // `_end` / `_size` symbols derived from the INPUT PATH; nothing in the tree
    // references them (the M85 side finds the image through the linker
    // script's own symbols), which is why a build-directory-dependent symbol
    // name is not a parity difference.
    const pack = b.addSystemCommand(&.{
        options.objcopy,
        "-I",
        "binary",
        "-O",
        "elf32-littlearm",
        "-B",
        "arm",
        "--rename-section",
        b.fmt(".data={s},alloc,load,readonly,contents", .{options.image.section}),
    });
    pack.addFileArg(bin);
    const blob = pack.addOutputFileArg(b.fmt("{s}_blob.o", .{name}));

    inline for (.{ .{ elf, "elf" }, .{ hex, "hex" }, .{ bin, "bin" }, .{ map, "map" } }) |artifact| {
        step.dependOn(&b.addInstallFileWithDir(
            artifact[0],
            .{ .custom = "arm" },
            b.fmt("{s}.{s}", .{ name, artifact[1] }),
        ).step);
    }

    const report_size = b.addSystemCommand(&.{options.size});
    report_size.addFileArg(elf);
    step.dependOn(&report_size.step);

    return blob;
}

fn objcopyTo(
    b: *std.Build,
    objcopy_path: []const u8,
    format: []const u8,
    elf: std.Build.LazyPath,
    output_name: []const u8,
) std.Build.LazyPath {
    const run = b.addSystemCommand(&.{ objcopy_path, "-O", format });
    run.addFileArg(elf);
    return run.addOutputFileArg(output_name);
}

/// The CPU1 translation units, as compile-database rows. Generic over the
/// database's own entry type so this module does not depend on the emitter.
pub fn appendCompileDbEntries(
    b: *std.Build,
    comptime Entry: type,
    out: *std.ArrayList(Entry),
    driver: []const u8,
    options: Options,
) void {
    const name = imageName(b.allocator, options.app);
    const flags = compileFlags(b.allocator, options.global_compile_flags);
    const include_dirs = includeDirs(b.allocator, options.app);
    for (sources(b.allocator, options.app, options.image)) |source| {
        out.append(.{
            .file = source,
            .driver = driver,
            .flags = flags,
            .include_dirs = include_dirs,
            .object = b.fmt("arm/{s}/{s}.o", .{ name, std.fs.path.basename(source) }),
        }) catch @panic("OOM");
    }
}
