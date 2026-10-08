//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for `ra8_dfu_device`: the pure DFU device state, the USBX
//! declarations, and the C membrane that binds them to the `ra8_dfu`
//! programmer and the vendored USB stack.
//!
//! The archive reaches `ra8_dfu`, the HAL and USBX through their C ABIs, so
//! it builds against none of them: an app links all of them.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const device = b.createModule(.{
        .root_source_file = b.path("src/internal/device.zig"),
        .target = target,
        .optimize = optimize,
    });
    const usbx = b.createModule(.{
        .root_source_file = b.path("src/internal/usbx.zig"),
        .target = target,
        .optimize = optimize,
    });

    const abi = b.createModule(.{
        .root_source_file = b.path("src/device_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    abi.addImport("device", device);
    abi.addImport("usbx", usbx);

    const root_module = b.createModule(.{
        .root_source_file = b.path("src/root.zig"),
        .target = target,
        .optimize = optimize,
    });
    root_module.addImport("device_abi", abi);

    const library = b.addLibrary(.{
        .name = "ra8_dfu_device",
        .linkage = .static,
        .root_module = root_module,
    });
    // Host C test executables are linked by the system toolchain rather than
    // by `zig cc`, so nothing else on that link line provides Zig's runtime
    // helpers.
    // A cortex-m image links with -lgcc -lm; compiler_rt's libm there is
    // soft-float and would shadow newlib's hard-float one (RA8FW-943).
    library.bundle_compiler_rt = library.root_module.resolved_target.?.result.os.tag != .freestanding;
    b.installArtifact(library);

    const unit_test = b.createModule(.{
        .root_source_file = b.path("tests/device_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    unit_test.addImport("device", device);
    unit_test.addImport("usbx", usbx);
    const test_step = b.step("test", "Run Zig ra8_dfu_device tests");
    test_step.dependOn(&b.addRunArtifact(b.addTest(.{ .root_module = unit_test })).step);
}
