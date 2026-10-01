//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The ARM cross-build slice (#936): one example app per row of the cross-app
//! table, cross-built for the RA8D2 (Cortex-M85) with no CMake in the loop,
//! plus the second images (CPU1, non-secure) that hang off the same configure.
//!
//! This is the wiring, not the data. The source-set rules live in
//! cross_sources.zig, the flag sets in arm_flags.zig, the toolchain and
//! per-sub-target contexts in cross_build.zig, and the configuration globals
//! in build_type.zig. Split out of the root build.zig for #2791, which had
//! reached the file-size ceiling with this block as its largest tenant.
//!
//! Every entry point takes the configuration it is building at as a
//! `build_type.Globals` parameter rather than reading a module-level global,
//! so a caller cannot wire two steps at configurations that disagree.

const std = @import("std");

const app_local = @import("app_local.zig");
const arm_flags = @import("arm_flags.zig");
const build_type = @import("build_type.zig");
const cpu1_image = @import("cpu1_image.zig");
const core_archive = @import("core_archive.zig");
const board_archive = @import("board_archive.zig");
const interface_archive = @import("interface_archive.zig");
const migrated_libs = @import("migrated_libs.zig");
const cross_build = @import("cross_build.zig");
const cross_sources = @import("cross_sources.zig");
const device = @import("device.zig");
const ld_fragments = @import("ld_fragments.zig");
const middleware = @import("middleware.zig");
const ns_image = @import("ns_image.zig");

const CrossApp = cross_sources.CrossApp;

/// The flag sets, under the names every call site below already uses.
const arm_cpu_flags = arm_flags.cpu_flags;
const arm_global_defines = arm_flags.global_defines;
const arm_dialect_flags = arm_flags.dialect_flags;
const arm_target_dialect_flags = arm_flags.target_dialect_flags;
const arm_link_flags = arm_flags.link_flags;
const armWarningFlags = arm_flags.warningFlags;

/// The cross toolchain and the per-sub-target contexts, in their own module
/// since #1179. Aliased here so the call sites below read as they did.
const ArmTools = cross_build.Tools;
const findArmTools = cross_build.findTools;

/// The sets that do not vary by configuration, as build_type.Base names them.
pub fn base() build_type.Base {
    return .{
        .c_flags = &arm_cpu_flags,
        .c_dialect = &.{"-std=gnu2x"},
        .asm_flags = &arm_flags.cpu_select_flags,
        .link_flags = &arm_link_flags,
    };
}

/// Wire the cross-build into `arm_step`. Missing cross tools are a skip, not a
/// failure: the host slice above has to keep working on a machine with no Arm
/// GNU Toolchain installed.
pub fn addCrossBuild(
    b: *std.Build,
    arm_step: *std.Build.Step,
    globals: build_type.Globals,
) void {
    const tools = findArmTools(b) orelse {
        const notice = b.addSystemCommand(&.{
            "echo",
            "arm: skipped -- no arm-none-eabi-gcc/objcopy/size on PATH (run `just setup` for the pinned Arm GNU Toolchain)",
        });
        arm_step.dependOn(&notice.step);
        return;
    };
    for (cross_sources.cross_apps) |app| addCrossApp(b, arm_step, tools, app, globals);
}

