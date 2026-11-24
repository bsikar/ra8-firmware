//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Shared SRAM-resident code-MRAM programmer used by both `program_abi`
//! (slot body/header writes) and `boot_state_abi` (ping-pong record writes).
//!
//! Every function touching code-MRAM program execution lands in `.sram_text`
//! so instruction fetches never touch MRAM while the array is busy.

const builtin = @import("builtin");
const err = @import("err");
const program = @import("program");

/// Linker section claiming SRAM execution for MRAM programmers.
pub const sram_text = ".sram_text";

const clocks = struct {
    pub const mrcfreq_mhz: u16 = 250;
    pub const mrefreq_mhz: u8 = 125;
};

const FlashCfg = extern struct {
    mrcfreq_mhz: u16,
    mrefreq_mhz: u8,
    prefetch_en: bool,
    ecc_encoder_enable: bool,
    ecc_decoder_enable: bool,
};

/// `k_ra8_flash_world_s`: MRCPC1 secure gate.
const world_secure: u8 = 1;

extern fn ra8_flash_open(cfg: *const FlashCfg) u16;
extern fn ra8_flash_set_window(low: usize, high: usize) u16;
extern fn ra8_flash_write_block(mram_addr: u32, src: [*]const u8, len: u32, world: u8) u16;

const flash_cfg = FlashCfg{
    .mrcfreq_mhz = clocks.mrcfreq_mhz,
    .mrefreq_mhz = clocks.mrefreq_mhz,
    .prefetch_en = true,
    .ecc_encoder_enable = true,
    .ecc_decoder_enable = true,
};

/// Holds the raw HAL error code behind Zig's error channel.
var last_flash_err: u16 = err.Err.ok.raw();

/// SRAM-resident MRAM page backend for `program.writePages`.
pub const Mram = struct {
    pub fn programPage(_: Mram, addr: u32, erased: []const u8, body: []const u8) linksection(sram_text) error{Flash}!void {
        const saved = maskInterrupts();
        defer restoreInterrupts(saved);

        var code = ra8_flash_write_block(addr, erased.ptr, @intCast(erased.len), world_secure);
        if (code == err.Err.ok.raw()) {
            code = ra8_flash_write_block(addr, body.ptr, @intCast(body.len), world_secure);
        }
        if (code != err.Err.ok.raw()) {
            last_flash_err = code;
            return error.Flash;
        }
    }
};

/// Return the raw HAL error code saved from the last failed `Mram.programPage`.
pub fn lastErr() u16 {
    return last_flash_err;
}

fn maskInterrupts() linksection(sram_text) u32 {
    if (builtin.target.cpu.arch != .thumb) return 0;
    const primask = asm volatile ("mrs %[out], primask"
        : [out] "=r" (-> u32),
    );
    asm volatile ("cpsid i" ::: .{ .memory = true });
    return primask;
}

fn restoreInterrupts(saved: u32) linksection(sram_text) void {
    if (builtin.target.cpu.arch != .thumb) return;
    asm volatile ("msr primask, %[in]"
        :
        : [in] "r" (saved),
        : .{ .memory = true });
}

/// Open the flash driver and fence writes to `[low, high)`.
pub fn prepareWindow(low: usize, high: usize) linksection(sram_text) u16 {
    const code = ra8_flash_open(&flash_cfg);
    if (code != err.Err.ok.raw()) return code;
    return ra8_flash_set_window(low, high);
}

/// Erase-then-program all 32-byte pages in `src` starting at `addr`.
pub fn writePages(addr: u32, src: []const u8) linksection(sram_text) u16 {
    program.writePages(Mram{}, addr, src) catch return last_flash_err;
    return err.Err.ok.raw();
}
