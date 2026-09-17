//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Argv membrane and exit-status contract for the offline C6
//! integration-contract gate (#858).
//!
//! `run` is parameterised on a directory, a repository root and both output
//! streams, so the whole contract, including the file-system statuses, is
//! exercised by tests against a temporary tree rather than the real
//! repository.
//!
//! Exit statuses, inherited from the predecessor Python gate:
//!
//!   0  the staging, component and ABI contract agrees, or the detector
//!      selftest passed
//!   1  at least one finding, a failing detector selftest, or an input file
//!      that could not be read or decoded
//!   2  one of the four required committed inputs is missing
//!
//! There is deliberately NO usage status. The predecessor looked in argv only
//! for `--selftest` and validated the recipe whatever else it was handed, so
//! an unrecognised flag is ignored rather than rejected, and `--selftest`
//! anywhere in argv wins over the scan. Both are pinned by tests.

const std = @import("std");
const implementation = @import("internal/root.zig");

pub const Contract = implementation.Contract;
pub const Inventory = implementation.Inventory;

pub const exit_ok: u8 = 0;
pub const exit_fail: u8 = 1;
pub const exit_config: u8 = 2;

/// Where the four committed inputs live, relative to the repository root.
pub const Paths = struct {
    build_script: []const u8 = "coprocessor/esp32c6/build.sh",
    patch_file: []const u8 = "coprocessor/esp32c6/patches/0001-custom-rpc-sync-response-hook.patch",
    service_header: []const u8 = "port/esp32_c6/inc/ra8_mdl_service.h",
    service_source: []const u8 = "port/esp32_c6/src/mdl_service.c",
};

const max_input_bytes = 8 * 1024 * 1024;

const good_build =
    \\
    \\  COMPONENT_DIR="${PERIPHERAL_DIR}/components/mdl_service"
    \\cp "${SCRIPT_DIR}/../../port/esp32_c6/CMakeLists.txt" "${COMPONENT_DIR}/CMakeLists.txt"
    \\cp "${SCRIPT_DIR}/../../port/esp32_c6/src/mdl_service.c" \
    \\  "${COMPONENT_DIR}/src/mdl_service.c"
    \\cp "${SCRIPT_DIR}/../../port/esp32_c6/inc/ra8_mdl_service.h" \
    \\  "${COMPONENT_DIR}/include/ra8_mdl_service.h"
    \\cp "${SCRIPT_DIR}/../../libs/ra8_c6link/inc/ra8_mdl_protocol.h" \
    \\  "${COMPONENT_DIR}/include/ra8_mdl_protocol.h"
    \\cp "${SCRIPT_DIR}/../../libs/ra8_c6link/inc/ra8_mdl_http.h" \
    \\  "${COMPONENT_DIR}/include/ra8_mdl_http.h"
    \\grep -Eq 'T[[:space:]]+ra8_mdl_service_component_abi$'
    \\grep -Eq 'T[[:space:]]+esp_hosted_custom_rpc_sync_handler$'
    \\
;

