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

// The whole FAT/exFAT driver -- and ONLY the driver (no ra8_core_hal, so no
// MMIO and no ra8_time weak-extern), which is what lets this build and RUN on
// macOS where the unit-test harness cannot. Mirrors the same glob in
// tests/cmake/tests_storage.cmake (test_ra8_fs_exfat).
const fat_tus = [_][]const u8{
    "ra8_fs_fat.c",
    "ra8_fs_fat_alloc.c",
    "ra8_fs_fat_check.c",
    "ra8_fs_fat_dir.c",
    "ra8_fs_fat_dirmk.c",
    "ra8_fs_fat_exfat_check.c",
    "ra8_fs_fat_exfat_dir.c",
    "ra8_fs_fat_exfat_fmt.c",
    "ra8_fs_fat_exfat_mutate.c",
    "ra8_fs_fat_exfat_read.c",
    "ra8_fs_fat_exfat_stream.c",
    "ra8_fs_fat_exfat_write.c",
    "ra8_fs_fat_file.c",
    "ra8_fs_fat_fileio.c",
    "ra8_fs_fat_fmt.c",
    "ra8_fs_fat_label.c",
    "ra8_fs_fat_lfn.c",
    "ra8_fs_fat_lfn_write.c",
    "ra8_fs_fat_mount.c",
    "ra8_fs_fat_name.c",
    "ra8_fs_fat_stat.c",
    "ra8_fs_fat_time.c",
    "ra8_fs_fat_truncate.c",
};

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    // The UTF codec and the library lock are Zig now (RA8FW-747, RA8FW-724);
    // they come from the ra8_fs archive beside the driver TUs below.
    const fs_dep = b.dependency("ra8_fs", .{ .target = target, .optimize = optimize });
    const fs_lib = fs_dep.artifact("ra8_fs");

    const mod = b.createModule(.{ .target = target, .optimize = optimize });
    mod.link_libc = true;
    mod.linkLibrary(fs_lib);
    mod.addIncludePath(b.path("../../libs/ra8_fs/inc"));
    mod.addIncludePath(b.path("../../libs/ra8_core/inc"));
    mod.addCSourceFiles(.{
        .root = b.path("src"),
        .files = &.{"exfat_mkimage.c"},
        .flags = &.{ "-std=gnu23", "-Wall", "-Wextra", "-Werror" },
    });
    mod.addCSourceFiles(.{
        .root = b.path("../../libs/ra8_fs/src"),
        .files = &fat_tus,
        .flags = &.{ "-std=gnu23", "-Wall", "-Wextra", "-Werror" },
    });
    const exe = b.addExecutable(.{ .name = "exfat_mkimage", .root_module = mod });

    b.installArtifact(exe);

    const integration = b.addSystemCommand(&.{ "bash", "tests/integration.sh" });
    integration.addArtifactArg(exe);
    integration.expectExitCode(0);
    const test_step = b.step("test", "Run the exfat_mkimage integration suite");
    test_step.dependOn(&integration.step);
}
