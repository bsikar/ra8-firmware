//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! CANFD MRAM ECC logic (RA8FW-793), ported from ra8_cnecc.c. ECCMB0/1 sit
//! at 0x4036F200 + 0x100*n; masks follow ra8_cnecc_regs.h (HUM Ch 42.2).

pub const codes = struct {
    pub const ok: u16 = 0;
    pub const invalid_arg: u16 = 0x103;
    pub const not_initialized: u16 = 0x10F;
    pub const crc_mismatch: u16 = 0x405;
    pub const null_ptr: u16 = 0x504;
};

pub const count: u8 = 2;
pub const base0: usize = 0x4036_F200;
pub const stride: usize = 0x100;

pub const off = struct {
    pub const ctl: usize = 0x00;
    pub const tmc: usize = 0x04;
    pub const ted: usize = 0x0C;
    pub const ead: usize = 0x10;
};

pub const ctl = struct {
    pub const ecemf: u32 = 0x0000_0001;
    pub const ecer1f: u32 = 0x0000_0002;
    pub const ecer2f: u32 = 0x0000_0004;
    pub const ec1edic: u32 = 0x0000_0008;
    pub const ec2edic: u32 = 0x0000_0010;
    pub const ec1ecp: u32 = 0x0000_0020;
    pub const ecervf: u32 = 0x0000_0040;
    pub const ecovff: u32 = 0x0000_0800;
    pub const emca: u32 = 0x0000_C000;
    pub const emca_unlock: u32 = 0x0000_4000;
    pub const ecsedf0: u32 = 0x0001_0000;
    pub const ecdedf0: u32 = 0x0002_0000;
    pub const clear_all: u32 = 0x0000_0600;
    pub const irq_all: u32 = 0x0000_0018;
    pub const writable: u32 = 0x0000_C67F;
};

pub const tmc = struct {
    pub const ectmce: u16 = 0x0080;
    pub const test_disable: u16 = 0x8000;
    pub const test_enable: u16 = 0x8080;
    pub const test_subst: u16 = 0x8082;
};

pub const ead_mask: u32 = 0x3FF;
pub const mstp_ids = [2]u16{ (2 << 8) | 27, (2 << 8) | 26 };
pub const events = [2]u16{ 0x338, 0x339 };
pub const prio_max: u8 = 15;

pub const InstanceCfg = extern struct {
    correct_1bit: bool,
    irq_1bit: bool,
    irq_2bit: bool,
    enable: bool,
};

pub const Config = extern struct { instances: [2]InstanceCfg };

pub const Counters = extern struct {
    one_bit_count: u32 = 0,
    two_bit_count: u32 = 0,
    overflow_count: u32 = 0,
};

pub const Inject = extern struct { substitute: u32, one_bit_flip: bool };

pub const Status = extern struct {
    raw_ctl: u32,
    raw_tmc: u16,
    reserved0: u16,
    one_bit_count: u32,
    two_bit_count: u32,
    overflow_count: u32,
    last_addr: u16,
    err_present: bool,
    err_1bit: bool,
    err_2bit: bool,
    overflow: bool,
    addr_is_1bit: bool,
    addr_is_2bit: bool,
    judgment_active: bool,
    correct_enabled: bool,
    irq1_enabled: bool,
    irq2_enabled: bool,
    test_mode: bool,
    reserved1: bool,
};

pub const ErrorFn = *const fn (ctx: ?*anyopaque, instance: u8, is_2bit: bool, err_addr: u16) callconv(.C) void;

comptime {
    if (@sizeOf(InstanceCfg) != 4 or @sizeOf(Config) != 8) @compileError("Config");
    if (@sizeOf(Counters) != 12 or @sizeOf(Inject) != 8) @compileError("Counters/Inject");
    if (@sizeOf(Status) != 36 or @offsetOf(Status, "last_addr") != 20 or @offsetOf(Status, "test_mode") != 32)
        @compileError("Status");
}

pub fn base(instance: u8) usize {
    return base0 + @as(usize, instance) * stride;
}

pub fn ctlValue(c: InstanceCfg) u32 {
    var v: u32 = ctl.emca_unlock;
    if (c.irq_1bit) v |= ctl.ec1edic;
    if (c.irq_2bit) v |= ctl.ec2edic;
    if (!c.correct_1bit) v |= ctl.ec1ecp;
    if (c.enable) v |= ctl.ecervf;
    return v;
}

/// Keep the writable bits, never echo a W1C clear, splice the requested
/// bits and re-assert EMCA = 01b so ECERVF writes land.
pub fn ctlRmw(live: u32, new_bits: u32, mask: u32) u32 {
    const kept = live & ctl.writable & ~ctl.clear_all;
    const spliced = (kept & ~mask) | (new_bits & mask);
    return (spliced & ~ctl.emca) | ctl.emca_unlock;
}

