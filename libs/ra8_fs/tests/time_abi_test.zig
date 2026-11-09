//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The wall-clock seam and the stamp packers (RA8FW-1120): the clamp policy,
//! the FAT and exFAT packings, the UtcOffset byte, and the installed-clock
//! state behind the entry stampers.

const std = @import("std");
const fs = @import("ra8_fs");
const time = fs.time;
const c = fs.c;
const fake = @import("fs_walker_fake.zig");

const expect = std.testing.expect;

const expectEqual = std.testing.expectEqual;

// A civil reading in ra8_fs_datetime_t shape.
fn dt(year: u16, month: u8, day: u8, hour: u8, minute: u8, second: u8, centi: u8, utc: i16) c.ra8_fs_datetime_t {
    return .{
        .year = year,
        .utc_offset_min = utc,
        .month = month,
        .day = day,
        .hour = hour,
        .minute = minute,
        .second = second,
        .centisecond = centi,
    };
}

// 2026-11-04 packed the FAT way, for assertions.
const ref_date: u16 = (46 << 9) | (11 << 5) | 4; // 0x5D64
const ref_time: u16 = (15 << 11) | (0 << 5) | 0; // 15:00:00.5 odd second -> seconds/2 = 0

test "clamp_datetime snaps every field into the on-disk range" {
    _ = fake.reset(0);
    var t = dt(1979, 13, 0, 24, 60, 60, 100, 300);
    time.clamp_datetime(&t);
    try expectEqual(@as(u16, 1980), t.year);
    try expectEqual(@as(u8, 12), t.month);
    try expectEqual(@as(u8, 1), t.day);
    try expectEqual(@as(u8, 23), t.hour);
    try expectEqual(@as(u8, 59), t.minute);
    try expectEqual(@as(u8, 59), t.second);
    try expectEqual(@as(u8, 99), t.centisecond);
    try expectEqual(@as(i16, 300), t.utc_offset_min); // untouched here; utc_byte rules it
    var late = dt(3000, 7, 4, 12, 0, 0, 0, 0);
    time.clamp_datetime(&late);
    try expectEqual(@as(u16, 2107), late.year);
}

test "fat_pack packs the MS FAT spec sec 6 fields" {
    const t = dt(2026, 11, 4, 15, 0, 1, 50, 90);
    var s = time.FatStamp{ .date = 0, .time = 0, .tenth = 0 };
    time.fat_pack(&t, &s);
    try expectEqual(ref_date, s.date);
    try expectEqual(ref_time, s.time);
    try expectEqual(@as(u8, 150), s.tenth); // odd second -> 100, plus 50 hundredths
    var epoch = time.FatStamp{ .date = 0, .time = 0, .tenth = 0 };
    const e = dt(1980, 1, 1, 0, 0, 0, 0, 0);
    time.fat_pack(&e, &epoch);
    try expectEqual(@as(u16, @intCast(c.k_fs_time_epoch_fat_date)), epoch.date);
    try expectEqual(@as(u16, 0), epoch.time);
    try expectEqual(@as(u8, 0), epoch.tenth);
}

test "utc_byte encodes whole 15-minute steps and refuses the rest" {
    try expectEqual(@as(u8, 0x86), time.utc_byte(90)); // +6 steps, OffsetValid
    try expectEqual(@as(u8, 0xB8), time.utc_byte(840)); // UTC+14:00, the span's top
    try expectEqual(@as(u8, 0xD0), time.utc_byte(-720)); // -48 steps, 7-bit two's complement
    try expectEqual(@as(u8, c.k_fs_utc_unknown), time.utc_byte(-750)); // below the span
    try expectEqual(@as(u8, c.k_fs_utc_unknown), time.utc_byte(33)); // not a 15-minute step
}

test "exfat_pack shares the date half and honours the offset only when real" {
    const t = dt(2026, 11, 4, 15, 0, 1, 50, 90);
    var real = time.ExfatStamp{ .stamp = 0, .inc10 = 0, .utc = 0 };
    time.exfat_pack(&t, true, &real);
    try expectEqual(@as(u32, (@as(u32, ref_date) << 16) | ref_time), real.stamp);
    try expectEqual(@as(u8, 150), real.inc10);
    try expectEqual(@as(u8, 0x86), real.utc);
    var epoch = time.ExfatStamp{ .stamp = 0, .inc10 = 0, .utc = 0 };
    const e = dt(1980, 1, 1, 0, 0, 0, 0, 90);
    time.exfat_pack(&e, false, &epoch); // placeholder: no fabricated UtcOffset
    try expectEqual(@as(u32, @as(u32, @intCast(c.k_fs_time_epoch_fat_date)) << 16), epoch.stamp);
    try expectEqual(@as(u8, 0), epoch.inc10);
    try expectEqual(@as(u8, c.k_fs_utc_unknown), epoch.utc);
}

