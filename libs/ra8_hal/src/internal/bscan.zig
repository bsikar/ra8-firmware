//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Boundary-scan bookkeeping (inc/ra8_bscan.h, RA8FW-551). The TAP
//! registers are not CPU-readable (HUM Ch 50.2.3 p 3259), so this is a
//! CPU-side snapshot of what the external fixture should see.

/// HUM Ch 50.2.2 "JTIDR" p 3258: DID[31:0] fixed by Renesas.
pub const jtidr_reset: u32 = 0x085DA447;

/// HUM Ch 50.2.1 "JTIR" p 3258: the legal opcodes.
pub const instr_extest: u8 = 0x0;
pub const instr_sample_preload: u8 = 0x1;
pub const instr_idcode: u8 = 0x3;
pub const instr_clamp: u8 = 0x5;
pub const instr_highz: u8 = 0x6;
pub const instr_bypass: u8 = 0xF;

pub const Error = error{ NotInitialized, InvalidArg };

/// `ra8_bscan_status_t`.
pub const Status = extern struct {
    initialized: bool,
    last_instruction: u8,
    expected_idcode: u32,
};

comptime {
    if (@sizeOf(Status) != 8 or @offsetOf(Status, "expected_idcode") != 4)
        @compileError("Status must match ra8_bscan_status_t");
}

pub fn isKnownInstruction(instr: u8) bool {
    return switch (instr) {
        instr_extest, instr_sample_preload, instr_idcode, instr_clamp, instr_highz, instr_bypass => true,
        else => false,
    };
}

pub const State = struct {
    initialized: bool = false,
    last_instruction: u8 = instr_bypass,
    expected_idcode: u32 = 0,

    /// Power-on: JTIR selects BYPASS (0xE/0xF) and JTIDR is the fixed ID.
    pub fn init(s: *State) void {
        s.* = .{ .initialized = true, .expected_idcode = jtidr_reset };
    }

    /// TAP reset equivalent: back to the power-on bookkeeping.
    pub fn deinit(s: *State) void {
        s.* = .{};
    }

    pub fn idcode(s: *const State) Error!u32 {
        if (!s.initialized) return error.NotInitialized;
        return s.expected_idcode;
    }

    pub fn status(s: *const State) Status {
        return .{ .initialized = s.initialized, .last_instruction = s.last_instruction, .expected_idcode = s.expected_idcode };
    }

    /// Only mask 0 is legal; "cleared" means BYPASS.
    pub fn clear(s: *State, mask: u32) Error!void {
        if (mask != 0) return error.InvalidArg;
        if (!s.initialized) return error.NotInitialized;
        s.last_instruction = instr_bypass;
    }

    pub fn setInstruction(s: *State, instr: u8) Error!void {
        if (!s.initialized) return error.NotInitialized;
        if (!isKnownInstruction(instr)) return error.InvalidArg;
        s.last_instruction = instr;
    }
};
