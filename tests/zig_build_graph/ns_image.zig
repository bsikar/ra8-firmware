//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The Non-Secure image of a two-project TrustZone build (#1111, part of #857).
//!
//! #1096 taught the graph the SECURE half of `tz_nsc_cgc_usb`: 192 TUs, the
//! `NSC_SRCS` veneer subset, `-mcmse`, and the CMSE import library the link
//! emits. That is one of the app's two images. The other one is a SEPARATE
//! executable the app's own CMakeLists declares with a raw `add_executable`,
//! linked against that import library at the Non-Secure addresses, and the two
//! are merged into one flashable hex.
//!
//! Five rules live here that no `ra8_add_app()` app can show, four of them
//! silent:
//!
//! 1. A raw `add_executable` target gets the toolchain's global flags and
//!    whatever its own CMakeLists asks for, in ITS declaration order. Here
//!    that order is the reverse of `ra8_add_app()`'s: `-fshort-enums`
//!    `-ffreestanding` land BEFORE the warning profile, not after it (#1090
//!    fixed the other direction). Compilers do not care; a compile-database
//!    row diffed against a real configure does.
//! 2. The middleware is a VARIANT, not the one the secure side uses:
//!    `cmake/threadx_ns.cmake` adds `RA8_THREADX_NON_SECURE`, which flips
//!    `port/threadx/inc/tx_user.h` from `TX_SINGLE_MODE_SECURE` to
//!    `TX_SINGLE_MODE_NON_SECURE`. Same 185 kernel sources, a different
//!    kernel, and not one diagnostic if the define goes missing.
//! 3. First-party TUs the secure side also compiles are recompiled NS-private
//!    (`ra8_log.c`, `ra8_scb.c`, `ra8_mstp.c`, the `ra8_usb*` driver). A
//!    single shared copy links fine and faults INVTRAN across the S/NS
//!    boundary at run time.
//! 4. A second vendored tree (USBX) with suppressions measured per set: the
//!    172 core and 15 CDC-ACM TUs take `-Wno-discarded-qualifiers`
//!    `-Wno-cast-align`; the 8 first-party `ux_dcd_ra8_usb*.c` bridge TUs
//!    beside them keep the full `-Werror` bar. A directory-wide `-w` would
//!    quietly cover the bridge too.
//! 5. The link takes the Secure image's import library as an ordinary input
//!    and carries NO `-lgcc` at all, because the NS target names no link
//!    libraries but `threadx_ns`. That is why the archive carries the three
//!    `ra8_freestanding_*` shims: they are this image's libc.
//!
//! Every field below was measured from a real standalone configure's own
//! `compile_commands.json` and `build.ninja`, not read off the listfiles.

const std = @import("std");
const middleware_mod = @import("middleware.zig");

/// One vendored tree the NS image compiles into itself, as the app's own
/// `file(GLOB)` selects it.
pub const VendoredSet = struct {
    dir: []const u8,
    /// Basename prefix the glob selects. Empty means every `*.c` in the
    /// directory. `ux_device_class_cdc_acm_` is a prefix rather than the whole
    /// directory on purpose: the tree holds 225 class drivers and this image
    /// wants 15 of them.
    prefix: []const u8 = "",
    /// Basename prefixes the app's `list(FILTER ... EXCLUDE REGEX)` removes.
    /// The two device/host simulators are not built for hardware.
    excluded_prefixes: []const []const u8 = &.{},
    /// Warning suppressions this set alone carries, via
    /// `set_source_files_properties(... COMPILE_OPTIONS ...)`. They land LAST
    /// on the compile line, after the target's own options.
    suppressions: []const []const u8 = &.{},
};

