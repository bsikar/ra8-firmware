//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the fault block: the crash-log state machine, the CRC that
//! protects it, the record layouts the C ABI pins, and the SCB register map.
//!
//! The state machine runs against an ordinary local record rather than the
//! `.noinit` instance, which is why it is a parameter everywhere below.

const std = @import("std");
const testing = std.testing;

const crashlog = @import("fault_crashlog");
const crc32 = @import("fault_crc32");
const record = @import("fault_record");
const scb = @import("fault_scb");

fn emptyRecord() crashlog.Record {
    return std.mem.zeroes(crashlog.Record);
}

fn sampleFault(pc: u32) record.Last {
    var last = std.mem.zeroes(record.Last);
    last.magic = record.magic.valid;
    last.exc_number = 3;
    last.frame.pc = pc;
    last.frame.lr = 0xDEAD_BEEF;
    last.diag.cfsr = 0x0000_0082;
    last.frame_ptr = 0x2000_1000;
    return last;
}

// ---- layouts the C ABI pins ----------------------------------------------

test "record layouts match the C header" {
    try testing.expectEqual(@as(usize, 32), @sizeOf(record.Frame));
    try testing.expectEqual(@as(usize, 32), @sizeOf(record.Diagnostics));
    try testing.expectEqual(@as(usize, 0), @offsetOf(record.Last, "magic"));
    try testing.expectEqual(@as(usize, 4), @offsetOf(record.Last, "exc_number"));
    try testing.expectEqual(@as(usize, 8), @offsetOf(record.Last, "frame"));
    try testing.expectEqual(@as(usize, 40), @offsetOf(record.Last, "diag"));
}

test "the crash record fits the linker reserve with magic and crc outside the payload" {
    try testing.expect(@sizeOf(crashlog.Record) <= crashlog.limits.reserve_bytes);
    try testing.expectEqual(@as(usize, 0), @offsetOf(crashlog.Record, "magic"));
    try testing.expectEqual(@as(usize, 4), @offsetOf(crashlog.Record, "crc"));
    try testing.expectEqual(@as(usize, 8), @offsetOf(crashlog.Record, "boot_loops"));
}

test "diagnosticsFrom puts bfar before mmfar, which is not the SCB order" {
    const diag = record.diagnosticsFrom(1, 2, 3, 4, 5, 6, 7, 8);
    try testing.expectEqual(@as(u32, 4), diag.mmfar);
    try testing.expectEqual(@as(u32, 5), diag.bfar);
    try testing.expect(@offsetOf(record.Diagnostics, "bfar") < @offsetOf(record.Diagnostics, "mmfar"));
    try testing.expect(@offsetOf(scb.FaultStatus, "mmfar") < @offsetOf(scb.FaultStatus, "bfar"));
}

// ---- the CRC --------------------------------------------------------------

test "crc32 matches known reflected CRC-32 vectors" {
    try testing.expectEqual(@as(u32, 0), crc32.compute(""));
    try testing.expectEqual(@as(u32, 0xCBF4_3926), crc32.compute("123456789"));
    try testing.expectEqual(@as(u32, 0x414F_A339), crc32.compute("The quick brown fox jumps over the lazy dog"));
}

test "crc32 agrees with the standard implementation on record-sized data" {
    var bytes: [128]u8 = undefined;
    for (&bytes, 0..) |*b, i| b.* = @truncate(i *% 7 +% 3);
    try testing.expectEqual(std.hash.Crc32.hash(&bytes), crc32.compute(&bytes));
}

// ---- validity -------------------------------------------------------------

test "a zeroed record is not valid" {
    var rec = emptyRecord();
    try testing.expect(!crashlog.isValid(&rec));
}

test "a recorded fault validates and reads back" {
    var rec = emptyRecord();
    const fault = sampleFault(0x0800_1234);
    crashlog.recordFault(&rec, &fault);

    try testing.expect(crashlog.isValid(&rec));
    try testing.expectEqual(crashlog.magic.valid, rec.magic);
    try testing.expectEqual(@as(u32, 1), rec.boot_loops);

    var out = emptyRecord();
    try testing.expect(crashlog.peek(&rec, &out));
    try testing.expectEqual(@as(u32, 0x0800_1234), out.fault.frame.pc);
    try testing.expectEqual(@as(u32, 0xDEAD_BEEF), out.fault.frame.lr);
}

test "the magic alone does not validate a record" {
    var rec = emptyRecord();
    const fault = sampleFault(0x1000);
    crashlog.recordFault(&rec, &fault);
    rec.crc +%= 1;
    try testing.expect(!crashlog.isValid(&rec));
    try testing.expect(!crashlog.peek(&rec, &rec));
}

test "the crc alone does not validate a record" {
    var rec = emptyRecord();
    const fault = sampleFault(0x1000);
    crashlog.recordFault(&rec, &fault);
    rec.magic = 0;
    try testing.expect(!crashlog.isValid(&rec));
}

