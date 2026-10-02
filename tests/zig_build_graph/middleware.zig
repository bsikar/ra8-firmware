//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Vendored middleware (`USES`) for the root build graph (part of RA8FW-339).
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
//! clean and never advances its time base (the USB-FS no-timer-tick bug), and objects linked
//! directly instead of through an archive pull in members a static link would
//! have left out. Only the source set fails loudly.
//!
//! The record below is therefore data with tests, and every field of it was
//! measured from a real cross configure's own `compile_commands.json`, not
//! read off `cmake/threadx.cmake`. ThreadX is the first middleware here; the
//! remaining six (usbx, netxduo, mbedtls, levelx, nimble, esp_hosted) are
//! later slices and each is a table entry rather than new code.

const std = @import("std");
const pkg_path = @import("pkg_path.zig");

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

    /// Vendored `*.c` selected by basename prefix rather than taken whole,
    /// the shape a `file(GLOB dir/<prefix>*.c)` plus `list(FILTER ... EXCLUDE
    /// REGEX)` pair has. Compiled at the same no-warning bar as soup_c_dirs.
    soup_c_globs: []const SoupGlob = &.{},

    /// Directories whose lowercase `*.s` still need the C preprocessor (they
    /// `#include` and `#define`), the shape ports_module/cortex_m33/gnu ships.
    /// Assembled with the assembly flags plus `-x assembler-with-cpp`.
    soup_cpp_asm_dirs: []const []const u8 = &.{},

    /// Middlewares this one compiles against: its own TUs get their PUBLIC
    /// defines and include directories, the way `target_link_libraries(<mw>
    /// PRIVATE <dep>)` hands them over. The app names the dependency in USES
    /// itself; nothing here adds it to the link.
    requires: []const []const u8 = &.{},

    /// True when CMake hands the app this middleware's OBJECTS rather than an
    /// archive (an INTERFACE library over `$<TARGET_OBJECTS:...>`). Every
    /// object then joins the link whether or not anything references it, so
    /// archiving them here would link a smaller, different image.
    link_objects: bool = false,

    /// Project-owned sources the middleware adds to the APP's own sources
    /// (a `RA8_<NAME>_PORT_SOURCES` global property). They are compiled at
    /// the app's bar, -Werror and all, not at the middleware's.
    app_sources: []const []const u8 = &.{},

    /// `-I` directories the middleware's port library adds to the app only,
    /// after its public ones. Its own TUs never see them.
    app_include_dirs: []const []const u8 = &.{},
};

/// One prefix-selected glob over a vendored source directory.
pub const SoupGlob = struct {
    dir: []const u8,
    /// Basename prefixes the glob selects. Empty means every `*.c`.
    prefixes: []const []const u8 = &.{},
    /// Basename prefixes the listfile filters back out afterwards.
    excluded_prefixes: []const []const u8 = &.{},
};

/// True when `glob` selects `basename`.
pub fn globSelects(glob: SoupGlob, basename: []const u8) bool {
    if (!std.mem.endsWith(u8, basename, ".c")) return false;
    for (glob.excluded_prefixes) |prefix| {
        if (std.mem.startsWith(u8, basename, prefix)) return false;
    }
    if (glob.prefixes.len == 0) return true;
    for (glob.prefixes) |prefix| {
        if (std.mem.startsWith(u8, basename, prefix)) return true;
    }
    return false;
}

