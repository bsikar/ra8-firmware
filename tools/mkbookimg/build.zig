//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! mkbookimg -- build a FAT32 SD-card image carrying compiled .rabook files,
//! written through the real ra8_fs so the firmware reads them back
//! bit-for-bit.
//!
//!   zig build --prefix build
//!   ./build/bin/mkbookimg books.img a.rabook b.rabook ...
//!
//! RA8FW-999: built through the Zig graph like every other host tool; the
//! C-era build linked the ra8_imgdec, ra8_mem and ra8_fs Zig archives and
//! compiled the ra8_fs fat TUs by path (the library lock is Zig, the fat
//! implementation stays C).

const std = @import("std");
const ra8_build = @import("ra8_zig_build");

// First-party sources carry the project warning bar (NASA Power of 10 Rule
// 10) exactly as they do in the ARM and host-test builds.
const first_party_flags = [_][]const u8{ "-std=gnu23", "-Wall", "-Wextra", "-Werror" };

// The tool is split across two TUs (file-size gate): the host container plus
// entry point, and the population pass that streams and verifies each book.
// Every ra8_fs TU is Zig (RA8FW-1138); the tool links the ra8_fs archive
// and nothing else.

const tool_sources = [_][]const u8{
    "src/mkbookimg.c",
    "src/mkbookimg_books.c",
};

fn addC(b: *std.Build, mod: *std.Build.Module, root: []const u8, files: []const []const u8, flags: []const []const u8) void {
    mod.addCSourceFiles(.{ .root = b.path(root), .files = files, .flags = flags });
}

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{ .default_target = ra8_build.hostDefaultTargetQuery(b) });
    // The tool writes 64 MiB images, so the default build optimizes like the
    // C-era tool build did; an explicit -Doptimize still wins.
    const optimize = b.standardOptimizeOption(.{ .preferred_optimize_mode = .ReleaseFast });

    // The libraries the C-era recipe linked as Zig archives.
    const core_lib = b.dependency("ra8_core", .{ .target = target, .optimize = optimize }).artifact("ra8_core");
    const imgdec_lib = b.dependency("ra8_imgdec", .{ .target = target, .optimize = optimize }).artifact("ra8_imgdec");
    const mem_lib = b.dependency("ra8_mem", .{ .target = target, .optimize = optimize }).artifact("ra8_mem");
    const fs_lib = b.dependency("ra8_fs", .{ .target = target, .optimize = optimize }).artifact("ra8_fs");

    const mod = b.createModule(.{ .target = target, .optimize = optimize });
    mod.link_libc = true;
    mod.linkLibrary(imgdec_lib);
    mod.linkLibrary(mem_lib);
    // The library lock is Zig (RA8FW-724): the fat TUs below stopped defining
    // ra8_fs_set_lock / priv_lock_*, so the archive rides beside them.
    mod.linkLibrary(fs_lib);
    mod.addIncludePath(core_lib.getEmittedIncludeTree());
    mod.addIncludePath(b.path("inc"));
    addC(b, mod, ".", &tool_sources, &first_party_flags);
    const exe = b.addExecutable(.{ .name = "mkbookimg", .root_module = mod });
    b.installArtifact(exe);

    // ---- the C-era integration suite, driven against the built exe ----
    // The C-era add_test handed the script "$(cmake)" for its sha256sum call;
    // the workspace's wrapper maps "-E sha256sum" onto coreutils sha256sum,
    // which is the only thing the script used cmake for.
    const integration_step = b.step("integration", "Run the mkbookimg integration suite");
    const cmd = b.addSystemCommand(&.{ "bash", "tests/integration.sh" });
    cmd.setCwd(b.path("."));
    cmd.addArtifactArg(exe);
    cmd.addArg("tests/cmake_sha256sum.sh");
    integration_step.dependOn(&cmd.step);

    const test_step = b.step("test", "Run the mkbookimg contract tests");
    const test_options = b.addOptions();
    test_options.addOptionPath("tool_exe", exe.getEmittedBin());
    const ztest_mod = b.createModule(.{
        .root_source_file = b.path("tests/mkbookimg_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    ztest_mod.addOptions("tool_options", test_options);
    const ztest = b.addTest(.{ .root_module = ztest_mod });
    const run_ztest = b.addRunArtifact(ztest);
    ra8_build.configureHostTestRun(b, test_step, ztest, run_ztest);
    test_step.dependOn(&run_ztest.step);
}
