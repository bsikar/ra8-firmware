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
//!
//! The `arm` step is the cross-build slice (#936): it is the first target
//! artifact this graph produces, and it is deliberately one app rather than
//! the app tree, so the diff stays reviewable.

const std = @import("std");

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
        "Cross-build the {s} example for the RA8D2 (Cortex-M85)",
        .{cross_app.name},
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

    // The cross-build slice's own manifest row: app, linker script, the
    // migrated Zig libraries its ELF links.
    const print_app = b.addSystemCommand(&.{ "printf", "%s\t%s\t%s\n" });
    print_app.addArg(cross_app.name);
    print_app.addArg(cross_app.linker_script);
    print_app.addArg(if (cross_app.zig_libraries.len == 0)
        "-"
    else
        b.fmt("{s}", .{cross_app.zig_libraries[0]}));
    parity_step.dependOn(&print_app.step);

    // The vendored-C slice's own manifest row: the SOUP tree, the porting
    // header that configures it, and the C suite that exercises it.
    const print_soup = b.addSystemCommand(&.{ "printf", "%s\t%s\t%s\n" });
    print_soup.addArg(vendored_slice.name);
    print_soup.addArg(vendored_slice.porting_header);
    print_soup.addArg(vendored_slice.c_suite_path);
    parity_step.dependOn(&print_soup.step);

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

/// The app this slice cross-builds, spelled the way ra8_add_app() resolves it.
const CrossApp = struct {
    name: []const u8,
    dir: []const u8,
    board: []const u8,
    linker_script: []const u8,
    /// Everything the app names in `LIBS`, migrated or not. Read by the
    /// board opt-in gate below, which keys off the declared set rather than
    /// off what happens to be on disk.
    libraries: []const []const u8,
    zig_libraries: []const []const u8,
};

const cross_app = CrossApp{
    .name = "blink_hal",
    .dir = "examples/ek_ra8d2/hw_validated/hil/blink_hal",
    .board = "libs/ra8_board_ek_ra8d2",
    // ra8_add_app() falls back to the board's canonical single-core map when
    // the app has no linker_script.ld of its own, which this app does not.
    .linker_script = "libs/ra8_board_ek_ra8d2/ld/linker_script.ld",
    // blink_hal names no LIBS at all: it is the universal first-party set and
    // nothing else, which is what makes it the right first app to cross-build
    // here. The `zig_libraries` hook below is wired and exercised by an empty
    // list; an app that links a migrated Zig ARCHIVE cannot be cross-built by
    // either build system yet, see #948.
    .libraries = &.{},
    .zig_libraries = &.{},
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
    "libs/ra8_power_profile/inc",
};

/// CPU flags from cmake/toolchain-ra8d2.cmake. The RA8D2 primary M85 is
/// single-precision, hence fpv5-sp-d16 with a hard float ABI; -mthumb because
/// the M-profile cores are Thumb-only. These go on compile AND link: the link
/// step picks its multilib from them.
const arm_cpu_flags = [_][]const u8{
    "-mcpu=cortex-m85",
    "-mthumb",
    "-mfloat-abi=hard",
    "-mfpu=fpv5-sp-d16",
    "-fdata-sections",
    "-ffunction-sections",
};

/// The Debug configuration ra8_add_app() sets for a standalone app build.
const arm_debug_flags = [_][]const u8{ "-O0", "-g3", "-DDEBUG" };

/// Dialect and bare-metal flags. -ffreestanding is what lets a firmware entry
/// point be `void main(void)`; drop it and every app main.c stops compiling.
const arm_dialect_flags = [_][]const u8{
    "-std=gnu2x",
    "-ffreestanding",
    "-fshort-enums",
    "-DRA8_FREESTANDING",
};

/// The first-party warning profile from cmake/ra8_warnings.cmake at this app's
/// STACK_BYTES budget. -Werror stays on for the same reason it does on the host
/// slice: a TU that only compiles here under a looser bar than CMake holds it to
/// would make the parity claim meaningless.
const arm_warning_flags = [_][]const u8{
    "-Wall",
    "-Wextra",
    "-Werror",
    "-Wconversion",
    "-Wcast-qual",
    "-Wcast-align",
    "-Wdouble-promotion",
    "-Wformat=2",
    "-Wpointer-arith",
    "-Wshadow",
    "-Wundef",
    "-Wvla",
    "-Wwrite-strings",
    "-Wbad-function-cast",
    "-Wmissing-declarations",
    "-Wmissing-prototypes",
    "-Wnested-externs",
    "-Wold-style-definition",
    "-Wredundant-decls",
    "-Wstrict-prototypes",
    "-Wduplicated-branches",
    "-Wduplicated-cond",
    "-Wformat-overflow=2",
    "-Wformat-truncation=2",
    "-Wlogical-op",
    "-Wstack-usage=2200",
    "-fstack-usage",
};

