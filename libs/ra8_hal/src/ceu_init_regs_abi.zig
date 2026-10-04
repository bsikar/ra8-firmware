//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the CEU register programming helpers ra8_ceu.c calls
//! (RA8FW-593). Logic is in internal/ceu_init_regs.zig.

const ceu = @import("internal/ceu_init_regs.zig");

const ceu_base: usize = 0x4034_8000;

comptime {
    if (@sizeOf(ceu.Config) != 60) @compileError("ra8_ceu_config_t is 60 bytes");
    if (@offsetOf(ceu.Config, "scale") != 38) @compileError("scale sits at +38");
    if (@offsetOf(ceu.Config, "image_area_size") != 56) @compileError("image_area_size sits at +56");
}

const Mmio = struct {
    pub fn write32(_: Mmio, off: usize, v: u32) void {
        const p: *volatile u32 = @ptrFromInt(ceu_base + off);
        p.* = v;
    }
};

export fn priv_ra8_ceu_min_stride_bytes(cfg: *const ceu.Config) u32 {
    return ceu.minStrideBytes(cfg);
}

export fn priv_ra8_ceu_program_geometry(cfg: *const ceu.Config) void {
    ceu.programGeometry(Mmio{}, cfg);
}

export fn priv_ra8_ceu_program_format(cfg: *const ceu.Config) void {
    ceu.programFormat(Mmio{}, cfg);
}

export fn priv_ra8_ceu_program_destination(cfg: *const ceu.Config) void {
    ceu.programDestination(Mmio{}, cfg);
}
