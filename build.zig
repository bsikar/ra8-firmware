//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Root Zig build graph for ra8-firmware (#857, the parity step #859 depends
//! on). CMake is still authoritative for the whole repository; this graph owns
//! one bounded slice of it and proves the slice can be built and verified with
//! no CMake in the loop at all.
//!
//! The slice is the set of first-party libraries whose implementation has
//! already moved to Zig, plus the unmodified C unit suites that are their
//! behavioural contract:
//!
//!   ra8_box            tests/misc/src/test_ra8_box.c
//!   ra8_power_profile  tests/misc/src/test_ra8_power_profile.c
//!   ra8_epd_cal        tests/misc/src/test_ra8_epd_cal.c
//!
//! Under CMake the same three archives are produced by
//! tests/cmake/zig_library.cmake shelling out to `zig build`, then linked into
//! ra8_core_hal so the C suites reach them. Here the archives are ordinary
//! build-graph dependencies and each C suite is its own executable, so the
//! intended inputs match while the orchestration does not.
//!
//! Steps:
//!   zig build            build the three archives
//!   zig build test       the Zig suites plus the C suites, all of them
//!   zig build test-c     only the unmodified C suites
//!   zig build test-zig   only the Zig-native suites
//!   zig build parity     print the slice manifest the parity check reads
//!   zig build arm        cross-build one example app for the RA8D2 target
//!   zig build test-soup  compile the vendored xz-embedded decoder and run its
//!                        unmodified first-party C suite against it
//!   zig build compile-db emit compile_commands.json for every TU this graph
//!                        compiles, the input the analysis gates parse against
//!   zig build abi        the Zig-to-C ABI contract, negative controls included
//!
//! The `arm` step is the cross-build slice (#936): it is the first target
//! artifact this graph produces, and it is deliberately one app rather than
//! the app tree, so the diff stays reviewable.

const std = @import("std");
pub const abi_contract = @import("tests/zig_build_graph/abi_contract.zig");
pub const compile_db = @import("tests/zig_build_graph/compile_db.zig");
pub const app_local = @import("tests/zig_build_graph/app_local.zig");
pub const cpu1_image = @import("tests/zig_build_graph/cpu1_image.zig");
pub const cross_sources = @import("tests/zig_build_graph/cross_sources.zig");
pub const middleware = @import("tests/zig_build_graph/middleware.zig");

/// One member of the migrated-library slice: the Zig archive, its public C
/// header directory, and the C suite CMake links against that archive today.
const SliceMember = struct {
    dependency_name: []const u8,
    artifact_name: []const u8,
    include_path: []const u8,
    c_suite_path: []const u8,
};

const slice = [_]SliceMember{
    .{
        .dependency_name = "ra8_box",
        .artifact_name = "ra8_box",
        .include_path = "libs/ra8_box/inc",
        .c_suite_path = "tests/misc/src/test_ra8_box.c",
    },
    .{
        .dependency_name = "ra8_power_profile",
        .artifact_name = "ra8_power_profile",
        .include_path = "libs/ra8_power_profile/inc",
        .c_suite_path = "tests/misc/src/test_ra8_power_profile.c",
    },
    .{
        .dependency_name = "ra8_epd_cal",
        .artifact_name = "ra8_epd_cal",
        .include_path = "libs/ra8_epd_cal/inc",
        .c_suite_path = "tests/misc/src/test_ra8_epd_cal.c",
    },
};

/// Header directories every C suite in the slice needs. These are the same
/// directories tests/cmake/core_hal.cmake puts on the host test include path.
const shared_include_paths = [_][]const u8{
    "libs/ra8_core/inc",
    "libs/ra8_ui/inc",
    "tests/support/inc",
};

/// C translation units every suite in the slice needs on top of the archive
/// under test. Under CMake these arrive through ra8_core_hal; the Zig archives
/// reference `ra8_log_emit_*`, which `ra8_core` defines weakly, so the same
/// file has to be in the link here. It is the real implementation, not a stub:
/// a stub would let a logging regression pass this graph and fail CMake.
const support_c_sources = [_][]const u8{
    "libs/ra8_core/src/ra8_log.c",
};

/// The host C dialect and warning set from tests/cmake/host_config.cmake.
/// `RA8_OFF_TARGET` and `UNIT_TEST` are the two definitions that file adds to
/// every host TU; without them `ra8_log.c` reaches for Cortex-M `mrs`.
/// `-Werror` stays on: a suite that only compiles under a looser dialect here
/// than it does under CMake would make the parity claim meaningless.
pub const c_flags = [_][]const u8{
    "-std=c23",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Werror",
    "-DRA8_OFF_TARGET",
    "-DUNIT_TEST",
};

