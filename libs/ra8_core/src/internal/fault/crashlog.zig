//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The cross-reset crash-log record: its layout, its integrity model, and
//! the write / validate / claim state machine over it.
//!
//! Every function here takes the record storage as a parameter rather than
//! reaching for a global, so the whole state machine is exercised in Zig
//! tests against an ordinary local. The membrane owns the one real instance
//! and its `.noinit` placement.
//!
//! THE WRITE ORDER IS THE INTEGRITY MODEL. `magic` is cleared before the
//! payload moves and written last once the CRC covers it, so a reset landing
//! mid-write leaves a record that fails validation rather than one that
//! looks intact. Callers reach the storage through a volatile pointer for
//! exactly that reason.

const crc32 = @import("fault_crc32");
const record = @import("fault_record");

/// The sentinel, "SAFE-BOOT".
pub const magic = struct {
    pub const valid: u32 = 0x5AFE_B007;
};

pub const limits = struct {
    /// Safe mode is requested once `boot_loops` exceeds this.
    pub const loop_threshold: u32 = 3;
    /// Bytes reserved for `.noinit` at the top of SRAM; must equal
    /// LENGTH(NOINIT) in the linker script.
    pub const reserve_bytes: usize = 256;
    /// `boot_loops` saturates here instead of wrapping, so a long crash loop
    /// can never roll the counter back under the threshold and silently
    /// clear the safe-mode request.
    pub const loops_max: u32 = 255;
};

/// The persisted record. `magic` is outside the CRC so the sentinel and the
/// checksum stay two independent integrity signals.
pub const Record = extern struct {
    magic: u32,
    crc: u32,
    boot_loops: u32,
    fault: record.Last,
};

comptime {
    if (@sizeOf(Record) > limits.reserve_bytes) {
        @compileError("ra8_crashlog Record must fit the linker NOINIT region");
    }
}

/// CRC over `boot_loops` to the end of the record, i.e. everything except
/// the leading sentinel and the checksum field itself.
pub fn payloadCrc(rec: *const volatile Record) u32 {
    const bytes: [*]const volatile u8 = @ptrCast(rec);
    return crc32.compute(bytes[@offsetOf(Record, "boot_loops")..@sizeOf(Record)]);
}

/// Trustworthy only when both signals agree. Random SRAM after a cold
/// power-on fails one or the other.
pub fn isValid(rec: *const volatile Record) bool {
    return rec.magic == magic.valid and payloadCrc(rec) == rec.crc;
}

/// Carry the prior crash count forward only if the existing record still
/// validates; a cold boot or a corrupted record restarts the count at 1.
pub fn nextLoops(rec: *const volatile Record) u32 {
    if (!isValid(rec)) return 1;
    const prior = rec.boot_loops;
    return if (prior < limits.loops_max) prior + 1 else limits.loops_max;
}

/// Write the payload, then validate LAST.
pub fn recordFault(rec: *volatile Record, decoded: *const volatile record.Last) void {
    const next = nextLoops(rec);
    rec.magic = 0; // invalidate for the write window
    rec.boot_loops = next;
    rec.fault = decoded.*;
    rec.crc = payloadCrc(rec);
    rec.magic = magic.valid;
}

/// Copy the record out when, and only when, it validates.
pub fn peek(rec: *const volatile Record, out: *Record) bool {
    if (!isValid(rec)) return false;
    out.* = rec.*;
    return true;
}

/// Consume the post-mortem and reset the loop guard.
pub fn claim(rec: *volatile Record) void {
    rec.magic = 0;
    rec.crc = 0;
    rec.boot_loops = 0;
}

pub fn safeModeRequested(rec: *const volatile Record) bool {
    if (!isValid(rec)) return false;
    return rec.boot_loops > limits.loop_threshold;
}

/// Zero every byte of the record. Host test support only.
pub fn wipe(rec: *volatile Record) void {
    const bytes: [*]volatile u8 = @ptrCast(rec);
    for (bytes[0..@sizeOf(Record)]) |*byte| byte.* = 0;
}
