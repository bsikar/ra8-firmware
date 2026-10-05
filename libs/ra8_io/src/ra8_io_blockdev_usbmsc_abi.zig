//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI of inc/ra8_io_blockdev_usbmsc.h (RA8FW-717): a USB host mass-storage
//! logical unit as an ra8_io block device. read/write forward to the
//! ra8_usb_hmsc READ(10)/WRITE(10) commands for the bound LUN, get_caps reads
//! READ CAPACITY(10). Replaces ra8_io_blockdev_usbmsc.c, which is deleted.

const tag = "ra8_io_blockdev_usbmsc";

/// ra8_err_t values this unit returns (ra8_err.h).
pub const ok: c_int = 0;
pub const err_not_supported: c_int = 0x107;
pub const err_out_of_range: c_int = 0x208;
pub const err_null_ptr: c_int = 0x504;

/// ra8_io_blockdev.h: k_ra8_io_block_size_bytes, k_ra8_io_erase_value_zero.
pub const block_bytes: u32 = 512;
pub const erase_value_zero: u8 = 0x00;
/// One logical block per erase unit.
pub const erase_unit_blocks: u32 = 1;
/// k_ra8_io_usbmsc_max_transfer_blocks: the READ(10)/WRITE(10) ceiling.
pub const max_transfer_blocks: u32 = 65535;
/// ra8_usb_hmsc.h: k_ra8_hmsc_max_lun.
pub const max_lun: u8 = 4;

/// ra8_io_blockdev_caps_t (ra8_io_blockdev.h).
pub const Caps = extern struct {
    block_count: u32,
    erase_unit_blocks: u32,
    program_size_bytes: u32,
    logical_block_bytes: u16,
    erase_value: u8,
    must_erase_before_write: bool,
    read_only: bool,
};

/// struct ra8_io_blockdev_iface (ra8_io_blockdev_backend.h).
pub const Iface = extern struct {
    read: ?*const fn (ctx: ?*anyopaque, lba: u32, count: u32, buf: ?[*]u8) callconv(.c) c_int,
    write: ?*const fn (ctx: ?*anyopaque, lba: u32, count: u32, buf: ?[*]const u8) callconv(.c) c_int,
    erase: ?*const fn (ctx: ?*anyopaque, lba: u32, count: u32) callconv(.c) c_int,
    get_caps: ?*const fn (ctx: ?*const anyopaque, out: ?*Caps) callconv(.c) c_int,
    sync: ?*const fn (ctx: ?*anyopaque) callconv(.c) c_int,
};

/// ra8_io_blockdev_t (ra8_io_blockdev.h): the handle a backend binds.
pub const Bd = extern struct {
    iface: ?*const Iface,
    ctx: ?*anyopaque,
};

/// ra8_io_blockdev_usbmsc_state_t (ra8_io_blockdev_usbmsc.h).
pub const State = extern struct {
    lun: u8,
};

comptime {
    if (@sizeOf(Caps) != 20 or @offsetOf(Caps, "read_only") != 16) @compileError("Caps must match ra8_io_blockdev_caps_t");
    if (@sizeOf(Iface) != 5 * @sizeOf(usize)) @compileError("Iface must be five pointers");
    if (@sizeOf(Bd) != 2 * @sizeOf(usize)) @compileError("Bd must be two pointers");
    if (@sizeOf(State) != 1) @compileError("State must be one byte");
}

extern fn ra8_usb_hmsc_read10(lun: u8, lba: u32, count: u16, out: ?[*]u8) c_int;
extern fn ra8_usb_hmsc_write10(lun: u8, lba: u32, count: u16, in: ?[*]const u8) c_int;
extern fn ra8_usb_hmsc_read_capacity(lun: u8, block_count: *u32, block_size: *u32) c_int;
extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;

fn nullPtr(message: [*:0]const u8) c_int {
    ra8_log_emit_error(tag, message);
    return err_null_ptr;
}

fn stateOf(ctx: ?*const anyopaque) ?*const State {
    return @ptrCast(ctx);
}

fn read(ctx: ?*anyopaque, lba: u32, count: u32, buf: ?[*]u8) callconv(.c) c_int {
    const st = stateOf(ctx) orelse return nullPtr("ctx must not be nullptr");
    if (buf == null) return nullPtr("buf must not be nullptr");
    if (count > max_transfer_blocks) return err_out_of_range;
    return ra8_usb_hmsc_read10(st.lun, lba, @intCast(count), buf);
}

fn write(ctx: ?*anyopaque, lba: u32, count: u32, buf: ?[*]const u8) callconv(.c) c_int {
    const st = stateOf(ctx) orelse return nullPtr("ctx must not be nullptr");
    if (buf == null) return nullPtr("buf must not be nullptr");
    if (count > max_transfer_blocks) return err_out_of_range;
    return ra8_usb_hmsc_write10(st.lun, lba, @intCast(count), buf);
}

fn getCaps(ctx: ?*const anyopaque, out: ?*Caps) callconv(.c) c_int {
    const st = stateOf(ctx) orelse return nullPtr("ctx must not be nullptr");
    const o = out orelse return nullPtr("out must not be nullptr");
    var blocks: u32 = 0;
    var block_size: u32 = 0;
    const rc = ra8_usb_hmsc_read_capacity(st.lun, &blocks, &block_size);
    if (rc != ok) return rc;
    if (block_size != block_bytes) return err_not_supported;
    o.* = .{
        .block_count = blocks,
        .erase_unit_blocks = erase_unit_blocks,
        .program_size_bytes = block_bytes,
        .logical_block_bytes = @intCast(block_bytes),
        .erase_value = erase_value_zero,
        .must_erase_before_write = false,
        .read_only = false,
    };
    return ok;
}

pub const iface = Iface{ .read = &read, .write = &write, .erase = null, .get_caps = &getCaps, .sync = null };

export fn ra8_io_blockdev_usbmsc_init(bd: ?*Bd, state: ?*State, lun: u8) c_int {
    const b = bd orelse return nullPtr("bd must not be nullptr");
    const s = state orelse return nullPtr("state must not be nullptr");
    if (lun > max_lun) return err_out_of_range;
    s.lun = lun;
    b.iface = &iface;
    b.ctx = s;
    return ok;
}
