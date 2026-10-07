//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! TrustZone SAU driver (Armv8-M SAU_CTRL/TYPE/RNR/RBAR/RLAR at
//! 0xE000EDD0). Port of ra8_sau.c (RA8FW-571); the C ABI lives in
//! sau_abi.zig. Arithmetic is 32-bit wrapping on purpose, so validation and
//! the programmed limit match the C bit for bit.

const builtin = @import("builtin");

pub const base_addr: usize = 0xE000EDD0;
pub const granule: u32 = 32;
pub const ctrl_enable: u32 = 1 << 0;
pub const ctrl_allns: u32 = 1 << 1;
pub const type_sregion_mask: u32 = 0xFF;
pub const rbar_base_mask: u32 = 0xFFFFFFE0;
pub const rlar_limit_mask: u32 = 0xFFFFFFE0;
pub const rlar_enable: u32 = 1 << 0;
pub const rlar_nsc: u32 = 1 << 1;
pub const attr_ns: u8 = 0;
pub const attr_nsc: u8 = 1;

/// True in a freestanding image, false in a host test binary.
pub const on_target = builtin.os.tag == .freestanding;

/// Mirrors `r_sau_regs_t`.
pub const Regs = extern struct {
    ctrl: u32,
    type: u32,
    rnr: u32,
    rbar: u32,
    rlar: u32,
};

/// Mirrors `ra8_sau_region_t`. `attr` stays a raw byte so an out-of-range
/// value from C is rejected rather than being an illegal enum.
pub const Region = extern struct {
    base: usize,
    size: u32,
    attr: u8,
};

/// Mirrors `ra8_sau_cfg_t`.
pub const Cfg = extern struct {
    regions: ?[*]const Region,
    region_count: u8,
    all_ns: bool,
};

pub const Error = error{ NullPtr, InvalidArg };

/// One region against the architectural granule and the 32-bit space.
pub fn regionValid(r: *const Region) bool {
    const mask = granule - 1;
    if (r.attr != attr_ns and r.attr != attr_nsc) return false;
    if (r.size < granule) return false;
    if ((r.size & mask) != 0) return false;
    const base: u32 = @truncate(r.base);
    if ((base & mask) != 0) return false;
    // A window that wraps would program a limit below its own base. The
    // headroom wraps to 0 for base 0 exactly as the C's does.
    if (r.size > (0xFFFFFFFF -% base) +% 1) return false;
    return true;
}

/// A whole descriptor, before any register is written.
pub fn cfgValid(cfg: ?*const Cfg, implemented: u8) Error!*const Cfg {
    const c = cfg orelse return error.NullPtr;
    if (c.regions == null and c.region_count != 0) return error.NullPtr;
    if (c.region_count > implemented) return error.InvalidArg;
    if (c.regions) |regions| {
        for (regions[0..c.region_count]) |*r| {
            if (!regionValid(r)) return error.InvalidArg;
        }
    }
    return c;
}

/// RLAR for an already-validated region.
pub fn rlarFor(r: *const Region) u32 {
    const base: u32 = @truncate(r.base);
    const limit = base +% r.size -% granule;
    var rlar = (limit & rlar_limit_mask) | rlar_enable;
    if (r.attr == attr_nsc) rlar |= rlar_nsc;
    return rlar;
}

pub fn dsb() void {
    if (on_target) asm volatile ("dsb 0xF" ::: .{ .memory = true });
}

pub fn isb() void {
    if (on_target) asm volatile ("isb 0xF" ::: .{ .memory = true });
}

pub const Block = struct {
    base: usize = base_addr,

    pub fn regs(b: Block) *volatile Regs {
        return @ptrFromInt(b.base);
    }

    pub fn sregionCount(b: Block) u8 {
        return @truncate(b.regs().type & type_sregion_mask);
    }

    pub fn writeRegion(b: Block, index: u8, r: *const Region) void {
        const base: u32 = @truncate(r.base);
        const p = b.regs();
        p.rnr = index;
        p.rbar = base & rbar_base_mask;
        p.rlar = rlarFor(r);
    }

    pub fn clearRegion(b: Block, index: u8) void {
        const p = b.regs();
        p.rnr = index;
        p.rlar = 0;
        p.rbar = 0;
    }

    /// Program a validated descriptor, clear the rest, then enable.
    pub fn install(b: Block, cfg: *const Cfg) void {
        const implemented = b.sregionCount();
        b.regs().ctrl = 0;
        var i: u8 = 0;
        while (i < cfg.region_count) : (i += 1) b.writeRegion(i, &cfg.regions.?[i]);
        while (i < implemented) : (i += 1) b.clearRegion(i);
        const ctrl = if (cfg.all_ns) ctrl_enable | ctrl_allns else ctrl_enable;
        dsb();
        b.regs().ctrl = ctrl;
        dsb();
        isb();
    }

    pub fn setEnabled(b: Block, on: bool) void {
        dsb();
        const p = b.regs();
        p.ctrl = if (on) p.ctrl | ctrl_enable else p.ctrl & ~ctrl_enable;
        dsb();
        isb();
    }

    pub fn isEnabled(b: Block) bool {
        return (b.regs().ctrl & ctrl_enable) != 0;
    }
};

/// The canonical boot partition (NS MRAM, NS SRAM, NS SDRAM, NSC alias).
pub const boot_region_count: u8 = 4;
pub const boot_regions = [boot_region_count]Region{
    .{ .base = 0x02080000, .size = 0x00080000, .attr = attr_ns },
    .{ .base = 0x22100000, .size = 0x00100000, .attr = attr_ns },
    .{ .base = 0x6A000000, .size = 0x02000000, .attr = attr_ns },
    .{ .base = 0x10000000, .size = 0x00100000, .attr = attr_nsc },
};
pub const boot_cfg = Cfg{
    .regions = &boot_regions,
    .region_count = boot_region_count,
    .all_ns = false,
};
