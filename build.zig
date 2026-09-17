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
const c_flags = [_][]const u8{
    "-std=c23",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Werror",
    "-DRA8_OFF_TARGET",
    "-DUNIT_TEST",
};

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const test_step = b.step("test", "Build and run the whole migrated-library slice");
    const c_test_step = b.step("test-c", "Run the unmodified C suites against the Zig archives");
    const zig_test_step = b.step("test-zig", "Run the Zig-native suites of the migrated libraries");
    test_step.dependOn(c_test_step);
    test_step.dependOn(zig_test_step);

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

    const parity_step = b.step("parity", "Print the slice manifest the CMake parity check reads");
    for (slice) |member| {
        const print = b.addSystemCommand(&.{ "printf", "%s\t%s\t%s\n" });
        print.addArg(member.artifact_name);
        print.addArg(member.include_path);
        print.addArg(member.c_suite_path);
        parity_step.dependOn(&print.step);
    }
}