/// Eclipse ThreadX on the Cortex-M85, from cmake/threadx.cmake.
pub const threadx = Middleware{
    .name = "threadx",
    .soup_c_dirs = &.{
        "pkg:threadx/common/src",
        "pkg:threadx/ports/cortex_m85/gnu/src",
    },
    .soup_asm_dirs = &.{"pkg:threadx/ports/cortex_m85/gnu/src"},
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
    // fw_os_threadx.c binds the libs/if `fw_os` contract onto ThreadX and
    // ra8_threadx.h takes a `fw_clock_t*` in its own API, so the seam's headers
    // are this target's public interface, not an implementation detail.
    // cmake/threadx.cmake adds it PUBLIC ahead of the port directory, and the
    // order is the one on the command line.
    .public_include_dirs = &.{ "libs/if/inc", "port/threadx/inc" },
    .public_system_include_dirs = &.{
        "pkg:threadx/common/inc",
        "pkg:threadx/ports/cortex_m85/gnu/inc",
    },
    // Without this the kernel reads its own defaults instead of
    // port/threadx/inc/tx_user.h: a different tick rate, different stack
    // sizes, different feature set, and not one diagnostic about it.
    .public_defines = &.{"-DTX_INCLUDE_USER_DEFINE_FILE"},
    // The shared SysTick_Handler, Zig in ra8_core's archive since the
    // ra8_time port, takes a WEAK reference to _tx_timer_interrupt so non-ThreadX apps
    // still link; a weak reference does not pull the archive member, so the
    // weak symbol resolves to NULL and the ThreadX time base never advances.
    // The link succeeds either way, which is exactly why this belongs in the
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
        "pkg:threadx/common/src",
        "pkg:threadx/ports/cortex_m85/gnu/src",
    },
    .soup_asm_dirs = &.{"pkg:threadx/ports/cortex_m85/gnu/src"},
    .replaced_basenames = &.{"tx_initialize_low_level.S"},
    .project_sources = &.{
        "port/threadx/src/cortex_m85/tx_initialize_low_level.S",
        "port/threadx/src/cortex_m85/tx_systick_ready.c",
    },
    .private_include_dirs = &.{"libs/ra8_core/inc"},
    .public_include_dirs = &.{"port/threadx/inc"},
    .public_system_include_dirs = &.{
        "pkg:threadx/common/inc",
        "pkg:threadx/ports/cortex_m85/gnu/inc",
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

/// Eclipse USBX device stack, from cmake/usbx.cmake and port/usbx/CMakeLists.txt.
/// Measured from usb_selftest_cdc's own cross configure (RA8FW-317): 247 vendored
/// TUs in usbx_objs, 10 port TUs compiled into the app.
pub const usbx = Middleware{
    .name = "usbx",
    .soup_c_dirs = &.{},
    .soup_asm_dirs = &.{},
    .replaced_basenames = &.{},
    .project_sources = &.{},
    .private_include_dirs = &.{},
    .public_include_dirs = &.{},
    .soup_c_globs = &.{
        // The simulator host and device controllers are dropped; the RA8
        // bridges in port/usbx/src replace them. 172 of 221 core TUs.
        .{
            .dir = "pkg:usbx/common/core/src",
            .excluded_prefixes = &.{ "ux_dcd_sim_slave_", "ux_hcd_sim_host_" },
        },
        // Four device classes, 75 of 225 TUs. PIMA is not storage, and the
        // stock inquiry handler is replaced by the port's own copy.
        .{
            .dir = "pkg:usbx/common/usbx_device_classes/src",
            .prefixes = &.{
                "ux_device_class_cdc_acm_",
                "ux_device_class_hid_",
                "ux_device_class_storage_",
                "ux_device_class_dfu_",
            },
            .excluded_prefixes = &.{ "ux_device_class_pima_storage_", "ux_device_class_storage_inquiry.c" },
        },
    },
    .public_system_include_dirs = &.{
        "pkg:usbx/common/core/inc",
        "pkg:usbx/common/usbx_device_classes/inc",
        "pkg:usbx/ports/cortex_m33/gnu/inc",
    },
    // RA8_USBX_MAX_PERIPHERAL_LUN and RA8_USBX_REQUEST_DATA_MAX_LENGTH, at
    // their cache defaults. Both size structures the app shares with the
    // stack, so a mismatch is an ABI break nothing reports.
    .public_defines = &.{ "-DUX_MAX_SLAVE_LUN=2", "-DUX_SLAVE_REQUEST_DATA_MAX_LENGTH=4096" },
    .link_options = &.{},
    .requires = &.{"threadx"},
    .link_objects = true,
    .app_sources = &.{
        "port/usbx/src/ux_dcd_ra8_usb.c",
        "port/usbx/src/ux_dcd_ra8_usb_ep.c",
        "port/usbx/src/ux_dcd_ra8_usb_xfer.c",
        "port/usbx/src/ux_dcd_ra8_usb_isr.c",
        "port/usbx/src/ux_dcd_ra8_usb_setup.c",
        "port/usbx/src/ux_dcd_ra8_usb_dvst.c",
        "port/usbx/src/ux_dcd_ra8_usb_dvst_default.c",
        "port/usbx/src/ux_dcd_ra8_usb_irq.c",
        "port/usbx/src/ux_hcd_ra8_usb.c",
        "port/usbx/src/ux_device_class_storage_inquiry.c",
    },
    .app_include_dirs = &.{"port/usbx/inc"},
};

const known = [_]Middleware{ threadx, threadx_ns, usbx };

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
    for (mws) |mw| {
        out.appendSlice(mw.public_include_dirs) catch @panic("OOM");
        out.appendSlice(mw.app_include_dirs) catch @panic("OOM");
    }
    return out.items;
}

