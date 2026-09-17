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

const std = @import("std");

/// Which libSystem stub the host build should link against.
pub const Choice = enum {
    /// Plain native query. On macOS this uses the system SDK's stub.
    native,
    /// Explicit `aarch64-macos` query, which makes Zig link its bundled stub.
    pinned_macos_arm64,

    pub fn query(self: Choice) std.Target.Query {
        return switch (self) {
            .native => .{},
            .pinned_macos_arm64 => .{ .cpu_arch = .aarch64, .os_tag = .macos },
        };
    }
};

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

const testing = std.testing;

const broken_clt_tbd =
    \\--- !tapi-tbd
    \\tbd-version: 4
    \\targets: [ x86_64-macos, x86_64-maccatalyst, arm64e-macos, arm64e-maccatalyst ]
    \\uuids:
    \\  - target: arm64e-macos
    \\    value: 00000000-0000-0000-0000-000000000000
    \\install-name: '/usr/lib/libSystem.B.dylib'
    \\
;

const healthy_tbd =
    \\--- !tapi-tbd
    \\tbd-version: 4
    \\targets: [ x86_64-macos, arm64-macos, arm64e-macos ]
    \\install-name: '/usr/lib/libSystem.B.dylib'
    \\
;

const block_list_tbd =
    \\--- !tapi-tbd
    \\targets:
    \\  - x86_64-macos
    \\  - arm64-macos
    \\install-name: '/usr/lib/libSystem.B.dylib'
    \\
;

const wrapped_flow_tbd =
    \\--- !tapi-tbd
    \\targets: [ x86_64-macos, x86_64-maccatalyst,
    \\           arm64-macos, arm64e-macos ]
    \\install-name: '/usr/lib/libSystem.B.dylib'
    \\
;

test "arm64e-macos does not answer for arm64-macos" {
    try testing.expect(!tbdDeclaresTarget(broken_clt_tbd, required_target));
    try testing.expect(tbdDeclaresTarget(broken_clt_tbd, "arm64e-macos"));
    try testing.expect(tbdDeclaresTarget(broken_clt_tbd, "x86_64-macos"));
}

test "healthy, block-list, and wrapped flow spellings all declare arm64-macos" {
    try testing.expect(tbdDeclaresTarget(healthy_tbd, required_target));
    try testing.expect(tbdDeclaresTarget(block_list_tbd, required_target));
    try testing.expect(tbdDeclaresTarget(wrapped_flow_tbd, required_target));
}

test "uuids entries never answer for the targets list" {
    const uuids_only =
        \\--- !tapi-tbd
        \\targets: [ arm64e-macos ]
        \\uuids:
        \\  - target: arm64-macos
        \\    value: 00000000-0000-0000-0000-000000000000
        \\
    ;
    try testing.expect(!tbdDeclaresTarget(uuids_only, required_target));
}

test "only an arm64 Mac with a broken SDK gets pinned" {
    try testing.expectEqual(Choice.pinned_macos_arm64, decide(.aarch64, .macos, .{ .libsystem_tbd = broken_clt_tbd }));
    try testing.expectEqual(Choice.native, decide(.aarch64, .macos, .{ .libsystem_tbd = healthy_tbd }));
    try testing.expectEqual(Choice.native, decide(.x86_64, .macos, .{ .libsystem_tbd = broken_clt_tbd }));
    try testing.expectEqual(Choice.native, decide(.aarch64, .linux, .{}));
    try testing.expectEqual(Choice.native, decide(.x86_64, .linux, .{}));
}

test "an unreadable SDK falls back to the bundled stub on arm64 macOS" {
    try testing.expectEqual(Choice.pinned_macos_arm64, decide(.aarch64, .macos, .{}));
    try testing.expectEqual(Choice.pinned_macos_arm64, decide(.aarch64, .macos, .{ .sdk_path = "/nope" }));
}

test "pinned choice is an explicit, non-native query" {
    const q = Choice.pinned_macos_arm64.query();
    try testing.expectEqual(std.Target.Cpu.Arch.aarch64, q.cpu_arch.?);
    try testing.expectEqual(std.Target.Os.Tag.macos, q.os_tag.?);
    try testing.expect(!q.isNativeOs());
    try testing.expect(Choice.native.query().isNativeOs());
}
