//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The RA8P1 CPU1 linker script (RA8FW-496) keeps the EK-RA8D2 partition and
//! stays inside the code MRAM user area of Renesas R01AN7880 Figure 6.

const std = @import("std");

const ra8p1_script = "libs/ra8_board_ra8p1/ld/linker_script_cpu1.ld";
const ek_ra8d2_script = "libs/ra8_board_ek_ra8d2/ld/linker_script_cpu1.ld";

/// R01AN7880 Rev.1.01 Figure 6: the 1 Mbyte code MRAM user area.
const mram_user_start: u32 = 0x0200_0000;
const mram_user_end: u32 = 0x0210_0000;
/// ra8_board_ra8p1 system_init region 4 (k_ra8_mpu_shram_base/limit).
const shram_start: u32 = 0x2210_0000;
const shram_end: u32 = 0x221A_0000;

fn readScript(path: []const u8) ![]u8 {
    return std.fs.cwd().readFileAlloc(std.testing.allocator, path, 1 << 16);
}

fn body(script: []const u8) ![]const u8 {
    const start = std.mem.indexOf(u8, script, "ENTRY(") orelse return error.NoEntry;
    return script[start..];
}

test "the RA8P1 CPU1 script is the EK-RA8D2 one below its header" {
    const ra8p1 = try readScript(ra8p1_script);
    defer std.testing.allocator.free(ra8p1);
    const ek = try readScript(ek_ra8d2_script);
    defer std.testing.allocator.free(ek);
    try std.testing.expectEqualStrings(try body(ek), try body(ra8p1));
}

test "the RA8P1 CPU1 windows sit in Figure 6's MRAM user area and the shared bank" {
    const ra8p1 = try readScript(ra8p1_script);
    defer std.testing.allocator.free(ra8p1);
    try std.testing.expect(std.mem.indexOf(u8, ra8p1, "MRAM_CPU1 (rx)  : ORIGIN = 0x020C0000, LENGTH = 256K") != null);
    try std.testing.expect(std.mem.indexOf(u8, ra8p1, "SRAM_CPU1 (rwx) : ORIGIN = 0x22190000, LENGTH = 64K") != null);
    const mram_origin: u32 = 0x020C_0000;
    const mram_length: u32 = 256 * 1024;
    try std.testing.expect(mram_origin >= mram_user_start);
    try std.testing.expectEqual(mram_user_end, mram_origin + mram_length);
    const sram_origin: u32 = 0x2219_0000;
    const sram_length: u32 = 64 * 1024;
    try std.testing.expect(sram_origin >= shram_start);
    try std.testing.expectEqual(shram_end, sram_origin + sram_length);
}

const ra8p1_fragment = "libs/ra8_board_ra8p1/ld/cpu1_image.ld.in";
const ek_ra8d2_fragment = "libs/ra8_board_ek_ra8d2/ld/cpu1_image.ld.in";
const ra8p1_map = "libs/ra8_board_ra8p1/ld/cpu1_memory_map.cmake";
const ek_ra8d2_map = "libs/ra8_board_ek_ra8d2/ld/cpu1_memory_map.cmake";

fn fromMarker(text: []const u8, marker: []const u8) ![]const u8 {
    const start = std.mem.indexOf(u8, text, marker) orelse return error.NoMarker;
    return text[start..];
}

/// Every `set(` line, in order, joined by newlines.
fn setLines(allocator: std.mem.Allocator, text: []const u8) ![]u8 {
    var out: std.ArrayList(u8) = .empty;
    errdefer out.deinit(allocator);
    var lines = std.mem.splitScalar(u8, text, '\n');
    while (lines.next()) |line| {
        if (!std.mem.startsWith(u8, line, "set(")) continue;
        try out.appendSlice(allocator, line);
        try out.append(allocator, '\n');
    }
    return out.toOwnedSlice(allocator);
}

test "the RA8P1 CPU1 image fragment is the EK-RA8D2 one below its header" {
    const ra8p1 = try readScript(ra8p1_fragment);
    defer std.testing.allocator.free(ra8p1);
    const ek = try readScript(ek_ra8d2_fragment);
    defer std.testing.allocator.free(ek);
    try std.testing.expectEqualStrings(try fromMarker(ek, "\nSECTIONS\n{"), try fromMarker(ra8p1, "\nSECTIONS\n{"));
}

test "the RA8P1 CPU1 window module sets what the EK-RA8D2 one sets" {
    const allocator = std.testing.allocator;
    const ra8p1 = try readScript(ra8p1_map);
    defer allocator.free(ra8p1);
    const ek = try readScript(ek_ra8d2_map);
    defer allocator.free(ek);
    const ra8p1_sets = try setLines(allocator, ra8p1);
    defer allocator.free(ra8p1_sets);
    const ek_sets = try setLines(allocator, ek);
    defer allocator.free(ek_sets);
    try std.testing.expectEqualStrings(ek_sets, ra8p1_sets);
    try std.testing.expect(std.mem.indexOf(u8, ra8p1_sets, "set(RA8_CPU1_IMAGE_ORIGIN 0x020C0000)") != null);
    try std.testing.expect(std.mem.indexOf(u8, ra8p1_sets, "set(RA8_CPU1_SRAM_ORIGIN 0x22190000)") != null);
}
