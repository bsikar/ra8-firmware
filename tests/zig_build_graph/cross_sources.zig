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
const app_local_mod = @import("app_local.zig");
const ns_image_mod = @import("ns_image.zig");
const off_target_mod = @import("off_target.zig");

/// The app this slice cross-builds, spelled the way ra8_add_app() resolves it.
pub const app_table = @import("app_table.zig");

/// One example app the graph cross-builds. The type and the table of apps
/// live in app_table.zig (#1146); they are re-exported here so every call
/// site, including build.zig, still reads them beside the rules they take an
/// arm of.
pub const CrossApp = app_table.CrossApp;
pub const cross_apps = app_table.cross_apps;

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

/// A translation unit a named library globs in that ra8_add_app() then drops
/// again unless the app ALSO names the library whose headers it reaches for.
/// `libs/ra8_io/src/ra8_io_blockdev_vsource.c` is the case today: a bare
/// `ra8_io` globs it in, but it includes ra8_vsource.h from ra8_mem, so
/// cmake/ra8_app/sources.cmake keeps it (and puts `libs/ra8_mem/inc` on the
/// include path) only for an app that declares `ra8_mem` as well.
///
/// Encoded for the same reason the board gate is: a directory-listing graph
/// compiles the unit for every `ra8_io` consumer, and it does not fail on the
/// missing header -- `libs/ra8_mem/inc` happens to be reachable through
/// nothing at all, so what you get is an extra object in an image CMake never
/// put it in. #1068 measured the drop arm on ra8_io_swap_demo (which names
/// `ra8_io` and not `ra8_mem`): 261 units against CMake's 260, this one
/// either-only. The KEEP arm is encoded from the listfile and is not yet
/// measured -- no app in the table names `ra8_mem`.
pub const LibrarySourceGate = struct {
    /// The gated unit, repo-relative.
    source: []const u8,
    /// The library the app must also name in `LIBS` to keep it.
    satisfied_by: []const u8,
    /// The include directory that same declaration adds.
    include_dir: []const u8,
};

pub const library_source_gates = [_]LibrarySourceGate{
    .{
        .source = "libs/ra8_io/src/ra8_io_blockdev_vsource.c",
        .satisfied_by = "ra8_mem",
        .include_dir = "libs/ra8_mem/inc",
    },
};

/// The OFF_TARGET_LIBS rule, aliased so call sites read the same as before the
/// extraction (#1133).
pub const off_target_define = off_target_mod.off_target_define;

/// True when `source` is one of this app's OFF_TARGET_LIBS units.
pub fn isOffTargetSource(app: CrossApp, source: []const u8) bool {
    return off_target_mod.isOffTargetSource(app.off_target_libs, source);
}

/// True when `source` is a library unit whose companion library the app does
/// not declare.
pub fn isGatedOutLibrarySource(app: CrossApp, source: []const u8) bool {
    for (library_source_gates) |gate| {
        if (!std.mem.eql(u8, gate.source, source)) continue;
        return !declaresLibrary(app, gate.satisfied_by);
    }
    return false;
}

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

// ===========================================================================
// The app table (#936, widened by #1021, #1036, #1044, #1054, #1068)
// ===========================================================================

/// The universal first-party source set ra8_add_app() globs into every app,
/// plus the board layer this app selects. Each entry is globbed for `*.c`
/// non-recursively, exactly as the CMake `file(GLOB ...)` calls do.
pub const cross_source_dirs = [_][]const u8{
    "libs/ra8_core/src",
    "libs/ra8_hal/src",
    "libs/ra8_nsc/src",
    "libs/ra8_net_pal/src",
    "libs/ra8_usb_pal/src",
    "libs/ra8_secure_app/src",
    // The board layer's own src/ is NOT listed here: it is `<app.board>/src`,
    // appended per app by crossSources() below. Every app cross-built before
    // #1131 is an ek_ra8d2 app, so the directory sat in this list looking like
    // a constant; `ra8_add_app(BOARD ra8p1)` resolves it to a different layer
    // entirely, and a hard-coded entry would compile the RA8D2 BSP into an
    // RA8P1 image and omit the RA8P1 one.
};