/// The root graph's own Zig test root, declared in .zig-test-contract.json
/// so `scripts/checks/check_zig.py --test` covers this build root too.
const build_graph_test_source = "tests/zig_build_graph/build_graph_test.zig";

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const test_step = b.step("test", "Build and run the whole migrated-library slice");
    const c_test_step = b.step("test-c", "Run the unmodified C suites against the Zig archives");
    const zig_test_step = b.step("test-zig", "Run the Zig-native suites of the migrated libraries");
    test_step.dependOn(c_test_step);
    test_step.dependOn(zig_test_step);

    // The graph's own Zig tests. Everything this file encodes that a directory
    // listing cannot tell you -- the board opt-in gate, the vendored
    // suppression order, the database's JSON escaping -- is ordinary data and
    // ordinary functions, so it is unit-tested directly rather than only being
    // exercised the long way round through a build. It is also what makes the
    // repository root a Zig build root the `check_zig` gate can contract: see
    // .zig-test-contract.json beside this file.
    const graph_test_module = b.createModule(.{
        .root_source_file = b.path(build_graph_test_source),
        .target = target,
        .optimize = optimize,
    });
    // This file, imported as an ordinary module. A relative import would reach
    // outside the test's module path, and duplicating the rules into the test
    // would be testing a copy of them.
    graph_test_module.addImport("build_graph", b.createModule(.{
        .root_source_file = b.path("build.zig"),
        .target = target,
        .optimize = optimize,
    }));
    const graph_tests = b.addTest(.{ .root_module = graph_test_module });
    zig_test_step.dependOn(&b.addRunArtifact(graph_tests).step);

    for (slice) |member| {
        const dependency = b.dependency(member.dependency_name, .{
            .target = target,
            .optimize = optimize,
        });
        const archive = dependency.artifact(member.artifact_name);
        b.installArtifact(archive);

        // The library's own `test` step, reached through the dependency graph
        // rather than through a second `zig build` process the way
        // tests/cmake/zig_library.cmake has to do it.
        zig_test_step.dependOn(&dependency.builder.top_level_steps.get("test").?.step);

        const suite_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
            .link_libc = true,
        });
        suite_module.addIncludePath(b.path(member.include_path));
        for (shared_include_paths) |include_path| {
            suite_module.addIncludePath(b.path(include_path));
        }
        suite_module.addCSourceFile(.{
            .file = b.path(member.c_suite_path),
            .flags = &c_flags,
        });
        for (support_c_sources) |support_path| {
            suite_module.addCSourceFile(.{
                .file = b.path(support_path),
                .flags = &c_flags,
            });
        }

        const suite = b.addExecutable(.{
            .name = b.fmt("c_suite_{s}", .{member.artifact_name}),
            .root_module = suite_module,
        });
        suite.linkLibrary(archive);

        const run_suite = b.addRunArtifact(suite);
        run_suite.expectExitCode(0);
        c_test_step.dependOn(&run_suite.step);
    }

    const soup_step = b.step(
        "test-soup",
        "Compile the vendored third-party C (xz-embedded) and run its C suite",
    );
    addVendoredCSuite(b, soup_step, target, optimize);
    test_step.dependOn(soup_step);

    const arm_step = b.step("arm", b.fmt(
        "Cross-build {d} example apps for the RA8D2 (Cortex-M85)",
        .{cross_apps.len},
    ));
    addArmCrossBuild(b, arm_step);

    const compile_db_step = b.step(
        "compile-db",
        "Emit compile_commands.json covering the TUs this graph compiles",
    );
    const database_entries = addCompileDb(b, compile_db_step);

    const parity_step = b.step("parity", "Print the slice manifest the CMake parity check reads");
    for (slice) |member| {
        const print = b.addSystemCommand(&.{ "printf", "%s\t%s\t%s\n" });
        print.addArg(member.artifact_name);
        print.addArg(member.include_path);
        print.addArg(member.c_suite_path);
        parity_step.dependOn(&print.step);
    }

    // One manifest row per cross-built app: app, linker script, and the count
    // of translation units its ELF compiles. The TU count is the number the
    // parity check compares against a real CMake configure of the same app,
    // and it is what makes a second app worth having here -- two apps with
    // different counts prove the source rules are rules.
    for (cross_apps) |app| {
        const print_app = b.addSystemCommand(&.{ "printf", "%s\t%s\t%s\n" });
        print_app.addArg(app.name);
        print_app.addArg(app.linker_script);
        print_app.addArg(b.fmt("{d} TUs", .{cross_sources.crossSources(b, app).len}));
        parity_step.dependOn(&print_app.step);

        // A dual-core app gets a second row: the embedded M33 image, the
        // linker script that places it, and the units it compiles.
        if (app.cpu1) |image| {
            const app_ref = cpu1_image.App{ .name = app.name, .dir = app.dir, .board = app.board };
            const print_cpu1 = b.addSystemCommand(&.{ "printf", "%s\t%s\t%s\n" });
            print_cpu1.addArg(cpu1_image.imageName(b.allocator, app_ref));
            print_cpu1.addArg(b.pathJoin(&.{ app.dir, image.linker_script }));
            print_cpu1.addArg(b.fmt("{d} TUs -> {s}", .{
                cpu1_image.sources(b.allocator, app_ref, image).len,
                image.section,
            }));
            parity_step.dependOn(&print_cpu1.step);
        }
    }

    // The vendored-C slice's own manifest row: the SOUP tree, the porting
    // header that configures it, and the C suite that exercises it.
    const print_soup = b.addSystemCommand(&.{ "printf", "%s\t%s\t%s\n" });
    print_soup.addArg(vendored_slice.name);
    print_soup.addArg(vendored_slice.porting_header);
    print_soup.addArg(vendored_slice.c_suite_path);
    parity_step.dependOn(&print_soup.step);

    const abi_step = b.step("abi", "Prove the Zig-to-C ABI contract, negative controls included");
    abi_contract.add(b, abi_step, parity_step, target, optimize);
    test_step.dependOn(abi_step);

    // The analysis-input slice's own manifest row: the database, where it
    // lands, and how many compile commands it carries. A row that read "-"
    // here would be the interesting case -- it would mean the graph stopped
    // describing its own translation units.
    const print_database = b.addSystemCommand(&.{ "printf", "%s\t%s\t%s\n" });
    print_database.addArg("compile_db");
    print_database.addArg("zig-out/analysis/compile_commands.json");
    print_database.addArg(b.fmt("{d} commands", .{database_entries}));
    parity_step.dependOn(&print_database.step);
}

