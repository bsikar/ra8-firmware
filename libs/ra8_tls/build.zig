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
//! The on-target backend reaches the vendored headers through translate-c: the
//! build writes the include list and translates it into the `mbedtls_h`
//! module. The include roots and the two config-file defines below mirror what
//! `cmake/mbedtls.cmake` already gives the C compiler.

const std = @import("std");
const Translator = @import("translate_c").Translator;

/// The headers mbedtls_c.zig exposes; the build writes them, so no C is added.
const mbedtls_header =
    \\#include <mbedtls/error.h>
    \\#include <mbedtls/ssl.h>
    \\#include <mbedtls/x509_crt.h>
    \\#include <psa/crypto.h>
    \\
;

/// Package (pinned in build.zig.zon) and the include root inside it.
const VendorRoot = struct { package: []const u8, dir: []const u8 };

const vendor_include_roots = [_]VendorRoot{
    .{ .package = "mbedtls", .dir = "include" },
    .{ .package = "tf_psa_crypto", .dir = "include" },
    .{ .package = "tf_psa_crypto", .dir = "drivers/builtin/include" },
};

/// On the configure pass that first asks for a package zig fetches it and
/// runs the configure again, so a missing one is skipped here.
fn addMbedtlsHeaders(
    b: *std.Build,
    module: *std.Build.Module,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
) void {
    const header = b.addWriteFiles().add("mbedtls_c.h", mbedtls_header);
    const translator: Translator = .init(b.dependency("translate_c", .{}), .{
        .c_source_file = header,
        .target = target,
        .optimize = optimize,
        .link_libc = false,
    });
    for (vendor_include_roots) |root| {
        const dep = b.lazyDependency(root.package, .{}) orelse continue;
        translator.addIncludePath(dep.path(root.dir));
    }
    translator.addIncludePath(b.path("../../port/mbedtls/inc"));
    translator.defineCMacro("TF_PSA_CRYPTO_CONFIG_FILE", "\"tf_psa_crypto_config.h\"");
    translator.defineCMacro("MBEDTLS_CONFIG_FILE", "\"mbedtls_config.h\"");
    module.addImport("mbedtls_h", translator.mod);
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
    if (!off_target) addMbedtlsHeaders(b, library_module, target, optimize);

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
