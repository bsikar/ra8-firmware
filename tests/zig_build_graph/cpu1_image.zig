//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The second (Cortex-M33 / CPU1) image a dual-core app embeds in its own
//! Cortex-M85 ELF.
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
//! CMake outside `ra8_add_app()` has to be in the graph before RA8FW-332 can delete
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
const cpu1_threadx = @import("cpu1_threadx.zig");
const middleware = @import("middleware.zig");
const pkg_path = @import("pkg_path.zig");

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
    /// M85 source set; the two rules are the same fact seen from both
    /// images.
    entry_source: []const u8,
    /// First-party translation units the M33 image links beside its entry
    /// point, repo-relative. Spelled out rather than globbed: this is a
    /// hand-written `add_executable()` naming four files, and a glob of
    /// `libs/ra8_hal/src` here would build a second 200-TU image.
    shared_sources: []const []const u8,
    /// The M33 linker script, relative to the app directory, falling back to
    /// the board layer's shared copy when the app has none. See `linkerScript`.
    linker_script: []const u8,
    /// The section the blob is renamed to, and which the M85 linker script
    /// pins at `ORIGIN(MRAM_CPU1)`.
    section: []const u8 = ".cpu1_image",
    /// Whether the board layer's `inc/` is the last directory on this image's
    /// include path. It is per-app data rather than part of the path below
    /// because the two dual-core apps disagree and neither answer is a
    /// default: cpu1_pingpong repeats `ra8_add_cpu1_image()`'s five-directory
    /// path because its `shared_pingpong.h` names the board's dual-core
    /// memory map, and cpu1_pingpong_ipc names four and stops before it.
    /// Assuming the board arm puts a board header within reach of a
    /// Cortex-M33 translation unit that CMake keeps it out of, and only
    /// freestanding-clean headers may be reached from one.
    board_include_dir: bool = true,
    /// CPU1 middleware this image links, by name (see `cpu1_threadx.find`).
    /// Each one adds its public defines and include paths to every CPU1 TU
    /// and its archive and link options to the M33 link. Empty for every
    /// image that predates RA8FW-403, which therefore builds as before.
    uses: []const []const u8 = &.{},
    /// What `entry_source` is written in. A `.zig` entry is its own Zig
    /// object built for the M33 (see `zigEntry`), linked ahead of the gcc
    /// objects, and never one of `sources()`: gcc does not compile it and
    /// the compile database has no row for it.
    entry_language: EntryLanguage = .c,
    /// The ThreadX module the image carries, by name (see
    /// `cpu1_txm_hello.find`), packed into `.txm_module` for its Module
    /// Manager to load in place. The app's own CPU1 linker script places it
    /// (RA8FW-431). Null for every image without a Module Manager.
    txm_module: ?[]const u8 = null,
    /// Whether the Zig entry imports `ra8_rpc` and `ra8_rpc_tx`, for an
    /// image that serves calls from its module (RA8FW-544).
    rpc: bool = false,
};

pub const EntryLanguage = enum { c, zig };

/// The Zig target every `.zig` CPU1 entry is built for. Same triple and CPU
/// model as ra8_core's cortex_m33 archive, so both halves of the M33 link
/// agree on the float ABI (fpv5-sp-d16, hard).
pub const zig_target_query = std.Target.Query{
    .cpu_arch = .thumb,
    .os_tag = .freestanding,
    .abi = .eabihf,
    .cpu_model = .{ .explicit = &std.Target.arm.cpu.cortex_m33 },
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
///
/// This is the whole define set even when the app around it is a TrustZone
/// build: `-DRA8_TRUSTZONE_ENABLE` and `-mcmse` ride on the `ra8_add_app()`
/// target and this executable is declared by hand, so they never reach the
/// M33 half. Measured on cpu1_pingpong_ipc, which is both;
/// `RA8_FREESTANDING` does reach it because the toolchain file adds that one
/// at DIRECTORY scope.
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
    if (image.entry_language == .c) {
        out.append(join(allocator, app.dir, image.entry_source)) catch @panic("OOM");
    }
    out.appendSlice(image.shared_sources) catch @panic("OOM");
    return out.toOwnedSlice() catch @panic("OOM");
}

fn join(allocator: std.mem.Allocator, left: []const u8, right: []const u8) []const u8 {
    return std.fs.path.join(allocator, &.{ left, right }) catch @panic("OOM");
}

