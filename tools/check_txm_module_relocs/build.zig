//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for `check_txm_module_relocs`, the host tool that holds a
//! linked ThreadX module to one rule: no data section may keep an absolute
//! relocation, because nothing rebases it when the module is loaded.
//!
//! `zig build` installs the tool and `zig build test` runs its tests, which
//! build their ELF fixtures in memory. The root build graph compiles
//! `src/main.zig` itself to run the tool on real module images.

const std = @import("std");
const ra8_build = @import("ra8_zig_build");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{
        .default_target = ra8_build.hostDefaultTargetQuery(b),
    });
    const optimize = b.standardOptimizeOption(.{});

    const exe = b.addExecutable(.{
        .name = "check_txm_module_relocs",
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/main.zig"),
            .target = target,
            .optimize = optimize,
        }),
    });
    b.installArtifact(exe);

    const checker = b.createModule(.{
        .root_source_file = b.path("src/root.zig"),
        .target = target,
        .optimize = optimize,
    });

    const test_step = b.step("test", "Run Zig check_txm_module_relocs tests");
    const roots = [_][]const u8{
        "tests/elf32_test.zig",
        "tests/check_test.zig",
        "tests/report_test.zig",
    };
    for (roots) |root| {
        const test_module = b.createModule(.{
            .root_source_file = b.path(root),
            .target = target,
            .optimize = optimize,
        });
        test_module.addImport("checker", checker);
        const tests = b.addTest(.{ .root_module = test_module });
        test_step.dependOn(&b.addRunArtifact(tests).step);
    }
}