/// The Non-Secure executable of a two-project TrustZone app.
pub const NsImage = struct {
    /// CMake's target name without the `.elf`, which is also the stem of every
    /// artifact it writes.
    name: []const u8,
    /// The app's own sources, spelled relative to the app directory exactly as
    /// `add_executable` spells them. These are the same three files the SECURE
    /// target lists in `AUX_SRCS` to keep OUT of its image (#1036): one file
    /// set, two images, and the only thing that decides which is which is
    /// these two lists agreeing.
    app_sources: []const []const u8,
    /// First-party TUs compiled FRESH into this image, repo-relative and named
    /// one by one. Never globbed: a glob of `libs/ra8_hal/src` would build a
    /// second 200-TU image.
    private_sources: []const []const u8,
    vendored: []const VendoredSet,
    /// `target_compile_definitions(... PRIVATE ...)` on this target.
    defines: []const []const u8,
    /// Include directories relative to the APP directory, in call order.
    app_include_dirs: []const []const u8,
    /// Repo-relative include directories, in call order across the target's
    /// `target_include_directories` calls.
    include_dirs: []const []const u8,
    /// The target's own SYSTEM include directories (`-isystem`). The vendored
    /// USBX headers are not clean under the first-party profile, and `-isystem`
    /// is what keeps their diagnostics from failing the first-party TUs that
    /// include them.
    system_include_dirs: []const []const u8,
    /// The middleware this image links, by name. `threadx_ns`, not `threadx`.
    uses: []const u8,
    /// `ra8_target_enable_project_warnings(... STACK_USAGE_BYTES <n>)`: a raw
    /// target does not get the profile for free, and the app's CMakeLists says
    /// so in as many words. Without that call the NS image would compile with
    /// no `-Wall`/`-Werror` at all.
    stack_bytes: u32 = 2200,
    /// The NS-image linker script, app-relative. It places this image at the
    /// Non-Secure addresses; the Secure image's own script is a different file.
    linker_script: []const u8,
    /// Link options the target adds ahead of the script. `-nostartfiles`
    /// because the Secure boot copies `.data` and `ns_reset_handler` zeroes
    /// `.bss`, so there is no C runtime startup to link.
    link_flags: []const []const u8,
};

/// `-fshort-enums -ffreestanding`, in the order the NS target declares them
/// and in the position CMake puts them: before the warning profile, because
/// `target_compile_options()` runs before
/// `ra8_target_enable_project_warnings()` in the app's CMakeLists. The same
/// two flags sit AFTER the profile on every `ra8_add_app()` target, which is
/// the whole reason this is a separate list rather than a shared constant.
pub const target_dialect_flags = [_][]const u8{ "-fshort-enums", "-ffreestanding" };

/// One translation unit of the NS image and the suppressions its own set
/// carries.
pub const Unit = struct {
    path: []const u8,
    suppressions: []const []const u8 = &.{},
};

fn lessThanPath(_: void, a: Unit, b: Unit) bool {
    return std.mem.lessThan(u8, a.path, b.path);
}

fn isExcluded(set: VendoredSet, basename: []const u8) bool {
    for (set.excluded_prefixes) |prefix| {
        if (std.mem.startsWith(u8, basename, prefix)) return true;
    }
    return false;
}

/// True when the glob for `set` selects `basename`: the prefix matches and no
/// exclusion does. A pure predicate so both arms of both rules are assertable
/// without a 400-TU cross-build.
pub fn vendoredSelects(set: VendoredSet, basename: []const u8) bool {
    if (!std.mem.endsWith(u8, basename, ".c")) return false;
    if (set.prefix.len != 0 and !std.mem.startsWith(u8, basename, set.prefix)) return false;
    return !isExcluded(set, basename);
}

fn collectVendored(b: *std.Build, set: VendoredSet, out: *std.ArrayList(Unit)) void {
    var dir = b.build_root.handle.openDir(set.dir, .{ .iterate = true }) catch |err| {
        std.debug.panic("ra8: cannot read vendored directory '{s}': {s}", .{ set.dir, @errorName(err) });
    };
    defer dir.close();

    const first = out.items.len;
    var it = dir.iterate();
    while (it.next() catch |err| {
        std.debug.panic("ra8: cannot walk '{s}': {s}", .{ set.dir, @errorName(err) });
    }) |entry| {
        if (entry.kind != .file) continue;
        if (!vendoredSelects(set, entry.name)) continue;
        out.append(.{
            .path = b.fmt("{s}/{s}", .{ set.dir, b.dupe(entry.name) }),
            .suppressions = set.suppressions,
        }) catch @panic("OOM");
    }
    std.mem.sort(Unit, out.items[first..], {}, lessThanPath);
}

