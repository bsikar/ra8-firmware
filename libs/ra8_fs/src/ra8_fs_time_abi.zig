//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The wall-clock seam and the packing of a reading into on-disk fields: the
//! clamp policy, the FAT and exFAT packers, the installed-clock state behind
//! them, and the entry stampers the create/write/rename paths call. Ported
//! whole from `ra8_fs_fat_time.c` (RA8FW-1120); the C file is deleted.

const std = @import("std");
const c = @import("fs_c.zig").c;

const ok: u16 = c.k_ra8_ok;
const err_invalid: u16 = c.k_ra8_err_invalid_arg;

// On-disk layout constants (ra8_fs_fat_time_internal.h).
const epoch_year: u32 = @intCast(c.k_fs_time_epoch_year);
const year_max: u32 = @intCast(c.k_fs_time_year_max);
const month_min: u32 = @intCast(c.k_fs_time_month_min);
const month_max: u32 = @intCast(c.k_fs_time_month_max);
const day_min: u32 = @intCast(c.k_fs_time_day_min);
const day_max: u32 = @intCast(c.k_fs_time_day_max);
const hour_max: u32 = @intCast(c.k_fs_time_hour_max);
const minute_max: u32 = @intCast(c.k_fs_time_minute_max);
const second_max: u32 = @intCast(c.k_fs_time_second_max);
const centi_max: u32 = @intCast(c.k_fs_time_centi_max);
const second_div: u32 = @intCast(c.k_fs_time_second_div);
const centi_per_second: u32 = @intCast(c.k_fs_centi_per_second);

const utc_span_min: i16 = @intCast(c.k_fs_utc_span_min);
const utc_span_max: i16 = @intCast(c.k_fs_utc_span_max);
const utc_step_min: i32 = @intCast(c.k_fs_utc_step_min);
const utc_unknown: u8 = @intCast(c.k_fs_utc_unknown);
const utc_valid_bit: u32 = @intCast(c.k_fs_utc_valid_bit);
const utc_field_mask: u32 = @intCast(c.k_fs_utc_field_mask);

const date_shift_year: u5 = @intCast(c.k_fs_date_shift_year);
const date_shift_month: u5 = @intCast(c.k_fs_date_shift_month);
const time_shift_hour: u5 = @intCast(c.k_fs_time_shift_hour);
const time_shift_minute: u5 = @intCast(c.k_fs_time_shift_minute);
const xf_shift_date: u5 = @intCast(c.k_fs_xf_shift_date);
const xf_shift_month: u5 = @intCast(c.k_fs_xf_shift_month);
const xf_shift_year: u5 = @intCast(c.k_fs_xf_shift_year);

const off_crt_time_tenth: usize = @intCast(c.k_dir_off_crt_time_tenth);
const off_crt_time: usize = @intCast(c.k_dir_off_crt_time);
const off_crt_date: usize = @intCast(c.k_dir_off_crt_date);
const off_lst_acc_date: usize = @intCast(c.k_dir_off_lst_acc_date);
const off_wrt_time: usize = @intCast(c.k_dir_off_wrt_time);
const off_wrt_date: usize = @intCast(c.k_dir_off_wrt_date);
const off_file_ctime: usize = @intCast(c.k_exfat_off_file_ctime);
const off_file_mtime: usize = @intCast(c.k_exfat_off_file_mtime);
const off_file_atime: usize = @intCast(c.k_exfat_off_file_atime);
const off_file_c10ms: usize = @intCast(c.k_exfat_off_file_c10ms);
const off_file_m10ms: usize = @intCast(c.k_exfat_off_file_m10ms);
const off_file_cutc: usize = @intCast(c.k_exfat_off_file_cutc);
const off_file_mutc: usize = @intCast(c.k_exfat_off_file_mutc);
const off_file_autc: usize = @intCast(c.k_exfat_off_file_autc);

extern fn priv_wr16(p: [*c]u8, v: u16) callconv(.c) void;
extern fn priv_wr32(p: [*c]u8, v: u32) callconv(.c) void;

/// One reading already packed into the two 16-bit FAT words plus tenths.
/// Every entry stamping call produces one, so a file's create, write and
/// access fields describe the same instant.
pub const FatStamp = struct {
    date: u16,
    time: u16,
    tenth: u8,
};

/// One reading packed into exFAT's 32-bit stamp plus the 10 ms increment and
/// the UtcOffset byte the FAT layout has no room for.
pub const ExfatStamp = struct {
    stamp: u32,
    inc10: u8,
    utc: u8,
};

