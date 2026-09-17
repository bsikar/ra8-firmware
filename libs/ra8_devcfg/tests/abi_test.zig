//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig-native tests for the C ABI membrane: the injected store seam, the
//! two-copy resolver, the header-last commit, the VCOM gate and the
//! null-argument guards. A RAM mock stands in for the durable medium exactly
//! as the C host suite's mock does, including the 0xFF blank window and a
//! write budget that models a power cut mid-commit.

const std = @import("std");
const abi = @import("abi");

const region: u32 = 448; // copy1_off (0x100) + slot (192)
const blank_byte: u8 = 0xFF;
const copy0_off: u32 = 0x40;
const slot_bytes: u32 = 192;
const off_magic: u32 = 0;
const off_schema: u32 = 4;
const off_reclen: u32 = 6;
const off_serial: u32 = 32;

const ok: abi.RawErr = 0;
const err_not_initialized: abi.RawErr = 0x10F;
const err_hw: abi.RawErr = 0x204;
const err_out_of_range: abi.RawErr = 0x208;
const err_validation_failed: abi.RawErr = 0x501;
const err_null_ptr: abi.RawErr = 0x504;

var mem: [region]u8 = @splat(blank_byte);
var write_budget: i32 = -1;

fn mockRead(offset: u32, dst: [*]u8, len: u32) callconv(.c) abi.RawErr {
    if (offset + len > region) return err_out_of_range;
    @memcpy(dst[0..len], mem[offset..][0..len]);
    return ok;
}

fn mockWrite(offset: u32, src: [*]const u8, len: u32) callconv(.c) abi.RawErr {
    if (offset + len > region) return err_out_of_range;
    if (write_budget == 0) return err_hw;
    if (write_budget > 0) write_budget -= 1;
    @memcpy(mem[offset..][0..len], src[0..len]);
    return ok;
}

fn failingRead(offset: u32, dst: [*]u8, len: u32) callconv(.c) abi.RawErr {
    _ = offset;
    _ = dst;
    _ = len;
    return err_hw;
}

const mock: abi.Store = .{ .read = mockRead, .write = mockWrite };

// The library logs through the repo's `ra8_log` externs, which live in
// `ra8_core` on a real link. A test binary links neither, so stand them up
// here; the C host suite's link brings the real ones.
export fn ra8_log_emit_error(log_tag: [*:0]const u8, message: [*:0]const u8) void {
    _ = log_tag;
    _ = message;
}
export fn ra8_log_emit_warn(log_tag: [*:0]const u8, message: [*:0]const u8) void {
    _ = log_tag;
    _ = message;
}
export fn ra8_log_emit_error_val(log_tag: [*:0]const u8, message: [*:0]const u8, value: u32) void {
    _ = log_tag;
    _ = message;
    _ = value;
}

fn blankMock() void {
    @memset(&mem, blank_byte);
    write_budget = -1;
    abi.ra8_devcfg_reset();
}

fn makeRecord(vcom: u16, flags: u32) abi.Record {
    var rec: abi.Record = .{};
    @memcpy(rec.body.serial[0..7], "SN-0001");
    @memcpy(rec.body.panel_serial[0..8], "PANEL-01");
    @memcpy(rec.body.panel_lut_id[0..4], "M641");
    for (&rec.body.touch_cal, 0..) |*byte, i| byte.* = @intCast(i);
    rec.body.mfg_date = 20260723;
    rec.body.device_key_id = 0xDEADBEEF;
    rec.body.hw_rev = 2;
    rec.body.fixture_id = 7;
    rec.body.panel_vcom_mv = vcom;
    rec.flags = flags;
    return rec;
}

fn pokeCopy0(field_off: u32, value: u8) void {
    mem[copy0_off + field_off] = value;
}

