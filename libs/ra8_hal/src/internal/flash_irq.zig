//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! MRAM IRQ, range and status logic (RA8FW-754 follow-on, RA8FW-758): the
//! IRQ source to enable-bit map, the ISR dispatch order, the code-region
//! range check, blank-check regions and the status decode (HUM Ch 59).
//! Register access goes through a `hw` value keyed by MRMS offset.

pub const base: usize = 0x4013C000;

pub const off = struct {
    pub const mrcraeint: u16 = 0x0014;
    pub const mrcraes: u16 = 0x0018;
    pub const mrcrtea: u16 = 0x001C;
    pub const mrcrdea: u16 = 0x0020;
    pub const mreraint: u16 = 0x0034;
    pub const mreraes: u16 = 0x0038;
    pub const mrertea: u16 = 0x003C;
    pub const mrerdea: u16 = 0x0040;
    pub const mastat: u16 = 0x2010;
    pub const mpaeint: u16 = 0x2014;
    pub const mrdyie: u16 = 0x2018;
    pub const mstatr: u16 = 0x2080;
    pub const mentryr: u16 = 0x2084;
    pub const mrcbprot0: u16 = 0x3008;
    pub const mrcbprot1: u16 = 0x300C;
    pub const mrcps: u16 = 0x3010;
    pub const mrcpaeint: u16 = 0x3014;
    pub const mrcpea: u16 = 0x3018;
};

pub const src = struct {
    pub const code_ecc_ted: u8 = 0;
    pub const code_ecc_dec: u8 = 1;
    pub const extra_ecc_ted: u8 = 2;
    pub const extra_ecc_dec: u8 = 3;
    pub const program_err: u8 = 4;
    pub const extra_err: u8 = 5;
    pub const extra_cmdlk: u8 = 6;
    pub const extra_ready: u8 = 7;
    pub const count: u8 = 8;
};

pub const block_size: u32 = 32;
pub const write_size: u32 = 32;
pub const code_start: u64 = 0x02000000;
pub const code_size: u64 = 0x00100000;
pub const extra_start: u64 = 0x02E07600;
pub const extra_size: u64 = 0x00010400;
pub const ofs_start: u64 = 0x02C9F000;
pub const ofs_size: u64 = 0x00001000;
pub const mrcps_errors: u8 = 0x03;

/// ra8_flash_isr_event_t.
pub const Event = extern struct {
    src: u8,
    fault_addr: u32,
    status_word: u32,
    user_ctx: ?*anyopaque,
};

/// ra8_flash_status_t: seven flags.
pub const Status = extern struct {
    programming_busy: bool,
    erase_busy: bool,
    illegal_command: bool,
    voltage_error: bool,
    sector_protected: bool,
    program_error: bool,
    ecc_error: bool,
};

comptime {
    if (@offsetOf(Event, "fault_addr") != 4 or @offsetOf(Event, "status_word") != 8) @compileError("ra8_flash_isr_event_t layout");
    if (@sizeOf(Status) != 7) @compileError("ra8_flash_status_t layout");
}

const Rmw = struct { reg: u16, bit: u8 };

fn rmwTarget(s: u8) ?Rmw {
    return switch (s) {
        src.code_ecc_ted => .{ .reg = off.mrcraeint, .bit = 0x02 },
        src.code_ecc_dec => .{ .reg = off.mrcraeint, .bit = 0x01 },
        src.extra_ecc_ted => .{ .reg = off.mreraint, .bit = 0x02 },
        src.extra_ecc_dec => .{ .reg = off.mreraint, .bit = 0x01 },
        src.extra_err => .{ .reg = off.mpaeint, .bit = 0x08 },
        src.extra_cmdlk => .{ .reg = off.mpaeint, .bit = 0x10 },
        else => null,
    };
}

