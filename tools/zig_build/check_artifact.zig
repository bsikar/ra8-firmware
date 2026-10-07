//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The two RA8FW-330 checks that read a build output: the emitted macOS
//! image (`host-artifact`) and a Rust archive about to be linked
//! (`archive`). `check.zig` parses the command line and calls these; each
//! failure exits non-zero with the reason, which fails the build step.

const std = @import("std");
const ar = @import("ar");
const macho = ar.macho;

/// What a host build was configured for, read off its resolved target.
pub const Target = struct {
    arch: std.Target.Cpu.Arch,
    os: std.Target.Os.Tag,

    pub fn parse(arch: []const u8, os: []const u8) ?Target {
        return .{
            .arch = std.meta.stringToEnum(std.Target.Cpu.Arch, arch) orelse return null,
            .os = std.meta.stringToEnum(std.Target.Os.Tag, os) orelse return null,
        };
    }
};

fn readAll(arena: std.mem.Allocator, io: std.Io, path: []const u8, limit: usize) ![]u8 {
    return std.Io.Dir.cwd().readFileAlloc(io, path, arena, .limited(limit));
}

fn sameVersion(a: std.SemanticVersion, e: std.SemanticVersion) bool {
    return a.major == e.major and a.minor == e.minor and a.patch == e.patch;
}

/// Is a code signature mandatory for this target? arm64 macOS refuses to
/// execute an unsigned image; x86_64 macOS still runs one.
fn signatureRequired(arch: std.Target.Cpu.Arch) bool {
    return arch == .aarch64;
}

/// Check the emitted image against what the build asked for: architecture,
/// macOS platform stamp, deployment target, system libSystem, signature.
pub fn verifyHostArtifact(
    arena: std.mem.Allocator,
    io: std.Io,
    path: []const u8,
    name: []const u8,
    target: Target,
    expected_minimum_os: ?std.SemanticVersion,
) void {
    if (target.os != .macos) {
        std.debug.print("verify-host-artifact: {s} targets {t}-{t}; no Mach-O image to read\n", .{
            name, target.arch, target.os,
        });
        return;
    }
    const bytes = readAll(arena, io, path, 64 * 1024 * 1024) catch |err|
        std.process.fatal("cannot read the emitted binary {s}: {t}", .{ path, err });
    const image = macho.read(bytes) catch |err|
        std.process.fatal("{s} is not a readable single-architecture Mach-O image: {t}", .{ path, err });
    const actual_arch = image.arch() orelse target.arch;
    if (image.arch() == null or actual_arch != target.arch) std.process.fatal(
        "{s} was built for {t} but the image is cpu type 0x{x:0>8}",
        .{ path, target.arch, @as(u32, @bitCast(image.cpu_type)) },
    );
    if (!image.isMacosPlatform()) std.process.fatal(
        "{s} carries no macOS platform stamp (LC_BUILD_VERSION platform {?d})",
        .{ path, image.platform },
    );
    const minimum = image.minimum_os orelse std.process.fatal(
        "{s} carries no minimum OS version",
        .{path},
    );
    if (expected_minimum_os) |expected| {
        if (!sameVersion(minimum, expected)) std.process.fatal(
            "{s} is stamped for macOS {f} but the build was configured for {f}; the pinned " ++
                "target and the emitted deployment target have drifted apart (RA8FW-330)",
            .{ path, minimum, expected },
        );
    }
    if (!image.links_system_libsystem) failLibSystem(arena, bytes, path, image);
    verifySignature(bytes, path, name, target.arch, minimum);
}

fn failLibSystem(arena: std.mem.Allocator, bytes: []const u8, path: []const u8, image: macho.Image) noreturn {
    var buffer: [16][]const u8 = undefined;
    const names = macho.dylibNames(bytes, &buffer) catch &.{};
    var listed: std.ArrayList(u8) = .empty;
    for (names) |dylib| listed.print(arena, "\n    {s}", .{dylib}) catch {};
    std.process.fatal("{s} does not link {s}; it links {d} dylib(s):{s}", .{
        path, macho.system_libsystem, image.dylib_count, listed.items,
    });
}

fn verifySignature(
    bytes: []const u8,
    path: []const u8,
    name: []const u8,
    arch: std.Target.Cpu.Arch,
    minimum: std.SemanticVersion,
) void {
    const signature = macho.readSignature(bytes) catch |err| {
        if (signatureRequired(arch)) std.process.fatal(
            "{s} carries no usable code signature ({t}); arm64 macOS refuses to execute an " ++
                "unsigned image, so this binary links but cannot run on the host it was built for (RA8FW-330)",
            .{ path, err },
        );
        std.debug.print(
            "verify-host-artifact: {s} is a {t} macOS Mach-O for {f}, linking {s}; " ++
                "no readable code signature ({t}), which {t} does not require\n",
            .{ name, arch, minimum, macho.system_libsystem, err, arch },
        );
        return;
    };
    if (!signature.coversImage()) std.process.fatal(
        "{s} has a code signature covering {d} bytes while the signature itself starts at {d}; " ++
            "the image was modified after the link, so macOS will reject the signature at exec (RA8FW-330)",
        .{ path, signature.code_limit, signature.region.data_offset },
    );
    if (signatureRequired(arch) and !signature.isAdhoc() and !signature.isLinkerSigned()) std.process.fatal(
        "{s} carries a code signature with neither the ad-hoc nor the linker-signed flag " ++
            "(flags 0x{x:0>8}); nothing in this build signs with an identity, so this is not " ++
            "the signature the link should have produced (RA8FW-330)",
        .{ path, signature.flags },
    );
    std.debug.print(
        "verify-host-artifact: {s} is a {t} macOS Mach-O for {f}, linking {s}, {s} signed as \"{s}\" over all {d} bytes\n",
        .{
            name,                                                          arch,
            minimum,                                                       macho.system_libsystem,
            if (signature.isLinkerSigned()) "linker ad-hoc" else "ad-hoc", signature.identifier,
            signature.code_limit,
        },
    );
}

/// Refuse a static archive this build's target cannot link, before the
/// linker fails on it with a less useful message.
pub fn requireArchive(
    arena: std.mem.Allocator,
    io: std.Io,
    path: []const u8,
    consumer: []const u8,
    target: Target,
    option_hint: []const u8,
) void {
    const bytes = readAll(arena, io, path, 256 * 1024 * 1024) catch |err| {
        if (err == error.FileNotFound) std.process.fatal(
            "{s} links {s}, which does not exist. Build it for {t}-{t}, or point {s} at one that is.",
            .{ consumer, path, target.arch, target.os, option_hint },
        );
        std.process.fatal("cannot read {s}: {t}", .{ path, err });
    };
    const description = ar.describe(bytes) catch |err|
        std.process.fatal("{s} is not a readable static archive: {t}", .{ path, err });
    if (!description.suits(target.arch, target.os)) std.process.fatal(
        "{s} holds {s} {s} objects, but {s} is linked for {t}-{t}, which needs {s} {t} objects. " ++
            "The archive was built for a different host than this build targets; " ++
            "build it for {t}-{t}, or point {s} at one that is (RA8FW-330).",
        .{
            path,                                                                   description.format.label(),
            if (description.arch) |a| @tagName(a) else "unrecognised-architecture", consumer,
            target.arch,                                                            target.os,
            ar.expectedFormat(target.os).label(),                                   target.arch,
            target.arch,                                                            target.os,
            option_hint,
        },
    );
    std.debug.print("require-archive: {s} holds {s} {t} objects, which {s} can link for {t}-{t}\n", .{
        path, description.format.label(), target.arch, consumer, target.arch, target.os,
    });
}
