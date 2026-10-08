//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for `ra8_rot_launch`: one unit, the default-deny rule, and
//! the C membrane that sequences the gates in front of the copy-to-run
//! hand-off.
//!
//! The archive calls into both `ra8_rot` (trailer, signature, anti-rollback)
//! and `ra8_dfu` (the unauthenticated hand-off) through their C ABIs, so it
//! builds against neither: an app links all three.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const logic = b.createModule(.{
        .root_source_file = b.path("src/internal/launch_gate.zig"),
        .target = target,
        .optimize = optimize,
    });

    const abi = b.createModule(.{
        .root_source_file = b.path("src/launch_gate_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    abi.addImport("launch_gate", logic);

    const root_module = b.createModule(.{
        .root_source_file = b.path("src/root.zig"),
        .target = target,
        .optimize = optimize,
    });
    root_module.addImport("launch_gate_abi", abi);

    const library = b.addLibrary(.{
        .name = "ra8_rot_launch",
        .linkage = .static,
        .root_module = root_module,
    });
    // The host C test executables are linked by the system toolchain rather
    // than by `zig cc`, so nothing else on that link line provides Zig's
    // runtime helpers.
    // A cortex-m image links with -lgcc -lm; compiler_rt's libm there is
    // soft-float and would shadow newlib's hard-float one (RA8FW-943).
    library.bundle_compiler_rt = library.root_module.resolved_target.?.result.os.tag != .freestanding;
    b.installArtifact(library);

    const unit_test = b.createModule(.{
        .root_source_file = b.path("tests/launch_gate_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    unit_test.addImport("launch_gate", logic);
    const test_step = b.step("test", "Run Zig ra8_rot_launch tests");
    test_step.dependOn(&b.addRunArtifact(b.addTest(.{ .root_module = unit_test })).step);
}