/// Every translation unit of the NS image, in CMake's own order: the app's own
/// sources from `add_executable`, then each vendored set as `target_sources`
/// appends it, then the named first-party recompiles.
pub fn units(b: *std.Build, app_dir: []const u8, image: NsImage) []const Unit {
    var out = std.ArrayList(Unit).init(b.allocator);
    for (image.app_sources) |source| {
        out.append(.{ .path = b.pathJoin(&.{ app_dir, source }) }) catch @panic("OOM");
    }
    for (image.vendored) |set| collectVendored(b, set, &out);
    for (image.private_sources) |source| out.append(.{ .path = source }) catch @panic("OOM");
    return out.items;
}

fn lessThanString(_: void, a: []const u8, b: []const u8) bool {
    return std.mem.lessThan(u8, a, b);
}

/// The `-D` flags this image's TUs really carry: the toolchain's
/// directory-scope defines, the target's own PRIVATE ones, and the middleware's
/// PUBLIC ones, deduplicated and SORTED.
///
/// Sorted because CMake's generator keeps a target's compile definitions in a
/// set, so it emits them in lexicographic order rather than declaration order.
/// Measured, not assumed: this target declares `RA8_TRUSTZONE_ENABLE`
/// `RA8_PERIPH_NS_ALIAS` `RA8_USB_POLLED_ONLY` in that order and the real
/// command line carries `-DRA8_PERIPH_NS_ALIAS -DRA8_TRUSTZONE_ENABLE
/// -DRA8_USB_POLLED_ONLY`. A graph that emitted declaration order would write
/// compile-database rows no consumer could diff against a real configure.
pub fn defines(
    allocator: std.mem.Allocator,
    image: NsImage,
    mw: middleware_mod.Middleware,
    global_defines: []const []const u8,
) []const []const u8 {
    var out = std.ArrayList([]const u8).init(allocator);
    out.appendSlice(global_defines) catch @panic("OOM");
    out.appendSlice(image.defines) catch @panic("OOM");
    out.appendSlice(mw.public_defines) catch @panic("OOM");
    std.mem.sort([]const u8, out.items, {}, lessThanString);

    var deduped = std.ArrayList([]const u8).init(allocator);
    for (out.items) |define| {
        if (deduped.items.len != 0 and std.mem.eql(u8, deduped.items[deduped.items.len - 1], define)) continue;
        deduped.append(define) catch @panic("OOM");
    }
    return deduped.items;
}

/// The `-I` path in compiler order: the app's own directories, the
/// repo-relative ones the target names, then the middleware's PUBLIC ones,
/// which CMake appends last because they arrive through the link interface.
pub fn includeDirs(
    b: *std.Build,
    app_dir: []const u8,
    image: NsImage,
    mw: middleware_mod.Middleware,
) []const []const u8 {
    var out = std.ArrayList([]const u8).init(b.allocator);
    for (image.app_include_dirs) |dir_path| {
        out.append(b.pathJoin(&.{ app_dir, dir_path })) catch @panic("OOM");
    }
    out.appendSlice(image.include_dirs) catch @panic("OOM");
    out.appendSlice(mw.public_include_dirs) catch @panic("OOM");
    return out.items;
}

/// The `-isystem` path, after every `-I`: the target's own vendored headers
/// first, the middleware's PUBLIC SYSTEM ones last.
pub fn systemIncludeDirs(
    allocator: std.mem.Allocator,
    image: NsImage,
    mw: middleware_mod.Middleware,
) []const []const u8 {
    var out = std.ArrayList([]const u8).init(allocator);
    out.appendSlice(image.system_include_dirs) catch @panic("OOM");
    out.appendSlice(mw.public_system_include_dirs) catch @panic("OOM");
    return out.items;
}

/// The flag list one unit is given, in the order the compiler sees it: the
/// global C flags, this target's dialect options, the first-party warning
/// profile at this image's frame budget, then whatever suppressions the unit's
/// own vendored set carries.
pub fn compileFlags(
    allocator: std.mem.Allocator,
    global_flags: []const []const u8,
    warning_flags: []const []const u8,
    unit: Unit,
) []const []const u8 {
    var out = std.ArrayList([]const u8).init(allocator);
    out.appendSlice(global_flags) catch @panic("OOM");
    out.appendSlice(&target_dialect_flags) catch @panic("OOM");
    out.appendSlice(warning_flags) catch @panic("OOM");
    out.appendSlice(unit.suppressions) catch @panic("OOM");
    return out.items;
}

