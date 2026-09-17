//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Which translation units an example app compiles, and on what include path
//! (#936, widened by #1017).
//!
//! Extracted from build.zig because the root build file is at the 1000-line
//! ceiling scripts/checks/check_file_size.py holds every Zig source to, and
//! this is the coherent piece: the SOURCE-SET rules ra8_add_app() applies, as
//! opposed to the flags, the tool discovery and the step wiring that stay
//! beside the rest of the graph.
//!
//! Every rule in here is one a directory listing cannot tell you, which is why
//! it is data with tests rather than a glob: board boot files resolve per app,
//! two board units are opt-in on a library the app must name, one library name
//! has no directory of its own at all, and one app-local source under `src/`
//! belongs to a second image and must be kept OUT of this one (#1029).

const std = @import("std");
const cpu1_image = @import("cpu1_image.zig");

/// The app this slice cross-builds, spelled the way ra8_add_app() resolves it.
pub const CrossApp = struct {
    name: []const u8,
    dir: []const u8,
    board: []const u8,
    linker_script: []const u8,
    /// Everything the app names in `LIBS`, migrated or not. Read by the
    /// board opt-in gate below, which keys off the declared set rather than
    /// off what happens to be on disk.
    libraries: []const []const u8,
    zig_libraries: []const []const u8,
    /// Translation units under the app's own `src/` that belong to a DIFFERENT
    /// image and must stay out of this one, spelled relative to the app
    /// directory exactly as `AUX_SRCS` spells them. See aux_srcs below.
    aux_srcs: []const []const u8 = &.{},
    /// The second (Cortex-M33) image this app embeds in its own ELF, when it
    /// has one. Null for a single-core app, which is every app whose whole
    /// CMakeLists is one ra8_add_app() call. See cpu1_image.zig.
    cpu1: ?cpu1_image.Cpu1Image = null,
};

/// A name in `LIBS` that contributes translation units from somewhere other
/// than `libs/<name>/src`. `ra8_io_bus` is the only one today and it is the
/// sharp case: there is no `libs/ra8_io_bus` directory at all, so the LIBS
/// loop's glob for it is silently empty and a graph built from the directory
/// listing compiles NOTHING for it. cmake/ra8_app/sources.cmake instead
/// compiles just the SPI/I2C bus facades out of `libs/ra8_io` and puts
/// `libs/ra8_io/inc` on the include path -- the bus contracts without the rest
/// of the ra8_io fabric (no ra8_fs, no ra8_sdmmc_spi, no stream layer).
///
/// Encoded here for the same reason the board opt-in gate is: nothing on disk
/// says a library with no directory contributes sources, and getting it wrong
/// is a link failure at the end of a 200-TU cross-build rather than anything
/// that names the rule.
pub const LibraryAlias = struct {
    name: []const u8,
    /// Skipped when the app also names one of these: the fuller library
    /// already compiles the same translation units.
    superseded_by: []const []const u8,
    source_dir: []const u8,
    source_prefixes: []const []const u8,
    include_dir: []const u8,
};

pub const library_aliases = [_]LibraryAlias{
    .{
        .name = "ra8_io_bus",
        .superseded_by = &.{"ra8_io"},
        .source_dir = "libs/ra8_io/src",
        .source_prefixes = &.{ "ra8_io_spi_bus", "ra8_io_i2c_bus" },
        .include_dir = "libs/ra8_io/inc",
    },
};

/// Board translation units that are opt-in rather than universal, and the
/// library an app must name in `LIBS` to get them. The rule lives in
/// cmake/ra8_app/sources.cmake, NOT in the board directory: the board glob
/// compiles every BSP unit into every app, and these two are then filtered
/// back out because they reach outside the unconditional include set
/// (`..._console_stream.c` hands back an `ra8_io_stream_t` and needs the full
/// `ra8_io`; `..._touch.c` binds GT911 through the ra8_io I2C facade, so
/// either `ra8_io` or `ra8_io_bus` will do).
///
/// Encoded here because a directory listing cannot tell you about it: globbing
/// the board's src/ and stopping there compiles `..._console_stream.c` into an
/// app that never opted in, and it fails on a missing ra8_io_stream.h rather
/// than on anything that names the gate.
pub const BoardOptIn = struct {
    suffix: []const u8,
    satisfied_by: []const []const u8,
};

