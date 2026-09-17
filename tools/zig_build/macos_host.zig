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
//!
//! `decide` returns the reason alongside the choice. "The stub lists targets and
//! arm64-macos is not one of them" and "I could not read a target list at all"
//! both pin, but they are different facts about the machine, and a gate that
//! prints them as the same thing cannot be read when it goes red.

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

/// Which libSystem stub the operator asked for. `auto` lets the SDK probe
/// decide (see `decide`); the other two are escape hatches for a host whose SDK
/// the probe reads wrongly, and they are surfaced to `zig build` as
/// `-Dmacos-libsystem=`.
pub const Selection = enum { auto, sdk, bundled };

/// Why a `Choice` was made. This is the diagnosis the CI gate and the docs
/// print, so each value names one distinguishable state of the machine, or
/// else says plainly that the machine was not what decided.
pub const Reason = enum {
    /// Not an arm64 Mac: the rule is inert everywhere else.
    not_arm64_macos_host,
    /// The stub's target list includes `arm64-macos`; nothing to work around.
    sdk_declares_target,
    /// The stub lists targets and `arm64-macos` is not among them. This is #899.
    sdk_omits_target,
    /// The stub was read but declares no target list in a spelling we parse, so
    /// it cannot be trusted either way.
    sdk_stub_unrecognized,
    /// An SDK was located but its `libSystem` stub could not be read.
    sdk_stub_unreadable,
    /// No SDK could be located at all (no `xcrun`, or it failed).
    sdk_not_probed,
    /// `-Dmacos-libsystem=sdk` forced the native query. Nothing about the host
    /// chose this, so it is its own reason rather than a borrowed finding.
    forced_sdk_stub,
    /// `-Dmacos-libsystem=bundled` forced the pinned query, same as above.
    forced_bundled_stub,

    /// One line, in plain words, for a build log or a gate transcript.
    pub fn explain(self: Reason) []const u8 {
        return switch (self) {
            .not_arm64_macos_host => "this host is not an arm64 Mac, so the SDK stub rule does not apply",
            .sdk_declares_target => "the SDK stub declares " ++ required_target ++ ", so the native query links against it",
            .sdk_omits_target => "the SDK stub lists its targets and " ++ required_target ++ " is not among them (#899)",
            .sdk_stub_unrecognized => "the SDK stub declares no target list in a recognised spelling, so it cannot be trusted to link " ++ required_target,
            .sdk_stub_unreadable => "an SDK was located but its libSystem stub could not be read",
            .sdk_not_probed => "no macOS SDK could be located through xcrun",
            .forced_sdk_stub => "-Dmacos-libsystem=sdk forced the native query, whatever the SDK stub says",
            .forced_bundled_stub => "-Dmacos-libsystem=bundled forced the pinned query, whatever the SDK stub says",
        };
    }
};

