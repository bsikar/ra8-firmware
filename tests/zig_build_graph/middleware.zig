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
const Translator = @import("translate_c").Translator;
pub const header_patch = @import("header_patch.zig");

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

    /// Project-owned Zig roots compiled into the same archive (RA8FW-526).
    /// Each is one object for thumb/eabihf at its own CPU, and its
    /// `@cImport` sees exactly this archive's include path and defines, so
    /// it reads the vendored structs in the layout the C half compiles.
    zig_sources: []const ZigSource = &.{},

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

    /// Vendored headers recompiled with some lines rewritten (RA8FW-484).
    /// Each lands in a generated directory that goes FIRST on the
    /// middleware's own `-I` path, so it shadows the vendored copy on the
    /// `-isystem` path. Only the middleware's TUs see it: no app links a
    /// middleware that sets this yet, and the compile database lists the
    /// vendored path.
    patched_headers: []const HeaderPatch = &.{},
};

/// One vendored header and the whole lines rewritten in its generated copy.
pub const HeaderPatch = struct {
    /// The vendored header, `pkg:`-prefixed like the other directories.
    header: []const u8,
    rewrites: []const header_patch.Rewrite,
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
    var out: std.ArrayList(Middleware) = .empty;
    for (uses) |name| {
        const record = find(name) orelse std.debug.panic(
            "ra8: app names USES {s}, which the root build graph does not know yet",
            .{name},
        );
        out.append(allocator, record) catch @panic("OOM");
    }
    return out.items;
}

/// The preprocessor defines the middleware set exports onto the APP's own
/// translation units. Missing one is not a compile error, it is a different
/// kernel configuration.
pub fn appDefines(allocator: std.mem.Allocator, mws: []const Middleware) []const []const u8 {
    var out: std.ArrayList([]const u8) = .empty;
    for (mws) |mw| out.appendSlice(allocator, mw.public_defines) catch @panic("OOM");
    return out.items;
}

/// The `-I` directories appended to the app's include path, after everything
/// `crossIncludeDirs` already put there.
pub fn appIncludeDirs(allocator: std.mem.Allocator, mws: []const Middleware) []const []const u8 {
    var out: std.ArrayList([]const u8) = .empty;
    for (mws) |mw| {
        out.appendSlice(allocator, mw.public_include_dirs) catch @panic("OOM");
        out.appendSlice(allocator, mw.app_include_dirs) catch @panic("OOM");
    }
    return out.items;
}

/// The project-owned sources the middleware set adds to the app's own.
pub fn appSources(allocator: std.mem.Allocator, mws: []const Middleware) []const []const u8 {
    var out: std.ArrayList([]const u8) = .empty;
    for (mws) |mw| out.appendSlice(allocator, mw.app_sources) catch @panic("OOM");
    return out.items;
}

/// The `-isystem` directories, which come after every `-I` on the app line.
pub fn appSystemIncludeDirs(allocator: std.mem.Allocator, mws: []const Middleware) []const []const u8 {
    var out: std.ArrayList([]const u8) = .empty;
    for (mws) |mw| out.appendSlice(allocator, mw.public_system_include_dirs) catch @panic("OOM");
    return out.items;
}

/// The link options the middleware set forces onto the app link.
pub fn appLinkOptions(allocator: std.mem.Allocator, mws: []const Middleware) []const []const u8 {
    var out: std.ArrayList([]const u8) = .empty;
    for (mws) |mw| out.appendSlice(allocator, mw.link_options) catch @panic("OOM");
    return out.items;
}

