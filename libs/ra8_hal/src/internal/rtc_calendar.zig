//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! RTC calendar set/get and the hh:mm:ss alarm (RA8FW-854, was part of
//! ra8_rtc.c). Exports live in src/rtc_calendar_abi.zig. `hw` supplies
//! wait(reg, mask, expect) and infoVal(msg, value).

pub const ok: u16 = 0;
pub const invalid_arg: u16 = 0x103;

/// `k_ra8_rtc_year_base`.
pub const year_base: u16 = 2000;
/// RCR2.START, bit 0 (HUM Ch 26.2.21 p 1232).
pub const rcr2_start: u8 = 0x01;
/// Alarm field ENB, bit 7 (HUM Ch 26.2.10-26.2.12).
pub const alarm_enb: u8 = 0x80;
pub const alarm_max_hour: u8 = 23;
pub const alarm_max_min: u8 = 59;
pub const alarm_max_sec: u8 = 59;

/// `ra8_rtc_datetime_t`.
pub const Datetime = extern struct {
    year: u16,
    month: u8,
    day: u8,
    weekday: u8,
    hour: u8,
    minute: u8,
    second: u8,
};

/// Counter and alarm block, RTC base +0x00..+0x1F (`r_rtc_regs_t`).
pub const Cal = extern struct {
    r64cnt: u8,
    _r0: u8,
    rseccnt: u8,
    _r1: u8,
    rmincnt: u8,
    _r2: u8,
    rhrcnt: u8,
    _r3: u8,
    rwkcnt: u8,
    _r4: u8,
    rdaycnt: u8,
    _r5: u8,
    rmoncnt: u8,
    _r6: u8,
    ryrcnt: u16,
    rsecar: u8,
    _r7: u8,
    rminar: u8,
    _r8: u8,
    rhrar: u8,
    _r9: u8,
    rwkar: u8,
    _ra: u8,
    rdayar: u8,
    _rb: u8,
    rmonar: u8,
    _rc: u8,
    ryrar: u16,
    ryraren: u8,
    _rd: u8,
};

comptime {
    if (@sizeOf(Datetime) != 8 or @offsetOf(Datetime, "second") != 7) @compileError("Datetime layout");
    if (@sizeOf(Cal) != 0x20) @compileError("Cal size");
    if (@offsetOf(Cal, "ryrcnt") != 0x0E or @offsetOf(Cal, "rsecar") != 0x10) @compileError("Cal counters");
    if (@offsetOf(Cal, "ryrar") != 0x1C or @offsetOf(Cal, "ryraren") != 0x1E) @compileError("Cal alarms");
}

pub fn bcdToBin(bcd: u8) u8 {
    return ((bcd >> 4) & 0x0F) * 10 + (bcd & 0x0F);
}

pub fn binToBcd(bin: u8) u8 {
    return ((bin / 10) << 4) | (bin % 10);
}

/// Count registers are written only while START = 0, then START is restored.
pub fn set(hw: anytype, cal: *volatile Cal, rcr2: *volatile u8, dt: *const Datetime) u16 {
    if (dt.year < year_base) return invalid_arg;
    const saved = rcr2.*;
    rcr2.* = saved & ~rcr2_start;
    hw.wait(rcr2, rcr2_start, 0);
    cal.rseccnt = binToBcd(dt.second);
    cal.rmincnt = binToBcd(dt.minute);
    cal.rhrcnt = binToBcd(dt.hour);
    cal.rwkcnt = dt.weekday;
    cal.rdaycnt = binToBcd(dt.day);
    cal.rmoncnt = binToBcd(dt.month);
    cal.ryrcnt = binToBcd(@truncate(dt.year - year_base));
    rcr2.* = saved | rcr2_start;
    hw.wait(rcr2, rcr2_start, rcr2_start);
    hw.infoVal("rtc_set year", dt.year);
    return ok;
}

pub fn get(cal: *const volatile Cal, out: *Datetime) void {
    out.second = bcdToBin(cal.rseccnt);
    out.minute = bcdToBin(cal.rmincnt);
    out.hour = bcdToBin(cal.rhrcnt);
    out.weekday = cal.rwkcnt;
    out.day = bcdToBin(cal.rdaycnt);
    out.month = bcdToBin(cal.rmoncnt);
    out.year = year_base + bcdToBin(@truncate(cal.ryrcnt));
}

/// hh:mm:ss match with the date, weekday, month and year alarms wildcarded.
pub fn setAlarm(cal: *volatile Cal, alarm: *const Datetime) u16 {
    if (alarm.hour > alarm_max_hour or alarm.minute > alarm_max_min or alarm.second > alarm_max_sec) {
        return invalid_arg;
    }
    cal.rsecar = binToBcd(alarm.second) | alarm_enb;
    cal.rminar = binToBcd(alarm.minute) | alarm_enb;
    cal.rhrar = binToBcd(alarm.hour) | alarm_enb;
    cal.rwkar = 0;
    cal.rdayar = 0;
    cal.rmonar = 0;
    cal.ryrar = 0;
    cal.ryraren = 0;
    return ok;
}
