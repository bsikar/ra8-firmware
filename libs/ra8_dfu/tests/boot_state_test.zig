//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig-native cover of the trial boot-state record, CRC/invariant validation,
//! dual-copy arbitration, and torn-write fault injection.

const std = @import("std");
const boot_state = @import("boot_state");
const image = @import("image");
const slot = @import("slot");

const Record = boot_state.Record;
const layout = boot_state.layout;

const FlashModel = struct {
    copies: [2][@sizeOf(Record)]u8 = @splat(@splat(layout.erased_byte)),
    ops: usize = 0,
    fail_on_op: ?usize = null,
    /// When true on a failing `programPage`, corrupt half the page after erasing
    /// to mimic power cutting off mid-program.
    partial_on_fail: bool = false,

    pub fn programPage(self: *FlashModel, addr: u32, erased: []const u8, body: []const u8) error{Flash}!void {
        const idx: usize = switch (addr) {
            layout.copy0_addr => 0,
            layout.copy1_addr => 1,
            else => return error.Flash,
        };
        const current_op = self.ops;
        self.ops += 1;
        if (self.fail_on_op) |target_op| {
            if (current_op == target_op) {
                @memcpy(&self.copies[idx], erased);
                if (self.partial_on_fail) {
                    @memcpy(self.copies[idx][0..12], body[0..12]);
                }
                return error.Flash;
            }
        }
        @memcpy(&self.copies[idx], erased);
        @memcpy(&self.copies[idx], body);
    }

    fn asRecord(self: *const FlashModel, idx: usize) *const Record {
        return @ptrCast(@alignCast(&self.copies[idx]));
    }
};

test "boot_state record matches one 32-byte MRAM page" {
    try std.testing.expectEqual(@as(usize, 32), @sizeOf(Record));
    try std.testing.expectEqual(@as(usize, image.layout.page_size), @sizeOf(Record));
    try std.testing.expectEqual(@as(usize, 28), @offsetOf(Record, "crc32"));
}

test "build rejects zero write_seq and validates normal records" {
    try std.testing.expectError(error.InvalidArg, boot_state.build(.a, 3, false, 1, 0));

    const rec = try boot_state.build(.b, 2, true, 9, 4);
    try std.testing.expect(boot_state.isValid(&rec));
    try std.testing.expectEqual(@backingInt(slot.Slot.b), rec.trial_slot);
    try std.testing.expectEqual(@as(u8, 2), rec.tries_left);
    try std.testing.expectEqual(@as(u8, 1), rec.confirmed);
    try std.testing.expectEqual(@as(u32, 9), rec.slot_seq);
    try std.testing.expectEqual(@as(u32, 4), rec.write_seq);
}

test "isValid rejects corrupt magic, zero write_seq, bad enum/bool, and bad CRC" {
    var rec = try boot_state.build(.a, 3, false, 7, 1);

    var bad_magic = rec;
    bad_magic.magic ^= 1;
    try std.testing.expect(!boot_state.isValid(&bad_magic));

    var bad_seq = rec;
    bad_seq.write_seq = 0;
    try std.testing.expect(!boot_state.isValid(&bad_seq));

    var bad_slot = rec;
    bad_slot.trial_slot = 3;
    bad_slot.crc32 = boot_state.payloadCrc(&bad_slot);
    try std.testing.expect(!boot_state.isValid(&bad_slot));

    var bad_conf = rec;
    bad_conf.confirmed = 2;
    bad_conf.crc32 = boot_state.payloadCrc(&bad_conf);
    try std.testing.expect(!boot_state.isValid(&bad_conf));

    rec.tries_left ^= 0x01;
    try std.testing.expect(!boot_state.isValid(&rec));
}

test "pick selects higher write_seq valid copy and tie goes to copy 0" {
    const erased_bytes: [32]u8 = @splat(0xFF);
    const erased: Record = std.mem.bytesToValue(Record, &erased_bytes);
    try std.testing.expectEqual(@as(?boot_state.PickResult, null), boot_state.pick(&erased, &erased));

    const r1 = try boot_state.build(.a, 3, false, 10, 1);
    const r2 = try boot_state.build(.a, 2, false, 10, 2);

    const p0 = boot_state.pick(&r1, &erased).?;
    try std.testing.expectEqual(@as(u8, 0), p0.copy_index);
    try std.testing.expectEqual(@as(u32, 1), p0.record.write_seq);

    const p1 = boot_state.pick(&erased, &r2).?;
    try std.testing.expectEqual(@as(u8, 1), p1.copy_index);
    try std.testing.expectEqual(@as(u32, 2), p1.record.write_seq);

    const newer_1 = boot_state.pick(&r1, &r2).?;
    try std.testing.expectEqual(@as(u8, 1), newer_1.copy_index);
    try std.testing.expectEqual(@as(u8, 2), newer_1.record.tries_left);

    const tie = boot_state.pick(&r2, &r2).?;
    try std.testing.expectEqual(@as(u8, 0), tie.copy_index);
}

test "ping-pong writes survive torn erase and torn program without losing active copy" {
    var model = FlashModel{};

    // First write lands on copy 0 (write_seq = 1).
    const first = try boot_state.writeRecord(
        &model,
        model.asRecord(0),
        model.asRecord(1),
        .b,
        3,
        false,
        12,
    );
    try std.testing.expectEqual(@as(u8, 0), first.copy_index);
    try std.testing.expectEqual(@as(u32, 1), first.record.write_seq);

    // Second write hits a torn write on copy 1 (partial bytes after erase).
    model.fail_on_op = model.ops;
    model.partial_on_fail = true;
    try std.testing.expectError(
        error.Flash,
        boot_state.writeRecord(&model, model.asRecord(0), model.asRecord(1), .b, 2, false, 12),
    );
    // Copy 0 remained untouched and still validates cleanly!
    const recovered = boot_state.pick(model.asRecord(0), model.asRecord(1)).?;
    try std.testing.expectEqual(@as(u8, 0), recovered.copy_index);
    try std.testing.expectEqual(@as(u8, 3), recovered.record.tries_left);
    try std.testing.expectEqual(@as(u32, 1), recovered.record.write_seq);

    // Retry succeeds cleanly on copy 1 with write_seq = 2.
    model.fail_on_op = null;
    const second = try boot_state.writeRecord(
        &model,
        model.asRecord(0),
        model.asRecord(1),
        .b,
        2,
        false,
        12,
    );
    try std.testing.expectEqual(@as(u8, 1), second.copy_index);
    try std.testing.expectEqual(@as(u32, 2), second.record.write_seq);

    // Corrupting copy 1 falls back cleanly to copy 0.
    model.copies[1][5] ^= 0x80;
    const after_rot = boot_state.pick(model.asRecord(0), model.asRecord(1)).?;
    try std.testing.expectEqual(@as(u8, 0), after_rot.copy_index);
    try std.testing.expectEqual(@as(u32, 1), after_rot.record.write_seq);

    // Erase all wipes both copies cleanly.
    try boot_state.eraseAll(&model);
    try std.testing.expectEqual(
        @as(?boot_state.PickResult, null),
        boot_state.pick(model.asRecord(0), model.asRecord(1)),
    );
}
