//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Root Zig build graph for ra8-firmware (RA8FW-339, the parity step RA8FW-332 depends
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
//!   ra8_dfu            tests/misc/src/test_ra8_dfu_boot.c
//!   ra8_dfu            tests/misc/src/test_ra8_dfu_launch.c
//!   ra8_rot            tests/misc/src/test_ra8_dfu_antirollback.c
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
//!   zig build analysis   prove every distinct command in that database still
//!                        compiles a translation unit it names
//!   zig build abi        the Zig-to-C ABI contract, negative controls included
//!   zig build shapes     hold the committed app-shape ledger to the tree's own
//!                        ra8_add_app() declarations
//!
//! The `arm` step is the cross-build slice: it is the first target
//! artifact this graph produces, and it is deliberately one app rather than
//! the app tree, so the diff stays reviewable.

const std = @import("std");
pub const analysis = @import("tests/zig_build_graph/analysis.zig");
pub const abi_contract = @import("tests/zig_build_graph/abi_contract.zig");
pub const compile_db = @import("tests/zig_build_graph/compile_db.zig");
pub const app_local = @import("tests/zig_build_graph/app_local.zig");
pub const cpu1_image = @import("tests/zig_build_graph/cpu1_image.zig");
pub const cpu1_threadx = @import("tests/zig_build_graph/cpu1_threadx.zig");
pub const cpu1_threadx_modules = @import("tests/zig_build_graph/cpu1_threadx_modules.zig");
pub const cpu1_txm_lib = @import("tests/zig_build_graph/cpu1_txm_lib.zig");
pub const cpu1_txm_hello = @import("tests/zig_build_graph/cpu1_txm_hello.zig");
pub const txm_module_object = @import("tests/zig_build_graph/txm_module_object.zig");
pub const m85_threadx_modules = @import("tests/zig_build_graph/m85_threadx_modules.zig");
pub const m85_shared_grant = @import("port/threadx/src/cortex_m85_modules/shared_grant.zig");
pub const cross_sources = @import("tests/zig_build_graph/cross_sources.zig");
pub const middleware = @import("tests/zig_build_graph/middleware.zig");
pub const ns_image = @import("tests/zig_build_graph/ns_image.zig");
const ns_linker_script = @import("tests/zig_build_graph/ns_linker_script.zig");
pub const command_surface = @import("tests/zig_build_graph/command_surface.zig");
pub const zig_archive = @import("tests/zig_build_graph/zig_archive.zig");
pub const app_shapes = @import("tests/zig_build_graph/app_shapes.zig");
pub const host_flags = @import("tests/zig_build_graph/host_flags.zig");
pub const vendored_soup = @import("tests/zig_build_graph/vendored_soup.zig");