pub fn irqBits(irq_1bit: bool, irq_2bit: bool) u32 {
    return (if (irq_1bit) ctl.ec1edic else 0) | (if (irq_2bit) ctl.ec2edic else 0);
}

fn has(v: u32, m: u32) bool {
    return v & m != 0;
}

pub fn decode(c: u32, t: u16, ead: u32, n: Counters) Status {
    return .{
        .raw_ctl = c,
        .raw_tmc = t,
        .reserved0 = 0,
        .one_bit_count = n.one_bit_count,
        .two_bit_count = n.two_bit_count,
        .overflow_count = n.overflow_count,
        .last_addr = @truncate(ead & ead_mask),
        .err_present = has(c, ctl.ecemf),
        .err_1bit = has(c, ctl.ecer1f),
        .err_2bit = has(c, ctl.ecer2f),
        .overflow = has(c, ctl.ecovff),
        .addr_is_1bit = has(c, ctl.ecsedf0),
        .addr_is_2bit = has(c, ctl.ecdedf0),
        .judgment_active = has(c, ctl.ecervf),
        .correct_enabled = !has(c, ctl.ec1ecp),
        .irq1_enabled = has(c, ctl.ec1edic),
        .irq2_enabled = has(c, ctl.ec2edic),
        .test_mode = t & tmc.ectmce != 0,
        .reserved1 = false,
    };
}

/// Reflected CRC32 (poly 0xEDB88320, seed and xorout 0xFFFFFFFF).
pub fn crc32(data: []const u8) u32 {
    var crc: u32 = 0xFFFF_FFFF;
    for (data) |b| {
        crc ^= b;
        for (0..8) |_| crc = (crc >> 1) ^ (if (crc & 1 != 0) @as(u32, 0xEDB8_8320) else 0);
    }
    return crc ^ 0xFFFF_FFFF;
}

pub fn computeCheck(addr: u32, len: u32) u16 {
    if (addr == 0) return codes.null_ptr;
    if (addr % 4 != 0 or len < 4) return codes.invalid_arg;
    return codes.ok;
}

pub fn alignedLen(len: u32) u32 {
    return len & ~@as(u32, 3);
}

/// Per-instance init writes: clear latched flags, program CTL, test off.
pub fn applyRegs(hw: anytype, instance: u8, c: InstanceCfg) void {
    const b = base(instance);
    hw.write32(b + off.ctl, ctl.clear_all);
    hw.write32(b + off.ctl, ctlValue(c));
    hw.write16(b + off.tmc, tmc.test_disable);
}

pub fn standbyRegs(hw: anytype, instance: u8) void {
    const b = base(instance);
    hw.write32(b + off.ctl, ctl.clear_all);
    hw.write32(b + off.ctl, ctl.emca_unlock);
}

pub fn injectRegs(hw: anytype, instance: u8, substitute: u32) void {
    const b = base(instance);
    hw.write16(b + off.tmc, tmc.test_disable);
    hw.write32(b + off.ted, substitute);
    hw.write16(b + off.tmc, tmc.test_enable);
    hw.write16(b + off.tmc, tmc.test_subst);
}

pub const State = struct {
    counts: [2]Counters = .{ .{}, .{} },
    mirror: [2]?*Counters = .{ null, null },
    cfg: Config = .{ .instances = [_]InstanceCfg{.{ .correct_1bit = false, .irq_1bit = false, .irq_2bit = false, .enable = false }} ** 2 },
    handler: ?ErrorFn = null,
    ctx: ?*anyopaque = null,
    initialized: bool = false,
    isr_attached: bool = false,

    pub fn clearCounts(self: *State, i: u8) void {
        self.counts[i] = .{};
    }

    pub fn resetCounts(self: *State, i: u8) void {
        self.clearCounts(i);
        if (self.mirror[i]) |m| m.* = .{};
    }

    pub fn setMirror(self: *State, i: u8, m: ?*Counters) void {
        self.mirror[i] = m;
        if (m) |p| p.* = self.counts[i];
    }

    pub fn dispatch(self: *State, i: u8, is_2bit: bool, addr: u16) void {
        if (i >= count) return;
        if (is_2bit) {
            self.counts[i].two_bit_count +%= 1;
            if (self.mirror[i]) |m| m.two_bit_count +%= 1;
        } else {
            self.counts[i].one_bit_count +%= 1;
            if (self.mirror[i]) |m| m.one_bit_count +%= 1;
        }
        if (self.handler) |f| f(self.ctx, i, is_2bit, addr);
    }

    pub fn dispatchOverflow(self: *State, i: u8) void {
        if (i >= count) return;
        self.counts[i].overflow_count +%= 1;
        if (self.mirror[i]) |m| m.overflow_count +%= 1;
    }
};