/// Everything the NS link and its post-build need that the graph owns
/// elsewhere: the cross tools, the app it belongs to, the middleware archive
/// it links, and the Secure half's ELF and import library.
pub const Context = struct {
    gcc: []const u8,
    objcopy: []const u8,
    size: []const u8,
    app_name: []const u8,
    app_dir: []const u8,
    image: NsImage,
    middleware: middleware_mod.Middleware,
    /// The three link inputs only the `arm` step has. They are optional
    /// because `zig build compile-db` builds the same Context to describe this
    /// image's compile commands, and a database row has no link.
    middleware_archive: ?std.Build.LazyPath = null,
    /// The import library the Secure link emitted (`--out-implib`). An input
    /// of this link, which is why the Secure link declares it as an output
    /// rather than writing it somewhere by convention.
    implib: ?std.Build.LazyPath = null,
    /// The Secure ELF, needed by the hex merge rather than by the link.
    secure_elf: ?std.Build.LazyPath = null,
    /// The defines cmake/toolchain-ra8d2.cmake adds at DIRECTORY scope, which
    /// reach this raw target as surely as they reach an ra8_add_app() one.
    global_defines: []const []const u8 = &.{},
    global_compile_flags: []const []const u8,
    warning_flags: []const []const u8,
    global_link_flags: []const []const u8,
    /// The repo's own ihex merger, the same script CMake's POST_BUILD runs.
    merge_script: []const u8,
};

fn objcopyTo(
    b: *std.Build,
    ctx: Context,
    format: []const u8,
    extra: []const []const u8,
    elf: std.Build.LazyPath,
    output_name: []const u8,
) std.Build.LazyPath {
    const run = b.addSystemCommand(&.{ctx.objcopy});
    run.addArgs(extra);
    run.addArgs(&.{ "-O", format });
    run.addFileArg(elf);
    return run.addOutputFileArg(output_name);
}

