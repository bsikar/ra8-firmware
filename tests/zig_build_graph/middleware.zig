//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Vendored middleware (`USES`) for the root build graph (#1054, part of #857).
//!
//! An app names a middleware in `USES` and `ra8_add_app()` does four separate
//! things with it, none of which the graph could see until this slice: it
//! compiles the middleware's own translation units at a bar that is NOT the
//! app's bar, it exports include directories and preprocessor defines onto the
//! app's own translation units, it forces link options onto the app link, and
//! it hands the app a static archive rather than a pile of objects.
//!
//! The four are independent, and three of them fail SILENTLY when they are
//! missing: an app compiled without the exported define gets a different
//! `tx_api.h`, an app linked without `--undefined=_tx_timer_interrupt` links
//! clean and never advances its time base (issue #8), and objects linked
//! directly instead of through an archive pull in members a static link would
//! have left out. Only the source set fails loudly.
//!
//! The record below is therefore data with tests, and every field of it was
//! measured from a real cross configure's own `compile_commands.json`, not
//! read off `cmake/threadx.cmake`. ThreadX is the first middleware here; the
//! remaining six (usbx, netxduo, mbedtls, levelx, nimble, esp_hosted) are
//! later slices and each is a table entry rather than new code.

const std = @import("std");

/// One vendored middleware, as `cmake/<name>.cmake` defines it.
pub const Middleware = struct {
    name: []const u8,

    /// Directories whose `*.c` are the vendored kernel and port. These carry
    /// NO warning flags at all: cmake/threadx.cmake wipes COMPILE_OPTIONS on
    /// exactly this set, and the library target never had the first-party
    /// profile in the first place.
    soup_c_dirs: []const []const u8,

    /// Directories whose `*.S` are the vendored port's assembly.
    soup_asm_dirs: []const []const u8,

    /// Basenames dropped from the globs above because a project-owned copy
    /// replaces them. Getting this wrong is a duplicate-symbol link error on a
    /// good day and the wrong low-level init on a bad one.
    replaced_basenames: []const []const u8,

    /// Project-owned sources compiled into the same archive, named one by one.
    project_sources: []const []const u8,

    /// PRIVATE include directories: the middleware's own TUs see them, the app
    /// does not get them from here.
    private_include_dirs: []const []const u8,

    /// PUBLIC include directories: on the middleware's TUs AND appended to the
    /// app's include path, after everything the app already had.
    public_include_dirs: []const []const u8,

    /// PUBLIC SYSTEM include directories (`-isystem`), which is what keeps the
    /// vendor headers' diagnostics from firing inside the app's `-Werror` bar.
    /// They come after every `-I` on both.
    public_system_include_dirs: []const []const u8,

    /// PUBLIC compile definitions, on the middleware's TUs and the app's.
    public_defines: []const []const u8,

    /// INTERFACE link options, forced onto the app's link line.
    link_options: []const []const u8,

    /// True when this middleware's listfile declares its PUBLIC include
    /// directory BEFORE its PRIVATE ones, so `-Iport/threadx/inc` lands ahead
    /// of `-Ilibs/ra8_core/inc` on the middleware's own compile line.
    ///
    /// There is no universal rule here: CMake keeps a target's
    /// INCLUDE_DIRECTORIES in the order of the `target_include_directories`
    /// calls, and the two ThreadX listfiles happen to make those calls in
    /// opposite orders (cmake/threadx.cmake: PRIVATE, then SYSTEM PUBLIC, then
    /// PUBLIC; cmake/threadx_ns.cmake: SYSTEM PUBLIC, PUBLIC, then PRIVATE).
    /// Measured from each configure's own database, because a private/public
    /// convention is exactly the kind of assumption that reads fine and puts
    /// the rows in the wrong order.
    public_include_dirs_first: bool = false,
};

