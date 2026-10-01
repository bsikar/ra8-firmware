//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for `ra8_dfu`.
//!
//! Three seams of this library are Zig: the polled USB-DFU host driver
//! (#2809), the pure boot logic the bootloader runs at reset (#2918), and the
//! root-of-trust image verifier (#2943). The rest of the library is still C,
//! which `.github/zig-parallel-tree-allowlist.tsv` records per file.
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
    .{ .name = "crc32", .imports = &.{} },
    .{ .name = "err", .imports = &.{} },
    .{ .name = "image", .imports = &.{} },
    .{ .name = "slot", .imports = &.{} },
    .{ .name = "proto", .imports = &.{} },
    .{ .name = "tune", .imports = &.{} },
    .{ .name = "hal", .imports = &.{"err"} },
    .{ .name = "control", .imports = &.{ "err", "hal", "proto", "tune" } },
    .{ .name = "attach", .imports = &.{ "control", "err", "hal", "proto", "tune" } },
    .{ .name = "status", .imports = &.{ "err", "hal", "proto", "tune" } },
    .{ .name = "download", .imports = &.{ "err", "hal", "proto", "status" } },
    .{ .name = "verify", .imports = &.{ "download", "err", "hal", "proto" } },
    .{ .name = "session", .imports = &.{ "attach", "control", "download", "err", "hal", "proto", "verify" } },
    .{ .name = "rot", .imports = &.{} },
    .{ .name = "antirollback", .imports = &.{} },
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

    const boot_abi_module = b.createModule(.{
        .root_source_file = b.path("src/dfu_boot_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    inline for (.{ "crc32", "image", "slot" }) |name| {
        boot_abi_module.addImport(name, modules.get(name).?);
    }

    const root_module = b.createModule(.{
        .root_source_file = b.path("src/root.zig"),
        .target = target,
        .optimize = optimize,
    });
    root_module.addImport("dfu_host_abi", abi_module);
    root_module.addImport("dfu_boot_abi", boot_abi_module);

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

    // The boot logic on its own, for links that cannot resolve the host
    // driver's `ra8_usb_host_*` seam. See src/boot_root.zig.
    const boot_root_module = b.createModule(.{
        .root_source_file = b.path("src/boot_root.zig"),
        .target = target,
        .optimize = optimize,
    });
    boot_root_module.addImport("dfu_boot_abi", boot_abi_module);
    const boot_library = b.addLibrary(.{
        .name = "ra8_dfu_boot",
        .linkage = .static,
        .root_module = boot_root_module,
    });
    boot_library.bundle_compiler_rt = true;
    b.installArtifact(boot_library);

    // The root of trust on its own. `ra8_rot.c` compiled to an empty
    // translation unit unless the app defined `RA8_ENABLE_ROOT_OF_TRUST`; a
    // prebuilt archive cannot see that definition, so the opt-in is which
    // apps link this artifact. See src/rot_root.zig.
    const rot_abi_module = b.createModule(.{
        .root_source_file = b.path("src/rot_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    rot_abi_module.addImport("rot", modules.get("rot").?);

    const rot_root_module = b.createModule(.{
        .root_source_file = b.path("src/rot_root.zig"),
        .target = target,
        .optimize = optimize,
    });
    const antirollback_abi_module = b.createModule(.{
        .root_source_file = b.path("src/antirollback_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    antirollback_abi_module.addImport("antirollback", modules.get("antirollback").?);

    rot_root_module.addImport("rot_abi", rot_abi_module);
    rot_root_module.addImport("antirollback_abi", antirollback_abi_module);
    const rot_library = b.addLibrary(.{
        .name = "ra8_rot",
        .linkage = .static,
        .root_module = rot_root_module,
    });
    rot_library.bundle_compiler_rt = true;
    b.installArtifact(rot_library);

    const test_module = b.createModule(.{
        .root_source_file = b.path("tests/dfu_host_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    inline for (units) |unit| {
        test_module.addImport(unit.name, modules.get(unit.name).?);
    }

    const boot_test_module = b.createModule(.{
        .root_source_file = b.path("tests/boot_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    inline for (.{ "crc32", "image", "slot" }) |name| {
        boot_test_module.addImport(name, modules.get(name).?);
    }

    const rot_test_module = b.createModule(.{
        .root_source_file = b.path("tests/rot_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    rot_test_module.addImport("rot", modules.get("rot").?);

    const antirollback_test_module = b.createModule(.{
        .root_source_file = b.path("tests/antirollback_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    antirollback_test_module.addImport("antirollback", modules.get("antirollback").?);

    const test_step = b.step("test", "Run Zig ra8_dfu tests");
    const tests = b.addTest(.{ .root_module = test_module });
    test_step.dependOn(&b.addRunArtifact(tests).step);
    const boot_tests = b.addTest(.{ .root_module = boot_test_module });
    test_step.dependOn(&b.addRunArtifact(boot_tests).step);
    const rot_tests = b.addTest(.{ .root_module = rot_test_module });
    test_step.dependOn(&b.addRunArtifact(rot_tests).step);
    const antirollback_tests = b.addTest(.{ .root_module = antirollback_test_module });
    test_step.dependOn(&b.addRunArtifact(antirollback_tests).step);
}
