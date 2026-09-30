//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for `ra8_dfu`.
//!
//! One seam of this library is Zig so far: the polled USB-DFU host driver
//! (#2809). The rest of the library is still C, which
//! `.github/zig-parallel-tree-allowlist.tsv` records per file.
//!
//! Each unit is its own module so the tests can import the same module
//! objects the archive does. That is what lets the tests drive the DFU
//! sequence against a scripted device without linking a USB controller:
//! the step modules are generic over their HAL, and only
//! `dfu_host_abi` binds the real one.

const std = @import("std");

/// Every unit under `src/internal/`, in dependency order, with the units it
/// imports. Adding a unit means adding one row here.
const units = [_]struct { name: []const u8, imports: []const []const u8 }{
    .{ .name = "err", .imports = &.{} },
    .{ .name = "proto", .imports = &.{} },
    .{ .name = "tune", .imports = &.{} },
    .{ .name = "hal", .imports = &.{"err"} },
    .{ .name = "control", .imports = &.{ "err", "hal", "proto", "tune" } },
    .{ .name = "attach", .imports = &.{ "control", "err", "hal", "proto", "tune" } },
    .{ .name = "status", .imports = &.{ "err", "hal", "proto", "tune" } },
    .{ .name = "download", .imports = &.{ "err", "hal", "proto", "status" } },
    .{ .name = "verify", .imports = &.{ "download", "err", "hal", "proto" } },
    .{ .name = "session", .imports = &.{ "attach", "control", "download", "err", "hal", "proto", "verify" } },
};

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    var modules = std.StringHashMap(*std.Build.Module).init(b.allocator);

    inline for (units) |unit| {
        const module = b.createModule(.{
            .root_source_file = b.path(b.fmt("src/internal/{s}.zig", .{unit.name})),
            .target = target,
            .optimize = optimize,
        });
        for (unit.imports) |dependency| {
            module.addImport(dependency, modules.get(dependency).?);
        }
        modules.put(unit.name, module) catch @panic("OOM");
    }

    const abi_module = b.createModule(.{
        .root_source_file = b.path("src/dfu_host_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    inline for (.{ "err", "hal", "proto", "session" }) |name| {
        abi_module.addImport(name, modules.get(name).?);
    }

    const root_module = b.createModule(.{
        .root_source_file = b.path("src/root.zig"),
        .target = target,
        .optimize = optimize,
    });
    root_module.addImport("dfu_host_abi", abi_module);

    const library = b.addLibrary(.{
        .name = "ra8_dfu",
        .linkage = .static,
        .root_module = root_module,
    });
    // The host C test executables are linked by the system toolchain rather
    // than by `zig cc`, so nothing else on that link line provides Zig's
    // runtime helpers.
    library.bundle_compiler_rt = true;
    b.installArtifact(library);

    const test_module = b.createModule(.{
        .root_source_file = b.path("tests/dfu_host_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    inline for (units) |unit| {
        test_module.addImport(unit.name, modules.get(unit.name).?);
    }

    const test_step = b.step("test", "Run Zig ra8_dfu tests");
    const tests = b.addTest(.{ .root_module = test_module });
    test_step.dependOn(&b.addRunArtifact(tests).step);
}