/// One member of the migrated-library slice: the Zig archive, its public C
/// header directory, and the C suite CMake links against that archive today.
const SliceMember = struct {
    dependency_name: []const u8,
    artifact_name: []const u8,
    include_path: []const u8,
    c_suite_path: []const u8,
    /// Archives this member's own archive externs into. A Zig archive is one
    /// compilation unit, so linking it for a suite pulls in every call the
    /// archive makes, not only the ones the suite exercises; the suite has to
    /// resolve them exactly as a target image does. Empty for a library whose
    /// only externs are into C the suite already compiles.
    extra_dependency_names: []const []const u8 = &.{},
    /// Zig files built for this suite alone, standing in for driver symbols
    /// the archive externs but the suite never calls. Each must answer every
    /// call with an error, so a suite that does reach one fails.
    host_seam_roots: []const []const u8 = &.{},
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
    .{
        .dependency_name = "ra8_dfu",
        .artifact_name = "ra8_dfu_boot",
        .include_path = "libs/ra8_dfu/inc",
        .c_suite_path = "tests/misc/src/test_ra8_dfu_boot.c",
        // The archive carries program_abi, whose flash driver externs have
        // no host implementation here (RA8FW-477).
        .host_seam_roots = &.{"tests/support/zig/dfu_flash_seam.zig"},
    },
    .{
        .dependency_name = "ra8_dfu",
        .artifact_name = "ra8_dfu_boot",
        .include_path = "libs/ra8_dfu/inc",
        .c_suite_path = "tests/misc/src/test_ra8_dfu_launch.c",
        .host_seam_roots = &.{"tests/support/zig/dfu_flash_seam.zig"},
    },
    .{
        .dependency_name = "ra8_rot",
        .artifact_name = "ra8_rot",
        .include_path = "libs/ra8_dfu/inc",
        .c_suite_path = "tests/misc/src/test_ra8_dfu_antirollback.c",
        // The image verifier sharing this archive calls the PSA crypto
        // archive. That library picks its software stand-ins for a
        // host target on its own, so the suite links it unconditionally.
        .extra_dependency_names = &.{"ra8_psa_crypto"},
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
/// under test. Under CMake these arrive through ra8_core_hal.
///
/// This list is what the Zig ports EXTERN, not what the suites call. It held
/// `ra8_log.c` and `ra8_time.c` until their ports, `ra8_scb.c` until the
/// fault block and `ra8_error_handler.c` until the error pair,
/// which is the one that emptied it.
///
/// The fault block is why that last entry existed: the general archive is one
/// compilation unit, so linking it for any port also pulls in
/// `exception_abi.zig`, whose host halt path calls `ra8_fatal_error()`. That
/// symbol is now a weak export of the same archive, so the suites resolve it
/// without a C translation unit on the side.
///
/// Empty is the expected steady state, not an oversight. A future port that
/// externs into C adds its TU here and takes it out again when that C goes.
const support_c_sources = [_][]const u8{};

/// The host C dialect and warning set every first-party host TU in this
/// graph compiles at, shared with the vendored SOUP slice. Defined in
/// tests/zig_build_graph/host_flags.zig.
pub const c_flags = host_flags.c_flags;

/// The root graph's own Zig test root, declared in .zig-test-contract.json
/// so `scripts/checks/check_zig.py --test` covers this build root too.
const build_graph_test_source = "tests/zig_build_graph/build_graph_test.zig";

pub fn build(b: *std.Build) void {
    // The one module this package exports. A consumer in another repository
    // pins a tarball of this one and calls `dependency.module("ra8_rpc")`.
    // No target and no optimize mode: it takes both from whatever imports it.
    _ = b.addModule("ra8_rpc", .{
        .root_source_file = b.path("libs/ra8_rpc/src/ra8_rpc.zig"),
    });
    // As a dependency, that module is all this package offers: stop before any lazy dependency.
    if (b.pkg_hash.len != 0) return;

    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    // CMAKE_BUILD_TYPE, by its CMake name. Debug by default so every existing
    // invocation builds exactly what it did before build types existed; an unrecognised name
    // is refused rather than silently built as Debug, which is the whole
    // failure this option exists to end.
    const build_type_name = b.option(
        []const u8,
        "build-type",
        b.fmt("CMAKE_BUILD_TYPE to build at: {s} (default Debug)", .{build_type.names(b.allocator)}),
    ) orelse "Debug";
    const selected = build_type.parse(build_type_name) orelse std.debug.panic(
        "ra8: -Dbuild-type={s} is not a configuration this graph declares; it knows {s}",
        .{ build_type_name, build_type.names(b.allocator) },
    );
    arm = build_type.globals(b.allocator, selected, cross_image.base());

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
    // This file and just/zig.just, so command_surface_test.zig can hold the
    // steps declared here to the recipes that expose them.
    command_surface.addSources(b, graph_test_module);
    // The root CMakeLists, so build_type_test.zig can hold the three
    // configurations against the listfile that declares them.
    graph_test_module.addAnonymousImport("root_cmakelists_source", .{
        .root_source_file = b.path("CMakeLists.txt"),
    });
    // And cmake/ra8_app/zig_libs.cmake, so zig_archive_test.zig can hold the
    // optimisation a migrated archive is cross-built at to the rule that
    // decides it under CMake. build.zig itself already arrives above,
    // through command_surface.addSources.
    graph_test_module.addAnonymousImport("zig_libs_cmake_source", .{
        .root_source_file = b.path("cmake/ra8_app/zig_libs.cmake"),
    });
    // And the cross-image wiring itself, which is where the archive request
    // lives since RA8FW-362 split it out of this file: the same rule has to read
    // the file that actually asks for the archive, not the one that used to.
    graph_test_module.addAnonymousImport("cross_image_source", .{
        .root_source_file = b.path("tests/zig_build_graph/cross_image.zig"),
    });
    // The committed app-shape ledger and the listfile that declares
    // ra8_add_app()'s keywords, so app_shapes_test.zig holds the cross-built
    // table to every kind of app the tree actually has.
    graph_test_module.addAnonymousImport("app_shape_ledger_source", .{
        .root_source_file = b.path(app_shapes.ledger_path),
    });
    graph_test_module.addAnonymousImport("ra8_add_app_cmake_source", .{
        .root_source_file = b.path("cmake/ra8_add_app.cmake"),
    });

    const graph_tests = b.addTest(.{ .root_module = graph_test_module });
    zig_test_step.dependOn(&b.addRunArtifact(graph_tests).step);

    const core_archive = b.dependency("ra8_core", .{
        .target = target,
        .optimize = optimize,
    }).artifact("ra8_core_zig");

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
        // The log backend the archives call into.
        suite.linkLibrary(core_archive);
        for (member.extra_dependency_names) |extra_name| {
            suite.linkLibrary(b.dependency(extra_name, .{
                .target = target,
                .optimize = optimize,
            }).artifact(extra_name));
        }
        for (member.host_seam_roots) |seam_root| {
            suite.linkLibrary(b.addLibrary(.{
                .name = b.fmt("{s}_seam", .{std.fs.path.stem(member.c_suite_path)}),
                .linkage = .static,
                .root_module = b.createModule(.{
                    .root_source_file = b.path(seam_root),
                    .target = target,
                    .optimize = optimize,
                }),
            }));
        }

        const run_suite = b.addRunArtifact(suite);
        run_suite.expectExitCode(0);
        c_test_step.dependOn(&run_suite.step);
    }

    const soup_step = b.step(
        "test-soup",
        "Compile the vendored third-party C (xz-embedded) and run its C suite",
    );
    vendored_soup.addSuite(b, soup_step, target, optimize);
    test_step.dependOn(soup_step);

    const arm_step = b.step("arm", b.fmt(
        "Cross-build {d} example apps for the RA8D2 (Cortex-M85)",
        .{cross_apps.len},
    ));
    cross_image.addCrossBuild(b, arm_step, arm);

    const threadx_m33_step = b.step("threadx-m33", cpu1_threadx.step_description);
    cross_image.addThreadxM33(b, threadx_m33_step, arm);

    const threadx_m33_modules_step = b.step("threadx-m33-modules", cpu1_threadx_modules.step_description);
    cross_image.addThreadxM33Modules(b, threadx_m33_modules_step, arm);

    const txm_m33_step = b.step("txm-m33", cpu1_txm_lib.step_description);
    cross_image.addTxmM33(b, txm_m33_step, arm);

    const txm_hello_m33_step = b.step("txm-hello-m33", cpu1_txm_hello.step_description);
    cross_image.addTxmHelloM33(b, txm_hello_m33_step, test_step, arm);

    const threadx_m85_modules_step = b.step("threadx-m85-modules", m85_threadx_modules.step_description);
    cross_image.addThreadxM85Modules(b, threadx_m85_modules_step, arm);

    // Not PATH: the cortex-m85 half needs Arm GNU 13.3, and an older
    // arm-none-eabi-gcc found first would reject the core.
    const arm_gnu_dir = b.option(
        []const u8,
        txm_module_object.toolchain_option,
        "Arm GNU Toolchain 13.3 bin directory for " ++ txm_module_object.step_name ++
            " (default " ++ txm_module_object.default_toolchain_dir ++ ")",
    ) orelse txm_module_object.default_toolchain_dir;
    const txm_module_check_step = b.step("txm-module-check", txm_module_object.step_description);
    cross_image.addTxmModuleCheck(b, txm_module_check_step, arm, arm_gnu_dir);

    const compile_db_step = b.step(
        "compile-db",
        "Emit compile_commands.json covering the TUs this graph compiles",
    );
    const database_entries = addCompileDb(b, compile_db_step);

    // The database is only an analysis INPUT if its commands still compile the
    // files they describe. compileDbEntries() is a separate code path from the
    // compile steps above, so this is the step that keeps the two honest.
    const analysis_step = b.step(
        "analysis",
        "Prove every distinct command in the analysis database compiles a TU it names",
    );
    analysis_step.dependOn(compile_db_step);
    const verified_commands = analysis.add(b, analysis_step, compileDbEntries(b));
    test_step.dependOn(analysis_step);

    // The app tree's own shape ledger: every ra8_add_app() declaration under
    // examples/ and apps/, reduced to its shape and diffed against the
    // committed file, so a new KIND of app fails this step.
    const shapes_step = b.step("shapes", app_shapes.step_description);
    const shape_summary = app_shapes.add(b, shapes_step, test_step);

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

        // A two-project TrustZone app gets a row for its Non-Secure image: the
        // script it links with (the app's override, else the board template
        // CMake configures) and the units it compiles. Without it the manifest
        // describes one of the app's two images and nothing says so.
        if (app.ns) |image| {
            const print_ns = b.addSystemCommand(&.{ "printf", "%s\t%s\t%s\n" });
            print_ns.addArg(image.name);
            print_ns.addArg(if (image.linker_script) |named|
                b.pathJoin(&.{ app.dir, named })
            else
                b.pathJoin(&.{ ns_linker_script.board_dir, ns_linker_script.template_name }));
            print_ns.addArg(b.fmt("{d} TUs", .{ns_image.units(b, app.dir, image).len}));
            parity_step.dependOn(&print_ns.step);
        }
    }

    // The vendored-C slice's own manifest row: the SOUP tree, the porting
    // header that configures it, and the C suite that exercises it.
    const print_soup = b.addSystemCommand(&.{ "printf", "%s\t%s\t%s\n" });
    print_soup.addArg(vendored_soup.slice.name);
    print_soup.addArg(vendored_soup.slice.porting_header);
    print_soup.addArg(vendored_soup.slice.c_suite_path);
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
    print_database.addArg(analysis.install_path);
    print_database.addArg(b.fmt("{d} commands, {d} verified", .{
        database_entries,
        verified_commands,
    }));
    parity_step.dependOn(&print_database.step);

    // The app tree's row. Its third number is how many kinds of app the
    // cross-build table has never built: the distance left to RA8FW-332.
    app_shapes.addParityRow(b, parity_step, shape_summary);
}

