//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Where the A/B application slots sit in MRAM, and the erase-then-program
//! page loop that fills one of them.
//!
//! The loop is generic over its flash backend for the same reason the DFU
//! session steps are generic over their HAL: the page sequencing is the part
//! worth testing, and it should not need an MRAM controller to run.
//! `program_abi` binds the real one.

const std = @import("std");
const image = @import("image");
const slot = @import("slot");

/// MRAM slot geometry. Mirrors `ra8_dfu_layout_t` in `ra8_dfu.h`.
pub const slots = struct {
    /// Slot A base; the application vector table sits here.
    pub const a_base: u32 = 0x0202_0000;
    /// Slot B base.
    pub const b_base: u32 = 0x0209_0000;
    /// Bytes per slot (448 KiB), body plus the trailing header page.
    pub const size: u32 = 0x0007_0000;
    /// Header offset inside a slot: its last program page.
    pub const hdr_offset: u32 = 0x0006_FFE0;
};

/// The byte every MRAM page holds once erased.
pub const erased_byte: u8 = 0xFF;

comptime {
    std.debug.assert(slots.a_base + slots.size == slots.b_base);
    std.debug.assert(slots.hdr_offset == image.layout.img_max);
    std.debug.assert(slots.hdr_offset + image.layout.page_size == slots.size);
}

/// MRAM base of `which`, or 0 for `.none`. Zero is the C sentinel for "no
/// such slot" and every entry point rejects it.
pub fn base(which: slot.Slot) u32 {
    return switch (which) {
        .a => slots.a_base,
        .b => slots.b_base,
        .none => 0,
    };
}

/// The slot that is not `which`. Anything that is not A answers A, which is
/// what the C did and what the bootloader wants: `.none` names no live image,
/// so the other slot is the one to program.
pub fn other(which: slot.Slot) slot.Slot {
    return if (which == .a) .b else .a;
}

/// Whether a body write of `len` bytes at `offset` lands wholly inside the
/// image area, on page boundaries at both ends.
pub fn bodyWriteValid(offset: u32, len: u32) bool {
    if (len == 0 or len % image.layout.page_size != 0) return false;
    if (offset % image.layout.page_size != 0) return false;
    return @as(u64, offset) + @as(u64, len) <= image.layout.img_max;
}

/// Erase each page to the all-ones baseline and program the body over it, one
/// page at a time, lowest address first.
///
/// `flash` supplies `programPage(addr, erased, body)`, which masks interrupts
/// across the pair so no ISR fetches code-MRAM while the array is busy.
///
/// The all-ones operand is this frame's buffer rather than a constant: a
/// constant would be read out of code-MRAM while the array is busy, which
/// faults the bus (the SRAM-resident warning in `ra8_flash.h`). Keeping it on
/// the stack keeps every operand in SRAM.
pub fn writePages(flash: anytype, addr: u32, src: []const u8) !void {
    var erased = [_]u8{erased_byte} ** image.layout.page_size;
    var off: u32 = 0;
    const len: u32 = @intCast(src.len);
    while (off < len) {
        const chunk = @min(len - off, image.layout.page_size);
        try flash.programPage(addr + off, erased[0..chunk], src[off..][0..chunk]);
        off += chunk;
    }
}