/// Link flags from the toolchain file: no hosted runtime, prune unused
/// sections, and report the region usage the map file details.
const arm_link_flags = [_][]const u8{
    "-nostdlib",
    "-Wl,--gc-sections",
    "-Wl,--print-memory-usage",
};

/// The three cross tools this slice drives.
const ArmTools = struct {
    gcc: []const u8,
    objcopy: []const u8,
    size: []const u8,
};

fn findArmTools(b: *std.Build) ?ArmTools {
    const gcc = b.findProgram(&.{"arm-none-eabi-gcc"}, &.{}) catch return null;
    const objcopy = b.findProgram(&.{"arm-none-eabi-objcopy"}, &.{}) catch return null;
    const size = b.findProgram(&.{"arm-none-eabi-size"}, &.{}) catch return null;
    return .{ .gcc = gcc, .objcopy = objcopy, .size = size };
}

/// Collect `*.c` from one directory, sorted, so the link order is stable
/// across machines and two builds of the same tree produce the same ELF.
fn collectCSources(b: *std.Build, dir_path: []const u8, out: *std.ArrayList([]const u8)) void {
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

/// Every C translation unit in the app's link, in ra8_add_app()'s own order:
/// the app's main.c, the resolved boot files, then the globbed library set.
fn crossSources(b: *std.Build) []const []const u8 {
    var sources = std.ArrayList([]const u8).init(b.allocator);

    sources.append(b.fmt("{s}/src/main.c", .{cross_app.dir})) catch @panic("OOM");

    for (cross_boot_sources) |boot| {
        const app_copy = b.fmt("{s}/src/{s}", .{ cross_app.dir, boot });
        const board_copy = b.fmt("{s}/src/boot/{s}", .{ cross_app.board, boot });
        const exists = if (b.build_root.handle.access(app_copy, .{})) |_| true else |_| false;
        sources.append(if (exists) app_copy else board_copy) catch @panic("OOM");
    }

    for (cross_source_dirs) |dir_path| collectCSources(b, dir_path, &sources);

    // Drop the opt-in board units this app did not opt into (see
    // board_opt_in_sources).
    var kept = std.ArrayList([]const u8).init(b.allocator);
    for (sources.items) |source| {
        if (!isGatedOutBoardSource(source)) kept.append(source) catch @panic("OOM");
    }
    return kept.items;
}

/// True when `source` is a board unit whose companion library is absent from
/// the app's declared `LIBS`.
pub fn isGatedOutBoardSource(source: []const u8) bool {
    if (!std.mem.startsWith(u8, source, cross_app.board)) return false;
    for (board_opt_in_sources) |gate| {
        if (!std.mem.endsWith(u8, source, gate.suffix)) continue;
        for (gate.satisfied_by) |required| {
            for (cross_app.libraries) |declared| {
                if (std.mem.eql(u8, declared, required)) return false;
            }
        }
        return true;
    }
    return false;
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
    for (cross_app.zig_libraries) |lib_name| {
        const dependency = b.dependency(lib_name, .{
            .target = arm_target,
            .optimize = .Debug,
        });
        archives.append(dependency.artifact(lib_name).getEmittedBin()) catch @panic("OOM");
    }

    var objects = std.ArrayList(std.Build.LazyPath).init(b.allocator);
    for (crossSources(b)) |source| {
        const compile = b.addSystemCommand(&.{tools.gcc});
        compile.addArgs(&arm_cpu_flags);
        compile.addArgs(&arm_debug_flags);
        compile.addArgs(&arm_dialect_flags);
        compile.addArgs(&arm_warning_flags);
        // Prefixed directory args, not bare -I strings: this both spells the
        // include flag and declares the directory as an input of the step, so
        // editing a header actually invalidates the cached object.
        compile.addPrefixedDirectoryArg("-I", b.path(b.fmt("{s}/src", .{cross_app.dir})));
        for (cross_include_dirs) |include_dir| {
            compile.addPrefixedDirectoryArg("-I", b.path(include_dir));
        }
        compile.addArg("-c");
        compile.addFileArg(b.path(source));
        compile.addArg("-o");
        const object_name = b.fmt("{s}.o", .{std.fs.path.basename(source)});
        objects.append(compile.addOutputFileArg(object_name)) catch @panic("OOM");
    }

    const link = b.addSystemCommand(&.{tools.gcc});
    link.addArgs(&arm_cpu_flags);
    link.addArgs(&arm_debug_flags);
    link.addArgs(&arm_link_flags);
    link.addPrefixedFileArg("-T", b.path(cross_app.linker_script));
    const map = link.addPrefixedOutputFileArg("-Wl,--Map=", b.fmt("{s}.map", .{cross_app.name}));
    link.addArg("-o");
    const elf = link.addOutputFileArg(b.fmt("{s}.elf", .{cross_app.name}));
    for (objects.items) |object| link.addFileArg(object);
    // Archives after the objects that reference them, then libgcc last, the
    // order CMake's link line uses.
    for (archives.items) |archive| link.addFileArg(archive);
    link.addArg("-lgcc");

    const hex = objcopyTo(b, tools.objcopy, "ihex", elf, b.fmt("{s}.hex", .{cross_app.name}));
    const bin = objcopyTo(b, tools.objcopy, "binary", elf, b.fmt("{s}.bin", .{cross_app.name}));

    arm_step.dependOn(&b.addInstallFileWithDir(elf, .{ .custom = "arm" }, b.fmt("{s}.elf", .{cross_app.name})).step);
    arm_step.dependOn(&b.addInstallFileWithDir(hex, .{ .custom = "arm" }, b.fmt("{s}.hex", .{cross_app.name})).step);
    arm_step.dependOn(&b.addInstallFileWithDir(bin, .{ .custom = "arm" }, b.fmt("{s}.bin", .{cross_app.name})).step);
    arm_step.dependOn(&b.addInstallFileWithDir(map, .{ .custom = "arm" }, b.fmt("{s}.map", .{cross_app.name})).step);

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

/// One compile command: the TU, the driver that compiles it, the flags it is
/// really given, its include path in order, and where its object goes.
const CompileDbEntry = struct {
    file: []const u8,
    driver: []const u8,
    flags: []const []const u8,
    include_dirs: []const []const u8,
    object: []const u8,
};

/// The full argument vector for one entry, in compiler order: driver, flags,
/// include path, then the TU and its output. Absolute paths, as CMake writes
/// them, so a consumer that ignores the `directory` field still resolves.
fn compileDbArguments(b: *std.Build, entry: CompileDbEntry) []const []const u8 {
    var arguments = std.ArrayList([]const u8).init(b.allocator);
    arguments.append(entry.driver) catch @panic("OOM");
    for (entry.flags) |flag| arguments.append(flag) catch @panic("OOM");
    for (entry.include_dirs) |include_dir| {
        arguments.append(b.fmt("-I{s}", .{b.pathFromRoot(include_dir)})) catch @panic("OOM");
    }
    arguments.append("-c") catch @panic("OOM");
    arguments.append(b.pathFromRoot(entry.file)) catch @panic("OOM");
    arguments.append("-o") catch @panic("OOM");
    arguments.append(entry.object) catch @panic("OOM");
    return arguments.items;
}

/// Everything in an entry except its object path, joined. Two entries with the
/// same signature are the same compile command written twice: `ra8_log.c` is
/// compiled into each of the three host suite modules identically, and one
/// command is what CMake's database would carry for it too. Two entries that
/// differ are a real difference and both stay -- which is how the vendored
/// slice's asymmetry survives into the database, `ra8_log.c` appearing once at
/// the host bar and again at the stricter -Wconversion bar the SOUP drivers
/// take.
fn compileDbSignature(b: *std.Build, entry: CompileDbEntry) []const u8 {
    var signature = std.ArrayList(u8).init(b.allocator);
    signature.appendSlice(entry.driver) catch @panic("OOM");
    signature.appendSlice("\x00") catch @panic("OOM");
    signature.appendSlice(entry.file) catch @panic("OOM");
    for (entry.flags) |flag| {
        signature.appendSlice("\x00") catch @panic("OOM");
        signature.appendSlice(flag) catch @panic("OOM");
    }
    for (entry.include_dirs) |include_dir| {
        signature.appendSlice("\x00") catch @panic("OOM");
        signature.appendSlice(include_dir) catch @panic("OOM");
    }
    return signature.items;
}

/// Every compile command this graph issues, deduplicated by signature.
fn compileDbEntries(b: *std.Build) []const CompileDbEntry {
    var candidates = std.ArrayList(CompileDbEntry).init(b.allocator);

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

    // --- the ARM cross slice (#936) ---------------------------------------
    // The set a host database structurally cannot describe, and the reason
    // build_cross_compile_db.py exists. Missing cross tools drop these rows
    // rather than failing the step, the same skip the `arm` step takes; the
    // count on `zig build parity` is what shows which of the two you got.
    if (findArmTools(b)) |tools| {
        var include_dirs = std.ArrayList([]const u8).init(b.allocator);
        include_dirs.append(b.fmt("{s}/src", .{cross_app.dir})) catch @panic("OOM");
        include_dirs.appendSlice(&cross_include_dirs) catch @panic("OOM");

        const arm_flags = arm_cpu_flags ++ arm_debug_flags ++ arm_dialect_flags ++ arm_warning_flags;
        for (crossSources(b)) |source| {
            candidates.append(.{
                .file = source,
                .driver = tools.gcc,
                .flags = &arm_flags,
                .include_dirs = include_dirs.items,
                .object = b.fmt("arm/{s}.o", .{std.fs.path.basename(source)}),
            }) catch @panic("OOM");
        }
    }

    var entries = std.ArrayList(CompileDbEntry).init(b.allocator);
    var seen = std.StringHashMap(void).init(b.allocator);
    for (candidates.items) |entry| {
        const signature = compileDbSignature(b, entry);
        if (seen.contains(signature)) continue;
        seen.put(signature, {}) catch @panic("OOM");
        entries.append(entry) catch @panic("OOM");
    }
    return entries.items;
}

pub fn appendJsonString(out: *std.ArrayList(u8), value: []const u8) void {
    out.append('"') catch @panic("OOM");
    for (value) |byte| switch (byte) {
        '"' => out.appendSlice("\\\"") catch @panic("OOM"),
        '\\' => out.appendSlice("\\\\") catch @panic("OOM"),
        '\n' => out.appendSlice("\\n") catch @panic("OOM"),
        '\t' => out.appendSlice("\\t") catch @panic("OOM"),
        else => out.append(byte) catch @panic("OOM"),
    };
    out.append('"') catch @panic("OOM");
}

/// Wire the database into `step` and hand back how many commands it carries,
/// so `zig build parity` can print the count without rebuilding the list.
fn addCompileDb(b: *std.Build, step: *std.Build.Step) usize {
    const entries = compileDbEntries(b);
    const directory = b.build_root.path orelse ".";

    var json = std.ArrayList(u8).init(b.allocator);
    json.appendSlice("[\n") catch @panic("OOM");
    for (entries, 0..) |entry, index| {
        json.appendSlice("  {\n    \"directory\": ") catch @panic("OOM");
        appendJsonString(&json, directory);
        json.appendSlice(",\n    \"file\": ") catch @panic("OOM");
        appendJsonString(&json, b.pathFromRoot(entry.file));
        json.appendSlice(",\n    \"output\": ") catch @panic("OOM");
        appendJsonString(&json, entry.object);
        json.appendSlice(",\n    \"arguments\": [") catch @panic("OOM");
        for (compileDbArguments(b, entry), 0..) |argument, argument_index| {
            if (argument_index != 0) json.appendSlice(", ") catch @panic("OOM");
            appendJsonString(&json, argument);
        }
        json.appendSlice("]\n  }") catch @panic("OOM");
        if (index + 1 != entries.len) json.append(',') catch @panic("OOM");
        json.append('\n') catch @panic("OOM");
    }
    json.appendSlice("]\n") catch @panic("OOM");

    const written = b.addWriteFiles();
    const database = written.add("compile_commands.json", json.items);
    const install = b.addInstallFileWithDir(
        database,
        .{ .custom = "analysis" },
        "compile_commands.json",
    );
    step.dependOn(&install.step);

    const report = b.addSystemCommand(&.{
        "printf",
        "compile-db: %s compile commands -> zig-out/analysis/compile_commands.json\n",
        b.fmt("{d}", .{entries.len}),
    });
    report.step.dependOn(&install.step);
    step.dependOn(&report.step);

    return entries.len;
}