/// The middleware's own include path, in compiler order: private, then public,
/// then the system directories. The app's path is a different list entirely
/// (this one has no board, no app directory, no net/usb PAL).
pub fn includeDirs(allocator: std.mem.Allocator, mw: Middleware) []const []const u8 {
    var out: std.ArrayList([]const u8) = .empty;
    if (mw.public_include_dirs_first) {
        out.appendSlice(allocator, mw.public_include_dirs) catch @panic("OOM");
        out.appendSlice(allocator, mw.private_include_dirs) catch @panic("OOM");
    } else {
        out.appendSlice(allocator, mw.private_include_dirs) catch @panic("OOM");
        out.appendSlice(allocator, mw.public_include_dirs) catch @panic("OOM");
    }
    for (required(allocator, mw)) |dep| out.appendSlice(allocator, dep.public_include_dirs) catch @panic("OOM");
    return out.items;
}

/// The middleware's own `-isystem` path: its directories, then each required
/// middleware's, the order the usbx_objs compile line has.
pub fn systemIncludeDirs(allocator: std.mem.Allocator, mw: Middleware) []const []const u8 {
    var out: std.ArrayList([]const u8) = .empty;
    out.appendSlice(allocator, mw.public_system_include_dirs) catch @panic("OOM");
    for (required(allocator, mw)) |dep| out.appendSlice(allocator, dep.public_system_include_dirs) catch @panic("OOM");
    return out.items;
}

