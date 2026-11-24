//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Torn-write-safe A/B trial boot-state record in dedicated code-MRAM pages
//! outside the never-written bootloader and outside both slot bodies/headers.
//!
//! Each write targets the copy slot that does NOT hold the current valid
//! record, erases that page to 0xFF (invalidating it first), and programs
//! the new 32-byte record with a trailing CRC32. Power loss at any point
//! during erase or program leaves the other intact copy as the valid source.

const std = @import("std");
const crc32 = @import("crc32");
const image = @import("image");
const slot = @import("slot");

/// Dedicated rewritable code-MRAM region above Slot B (`0x020FE000..0x02100000`).
pub const layout = struct {
    /// Base of the 8 KiB reserved boot-state MRAM region.
    pub const base: u32 = 0x020F_E000;
    /// Total reserved bytes (8 KiB at the top of 1 MiB code-MRAM).
    pub const size: u32 = 0x0000_2000;
    /// Ping-pong copy 0 page address.
    pub const copy0_addr: u32 = base;
    /// Ping-pong copy 1 page address.
    pub const copy1_addr: u32 = base + image.layout.page_size;
    /// Exclusive upper bound passed to `ra8_flash_set_window`.
    pub const window_end: u32 = base + (2 * image.layout.page_size);
    /// Record magic, "RA8B".
    pub const magic: u32 = 0x5241_3842;
    /// Default number of trial attempts for a newly committed slot.
    pub const default_tries: u8 = 3;
    /// Bytes covered by `crc32` (everything preceding the trailing `crc32`).
    pub const crc_span: usize = 28;
    /// Number of ping-pong copies.
    pub const copy_count: usize = 2;
    /// Erased byte value in MRAM.
    pub const erased_byte: u8 = 0xFF;
};

/// 32-byte boot-state record (`ra8_dfu_boot_state_t` in `ra8_dfu.h`).
pub const Record = extern struct {
    magic: u32,
    write_seq: u32,
    trial_slot: u8,
    tries_left: u8,
    confirmed: u8,
    reserved0: u8,
    slot_seq: u32,
    reserved1: u32,
    reserved2: u32,
    reserved3: u32,
    crc32: u32,
};

comptime {
    std.debug.assert(@sizeOf(Record) == image.layout.page_size);
    std.debug.assert(@offsetOf(Record, "crc32") == layout.crc_span);
    std.debug.assert(layout.base + layout.size == 0x0210_0000);
    std.debug.assert(layout.copy1_addr == layout.copy0_addr + @sizeOf(Record));
    std.debug.assert(layout.window_end == layout.copy1_addr + @sizeOf(Record));
}

/// Outcome of arbitrating between the two ping-pong copies.
pub const PickResult = struct {
    copy_index: u8,
    record: Record,
};

/// Address of copy `index` (0 or 1).
pub fn copyAddr(index: u8) error{InvalidArg}!u32 {
    return switch (index) {
        0 => layout.copy0_addr,
        1 => layout.copy1_addr,
        else => error.InvalidArg,
    };
}

/// Compute the IEEE CRC32 over the 28-byte payload preceding `crc32`.
pub fn payloadCrc(rec: *const Record) u32 {
    const bytes: [*]const u8 = @ptrCast(rec);
    return crc32.compute(bytes[0..layout.crc_span]);
}

/// Whether `rec` passes magic, non-zero generation, enum/bool range, and CRC.
pub fn isValid(rec: *const Record) bool {
    if (rec.magic != layout.magic) return false;
    if (rec.write_seq == 0) return false;
    if (rec.trial_slot > @backingInt(slot.Slot.none)) return false;
    if (rec.confirmed > 1) return false;
    return payloadCrc(rec) == rec.crc32;
}