// ===========================================================================
// ARM cross-build slice (#936)
// ===========================================================================
// One example app, cross-built for the RA8D2 (Cortex-M85) with no CMake in the
// loop. blink_hal is a hardware-validated app whose whole source set is one
// main.c plus the universal first-party set every app compiles, so the ELF this
// step produces is the smallest artifact that still proves the real thing: the
// same 200 translation units, the same flags, the same board linker script and
// the same .elf / .hex / .bin that `ra8_add_app(NAME blink_hal STACK_BYTES
// 2200)` produces under cmake/toolchain-ra8d2.cmake in a Debug configuration.
//
// An app that also links a MIGRATED Zig library on ARM was the first choice
// (power_profiler, which names ra8_power_profile in LIBS) and it does not link
// under either build system today: the archive references
// __aeabi_unwind_cpp_pr0/pr1, which pulls libgcc's unwind-arm.o into a
// -nostdlib image, and that object then wants __exidx_start / __exidx_end /
// abort, none of which the board linker script defines (it names its own
// g_ra8_ls_exidx_start / _end). Filed as #948 with the evidence; the
// `zig_libraries` hook here is the seam that slice will use, wired and
// exercised with an empty list rather than left to be invented later.
//
// Nothing in CMake is changed or deleted; CMake stays authoritative.

const CrossApp = cross_sources.CrossApp;

/// The apps this slice cross-builds. The table itself lives beside the
/// source-set rules it exercises, in cross_sources.zig, because that is what
/// each entry is FOR: an app is in here when it takes an arm of an
/// ra8_add_app() rule no other app does.
pub const cross_apps = cross_sources.cross_apps;

