//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig side of `ra8_mem`: the slab allocator, the
//! byte-stream adapter over the page cache, and the glyph cache. CMake consumes
//! the installed static library through the unchanged `inc/ra8_slab.h`,
//! `inc/ra8_vmem_stream.h` and `inc/ra8_glyph_atlas.h`.
//!
//! The stream adapter calls `ra8_vmem_get`/`ra8_vmem_put`, which are still C.
//! The archive leaves those two undefined and the link resolves them, exactly
//! as the C translation unit did. The glyph atlas does the same with the four
//! `ra8_keycache_*` symbols. The rest of `libs/ra8_mem` (arena, keycache, tile
//! cache, vmem, vsource) is still C and still built by CMake from `src/`.

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
        .{
            .name = "vmem_stream",
            .source = "src/internal/vmem_stream.zig",
            .root = "tests/vmem_stream_test.zig",
        },
        .{
            .name = "glyph_atlas",
            .source = "src/internal/glyph_atlas.zig",
            .root = "tests/glyph_atlas_test.zig",
        },
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