fn fixed_now(ctx: ?*anyopaque, out: [*c]c.ra8_fs_datetime_t) callconv(.c) u16 {
    _ = ctx;
    out.* = dt(2026, 11, 4, 15, 30, 0, 0, 330);
    return c.k_ra8_ok;
}

fn failing_now(ctx: ?*anyopaque, out: [*c]c.ra8_fs_datetime_t) callconv(.c) u16 {
    _ = ctx;
    _ = out;
    return c.k_ra8_err_not_found;
}

test "the installed clock stamps; its failure and removal fall back to the epoch" {
    defer _ = time.ra8_fs_set_clock(null);
    // No binding: the epoch placeholder, never zero fields.
    _ = time.ra8_fs_set_clock(null);
    var s = time.FatStamp{ .date = 0, .time = 0, .tenth = 0 };
    time.fat_stamp_now(&s);
    try expectEqual(@as(u16, @intCast(c.k_fs_time_epoch_fat_date)), s.date);
    // A `now` of null is rejected, and the installed binding is unchanged.
    const bad = c.ra8_fs_clock_t{ .now = null, .ctx = null };
    try expectEqual(@as(u16, @intCast(c.k_ra8_err_invalid_arg)), time.ra8_fs_set_clock(&bad));
    // Install a working clock: every stamp comes from it, clamped or not.
    const good = c.ra8_fs_clock_t{ .now = fixed_now, .ctx = null };
    try expectEqual(@as(u16, @intCast(c.k_ra8_ok)), time.ra8_fs_set_clock(&good));
    time.fat_stamp_now(&s);
    try expectEqual(@as(u16, ref_date), s.date);
    try expectEqual(@as(u16, (15 << 11) | (30 << 5) | 0), s.time);
    // A `now()` that fails falls back to the epoch for that one stamp.
    const broken = c.ra8_fs_clock_t{ .now = failing_now, .ctx = null };
    try expectEqual(@as(u16, @intCast(c.k_ra8_ok)), time.ra8_fs_set_clock(&broken));
    time.fat_stamp_now(&s);
    try expectEqual(@as(u16, @intCast(c.k_fs_time_epoch_fat_date)), s.date);
    // Uninstall: back to the epoch default.
    try expectEqual(@as(u16, @intCast(c.k_ra8_ok)), time.ra8_fs_set_clock(null));
    time.fat_stamp_now(&s);
    try expectEqual(@as(u16, @intCast(c.k_fs_time_epoch_fat_date)), s.date);
}

test "the entry stampers write the packed fields at the spec offsets" {
    defer _ = time.ra8_fs_set_clock(null);
    _ = time.ra8_fs_set_clock(null);
    var entry: [32]u8 = undefined;
    @memset(&entry, 0);
    time.priv_fat_entry_stamp_create(&entry);
    try expectEqual(@as(u8, 0), entry[13]);
    try expectEqual(@as(u16, 0), std.mem.readInt(u16, entry[14..16], .little));
    try expectEqual(@as(u16, @intCast(c.k_fs_time_epoch_fat_date)), std.mem.readInt(u16, entry[16..18], .little));
    try expectEqual(@as(u16, @intCast(c.k_fs_time_epoch_fat_date)), std.mem.readInt(u16, entry[18..20], .little));
    try expectEqual(@as(u16, 0), std.mem.readInt(u16, entry[22..24], .little));
    try expectEqual(@as(u16, @intCast(c.k_fs_time_epoch_fat_date)), std.mem.readInt(u16, entry[24..26], .little));
    var fe: [32]u8 = undefined;
    @memset(&fe, 0);
    time.priv_exfat_file_stamp_access(&fe);
    try expectEqual(@as(u32, @as(u32, @intCast(c.k_fs_time_epoch_fat_date)) << 16), std.mem.readInt(u32, fe[16..20], .little));
    try expectEqual(@as(u8, c.k_fs_utc_unknown), fe[24]);
    // Caller-chosen times: modify only, clamped, UtcOffset honoured on exFAT.
    const tmod = dt(2026, 11, 4, 15, 0, 1, 50, 90);
    var e2: [32]u8 = undefined;
    @memset(&e2, 0);
    time.priv_fat_entry_set_times(&e2, null, &tmod, null);
    try expectEqual(ref_time, std.mem.readInt(u16, e2[22..24], .little));
    try expectEqual(ref_date, std.mem.readInt(u16, e2[24..26], .little));
    try expectEqual(@as(u8, 0), e2[13]); // create untouched
    var fe2: [32]u8 = undefined;
    @memset(&fe2, 0);
    time.priv_exfat_file_set_times(&fe2, null, &tmod, null);
    try expectEqual(@as(u32, (@as(u32, ref_date) << 16) | ref_time), std.mem.readInt(u32, fe2[12..16], .little));
    try expectEqual(@as(u8, 150), fe2[21]);
    try expectEqual(@as(u8, 0x86), fe2[23]);
    try expectEqual(@as(u32, 0), std.mem.readInt(u32, fe2[8..12], .little)); // create untouched
}