/// The chosen stub source and the reason for it.
pub const Decision = struct {
    choice: Choice,
    reason: Reason,
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

/// What we could learn about the host SDK. `libsystem_tbd` is null when the
/// stub could not be read; `sdk_path` is null when no SDK was located at all.
/// `libsystem_tbd_path` is carried for diagnostics only.
pub const SdkProbe = struct {
    sdk_path: ?[]const u8 = null,
    libsystem_tbd_path: ?[]const u8 = null,
    libsystem_tbd: ?[]const u8 = null,
};

/// The stub target an arm64 Mac needs to see declared in `libSystem.tbd`.
pub const required_target = "arm64-macos";

/// Pick the stub source for a host running `host_arch`/`host_os` given `probe`.
///
/// Anything that is not an arm64 Mac keeps the native query untouched. On an
/// arm64 Mac an unreadable, absent or unparsable SDK stub counts as "cannot
/// link us": the bundled stub is correct for libc-only host tools either way, so
/// an unknown SDK should not reintroduce the #899 link failure. The reason field
/// keeps those three states apart for whoever reads the log.
pub fn decide(host_arch: std.Target.Cpu.Arch, host_os: std.Target.Os.Tag, probe: SdkProbe) Decision {
    if (host_os != .macos or host_arch != .aarch64) {
        return .{ .choice = .native, .reason = .not_arm64_macos_host };
    }
    const tbd = probe.libsystem_tbd orelse {
        const reason: Reason = if (probe.sdk_path == null) .sdk_not_probed else .sdk_stub_unreadable;
        return .{ .choice = .pinned_macos_arm64, .reason = reason };
    };
    return switch (classifyTbd(tbd, required_target)) {
        .declares => .{ .choice = .native, .reason = .sdk_declares_target },
        .omits => .{ .choice = .pinned_macos_arm64, .reason = .sdk_omits_target },
        .unrecognized => .{ .choice = .pinned_macos_arm64, .reason = .sdk_stub_unrecognized },
    };
}

/// What the build will do, and what the machine actually said.
///
/// These come apart whenever `-Dmacos-libsystem=` is used. The probe still
/// runs, so its finding is still known, and it is the finding worth printing:
/// the whole point of the forced-SDK leg in the CI gate is to see what the SDK
/// stub does on that runner. Reporting the forced choice as though the probe
/// had concluded it throws away the one observation the leg exists to make.
pub const Resolution = struct {
    /// The choice the build actually uses, and why.
    effective: Decision,
    /// What the SDK probe concluded about this host, regardless of any force.
    observed: Decision,

    /// True when a force was applied and the probe would have chosen otherwise.
    pub fn overridesProbe(self: Resolution) bool {
        return self.effective.choice != self.observed.choice;
    }
};

/// Resolve the operator's `selection` against what the probe found.
///
/// `observed` is always the honest reading of the machine. `effective` is what
/// the build uses: the same decision under `auto`, and otherwise the forced
/// choice carrying a reason that names the force rather than inventing a
/// finding about the SDK.
pub fn resolve(
    selection: Selection,
    host_arch: std.Target.Cpu.Arch,
    host_os: std.Target.Os.Tag,
    probe: SdkProbe,
) Resolution {
    const observed = decide(host_arch, host_os, probe);
    const effective: Decision = switch (selection) {
        .auto => observed,
        .sdk => .{ .choice = .native, .reason = .forced_sdk_stub },
        .bundled => .{ .choice = .pinned_macos_arm64, .reason = .forced_bundled_stub },
    };
    return .{ .effective = effective, .observed = observed };
}

/// What a `.tbd` says about one target triple.
pub const TbdVerdict = enum {
    /// A target list was found and it names the wanted triple.
    declares,
    /// A target list was found and it does not name the wanted triple.
    omits,
    /// No target list was found in any spelling this parser knows.
    unrecognized,
};

/// The `platform:` spelling a tbd-v1..v3 stub uses for `macos`.
pub const v3_macos_platform = "macosx";

/// Classify what `tbd_text` says about `wanted` (e.g. `arm64-macos`).
///
/// Two stub generations are read. TAPI v4 carries a `targets:` list of full
/// triples. TAPI v1 to v3 carry `archs:` plus a separate `platform:`, with no
/// triples anywhere: an SDK of that vintage that genuinely does declare arm64
/// used to read here as "arm64-macos absent" and pinned the build. Pinning is
/// the safe direction, but the DIAGNOSIS was wrong, and it is the diagnosis the
/// gate prints.
pub fn classifyTbd(tbd_text: []const u8, wanted: []const u8) TbdVerdict {
    if (targetsFieldDeclares(tbd_text, wanted)) |declared| {
        return if (declared) .declares else .omits;
    }
    const dash = std.mem.indexOfScalar(u8, wanted, '-') orelse return .unrecognized;
    const arch = wanted[0..dash];
    const os_name = wanted[dash + 1 ..];
    if (archsFieldDeclares(tbd_text, arch, os_name)) |declared| {
        return if (declared) .declares else .omits;
    }
    return .unrecognized;
}

/// Return true when the `targets:` list of a text `.tbd` declares `wanted`.
///
/// Kept as the plain boolean question for callers that only want the answer;
/// `classifyTbd` is the one that also says "there was no list to read".
pub fn tbdDeclaresTarget(tbd_text: []const u8, wanted: []const u8) bool {
    return classifyTbd(tbd_text, wanted) == .declares;
}

/// Read a TAPI v4 `targets:` field: null when the file has none.
///
/// Handles both TAPI spellings: the inline flow list
/// (`targets: [ x86_64-macos, arm64e-macos ]`, possibly wrapped over lines) and
/// the YAML block list (`targets:` followed by indented `- x86_64-macos` items).
/// Only the `targets:` field is read, because `uuids:` repeats target names and
/// would otherwise answer for it.
pub fn targetsFieldDeclares(tbd_text: []const u8, wanted: []const u8) ?bool {
    var seen_field = false;
    var lines = std.mem.splitScalar(u8, tbd_text, '\n');
    while (lines.next()) |raw_line| {
        const line = std.mem.trim(u8, raw_line, " \t\r-");
        if (!std.mem.startsWith(u8, line, "targets:")) continue;
        seen_field = true;
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
    return if (seen_field) false else null;
}

/// Read a TAPI v1..v3 `archs:` + `platform:` pair: null when either is absent.
///
/// A v3 stub says
///
///     archs: [ i386, x86_64, arm64, arm64e ]
///     platform: macosx
///
/// so both halves have to agree before the stub can be said to declare
/// `arm64-macos`. `archs:` also appears inside each `exports:` entry, which is
/// exactly the same question asked per-slice, so any occurrence counts.
pub fn archsFieldDeclares(tbd_text: []const u8, arch: []const u8, os_name: []const u8) ?bool {
    const wanted_platform = if (std.mem.eql(u8, os_name, "macos")) v3_macos_platform else os_name;
    var seen_archs = false;
    var seen_platform = false;
    var arch_declared = false;
    var platform_declared = false;

    var lines = std.mem.splitScalar(u8, tbd_text, '\n');
    while (lines.next()) |raw_line| {
        const line = std.mem.trim(u8, raw_line, " \t\r-");
        if (std.mem.startsWith(u8, line, "archs:")) {
            seen_archs = true;
            if (listContains(line["archs:".len..], arch)) arch_declared = true;
            continue;
        }
        if (std.mem.startsWith(u8, line, "platform:")) {
            seen_platform = true;
            if (listContains(line["platform:".len..], wanted_platform)) platform_declared = true;
            continue;
        }
        if (std.mem.startsWith(u8, line, "platforms:")) {
            seen_platform = true;
            if (listContains(line["platforms:".len..], wanted_platform)) platform_declared = true;
        }
    }
    if (!seen_archs or !seen_platform) return null;
    return arch_declared and platform_declared;
}

/// Can the machine running this build execute a binary built for
/// `target_arch`/`target_os`?
///
/// This decides whether a test *run* may be excused when it cannot execute.
/// Excusing it is right off the host: a Linux checkout cross-configures
/// `aarch64-macos` precisely to exercise the Mach-O link path, and a run step
/// that hard-fails there ("the host system is unable to execute binaries from
/// the target") makes `zig build test -Dtarget=aarch64-macos` unusable as a
/// check.
///
/// Excusing it ON the host is a different thing entirely, and it is what this
/// exists to stop. The whole point of the arm64 macOS gate is that the host
/// tests RUN natively on the Mac; a blanket excuse means a Mac on which they
/// cannot run reports "0 skipped" nowhere and still exits zero, so the gate's
/// verdict would rest on tests that never executed. Only the build host itself
/// can tell those two situations apart, so the excuse is scoped to a target
/// this host genuinely cannot run.
///
/// The rule is deliberately narrow: same architecture and same OS. An arm64
/// Mac can in fact run `x86_64-macos` under Rosetta 2, and a Linux host may
/// have an emulator registered, but neither is something to assume while
/// deciding whether to forgive a missing run. Assuming less means a run that
/// is genuinely impossible fails loudly instead of disappearing.
pub fn targetRunsOnBuildHost(
    target_arch: std.Target.Cpu.Arch,
    target_os: std.Target.Os.Tag,
    host_arch: std.Target.Cpu.Arch,
    host_os: std.Target.Os.Tag,
) bool {
    return target_arch == host_arch and target_os == host_os;
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