pub const board_opt_in_sources = [_]BoardOptIn{
    .{ .suffix = "_console_stream.c", .satisfied_by = &.{"ra8_io"} },
    .{ .suffix = "_touch.c", .satisfied_by = &.{ "ra8_io", "ra8_io_bus" } },
};

/// The universal first-party source set ra8_add_app() globs into every app,
/// plus the board layer this app selects. Each entry is globbed for `*.c`
/// non-recursively, exactly as the CMake `file(GLOB ...)` calls do.
const cross_source_dirs = [_][]const u8{
    "libs/ra8_core/src",
    "libs/ra8_hal/src",
    "libs/ra8_nsc/src",
    "libs/ra8_net_pal/src",
    "libs/ra8_usb_pal/src",
    "libs/ra8_secure_app/src",
    "libs/ra8_board_ek_ra8d2/src",
};

/// Boot translation units resolved per app: the app's own copy under `src/`
/// when it has one, otherwise the board layer's copy under `src/boot/`. This
/// is the per-app override rule in ra8_add_app(), and it is why
/// `libs/ra8_board_ek_ra8d2/src/boot` is NOT in cross_source_dirs above -- a
/// blind glob of that directory would link the board's vector table into an
/// app that ships its own.
const cross_boot_sources = [_][]const u8{
    "vector_table.c",
    "system_init.c",
    "secure_exception.c",
    "nmi_exception.c",
    "trustzone_init.c",
};

/// True when `basename` is one of the boot units resolved per app above. The
/// app-local glob below has to skip them, because the resolver already picked
/// the app copy or the board copy and adding the app copy a second time is a
/// duplicate object in the link.
fn isBootSource(basename: []const u8) bool {
    for (cross_boot_sources) |boot| {
        if (std.mem.eql(u8, boot, basename)) return true;
    }
    return false;
}

/// True when `relative_path` (relative to the app directory) is named in this
/// app's `AUX_SRCS`.
///
/// AUX_SRCS is the rule with the least presence on disk of any in this file:
/// the file sits under the app's own `src/`, looks exactly like every other
/// app-local helper, and compiles cleanly -- into the WRONG IMAGE. A dual-core
/// app keeps its Cortex-M33 entry point beside its M85 one (`src/cpu1_main.c`),
/// names it in AUX_SRCS, and ra8_add_app() then drops it from the primary
/// image's source list; a second executable elsewhere in the app's CMakeLists
/// compiles it for the M33. A graph that globs `<app>/src/*.c` and stops there
/// links two `main`-shaped entry points and two copies of the shared state into
/// the M85 image.
pub fn isAuxSource(app: CrossApp, relative_path: []const u8) bool {
    for (app.aux_srcs) |aux| {
        if (std.mem.eql(u8, aux, relative_path)) return true;
    }
    return false;
}

/// Whether a source the app-local glob turned up is compiled into this image
/// BY THAT GLOB. Three are not, each for its own reason, and the reasons are
/// why this is a function with tests rather than an inline condition:
///
///   main.c          already added, as the first object in the link.
///   a boot unit     already placed by the per-app boot resolver, which chose
///                   between this copy and the board's; taking it again is a
///                   duplicate object.
///   an AUX_SRCS     belongs to another image entirely (see isAuxSource).
///
/// `relative_path` is spelled relative to the app directory ("src/main.c"),
/// the way AUX_SRCS spells it in the app's CMakeLists.
pub fn appLocalIsCompiled(app: CrossApp, relative_path: []const u8) bool {
    const basename = std.fs.path.basename(relative_path);
    if (std.mem.eql(u8, basename, "main.c")) return false;
    if (isBootSource(basename)) return false;
    if (isAuxSource(app, relative_path)) return false;
    return true;
}