/// Construct a CRC-protected boot-state record with the supplied sequence.
pub fn build(
    trial_slot: slot.Slot,
    tries_left: u8,
    confirmed: bool,
    slot_seq: u32,
    write_seq: u32,
) error{InvalidArg}!Record {
    if (write_seq == 0) return error.InvalidArg;
    var rec = Record{
        .magic = layout.magic,
        .write_seq = write_seq,
        .trial_slot = @backingInt(trial_slot),
        .tries_left = tries_left,
        .confirmed = @intFromBool(confirmed),
        .reserved0 = 0,
        .slot_seq = slot_seq,
        .reserved1 = 0,
        .reserved2 = 0,
        .reserved3 = 0,
        .crc32 = 0,
    };
    rec.crc32 = payloadCrc(&rec);
    return rec;
}

/// Pick the newer valid copy, preferring copy 0 on a tie or `null` when both
/// copies fail validation.
pub fn pick(copy0: *const Record, copy1: *const Record) ?PickResult {
    const valid0 = isValid(copy0);
    const valid1 = isValid(copy1);
    if (!valid0 and !valid1) return null;
    if (valid0 and (!valid1 or copy0.write_seq >= copy1.write_seq)) {
        return .{ .copy_index = 0, .record = copy0.* };
    }
    return .{ .copy_index = 1, .record = copy1.* };
}

/// Target copy slot and generation counter for the next torn-write-safe update.
pub const WritePlan = struct {
    copy_index: u8,
    write_seq: u32,
};

/// Select the alternate copy so the currently active valid record is never
/// overwritten in place.
pub fn planWrite(copy0: *const Record, copy1: *const Record) WritePlan {
    if (pick(copy0, copy1)) |active| {
        const next_idx: u8 = if (active.copy_index == 0) 1 else 0;
        const next_seq: u32 = if (active.record.write_seq == std.math.maxInt(u32))
            1
        else
            active.record.write_seq + 1;
        return .{ .copy_index = next_idx, .write_seq = next_seq };
    }
    return .{ .copy_index = 0, .write_seq = 1 };
}

/// Write a new record to the inactive ping-pong copy via `flash.programPage`.
/// `flash` is expected to erase to `0xFF` before programming the page.
pub fn writeRecord(
    flash: anytype,
    copy0: *const Record,
    copy1: *const Record,
    trial_slot: slot.Slot,
    tries_left: u8,
    confirmed: bool,
    slot_seq: u32,
) !PickResult {
    const plan = planWrite(copy0, copy1);
    const rec = try build(trial_slot, tries_left, confirmed, slot_seq, plan.write_seq);
    const addr = try copyAddr(plan.copy_index);
    const erased: [image.layout.page_size]u8 = @splat(layout.erased_byte);
    try flash.programPage(addr, &erased, std.mem.asBytes(&rec));
    return .{ .copy_index = plan.copy_index, .record = rec };
}

/// Erase both ping-pong copies to all-ones.
pub fn eraseAll(flash: anytype) !void {
    const erased: [image.layout.page_size]u8 = @splat(layout.erased_byte);
    try flash.programPage(layout.copy0_addr, &erased, &erased);
    try flash.programPage(layout.copy1_addr, &erased, &erased);
}

/// Action required by `ra8_dfu_confirm` for the active boot-state pair.
pub const ConfirmOutcome = enum {
    already_confirmed,
    write_required,
};

/// Result of checking whether a trial record can be confirmed.
pub const ConfirmPlan = struct {
    outcome: ConfirmOutcome,
    record: Record,
};

/// Inspect `(copy0, copy1)` and decide how `ra8_dfu_confirm` should proceed.
/// Accepts `tries_left == 0` because the payload executes its last try while
/// `tries_left` already reads zero in MRAM.
pub fn planConfirm(copy0: *const Record, copy1: *const Record) error{ NotFound, InvalidState }!ConfirmPlan {
    const active = pick(copy0, copy1) orelse return error.NotFound;
    if (active.record.trial_slot > @backingInt(slot.Slot.b)) return error.InvalidState;
    if (active.record.confirmed != 0) {
        return .{ .outcome = .already_confirmed, .record = active.record };
    }
    return .{ .outcome = .write_required, .record = active.record };
}