/// The narrow include path the M33 target repeats from `ra8_add_cpu1_image()`:
/// the app, core, HAL, and, when the image asks for it, the board directory
/// that carries the dual-core memory map. Deliberately NOT the M85 app's path
/// -- only freestanding-clean headers may be reached from a Cortex-M33 TU,
/// and the difference between the two paths is the rule. The board arm is the
/// one directory the two dual-core apps disagree about, so it is data on the
/// image rather than a constant here.
/// The compile flags plus the public defines of every middleware the image
/// uses, which ride after the CPU1 flags exactly as an M85 app's do.
pub fn unitFlags(allocator: std.mem.Allocator, image: Cpu1Image, global_flags: []const []const u8) []const []const u8 {
    var out = std.ArrayList([]const u8).init(allocator);
    out.appendSlice(compileFlags(allocator, global_flags)) catch @panic("OOM");
    out.appendSlice(middleware.appDefines(allocator, cpu1_threadx.resolve(allocator, image.uses))) catch @panic("OOM");
    return out.toOwnedSlice() catch @panic("OOM");
}

/// The image's own include path, then its middleware's public directories.
pub fn unitIncludeDirs(allocator: std.mem.Allocator, app: App, image: Cpu1Image) []const []const u8 {
    var out = std.ArrayList([]const u8).init(allocator);
    out.appendSlice(includeDirs(allocator, app, image)) catch @panic("OOM");
    out.appendSlice(middleware.appIncludeDirs(allocator, cpu1_threadx.resolve(allocator, image.uses))) catch @panic("OOM");
    return out.toOwnedSlice() catch @panic("OOM");
}

/// The middleware's `-isystem` directories, after every `-I`.
pub fn systemIncludeDirs(allocator: std.mem.Allocator, image: Cpu1Image) []const []const u8 {
    return middleware.appSystemIncludeDirs(allocator, cpu1_threadx.resolve(allocator, image.uses));
}

pub fn includeDirs(allocator: std.mem.Allocator, app: App, image: Cpu1Image) []const []const u8 {
    var out = std.ArrayList([]const u8).init(allocator);
    out.append(join(allocator, app.dir, "inc")) catch @panic("OOM");
    out.append(join(allocator, app.dir, "src")) catch @panic("OOM");
    out.append("libs/ra8_core/inc") catch @panic("OOM");
    out.append("libs/ra8_hal/inc") catch @panic("OOM");
    if (image.board_include_dir) {
        out.append(join(allocator, app.board, "inc")) catch @panic("OOM");
    }
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

    /// ra8_core's archive built for THIS image's core, not the app's.
    /// cmake/ra8_add_app.cmake asks for cortex_m33 here while the main
    /// expansion registers the M85 one, because the compiler emits calls to
    /// memcpy / memset from ordinary struct assignment and those names have to
    /// be present in this image's own instruction set.
    /// Null on the compile-database path, which has no link; the link itself
    /// refuses to run without it, the same way the NS image treats the
    /// kernel's archive.
    core_archive: ?std.Build.LazyPath = null,
    /// The archives of `image.uses`, from `cpu1_threadx.archives`, in order.
    /// Empty on the compile-database path.
    middleware_archives: []const std.Build.LazyPath = &.{},
    /// The optimize mode a `.zig` entry is built at: the configuration's
    /// `zig_optimize`, the same one ra8_core's archive takes.
    zig_optimize: std.builtin.OptimizeMode = .ReleaseSmall,
    /// Prebuilt objects linked after the image's own, such as the packed
    /// module of `image.txm_module`.
    extra_objects: []const std.Build.LazyPath = &.{},
};

/// Build the M33 image, install its `.elf` / `.hex` / `.bin` / `.map` beside
/// the M85 artifacts, and hand back the relocatable `.cpu1_image` object for
/// the caller to link into the M85 ELF.
/// App dir if the script is there, else the shared M33 map in the board layer.
///
/// `ra8_add_cpu1_image()` has made exactly this choice since RA8FW-309, which
/// dropped eight byte-identical `linker_script_cpu1.ld` forks onto one source
/// (fcb624f8 carried the last two over to this branch). The graph resolved the
/// name against the app directory alone, so those apps asked for a file that
/// is no longer in the tree and the link failed before it started. Only an app
/// whose M33 image genuinely diverges keeps its own copy, and it still wins.
///
/// The fallback is the app's own board layer, `<board>/ld/<name>`
/// (RA8FW-496). EK-RA8D2 apps get the same file as before, and an RA8P1 app
/// gets `libs/ra8_board_ra8p1/ld/linker_script_cpu1.ld`, which keeps the
/// EK-RA8D2 windows.
pub fn linkerScript(b: *std.Build, app: App, name: []const u8) []const u8 {
    const in_app = b.pathJoin(&.{ app.dir, name });
    b.build_root.handle.access(in_app, .{}) catch {
        return boardLinkerScript(b.allocator, app.board, name);
    };
    return in_app;
}

