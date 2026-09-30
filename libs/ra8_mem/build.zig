//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig implementation of the `ra8_mem` slab. CMake
//! consumes the installed static library through the unchanged
//! `inc/ra8_slab.h`.
//!
//! Only the slab lives here. The rest of `libs/ra8_mem` (arena, keycache, tile
//! cache, vmem, glyph atlas, vmem stream, vsource) is still C and still built
//! by CMake from `src/`.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const library_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_mem_abi.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });

    const library = b.addLibrary(.{
        .name = "ra8_mem",
        .linkage = .static,
        .root_module = library_module,
    });
    library.bundle_compiler_rt = true;
    library.root_module.pic = true;
    b.installArtifact(library);

    const test_step = b.step("test", "Run Zig ra8_mem tests");

    const units = [_]struct { name: []const u8, source: []const u8, root: []const u8 }{
        .{ .name = "vocab", .source = "src/internal/vocab.zig", .root = "tests/vocab_test.zig" },
        .{ .name = "slab", .source = "src/internal/slab.zig", .root = "tests/slab_test.zig" },
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
