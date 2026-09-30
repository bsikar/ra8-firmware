//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for `ra8_jpeg`.
//!
//! Two of the library's seams are Zig: the `ra8_imgdec` backend (#2786) and
//! the baseline encoder (#2795). The decoder half (the marker walk, the
//! whole-buffer decoder and the striped driver) is still C behind
//! `src/ra8_jpeg_sw_internal.h` and ports in a later slice, so the archive's
//! root is `src/root.zig`, which exists only to pull both membranes in.
//!
//! The encoder's decision units are declared as modules here and imported by
//! name rather than by relative path, so the same declarations wire both the
//! archive and the tests and no file belongs to two modules.

const std = @import("std");

/// The encoder's decision units, each with the units it depends on. Declared
/// bottom-up so a dependency is always already built when it is wired.
const internal_modules = [_]struct { name: []const u8, deps: []const []const u8 }{
    .{ .name = "spec", .deps = &.{} },
    .{ .name = "dct", .deps = &.{"spec"} },
    .{ .name = "quant", .deps = &.{"spec"} },
    .{ .name = "color", .deps = &.{"spec"} },
    .{ .name = "sampling", .deps = &.{"spec"} },
    .{ .name = "huffman", .deps = &.{"spec"} },
    .{ .name = "sink", .deps = &.{"spec"} },
    .{ .name = "headers", .deps = &.{ "spec", "huffman", "sink" } },
    .{ .name = "entropy", .deps = &.{ "spec", "dct", "quant", "huffman", "sink" } },
};

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    var built = std.StringHashMap(*std.Build.Module).init(b.allocator);
    for (internal_modules) |entry| {
        const module = b.createModule(.{
            .root_source_file = b.path(b.fmt("src/internal/{s}.zig", .{entry.name})),
            .target = target,
            .optimize = optimize,
        });
        for (entry.deps) |dep| module.addImport(dep, built.get(dep).?);
        built.put(entry.name, module) catch @panic("OOM");
    }

    const root_module = b.createModule(.{
        .root_source_file = b.path("src/root.zig"),
        .target = target,
        .optimize = optimize,
    });
    for (internal_modules) |entry| root_module.addImport(entry.name, built.get(entry.name).?);

    const library = b.addLibrary(.{
        .name = "ra8_jpeg",
        .linkage = .static,
        .root_module = root_module,
    });
    // The host C test executables are linked by the system toolchain, not by
    // `zig cc`, so nothing else on that link line provides Zig's runtime
    // helpers. Without this the archive leaves `__zig_probe_stack` undefined.
    library.bundle_compiler_rt = true;
    b.installArtifact(library);

    const test_step = b.step("test", "Run Zig ra8_jpeg tests");

    const policy_module = b.createModule(.{
        .root_source_file = b.path("src/internal/imgdec.zig"),
        .target = target,
        .optimize = optimize,
    });
    const imgdec_test_module = b.createModule(.{
        .root_source_file = b.path("tests/imgdec_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    imgdec_test_module.addImport("policy", policy_module);
    const imgdec_tests = b.addTest(.{ .root_module = imgdec_test_module });
    test_step.dependOn(&b.addRunArtifact(imgdec_tests).step);

    const encoder_test_module = b.createModule(.{
        .root_source_file = b.path("tests/encode_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    for ([_][]const u8{ "spec", "dct", "quant", "huffman", "sink", "entropy" }) |name| {
        encoder_test_module.addImport(name, built.get(name).?);
    }
    const encoder_tests = b.addTest(.{ .root_module = encoder_test_module });
    test_step.dependOn(&b.addRunArtifact(encoder_tests).step);
}
