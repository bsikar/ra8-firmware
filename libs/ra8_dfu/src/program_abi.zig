//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The C ABI for the DFU slot programmer, and the only place that touches the
//! MRAM controller.
//!
//! Every entry point here is SRAM-resident. The MRAM program loop must not
//! execute from the array it is writing, so each one is placed in
//! `.sram_text`, the section `ra8_add_app(SRAM_TEXT ...)` loads into SRAM at
//! boot. That generated fragment claims `*(.sram_text)` ahead of the board
//! map's `.text` catch-all, which is the same route
//! `__attribute__((section(".sram_text")))` took from C, so nothing in the
//! linker scripts changes with this port.

const std = @import("std");
const builtin = @import("builtin");
const image = @import("image");
const program = @import("program");
const slot = @import("slot");

/// Section every exported function lands in, named once so the reason above
/// does not have to be repeated on ten declarations.
const sram_text = ".sram_text";

/// The `ra8_err_t` subset this membrane returns.
const Err = enum(u16) {
    ok = 0,
    invalid_arg = 0x103,
    crc_mismatch = 0x405,
    null_ptr = 0x504,

    fn raw(self: Err) u16 {
        return @backingInt(self);
    }
};

/// MRAM operating clocks the EK-RA8D2 apps advertise through MRCFREQ /
/// MREFREQ (HUM Ch 59.4.3 p 3550).
const clocks = struct {
    pub const mrcfreq_mhz: u16 = 250;
    pub const mrefreq_mhz: u8 = 125;
};

/// `ra8_flash_cfg_t`.
const FlashCfg = extern struct {
    mrcfreq_mhz: u16,
    mrefreq_mhz: u8,
    prefetch_en: bool,
    ecc_encoder_enable: bool,
    ecc_decoder_enable: bool,
};

/// `k_ra8_flash_world_s`: program through MRCPC1, the secure half. The DFU
/// core runs in the secure world and its slot MRAM carries secure
/// attribution.
const world_secure: u8 = 1;

extern fn ra8_flash_open(cfg: *const FlashCfg) u16;
extern fn ra8_flash_set_window(low: usize, high: usize) u16;
extern fn ra8_flash_write_block(mram_addr: u32, src: [*]const u8, len: u32, world: u8) u16;
extern fn ra8_dfu_crc32(data: [*]const u8, len: u32) u32;

/// Controller bring-up descriptor for `ra8_dfu_program_prepare`.
const flash_cfg = FlashCfg{
    .mrcfreq_mhz = clocks.mrcfreq_mhz,
    .mrefreq_mhz = clocks.mrefreq_mhz,
    .prefetch_en = true,
    .ecc_encoder_enable = true,
    .ecc_decoder_enable = true,
};

/// Carries the `ra8_err_t` the HAL returned back out past Zig's error
/// channel, which cannot hold a payload. Single-threaded by construction:
/// the write loop masks interrupts across every page.
var last_flash_err: u16 = 0;

/// The real MRAM backend `program.writePages` drives.
const Mram = struct {
    /// Erase one page to all-ones and program the body over it, interrupts
    /// masked across the pair so no ISR fetches code-MRAM while the array is
    /// busy. Code-MRAM cannot be reliably re-programmed over stale contents
    /// with ECC on, so the erased baseline goes down first.
    pub fn programPage(_: Mram, addr: u32, erased: []const u8, body: []const u8) error{Flash}!void {
        const saved = maskInterrupts();
        defer restoreInterrupts(saved);

        var err = ra8_flash_write_block(addr, erased.ptr, @intCast(erased.len), world_secure);
        if (err == Err.ok.raw()) {
            err = ra8_flash_write_block(addr, body.ptr, @intCast(body.len), world_secure);
        }
        if (err != Err.ok.raw()) {
            last_flash_err = err;
            return error.Flash;
        }
    }
};

/// PRIMASK save-and-mask, the `ra8_register_guard_t` pair as one value. The
/// mnemonics only assemble for the firmware target, so a host build of this
/// archive gets the no-op the C header's `RA8_OFF_TARGET` arm gave it.
fn maskInterrupts() u32 {
    if (builtin.target.cpu.arch != .thumb) return 0;
    const primask = asm volatile ("mrs %[out], primask"
        : [out] "=r" (-> u32),
    );
    asm volatile ("cpsid i" ::: .{ .memory = true });
    return primask;
}

fn restoreInterrupts(saved: u32) void {
    if (builtin.target.cpu.arch != .thumb) return;
    asm volatile ("msr primask, %[in]"
        :
        : [in] "r" (saved),
        : .{ .memory = true });
}

/// The header a slot carries, or null when the slot has no base. A plain
/// MRAM data read of the slot's last 32-byte page (HUM Ch 59.1 p 3543), not
/// a control register access.
fn readHeader(which: slot.Slot) ?image.Header {
    const slot_base = program.base(which);
    if (slot_base == 0) return null;
    const at: *const image.Header = @ptrFromInt(slot_base + program.slots.hdr_offset);
    return at.*;
}

