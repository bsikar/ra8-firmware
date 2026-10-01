//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig implementation of `ra8_tls`. CMake consumes the
//! installed static library through the unchanged `inc/ra8_tls.h`.
//!
//! One build option, `off-target`, carries the C's `RA8_OFF_TARGET` switch
//! and picks which backend the facade imports: the loopback stand-in in
//! `src/internal/backend_fake.zig`, or the real Mbed TLS binding in
//! `src/internal/backend_mbedtls.zig`. It defaults from the target, so a
//! hosted build gets the stand-in and a freestanding build gets Mbed TLS
//! without CMake passing anything.
//!
//! The on-target backend reaches the vendored headers through `@cImport`, so
//! the include roots and the two config-file defines below mirror what
//! `cmake/mbedtls.cmake` already gives the C compiler.

const std = @import("std");

const vendor_include_roots = [_][]const u8{
    "../third_party/mbedtls/include",
    "../third_party/tf-psa-crypto/include",
    "../third_party/tf-psa-crypto/drivers/builtin/include",
    "../../port/mbedtls/inc",
};

fn addVendorHeaders(b: *std.Build, module: *std.Build.Module) void {
    for (vendor_include_roots) |root| module.addIncludePath(b.path(root));
    module.addCMacro("TF_PSA_CRYPTO_CONFIG_FILE", "\"tf_psa_crypto_config.h\"");
    module.addCMacro("MBEDTLS_CONFIG_FILE", "\"mbedtls_config.h\"");
}

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const off_target = b.option(
        bool,
        "off-target",
        "Use the loopback transport stand-in instead of the vendored Mbed TLS",
    ) orelse (target.result.os.tag != .freestanding);

    const build_options = b.addOptions();
    build_options.addOption(bool, "off_target", off_target);

    const library_module = b.createModule(.{
        .root_source_file = b.path("src/root.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });
    library_module.addOptions("build_config", build_options);
    if (!off_target) addVendorHeaders(b, library_module);

    const library = b.addLibrary(.{
        .name = "ra8_tls",
        .linkage = .static,
        .root_module = library_module,
    });
    library.bundle_compiler_rt = true;
    library.root_module.pic = true;
    b.installArtifact(library);

    const test_step = b.step("test", "Run Zig ra8_tls tests");

    // One unit module per suite, and the suite reaches the vocabulary through
    // it, so no internal file is pulled into two modules of the same binary.
    // `needs_config` marks the one unit that crosses the backend switch.
    const suites = [_]struct {
        name: []const u8,
        source: []const u8,
        root: []const u8,
        needs_config: bool = false,
    }{
        .{ .name = "mss", .source = "src/internal/mss.zig", .root = "tests/mss_test.zig" },
        .{ .name = "cstr", .source = "src/internal/cstr.zig", .root = "tests/cstr_test.zig" },
        .{ .name = "pool", .source = "src/internal/pool.zig", .root = "tests/pool_test.zig" },
        .{
            .name = "backend_fake",
            .source = "src/internal/backend_fake.zig",
            .root = "tests/backend_fake_test.zig",
        },
        .{
            .name = "facade",
            .source = "src/internal/facade.zig",
            .root = "tests/facade_test.zig",
            .needs_config = true,
        },
    };
    for (suites) |suite| {
        const under_test = b.createModule(.{
            .root_source_file = b.path(suite.source),
            .target = target,
            .optimize = optimize,
        });
        if (suite.needs_config) under_test.addOptions("build_config", build_options);
        const test_module = b.createModule(.{
            .root_source_file = b.path(suite.root),
            .target = target,
            .optimize = optimize,
        });
        test_module.addImport(suite.name, under_test);
        const tests = b.addTest(.{ .root_module = test_module });
        test_step.dependOn(&b.addRunArtifact(tests).step);
    }
}