/// Compile, link and merge the Non-Secure image, and install what CMake's own
/// build writes: the NS ELF, bin and map, the two intermediate hex files, and
/// the merged hex that overwrites the app's.
pub fn add(b: *std.Build, arm_step: *std.Build.Step, ctx: Context) void {
    const image = ctx.image;
    const include_dirs = includeDirs(b, ctx.app_dir, image, ctx.middleware);
    const system_dirs = systemIncludeDirs(b.allocator, image, ctx.middleware);
    const define_flags = defines(b.allocator, image, ctx.middleware, ctx.global_defines);

    var objects = std.ArrayList(std.Build.LazyPath).init(b.allocator);
    for (units(b, ctx.app_dir, image)) |unit| {
        const compile = b.addSystemCommand(&.{ctx.gcc});
        compile.addArgs(define_flags);
        compile.addArgs(compileFlags(b.allocator, ctx.global_compile_flags, ctx.warning_flags, unit));
        for (include_dirs) |dir_path| compile.addPrefixedDirectoryArg("-I", b.path(dir_path));
        for (system_dirs) |dir_path| {
            compile.addArg("-isystem");
            compile.addDirectoryArg(b.path(dir_path));
        }
        compile.addArg("-c");
        compile.addFileArg(b.path(unit.path));
        compile.addArg("-o");
        const object_name = b.fmt("{s}.o", .{std.fs.path.basename(unit.path)});
        objects.append(compile.addOutputFileArg(object_name)) catch @panic("OOM");
    }

    const link = b.addSystemCommand(&.{ctx.gcc});
    link.addArgs(ctx.global_link_flags);
    link.addArgs(image.link_flags);
    link.addPrefixedFileArg("-T", b.path(b.pathJoin(&.{ ctx.app_dir, image.linker_script })));
    const map = link.addPrefixedOutputFileArg("-Wl,--Map=", b.fmt("{s}.map", .{image.name}));
    // The import library sits here, as a positional input between the map and
    // the middleware's --undefined options: CMake carries it in LINK_FLAGS,
    // not in LINK_LIBRARIES.
    link.addFileArg(ctx.implib orelse @panic("ra8: the NS link needs the secure link's CMSE import library"));
    link.addArgs(middleware_mod.appLinkOptions(b.allocator, &.{ctx.middleware}));
    link.addArg("-o");
    const elf = link.addOutputFileArg(b.fmt("{s}.elf", .{image.name}));
    for (objects.items) |object| link.addFileArg(object);
    // The archive last and NO -lgcc: this target names threadx_ns as its only
    // link library, so the three ra8_freestanding_* shims inside that archive
    // are the whole of this image's libc.
    link.addFileArg(ctx.middleware_archive orelse @panic("ra8: the NS link needs the threadx_ns archive"));

    const bin = objcopyTo(b, ctx, "binary", &.{}, elf, b.fmt("{s}.bin", .{image.name}));
    const ns_hex = objcopyTo(b, ctx, "ihex", &.{}, elf, b.fmt("{s}.hex", .{image.name}));
    // The Secure side's option-setting records cannot be written by the MRAM
    // flasher, so they are stripped before the merge; the Secure ELF keeps
    // them.
    const secure_hex = objcopyTo(
        b,
        ctx,
        "ihex",
        &.{"--remove-section=.option_setting*"},
        ctx.secure_elf orelse @panic("ra8: the hex merge needs the secure ELF"),
        b.fmt("{s}_secure.hex", .{ctx.app_name}),
    );

    const merge = b.addSystemCommand(&.{ctx.merge_script});
    merge.addFileArg(secure_hex);
    merge.addFileArg(ns_hex);
    const merged = merge.addOutputFileArg(b.fmt("{s}.hex", .{ctx.app_name}));

    const installs = [_]struct { path: std.Build.LazyPath, name: []const u8 }{
        .{ .path = elf, .name = b.fmt("{s}.elf", .{image.name}) },
        .{ .path = bin, .name = b.fmt("{s}.bin", .{image.name}) },
        .{ .path = map, .name = b.fmt("{s}.map", .{image.name}) },
        .{ .path = ns_hex, .name = b.fmt("{s}.hex", .{image.name}) },
        .{ .path = secure_hex, .name = b.fmt("{s}_secure.hex", .{ctx.app_name}) },
        .{ .path = merged, .name = b.fmt("{s}.hex", .{ctx.app_name}) },
    };
    for (installs) |entry| {
        arm_step.dependOn(&b.addInstallFileWithDir(entry.path, .{ .custom = "arm" }, entry.name).step);
    }

    const report_size = b.addSystemCommand(&.{ctx.size});
    report_size.addFileArg(elf);
    arm_step.dependOn(&report_size.step);
}

/// The NS image's rows for `zig build compile-db`. Their own rows, not a
/// repeat of the app's: a different define set, a different include path, a
/// warning profile that arrives from a different call, and per-set
/// suppressions the Secure half does not have.
pub fn appendCompileDbEntries(
    b: *std.Build,
    comptime Entry: type,
    out: *std.ArrayList(Entry),
    ctx: Context,
) void {
    const image = ctx.image;
    const include_dirs = includeDirs(b, ctx.app_dir, image, ctx.middleware);
    const system_dirs = systemIncludeDirs(b.allocator, image, ctx.middleware);
    const define_flags = defines(b.allocator, image, ctx.middleware, ctx.global_defines);
    for (units(b, ctx.app_dir, image)) |unit| {
        var flags = std.ArrayList([]const u8).init(b.allocator);
        flags.appendSlice(define_flags) catch @panic("OOM");
        flags.appendSlice(compileFlags(b.allocator, ctx.global_compile_flags, ctx.warning_flags, unit)) catch @panic("OOM");
        out.append(.{
            .file = unit.path,
            .driver = ctx.gcc,
            .flags = flags.items,
            .include_dirs = include_dirs,
            .system_include_dirs = system_dirs,
            .object = b.fmt("arm/{s}/{s}.o", .{ image.name, std.fs.path.basename(unit.path) }),
        }) catch @panic("OOM");
    }
}
