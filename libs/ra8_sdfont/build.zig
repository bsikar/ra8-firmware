//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig implementation of `ra8_sdfont`. CMake consumes the
//! installed static library through the unchanged `inc/ra8_sdfont.h` C ABI.
//!
//! The archive leaves the pin, SPI, SD and filesystem symbols unresolved: they
//! are link-time seams satisfied by the ring below, exactly as the C was.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const library_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_sdfont_abi.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });
    const library = b.addLibrary(.{
        .name = "ra8_sdfont",
        .linkage = .static,
        .root_module = library_module,
    });
    // A cortex-m image links with -lgcc -lm; compiler_rt's libm there is
    // soft-float and would shadow newlib's hard-float one (RA8FW-943).
    library.bundle_compiler_rt = library.root_module.resolved_target.?.result.os.tag != .freestanding;
    library.root_module.pic = true;
    b.installArtifact(library);

    const test_step = b.step("test", "Run Zig ra8_sdfont tests");

    // The membrane and the bus half name pin, SPI, SD and filesystem symbols
    // that only resolve in a firmware link, so the host suite covers the two
    // files that have no externs: the policy and the published layouts.
    const units = [_]struct { name: []const u8, source: []const u8, root: []const u8 }{
        .{ .name = "policy", .source = "src/internal/policy.zig", .root = "tests/policy_test.zig" },
        .{ .name = "abi_types", .source = "src/internal/abi_types.zig", .root = "tests/abi_test.zig" },
    };
    for (units) |unit| {
        const under_test = b.createModule(.{
            .root_source_file = b.path(unit.source),
            .target = target,
            .optimize = optimize,
        });
        const test_module = b.createModule(.{
            .root_source_file = b.path(unit.root),
            .target = target,
            .optimize = optimize,
        });
        test_module.addImport(unit.name, under_test);
        const tests = b.addTest(.{ .root_module = test_module });
        test_step.dependOn(&b.addRunArtifact(tests).step);
    }
}
