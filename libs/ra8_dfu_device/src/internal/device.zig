//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The USB-DFU device class's state, with no USB and no MRAM in it.
//!
//! `write` stages one DNLOAD block, pads a short final block to a whole
//! 32-byte MRAM page with the erased value and programs it; the first
//! program error latches. `readSpan` clamps a DFU_UPLOAD to the bytes
//! accepted so far. `workerStep` commits the slot header once, after the
//! host's end-of-download, with a sequence one past the other slot's.
//!
//! The programmer is a parameter so the tests can drive the sequence with a
//! fake: it needs `image(target, offset, bytes) u16`, `commit(target,
//! img_len, seq) u16` and `otherSeq(target) u32`.

const std = @import("std");

/// wTransferSize: bytes per DFU block.
pub const block_bytes: u32 = 64;
/// MRAM programs whole 32-byte pages.
const page_mask: u32 = 0x1F;
/// MRAM's erased value, used to pad a short final block.
const erased_byte: u8 = 0xFF;
/// `k_ra8_ok`.
pub const ok: u16 = 0;

/// The C `ra8_dfu_slot_t` values a device may target.
pub const Slot = enum(u8) {
    a = 0,
    b = 1,
};

/// The part of the target slot a DFU_UPLOAD block reads.
pub const Span = struct {
    offset: u32,
    len: u32,
};

pub const State = struct {
    target: Slot = .b,
    prepared: bool = false,
    manifest: bool = false,
    committed: bool = false,
    img_len: u32 = 0,
    writes: u32 = 0,
    prog_err: u16 = ok,
    stage: [block_bytes]u8 = undefined,

    /// Ignores anything that is not slot A or B, as the C did.
    pub fn setTarget(self: *State, raw: u8) void {
        self.target = std.meta.intToEnum(Slot, raw) catch return;
    }

    /// An empty block is the host's end-of-download.
    pub fn write(self: *State, programmer: anytype, block_number: u32, data: []const u8) void {
        if (data.len == 0) {
            self.manifest = true;
            return;
        }
        const len: u32 = @intCast(@min(data.len, block_bytes));
        @memcpy(self.stage[0..len], data[0..len]);
        const padded = (len + page_mask) & ~page_mask;
        @memset(self.stage[len..padded], erased_byte);
        const offset = block_number *% block_bytes;
        const err = programmer.image(self.target, offset, self.stage[0..padded]);
        if (err != ok) {
            self.prog_err = err;
            return;
        }
        self.writes +%= 1;
        self.img_len = @max(self.img_len, offset +% padded);
    }

    /// Null once `block_number` is past the image.
    pub fn readSpan(self: *const State, block_number: u32, length: u32) ?Span {
        const offset = block_number *% block_bytes;
        if (offset >= self.img_len) return null;
        return .{ .offset = offset, .len = @min(self.img_len - offset, length) };
    }

    pub fn mediaOk(self: *const State) bool {
        return self.prog_err == ok;
    }

    pub fn workerStep(self: *State, programmer: anytype) u16 {
        if (!self.manifest or self.committed or !self.prepared or self.prog_err != ok) {
            return self.prog_err;
        }
        const seq = programmer.otherSeq(self.target) +% 1;
        const err = programmer.commit(self.target, self.img_len, seq);
        if (err != ok) self.prog_err = err;
        self.committed = true;
        return err;
    }
};
