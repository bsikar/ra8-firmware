//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for two host tools over a linked ThreadX module.
//!
//! `gen_txm_rebase_table` turns the data words that hold an absolute
//! address into the table the module's start-up rebases them from.
//! `check_txm_module_relocs` holds the finished module to one rule: every
//! such word is in that table, and the table names nothing else.
//!
//! `zig build` installs both and `zig build test` runs their tests, which
//! build their ELF fixtures in memory. The root build graph compiles
//! `src/main.zig` and `src/gen_main.zig` itself to run them on real images.

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

    const generator = b.addExecutable(.{
        .name = "gen_txm_rebase_table",
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/gen_main.zig"),
            .target = target,
            .optimize = optimize,
        }),
    });
    b.installArtifact(generator);

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
        "tests/layout_test.zig",
        "tests/records_test.zig",
        "tests/table_test.zig",
        "tests/coverage_test.zig",
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