test "a payload edit after the write invalidates the record" {
    var rec = emptyRecord();
    const fault = sampleFault(0x2000);
    crashlog.recordFault(&rec, &fault);
    rec.fault.frame.pc = 0x3000;
    try testing.expect(!crashlog.isValid(&rec));
}

// ---- the loop counter -----------------------------------------------------

test "boot_loops counts consecutive recorded faults" {
    var rec = emptyRecord();
    const fault = sampleFault(0x4000);
    for (1..6) |expected| {
        crashlog.recordFault(&rec, &fault);
        try testing.expectEqual(@as(u32, @intCast(expected)), rec.boot_loops);
    }
}

test "boot_loops saturates instead of wrapping past the threshold" {
    var rec = emptyRecord();
    const fault = sampleFault(0x5000);
    crashlog.recordFault(&rec, &fault);
    rec.boot_loops = crashlog.limits.loops_max;
    rec.crc = crashlog.payloadCrc(&rec);

    crashlog.recordFault(&rec, &fault);
    try testing.expectEqual(crashlog.limits.loops_max, rec.boot_loops);
    try testing.expect(crashlog.isValid(&rec));
}

test "an invalid prior record restarts the count at one" {
    var rec = emptyRecord();
    const fault = sampleFault(0x6000);
    crashlog.recordFault(&rec, &fault);
    crashlog.recordFault(&rec, &fault);
    try testing.expectEqual(@as(u32, 2), rec.boot_loops);

    rec.magic = 0xFFFF_FFFF; // cold-boot SRAM garbage
    crashlog.recordFault(&rec, &fault);
    try testing.expectEqual(@as(u32, 1), rec.boot_loops);
}

// ---- safe mode and claim --------------------------------------------------

test "safe mode is requested only once the count exceeds the threshold" {
    var rec = emptyRecord();
    const fault = sampleFault(0x7000);
    for (0..crashlog.limits.loop_threshold) |_| {
        crashlog.recordFault(&rec, &fault);
        try testing.expect(!crashlog.safeModeRequested(&rec));
    }
    crashlog.recordFault(&rec, &fault);
    try testing.expect(crashlog.safeModeRequested(&rec));
}

test "claim clears the record and the guard" {
    var rec = emptyRecord();
    const fault = sampleFault(0x8000);
    for (0..5) |_| crashlog.recordFault(&rec, &fault);
    try testing.expect(crashlog.safeModeRequested(&rec));

    crashlog.claim(&rec);
    try testing.expect(!crashlog.isValid(&rec));
    try testing.expect(!crashlog.safeModeRequested(&rec));
    try testing.expectEqual(@as(u32, 0), rec.boot_loops);

    // A claim is not a wipe: the post-mortem payload is still readable by a
    // debugger, it just no longer validates.
    try testing.expectEqual(@as(u32, 0x8000), rec.fault.frame.pc);
}

test "wipe zeroes every byte of the record" {
    var rec = emptyRecord();
    const fault = sampleFault(0x9000);
    crashlog.recordFault(&rec, &fault);
    crashlog.wipe(&rec);

    const bytes: [*]const u8 = @ptrCast(&rec);
    for (bytes[0..@sizeOf(crashlog.Record)]) |byte| {
        try testing.expectEqual(@as(u8, 0), byte);
    }
}

test "peek leaves the stored record untouched" {
    var rec = emptyRecord();
    const fault = sampleFault(0xA000);
    crashlog.recordFault(&rec, &fault);
    const before = rec;

    var out = emptyRecord();
    try testing.expect(crashlog.peek(&rec, &out));
    try testing.expectEqual(before.crc, rec.crc);
    try testing.expectEqual(before.magic, rec.magic);
    try testing.expectEqual(before.boot_loops, rec.boot_loops);
}

// ---- the register map -----------------------------------------------------

test "SCB addresses match the Arm v8-M PPB window" {
    try testing.expectEqual(@as(usize, 0xE000_ED08), scb.addr.vtor);
    try testing.expectEqual(@as(usize, 0xE000_ED28), scb.addr.cfsr);
    try testing.expectEqual(@as(usize, 0xE000_ED2C), scb.addr.hfsr);
    try testing.expectEqual(@as(usize, 0xE000_ED30), scb.addr.dfsr);
    try testing.expectEqual(@as(usize, 0xE000_ED34), scb.addr.mmfar);
    try testing.expectEqual(@as(usize, 0xE000_ED38), scb.addr.bfar);
    try testing.expectEqual(@as(usize, 0xE000_ED3C), scb.addr.afsr);
    try testing.expectEqual(@as(usize, 0xE000_EDE4), scb.addr.sfsr);
    try testing.expectEqual(@as(usize, 0xE000_EDE8), scb.addr.sfar);
    try testing.expectEqual(@as(usize, 0xE000_EDFC), scb.addr.demcr);
}

test "TRCENA is DEMCR bit 24" {
    try testing.expectEqual(@as(u32, 0x0100_0000), scb.bits.demcr_trcena);
}
