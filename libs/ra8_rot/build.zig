//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for `ra8_rot`, the root of trust: image verification
//! (SHA-256 + ECDSA-P256 against the provisioned root public key) and the
//! anti-rollback counter behind it.
//!
//! The library is Zig only; it has no C implementation. It lived inside
//! `ra8_dfu` while it was being ported, which left its archive
//! unreachable: `cmake/ra8_app/sources.cmake` resolves a `LIBS` entry to
//! `libs/<name>`, so the four apps that declare `LIBS ra8_rot` resolved
//! nothing and linked no root of trust at all. Its own directory is what
//! makes those declarations true.
//!
//! Each unit under `src/internal/` is a module the tests import directly, so
//! the decision tables are driven without an engine behind them.

const std = @import("std");

/// Every unit under `src/internal/`, with the exported ABI surface that wraps
/// it and the test that drives it. Adding a unit means adding one row.
const units = [_]struct {
    name: []const u8,
    abi_source: []const u8,
    test_source: []const u8,
}{
    .{
        .name = "rot",
        .abi_source = "src/rot_abi.zig",
        .test_source = "tests/rot_test.zig",
    },
    .{
        .name = "antirollback",
        .abi_source = "src/antirollback_abi.zig",
        .test_source = "tests/antirollback_test.zig",
    },
};

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const root_module = b.createModule(.{
        .root_source_file = b.path("src/root.zig"),
        .target = target,
        .optimize = optimize,
    });

    const test_step = b.step("test", "Run Zig ra8_rot tests");

    inline for (units) |unit| {
        const logic = b.createModule(.{
            .root_source_file = b.path(b.fmt("src/internal/{s}.zig", .{unit.name})),
            .target = target,
            .optimize = optimize,
        });

        const abi = b.createModule(.{
            .root_source_file = b.path(unit.abi_source),
            .target = target,
            .optimize = optimize,
        });
        abi.addImport(unit.name, logic);
        root_module.addImport(b.fmt("{s}_abi", .{unit.name}), abi);

        const unit_test = b.createModule(.{
            .root_source_file = b.path(unit.test_source),
            .target = target,
            .optimize = optimize,
        });
        unit_test.addImport(unit.name, logic);
        const tests = b.addTest(.{ .root_module = unit_test });
        test_step.dependOn(&b.addRunArtifact(tests).step);
    }

    const library = b.addLibrary(.{
        .name = "ra8_rot",
        .linkage = .static,
        .root_module = root_module,
    });
    // The host C test executables are linked by the system toolchain rather
    // than by `zig cc`, so nothing else on that link line provides Zig's
    // runtime helpers.
    library.bundle_compiler_rt = true;
    b.installArtifact(library);
}