/// The project-owned sources the middleware set adds to the app's own.
pub fn appSources(allocator: std.mem.Allocator, mws: []const Middleware) []const []const u8 {
    var out = std.ArrayList([]const u8).init(allocator);
    for (mws) |mw| out.appendSlice(mw.app_sources) catch @panic("OOM");
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
    for (required(allocator, mw)) |dep| out.appendSlice(dep.public_include_dirs) catch @panic("OOM");
    return out.items;
}

/// The middleware's own `-isystem` path: its directories, then each required
/// middleware's, the order the usbx_objs compile line has.
pub fn systemIncludeDirs(allocator: std.mem.Allocator, mw: Middleware) []const []const u8 {
    var out = std.ArrayList([]const u8).init(allocator);
    out.appendSlice(mw.public_system_include_dirs) catch @panic("OOM");
    for (required(allocator, mw)) |dep| out.appendSlice(dep.public_system_include_dirs) catch @panic("OOM");
    return out.items;
}

/// The defines on the middleware's own TUs: each required middleware's
/// PUBLIC ones first, then its own.
pub fn unitDefines(allocator: std.mem.Allocator, mw: Middleware) []const []const u8 {
    var out = std.ArrayList([]const u8).init(allocator);
    for (required(allocator, mw)) |dep| out.appendSlice(dep.public_defines) catch @panic("OOM");
    out.appendSlice(mw.public_defines) catch @panic("OOM");
    return out.items;
}

fn required(allocator: std.mem.Allocator, mw: Middleware) []const Middleware {
    return resolve(allocator, mw.requires);
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
    language: enum { c, assembly, assembly_cpp },
};

fn collect(
    b: *std.Build,
    dir_path: []const u8,
    extension: []const u8,
    mw: Middleware,
    out: *std.ArrayList(Unit),
) void {
    var dir = pkg_path.openDir(b, dir_path) orelse return;
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
            .language = if (std.mem.eql(u8, extension, ".c"))
                .c
            else if (std.mem.eql(u8, extension, ".s"))
                .assembly_cpp
            else
                .assembly,
        }) catch @panic("OOM");
    }
}

fn collectGlob(b: *std.Build, glob: SoupGlob, out: *std.ArrayList(Unit)) void {
    var dir = pkg_path.openDir(b, glob.dir) orelse return;
    defer dir.close();

    var names = std.ArrayList([]const u8).init(b.allocator);
    var it = dir.iterate();
    while (it.next() catch |err| {
        std.debug.panic("ra8: cannot walk '{s}': {s}", .{ glob.dir, @errorName(err) });
    }) |entry| {
        if (entry.kind != .file or !globSelects(glob, entry.name)) continue;
        names.append(b.dupe(entry.name)) catch @panic("OOM");
    }
    std.mem.sort([]const u8, names.items, {}, struct {
        fn lessThan(_: void, a: []const u8, c: []const u8) bool {
            return std.mem.lessThan(u8, a, c);
        }
    }.lessThan);
    for (names.items) |name| {
        out.append(.{ .path = b.fmt("{s}/{s}", .{ glob.dir, name }), .language = .c }) catch @panic("OOM");
    }
}

