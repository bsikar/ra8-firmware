//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const bscan = @import("bscan");

test "a fresh state is uninitialized on BYPASS with no idcode" {
    const s: bscan.State = .{};
    const st = s.status();
    try std.testing.expect(!st.initialized);
    try std.testing.expectEqual(bscan.instr_bypass, st.last_instruction);
    try std.testing.expectEqual(@as(u32, 0), st.expected_idcode);
    try std.testing.expectError(error.NotInitialized, s.idcode());
}

test "init caches the fixed JTIDR and selects BYPASS" {
    var s: bscan.State = .{};
    s.init();
    try std.testing.expectEqual(@as(u32, 0x085DA447), try s.idcode());
    try std.testing.expect(s.status().initialized);
}

test "deinit drops back to the power-on bookkeeping" {
    var s: bscan.State = .{};
    s.init();
    try s.setInstruction(bscan.instr_clamp);
    s.deinit();
    try std.testing.expectEqual(bscan.State{}, s);
}

test "setInstruction accepts only the six legal opcodes" {
    var s: bscan.State = .{};
    s.init();
    for (0..16) |i| {
        const op: u8 = @intCast(i);
        const legal = op == 0 or op == 1 or op == 3 or op == 5 or op == 6 or op == 0xF;
        try std.testing.expectEqual(legal, bscan.isKnownInstruction(op));
        if (legal) try s.setInstruction(op) else try std.testing.expectError(error.InvalidArg, s.setInstruction(op));
    }
    try std.testing.expectError(error.InvalidArg, s.setInstruction(0xFF));
}

test "setInstruction needs init before it checks the opcode" {
    var s: bscan.State = .{};
    try std.testing.expectError(error.NotInitialized, s.setInstruction(0x42));
}

test "clear checks the mask before init and resets to BYPASS" {
    var s: bscan.State = .{};
    try std.testing.expectError(error.InvalidArg, s.clear(1));
    try std.testing.expectError(error.NotInitialized, s.clear(0));
    s.init();
    try s.setInstruction(bscan.instr_extest);
    try s.clear(0);
    try std.testing.expectEqual(bscan.instr_bypass, s.status().last_instruction);
}

test "Status has the C struct layout" {
    try std.testing.expectEqual(@as(usize, 8), @sizeOf(bscan.Status));
    try std.testing.expectEqual(@as(usize, 1), @offsetOf(bscan.Status, "last_instruction"));
}