/// Eclipse ThreadX on the Cortex-M85, from cmake/threadx.cmake.
pub const threadx = Middleware{
    .name = "threadx",
    .soup_c_dirs = &.{
        "libs/third_party/threadx/common/src",
        "libs/third_party/threadx/ports/cortex_m85/gnu/src",
    },
    .soup_asm_dirs = &.{"libs/third_party/threadx/ports/cortex_m85/gnu/src"},
    // The upstream low-level init is dropped for the project-tuned copy in
    // port/threadx/src/cortex_m85 below. It is the ONLY exclusion, and it is a
    // whole-basename match, not a prefix: `tx_initialize_kernel_enter.c` and
    // `tx_initialize_kernel_setup.c` sit beside it and both stay.
    .replaced_basenames = &.{"tx_initialize_low_level.S"},
    .project_sources = &.{
        "port/threadx/src/cortex_m85/tx_initialize_low_level.S",
        "port/threadx/src/cortex_m85/tx_systick_ready.c",
        "port/threadx/src/cortex_m85/tx_systick_retune.c",
    },
    // tx_systick_retune.c reprograms SysTick from the live CGC clock, so the
    // middleware's own TUs need the first-party headers. PRIVATE: the symbols
    // resolve at final-app link time against ra8_core / ra8_hal.
    .private_include_dirs = &.{ "libs/ra8_core/inc", "libs/ra8_hal/inc" },
    .public_include_dirs = &.{"port/threadx/inc"},
    .public_system_include_dirs = &.{
        "libs/third_party/threadx/common/inc",
        "libs/third_party/threadx/ports/cortex_m85/gnu/inc",
    },
    // Without this the kernel reads its own defaults instead of
    // port/threadx/inc/tx_user.h: a different tick rate, different stack
    // sizes, different feature set, and not one diagnostic about it.
    .public_defines = &.{"-DTX_INCLUDE_USER_DEFINE_FILE"},
    // Issue #8. The shared SysTick_Handler in libs/ra8_core/src/ra8_time.c
    // takes a WEAK reference to _tx_timer_interrupt so non-ThreadX apps still
    // link; a weak reference does not pull the archive member, so the weak
    // symbol resolves to NULL and the ThreadX time base never advances. The
    // link succeeds either way, which is exactly why this belongs in the
    // graph rather than in a comment.
    .link_options = &.{
        "-Wl,--undefined=_tx_timer_interrupt",
        "-Wl,--undefined=g_ra8_threadx_systick_ready",
    },
};

/// The NON-SECURE variant of the same kernel, from cmake/threadx_ns.cmake. It
/// is a separate archive rather than a flag on the one above, and the
/// difference is not cosmetic:
///
///   * RA8_THREADX_NON_SECURE flips port/threadx/inc/tx_user.h from
///     TX_SINGLE_MODE_SECURE to TX_SINGLE_MODE_NON_SECURE. Same 185 kernel
///     sources, a different kernel, and nothing diagnoses the wrong one.
///   * tx_systick_retune.c is NOT in this archive. It reprograms SysTick from
///     the live CGC clock, which is a secure-world peripheral here, and its
///     absence is why the private include path narrows to ra8_core alone: the
///     secure variant needs libs/ra8_hal/inc only for that TU.
///   * The three ra8_freestanding_* shims ARE in it. The Non-Secure image
///     links no libgcc and no libc at all, so this archive is where its
///     memcpy/memset/str/math come from. Leave them out and the NS link fails
///     on symbols nothing in the tree appears to reference.
///
/// 206 TUs against the secure variant's 204, measured on a real configure.
pub const threadx_ns = Middleware{
    .name = "threadx_ns",
    .soup_c_dirs = &.{
        "libs/third_party/threadx/common/src",
        "libs/third_party/threadx/ports/cortex_m85/gnu/src",
    },
    .soup_asm_dirs = &.{"libs/third_party/threadx/ports/cortex_m85/gnu/src"},
    .replaced_basenames = &.{"tx_initialize_low_level.S"},
    .project_sources = &.{
        "port/threadx/src/cortex_m85/tx_initialize_low_level.S",
        "port/threadx/src/cortex_m85/tx_systick_ready.c",
        "libs/ra8_core/src/ra8_freestanding_mem.c",
        "libs/ra8_core/src/ra8_freestanding_str.c",
        "libs/ra8_core/src/ra8_freestanding_math.c",
    },
    .private_include_dirs = &.{"libs/ra8_core/inc"},
    .public_include_dirs = &.{"port/threadx/inc"},
    .public_system_include_dirs = &.{
        "libs/third_party/threadx/common/inc",
        "libs/third_party/threadx/ports/cortex_m85/gnu/inc",
    },
    // Both PUBLIC, so the app's own TUs see the same kernel-option view the
    // archive was built with. Declared in the order CMake's generator emits
    // them, which is lexicographic rather than declaration order.
    .public_defines = &.{ "-DRA8_THREADX_NON_SECURE", "-DTX_INCLUDE_USER_DEFINE_FILE" },
    .link_options = &.{
        "-Wl,--undefined=_tx_timer_interrupt",
        "-Wl,--undefined=g_ra8_threadx_systick_ready",
    },
    // cmake/threadx_ns.cmake declares its PUBLIC includes first and the
    // PRIVATE ra8_core one last, the opposite of cmake/threadx.cmake.
    .public_include_dirs_first = true,
};