test "commit then load reproduces every body field" {
    blankMock();
    const rec = makeRecord(1530, abi.flag_provisioned | abi.flag_vcom_valid);
    try std.testing.expectEqual(ok, abi.ra8_devcfg_commit(&mock, &rec));
    try std.testing.expectEqual(ok, abi.ra8_devcfg_load(&mock));

    var body: ?*const abi.Body = null;
    try std.testing.expectEqual(ok, abi.ra8_devcfg_get_body(&body));
    const b = body.?;
    try std.testing.expectEqualSlices(u8, &rec.body.serial, &b.serial);
    try std.testing.expectEqualSlices(u8, &rec.body.panel_serial, &b.panel_serial);
    try std.testing.expectEqualSlices(u8, &rec.body.panel_lut_id, &b.panel_lut_id);
    try std.testing.expectEqualSlices(u8, &rec.body.touch_cal, &b.touch_cal);
    try std.testing.expectEqual(@as(u32, 20260723), b.mfg_date);
    try std.testing.expectEqual(@as(u32, 0xDEADBEEF), b.device_key_id);
    try std.testing.expectEqual(@as(u16, 2), b.hw_rev);
    try std.testing.expectEqual(@as(u16, 7), b.fixture_id);
    try std.testing.expectEqual(@as(u16, 1530), b.panel_vcom_mv);
    try std.testing.expect(!abi.ra8_devcfg_is_blank());
}

test "a blank window resolves to UNPROVISIONED" {
    blankMock();
    try std.testing.expectEqual(err_validation_failed, abi.ra8_devcfg_load(&mock));
    try std.testing.expect(abi.ra8_devcfg_is_blank());
}

test "a corrupt magic word rejects the copy" {
    blankMock();
    const rec = makeRecord(1530, abi.flag_vcom_valid);
    try std.testing.expectEqual(ok, abi.ra8_devcfg_commit(&mock, &rec));
    abi.ra8_devcfg_reset();
    pokeCopy0(off_magic, mem[copy0_off] ^ 0x01);
    try std.testing.expectEqual(err_validation_failed, abi.ra8_devcfg_load(&mock));
}

test "a future schema rejects the copy" {
    blankMock();
    const rec = makeRecord(1530, abi.flag_vcom_valid);
    _ = abi.ra8_devcfg_commit(&mock, &rec);
    pokeCopy0(off_schema, 99);
    try std.testing.expectEqual(err_validation_failed, abi.ra8_devcfg_load(&mock));
}

test "a wrong record_len rejects the copy" {
    blankMock();
    const rec = makeRecord(1530, abi.flag_vcom_valid);
    _ = abi.ra8_devcfg_commit(&mock, &rec);
    pokeCopy0(off_reclen, mem[copy0_off + off_reclen] ^ 0x01);
    try std.testing.expectEqual(err_validation_failed, abi.ra8_devcfg_load(&mock));
}

test "a single-bit body corruption fails the CRC gate" {
    blankMock();
    const rec = makeRecord(1530, abi.flag_vcom_valid);
    _ = abi.ra8_devcfg_commit(&mock, &rec);
    pokeCopy0(off_serial, mem[copy0_off + off_serial] ^ 0x01);
    try std.testing.expectEqual(err_validation_failed, abi.ra8_devcfg_load(&mock));
    try std.testing.expect(abi.ra8_devcfg_is_blank());
}

test "both copies valid: the higher sequence wins, in both directions" {
    blankMock();
    const r0 = makeRecord(1530, abi.flag_vcom_valid);
    const r1 = makeRecord(1670, abi.flag_vcom_valid);
    var mv: u16 = 0;

    try std.testing.expectEqual(ok, abi.ra8_devcfg_commit(&mock, &r0)); // copy0, seq1
    try std.testing.expectEqual(ok, abi.ra8_devcfg_commit(&mock, &r1)); // copy1, seq2
    try std.testing.expectEqual(ok, abi.ra8_devcfg_load(&mock));
    try std.testing.expectEqual(ok, abi.ra8_devcfg_get_vcom_mv(&mv));
    try std.testing.expectEqual(@as(u16, 1670), mv);

    // The third commit overwrites the older slot, so copy 0 becomes newest.
    try std.testing.expectEqual(ok, abi.ra8_devcfg_commit(&mock, &r0)); // copy0, seq3
    try std.testing.expectEqual(ok, abi.ra8_devcfg_load(&mock));
    try std.testing.expectEqual(ok, abi.ra8_devcfg_get_vcom_mv(&mv));
    try std.testing.expectEqual(@as(u16, 1530), mv);
}