/// Which copy of one boot unit this app links: its own when it ships one,
/// otherwise the board layer's. Pure so both arms can be asserted directly;
/// the caller does the one filesystem probe and passes the answer in.
pub fn bootSourcePath(
    allocator: std.mem.Allocator,
    app: CrossApp,
    boot: []const u8,
    app_has_copy: bool,
) []const u8 {
    return if (app_has_copy)
        std.fmt.allocPrint(allocator, "{s}/src/{s}", .{ app.dir, boot }) catch @panic("OOM")
    else
        std.fmt.allocPrint(allocator, "{s}/src/boot/{s}", .{ app.board, boot }) catch @panic("OOM");
}

/// Include path, in the order ra8_add_app() adds it. Order is preserved
/// because a header shadowed by an earlier directory resolves differently, and
/// a parity claim that only holds for one ordering is not a parity claim.
const cross_include_dirs = [_][]const u8{
    "libs/ra8_core/inc",
    "libs/ra8_hal/inc",
    "libs/ra8_net_pal/inc",
    "libs/ra8_usb_pal/inc",
    "libs/ra8_nsc/inc",
    "libs/ra8_secure_app/inc",
    "libs/ra8_board_ek_ra8d2/inc",
};

/// Collect `*.c` from one directory, sorted, so the link order is stable
/// across machines and two builds of the same tree produce the same ELF.
pub fn collectCSources(b: *std.Build, dir_path: []const u8, out: *std.ArrayList([]const u8)) void {
    var dir = b.build_root.handle.openDir(dir_path, .{ .iterate = true }) catch |err| {
        std.debug.panic("ra8: cannot read source directory '{s}': {s}", .{ dir_path, @errorName(err) });
    };
    defer dir.close();

    var names = std.ArrayList([]const u8).init(b.allocator);
    var it = dir.iterate();
    while (it.next() catch |err| {
        std.debug.panic("ra8: cannot walk '{s}': {s}", .{ dir_path, @errorName(err) });
    }) |entry| {
        if (entry.kind != .file) continue;
        if (!std.mem.endsWith(u8, entry.name, ".c")) continue;
        names.append(b.dupe(entry.name)) catch @panic("OOM");
    }
    std.mem.sort([]const u8, names.items, {}, struct {
        fn lessThan(_: void, a: []const u8, c: []const u8) bool {
            return std.mem.lessThan(u8, a, c);
        }
    }.lessThan);
    for (names.items) |name| {
        out.append(b.fmt("{s}/{s}", .{ dir_path, name })) catch @panic("OOM");
    }
}

/// True when `app` names `library` in its `LIBS`.
pub fn declaresLibrary(app: CrossApp, library: []const u8) bool {
    for (app.libraries) |declared| {
        if (std.mem.eql(u8, declared, library)) return true;
    }
    return false;
}

/// Every C translation unit in the app's link, in ra8_add_app()'s own order:
/// the app's main.c, the resolved boot files, the globbed universal set, then
/// whatever the app's `LIBS` add on top.
pub fn crossSources(b: *std.Build, app: CrossApp) []const []const u8 {
    var sources = std.ArrayList([]const u8).init(b.allocator);

    sources.append(b.fmt("{s}/src/main.c", .{app.dir})) catch @panic("OOM");

    for (cross_boot_sources) |boot| {
        const app_copy = b.fmt("{s}/src/{s}", .{ app.dir, boot });
        const app_has_copy = if (b.build_root.handle.access(app_copy, .{})) |_| true else |_| false;
        sources.append(bootSourcePath(b.allocator, app, boot, app_has_copy)) catch @panic("OOM");
    }

    // Everything else the app keeps under `src/`: ra8_add_app() globs that
    // directory and compiles what is left after main.c (added above), the boot
    // units the resolver just placed, and the app's AUX_SRCS are taken out of
    // it. Globbing without those three subtractions is not a smaller parity
    // claim, it is a different image.
    var app_local = std.ArrayList([]const u8).init(b.allocator);
    collectCSources(b, b.fmt("{s}/src", .{app.dir}), &app_local);
    for (app_local.items) |source| {
        if (!appLocalIsCompiled(app, source[app.dir.len + 1 ..])) continue;
        sources.append(source) catch @panic("OOM");
    }

    for (cross_source_dirs) |dir_path| collectCSources(b, dir_path, &sources);

    // Named libraries. A library with a directory of its own contributes
    // `libs/<name>/src/*.c`; a board named in LIBS contributes the same set the
    // universal glob already took, minus src/boot (the per-app override above
    // decides those), so it lands as duplicates and is deduplicated below.
    for (app.libraries) |library| {
        const library_dir = b.fmt("libs/{s}/src", .{library});
        const exists = if (b.build_root.handle.access(library_dir, .{})) |_| true else |_| false;
        if (exists) collectCSources(b, library_dir, &sources);
    }
    for (library_aliases) |alias| {
        if (!declaresLibrary(app, alias.name)) continue;
        var superseded = false;
        for (alias.superseded_by) |fuller| {
            if (declaresLibrary(app, fuller)) superseded = true;
        }
        if (superseded) continue;
        var aliased = std.ArrayList([]const u8).init(b.allocator);
        collectCSources(b, alias.source_dir, &aliased);
        for (aliased.items) |source| {
            const name = std.fs.path.basename(source);
            for (alias.source_prefixes) |prefix| {
                if (std.mem.startsWith(u8, name, prefix)) {
                    sources.append(source) catch @panic("OOM");
                    break;
                }
            }
        }
    }

    // Drop the opt-in board units this app did not opt into (see
    // board_opt_in_sources), then the duplicates a named board produces.
    var kept = std.ArrayList([]const u8).init(b.allocator);
    var seen = std.StringHashMap(void).init(b.allocator);
    for (sources.items) |source| {
        if (isGatedOutBoardSource(app, source)) continue;
        if (seen.contains(source)) continue;
        seen.put(source, {}) catch @panic("OOM");
        kept.append(source) catch @panic("OOM");
    }
    return kept.items;
}