// ===========================================================================
// ARM cross-build slice
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
// g_ra8_ls_exidx_start / _end). Filed with the evidence; the
// `zig_libraries` hook here is the seam that slice will use, wired and
// exercised with an empty list rather than left to be invented later.
//
// Nothing in CMake is changed or deleted; CMake stays authoritative.

/// The apps this slice cross-builds. The table itself lives beside the
/// source-set rules it exercises, in cross_sources.zig, because that is what
/// each entry is FOR: an app is in here when it takes an arm of an
/// ra8_add_app() rule no other app does.
pub const cross_apps = cross_sources.cross_apps;

/// The flag sets a cross configure hands each kind of translation unit, and
/// the two flags TrustZone adds. Data with tests, in its own module since
/// the TrustZone build: build.zig is at the file-size ceiling and these are measurements,
/// not wiring. Aliased here under their old names so every call site below
/// still reads as the flag set it is.
pub const arm_flags = @import("tests/zig_build_graph/arm_flags.zig");
pub const build_type = @import("tests/zig_build_graph/build_type.zig");
pub const cross_build = @import("tests/zig_build_graph/cross_build.zig");
pub const cross_image = @import("tests/zig_build_graph/cross_image.zig");
pub const device = @import("tests/zig_build_graph/device.zig");
pub const off_target = @import("tests/zig_build_graph/off_target.zig");
const arm_cpu_flags = arm_flags.cpu_flags;
const arm_global_defines = arm_flags.global_defines;
const arm_dialect_flags = arm_flags.dialect_flags;
const arm_target_dialect_flags = arm_flags.target_dialect_flags;
pub const armWarningFlags = arm_flags.warningFlags;