/// The one directory in cross_source_dirs an app can narrow. Named so the
/// glob loop and the NSC_SRCS rule cannot drift apart about which directory
/// the subset applies to.
pub const nsc_source_dir = "libs/ra8_nsc/src";

/// Whether one `libs/ra8_nsc/src` unit is compiled into this app, as the
/// three arms of a single decision: `NO_NSC` compiles none of them, `NSC_SRCS`
/// compiles exactly the named subset, and an app that says neither compiles
/// the whole directory. Pure, so test-zig asserts all three directly instead
/// of only through a 192-unit cross-build.
///
/// Takes a repo-relative path and answers only about that directory; any
/// other path is not this rule's business and comes back true.
pub fn nscIsCompiled(app: CrossApp, relpath: []const u8) bool {
    if (!std.mem.startsWith(u8, relpath, nsc_source_dir ++ "/")) return true;
    if (app.no_nsc) return false;
    if (app.nsc_srcs.len == 0) return true;
    const name = std.fs.path.basename(relpath);
    for (app.nsc_srcs) |named| {
        if (std.mem.eql(u8, named, name)) return true;
    }
    return false;
}

/// Whether `relpath` is an NSC translation unit this app does NOT compile.
/// Pure, so both arms assert directly in test-zig instead of only through a
/// 192-TU cross-build: an app that names no NSC_SRCS compiles all ten, and an
/// app that names a subset compiles exactly that subset.
pub fn isGatedOutNscSource(app: CrossApp, relpath: []const u8) bool {
    if (app.nsc_srcs.len == 0) return false;
    if (!std.mem.startsWith(u8, relpath, nsc_source_dir ++ "/")) return false;
    const name = std.fs.path.basename(relpath);
    for (app.nsc_srcs) |named| {
        if (std.mem.eql(u8, named, name)) return false;
    }
    return true;
}

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
pub const cross_include_dirs = [_][]const u8{
    "libs/ra8_core/inc",
    "libs/ra8_hal/inc",
    "libs/ra8_net_pal/inc",
    "libs/ra8_usb_pal/inc",
    "libs/ra8_nsc/inc",
    "libs/ra8_secure_app/inc",
    // `<app.board>/inc` closes this list, appended per app by
    // crossIncludeDirs() below rather than spelled here: see the note on
    // cross_source_dirs. It is LAST of the universal set and ahead of every
    // library directory, so a board header shadows a library's same-named one.
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

    // EXTRA_SRCS, appended in the order the app names them and BEFORE the
    // library globs, which is the order cmake/ra8_app/sources.cmake builds the
    // list in and therefore the order the objects reach the linker.
    for (app.extra_srcs) |source| sources.append(source) catch @panic("OOM");

    for (cross_source_dirs) |dir_path| {
        // NO_NSC drops the whole libs/ra8_nsc/src glob, ahead of NSC_SRCS and
        // for the same reason: cmake/ra8_app/sources.cmake decides that one
        // directory three ways, and neither the glob nor the subset is what
        // an app naming NO_NSC gets. Its include directory stays on the path
        // regardless -- see CrossApp.no_nsc, and nscIsCompiled below for both
        // decisions as one predicate.
        if (std.mem.eql(u8, dir_path, nsc_source_dir) and app.no_nsc) continue;
        // NSC_SRCS replaces the glob of libs/ra8_nsc/src with exactly the
        // files the app named, in the order it named them. See
        // CrossApp.nsc_srcs: this is a subtraction no listing shows, and the
        // build succeeds either way.
        if (std.mem.eql(u8, dir_path, nsc_source_dir) and app.nsc_srcs.len > 0) {
            for (app.nsc_srcs) |name| {
                sources.append(b.fmt("{s}/{s}", .{ nsc_source_dir, name })) catch @panic("OOM");
            }
            continue;
        }
        collectCSources(b, dir_path, &sources);
    }
    // The board layer this app selected, last of the universal set and
    // resolved per app rather than hard-coded (#1131).
    collectCSources(b, b.fmt("{s}/src", .{app.board}), &sources);

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

    // OFF_TARGET_LIBS, last of the whole list and at their own preprocessor
    // view. See off_target.zig.
    off_target_mod.appendSources(b, app.off_target_libs, &sources, collectCSources);

    // Drop the opt-in board units this app did not opt into (see
    // board_opt_in_sources), then the duplicates a named board produces.
    var kept = std.ArrayList([]const u8).init(b.allocator);
    var seen = std.StringHashMap(void).init(b.allocator);
    for (sources.items) |source| {
        if (isGatedOutBoardSource(app, source)) continue;
        if (isGatedOutLibrarySource(app, source)) continue;
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
    // CMake adds `<app>/inc` UNCONDITIONALLY, and it is FIRST, ahead of the
    // app's own src/ and every library: an app-local header shadows a
    // same-named one further down the path. cpu1_pingpong is the app that
    // ships one (inc/shared_pingpong.h, the dual-core mailbox contract).
    //
    // It is emitted for an app that ships no inc/ too, which is a real
    // difference and not a formality: this list is also what
    // `zig build compile-db` writes, and a database row whose include path is
    // one directory short of the compiler's is a row clang-tidy resolves
    // differently than the build did. #1068 measured it on ra8_io_swap_demo,
    // which ships no inc/; the step spells an absent directory as a plain -I
    // string, because only an existing directory can be declared as a step
    // input.
    dirs.append(b.fmt("{s}/inc", .{app.dir})) catch @panic("OOM");
    dirs.append(b.fmt("{s}/src", .{app.dir})) catch @panic("OOM");
    dirs.appendSlice(&cross_include_dirs) catch @panic("OOM");
    dirs.append(b.fmt("{s}/inc", .{app.board})) catch @panic("OOM");

    for (app.libraries) |library| {
        const library_inc = b.fmt("libs/{s}/inc", .{library});
        const exists = if (b.build_root.handle.access(library_inc, .{})) |_| true else |_| false;
        if (exists) dirs.append(library_inc) catch @panic("OOM");
    }
    // The include directory a gated library unit's companion brings with it,
    // added where cmake/ra8_app/sources.cmake adds it: after the per-library
    // directories, before the alias one.
    for (library_source_gates) |gate| {
        if (declaresLibrary(app, gate.satisfied_by)) {
            dirs.append(gate.include_dir) catch @panic("OOM");
        }
    }
    for (library_aliases) |alias| {
        if (!declaresLibrary(app, alias.name)) continue;
        var superseded = false;
        for (alias.superseded_by) |fuller| {
            if (declaresLibrary(app, fuller)) superseded = true;
        }
        if (!superseded) dirs.append(alias.include_dir) catch @panic("OOM");
    }

    // Every OFF_TARGET_LIBS entry's `inc`, which lands on EVERY unit in the
    // app and not only on the library's own. See off_target.zig.
    off_target_mod.appendIncludeDirs(b, app.off_target_libs, &dirs);

    // One directory per EXTRA_SRCS entry, deduplicated, added LAST of
    // everything ra8_add_app() puts on the path (cmake/ra8_add_app.cmake
    // spells `${_ra8_extra_inc}` after `${_ra8_lib_inc}` in the same
    // target_include_directories call).
    for (app.extra_srcs) |source| {
        dirs.append(std.fs.path.dirname(source) orelse ".") catch @panic("OOM");
    }
    // Then whatever the app's own CMakeLists adds, which lands after
    // ra8_add_app() has already run.
    dirs.appendSlice(app.local.include_dirs) catch @panic("OOM");

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
