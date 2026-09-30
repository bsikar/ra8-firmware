//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig implementation of `ra8_num`. CMake consumes the
//! installed static library through the unchanged `inc/ra8_num.h` C ABI.
//!
//! No build options and no link-time seams: the library is a pure computation
//! and the archive resolves every symbol it names.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const library_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_num_abi.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });
    const library = b.addLibrary(.{
        .name = "ra8_num",
        .linkage = .static,
        .root_module = library_module,
    });
    library.bundle_compiler_rt = true;
    library.root_module.pic = true;
    b.installArtifact(library);

    const test_step = b.step("test", "Run Zig ra8_num tests");

    // Each internal file is tested through its own root, so a failure names
    // the layer it belongs to rather than the whole conversion.
    const units = [_]struct { name: []const u8, source: []const u8, root: []const u8 }{
        .{ .name = "big", .source = "src/internal/big.zig", .root = "tests/big_test.zig" },
        .{ .name = "binary64", .source = "src/internal/binary64.zig", .root = "tests/binary64_test.zig" },
        .{ .name = "decimal", .source = "src/internal/decimal.zig", .root = "tests/decimal_test.zig" },
        .{ .name = "abi", .source = "src/ra8_num_abi.zig", .root = "tests/abi_test.zig" },
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
