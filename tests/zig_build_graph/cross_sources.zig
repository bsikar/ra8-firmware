//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Which translation units an example app compiles, and on what include path
//! (first proven on one app, then widened).
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
//! belongs to a second image and must be kept OUT of this one.

const std = @import("std");
const src_tree = @import("src_tree.zig");
const cpu1_image = @import("cpu1_image.zig");
const app_local_mod = @import("app_local.zig");
const ns_image_mod = @import("ns_image.zig");
const off_target_mod = @import("off_target.zig");

/// The app this slice cross-builds, spelled the way ra8_add_app() resolves it.
pub const app_table = @import("app_table.zig");

/// One example app the graph cross-builds. The type and the table of apps
/// live in app_table.zig; they are re-exported here so every call
/// site, including build.zig, still reads them beside the rules they take an
/// arm of.
pub const CrossApp = app_table.CrossApp;
pub const cross_apps = app_table.cross_apps;

/// A name in `LIBS` with no `libs/<name>` directory of its own. `ra8_io_bus`
/// is the only one today: it is the narrow way to reach the ra8_io SPI/I2C bus
/// facades without the rest of the ra8_io fabric (no ra8_fs, no ra8_sdmmc_spi,
/// no stream layer). Since RA8FW-714 those facades are all Zig, so the alias
/// contributes no C translation units: cmake/ra8_app/sources.cmake puts
/// `libs/ra8_io/inc` on the include path and links the ra8_io Zig archive.
///
/// Encoded here for the same reason the board opt-in gate is: nothing on disk
/// says a library with no directory contributes an include path and an
/// archive, and getting it wrong is a link failure at the end of a 200-TU
/// cross-build rather than anything that names the rule.
pub const LibraryAlias = struct {
    name: []const u8,
    /// Skipped when the app also names one of these: the fuller library
    /// already compiles the same translation units.
    superseded_by: []const []const u8,
    include_dir: []const u8,
    /// The Zig archive the alias links, because the units it stands for are
    /// Zig now (RA8FW-709 onward; all of them since RA8FW-714). Null when none.
    zig_archive: ?[]const u8,
};

