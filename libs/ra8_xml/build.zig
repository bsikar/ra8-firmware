//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig implementation of `ra8_xml`. CMake consumes the
//! installed static library through the unchanged `inc/ra8_xml_writer.h` C ABI.
//!
//! No build options and no link-time seams: the emitter is pure string
//! construction over caller storage and the archive resolves every symbol it
//! names.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const library_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_xml_writer_abi.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });
    const library = b.addLibrary(.{
        .name = "ra8_xml",
        .linkage = .static,
        .root_module = library_module,
    });
    library.bundle_compiler_rt = true;
    library.root_module.pic = true;
    b.installArtifact(library);

    const test_step = b.step("test", "Run Zig ra8_xml tests");

    // Each file is tested through its own root, so a failure names the layer
    // it belongs to rather than the whole emitter.
    const units = [_]struct { name: []const u8, source: []const u8, root: []const u8 }{
        .{ .name = "entity", .source = "src/internal/entity.zig", .root = "tests/entity_test.zig" },
        .{ .name = "name", .source = "src/internal/name.zig", .root = "tests/name_test.zig" },
        .{ .name = "writer", .source = "src/internal/writer.zig", .root = "tests/writer_test.zig" },
        .{ .name = "abi", .source = "src/ra8_xml_writer_abi.zig", .root = "tests/abi_test.zig" },
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