fn writeSecure(addr: u32, src: []const u8) u16 {
    program.writePages(Mram{}, addr, src) catch return last_flash_err;
    return Err.ok.raw();
}

export fn ra8_dfu_slot_base(which: slot.Slot) linksection(sram_text) usize {
    return program.base(which);
}

export fn ra8_dfu_other_slot(which: slot.Slot) linksection(sram_text) slot.Slot {
    return program.other(which);
}

export fn ra8_dfu_read_header(which: slot.Slot, out_hdr: ?*image.Header) linksection(sram_text) u16 {
    const out = out_hdr orelse return Err.null_ptr.raw();
    out.* = readHeader(which) orelse return Err.invalid_arg.raw();
    return Err.ok.raw();
}

export fn ra8_dfu_slot_valid(which: slot.Slot) linksection(sram_text) bool {
    const hdr = readHeader(which) orelse return false;
    // Fold the CRC only over an in-range, page-aligned length, so a corrupt
    // header cannot drive an MRAM over-read. A length that fails this fails
    // `headerValid` too, so the zero it is then checked against is a value
    // the predicate already rejects.
    var crc: u32 = 0;
    if (image.lengthValid(hdr.img_len)) {
        crc = ra8_dfu_crc32(@ptrFromInt(program.base(which)), hdr.img_len);
    }
    return image.headerValid(&hdr, crc);
}

export fn ra8_dfu_slot_seq(which: slot.Slot, out_seq: ?*u32) linksection(sram_text) u16 {
    const out = out_seq orelse return Err.null_ptr.raw();
    const hdr = readHeader(which) orelse return Err.invalid_arg.raw();
    out.* = if (hdr.magic == image.layout.hdr_magic) hdr.seq else 0;
    return Err.ok.raw();
}

export fn ra8_dfu_program_prepare(inactive: slot.Slot) linksection(sram_text) u16 {
    const slot_base = program.base(inactive);
    if (slot_base == 0) return Err.invalid_arg.raw();
    const err = ra8_flash_open(&flash_cfg);
    if (err != Err.ok.raw()) return err;
    // Fence every later write to this one slot: the bootloader and the active
    // slot sit outside the window and cannot be touched. No erase here. MRAM
    // is byte-alterable, so the body is written directly and the header
    // overwritten at commit; a torn download leaves the OLD header over a NEW
    // partial image, whose CRC will not match, so torn-write safety holds
    // without one. An erase here would also read the driver's 0xFF constant
    // out of code-MRAM while the array is busy, faulting the bus.
    return ra8_flash_set_window(slot_base, slot_base + program.slots.size);
}

export fn ra8_dfu_program_image(
    inactive: slot.Slot,
    img_offset: u32,
    data: ?[*]const u8,
    len: u32,
) linksection(sram_text) u16 {
    const src = data orelse return Err.null_ptr.raw();
    const slot_base = program.base(inactive);
    if (slot_base == 0) return Err.invalid_arg.raw();
    if (!program.bodyWriteValid(img_offset, len)) return Err.invalid_arg.raw();
    return writeSecure(slot_base + img_offset, src[0..len]);
}

export fn ra8_dfu_program_commit(
    inactive: slot.Slot,
    img_len: u32,
    seq: u32,
) linksection(sram_text) u16 {
    const slot_base = program.base(inactive);
    if (slot_base == 0) return Err.invalid_arg.raw();
    if (!image.lengthValid(img_len)) return Err.invalid_arg.raw();

    const hdr = image.Header{
        .magic = image.layout.hdr_magic,
        .seq = seq,
        .img_len = img_len,
        .img_crc32 = ra8_dfu_crc32(@ptrFromInt(slot_base), img_len),
        .entry = image.layout.run_base,
        .rsv0 = 0,
        .rsv1 = 0,
        .rsv2 = 0,
    };
    // Header last, into the slot's last page, so a torn write leaves it
    // erased (invalid) rather than valid over a partial image.
    return writeSecure(slot_base + program.slots.hdr_offset, std.mem.asBytes(&hdr));
}

export fn ra8_dfu_program_verify(which: slot.Slot) linksection(sram_text) u16 {
    if (program.base(which) == 0) return Err.invalid_arg.raw();
    return if (ra8_dfu_slot_valid(which)) Err.ok.raw() else Err.crc_mismatch.raw();
}

/// `priv_dfu_write_secure`: promoted out of TU-private linkage so the host
/// suite can exercise this argument guard directly, which no public-API path
/// can present. Declared in `src/ra8_dfu_internal.h`.
export fn priv_dfu_write_secure(
    addr: usize,
    src: ?[*]const u8,
    len: u32,
) linksection(sram_text) u16 {
    const buf = src orelse return Err.invalid_arg.raw();
    if (len == 0) return Err.invalid_arg.raw();
    return writeSecure(@intCast(addr), buf[0..len]);
}