pub const library_aliases = [_]LibraryAlias{
    .{
        .name = "ra8_io_bus",
        .superseded_by = &.{"ra8_io"},
        .include_dir = "libs/ra8_io/inc",
        .zig_archive = "ra8_io",
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
/// put it in. The stack-budget slice measured the drop arm on ra8_io_swap_demo (which names
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
/// extraction.
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
// The app table
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
    // the RA8P1 board landed is an ek_ra8d2 app, so the directory sat in this list looking like
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

/// Where one boot unit comes from (RA8FW-616): a C translation unit compiled
/// at the app's bar, or a board unit written in Zig and built as its OWN
/// object. Never the board archive: both vector tables define the handlers as
/// weak aliases of Default_Handler, so a handler has to arrive as a strong
/// definition in an object of its own (RA8FW-615), and an app-local C copy
/// still has to be able to replace it.
pub const BootUnit = union(enum) {
    c: []const u8,
    zig: []const u8,
};

/// The board layer's Zig spelling of one boot unit: `<board>/src/boot/<stem>.zig`.
/// What a Zig boot unit learns about the app it is linked into, as
/// `@import("boot_options")`. CMake's `_ra8_app_zig_boot_object` writes the
/// same fields from the app's RA8_TRUSTZONE_ENABLE (RA8FW-622).
pub const BootOptions = struct { trust_zone: bool };

pub fn bootOptions(app: CrossApp) BootOptions {
    return .{ .trust_zone = app.trust_zone };
}

pub fn bootZigPath(allocator: std.mem.Allocator, app: CrossApp, boot: []const u8) []const u8 {
    const stem = boot[0 .. boot.len - ".c".len];
    return std.fmt.allocPrint(allocator, "{s}/src/boot/{s}.zig", .{ app.board, stem }) catch @panic("OOM");
}

/// ra8_add_app()'s order: the app's own C copy, then the board's Zig unit,
/// then the board's C copy. Pure, like bootSourcePath; the caller probes.
pub fn resolveBootUnit(
    allocator: std.mem.Allocator,
    app: CrossApp,
    boot: []const u8,
    app_has_copy: bool,
    board_has_zig: bool,
) BootUnit {
    if (!app_has_copy and board_has_zig) return .{ .zig = bootZigPath(allocator, app, boot) };
    return .{ .c = bootSourcePath(allocator, app, boot, app_has_copy) };
}

fn bootPathExists(b: *std.Build, path: []const u8) bool {
    return src_tree.exists(b, path);
}

fn resolveBoot(b: *std.Build, app: CrossApp, boot: []const u8) BootUnit {
    const app_has_copy = bootPathExists(b, b.fmt("{s}/src/{s}", .{ app.dir, boot }));
    if (!app_has_copy) if (profileBootUnit(b, app, boot)) |unit| return unit;
    const board_has_zig = bootPathExists(b, bootZigPath(b.allocator, app, boot));
    return resolveBootUnit(b.allocator, app, boot, app_has_copy, board_has_zig);
}

/// The board boot units this app links as standalone Zig objects.
pub fn crossBootZigUnits(b: *std.Build, app: CrossApp) []const []const u8 {
    var units: std.ArrayList([]const u8) = .empty;
    for (cross_boot_sources) |boot| {
        switch (resolveBoot(b, app, boot)) {
            .zig => |path| units.append(b.allocator, path) catch @panic("OOM"),
            .c => {},
        }
    }
    return units.items;
}

/// The middle rung of the boot resolver: the board's `src/boot/<profile>/`
/// copy of one boot unit, or null when the app names no `BOOT_PROFILE`. A
/// profile directory holds only the units it overrides, so the caller probes
/// the returned path and falls through to the board default when it is absent.
pub fn profileBootPath(allocator: std.mem.Allocator, app: CrossApp, boot: []const u8) ?[]const u8 {
    const profile = app.boot_profile orelse return null;
    return std.fmt.allocPrint(allocator, "{s}/src/boot/{s}/{s}", .{ app.board, profile, boot }) catch @panic("OOM");
}

/// The profile rung as cmake/ra8_app/sources.cmake walks it: the profile's C
/// copy, else its Zig spelling (RA8FW-659), else null so the board default
/// applies.
fn profileBootUnit(b: *std.Build, app: CrossApp, boot: []const u8) ?BootUnit {
    const c_path = profileBootPath(b.allocator, app, boot) orelse return null;
    if (bootPathExists(b, c_path)) return .{ .c = c_path };
    const stem = c_path[0 .. c_path.len - ".c".len];
    const zig_path = b.fmt("{s}.zig", .{stem});
    if (bootPathExists(b, zig_path)) return .{ .zig = zig_path };
    return null;
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
    // The neutral fw_if_* port contracts. The board umbrella publishes the
    // board's clock profile, which is written against fw_if_clock.h, so every
    // app that speaks board coordinates needs this on the path.
    // cmake/ra8_add_app.cmake spells it here, after ra8_secure_app and ahead
    // of the board directory, and the order is load-bearing: a board header
    // shadows a same-named one from this set.
    "libs/if/inc",
    // `<app.board>/inc` closes this list, appended per app by
    // crossIncludeDirs() below rather than spelled here: see the note on
    // cross_source_dirs. It is LAST of the universal set and ahead of every
    // library directory, so a board header shadows a library's same-named one.
};

/// The chip adapter include directories every board app gets, in the order
/// cmake/ra8_app/board_adapters.cmake appends them: the board's clock profile
/// includes fw_if_clock_ra8.h and its GPT profile includes fw_if_pwm_ra8.h and
/// fw_if_timer_ra8.h.
pub const board_adapter_include_dirs = [_][]const u8{
    "libs/if_ra8_cgc/inc",
    "libs/if_ra8_gpt/inc",
};

/// Collect `*.c` from one directory, sorted, so the link order is stable
/// across machines and two builds of the same tree produce the same ELF.
pub fn collectCSources(b: *std.Build, dir_path: []const u8, out: *std.ArrayList([]const u8)) void {
    var dir = src_tree.openDir(b, dir_path) catch |err| {
        std.debug.panic("ra8: cannot read source directory '{s}': {s}", .{ dir_path, @errorName(err) });
    };
    defer dir.close(b.graph.io);

    var names: std.ArrayList([]const u8) = .empty;
    var it = dir.iterate();
    while (it.next(b.graph.io) catch |err| {
        std.debug.panic("ra8: cannot walk '{s}': {s}", .{ dir_path, @errorName(err) });
    }) |entry| {
        if (entry.kind != .file) continue;
        if (!std.mem.endsWith(u8, entry.name, ".c")) continue;
        names.append(b.allocator, b.dupe(entry.name)) catch @panic("OOM");
    }
    std.mem.sort([]const u8, names.items, {}, struct {
        fn lessThan(_: void, a: []const u8, c: []const u8) bool {
            return std.mem.lessThan(u8, a, c);
        }
    }.lessThan);
    for (names.items) |name| {
        out.append(b.allocator, b.fmt("{s}/{s}", .{ dir_path, name })) catch @panic("OOM");
    }
}

/// True when `app` names `library` in its `LIBS`.
pub fn declaresLibrary(app: CrossApp, library: []const u8) bool {
    for (app.libraries) |declared| {
        if (std.mem.eql(u8, declared, library)) return true;
    }
    return false;
}

/// The target an app's Zig code is built for: cortex-m85 with hard float,
/// what zig_libs.cmake derives from the toolchain's -mcpu and -mfloat-abi.
pub const zig_target_query = std.Target.Query{
    .cpu_arch = .thumb,
    .os_tag = .freestanding,
    .abi = .eabihf,
    .cpu_model = .{ .explicit = &std.Target.arm.cpu.cortex_m85 },
};

/// False for an app whose main is a Zig root (`zig_main`): it has no
/// `src/main.c`, and listing one would fail the compile on a missing file.
pub fn hasCMain(app: CrossApp) bool {
    return app.zig_main == null;
}

/// Every C translation unit in the app's link, in ra8_add_app()'s own order:
/// the app's main.c, the resolved boot files, the globbed universal set, then
/// whatever the app's `LIBS` add on top.
pub fn crossSources(b: *std.Build, app: CrossApp) []const []const u8 {
    var sources: std.ArrayList([]const u8) = .empty;

    if (hasCMain(app)) sources.append(b.allocator, b.fmt("{s}/src/main.c", .{app.dir})) catch @panic("OOM");

    for (cross_boot_sources) |boot| {
        switch (resolveBoot(b, app, boot)) {
            .c => |path| sources.append(b.allocator, path) catch @panic("OOM"),
            // Built as its own object in cross_image.zig, not a C unit.
            .zig => {},
        }
    }

    // Everything else the app keeps under `src/`: ra8_add_app() globs that
    // directory and compiles what is left after main.c (added above), the boot
    // units the resolver just placed, and the app's AUX_SRCS are taken out of
    // it. Globbing without those three subtractions is not a smaller parity
    // claim, it is a different image.
    var app_local: std.ArrayList([]const u8) = .empty;
    collectCSources(b, b.fmt("{s}/src", .{app.dir}), &app_local);
    for (app_local.items) |source| {
        if (!appLocalIsCompiled(app, source[app.dir.len + 1 ..])) continue;
        sources.append(b.allocator, source) catch @panic("OOM");
    }

    // EXTRA_SRCS, appended in the order the app names them and BEFORE the
    // library globs, which is the order cmake/ra8_app/sources.cmake builds the
    // list in and therefore the order the objects reach the linker.
    for (app.extra_srcs) |source| sources.append(b.allocator, source) catch @panic("OOM");

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
                sources.append(b.allocator, b.fmt("{s}/{s}", .{ nsc_source_dir, name })) catch @panic("OOM");
            }
            continue;
        }
        collectCSources(b, dir_path, &sources);
    }
    // The board layer this app selected, last of the universal set and
    // resolved per app rather than hard-coded.
    collectCSources(b, b.fmt("{s}/src", .{app.board}), &sources);

    // Named libraries. A library with a directory of its own contributes
    // `libs/<name>/src/*.c`; a board named in LIBS contributes the same set the
    // universal glob already took, minus src/boot (the per-app override above
    // decides those), so it lands as duplicates and is deduplicated below.
    for (app.libraries) |library| {
        const library_dir = b.fmt("libs/{s}/src", .{library});
        const exists = src_tree.exists(b, library_dir);
        if (exists) collectCSources(b, library_dir, &sources);
    }

    // OFF_TARGET_LIBS, last of the whole list and at their own preprocessor
    // view. See off_target.zig.
    off_target_mod.appendSources(b, app.off_target_libs, &sources, collectCSources);

    // Drop the opt-in board units this app did not opt into (see
    // board_opt_in_sources), then the duplicates a named board produces.
    var kept: std.ArrayList([]const u8) = .empty;
    var seen = std.StringHashMap(void).init(b.allocator);
    for (sources.items) |source| {
        if (isGatedOutBoardSource(app, source)) continue;
        if (isGatedOutLibrarySource(app, source)) continue;
        if (seen.contains(source)) continue;
        seen.put(source, {}) catch @panic("OOM");
        kept.append(b.allocator, source) catch @panic("OOM");
    }
    return kept.items;
}

