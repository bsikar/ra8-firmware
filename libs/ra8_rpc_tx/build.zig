//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for `ra8_rpc_tx`. Like `ra8_rpc` it is a Zig module and
//! nothing else: it exports no symbol, so there is no archive to install.
//! A consumer adds the `ra8_rpc_tx` module to its own root.
//!
//! The tests run on the host against fake ThreadX entry points. The real
//! ones are named in `src/kernel.zig` and resolve only in an image that
//! links the ThreadX kernel.

const std = @import("std");
const ra8_build = @import("ra8_zig_build");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{
        .default_target = ra8_build.hostDefaultTargetQuery(b),
    });
    const optimize = b.standardOptimizeOption(.{});

    const rpc = b.dependency("ra8_rpc", .{ .target = target, .optimize = optimize });
    const module = b.addModule("ra8_rpc_tx", .{
        .root_source_file = b.path("src/ra8_rpc_tx.zig"),
        .target = target,
        .optimize = optimize,
    });
    module.addImport("ra8_rpc", rpc.module("ra8_rpc"));

    const test_step = b.step("test", "Run Zig ra8_rpc_tx tests");

    const roots = [_][]const u8{
        "tests/queue_test.zig",
        "tests/session_test.zig",
        "tests/module_test.zig",
        "tests/module_session_test.zig",
    };
    for (roots) |root| {
        const test_module = b.createModule(.{
            .root_source_file = b.path(root),
            .target = target,
            .optimize = optimize,
        });
        test_module.addImport("ra8_rpc", rpc.module("ra8_rpc"));
        test_module.addImport("ra8_rpc_tx", module);
        const tests = b.addTest(.{ .root_module = test_module });
        test_step.dependOn(&b.addRunArtifact(tests).step);
    }
}