/// The flag sets a cross configure hands each kind of translation unit, and
/// the two flags TrustZone adds. Data with tests, in its own module since
/// #1096: build.zig is at the file-size ceiling and these are measurements,
/// not wiring. Aliased here under their old names so every call site below
/// still reads as the flag set it is.
pub const arm_flags = @import("tests/zig_build_graph/arm_flags.zig");
const arm_global_flags = arm_flags.global_flags;
const arm_cpu_flags = arm_flags.cpu_flags;
const arm_asm_flags = arm_flags.asm_flags;
const arm_global_defines = arm_flags.global_defines;
const arm_debug_flags = arm_flags.debug_flags;
const arm_dialect_flags = arm_flags.dialect_flags;
const arm_target_dialect_flags = arm_flags.target_dialect_flags;
const arm_link_flags = arm_flags.link_flags;
pub const armWarningFlags = arm_flags.warningFlags;

/// The three cross tools this slice drives.
const ArmTools = struct {
    gcc: []const u8,
    objcopy: []const u8,
    size: []const u8,
    /// The archiver, needed only since #1054: a middleware is handed to the
    /// app as a static archive, and a static link pulls only the members
    /// something references.
    ar: []const u8,
};

fn findArmTools(b: *std.Build) ?ArmTools {
    const gcc = b.findProgram(&.{"arm-none-eabi-gcc"}, &.{}) catch return null;
    const objcopy = b.findProgram(&.{"arm-none-eabi-objcopy"}, &.{}) catch return null;
    const size = b.findProgram(&.{"arm-none-eabi-size"}, &.{}) catch return null;
    const ar = b.findProgram(&.{"arm-none-eabi-ar"}, &.{}) catch return null;
    return .{ .gcc = gcc, .objcopy = objcopy, .size = size, .ar = ar };
}

/// The two global flag sets a middleware archive is built with, and the tools
/// that build it. Named once so `zig build arm` and `zig build compile-db`
/// cannot drift apart about what a middleware TU is really given.
/// The same two global sets, handed to an app-local vendored library. It gets
/// the toolchain's flags and the directory-scope defines, and none of the
/// project warning profile: that profile is applied by ra8_add_app() to the
/// app target, and a separately-declared library never passed through it.
fn appLocalToolchain(tools: ArmTools) app_local.Toolchain {
    return .{
        .gcc = tools.gcc,
        .ar = tools.ar,
        .global_flags = &arm_global_flags,
        .global_defines = &arm_global_defines,
    };
}

fn middlewareToolchain(tools: ArmTools) middleware.Toolchain {
    return .{
        .gcc = tools.gcc,
        .ar = tools.ar,
        .global_defines = &arm_global_defines,
        .c_flags = &arm_global_flags,
        .asm_flags = &arm_asm_flags,
    };
}

/// Wire the cross-build into `arm_step`. Missing cross tools are a skip, not a
/// failure: the host slice above has to keep working on a machine with no Arm
/// GNU Toolchain installed.
fn addArmCrossBuild(b: *std.Build, arm_step: *std.Build.Step) void {
    const tools = findArmTools(b) orelse {
        const notice = b.addSystemCommand(&.{
            "echo",
            "arm: skipped -- no arm-none-eabi-gcc/objcopy/size on PATH (run `just setup` for the pinned Arm GNU Toolchain)",
        });
        arm_step.dependOn(&notice.step);
        return;
    };
    for (cross_apps) |app| addArmCrossApp(b, arm_step, tools, app);
}