/// The app's include path, in the order ra8_add_app() adds it: the app's own
/// directories, the universal first-party set, then one directory per named
/// library that has headers.
pub fn crossIncludeDirs(b: *std.Build, app: CrossApp) []const []const u8 {
    var dirs: std.ArrayList([]const u8) = .empty;
    // CMake adds `<app>/inc` UNCONDITIONALLY, and it is FIRST, ahead of the
    // app's own src/ and every library: an app-local header shadows a
    // same-named one further down the path. cpu1_pingpong is the app that
    // ships one (inc/shared_pingpong.h, the dual-core mailbox contract).
    //
    // It is emitted for an app that ships no inc/ too, which is a real
    // difference and not a formality: this list is also what
    // `zig build compile-db` writes, and a database row whose include path is
    // one directory short of the compiler's is a row clang-tidy resolves
    // differently than the build did. The stack-budget slice measured it on ra8_io_swap_demo,
    // which ships no inc/; the step spells an absent directory as a plain -I
    // string, because only an existing directory can be declared as a step
    // input.
    dirs.append(b.allocator, b.fmt("{s}/inc", .{app.dir})) catch @panic("OOM");
    dirs.append(b.allocator, b.fmt("{s}/src", .{app.dir})) catch @panic("OOM");
    dirs.appendSlice(b.allocator, &cross_include_dirs) catch @panic("OOM");
    dirs.append(b.allocator, b.fmt("{s}/inc", .{app.board})) catch @panic("OOM");

    // The chip adapters' headers ride with the board, ahead of every named
    // library, where cmake/ra8_app/sources.cmake puts them through
    // _ra8_app_board_adapter_includes(). No app names an adapter in LIBS.
    for (board_adapter_include_dirs) |adapter_inc| {
        const exists = src_tree.exists(b, adapter_inc);
        if (exists) dirs.append(b.allocator, adapter_inc) catch @panic("OOM");
    }

    for (app.libraries) |library| {
        const library_inc = b.fmt("libs/{s}/inc", .{library});
        const exists = src_tree.exists(b, library_inc);
        if (exists) dirs.append(b.allocator, library_inc) catch @panic("OOM");
    }
    // The include directory a gated library unit's companion brings with it,
    // added where cmake/ra8_app/sources.cmake adds it: after the per-library
    // directories, before the alias one.
    for (library_source_gates) |gate| {
        if (declaresLibrary(app, gate.satisfied_by)) {
            dirs.append(b.allocator, gate.include_dir) catch @panic("OOM");
        }
    }
    for (library_aliases) |alias| {
        if (!declaresLibrary(app, alias.name)) continue;
        var superseded = false;
        for (alias.superseded_by) |fuller| {
            if (declaresLibrary(app, fuller)) superseded = true;
        }
        if (!superseded) dirs.append(b.allocator, alias.include_dir) catch @panic("OOM");
    }

    // Every OFF_TARGET_LIBS entry's `inc`, which lands on EVERY unit in the
    // app and not only on the library's own. See off_target.zig.
    off_target_mod.appendIncludeDirs(b, app.off_target_libs, &dirs);

    // One directory per EXTRA_SRCS entry, deduplicated, added LAST of
    // everything ra8_add_app() puts on the path (cmake/ra8_add_app.cmake
    // spells `${_ra8_extra_inc}` after `${_ra8_lib_inc}` in the same
    // target_include_directories call).
    for (app.extra_srcs) |source| {
        dirs.append(b.allocator, std.fs.path.dirname(source) orelse ".") catch @panic("OOM");
    }
    // Then whatever the app's own CMakeLists adds, which lands after
    // ra8_add_app() has already run.
    dirs.appendSlice(b.allocator, app.local.include_dirs) catch @panic("OOM");

    var kept: std.ArrayList([]const u8) = .empty;
    var seen = std.StringHashMap(void).init(b.allocator);
    for (dirs.items) |dir_path| {
        if (seen.contains(dir_path)) continue;
        seen.put(dir_path, {}) catch @panic("OOM");
        kept.append(b.allocator, dir_path) catch @panic("OOM");
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
