//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for `ra8_rpc`. The library is a Zig module and nothing else:
//! it exports no symbol and has no C ABI, so there is no archive to install.
//! A consumer adds the `ra8_rpc` module to its own root.
//!
//! No build options: the library is pure byte work with no target conditionals.

const std = @import("std");
const ra8_build = @import("ra8_zig_build");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{
        .default_target = ra8_build.hostDefaultTargetQuery(b),
    });
    const optimize = b.standardOptimizeOption(.{});

    const module = b.addModule("ra8_rpc", .{
        .root_source_file = b.path("src/ra8_rpc.zig"),
        .target = target,
        .optimize = optimize,
    });

    const test_step = b.step("test", "Run Zig ra8_rpc tests");

    // One root per concern, so a failure names the layer it belongs to.
    const roots = [_][]const u8{
        "tests/codec_test.zig",
        "tests/frame_test.zig",
        "tests/golden_test.zig",
        "tests/envelope_test.zig",
        "tests/loopback_test.zig",
        "tests/link_test.zig",
        "tests/pending_test.zig",
        "tests/session_test.zig",
        "tests/fuzz_test.zig",
    };
    for (roots) |root| {
        const test_module = b.createModule(.{
            .root_source_file = b.path(root),
            .target = target,
            .optimize = optimize,
        });
        test_module.addImport("ra8_rpc", module);
        const tests = b.addTest(.{ .root_module = test_module });
        test_step.dependOn(&b.addRunArtifact(tests).step);
    }
}