test "only copy 1 valid: copy 1 is used" {
    blankMock();
    const r0 = makeRecord(1530, abi.flag_vcom_valid);
    const r1 = makeRecord(1670, abi.flag_vcom_valid);
    _ = abi.ra8_devcfg_commit(&mock, &r0);
    _ = abi.ra8_devcfg_commit(&mock, &r1);
    @memset(mem[copy0_off..][0..slot_bytes], blank_byte);
    var mv: u16 = 0;
    try std.testing.expectEqual(ok, abi.ra8_devcfg_load(&mock));
    try std.testing.expectEqual(ok, abi.ra8_devcfg_get_vcom_mv(&mv));
    try std.testing.expectEqual(@as(u16, 1670), mv);
}

test "only copy 0 valid: copy 0 is used" {
    blankMock();
    const r0 = makeRecord(1530, abi.flag_vcom_valid);
    _ = abi.ra8_devcfg_commit(&mock, &r0);
    var mv: u16 = 0;
    try std.testing.expectEqual(ok, abi.ra8_devcfg_load(&mock));
    try std.testing.expectEqual(ok, abi.ra8_devcfg_get_vcom_mv(&mv));
    try std.testing.expectEqual(@as(u16, 1530), mv);
}

test "a torn header write leaves the previous copy the sole survivor" {
    blankMock();
    const good = makeRecord(1530, abi.flag_vcom_valid);
    const next = makeRecord(1670, abi.flag_vcom_valid);
    try std.testing.expectEqual(ok, abi.ra8_devcfg_commit(&mock, &good));

    write_budget = 1; // allow the body write, fail the header write
    try std.testing.expectEqual(err_hw, abi.ra8_devcfg_commit(&mock, &next));

    write_budget = -1;
    var mv: u16 = 0;
    try std.testing.expectEqual(ok, abi.ra8_devcfg_load(&mock));
    try std.testing.expectEqual(ok, abi.ra8_devcfg_get_vcom_mv(&mv));
    try std.testing.expectEqual(@as(u16, 1530), mv);
}

test "a body write fault propagates and leaves nothing loadable" {
    blankMock();
    const rec = makeRecord(1530, abi.flag_vcom_valid);
    write_budget = 0;
    try std.testing.expectEqual(err_hw, abi.ra8_devcfg_commit(&mock, &rec));
    write_budget = -1;
    try std.testing.expectEqual(err_validation_failed, abi.ra8_devcfg_load(&mock));
}

test "a read fault marks both copies absent rather than failing hard" {
    blankMock();
    const faulty: abi.Store = .{ .read = failingRead, .write = mockWrite };
    try std.testing.expectEqual(err_validation_failed, abi.ra8_devcfg_load(&faulty));
    try std.testing.expect(abi.ra8_devcfg_is_blank());
}

test "get_vcom_mv refuses before a load" {
    abi.ra8_devcfg_reset();
    var mv: u16 = 0;
    try std.testing.expectEqual(err_not_initialized, abi.ra8_devcfg_get_vcom_mv(&mv));
}

test "get_vcom_mv refuses when the valid flag is clear" {
    blankMock();
    const rec = makeRecord(1530, abi.flag_provisioned);
    _ = abi.ra8_devcfg_commit(&mock, &rec);
    try std.testing.expectEqual(ok, abi.ra8_devcfg_load(&mock));
    var mv: u16 = 0;
    try std.testing.expectEqual(err_validation_failed, abi.ra8_devcfg_get_vcom_mv(&mv));
}

test "get_vcom_mv refuses values outside the plausible window" {
    for ([_]u16{ 100, 5000, 0, 0xFFFF }) |bad| {
        blankMock();
        const rec = makeRecord(bad, abi.flag_vcom_valid);
        _ = abi.ra8_devcfg_commit(&mock, &rec);
        _ = abi.ra8_devcfg_load(&mock);
        var mv: u16 = 0;
        try std.testing.expectEqual(err_validation_failed, abi.ra8_devcfg_get_vcom_mv(&mv));
        try std.testing.expectEqual(@as(u16, 0), mv); // untouched on error
    }
}

