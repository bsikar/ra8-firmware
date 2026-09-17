//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host-target selection for the Zig host applications on Apple Silicon (#899).
//!
//! On macOS 15+ / Darwin 26 the Command Line Tools SDK ships a `libSystem.tbd`
//! whose `targets:` list omits `arm64-macos`:
//!
//!     targets: [ x86_64-macos, x86_64-maccatalyst, arm64e-macos, arm64e-maccatalyst ]
//!
//! Zig 0.14.1's Mach-O linker only matches stub symbols whose target is listed,
//! so a *native* `zig build` on an arm64 Mac resolves nothing out of the SDK and
//! fails with `undefined symbol: _abort`, `_malloc_size`, `_sysctlbyname`, and
//! friends. Zig's own bundled `libSystem.tbd` does declare `arm64-macos`, and it
//! is used whenever the target query is not native (an explicit `os_tag` makes a
//! query non-native, which is what turns off the system-SDK lookup).
//!
//! So the selection rule is: keep the plain native query everywhere, except on an
//! arm64 Mac whose SDK stub cannot link us, where we pin an explicit
//! `aarch64-macos` query and let Zig link its own stub. The host apps here are
//! libc-only, so nothing needs the SDK's frameworks; `-Dtarget=native` remains the
//! escape hatch for anyone who does.
//!
//! The pinned query also carries the host's own macOS version, so that standing
//! in for the native build does not silently change the deployment target (see
//! `pinnedOsVersion`).

const std = @import("std");

/// Which libSystem stub the host build should link against.
pub const Choice = enum {
    /// Plain native query. On macOS this uses the system SDK's stub.
    native,
    /// Explicit `aarch64-macos` query, which makes Zig link its bundled stub.
    pinned_macos_arm64,

    /// The target query for this choice.
    ///
    /// `host_macos_version` is the version the host is actually running, when
    /// it is known. Pinning an explicit target otherwise drops the build onto
    /// Zig's default macOS range, which is a much older floor than the machine
    /// doing the build: see `pinnedOsVersion`.
    pub fn query(self: Choice, host_macos_version: ?std.SemanticVersion) std.Target.Query {
        return switch (self) {
            .native => .{},
            .pinned_macos_arm64 => blk: {
                var pinned: std.Target.Query = .{ .cpu_arch = .aarch64, .os_tag = .macos };
                if (pinnedOsVersion(host_macos_version)) |version| {
                    pinned.os_version_min = .{ .semver = version };
                    pinned.os_version_max = .{ .semver = version };
                }
                break :blk pinned;
            },
        };
    }
};

/// macOS 11 Big Sur is the first release that ran on Apple silicon, so an
/// arm64 Mac cannot truthfully report anything older.
pub const first_arm64_macos_major = 11;

/// The macOS version to write into the pinned query, or null to leave Zig's
/// own default range alone.
///
/// A native build stamps the host's own OS version as both the minimum and the
/// maximum. Pinning `aarch64-macos` with no version instead takes Zig's default
/// range, whose floor is several releases below any Apple silicon Mac, so the
/// pinned build would quietly differ from the native one it stands in for: a
/// lower `LC_BUILD_VERSION` minimum in the Mach-O, and `Target.Os.isAtLeast`
/// answering against the wrong floor in conditionally compiled code. Carrying
/// the host version across closes that gap.
///
/// A reading below `first_arm64_macos_major` cannot have come from the arm64
/// Mac this rule is about, so it is discarded rather than pinned; the pre-release
/// and build metadata fields are dropped for the same reason, as a deployment
/// target has no use for them.
pub fn pinnedOsVersion(host_macos_version: ?std.SemanticVersion) ?std.SemanticVersion {
    const version = host_macos_version orelse return null;
    if (version.major < first_arm64_macos_major) return null;
    return .{ .major = version.major, .minor = version.minor, .patch = version.patch };
}

/// What we could learn about the host SDK. Both fields are null when the probe
/// could not run at all (non-macOS host, no `xcrun`, unreadable SDK).
pub const SdkProbe = struct {
    sdk_path: ?[]const u8 = null,
    libsystem_tbd: ?[]const u8 = null,
};

/// The stub target an arm64 Mac needs to see declared in `libSystem.tbd`.
pub const required_target = "arm64-macos";

/// Pick the stub source for a host running `host_arch`/`host_os` given `probe`.
///
/// Anything that is not an arm64 Mac keeps the native query untouched. On an
/// arm64 Mac an unreadable or silent SDK counts as "cannot link us": the bundled
/// stub is correct for libc-only host tools either way, so an unknown SDK should
/// not reintroduce the #899 link failure.
pub fn decide(host_arch: std.Target.Cpu.Arch, host_os: std.Target.Os.Tag, probe: SdkProbe) Choice {
    if (host_os != .macos or host_arch != .aarch64) return .native;
    const tbd = probe.libsystem_tbd orelse return .pinned_macos_arm64;
    return if (tbdDeclaresTarget(tbd, required_target)) .native else .pinned_macos_arm64;
}

/// Return true when the `targets:` list of a text `.tbd` declares `wanted`.
///
/// Handles both TAPI spellings: the inline flow list
/// (`targets: [ x86_64-macos, arm64e-macos ]`, possibly wrapped over lines) and
/// the YAML block list (`targets:` followed by indented `- x86_64-macos` items).
/// Only the `targets:` field is read, because `uuids:` repeats target names and
/// would otherwise answer for it.
pub fn tbdDeclaresTarget(tbd_text: []const u8, wanted: []const u8) bool {
    var lines = std.mem.splitScalar(u8, tbd_text, '\n');
    while (lines.next()) |raw_line| {
        const line = std.mem.trim(u8, raw_line, " \t\r-");
        if (!std.mem.startsWith(u8, line, "targets:")) continue;
        const rest = std.mem.trim(u8, line["targets:".len..], " \t\r");

        if (std.mem.indexOfScalar(u8, rest, '[') != null) {
            // Flow list, possibly wrapped over several lines. A target triple is
            // never split across a line break, so each line can be read on its own.
            var chunk = rest;
            while (true) {
                if (listContains(chunk, wanted)) return true;
                if (std.mem.indexOfScalar(u8, chunk, ']') != null) break;
                chunk = lines.next() orelse break;
            }
            continue;
        }

        if (rest.len != 0) {
            if (listContains(rest, wanted)) return true;
            continue;
        }

        // Block list: consume the following `- item` lines.
        while (lines.next()) |item_raw| {
            const item_line = std.mem.trim(u8, item_raw, " \t\r");
            if (!std.mem.startsWith(u8, item_line, "- ")) break;
            if (listContains(item_line[2..], wanted)) return true;
        }
    }
    return false;
}

/// True when `haystack` contains `wanted` as a whole token. Token characters are
/// the ones Apple uses in a target triple, so `arm64e-macos` never answers for
/// `arm64-macos`.
fn listContains(haystack: []const u8, wanted: []const u8) bool {
    var index: usize = 0;
    while (std.mem.indexOfPos(u8, haystack, index, wanted)) |found| {
        index = found + wanted.len;
        const before_ok = found == 0 or !isTokenChar(haystack[found - 1]);
        const after = found + wanted.len;
        const after_ok = after == haystack.len or !isTokenChar(haystack[after]);
        if (before_ok and after_ok) return true;
    }
    return false;
}

fn isTokenChar(c: u8) bool {
    return std.ascii.isAlphanumeric(c) or c == '_' or c == '-' or c == '.';
}
