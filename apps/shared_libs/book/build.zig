//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig book archive (RA8FW-1242, RA8FW-1244,
//! RA8FW-1248, RA8FW-1249):
//! src/book_root.zig aggregates the ported TUs (book_stream_wire.zig, then
//! book_chunked.zig) so their C ABI exports land in the installed static
//! library. CMake consumes it through those symbol names; the remaining
//! book C TUs call them unchanged.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const library_module = b.createModule(.{
        .root_source_file = b.path("src/book_root.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });
    const library = b.addLibrary(.{
        .name = "ra8_book",
        .linkage = .static,
        .root_module = library_module,
    });
    library.bundle_compiler_rt = library.root_module.resolved_target.?.result.os.tag != .freestanding;
    library.root_module.pic = true;
    b.installArtifact(library);

    const test_step = b.step("test", "Run Zig book archive tests");
    const test_module = b.createModule(.{
        .root_source_file = b.path("src/book_stream_wire_test.zig"),
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    const book_wire_test = b.addTest(.{ .root_module = test_module });
    test_step.dependOn(&b.addRunArtifact(book_wire_test).step);

    const chunked_test_module = b.createModule(.{
        .root_source_file = b.path("src/book_chunked_test.zig"),
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    const book_chunked_test = b.addTest(.{ .root_module = chunked_test_module });
    test_step.dependOn(&b.addRunArtifact(book_chunked_test).step);

    const validate_test_module = b.createModule(.{
        .root_source_file = b.path("src/book_chunked_validate_test.zig"),
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    const book_validate_test = b.addTest(.{ .root_module = validate_test_module });
    test_step.dependOn(&b.addRunArtifact(book_validate_test).step);

    const stream_test_module = b.createModule(.{
        .root_source_file = b.path("src/book_stream_test.zig"),
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    const book_stream_test = b.addTest(.{ .root_module = stream_test_module });
    test_step.dependOn(&b.addRunArtifact(book_stream_test).step);

    const paged_test_module = b.createModule(.{
        .root_source_file = b.path("src/book_paged_test.zig"),
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    const book_paged_test = b.addTest(.{ .root_module = paged_test_module });
    test_step.dependOn(&b.addRunArtifact(book_paged_test).step);
}