/// The board layer's shared copy of an M33 linker script.
pub fn boardLinkerScript(allocator: std.mem.Allocator, board: []const u8, name: []const u8) []const u8 {
    return std.fs.path.join(allocator, &.{ board, "ld", name }) catch @panic("OOM");
}

pub fn add(b: *std.Build, step: *std.Build.Step, options: Options) std.Build.LazyPath {
    const name = imageName(b.allocator, options.app);
    const flags = unitFlags(b.allocator, options.image, options.global_compile_flags);
    const include_dirs = unitIncludeDirs(b.allocator, options.app, options.image);
    const system_dirs = systemIncludeDirs(b.allocator, options.image);

    var objects = std.ArrayList(std.Build.LazyPath).init(b.allocator);
    if (options.image.entry_language == .zig) {
        objects.append(zigEntry(b, options, name)) catch @panic("OOM");
    }
    for (sources(b.allocator, options.app, options.image)) |source| {
        const compile = b.addSystemCommand(&.{options.gcc});
        compile.addArgs(flags);
        for (include_dirs) |include_dir| {
            compile.addPrefixedDirectoryArg("-I", pkg_path.lazy(b, include_dir));
        }
        for (system_dirs) |include_dir| {
            compile.addArg("-isystem");
            compile.addDirectoryArg(pkg_path.lazy(b, include_dir));
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
    link.addPrefixedFileArg("-T", b.path(linkerScript(b, options.app, options.image.linker_script)));
    const map = link.addPrefixedOutputFileArg("-Wl,--Map=", b.fmt("{s}.map", .{name}));
    link.addArg("-o");
    const elf = link.addOutputFileArg(b.fmt("{s}.elf", .{name}));
    for (objects.items) |object| link.addFileArg(object);
    for (options.extra_objects) |object| link.addFileArg(object);
    for (options.middleware_archives) |archive| link.addFileArg(archive);
    link.addArgs(middleware.appLinkOptions(b.allocator, cpu1_threadx.resolve(b.allocator, options.image.uses)));
    link.addFileArg(options.core_archive orelse @panic("ra8: the CPU1 link needs ra8_core's archive built for cortex_m33"));

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

/// The `.zig` entry as one relocatable object for the M33.
fn zigEntry(b: *std.Build, options: Options, name: []const u8) std.Build.LazyPath {
    const root = zigModule(b, options, join(b.allocator, options.app.dir, options.image.entry_source));
    if (cpu1_threadx.wantsGlue(options.image.uses)) {
        const glue = zigModule(b, options, cpu1_threadx.zig_glue);
        const handlers = cpu1_threadx.handlersFor(options.image.uses);
        glue.addImport(cpu1_threadx.handlers_import, zigModule(b, options, handlers));
        root.addImport(cpu1_threadx.zig_glue_import, glue);
    }
    if (options.image.rpc) {
        const rpc = zigModule(b, options, "libs/ra8_rpc/src/ra8_rpc.zig");
        const rpc_tx = zigModule(b, options, "libs/ra8_rpc_tx/src/ra8_rpc_tx.zig");
        rpc_tx.addImport("ra8_rpc", rpc);
        root.addImport("ra8_rpc", rpc);
        root.addImport("ra8_rpc_tx", rpc_tx);
    }
    const object = b.addObject(.{ .name = b.fmt("{s}_entry", .{name}), .root_module = root });
    // The RPC stack copies messages, and Zig lowers those copies to
    // `__aeabi_memcpy` and `__aeabi_memclr`, which this -nostdlib link has
    // nowhere else to find.
    if (options.image.rpc) object.bundle_compiler_rt = true;
    return object.getEmittedBin();
}

fn zigModule(b: *std.Build, options: Options, path: []const u8) *std.Build.Module {
    return b.createModule(.{
        .root_source_file = b.path(path),
        .target = b.resolveTargetQuery(zig_target_query),
        .optimize = options.zig_optimize,
        // ra8_core's archive does the same: an .ARM.exidx entry names
        // __aeabi_unwind_cpp_pr0, which this -nostdlib link cannot resolve.
        .unwind_tables = .none,
    });
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
    const flags = unitFlags(b.allocator, options.image, options.global_compile_flags);
    const include_dirs = unitIncludeDirs(b.allocator, options.app, options.image);
    const system_dirs = systemIncludeDirs(b.allocator, options.image);
    for (sources(b.allocator, options.app, options.image)) |source| {
        out.append(.{
            .file = source,
            .driver = driver,
            .flags = flags,
            .include_dirs = include_dirs,
            .system_include_dirs = system_dirs,
            .object = b.fmt("arm/{s}/{s}.o", .{ name, std.fs.path.basename(source) }),
        }) catch @panic("OOM");
    }
}