const known = [_]Middleware{ threadx, threadx_ns };

/// The middleware record for one `USES` entry, or null when the graph does not
/// know it yet. An app naming an unknown middleware is a build error rather
/// than an app quietly built without it (see `resolve`).
pub fn find(name: []const u8) ?Middleware {
    for (known) |candidate| {
        if (std.mem.eql(u8, candidate.name, name)) return candidate;
    }
    return null;
}

/// Every middleware an app uses, in the order it names them.
pub fn resolve(allocator: std.mem.Allocator, uses: []const []const u8) []const Middleware {
    var out = std.ArrayList(Middleware).init(allocator);
    for (uses) |name| {
        const record = find(name) orelse std.debug.panic(
            "ra8: app names USES {s}, which the root build graph does not know yet",
            .{name},
        );
        out.append(record) catch @panic("OOM");
    }
    return out.items;
}

/// The preprocessor defines the middleware set exports onto the APP's own
/// translation units. Missing one is not a compile error, it is a different
/// kernel configuration.
pub fn appDefines(allocator: std.mem.Allocator, mws: []const Middleware) []const []const u8 {
    var out = std.ArrayList([]const u8).init(allocator);
    for (mws) |mw| out.appendSlice(mw.public_defines) catch @panic("OOM");
    return out.items;
}

/// The `-I` directories appended to the app's include path, after everything
/// `crossIncludeDirs` already put there.
pub fn appIncludeDirs(allocator: std.mem.Allocator, mws: []const Middleware) []const []const u8 {
    var out = std.ArrayList([]const u8).init(allocator);
    for (mws) |mw| out.appendSlice(mw.public_include_dirs) catch @panic("OOM");
    return out.items;
}

/// The `-isystem` directories, which come after every `-I` on the app line.
pub fn appSystemIncludeDirs(allocator: std.mem.Allocator, mws: []const Middleware) []const []const u8 {
    var out = std.ArrayList([]const u8).init(allocator);
    for (mws) |mw| out.appendSlice(mw.public_system_include_dirs) catch @panic("OOM");
    return out.items;
}

/// The link options the middleware set forces onto the app link.
pub fn appLinkOptions(allocator: std.mem.Allocator, mws: []const Middleware) []const []const u8 {
    var out = std.ArrayList([]const u8).init(allocator);
    for (mws) |mw| out.appendSlice(mw.link_options) catch @panic("OOM");
    return out.items;
}

