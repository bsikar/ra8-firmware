//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig implementation of `ra8_net_policy`. CMake consumes
//! the installed static library through the unchanged `inc/ra8_net_urlguard.h`
//! C ABI.
//!
//! The archive leaves nothing unresolved: this library is pure lexical and
//! numeric policy over caller-owned storage, with no ring below it, so the
//! whole of it is covered by the host suite.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const library_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_net_urlguard_abi.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });
    const library = b.addLibrary(.{
        .name = "ra8_net_policy",
        .linkage = .static,
        .root_module = library_module,
    });
    // A cortex-m image links with -lgcc -lm; compiler_rt's libm there is
    // soft-float and would shadow newlib's hard-float one (RA8FW-943).
    library.bundle_compiler_rt = library.root_module.resolved_target.?.result.os.tag != .freestanding;
    library.root_module.pic = true;
    b.installArtifact(library);

    const test_step = b.step("test", "Run Zig ra8_net_policy tests");

    const units = [_]struct { name: []const u8, source: []const u8, root: []const u8 }{
        .{ .name = "policy", .source = "src/internal/policy.zig", .root = "tests/addr_test.zig" },
        .{ .name = "policy", .source = "src/internal/policy.zig", .root = "tests/policy_test.zig" },
        .{ .name = "url_policy", .source = "src/internal/url.zig", .root = "tests/url_test.zig" },
        .{ .name = "vocab", .source = "src/internal/root.zig", .root = "tests/abi_test.zig" },
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