// The installed calendar binding, copied from the caller's struct so a
// binding built as a compound literal or on the installing function's stack
// stays valid. `s_clock_bound` is kept separate so "no clock" is one
// predictable branch rather than a null test on a function pointer.
var s_clock: c.ra8_fs_clock_t = std.mem.zeroes(c.ra8_fs_clock_t);
var s_clock_bound: bool = false;

/// Clamp `v` into the closed range [lo, hi]. The whole clamping policy in one
/// place: a clock that returns nonsense degrades the stamp, it does not fail
/// the write.
fn clamp_u32(v: u32, lo: u32, hi: u32) u32 {
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

/// Clamp every field of a broken-down time into its on-disk range, in place:
/// year into [1980, 2107], month/day/hour/minute/second/centisecond into
/// their legal spans. `utc_offset_min` is left untouched; `utc_byte`
/// validates it at pack time.
pub fn clamp_datetime(t: *c.ra8_fs_datetime_t) void {
    t.year = @intCast(clamp_u32(t.year, epoch_year, year_max));
    t.month = @intCast(clamp_u32(t.month, month_min, month_max));
    t.day = @intCast(clamp_u32(t.day, day_min, day_max));
    t.hour = @intCast(clamp_u32(t.hour, 0, hour_max));
    t.minute = @intCast(clamp_u32(t.minute, 0, minute_max));
    t.second = @intCast(clamp_u32(t.second, 0, second_max));
    t.centisecond = @intCast(clamp_u32(t.centisecond, 0, centi_max));
}

/// Read the installed clock, or hand back the FAT epoch. The out struct is
/// epoch-initialised first, so every path (no binding, a `now()` that fails,
/// a `now()` that succeeds) leaves a fully populated, clamped reading.
/// Returns whether the fields came from a real clock; the caller must not
/// attach a UTC offset to an epoch placeholder.
pub fn now_or_epoch(out: *c.ra8_fs_datetime_t) bool {
    out.* = std.mem.zeroes(c.ra8_fs_datetime_t);
    out.year = @intCast(epoch_year);
    out.month = @intCast(month_min);
    out.day = @intCast(day_min);
    var real = false;
    if (s_clock_bound) {
        if (s_clock.now) |now_fn| {
            var got: c.ra8_fs_datetime_t = std.mem.zeroes(c.ra8_fs_datetime_t);
            if (now_fn(s_clock.ctx, &got) == c.k_ra8_ok) {
                out.* = got;
                real = true;
            }
        }
    }
    clamp_datetime(out);
    return real;
}

/// Count the 10 ms increments the 2-second stamp granularity discards: the
/// odd second contributes a whole 100 and the hundredths contribute the rest.
pub fn tenths_of(t: *const c.ra8_fs_datetime_t) u8 {
    const odd: u32 = @as(u32, t.second) % second_div;
    return @intCast((odd * centi_per_second) + @as(u32, t.centisecond));
}

/// Encode a UTC offset in minutes as an exFAT `UtcOffset` byte: bit 7 is
/// OffsetValid, bits 6:0 the offset in 15-minute two's-complement steps. An
/// offset that is not a whole 15-minute step, or that falls outside
/// UTC-12:00..UTC+14:00, cannot be expressed, so it is recorded as "not
/// valid" rather than rounded into a lie about which zone the civil time
/// belongs to.
pub fn utc_byte(minutes: i16) u8 {
    if (minutes < utc_span_min) {
        return utc_unknown;
    }
    if (minutes > utc_span_max) {
        return utc_unknown;
    }
    if (@rem(@as(i32, minutes), utc_step_min) != 0) {
        return utc_unknown;
    }
    const steps: i32 = @divTrunc(@as(i32, minutes), utc_step_min);
    return @intCast(utc_valid_bit | (@as(u32, @bitCast(steps)) & utc_field_mask));
}

/// Pack an already-clamped reading into FAT's two words plus the tenths
/// (MS FAT spec sec 6): date `(year - 1980) << 9 | month << 5 | day`, time
/// `hour << 11 | minute << 5 | second / 2`. Takes the reading as an argument
/// rather than reading the clock, so the clock-driven stamps and the
/// caller-chosen `utime` timestamps share one packing.
pub fn fat_pack(t: *const c.ra8_fs_datetime_t, out: *FatStamp) void {
    const years: u32 = @as(u32, t.year) - epoch_year;
    out.date = @intCast((years << date_shift_year) |
        (@as(u32, t.month) << date_shift_month) | @as(u32, t.day));
    out.time = @intCast((@as(u32, t.hour) << time_shift_hour) |
        (@as(u32, t.minute) << time_shift_minute) |
        (@as(u32, t.second) / second_div));
    out.tenth = tenths_of(t);
}

/// Pack the installed clock's reading into FAT's two words plus tenths. FAT
/// has no offset field, so a real reading and the epoch placeholder pack
/// identically.
pub fn fat_stamp_now(out: *FatStamp) void {
    var t: c.ra8_fs_datetime_t = std.mem.zeroes(c.ra8_fs_datetime_t);
    _ = now_or_epoch(&t);
    fat_pack(&t, out);
}

/// Pack an already-clamped reading into exFAT's stamp plus side fields
/// (exFAT spec sec 7.4.8): the low 16 bits are FAT's time word and the high
/// 16 bits FAT's date word, which is why the shifts are shared with
/// `fat_pack`. `real` selects whether the UtcOffset is honoured or recorded
/// as unknown: the epoch placeholder is not a real instant, so saying "this
/// is UTC" about it would be a fabrication.
pub fn exfat_pack(t: *const c.ra8_fs_datetime_t, real: bool, out: *ExfatStamp) void {
    const years: u32 = @as(u32, t.year) - epoch_year;
    const date: u32 = (years << xf_shift_year) |
        (@as(u32, t.month) << xf_shift_month) | @as(u32, t.day);
    const time: u32 = (@as(u32, t.hour) << time_shift_hour) |
        (@as(u32, t.minute) << time_shift_minute) |
        (@as(u32, t.second) / second_div);
    out.stamp = @intCast((date << xf_shift_date) | time);
    out.inc10 = tenths_of(t);
    out.utc = if (real) utc_byte(t.utc_offset_min) else utc_unknown;
}

/// Pack the installed clock's reading into exFAT's stamp plus side fields,
/// forwarding whether the reading is real so the epoch placeholder never
/// carries a fabricated UtcOffset.
pub fn exfat_stamp_now(out: *ExfatStamp) void {
    var t: c.ra8_fs_datetime_t = std.mem.zeroes(c.ra8_fs_datetime_t);
    const real = now_or_epoch(&t);
    exfat_pack(&t, real, out);
}

/// Stamp a fresh FAT directory entry's create, write, and access fields.
pub export fn priv_fat_entry_stamp_create(entry: [*c]u8) callconv(.c) void {
    var s = FatStamp{ .date = 0, .time = 0, .tenth = 0 };
    fat_stamp_now(&s);
    entry[off_crt_time_tenth] = s.tenth;
    priv_wr16(entry + off_crt_time, s.time);
    priv_wr16(entry + off_crt_date, s.date);
    priv_wr16(entry + off_lst_acc_date, s.date);
    priv_wr16(entry + off_wrt_time, s.time);
    priv_wr16(entry + off_wrt_date, s.date);
}

/// Stamp a FAT entry's write and access fields.
pub export fn priv_fat_entry_stamp_write(entry: [*c]u8) callconv(.c) void {
    var s = FatStamp{ .date = 0, .time = 0, .tenth = 0 };
    fat_stamp_now(&s);
    priv_wr16(entry + off_wrt_time, s.time);
    priv_wr16(entry + off_wrt_date, s.date);
    priv_wr16(entry + off_lst_acc_date, s.date);
}

/// Stamp a FAT entry's access field.
pub export fn priv_fat_entry_stamp_access(entry: [*c]u8) callconv(.c) void {
    var s = FatStamp{ .date = 0, .time = 0, .tenth = 0 };
    fat_stamp_now(&s);
    priv_wr16(entry + off_lst_acc_date, s.date);
}

/// Stamp a fresh exFAT file entry's create, write, and access stamps.
pub export fn priv_exfat_file_stamp_create(file_entry: [*c]u8) callconv(.c) void {
    var s = ExfatStamp{ .stamp = 0, .inc10 = 0, .utc = 0 };
    exfat_stamp_now(&s);
    priv_wr32(file_entry + off_file_ctime, s.stamp);
    priv_wr32(file_entry + off_file_mtime, s.stamp);
    priv_wr32(file_entry + off_file_atime, s.stamp);
    file_entry[off_file_c10ms] = s.inc10;
    file_entry[off_file_m10ms] = s.inc10;
    file_entry[off_file_cutc] = s.utc;
    file_entry[off_file_mutc] = s.utc;
    file_entry[off_file_autc] = s.utc;
}

/// Stamp an exFAT file entry's write and access stamps.
pub export fn priv_exfat_file_stamp_write(file_entry: [*c]u8) callconv(.c) void {
    var s = ExfatStamp{ .stamp = 0, .inc10 = 0, .utc = 0 };
    exfat_stamp_now(&s);
    priv_wr32(file_entry + off_file_mtime, s.stamp);
    priv_wr32(file_entry + off_file_atime, s.stamp);
    file_entry[off_file_m10ms] = s.inc10;
    file_entry[off_file_mutc] = s.utc;
    file_entry[off_file_autc] = s.utc;
}

/// Stamp an exFAT file entry's access stamp.
pub export fn priv_exfat_file_stamp_access(file_entry: [*c]u8) callconv(.c) void {
    var s = ExfatStamp{ .stamp = 0, .inc10 = 0, .utc = 0 };
    exfat_stamp_now(&s);
    priv_wr32(file_entry + off_file_atime, s.stamp);
    file_entry[off_file_autc] = s.utc;
}

/// Stamp a FAT entry's individual times from caller-chosen readings; a null
/// reading leaves that field untouched. Each reading is clamped first.
pub export fn priv_fat_entry_set_times(
    entry: [*c]u8,
    create: [*c]const c.ra8_fs_datetime_t,
    modify: [*c]const c.ra8_fs_datetime_t,
    access: [*c]const c.ra8_fs_datetime_t,
) callconv(.c) void {
    if (create != null) {
        var t: c.ra8_fs_datetime_t = create.*;
        clamp_datetime(&t);
        var s = FatStamp{ .date = 0, .time = 0, .tenth = 0 };
        fat_pack(&t, &s);
        entry[off_crt_time_tenth] = s.tenth;
        priv_wr16(entry + off_crt_time, s.time);
        priv_wr16(entry + off_crt_date, s.date);
    }
    if (modify != null) {
        var t: c.ra8_fs_datetime_t = modify.*;
        clamp_datetime(&t);
        var s = FatStamp{ .date = 0, .time = 0, .tenth = 0 };
        fat_pack(&t, &s);
        priv_wr16(entry + off_wrt_time, s.time);
        priv_wr16(entry + off_wrt_date, s.date);
    }
    if (access != null) {
        var t: c.ra8_fs_datetime_t = access.*;
        clamp_datetime(&t);
        var s = FatStamp{ .date = 0, .time = 0, .tenth = 0 };
        fat_pack(&t, &s);
        priv_wr16(entry + off_lst_acc_date, s.date);
    }
}

/// Stamp an exFAT file entry's individual times from caller-chosen readings;
/// a null reading leaves that stamp untouched. Each reading is clamped first,
/// and every reading is a real instant, so its UtcOffset is honoured.
pub export fn priv_exfat_file_set_times(
    file_entry: [*c]u8,
    create: [*c]const c.ra8_fs_datetime_t,
    modify: [*c]const c.ra8_fs_datetime_t,
    access: [*c]const c.ra8_fs_datetime_t,
) callconv(.c) void {
    if (create != null) {
        var t: c.ra8_fs_datetime_t = create.*;
        clamp_datetime(&t);
        var s = ExfatStamp{ .stamp = 0, .inc10 = 0, .utc = 0 };
        exfat_pack(&t, true, &s);
        priv_wr32(file_entry + off_file_ctime, s.stamp);
        file_entry[off_file_c10ms] = s.inc10;
        file_entry[off_file_cutc] = s.utc;
    }
    if (modify != null) {
        var t: c.ra8_fs_datetime_t = modify.*;
        clamp_datetime(&t);
        var s = ExfatStamp{ .stamp = 0, .inc10 = 0, .utc = 0 };
        exfat_pack(&t, true, &s);
        priv_wr32(file_entry + off_file_mtime, s.stamp);
        file_entry[off_file_m10ms] = s.inc10;
        file_entry[off_file_mutc] = s.utc;
    }
    if (access != null) {
        var t: c.ra8_fs_datetime_t = access.*;
        clamp_datetime(&t);
        var s = ExfatStamp{ .stamp = 0, .inc10 = 0, .utc = 0 };
        exfat_pack(&t, true, &s);
        priv_wr32(file_entry + off_file_atime, s.stamp);
        file_entry[off_file_autc] = s.utc;
    }
}

/// Install (or remove) the calendar source used to stamp files. The binding
/// is copied; a null pointer goes back to the epoch default, and a non-null
/// binding whose `now` is null is rejected without touching the installed one.
pub export fn ra8_fs_set_clock(clock: [*c]const c.ra8_fs_clock_t) callconv(.c) u16 {
    if (clock == null) {
        s_clock_bound = false;
        s_clock = std.mem.zeroes(c.ra8_fs_clock_t);
        return ok;
    }
    if (clock.*.now == null) {
        return err_invalid;
    }
    s_clock = clock.*;
    s_clock_bound = true;
    return ok;
}