test "get_vcom_mv accepts both window edges" {
    for ([_]u16{ 500, 4000 }) |edge| {
        blankMock();
        const rec = makeRecord(edge, abi.flag_vcom_valid);
        _ = abi.ra8_devcfg_commit(&mock, &rec);
        _ = abi.ra8_devcfg_load(&mock);
        var mv: u16 = 0;
        try std.testing.expectEqual(ok, abi.ra8_devcfg_get_vcom_mv(&mv));
        try std.testing.expectEqual(edge, mv);
    }
}

test "every public entry rejects a null argument" {
    const no_read: abi.Store = .{ .read = null, .write = mockWrite };
    const no_write: abi.Store = .{ .read = mockRead, .write = null };
    const rec = makeRecord(1530, abi.flag_vcom_valid);

    try std.testing.expectEqual(err_null_ptr, abi.ra8_devcfg_load(null));
    try std.testing.expectEqual(err_null_ptr, abi.ra8_devcfg_load(&no_read));
    try std.testing.expectEqual(err_null_ptr, abi.ra8_devcfg_get_vcom_mv(null));
    try std.testing.expectEqual(err_null_ptr, abi.ra8_devcfg_get_body(null));
    try std.testing.expectEqual(err_null_ptr, abi.ra8_devcfg_commit(null, &rec));
    try std.testing.expectEqual(err_null_ptr, abi.ra8_devcfg_commit(&no_read, &rec));
    try std.testing.expectEqual(err_null_ptr, abi.ra8_devcfg_commit(&no_write, &rec));
    try std.testing.expectEqual(err_null_ptr, abi.ra8_devcfg_commit(&mock, null));
}

test "the null guard runs ahead of the not-initialized guard" {
    abi.ra8_devcfg_reset();
    try std.testing.expectEqual(err_null_ptr, abi.ra8_devcfg_get_body(null));
    var body: ?*const abi.Body = null;
    try std.testing.expectEqual(err_not_initialized, abi.ra8_devcfg_get_body(&body));
    try std.testing.expect(body == null);
}

test "reset returns the module to the never-loaded state" {
    blankMock();
    const rec = makeRecord(1530, abi.flag_vcom_valid);
    _ = abi.ra8_devcfg_commit(&mock, &rec);
    try std.testing.expectEqual(ok, abi.ra8_devcfg_load(&mock));
    try std.testing.expect(!abi.ra8_devcfg_is_blank());

    abi.ra8_devcfg_reset();
    try std.testing.expect(abi.ra8_devcfg_is_blank());
    var mv: u16 = 0;
    try std.testing.expectEqual(err_not_initialized, abi.ra8_devcfg_get_vcom_mv(&mv));
    var body: ?*const abi.Body = null;
    try std.testing.expectEqual(err_not_initialized, abi.ra8_devcfg_get_body(&body));
}

test "the cached body follows the newest committed record" {
    blankMock();
    const r0 = makeRecord(1530, abi.flag_vcom_valid);
    var r1 = makeRecord(1670, abi.flag_vcom_valid);
    r1.body.hw_rev = 9;
    _ = abi.ra8_devcfg_commit(&mock, &r0);
    _ = abi.ra8_devcfg_commit(&mock, &r1);
    _ = abi.ra8_devcfg_load(&mock);

    var body: ?*const abi.Body = null;
    try std.testing.expectEqual(ok, abi.ra8_devcfg_get_body(&body));
    try std.testing.expectEqual(@as(u16, 9), body.?.hw_rev);
    try std.testing.expectEqual(@as(u16, 1670), body.?.panel_vcom_mv);
}

test "commit writes the body before the header" {
    blankMock();
    const rec = makeRecord(1530, abi.flag_vcom_valid);
    write_budget = 1; // only the first write lands
    _ = abi.ra8_devcfg_commit(&mock, &rec);
    // The body span carries the record; the header page is still blank.
    try std.testing.expectEqual(blank_byte, mem[copy0_off + off_magic]);
    try std.testing.expect(mem[copy0_off + off_serial] != blank_byte);
}