fn addArmCrossApp(
    b: *std.Build,
    arm_step: *std.Build.Step,
    tools: ArmTools,
    app: CrossApp,
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

    // Debug, matching the C flags above: zig_libs.cmake maps a Debug CMake
    // configuration onto a Debug archive and everything else onto ReleaseSmall,
    // so an archive built at a different optimisation than the objects beside
    // it would not be the artifact CMake links.
    var archives = std.ArrayList(std.Build.LazyPath).init(b.allocator);
    for (app.zig_libraries) |lib_name| {
        const dependency = b.dependency(lib_name, .{
            .target = arm_target,
            .optimize = .Debug,
        });
        archives.append(dependency.artifact(lib_name).getEmittedBin()) catch @panic("OOM");
    }

    // Everything the app names in USES. Each one is built as its own archive
    // AND changes how the app's own translation units are compiled: the
    // exported define and include directories below are not decoration, an
    // app compiled without them gets a different kernel configuration and no
    // diagnostic about it.
    const middlewares = middleware.resolve(b.allocator, app.uses);
    const middleware_archives = b.allocator.alloc(std.Build.LazyPath, middlewares.len) catch @panic("OOM");
    for (middlewares, 0..) |mw, index| {
        middleware_archives[index] = middleware.add(b, mw, middlewareToolchain(tools));
    }
    const middleware_defines = middleware.appDefines(b.allocator, middlewares);
    const middleware_include_dirs = middleware.appIncludeDirs(b.allocator, middlewares);
    const middleware_system_dirs = middleware.appSystemIncludeDirs(b.allocator, middlewares);

    // A vendored static library the app's OWN CMakeLists declares, plus the
    // defines and -isystem directories it exports onto the app's translation
    // units. Silent when missed, see app_local.zig.
    const local_archive: ?std.Build.LazyPath = if (app.local.vendored) |lib|
        app_local.add(b, lib, appLocalToolchain(tools))
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
        compile.addArgs(&arm_debug_flags);
        compile.addArgs(&arm_dialect_flags);
        if (app.trust_zone) compile.addArg(arm_flags.trust_zone.define);
        compile.addArgs(middleware_defines);
        compile.addArgs(local_defines);
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
        .global_compile_flags = &arm_global_flags,
        .global_link_flags = &(arm_cpu_flags ++ arm_debug_flags ++ arm_link_flags),
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
    link.addArgs(&arm_debug_flags);
    link.addArgs(&arm_link_flags);
    // The middleware's INTERFACE link options. Dropping these does not fail
    // the link, it produces a firmware image whose kernel time base never
    // advances (issue #8), which is the sharpest reason middleware belongs in
    // the graph as data rather than as a pile of source paths.
    link.addArgs(middleware.appLinkOptions(b.allocator, middlewares));
    // Before -T, where CMake puts it: the link picks its multilib and its
    // secure-gateway handling from this flag.
    if (app.trust_zone) link.addArg(arm_flags.trust_zone.cmse);
    link.addPrefixedFileArg("-T", b.path(app.linker_script));
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
    arm_step.dependOn(&b.addInstallFileWithDir(hex, .{ .custom = "arm" }, b.fmt("{s}.hex", .{app.name})).step);
    arm_step.dependOn(&b.addInstallFileWithDir(bin, .{ .custom = "arm" }, b.fmt("{s}.bin", .{app.name})).step);
    arm_step.dependOn(&b.addInstallFileWithDir(map, .{ .custom = "arm" }, b.fmt("{s}.map", .{app.name})).step);
    if (implib) |object| {
        arm_step.dependOn(&b.addInstallFileWithDir(object, .{ .custom = "arm" }, app.cmse_implib.?).step);
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

// ===========================================================================
// Third-party (SOUP) C compilation slice
// ===========================================================================
// The third slice of #857: a vendored third-party C tree compiled by the root
// build graph, with no CMake in the loop, and held to the SAME per-TU flag
// discipline CMake applies to it.
//
// xz-embedded is the right first one. It is four translation units, it is
// decode-only, and it is the vendored tree whose CMake treatment is the most
// precisely specified: tests/cmake/core_hal.cmake gives it exactly
// `-Wno-conversion -fno-strict-aliasing` and nothing else, with a comment
// recording that the set was measured one flag at a time on all four TUs
// (only -Wconversion ever fires, from the size_t -> uint32_t narrowing in the
// first-party porting header). A blanket `-w` would have made this slice
// meaningless, which is the whole point: the vendored TUs get the narrow
// suppression and the first-party wrapper beside them keeps the full bar.
//
// The suite is `apps/shared_libs/unarch/tests/src/test_unarch_xz.c`,
// unmodified, over the committed real .xz fixtures. It is the behavioural
// contract for the decoder's integration: honest streams decode byte-exactly,
// and every hostile shape (SHA-256 check, an 8 MiB declared dictionary,
// corruption, truncation, trailing bytes, a 3690:1 zeros bomb) is rejected
// fail-closed. A build graph that compiled the SOUP but got the porting header
// or the mode selection wrong would fail those cases rather than pass quietly.
//
// Nothing in CMake is changed or deleted; CMake stays authoritative.

const VendoredSlice = struct {
    name: []const u8,
    porting_header: []const u8,
    c_suite_path: []const u8,
};

const vendored_slice = VendoredSlice{
    .name = "xz_embedded",
    .porting_header = "apps/shared_libs/unarch/inc/xz_config.h",
    .c_suite_path = "apps/shared_libs/unarch/tests/src/test_unarch_xz.c",
};

/// The vendored decode-only TUs, exactly the set
/// `RA8_XZ_THIRD_PARTY` in tests/cmake/library_sources.cmake lists. The
/// upstream tree carries more (the BCJ filters, the single-call decoder); this
/// firmware enables neither, so compiling them would be dead weight the CMake
/// build does not carry either.
const vendored_c_sources = [_][]const u8{
    "apps/shared_libs/third_party/xz_embedded/xz_crc32.c",
    "apps/shared_libs/third_party/xz_embedded/xz_crc64.c",
    "apps/shared_libs/third_party/xz_embedded/xz_dec_lzma2.c",
    "apps/shared_libs/third_party/xz_embedded/xz_dec_stream.c",
};

/// The first-party sources that drive the SOUP: the bounded XZ wrapper, its
/// zero-heap pool arena, and the flat-memory read seam the wrapper decodes
/// through. These are NOT vendored, so they take the full warning bar below.
const vendored_first_party_sources = [_][]const u8{
    "apps/shared_libs/unarch/src/unarch_xz.c",
    "apps/shared_libs/unarch/src/unarch_xz_pool.c",
    "apps/shared_libs/unarch/src/unarch_io.c",
    "libs/ra8_core/src/ra8_decomp_limits.c",
    "libs/ra8_core/src/ra8_log.c",
};

/// Include path for the slice. `apps/shared_libs/unarch/inc` has to be on it
/// for the VENDORED TUs too: xz_private.h includes "xz_config.h", and that
/// porting header is first-party and lives there. Getting this wrong is not a
/// compile error, it is a different decoder (upstream's kernel-allocator
/// defaults instead of the zero-heap pool), which is why the suite matters.
const vendored_include_paths = [_][]const u8{
    "apps/shared_libs/third_party/xz_embedded",
    "apps/shared_libs/unarch/inc",
    "apps/shared_libs/unarch/tests/inc",
    "libs/ra8_core/inc",
    "tests/support/inc",
    "tests/fixtures/inc",
    "tests/mocks/inc",
};

/// First-party bar for this slice: the host set plus `-Wconversion`, which is
/// the one class CMake's measurement found the vendored TUs trip. Without it
/// on the first-party TUs the narrow suppression below would be suppressing
/// nothing, and the parity claim would be empty.
pub const vendored_first_party_flags = c_flags ++ [_][]const u8{"-Wconversion"};

/// The vendored bar, from tests/cmake/core_hal.cmake: the first-party set with
/// `-Wconversion` suppressed for the porting header's fixed-width narrowing,
/// plus `-fno-strict-aliasing` because the decoder type-puns through byte
/// buffers. -Werror stays in force for every other class, including the
/// memory-safety ones, on an attacker-facing decoder.
pub const vendored_soup_flags = vendored_first_party_flags ++ [_][]const u8{
    "-Wno-conversion",
    "-fno-strict-aliasing",
};

fn addVendoredCSuite(
    b: *std.Build,
    step: *std.Build.Step,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
) void {
    const module = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    for (vendored_include_paths) |include_path| {
        module.addIncludePath(b.path(include_path));
    }
    module.addCSourceFiles(.{
        .files = &vendored_c_sources,
        .flags = &vendored_soup_flags,
    });
    module.addCSourceFiles(.{
        .files = &vendored_first_party_sources,
        .flags = &vendored_first_party_flags,
    });
    module.addCSourceFile(.{
        .file = b.path(vendored_slice.c_suite_path),
        .flags = &c_flags,
    });

    const suite = b.addExecutable(.{
        .name = "c_suite_unarch_xz",
        .root_module = module,
    });

    const run_suite = b.addRunArtifact(suite);
    run_suite.expectExitCode(0);
    step.dependOn(&run_suite.step);
}

// ===========================================================================
// Analysis-input slice (#959): compile_commands.json
// ===========================================================================
// The fourth slice of #857, and the one #859 most depends on, because the
// static-analysis gates do not analyse source -- they analyse a compile
// database, and only a CMake configure produces one today:
//
//   scripts/checks/tidy/compile_db.sh      configures tests/ with
//                                          -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
//                                          into build/tidy, and derives the
//                                          union of -I flags from it for the
//                                          TUs clang-tidy has no command for
//   scripts/builders/build_cross_compile_db.py
//                                          merges the CROSS configures into one
//                                          firmware database, and fails when a
//                                          first-party firmware TU has no
//                                          command -- the check that stops the
//                                          firmware pass silently shrinking
//   scripts/checks/check_unused_includes.py loads compile_commands.json from the
//                                          repo root, build/tidy/ or build/
//   scripts/checks/check_tool_warning_flags.py
//                                          takes databases as operands and
//                                          asserts the warning flags really are
//                                          on the command line
//
// So retiring CMake without this slice would take clang-tidy, the unused-include
// check and every IDE's language server with it, and it would do so in the quiet
// way: a gate that reports a clean run over code it never parsed.
//
// This step emits a standard JSON compilation database for exactly the
// translation units THIS graph compiles, from the same in-graph lists the build
// steps pass to the compiler. It is generated, never committed, and it covers
// the three slices already here: the host suites, the vendored xz tree at its
// asymmetric flag bars, and every ARM-cross TU with its real driver.
//
// What it deliberately does NOT claim to cover: the alternate reflow engine and
// libFuzzer merges, the tool projects under tools/, and the whole-tree database
// `just apps::compile_commands` builds. Those follow their code as later slices
// move it. A database asserting coverage it does not have is worse than no
// database, because the gates above treat coverage as proof.
//
// Nothing in CMake is changed or deleted; CMake stays authoritative.

/// argv[0] for the host commands. The graph compiles host C through zig's own
/// `cc` frontend, which IS clang, and every consumer above resolves argv[0] as
/// a compiler driver, so the database names the driver rather than the wrapper:
/// `zig cc` in argv[0] would leave a bare `cc` operand that clang tooling reads
/// as an input file. A consumer that shells argv[0] literally needs a clang on
/// PATH, exactly as CMake's database needs the compiler it recorded.
const host_c_driver = "clang";

/// Every compile command this graph issues, before deduplication. The
/// mechanics of the database itself -- the record, its argument vector, how two
/// records are told apart, and how the set is rendered and installed -- live in
/// tests/zig_build_graph/compile_db.zig; what stays here is the part that
/// cannot move, which translation units this graph compiles and at which bars.
fn compileDbEntries(b: *std.Build) []const compile_db.Entry {
    var candidates = std.ArrayList(compile_db.Entry).init(b.allocator);

    // --- the host slice (#925) --------------------------------------------
    for (slice) |member| {
        var include_dirs = std.ArrayList([]const u8).init(b.allocator);
        include_dirs.append(member.include_path) catch @panic("OOM");
        include_dirs.appendSlice(&shared_include_paths) catch @panic("OOM");

        candidates.append(.{
            .file = member.c_suite_path,
            .driver = host_c_driver,
            .flags = &c_flags,
            .include_dirs = include_dirs.items,
            .object = b.fmt("host/{s}/{s}.o", .{
                member.artifact_name,
                std.fs.path.basename(member.c_suite_path),
            }),
        }) catch @panic("OOM");

        for (support_c_sources) |support| {
            candidates.append(.{
                .file = support,
                .driver = host_c_driver,
                .flags = &c_flags,
                .include_dirs = include_dirs.items,
                .object = b.fmt("host/{s}/{s}.o", .{
                    member.artifact_name,
                    std.fs.path.basename(support),
                }),
            }) catch @panic("OOM");
        }
    }

    // --- the vendored slice (#950) ----------------------------------------
    // Three bars, in the order addVendoredCSuite passes them: the vendored TUs
    // with the narrow SOUP suppression, the first-party drivers beside them at
    // the stricter bar, then the suite at the plain host set.
    for (vendored_c_sources) |source| {
        candidates.append(.{
            .file = source,
            .driver = host_c_driver,
            .flags = &vendored_soup_flags,
            .include_dirs = &vendored_include_paths,
            .object = b.fmt("soup/{s}.o", .{std.fs.path.basename(source)}),
        }) catch @panic("OOM");
    }
    for (vendored_first_party_sources) |source| {
        candidates.append(.{
            .file = source,
            .driver = host_c_driver,
            .flags = &vendored_first_party_flags,
            .include_dirs = &vendored_include_paths,
            .object = b.fmt("soup/{s}.o", .{std.fs.path.basename(source)}),
        }) catch @panic("OOM");
    }
    candidates.append(.{
        .file = vendored_slice.c_suite_path,
        .driver = host_c_driver,
        .flags = &c_flags,
        .include_dirs = &vendored_include_paths,
        .object = b.fmt("soup/{s}.o", .{std.fs.path.basename(vendored_slice.c_suite_path)}),
    }) catch @panic("OOM");

    // --- the ABI-contract slice (#1007) ------------------------------------
    abi_contract.appendCompileDbEntries(b, compile_db.Entry, &candidates, host_c_driver);

    // --- the ARM cross slice (#936) ---------------------------------------
    // The set a host database structurally cannot describe, and the reason
    // build_cross_compile_db.py exists. Missing cross tools drop these rows
    // rather than failing the step, the same skip the `arm` step takes; the
    // count on `zig build parity` is what shows which of the two you got.
    if (findArmTools(b)) |tools| {
        const cross_flags = arm_cpu_flags ++ arm_debug_flags ++ arm_dialect_flags;
        for (cross_apps) |app| {
            const middlewares = middleware.resolve(b.allocator, app.uses);
            // A middleware's exports change the app's OWN rows, so an analysis
            // gate reading this database sees the same preprocessor view the
            // compiler had. Get this wrong and clang-tidy parses the app
            // against a different tx_api.h than the build does.
            var app_flags = std.ArrayList([]const u8).init(b.allocator);
            app_flags.appendSlice(&cross_flags) catch @panic("OOM");
            if (app.trust_zone) app_flags.append(arm_flags.trust_zone.define) catch @panic("OOM");
            app_flags.appendSlice(middleware.appDefines(b.allocator, middlewares)) catch @panic("OOM");
            // Same reason for the app's own CMakeLists: its vendored
            // library's PUBLIC defines and its own PRIVATE ones are part of
            // the preprocessor view the compiler had.
            app_flags.appendSlice(app_local.appDefines(b.allocator, app.local)) catch @panic("OOM");
            // At this app's own frame budget, in the position the compile step
            // puts it: a database row whose -Wstack-usage disagrees with the
            // build would hand clang-tidy a different bar than the compiler had.
            app_flags.appendSlice(armWarningFlags(b.allocator, app)) catch @panic("OOM");
            app_flags.appendSlice(&arm_target_dialect_flags) catch @panic("OOM");
            if (app.trust_zone) app_flags.append(arm_flags.trust_zone.cmse) catch @panic("OOM");
            var include_dirs = std.ArrayList([]const u8).init(b.allocator);
            include_dirs.appendSlice(cross_sources.crossIncludeDirs(b, app)) catch @panic("OOM");
            include_dirs.appendSlice(middleware.appIncludeDirs(b.allocator, middlewares)) catch @panic("OOM");
            var system_dirs = std.ArrayList([]const u8).init(b.allocator);
            system_dirs.appendSlice(middleware.appSystemIncludeDirs(b.allocator, middlewares)) catch @panic("OOM");
            system_dirs.appendSlice(app_local.appSystemIncludeDirs(app.local)) catch @panic("OOM");
            for (cross_sources.crossSources(b, app)) |source| {
                candidates.append(.{
                    .file = source,
                    .driver = tools.gcc,
                    .flags = app_flags.items,
                    .include_dirs = include_dirs.items,
                    .system_include_dirs = system_dirs.items,
                    .object = b.fmt("arm/{s}/{s}.o", .{ app.name, std.fs.path.basename(source) }),
                }) catch @panic("OOM");
            }
            // The middleware's own TUs, at their own bar. They are shared
            // across every app that names the same middleware, so the
            // deduplicator collapses them to one set of rows.
            for (middlewares) |mw| {
                middleware.appendCompileDbEntries(b, compile_db.Entry, &candidates, mw, middlewareToolchain(tools));
            }
            // And the app-local vendored library's, at the bar a target with
            // no project profile really gets.
            if (app.local.vendored) |lib| {
                app_local.appendCompileDbEntries(b, compile_db.Entry, &candidates, lib, appLocalToolchain(tools));
            }
            // The second image's TUs are compiled at a different bar entirely
            // (no warning profile, -Os over -O0, an M33 -mcpu after the M85
            // one), so they are their own rows rather than a repeat.
            if (app.cpu1) |image| cpu1_image.appendCompileDbEntries(b, compile_db.Entry, &candidates, tools.gcc, .{
                .gcc = tools.gcc,
                .objcopy = tools.objcopy,
                .size = tools.size,
                .app = .{ .name = app.name, .dir = app.dir, .board = app.board },
                .image = image,
                .global_compile_flags = &arm_global_flags,
                .global_link_flags = &(arm_cpu_flags ++ arm_debug_flags ++ arm_link_flags),
            });
        }
    }

    return candidates.items;
}

/// Wire the database into `step` and hand back how many commands it carries,
/// so `zig build parity` can print the count without rebuilding the list.
fn addCompileDb(b: *std.Build, step: *std.Build.Step) usize {
    return compile_db.add(b, step, compileDbEntries(b));
}