/// The middleware's own include path, in compiler order: private, then public,
/// then the system directories. The app's path is a different list entirely
/// (this one has no board, no app directory, no net/usb PAL).
pub fn includeDirs(allocator: std.mem.Allocator, mw: Middleware) []const []const u8 {
    var out = std.ArrayList([]const u8).init(allocator);
    if (mw.public_include_dirs_first) {
        out.appendSlice(mw.public_include_dirs) catch @panic("OOM");
        out.appendSlice(mw.private_include_dirs) catch @panic("OOM");
    } else {
        out.appendSlice(mw.private_include_dirs) catch @panic("OOM");
        out.appendSlice(mw.public_include_dirs) catch @panic("OOM");
    }
    return out.items;
}

/// True when `basename` is one the project replaces, so the vendored glob must
/// drop it.
pub fn isReplaced(mw: Middleware, basename: []const u8) bool {
    for (mw.replaced_basenames) |replaced| {
        if (std.mem.eql(u8, replaced, basename)) return true;
    }
    return false;
}

/// One translation unit of a middleware, and whether it is assembled or
/// compiled: the two take different flag sets, because CMAKE_ASM_FLAGS is not
/// CMAKE_C_FLAGS and the assembler rejects most of what the C driver takes.
pub const Unit = struct {
    path: []const u8,
    language: enum { c, assembly },
};

fn collect(
    b: *std.Build,
    dir_path: []const u8,
    extension: []const u8,
    mw: Middleware,
    out: *std.ArrayList(Unit),
) void {
    var dir = b.build_root.handle.openDir(dir_path, .{ .iterate = true }) catch |err| {
        std.debug.panic("ra8: cannot read middleware directory '{s}': {s}", .{ dir_path, @errorName(err) });
    };
    defer dir.close();

    var names = std.ArrayList([]const u8).init(b.allocator);
    var it = dir.iterate();
    while (it.next() catch |err| {
        std.debug.panic("ra8: cannot walk '{s}': {s}", .{ dir_path, @errorName(err) });
    }) |entry| {
        if (entry.kind != .file) continue;
        if (!std.mem.endsWith(u8, entry.name, extension)) continue;
        if (isReplaced(mw, entry.name)) continue;
        names.append(b.dupe(entry.name)) catch @panic("OOM");
    }
    std.mem.sort([]const u8, names.items, {}, struct {
        fn lessThan(_: void, a: []const u8, c: []const u8) bool {
            return std.mem.lessThan(u8, a, c);
        }
    }.lessThan);
    for (names.items) |name| {
        out.append(.{
            .path = b.fmt("{s}/{s}", .{ dir_path, name }),
            .language = if (std.mem.eql(u8, extension, ".c")) .c else .assembly,
        }) catch @panic("OOM");
    }
}

/// Every translation unit compiled into the middleware archive: the vendored
/// globs with the replaced basenames dropped, then the project-owned sources.
pub fn units(b: *std.Build, mw: Middleware) []const Unit {
    var out = std.ArrayList(Unit).init(b.allocator);
    for (mw.soup_c_dirs) |dir_path| collect(b, dir_path, ".c", mw, &out);
    for (mw.soup_asm_dirs) |dir_path| collect(b, dir_path, ".S", mw, &out);
    for (mw.project_sources) |source| {
        out.append(.{
            .path = source,
            .language = if (std.mem.endsWith(u8, source, ".S")) .assembly else .c,
        }) catch @panic("OOM");
    }
    return out.items;
}