fn addCrossApp(
    b: *std.Build,
    arm_step: *std.Build.Step,
    tools: ArmTools,
    app: CrossApp,
    globals: build_type.Globals,
) void {

    // The target zig_libs.cmake derives from the toolchain's own -mcpu and
    // -mfloat-abi (cortex-m85 + hard float -> thumb-freestanding-eabihf,
    // cortex_m85), spelled here as a query instead of a string so the graph
    // itself type-checks it.
    const arm_target = b.resolveTargetQuery(.{
        .cpu_arch = .thumb,
        .os_tag = .freestanding,
        .abi = .eabihf,
        .cpu_model = .{ .explicit = &std.Target.arm.cpu.cortex_m85 },
    });

    // At the optimisation THIS configuration asks for, not a fixed Debug:
    // zig_libs.cmake maps a Debug configure onto a Debug archive and every
    // other configure onto ReleaseSmall, so an archive built at one of the two
    // and kept there is not the artifact CMake links in the other. The hook
    // was written when the graph only had Debug (#1179 gave it the other two)
    // and nothing failed in between, because an archive at the wrong
    // optimisation links perfectly well.
    var archives = std.ArrayList(std.Build.LazyPath).init(b.allocator);
    var names_core = false;
    var names_board = false;
    const board_lib = board_archive.nameFor(app.board);
    for (app.zig_libraries) |lib_name| {
        if (std.mem.eql(u8, lib_name, core_archive.lib_name)) names_core = true;
        if (std.mem.eql(u8, lib_name, board_lib)) names_board = true;
        const dependency = b.dependency(lib_name, .{
            .target = arm_target,
            .optimize = globals.configuration.zig_optimize,
        });
        archives.append(dependency.artifact(lib_name).getEmittedBin()) catch @panic("OOM");
    }

    // ra8_core, which is not optional and is not in the table above.
    // cmake/ra8_app/sources.cmake registers it unconditionally and names the
    // reason in its own guard string: "links ra8_core into every app". Since
    // #2820 libs/ra8_core/src holds no .c at all, so this archive is the only
    // place an image gets memcpy / memset / str* / abs, which the compiler
    // emits calls to from ordinary struct assignment, along with the log
    // backend, the timebase and the fault block.
    //
    // The dedupe mirrors _ra8_app_link_zig_libraries(), which runs
    // list(REMOVE_DUPLICATES) for the same reason: an app free to name a
    // library the universal set already carries would otherwise link it twice.
    if (!names_core) {
        archives.append(core_archive.forTarget(
            b,
            arm_target,
            globals.configuration.zig_optimize,
        )) catch @panic("OOM");
    }

    // The selected board's own archive, on the same unconditional footing.
    // cmake/ra8_app/sources.cmake:276 registers it as soon as the board layer
    // has a build.zig, deliberately NOT gated on the board being fully
    // ported: a partly-ported board links the archive beside its remaining C
    // objects, and gating on the absence of board .c "silently dropped the
    // ported half of such a board out of the link" (#2998). This graph globs
    // the board's src/*.c but linked no archive, so every board symbol
    // already moved to Zig -- ra8_board_uart_console_write, the four
    // ra8_board_led_* entries, ra8_board_clock and the rest of the board ABI
    // -- was undefined at link with no .c definition left to satisfy it.
    //
    // Same dedupe as ra8_core, for the same reason: a board named in LIBS
    // would otherwise be linked twice.
    if (!names_board and board_archive.has(b, app.board)) {
        archives.append(board_archive.forTarget(
            b,
            app.board,
            arm_target,
            globals.configuration.zig_optimize,
        )) catch @panic("OOM");
    }

    // The RA8 chip clock adapter, immediately behind the board archive whose
    // externs it resolves. ra8_board_ek_ra8d2's hal.zig declares
    // fw_clock_ra8_iface as a pub extern and clock_profile.zig calls it; the
    // only definition is libs/if_ra8_cgc/src/clock_ops_abi.zig. No app names
    // if_ra8_cgc in LIBS and none ever will -- it is an adapter the board
    // layer binds to, not a library an app chooses -- so the LIBS sweep below
    // cannot reach it and the symbol stayed undefined in every board image.
    // tests/cmake/zig_libraries.cmake:384 states the same coupling from the
    // CMake side: these ops are "linked beside fw_if_fs rather than standing
    // alone" precisely because they resolve at the final link.
    if (board_archive.hasChipClockAdapter(b)) {
        var names_adapter = false;
        for (app.zig_libraries) |lib_name| {
            if (std.mem.eql(u8, lib_name, board_archive.chip_clock_adapter)) names_adapter = true;
        }
        if (!names_adapter) {
            archives.append(board_archive.chipClockAdapterForTarget(
                b,
                arm_target,
                globals.configuration.zig_optimize,
            )) catch @panic("OOM");
        }
    }

    // The portable interface archive, on the same unconditional footing and
    // for the same reason. `fw_clock_bind` and `fw_clock_rate_for` are called
    // from around twenty-five example main.c files; both are defined in
    // `libs/if`, which no cross app names in LIBS, so the sweep below cannot
    // reach it. Before #2791 the definitions were in
    // `libs/if/src/fw_if_clock.c`, which nothing in the tree compiles --
    // library_sources.cmake:60 records the RA8_IF_SOURCES glob being removed
    // when libs/if was declared fully migrated -- so the symbols had no
    // definition anywhere and every image calling them linked short.
    if (interface_archive.has(b)) {
        var names_interface = false;
        for (app.zig_libraries) |lib_name| {
            if (std.mem.eql(u8, lib_name, interface_archive.name)) names_interface = true;
        }
        for (app.libraries) |lib_name| {
            if (std.mem.eql(u8, lib_name, interface_archive.name)) names_interface = true;
        }
        if (!names_interface) {
            archives.append(interface_archive.forTarget(
                b,
                arm_target,
                globals.configuration.zig_optimize,
            )) catch @panic("OOM");
        }
    }

    // Every OTHER library the app names in LIBS that contributes an archive,
    // decided by cmake/ra8_app/sources.cmake:371's rule rather than by a copy
    // of its output: build.zig present AND the library's primary src/<lib>.c
    // gone. See migrated_libs.zig for why the second clause is the real test.
    //
    // app_table.zig's zig_libraries held the OUTPUT of that rule by hand, and
    // a hand-kept copy of a derived set cannot drift loudly: eleven of twelve
    // apps carried an empty list while six of the libraries the table names
    // qualify. zig_libraries stays as the explicit escape hatch and is unioned
    // with this; the board and ra8_core are already linked above, so both are
    // skipped here rather than linked twice.
    //
    // Both lists, because sources.cmake runs the SAME rule over LIBS (:371)
    // and over OFF_TARGET_LIBS (:437), as two copies of one block. An
    // off-target library's units are compiled into the app like any other,
    // just with RA8_OFF_TARGET defined on those units alone, and its archive
    // is registered identically. crypto_aes_demo is the case in the table: it
    // reaches ra8_psa_crypto through OFF_TARGET_LIBS, not LIBS, so a LIBS-only
    // sweep leaves exactly that app's archive out.
    for ([_][]const []const u8{ app.libraries, app.off_target_libs }) |list| {
        for (list) |library| {
            if (std.mem.eql(u8, library, board_lib)) continue;
            if (std.mem.eql(u8, library, core_archive.lib_name)) continue;
            var already = false;
            for (app.zig_libraries) |named| {
                if (std.mem.eql(u8, named, library)) already = true;
            }
            if (already) continue;
            if (!migrated_libs.contributesArchive(b, library)) continue;
            const dependency = b.dependency(library, .{
                .target = arm_target,
                .optimize = globals.configuration.zig_optimize,
            });
            archives.append(dependency.artifact(library).getEmittedBin()) catch @panic("OOM");
        }
    }

    // Everything the app names in USES. Each one is built as its own archive
    // AND changes how the app's own translation units are compiled: the
    // exported define and include directories below are not decoration, an
    // app compiled without them gets a different kernel configuration and no
    // diagnostic about it.
    const middlewares = middleware.resolve(b.allocator, app.uses);
    const middleware_archives = b.allocator.alloc(std.Build.LazyPath, middlewares.len) catch @panic("OOM");
    for (middlewares, 0..) |mw, index| {
        middleware_archives[index] = middleware.add(b, mw, cross_build.middlewareToolchain(tools, globals, &arm_global_defines));
    }
    const middleware_defines = middleware.appDefines(b.allocator, middlewares);
    const middleware_include_dirs = middleware.appIncludeDirs(b.allocator, middlewares);
    const middleware_system_dirs = middleware.appSystemIncludeDirs(b.allocator, middlewares);

    // A vendored static library the app's OWN CMakeLists declares, plus the
    // defines and -isystem directories it exports onto the app's translation
    // units. Silent when missed, see app_local.zig.
    const local_archive: ?std.Build.LazyPath = if (app.local.vendored) |lib|
        app_local.add(b, lib, cross_build.appLocalToolchain(tools, globals, &arm_global_defines))
    else
        null;
    const local_defines = app_local.appDefines(b.allocator, app.local);
    const local_system_dirs = app_local.appSystemIncludeDirs(app.local);

    var include_dirs = std.ArrayList([]const u8).init(b.allocator);
    include_dirs.appendSlice(cross_sources.crossIncludeDirs(b, app)) catch @panic("OOM");
    include_dirs.appendSlice(middleware_include_dirs) catch @panic("OOM");

    var objects = std.ArrayList(std.Build.LazyPath).init(b.allocator);
    for (cross_sources.crossSources(b, app)) |source| {
        const compile = b.addSystemCommand(&.{tools.gcc});
        compile.addArgs(&arm_cpu_flags);
        // The device tail, where the toolchain file's *_INIT append puts it:
        // after the shared CPU flags (so its -mfpu wins) and before the
        // configuration's own set. Empty for every ek_ra8d2 app (#1131).
        compile.addArgs(device.compileFlags(app.board));
        compile.addArgs(globals.config_flags);
        compile.addArgs(&arm_dialect_flags);
        if (app.trust_zone) compile.addArg(arm_flags.trust_zone.define);
        compile.addArgs(middleware_defines);
        compile.addArgs(local_defines);
        // A SOURCE-scope define, so it lands after every target-scope one and
        // on these units alone: OFF_TARGET_LIBS is the only rule here that
        // compiles one executable at two preprocessor views (#1133).
        if (cross_sources.isOffTargetSource(app, source)) compile.addArg(cross_sources.off_target_define);
        compile.addArgs(armWarningFlags(b.allocator, app));
        compile.addArgs(&arm_target_dialect_flags);
        if (app.trust_zone) compile.addArg(arm_flags.trust_zone.cmse);
        // Prefixed directory args, not bare -I strings: this both spells the
        // include flag and declares the directory as an input of the step, so
        // editing a header actually invalidates the cached object.
        for (include_dirs.items) |include_dir| {
            compile.addPrefixedDirectoryArg("-I", b.path(include_dir));
        }
        // After every -I, and -isystem rather than -I: the vendor headers are
        // not held to the app's -Werror bar, and putting them on the ordinary
        // include path would fail the app's own compile on the middleware's
        // diagnostics.
        for (middleware_system_dirs) |include_dir| {
            compile.addArg("-isystem");
            compile.addDirectoryArg(b.path(include_dir));
        }
        for (local_system_dirs) |include_dir| {
            compile.addArg("-isystem");
            compile.addDirectoryArg(b.path(include_dir));
        }
        compile.addArg("-c");
        compile.addFileArg(b.path(source));
        compile.addArg("-o");
        const object_name = b.fmt("{s}.o", .{std.fs.path.basename(source)});
        objects.append(compile.addOutputFileArg(object_name)) catch @panic("OOM");
    }

    // A dual-core app's second image is built first and linked in as an
    // ordinary object: CMake adds the packed blob to the M85 target's sources,
    // ahead of its own, and the app linker script pins the section.
    const cpu1_blob: ?std.Build.LazyPath = if (app.cpu1) |image| cpu1_image.add(b, arm_step, .{
        .gcc = tools.gcc,
        .objcopy = tools.objcopy,
        .size = tools.size,
        .app = .{ .name = app.name, .dir = app.dir, .board = app.board },
        .image = image,
        .core_archive = core_archive.forCpu(
            b,
            &std.Target.arm.cpu.cortex_m33,
            globals.configuration.zig_optimize,
        ),
        .global_compile_flags = globals.c_flags,
        .global_link_flags = globals.link_flags,
    }) else null;

    // An app that does not link in a Debug configure under EITHER build system
    // still compiles every one of its translation units here; only the final
    // link is held back, with the reason printed rather than a red step.
    if (!app.links_in_debug) {
        for (objects.items) |object| object.addStepDependencies(arm_step);
        const notice = b.addSystemCommand(&.{
            "echo",
            b.fmt(
                "arm: {s} compiled ({d} TUs) but NOT linked -- it overflows MRAM in a Debug configure, and CMake's own standalone configure of it fails the same way",
                .{ app.name, objects.items.len },
            ),
        });
        arm_step.dependOn(&notice.step);
        return;
    }

    const link = b.addSystemCommand(&.{tools.gcc});
    link.addArgs(&arm_cpu_flags);
    link.addArgs(device.linkFlags(app.board));
    link.addArgs(globals.config_flags);
    link.addArgs(&arm_link_flags);
    // The middleware's INTERFACE link options. Dropping these does not fail
    // the link, it produces a firmware image whose kernel time base never
    // advances (issue #8), which is the sharpest reason middleware belongs in
    // the graph as data rather than as a pile of source paths.
    link.addArgs(middleware.appLinkOptions(b.allocator, middlewares));
    // Before -T, where CMake puts it: the link picks its multilib and its
    // secure-gateway handling from this flag.
    if (app.trust_zone) link.addArg(arm_flags.trust_zone.cmse);
    // Both board memory maps INCLUDE ra8_app_pre_memory.ld and
    // ra8_app_pre_text.ld by bare name, and ra8_add_app() generates both into
    // the directory the link runs in. There is no such directory here, so the
    // pair is written into a generated one and that goes on the search path.
    // Ahead of -T, because ld resolves an INCLUDE as it reads the script.
    const fragments = ld_fragments.directory(b, app);
    link.addPrefixedDirectoryArg("-L", fragments);
    // THREADX_HEAP / CPU1_IMAGE do not inject through those two INCLUDE
    // points: ra8_add_app() writes a third script that INCLUDEs the board map
    // and appends to it, and -T takes that one (sources.cmake:1235-1238).
    if (ld_fragments.composes(b, app)) {
        link.addPrefixedFileArg("-T", fragments.path(b, ld_fragments.composed_name));
    } else {
        link.addPrefixedFileArg("-T", b.path(app.linker_script));
    }
    const map = link.addPrefixedOutputFileArg("-Wl,--Map=", b.fmt("{s}.map", .{app.name}));
    // The import library the Non-Secure link binds veneer names against. It
    // is an OUTPUT of the secure link, so it is declared as one: a follow-up
    // slice building the NS half consumes this path rather than re-deriving it.
    const implib: ?std.Build.LazyPath = if (app.cmse_implib) |name| blk: {
        link.addArg(arm_flags.trust_zone.implib_flag);
        break :blk link.addPrefixedOutputFileArg(arm_flags.trust_zone.out_implib_prefix, name);
    } else null;
    link.addArg("-o");
    const elf = link.addOutputFileArg(b.fmt("{s}.elf", .{app.name}));
    if (cpu1_blob) |blob| link.addFileArg(blob);
    for (objects.items) |object| link.addFileArg(object);
    // Archives after the objects that reference them, then libgcc last, the
    // order CMake's link line uses.
    for (middleware_archives) |archive| link.addFileArg(archive);
    for (archives.items) |archive| link.addFileArg(archive);
    link.addArg("-lgcc");
    // After -lgcc, which is where CMake puts it: target_link_libraries() in
    // the app's own CMakeLists appends to a list that already holds -lgcc, and
    // a static archive resolved on either side of libgcc can pull a different
    // set of members.
    if (local_archive) |archive| link.addFileArg(archive);

    const hex = objcopyTo(b, tools.objcopy, "ihex", elf, b.fmt("{s}.hex", .{app.name}));
    const bin = objcopyTo(b, tools.objcopy, "binary", elf, b.fmt("{s}.bin", .{app.name}));

    arm_step.dependOn(&b.addInstallFileWithDir(elf, .{ .custom = "arm" }, b.fmt("{s}.elf", .{app.name})).step);
    // Only when this app has no Non-Secure half. When it does, the hex a flash
    // flow wants is the MERGED one, and CMake's own POST_BUILD writes it over
    // the app's hex; ns_image.add installs that trio instead, so the two never
    // race for the same output path.
    if (app.ns == null) {
        arm_step.dependOn(&b.addInstallFileWithDir(hex, .{ .custom = "arm" }, b.fmt("{s}.hex", .{app.name})).step);
    }
    arm_step.dependOn(&b.addInstallFileWithDir(bin, .{ .custom = "arm" }, b.fmt("{s}.bin", .{app.name})).step);
    arm_step.dependOn(&b.addInstallFileWithDir(map, .{ .custom = "arm" }, b.fmt("{s}.map", .{app.name})).step);
    if (implib) |object| {
        arm_step.dependOn(&b.addInstallFileWithDir(object, .{ .custom = "arm" }, app.cmse_implib.?).step);
    }

    // The second, SEPARATE executable of a two-project TrustZone build. It
    // consumes this link's own outputs: the import library binds its calls to
    // the .gnu.sgstubs veneers, and the Secure ELF is an input of the hex
    // merge. Both are declared outputs above rather than paths by convention.
    if (app.ns) |image| {
        var ctx = cross_build.nsContext(b, tools, app, image, globals, &arm_global_defines);
        ctx.core_archive = core_archive.forTarget(b, arm_target, globals.configuration.zig_optimize);
        ctx.middleware_archive = middleware.add(b, ctx.middleware, cross_build.middlewareToolchain(tools, globals, &arm_global_defines));
        ctx.implib = implib;
        ctx.secure_elf = elf;
        ns_image.add(b, arm_step, ctx);
    }

    // The size report CMake prints as a post-build command.
    const report_size = b.addSystemCommand(&.{tools.size});
    report_size.addFileArg(elf);
    arm_step.dependOn(&report_size.step);
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