/// Every translation unit compiled into the middleware archive: the vendored
/// globs with the replaced basenames dropped, then the project-owned sources.
pub fn units(b: *std.Build, mw: Middleware) []const Unit {
    var out = std.ArrayList(Unit).init(b.allocator);
    for (mw.soup_c_dirs) |dir_path| collect(b, dir_path, ".c", mw, &out);
    for (mw.soup_c_globs) |glob| collectGlob(b, glob, &out);
    for (mw.soup_asm_dirs) |dir_path| collect(b, dir_path, ".S", mw, &out);
    for (mw.soup_cpp_asm_dirs) |dir_path| collect(b, dir_path, ".s", mw, &out);
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
        .assembly, .assembly_cpp => tc.asm_flags,
    };
}

/// The driver's language switch a unit needs on top of its flags: a lowercase
/// `.s` that still has to go through the preprocessor.
pub fn languageFlags(unit: Unit) []const []const u8 {
    return switch (unit.language) {
        .assembly_cpp => &.{ "-x", "assembler-with-cpp" },
        .c, .assembly => &.{},
    };
}

/// Build the middleware as a static archive, the artifact CMake's
/// `add_library(<name> STATIC ...)` hands the app. An archive rather than a
/// bag of objects on purpose: a static link pulls only the members something
/// references, and several of the hand-written assembly units carry sections
/// `--gc-sections` would otherwise keep.
pub fn add(b: *std.Build, mw: Middleware, tc: Toolchain) std.Build.LazyPath {
    const archive_step = b.addSystemCommand(&.{ tc.ar, "rcs" });
    const archive = archive_step.addOutputFileArg(b.fmt("lib{s}.a", .{mw.name}));
    for (addObjects(b, mw, tc)) |object| archive_step.addFileArg(object);
    return archive;
}

/// Compile every unit and hand back the objects, for a middleware whose
/// objects go straight onto the app link (`link_objects`), and for `add`.
pub fn addObjects(b: *std.Build, mw: Middleware, tc: Toolchain) []const std.Build.LazyPath {
    const include_dirs = includeDirs(b.allocator, mw);
    const defines = unitDefines(b.allocator, mw);
    const system_dirs = systemIncludeDirs(b.allocator, mw);

    var objects = std.ArrayList(std.Build.LazyPath).init(b.allocator);
    for (units(b, mw)) |unit| {
        const compile = b.addSystemCommand(&.{tc.gcc});
        compile.addArgs(tc.global_defines);
        compile.addArgs(defines);
        for (include_dirs) |include_dir| {
            compile.addPrefixedDirectoryArg("-I", pkg_path.lazy(b, include_dir));
        }
        for (system_dirs) |include_dir| {
            compile.addArg("-isystem");
            compile.addDirectoryArg(pkg_path.lazy(b, include_dir));
        }
        compile.addArgs(unitFlags(tc, mw, unit));
        compile.addArgs(languageFlags(unit));
        compile.addArg("-c");
        compile.addFileArg(pkg_path.lazy(b, unit.path));
        compile.addArg("-o");
        const object_name = b.fmt("{s}.o", .{std.fs.path.basename(unit.path)});
        objects.append(compile.addOutputFileArg(object_name)) catch @panic("OOM");
    }
    return objects.items;
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
        flags.appendSlice(unitDefines(b.allocator, mw)) catch @panic("OOM");
        flags.appendSlice(unitFlags(tc, mw, unit)) catch @panic("OOM");
        flags.appendSlice(languageFlags(unit)) catch @panic("OOM");
        out.append(.{
            .file = unit.path,
            .driver = tc.gcc,
            .flags = flags.items,
            .include_dirs = include_dirs,
            .system_include_dirs = systemIncludeDirs(b.allocator, mw),
            .object = b.fmt("middleware/{s}/{s}.o", .{ mw.name, std.fs.path.basename(unit.path) }),
        }) catch @panic("OOM");
    }
}