/// The app's include path, in the order ra8_add_app() adds it: the app's own
/// directories, the universal first-party set, then one directory per named
/// library that has headers.
pub fn crossIncludeDirs(b: *std.Build, app: CrossApp) []const []const u8 {
    var dirs = std.ArrayList([]const u8).init(b.allocator);
    // CMake adds `<app>/inc` unconditionally, and it is FIRST, ahead of the
    // app's own src/ and every library: an app-local header shadows a
    // same-named one further down the path. cpu1_pingpong is the app that
    // ships one (inc/shared_pingpong.h, the dual-core mailbox contract), so
    // this arm is exercised rather than assumed. A directory argument has to
    // exist to be declared as a step input, so it is skipped when absent.
    const app_inc = b.fmt("{s}/inc", .{app.dir});
    if (b.build_root.handle.access(app_inc, .{})) |_| {
        dirs.append(app_inc) catch @panic("OOM");
    } else |_| {}
    dirs.append(b.fmt("{s}/src", .{app.dir})) catch @panic("OOM");
    dirs.appendSlice(&cross_include_dirs) catch @panic("OOM");

    for (app.libraries) |library| {
        const library_inc = b.fmt("libs/{s}/inc", .{library});
        const exists = if (b.build_root.handle.access(library_inc, .{})) |_| true else |_| false;
        if (exists) dirs.append(library_inc) catch @panic("OOM");
    }
    for (library_aliases) |alias| {
        if (!declaresLibrary(app, alias.name)) continue;
        var superseded = false;
        for (alias.superseded_by) |fuller| {
            if (declaresLibrary(app, fuller)) superseded = true;
        }
        if (!superseded) dirs.append(alias.include_dir) catch @panic("OOM");
    }

    var kept = std.ArrayList([]const u8).init(b.allocator);
    var seen = std.StringHashMap(void).init(b.allocator);
    for (dirs.items) |dir_path| {
        if (seen.contains(dir_path)) continue;
        seen.put(dir_path, {}) catch @panic("OOM");
        kept.append(dir_path) catch @panic("OOM");
    }
    return kept.items;
}

/// True when `source` is a board unit whose companion library is absent from
/// the app's declared `LIBS`.
pub fn isGatedOutBoardSource(app: CrossApp, source: []const u8) bool {
    if (!std.mem.startsWith(u8, source, app.board)) return false;
    for (board_opt_in_sources) |gate| {
        if (!std.mem.endsWith(u8, source, gate.suffix)) continue;
        for (gate.satisfied_by) |required| {
            if (declaresLibrary(app, required)) return false;
        }
        return true;
    }
    return false;
}