/// ECC and extra-area error sources read-modify-write one bit; the program
/// error and ready enables are whole-register writes. False for a bad src.
pub fn setIrq(hw: anytype, s: u8, enable: bool) bool {
    if (rmwTarget(s)) |t| {
        const v = hw.read8(t.reg);
        hw.write8(t.reg, if (enable) v | t.bit else v & ~t.bit);
        return true;
    }
    switch (s) {
        src.program_err => hw.write8(off.mrcpaeint, if (enable) 0x80 else 0),
        src.extra_ready => hw.write8(off.mrdyie, if (enable) 0x01 else 0),
        else => return false,
    }
    return true;
}

fn dispatchEcc(hw: anytype, sink: anytype, status_off: u16, ted_off: u16, dec_off: u16, ted: u8, dec: u8) u32 {
    var n: u32 = 0;
    const status = hw.read8(status_off);
    if (status & 0x02 != 0) {
        sink.deliver(ted, hw.read32(ted_off), status);
        n += 1;
    }
    if (status & 0x01 != 0) {
        sink.deliver(dec, hw.read32(dec_off), status);
        n += 1;
    }
    if (status != 0) hw.write8(status_off, 0);
    return n;
}

/// Code ECC, extra ECC, program error (MRCPS errors written back; `ram_w1c`
/// also clears them for host RAM), MASTAT MREAE/CMDLK, then MSTATR MRDY.
pub fn dispatch(hw: anytype, sink: anytype, ram_w1c: bool) u32 {
    var n = dispatchEcc(hw, sink, off.mrcraes, off.mrcrtea, off.mrcrdea, src.code_ecc_ted, src.code_ecc_dec);
    n += dispatchEcc(hw, sink, off.mreraes, off.mrertea, off.mrerdea, src.extra_ecc_ted, src.extra_ecc_dec);
    const mrcps = hw.read8(off.mrcps);
    if (mrcps & mrcps_errors != 0) {
        sink.deliver(src.program_err, hw.read32(off.mrcpea), mrcps);
        hw.write8(off.mrcps, mrcps_errors);
        if (ram_w1c) hw.write8(off.mrcps, hw.read8(off.mrcps) & ~mrcps_errors);
        n += 1;
    }
    const mastat = hw.read8(off.mastat);
    if (mastat & 0x08 != 0) {
        sink.deliver(src.extra_err, 0, mastat);
        n += 1;
    }
    if (mastat & 0x10 != 0) {
        sink.deliver(src.extra_cmdlk, 0, mastat);
        n += 1;
    }
    const mstatr = hw.read32(off.mstatr);
    if (mstatr & 0x8000 != 0) {
        sink.deliver(src.extra_ready, 0, mstatr);
        n += 1;
    }
    return n;
}

/// Block aligned and fully inside the code region (window checked apart).
pub fn codeRangeOk(address: usize, total_len: u64) bool {
    if (address & (block_size - 1) != 0) return false;
    if (address < code_start) return false;
    return @as(u64, address) + total_len <= code_start + code_size;
}

fn within(address: usize, len: u32, start: u64, size: u64) bool {
    return address >= start and @as(u64, address) + len <= start + size;
}

/// Blank checks may read the code, extra or OFS regions.
pub fn blankRegionOk(address: usize, len: u32) bool {
    return within(address, len, code_start, code_size) or
        within(address, len, extra_start, extra_size) or
        within(address, len, ofs_start, ofs_size);
}

pub fn decodeStatus(mrcps: u8, mastat: u8, mentryr: u16, mstatr: u32, prot0: u16, prot1: u16) Status {
    const busy = (mrcps & 0x80 != 0) or (mentryr & 0x0080 != 0);
    return .{
        .programming_busy = busy,
        .erase_busy = busy,
        .illegal_command = (mastat & 0x10 != 0) or (mstatr & 0x0080_0000 != 0),
        .voltage_error = mstatr & 0x0010_0000 != 0,
        .sector_protected = (prot0 & 1 == 0) or (prot1 & 1 == 0),
        .program_error = mrcps & 0x01 != 0,
        .ecc_error = mrcps & 0x02 != 0,
    };
}
