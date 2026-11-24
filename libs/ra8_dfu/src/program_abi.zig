//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The C ABI for the DFU slot programmer, delegating MRAM I/O to `mram_port`
//! and staging / `"RA8U"` verification / commit through `dfu_pipeline`
//! (`libs/ra8_update`).

const std = @import("std");
const dfu_pipeline = @import("dfu_pipeline");
const err = @import("err");
const image = @import("image");
const mram = @import("mram_port");
const program = @import("program");
const slot = @import("slot");
const upd_sess = @import("upd_session");

extern fn ra8_dfu_crc32(data: [*]const u8, len: u32) u32;

fn mramPrepare(_: ?*anyopaque, which: slot.Slot) u16 {
    const slot_base = program.base(which);
    if (slot_base == 0) return err.Err.invalid_arg.raw();
    return mram.prepareWindow(slot_base, slot_base + program.slots.size);
}

fn mramWrite(_: ?*anyopaque, which: slot.Slot, offset: u32, data: []const u8) u16 {
    const slot_base = program.base(which);
    if (slot_base == 0) return err.Err.invalid_arg.raw();
    return mram.writePages(slot_base + offset, data);
}

fn mramRead(_: ?*anyopaque, which: slot.Slot, offset: u32, dst: []u8) u16 {
    const slot_base = program.base(which);
    if (slot_base == 0) return err.Err.invalid_arg.raw();
    const ptr: [*]const u8 = @ptrFromInt(slot_base + offset);
    @memcpy(dst, ptr[0..dst.len]);
    return err.Err.ok.raw();
}

fn mramCrc(_: ?*anyopaque, which: slot.Slot, len: u32) u32 {
    const slot_base = program.base(which);
    if (slot_base == 0) return 0;
    return ra8_dfu_crc32(@ptrFromInt(slot_base), len);
}

fn mramWriteHeader(_: ?*anyopaque, which: slot.Slot, hdr: *const image.Header) u16 {
    const slot_base = program.base(which);
    if (slot_base == 0) return err.Err.invalid_arg.raw();
    return mram.writePages(slot_base + program.slots.hdr_offset, std.mem.asBytes(hdr));
}

fn mramBackend() dfu_pipeline.Backend {
    return .{
        .prepare = mramPrepare,
        .write = mramWrite,
        .read = mramRead,
        .crc32 = mramCrc,
        .write_header = mramWriteHeader,
        .ctx = null,
    };
}

fn readHeader(which: slot.Slot) ?image.Header {
    const slot_base = program.base(which);
    if (slot_base == 0) return null;
    const at: *const image.Header = @ptrFromInt(slot_base + program.slots.hdr_offset);
    return at.*;
}

export fn ra8_dfu_slot_base(which: slot.Slot) linksection(mram.sram_text) usize {
    return program.base(which);
}

export fn ra8_dfu_other_slot(which: slot.Slot) linksection(mram.sram_text) slot.Slot {
    return program.other(which);
}

export fn ra8_dfu_read_header(
    which: slot.Slot,
    out_hdr: ?*image.Header,
) linksection(mram.sram_text) u16 {
    const out = out_hdr orelse return err.Err.null_ptr.raw();
    out.* = readHeader(which) orelse return err.Err.invalid_arg.raw();
    return err.Err.ok.raw();
}

export fn ra8_dfu_slot_valid(which: slot.Slot) linksection(mram.sram_text) bool {
    const hdr = readHeader(which) orelse return false;
    var crc: u32 = 0;
    if (image.lengthValid(hdr.img_len)) {
        crc = ra8_dfu_crc32(@ptrFromInt(program.base(which)), hdr.img_len);
    }
    return image.headerValid(&hdr, crc);
}

export fn ra8_dfu_slot_seq(which: slot.Slot, out_seq: ?*u32) linksection(mram.sram_text) u16 {
    const out = out_seq orelse return err.Err.null_ptr.raw();
    const hdr = readHeader(which) orelse return err.Err.invalid_arg.raw();
    out.* = if (hdr.magic == image.layout.hdr_magic) hdr.seq else 0;
    return err.Err.ok.raw();
}

export fn ra8_dfu_set_bundle_hooks(
    crypto: ?*const upd_sess.CryptoIface,
    pubkey_handle: u32,
    rollback: ?*const upd_sess.RollbackIface,
) u16 {
    return dfu_pipeline.shared.setBundleHooks(crypto, pubkey_handle, rollback);
}

export fn ra8_dfu_program_prepare(inactive: slot.Slot) linksection(mram.sram_text) u16 {
    if (program.base(inactive) == 0) return err.Err.invalid_arg.raw();
    return dfu_pipeline.shared.prepare(inactive, mramBackend());
}

export fn ra8_dfu_program_image(
    inactive: slot.Slot,
    img_offset: u32,
    data: ?[*]const u8,
    len: u32,
) linksection(mram.sram_text) u16 {
    const src = data orelse return err.Err.null_ptr.raw();
    if (program.base(inactive) == 0) return err.Err.invalid_arg.raw();
    if (!program.bodyWriteValid(img_offset, len)) return err.Err.invalid_arg.raw();
    return dfu_pipeline.shared.writeImage(inactive, img_offset, src[0..len], mramBackend());
}

export fn ra8_dfu_program_commit(
    inactive: slot.Slot,
    img_len: u32,
    seq: u32,
) linksection(mram.sram_text) u16 {
    if (program.base(inactive) == 0) return err.Err.invalid_arg.raw();
    if (!image.lengthValid(img_len)) return err.Err.invalid_arg.raw();
    return dfu_pipeline.shared.commit(inactive, img_len, seq, mramBackend());
}

export fn ra8_dfu_program_verify(which: slot.Slot) linksection(mram.sram_text) u16 {
    if (program.base(which) == 0) return err.Err.invalid_arg.raw();
    return if (ra8_dfu_slot_valid(which)) err.Err.ok.raw() else err.Err.crc_mismatch.raw();
}

/// `priv_dfu_write_secure`: promoted out of TU-private linkage so the host
/// suite can exercise this argument guard directly. Declared in `src/ra8_dfu_internal.h`.
export fn priv_dfu_write_secure(
    addr: usize,
    src: ?[*]const u8,
    len: u32,
) linksection(mram.sram_text) u16 {
    const buf = src orelse return err.Err.invalid_arg.raw();
    if (len == 0) return err.Err.invalid_arg.raw();
    return mram.writePages(@intCast(addr), buf[0..len]);
}