/// The cross tools and the two global flag sets a middleware archive is built
/// with. `c_flags` is CMAKE_C_FLAGS plus the Debug configuration; `asm_flags`
/// is CMAKE_ASM_FLAGS plus the same configuration, which on this toolchain is
/// the CPU selection and `-g3` and nothing else.
pub const Toolchain = struct {
    gcc: []const u8,
    ar: []const u8,
    /// Definitions the TOOLCHAIN adds to every translation unit in the
    /// configure, app target or not. cmake/toolchain-ra8d2.cmake calls
    /// add_compile_definitions(RA8_FREESTANDING) at directory scope, so a
    /// middleware's TUs carry it as surely as the app's do. Leave it off and
    /// the middleware is preprocessed against a different set of first-party
    /// headers than the app that links it, with nothing to show for it until
    /// something behaves oddly on hardware.
    global_defines: []const []const u8,
    c_flags: []const []const u8,
    asm_flags: []const []const u8,
};

/// The flags one unit is really given, its own language's set. Neither carries
/// a warning flag: the vendored sources have their COMPILE_OPTIONS wiped, and
/// the first-party SysTick glue beside them is in the same target, which never
/// carried the first-party profile either. That asymmetry with the app's own
/// TUs is the thing worth seeing in the compile database.
pub fn unitFlags(tc: Toolchain, mw: Middleware, unit: Unit) []const []const u8 {
    _ = mw;
    return switch (unit.language) {
        .c => tc.c_flags,
        .assembly => tc.asm_flags,
    };
}

/// Build the middleware as a static archive, the artifact CMake's
/// `add_library(<name> STATIC ...)` hands the app. An archive rather than a
/// bag of objects on purpose: a static link pulls only the members something
/// references, and several of the hand-written assembly units carry sections
/// `--gc-sections` would otherwise keep.
pub fn add(b: *std.Build, mw: Middleware, tc: Toolchain) std.Build.LazyPath {
    const include_dirs = includeDirs(b.allocator, mw);

    var objects = std.ArrayList(std.Build.LazyPath).init(b.allocator);
    for (units(b, mw)) |unit| {
        const compile = b.addSystemCommand(&.{tc.gcc});
        compile.addArgs(tc.global_defines);
        compile.addArgs(mw.public_defines);
        for (include_dirs) |include_dir| {
            compile.addPrefixedDirectoryArg("-I", b.path(include_dir));
        }
        for (mw.public_system_include_dirs) |include_dir| {
            compile.addArg("-isystem");
            compile.addDirectoryArg(b.path(include_dir));
        }
        compile.addArgs(unitFlags(tc, mw, unit));
        compile.addArg("-c");
        compile.addFileArg(b.path(unit.path));
        compile.addArg("-o");
        const object_name = b.fmt("{s}.o", .{std.fs.path.basename(unit.path)});
        objects.append(compile.addOutputFileArg(object_name)) catch @panic("OOM");
    }

    const archive_step = b.addSystemCommand(&.{ tc.ar, "rcs" });
    const archive = archive_step.addOutputFileArg(b.fmt("lib{s}.a", .{mw.name}));
    for (objects.items) |object| archive_step.addFileArg(object);
    return archive;
}

/// Append this middleware's compile commands to the database. They are their
/// own rows, not a repeat of the app's: same driver, same CPU, an entirely
/// different flag bar and include path.
pub fn appendCompileDbEntries(
    b: *std.Build,
    comptime Entry: type,
    out: *std.ArrayList(Entry),
    mw: Middleware,
    tc: Toolchain,
) void {
    const include_dirs = includeDirs(b.allocator, mw);
    for (units(b, mw)) |unit| {
        var flags = std.ArrayList([]const u8).init(b.allocator);
        flags.appendSlice(tc.global_defines) catch @panic("OOM");
        flags.appendSlice(mw.public_defines) catch @panic("OOM");
        flags.appendSlice(unitFlags(tc, mw, unit)) catch @panic("OOM");
        out.append(.{
            .file = unit.path,
            .driver = tc.gcc,
            .flags = flags.items,
            .include_dirs = include_dirs,
            .system_include_dirs = mw.public_system_include_dirs,
            .object = b.fmt("middleware/{s}/{s}.o", .{ mw.name, std.fs.path.basename(unit.path) }),
        }) catch @panic("OOM");
    }
}
