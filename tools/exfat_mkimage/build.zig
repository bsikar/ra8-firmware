//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! exfat_mkimage -- build an exFAT volume image through the real ra8_fs, so a
//! host operating system can be asked to mount it and DISPLAY the names. That
//! is the one question our own byte assertions cannot answer, and it is why
//! scripts/dev/exfat_macos_interop.sh drives this tool rather than borrowing
//! images from the unit-test binaries on another machine.
//!
//!   zig build --prefix build
//!   ./build/bin/exfat_mkimage /tmp/showcase.img
//!
//! Standalone host dev tool under tools/, outside the firmware cross-build.
//! RA8FW-999: built through the Zig graph like every other host tool; the
//! C-era CMake build this replaces compiled the same driver subset with the
//! same warning bar.

const std = @import("std");
const ra8_build = @import("ra8_zig_build");

// The whole FAT/exFAT driver -- and ONLY the driver (no ra8_core_hal, so no
// MMIO and no ra8_time weak-extern), which is what lets this build and RUN on
// macOS where the unit-test harness cannot. The exFAT host test in tests/cmake/tests_storage.cmake links the same archive.

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{ .default_target = ra8_build.hostDefaultTargetQuery(b) });
    const optimize = b.standardOptimizeOption(.{});

    // The UTF codec and the library lock are Zig now (RA8FW-747, RA8FW-724);
    // they come from the ra8_fs archive, which is all the tool links now that every
    // driver TU is Zig (RA8FW-1138).
    const core_lib = b.dependency("ra8_core", .{ .target = target, .optimize = optimize }).artifact("ra8_core");
    const fs_dep = b.dependency("ra8_fs", .{ .target = target, .optimize = optimize });
    const fs_lib = fs_dep.artifact("ra8_fs");

    const mod = b.createModule(.{ .target = target, .optimize = optimize });
    mod.link_libc = true;
    mod.linkLibrary(fs_lib);
    mod.addIncludePath(core_lib.getEmittedIncludeTree());
    mod.addCSourceFiles(.{
        .root = b.path("src"),
        .files = &.{"exfat_mkimage.c"},
        .flags = &.{ "-std=gnu23", "-Wall", "-Wextra", "-Werror" },
    });
    const exe = b.addExecutable(.{ .name = "exfat_mkimage", .root_module = mod });

    b.installArtifact(exe);

    const integration = b.addSystemCommand(&.{ "bash", "tests/integration.sh" });
    integration.setCwd(b.path("."));
    integration.addArtifactArg(exe);
    integration.expectExitCode(0);
    const test_step = b.step("test", "Run the exfat_mkimage integration suite");
    test_step.dependOn(&integration.step);

    const test_options = b.addOptions();
    test_options.addOptionPath("tool_exe", exe.getEmittedBin());
    const ztest_mod = b.createModule(.{
        .root_source_file = b.path("tests/exfat_mkimage_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    ztest_mod.addOptions("tool_options", test_options);
    const ztest = b.addTest(.{ .root_module = ztest_mod });
    const run_ztest = b.addRunArtifact(ztest);
    ra8_build.configureHostTestRun(b, test_step, ztest, run_ztest);
    test_step.dependOn(&run_ztest.step);
}