/// The defines on the middleware's own TUs: each required middleware's
/// PUBLIC ones first, then its own.
pub fn unitDefines(allocator: std.mem.Allocator, mw: Middleware) []const []const u8 {
    var out: std.ArrayList([]const u8) = .empty;
    for (required(allocator, mw)) |dep| out.appendSlice(allocator, dep.public_defines) catch @panic("OOM");
    out.appendSlice(allocator, mw.public_defines) catch @panic("OOM");
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
    defer dir.close(b.graph.io);

    var names: std.ArrayList([]const u8) = .empty;
    var it = dir.iterate();
    while (it.next(b.graph.io) catch |err| {
        std.debug.panic("ra8: cannot walk '{s}': {s}", .{ dir_path, @errorName(err) });
    }) |entry| {
        if (entry.kind != .file) continue;
        if (!std.mem.endsWith(u8, entry.name, extension)) continue;
        if (isReplaced(mw, entry.name)) continue;
        names.append(b.allocator, b.dupe(entry.name)) catch @panic("OOM");
    }
    std.mem.sort([]const u8, names.items, {}, struct {
        fn lessThan(_: void, a: []const u8, c: []const u8) bool {
            return std.mem.lessThan(u8, a, c);
        }
    }.lessThan);
    for (names.items) |name| {
        out.append(b.allocator, .{
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
    defer dir.close(b.graph.io);

    var names: std.ArrayList([]const u8) = .empty;
    var it = dir.iterate();
    while (it.next(b.graph.io) catch |err| {
        std.debug.panic("ra8: cannot walk '{s}': {s}", .{ glob.dir, @errorName(err) });
    }) |entry| {
        if (entry.kind != .file or !globSelects(glob, entry.name)) continue;
        names.append(b.allocator, b.dupe(entry.name)) catch @panic("OOM");
    }
    std.mem.sort([]const u8, names.items, {}, struct {
        fn lessThan(_: void, a: []const u8, c: []const u8) bool {
            return std.mem.lessThan(u8, a, c);
        }
    }.lessThan);
    for (names.items) |name| {
        out.append(b.allocator, .{ .path = b.fmt("{s}/{s}", .{ glob.dir, name }), .language = .c }) catch @panic("OOM");
    }
}

/// Every translation unit compiled into the middleware archive: the vendored
/// globs with the replaced basenames dropped, then the project-owned sources.
pub fn units(b: *std.Build, mw: Middleware) []const Unit {
    var out: std.ArrayList(Unit) = .empty;
    for (mw.soup_c_dirs) |dir_path| collect(b, dir_path, ".c", mw, &out);
    for (mw.soup_c_globs) |glob| collectGlob(b, glob, &out);
    for (mw.soup_asm_dirs) |dir_path| collect(b, dir_path, ".S", mw, &out);
    for (mw.soup_cpp_asm_dirs) |dir_path| collect(b, dir_path, ".s", mw, &out);
    for (mw.project_sources) |source| {
        out.append(b.allocator, .{
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
/// One Zig root in a middleware archive. The CPU is named here because the
/// toolchain carries only gcc and ar, never a Zig target.
pub const ZigSource = struct {
    path: []const u8,
    cpu: *const std.Target.Cpu.Model,
    /// Headers translate-c turns into the root's `c` import, translated with
    /// the archive's own include path and defines.
    c_headers: []const []const u8 = &.{},
};

/// A `-DNAME` or `-DNAME=VALUE` flag as the name and value translate-c is
/// given. A bare name is 1, gcc's default; anything else is not a define.
pub const Define = struct { name: []const u8, value: []const u8 };

pub fn splitDefine(flag: []const u8) ?Define {
    if (!std.mem.startsWith(u8, flag, "-D") or flag.len == 2) return null;
    const body = flag[2..];
    const eq = std.mem.indexOfScalar(u8, body, '=') orelse return .{ .name = body, .value = "1" };
    if (eq == 0) return null;
    return .{ .name = body[0..eq], .value = body[eq + 1 ..] };
}

/// The object a Zig source becomes, named like the C objects beside it.
pub fn zigObjectName(b: *std.Build, source: ZigSource) []const u8 {
    return b.fmt("{s}.o", .{std.fs.path.stem(source.path)});
}

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

    const patched_dirs = patchedHeaderDirs(b, mw);

    var objects: std.ArrayList(std.Build.LazyPath) = .empty;
    for (units(b, mw)) |unit| {
        const compile = b.addSystemCommand(&.{tc.gcc});
        compile.addArgs(tc.global_defines);
        compile.addArgs(defines);
        for (patched_dirs) |dir| compile.addPrefixedDirectoryArg("-I", dir);
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
        objects.append(b.allocator, compile.addOutputFileArg(object_name)) catch @panic("OOM");
    }
    for (mw.zig_sources) |source| {
        objects.append(b.allocator, addZigObject(b, mw, tc, source)) catch @panic("OOM");
    }
    return objects.items;
}

/// The newlib headers gcc finds on its own and translate-c does not: the
/// vendored headers include <stdlib.h>, and a freestanding Zig target has no
/// libc of its own. Asked of the toolchain, never a guessed path.
fn libcInclude(b: *std.Build, tc: Toolchain) []const u8 {
    var code: u8 = 0;
    const out = b.runAllowFail(&.{ tc.gcc, "-print-sysroot" }, &code, .ignore) catch
        std.debug.panic("{s} -print-sysroot failed, so a Zig source cannot see newlib", .{tc.gcc});
    return b.pathJoin(&.{ std.mem.trim(u8, out, " \r\n"), "include" });
}

/// gcc's own freestanding headers (stddef.h, stdint.h), searched before
/// newlib as gcc searches them. newlib's sys/_types.h asks stddef.h for
/// wint_t through __need_wint_t, which only gcc's copy answers.
fn gccInclude(b: *std.Build, tc: Toolchain) []const u8 {
    var code: u8 = 0;
    const out = b.runAllowFail(&.{ tc.gcc, "-print-file-name=include" }, &code, .ignore) catch
        std.debug.panic("{s} -print-file-name=include failed, so a Zig source cannot see stddef.h", .{tc.gcc});
    return std.mem.trim(u8, out, " \r\n");
}

/// One Zig root built for the archive's target. Its `c` import is the
/// translate-c view of `source.c_headers` under the same patched, -I,
/// -isystem and -D set the C units get, in the same order.
fn addZigObject(b: *std.Build, mw: Middleware, tc: Toolchain, source: ZigSource) std.Build.LazyPath {
    const target = b.resolveTargetQuery(.{
        .cpu_arch = .thumb,
        .os_tag = .freestanding,
        .abi = .eabihf,
        .cpu_model = .{ .explicit = source.cpu },
    });
    const module = b.createModule(.{
        .root_source_file = pkg_path.lazy(b, source.path),
        .target = target,
        .optimize = .ReleaseSmall,
        .single_threaded = true,
    });
    if (source.c_headers.len != 0) module.addImport("c", translateHeaders(b, mw, tc, source, target));
    const object = b.addObject(.{ .name = std.fs.path.stem(source.path), .root_module = module });
    object.bundle_compiler_rt = false;
    return object.getEmittedBin();
}

/// translate-c over a generated include list (no committed C) for one Zig root.
fn translateHeaders(
    b: *std.Build,
    mw: Middleware,
    tc: Toolchain,
    source: ZigSource,
    target: std.Build.ResolvedTarget,
) *std.Build.Module {
    var text: std.ArrayList(u8) = .empty;
    for (source.c_headers) |header| text.print(b.allocator, "#include \"{s}\"\n", .{header}) catch @panic("OOM");
    const name = b.fmt("{s}_c.h", .{std.fs.path.stem(source.path)});
    const translator: Translator = .init(b.dependency("translate_c", .{}), .{
        .c_source_file = b.addWriteFiles().add(name, text.items),
        .target = target,
        .optimize = .ReleaseSmall,
        .link_libc = false,
    });
    for (patchedHeaderDirs(b, mw)) |dir| translator.addIncludePath(dir);
    for (includeDirs(b.allocator, mw)) |dir| translator.addIncludePath(pkg_path.lazy(b, dir));
    for (systemIncludeDirs(b.allocator, mw)) |dir| translator.addSystemIncludePath(pkg_path.lazy(b, dir));
    translator.addSystemIncludePath(.{ .cwd_relative = gccInclude(b, tc) });
    translator.addSystemIncludePath(.{ .cwd_relative = libcInclude(b, tc) });
    for ([_][]const []const u8{ tc.global_defines, unitDefines(b.allocator, mw) }) |set| {
        for (set) |flag| if (splitDefine(flag)) |d| translator.defineCMacro(d.name, d.value);
    }
    return translator.mod;
}

/// One generated directory per patched header, each holding the rewritten
/// copy under the vendored basename. The rewrite runs on the host and fails
/// the build when a line it expects is missing or repeated.
pub fn patchedHeaderDirs(b: *std.Build, mw: Middleware) []const std.Build.LazyPath {
    if (mw.patched_headers.len == 0) return &.{};
    const tool = b.addExecutable(.{
        .name = "header_patch",
        .root_module = b.createModule(.{
            .root_source_file = b.path("tests/zig_build_graph/header_patch.zig"),
            .target = b.graph.host,
        }),
    });
    var dirs: std.ArrayList(std.Build.LazyPath) = .empty;
    for (mw.patched_headers) |patch| {
        const run = b.addRunArtifact(tool);
        run.addFileArg(pkg_path.lazy(b, patch.header));
        const out = run.addOutputFileArg(std.fs.path.basename(patch.header));
        for (patch.rewrites) |rewrite| run.addArgs(&.{ rewrite.old, rewrite.new });
        dirs.append(b.allocator, out.dirname()) catch @panic("OOM");
    }
    return dirs.items;
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
        var flags: std.ArrayList([]const u8) = .empty;
        flags.appendSlice(b.allocator, tc.global_defines) catch @panic("OOM");
        flags.appendSlice(b.allocator, unitDefines(b.allocator, mw)) catch @panic("OOM");
        flags.appendSlice(b.allocator, unitFlags(tc, mw, unit)) catch @panic("OOM");
        flags.appendSlice(b.allocator, languageFlags(unit)) catch @panic("OOM");
        out.append(b.allocator, .{
            .file = unit.path,
            .driver = tc.gcc,
            .flags = flags.items,
            .include_dirs = include_dirs,
            .system_include_dirs = systemIncludeDirs(b.allocator, mw),
            .object = b.fmt("middleware/{s}/{s}.o", .{ mw.name, std.fs.path.basename(unit.path) }),
        }) catch @panic("OOM");
    }
}
