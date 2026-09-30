//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig side of `ra8_mem`: the slab allocator, the page
//! cache, the byte-stream adapter over it, the glyph cache, and the
//! object-source registry. CMake consumes the installed static library through
//! the unchanged `inc/ra8_slab.h`, `inc/ra8_vmem.h`, `inc/ra8_vmem_stream.h`,
//! `inc/ra8_glyph_atlas.h` and `inc/ra8_vsource.h`.
//!
//! The page cache and the glyph atlas are typed facades over `ra8_keycache`,
//! which is still C, so the archive leaves the four `ra8_keycache_*` symbols
//! undefined and the link resolves them exactly as the C translation units
//! did. `ra8_vmem_get`/`ra8_vmem_put` are no longer among them: the stream
//! adapter now calls the Zig page cache in the same archive. The source
//! registry adds no extern of its own: a paged object's read callback is a
//! pointer it is handed, not a link-time symbol. The rest of `libs/ra8_mem`
//! (arena, keycache, tile cache) is still C and still built by CMake from
//! `src/`.

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
        .{
            .name = "vsource",
            .source = "src/internal/vsource.zig",
            .root = "tests/vsource_test.zig",
        },
        .{ .name = "vmem", .source = "src/internal/vmem.zig", .root = "tests/vmem_test.zig" },
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
