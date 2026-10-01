//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Which DEVICE of the RA8 family an app is cross-built for, and what that
//! choice adds to every command line (#1131).
//!
//! `ra8_add_app(BOARD <name>)` selects a board layer, and the board layer is
//! only half of the choice: the other half is the CMake TOOLCHAIN FILE the app
//! is configured with, which is what actually carries the device's CPU
//! differences. cmake/toolchain-ra8p1.cmake includes toolchain-ra8d2.cmake
//! verbatim and then appends ONE thing to the *_INIT flag groups by default, so
//! the RA8P1 command line is the RA8D2 one plus a one-flag tail.
//!
//! What it does NOT append matters as much. The toolchain file used to override
//! -mfpu to fpv5-d16 unconditionally; #225 settled that the RA8P1 primary M85
//! declares `__FPU_DP 0` in FSP's own CMSIS header, byte-identical to the RA8D2,
//! so both parts build fpv5-sp-d16 and fpv5-d16 is reachable only through the
//! opt-in `-DRA8P1_DP_FPU=ON` bench switch for #229 (which also defines
//! `RA8_FPU_DP_ENABLED`). Emitting fpv5-d16 by default is not a harmless
//! surplus: `libs/ra8_hal/inc/ra8_fpu_probe.h` refuses a double-precision
//! -mfpu without that define, and on silicon the .f64 opcodes would be
//! UNDEFINED instructions. The opt-in configuration is not modelled here.
//!
//! The one flag is silent when missed:
//!
//!   -DRA8_DEVICE_RA8P1  libs/ra8_core/inc/ra8_device.h reads it to switch
//!                       register bases, memory-map sizes and feature flags.
//!                       Without it the header defaults to the RA8D2 and the
//!                       firmware is compiled against another chip's memory
//!                       map, with no diagnostic anywhere.
//!
//! Encoded as data keyed on the board because the graph had treated the flag
//! set as a constant: every app cross-built before this slice is an ek_ra8d2
//! app, so the device layer was the empty tail and indistinguishable from not
//! existing.

const std = @import("std");

/// One device of the family: the board layer that selects it, and the tail its
/// toolchain file appends to the shared RA8D2 flag body.
pub const Device = struct {
    /// The `BOARD` name, as ra8_add_app() spells it.
    name: []const u8,
    /// The board layer directory that name resolves to.
    board: []const u8,
    /// The toolchain file a standalone configure of such an app is given.
    /// Carried for the record: it is what the per-app justfile names, and it
    /// is the reason these flags exist at all.
    toolchain_file: []const u8,
    /// What that toolchain file appends, in its own order. Emitted after the
    /// shared CPU flags and before the configuration's -O/-g/-D set, which is
    /// where CMake's *_INIT append puts it.
    compile_flags: []const []const u8 = &.{},
    /// The subset of the above that also reaches the LINK line. The device
    /// define does not (CMAKE_EXE_LINKER_FLAGS_INIT never gets it). An -mfpu
    /// override would, because the link picks its newlib multilib from the
    /// effective -mfpu; no default device carries one today.
    link_flags: []const []const u8 = &.{},
};

/// The devices this graph cross-builds for. The RA8D2 entry is deliberately
/// EMPTY rather than absent: it is the base every other device layers onto, and
/// naming it keeps "this board adds nothing" a measured fact in the table
/// rather than a gap in it.
pub const devices = [_]Device{
    .{
        .name = "ek_ra8d2",
        .board = "libs/ra8_board_ek_ra8d2",
        .toolchain_file = "cmake/toolchain-ra8d2.cmake",
    },
    .{
        .name = "ra8p1",
        .board = "libs/ra8_board_ra8p1",
        .toolchain_file = "cmake/toolchain-ra8p1.cmake",
        // toolchain-ra8p1.cmake appends the define to C, CXX and ASM but not
        // to CMAKE_EXE_LINKER_FLAGS_INIT, so the link line gets nothing.
        .compile_flags = &.{"-DRA8_DEVICE_RA8P1"},
    },
};

/// The device an app's board layer selects. Panics on an unknown board rather
/// than falling back to the RA8D2: a silent fallback is exactly the failure
/// this module exists to make impossible, and a new board layer should have to
/// name what its toolchain file adds.
pub fn forBoard(board: []const u8) Device {
    for (devices) |device| {
        if (std.mem.eql(u8, device.board, board)) return device;
    }
    std.debug.panic("ra8: board '{s}' has no device entry in device.zig", .{board});
}

/// The device tail for a board, on the compile line.
pub fn compileFlags(board: []const u8) []const []const u8 {
    return forBoard(board).compile_flags;
}

/// The device tail for a board, on the link line.
pub fn linkFlags(board: []const u8) []const []const u8 {
    return forBoard(board).link_flags;
}

test "the RA8D2 is the base and adds nothing" {
    const d2 = forBoard("libs/ra8_board_ek_ra8d2");
    try std.testing.expectEqual(@as(usize, 0), d2.compile_flags.len);
    try std.testing.expectEqual(@as(usize, 0), d2.link_flags.len);
}

test "the RA8P1 adds only the device define, never a DP -mfpu" {
    const p1 = forBoard("libs/ra8_board_ra8p1");
    try std.testing.expectEqual(@as(usize, 1), p1.compile_flags.len);
    try std.testing.expectEqualStrings("-DRA8_DEVICE_RA8P1", p1.compile_flags[0]);
    // #225: a default fpv5-d16 trips ra8_fpu_probe.h's #error and would emit
    // .f64 opcodes the part's single-precision FPU takes as UNDEFINED.
    for (devices) |device| {
        for (device.compile_flags) |flag| {
            try std.testing.expect(!std.mem.eql(u8, flag, "-mfpu=fpv5-d16"));
        }
    }
}

test "the device define does not reach the link line" {
    const p1 = forBoard("libs/ra8_board_ra8p1");
    try std.testing.expectEqual(@as(usize, 0), p1.link_flags.len);
}

test "every device in the table names a board layer and a toolchain file" {
    for (devices) |device| {
        try std.testing.expect(std.mem.startsWith(u8, device.board, "libs/ra8_board_"));
        try std.testing.expect(std.mem.endsWith(u8, device.toolchain_file, ".cmake"));
        // A link flag has to be one of the compile flags: the toolchain file
        // appends to CMAKE_EXE_LINKER_FLAGS_INIT out of the same variable it
        // appends to the C flags from, so a link-only device flag would be a
        // transcription error rather than a rule.
        for (device.link_flags) |link_flag| {
            var found = false;
            for (device.compile_flags) |compile_flag| {
                if (std.mem.eql(u8, link_flag, compile_flag)) found = true;
            }
            try std.testing.expect(found);
        }
    }
}