const good_patch =
    \\
    \\+set(COMPONENTS esp_timer main mdl_service)
    \\+__attribute__((weak)) esp_err_t esp_hosted_custom_rpc_sync_handler(
    \\
;

const good_header = "uint32_t ra8_mdl_service_component_abi(void);\n";

const good_source =
    \\
    \\[[gnu::noinline]] uint32_t ra8_mdl_service_component_abi(void) { return 1U; }
    \\esp_err_t esp_hosted_custom_rpc_sync_handler(uint32_t id) { return ESP_OK; }
    \\
;

/// One detector selftest case: a label, the four texts, and whether the
/// detector is expected to fire.
pub const SelftestCase = struct {
    label: []const u8,
    build: []const u8,
    patch: []const u8,
    header: []const u8,
    source: []const u8,
    fires: bool,
};

fn replaceOnce(allocator: std.mem.Allocator, text: []const u8, needle: []const u8, with: []const u8) ![]const u8 {
    const at = std.mem.indexOf(u8, text, needle) orelse return allocator.dupe(u8, text);
    return std.mem.concat(allocator, u8, &.{ text[0..at], with, text[at + needle.len ..] });
}

fn replaceAll(allocator: std.mem.Allocator, text: []const u8, needle: []const u8, with: []const u8) ![]const u8 {
    const size = std.mem.replacementSize(u8, text, needle, with);
    const out = try allocator.alloc(u8, size);
    _ = std.mem.replace(u8, text, needle, with, out);
    return out;
}

/// One quiet control plus one case for each protected seam: staged-file and
/// component drift first, then the ABI and hook chain.
pub fn selftestCases(allocator: std.mem.Allocator) ![]SelftestCase {
    var cases: std.ArrayListUnmanaged(SelftestCase) = .{};
    try cases.append(allocator, .{
        .label = "contract agrees",
        .build = good_build,
        .patch = good_patch,
        .header = good_header,
        .source = good_source,
        .fires = false,
    });
    try cases.append(allocator, .{
        .label = "staged source renamed away",
        .build = try replaceOnce(allocator, good_build, "inc/ra8_mdl_http.h", "inc/mdl_http.h"),
        .patch = good_patch,
        .header = good_header,
        .source = good_source,
        .fires = true,
    });
    try cases.append(allocator, .{
        .label = "destination kept stale basename",
        .build = try replaceAll(allocator, good_build, "include/ra8_mdl_protocol.h", "include/mdl_protocol.h"),
        .patch = good_patch,
        .header = good_header,
        .source = good_source,
        .fires = true,
    });
    try cases.append(allocator, .{
        .label = "required copy removed",
        .build = try replaceAll(
            allocator,
            good_build,
            "cp \"${SCRIPT_DIR}/../../port/esp32_c6/CMakeLists.txt\" \"${COMPONENT_DIR}/CMakeLists.txt\"\n",
            "",
        ),
        .patch = good_patch,
        .header = good_header,
        .source = good_source,
        .fires = true,
    });
    try cases.append(allocator, .{
        .label = "component identity drifted",
        .build = good_build,
        .patch = try replaceAll(allocator, good_patch, "main mdl_service", "main ra8_mdl_service"),
        .header = good_header,
        .source = good_source,
        .fires = true,
    });
    try cases.append(allocator, .{
        .label = "component assignment became nonliteral",
        .build = try replaceAll(
            allocator,
            good_build,
            "  COMPONENT_DIR=\"${PERIPHERAL_DIR}/components/mdl_service\"",
            "  COMPONENT_DIR=\"${PERIPHERAL_DIR}/components/${COMPONENT_NAME}\"",
        ),
        .patch = good_patch,
        .header = good_header,
        .source = good_source,
        .fires = true,
    });
    try cases.append(allocator, .{
        .label = "public ABI name drifted",
        .build = good_build,
        .patch = good_patch,
        .header = try replaceAll(allocator, good_header, implementation.component_abi, "mdl_service_component_abi"),
        .source = good_source,
        .fires = true,
    });
    try cases.append(allocator, .{
        .label = "ABI became private",
        .build = good_build,
        .patch = good_patch,
        .header = good_header,
        .source = try replaceAll(allocator, good_source, "[[gnu::noinline]] uint32_t", "static uint32_t"),
        .fires = true,
    });
    try cases.append(allocator, .{
        .label = "post-link ABI name drifted",
        .build = try replaceAll(allocator, good_build, implementation.component_abi, "mdl_service_component_abi"),
        .patch = good_patch,
        .header = good_header,
        .source = good_source,
        .fires = true,
    });
    try cases.append(allocator, .{
        .label = "weak hook disappeared",
        .build = good_build,
        .patch = try replaceAll(allocator, good_patch, implementation.custom_rpc_hook, "custom_rpc_sync_handler"),
        .header = good_header,
        .source = good_source,
        .fires = true,
    });
    return cases.toOwnedSlice(allocator);
}

/// The inventory the selftest checks existence against: the sources the quiet
/// control stages, so the cases never reach the file system.
fn selftestInventory(allocator: std.mem.Allocator) ![]const []const u8 {
    const copies = try implementation.parseStagedCopies(allocator, good_build);
    var sources: std.ArrayListUnmanaged([]const u8) = .{};
    for (copies) |copy| try sources.append(allocator, copy.source);
    return sources.toOwnedSlice(allocator);
}

/// Labels of the cases whose expectation the detector failed to meet.
pub fn selftestFailures(allocator: std.mem.Allocator) ![][]const u8 {
    const inventory = Inventory{ .sources = try selftestInventory(allocator) };
    var failures: std.ArrayListUnmanaged([]const u8) = .{};
    for (try selftestCases(allocator)) |case| {
        const findings = try implementation.checkContract(allocator, .{
            .build_text = case.build,
            .patch_text = case.patch,
            .header_text = case.header,
            .source_text = case.source,
        }, inventory);
        if ((findings.len > 0) != case.fires) {
            const verb = if (case.fires) try allocator.dupe(u8, "reported nothing") else blk: {
                var rendered: std.ArrayListUnmanaged(u8) = .{};
                try rendered.appendSlice(allocator, "reported [");
                for (findings, 0..) |finding, index| {
                    if (index > 0) try rendered.appendSlice(allocator, ", ");
                    try rendered.writer(allocator).print("'{s}'", .{finding});
                }
                try rendered.append(allocator, ']');
                break :blk try rendered.toOwnedSlice(allocator);
            };
            try failures.append(allocator, try std.fmt.allocPrint(allocator, "  {s}: {s}", .{ case.label, verb }));
        }
    }
    return failures.toOwnedSlice(allocator);
}

fn runSelftest(
    allocator: std.mem.Allocator,
    out: anytype,
    err: anytype,
) !u8 {
    const failures = try selftestFailures(allocator);
    if (failures.len > 0) {
        try err.writeAll("check_c6_integration --selftest: FAILED\n");
        for (failures) |failure| try err.print("{s}\n", .{failure});
        return exit_fail;
    }
    const cases = try selftestCases(allocator);
    var fires: usize = 0;
    for (cases) |case| {
        if (case.fires) fires += 1;
    }
    try out.print(
        "check_c6_integration --selftest: OK ({d} cases: {d} fire, {d} stays quiet).\n",
        .{ cases.len, fires, cases.len - fires },
    );
    return exit_ok;
}

fn readInput(allocator: std.mem.Allocator, dir: std.fs.Dir, path: []const u8) !?[]u8 {
    const text = dir.readFileAlloc(allocator, path, max_input_bytes) catch return null;
    if (!std.unicode.utf8ValidateSlice(text)) return null;
    // The predecessor read every input in text mode, so a carriage return
    // never reached its patterns.
    const collapsed = try implementation.normalizeTerminators(allocator, text);
    return collapsed;
}

/// Run the gate: the detector selftest, or the real committed recipe read
/// from `repo_root` under `dir`.
pub fn run(
    allocator: std.mem.Allocator,
    dir: std.fs.Dir,
    repo_root: []const u8,
    argv: []const []const u8,
    paths: Paths,
    out: anytype,
    err: anytype,
) !u8 {
    for (argv) |argument| {
        if (std.mem.eql(u8, argument, "--selftest")) return runSelftest(allocator, out, err);
    }

    const required = [_][]const u8{
        paths.build_script,
        paths.patch_file,
        paths.service_header,
        paths.service_source,
    };
    var resolved: [required.len][]const u8 = undefined;
    for (required, 0..) |relative, index| {
        resolved[index] = try std.fs.path.join(allocator, &.{ repo_root, relative });
        const stat = dir.statFile(resolved[index]) catch {
            try err.print("check_c6_integration: FATAL -- missing {s}\n", .{resolved[index]});
            return exit_config;
        };
        if (stat.kind != .file) {
            try err.print("check_c6_integration: FATAL -- missing {s}\n", .{resolved[index]});
            return exit_config;
        }
    }

    var texts: [required.len][]const u8 = undefined;
    for (resolved, 0..) |path, index| {
        texts[index] = (try readInput(allocator, dir, path)) orelse {
            // Where the predecessor's UnicodeDecodeError traceback landed.
            try err.print("check_c6_integration: FATAL -- cannot read {s}\n", .{path});
            return exit_fail;
        };
    }

    // The predecessor asked the tree for each staged source one at a time.
    // Resolving them here keeps every matcher in `internal/root.zig` free of
    // the file system while the real run still answers from the real tree.
    const contract = Contract{
        .build_text = texts[0],
        .patch_text = texts[1],
        .header_text = texts[2],
        .source_text = texts[3],
    };
    var present: std.ArrayListUnmanaged([]const u8) = .{};
    for (try implementation.parseStagedCopies(allocator, contract.build_text)) |copy| {
        const staged = try std.fs.path.join(allocator, &.{ repo_root, copy.source });
        const stat = dir.statFile(staged) catch continue;
        if (stat.kind == .file) try present.append(allocator, copy.source);
    }

    const findings = try implementation.checkContract(
        allocator,
        contract,
        .{ .sources = present.items },
    );

    if (findings.len > 0) {
        try err.print("check_c6_integration: {d} C6 integration drift(s):\n", .{findings.len});
        for (findings) |finding| try err.print("  {s}\n", .{finding});
        return exit_fail;
    }
    try out.writeAll("check_c6_integration: C6 staging/component/ABI contract agrees.\n");
    return exit_ok;
}