/// The global flag sets at the configuration THIS invocation selected, and the
/// one piece of build-wide state in this file. A configuration is a single
/// choice for the whole graph the way CMAKE_BUILD_TYPE is for a configure, and
/// threading it through ten signatures would say the same thing ten times.
/// Assembled once at the top of build() from `-Dbuild-type=`, before anything
/// below is called, so the `arm` step, both second images and every
/// compile-database row cannot disagree about which configuration they are.
var arm: build_type.Globals = undefined;

/// The cross toolchain probe, kept here because the compile-database slice
/// below asks the same question the `arm` step does: are the cross tools on
/// PATH at all? The wiring they feed lives in cross_image.zig.
const findArmTools = cross_build.findTools;

// Analysis-input slice: compile_commands.json
// ===========================================================================
// The fourth slice of RA8FW-339, and the one RA8FW-332 most depends on, because the
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

    // --- the host slice --------------------------------------------
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

    // --- the vendored slice ----------------------------------------
    // Three bars, in the order vendored_soup.addSuite passes them: the vendored TUs
    // with the narrow SOUP suppression, the first-party drivers beside them at
    // the stricter bar, then the suite at the plain host set.
    for (vendored_soup.c_sources) |source| {
        candidates.append(.{
            .file = source,
            .driver = host_c_driver,
            .flags = &vendored_soup.soup_flags,
            .include_dirs = &vendored_soup.include_paths,
            .object = b.fmt("soup/{s}.o", .{std.fs.path.basename(source)}),
        }) catch @panic("OOM");
    }
    for (vendored_soup.first_party_sources) |source| {
        candidates.append(.{
            .file = source,
            .driver = host_c_driver,
            .flags = &vendored_soup.first_party_flags,
            .include_dirs = &vendored_soup.include_paths,
            .object = b.fmt("soup/{s}.o", .{std.fs.path.basename(source)}),
        }) catch @panic("OOM");
    }
    candidates.append(.{
        .file = vendored_soup.slice.c_suite_path,
        .driver = host_c_driver,
        .flags = &c_flags,
        .include_dirs = &vendored_soup.include_paths,
        .object = b.fmt("soup/{s}.o", .{std.fs.path.basename(vendored_soup.slice.c_suite_path)}),
    }) catch @panic("OOM");

    // --- the ABI-contract slice ------------------------------------
    abi_contract.appendCompileDbEntries(b, compile_db.Entry, &candidates, host_c_driver);

    // --- the ARM cross slice ---------------------------------------
    // The set a host database structurally cannot describe, and the reason
    // build_cross_compile_db.py exists. Missing cross tools drop these rows
    // rather than failing the step, the same skip the `arm` step takes; the
    // count on `zig build parity` is what shows which of the two you got.
    if (findArmTools(b)) |tools| {
        for (cross_apps) |app| {
            const middlewares = middleware.resolve(b.allocator, app.uses);
            // A middleware's exports change the app's OWN rows, so an analysis
            // gate reading this database sees the same preprocessor view the
            // compiler had. Get this wrong and clang-tidy parses the app
            // against a different tx_api.h than the build does.
            var app_flags = std.ArrayList([]const u8).init(b.allocator);
            app_flags.appendSlice(&arm_cpu_flags) catch @panic("OOM");
            app_flags.appendSlice(device.compileFlags(app.board)) catch @panic("OOM");
            app_flags.appendSlice(arm.config_flags) catch @panic("OOM");
            app_flags.appendSlice(&arm_dialect_flags) catch @panic("OOM");
            if (app.trust_zone) app_flags.append(arm_flags.trust_zone.define) catch @panic("OOM");
            app_flags.appendSlice(middleware.appDefines(b.allocator, middlewares)) catch @panic("OOM");
            // Same reason for the app's own CMakeLists: its vendored
            // library's PUBLIC defines and its own PRIVATE ones are part of
            // the preprocessor view the compiler had.
            app_flags.appendSlice(app_local.appDefines(b.allocator, app.local)) catch @panic("OOM");
            // Where a source-scope define belongs: after every target-scope
            // one. The off-target rows are the same vector with it spliced in
            // here, so an analysis gate reading this database preprocesses
            // those two units the way the compiler did.
            const defines_end = app_flags.items.len;
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
            var off_target_flags = std.ArrayList([]const u8).init(b.allocator);
            off_target_flags.appendSlice(app_flags.items[0..defines_end]) catch @panic("OOM");
            off_target_flags.append(cross_sources.off_target_define) catch @panic("OOM");
            off_target_flags.appendSlice(app_flags.items[defines_end..]) catch @panic("OOM");
            var app_sources = std.ArrayList([]const u8).init(b.allocator);
            app_sources.appendSlice(cross_sources.crossSources(b, app)) catch @panic("OOM");
            app_sources.appendSlice(middleware.appSources(b.allocator, middlewares)) catch @panic("OOM");
            for (app_sources.items) |source| {
                candidates.append(.{
                    .file = source,
                    .driver = tools.gcc,
                    .flags = if (cross_sources.isOffTargetSource(app, source))
                        off_target_flags.items
                    else
                        app_flags.items,
                    .include_dirs = include_dirs.items,
                    .system_include_dirs = system_dirs.items,
                    .object = b.fmt("arm/{s}/{s}.o", .{ app.name, std.fs.path.basename(source) }),
                }) catch @panic("OOM");
            }
            // The middleware's own TUs, at their own bar. They are shared
            // across every app that names the same middleware, so the
            // deduplicator collapses them to one set of rows.
            for (middlewares) |mw| {
                middleware.appendCompileDbEntries(b, compile_db.Entry, &candidates, mw, cross_build.middlewareToolchain(tools, arm, &arm_global_defines));
            }
            // And the app-local vendored library's, at the bar a target with
            // no project profile really gets.
            if (app.local.vendored) |lib| {
                app_local.appendCompileDbEntries(b, compile_db.Entry, &candidates, lib, cross_build.appLocalToolchain(tools, arm, &arm_global_defines));
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
                .global_compile_flags = arm.c_flags,
                .global_link_flags = arm.link_flags,
            });
            // And the Non-Secure image's, which are their own rows for the
            // same reason: a different define set, a different include path,
            // and per-set vendored suppressions the secure half never carries.
            if (app.ns) |image| {
                const ctx = cross_build.nsContext(b, tools, app, image, arm, &arm_global_defines);
                ns_image.appendCompileDbEntries(b, compile_db.Entry, &candidates, ctx);
                middleware.appendCompileDbEntries(b, compile_db.Entry, &candidates, ctx.middleware, cross_build.middlewareToolchain(tools, arm, &arm_global_defines));
            }
        }
    }

    return candidates.items;
}

/// Wire the database into `step` and hand back how many commands it carries,
/// so `zig build parity` can print the count without rebuilding the list.
fn addCompileDb(b: *std.Build, step: *std.Build.Step) usize {
    return compile_db.add(b, step, compileDbEntries(b));
}
