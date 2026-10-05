//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI of inc/ra8_io_blockdev.h (RA8FW-723): the block-device front end
//! that validates a bound device and dispatches to its backend vtable, plus
//! ra8_io_blockdev_as_fs_backend, which adapts a device to ra8_fs_backend_t.
//! Replaces ra8_io_blockdev.c, which is deleted.

const sdhi = @import("ra8_io_blockdev_sdhi_abi.zig");

const tag = "ra8_io_blockdev";

/// ra8_err_t values this unit returns (ra8_err.h).
pub const ok = sdhi.ok;
pub const err_not_supported: c_int = 0x107;
pub const err_not_initialized: c_int = 0x10F;
pub const err_out_of_range: c_int = 0x208;
pub const err_null_ptr = sdhi.err_null_ptr;

pub const Caps = sdhi.Caps;
pub const Iface = sdhi.Iface;
pub const Device = sdhi.Device;

/// Mirror of ra8_fs_backend_t (ra8_fs_types.h).
pub const FsBackend = extern struct {
    read_block: ?*const fn (?*anyopaque, u64, u32, ?[*]u8) callconv(.c) c_int,
    write_block: ?*const fn (?*anyopaque, u64, u32, ?[*]const u8) callconv(.c) c_int,
    get_capacity: ?*const fn (?*anyopaque, ?*u64, ?*u32) callconv(.c) c_int,
    erase_blocks: ?*const fn (?*anyopaque, u64, u64) callconv(.c) c_int,
    ctx: ?*anyopaque,
};

comptime {
    if (@sizeOf(FsBackend) != 5 * @sizeOf(usize)) @compileError("fs backend is five pointers");
}

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;

fn nullPtr(message: [*:0]const u8) c_int {
    ra8_log_emit_error(tag, message);
    return err_null_ptr;
}

/// A null device or one with no bound vtable; neither is logged.
fn validate(bd: ?*const Device) ?c_int {
    const device = bd orelse return err_null_ptr;
    if (device.iface == null) return err_not_initialized;
    return null;
}

export fn ra8_io_blockdev_read(bd: ?*const Device, lba: u32, count: u32, buf: ?[*]u8) c_int {
    if (validate(bd)) |rc| return rc;
    const dst = buf orelse return nullPtr("buf must not be nullptr");
    const op = bd.?.iface.?.read orelse return nullPtr("backend read op missing");
    return op(bd.?.ctx, lba, count, dst);
}

export fn ra8_io_blockdev_write(bd: ?*const Device, lba: u32, count: u32, buf: ?[*]const u8) c_int {
    if (validate(bd)) |rc| return rc;
    const src = buf orelse return nullPtr("buf must not be nullptr");
    const op = bd.?.iface.?.write orelse return nullPtr("backend write op missing");
    return op(bd.?.ctx, lba, count, src);
}

export fn ra8_io_blockdev_erase(bd: ?*const Device, lba: u32, count: u32) c_int {
    if (validate(bd)) |rc| return rc;
    const op = bd.?.iface.?.erase orelse return err_not_supported;
    return op(bd.?.ctx, lba, count);
}

export fn ra8_io_blockdev_get_caps(bd: ?*const Device, out: ?*Caps) c_int {
    if (validate(bd)) |rc| return rc;
    const caps = out orelse return nullPtr("out must not be nullptr");
    const op = bd.?.iface.?.get_caps orelse return nullPtr("backend get_caps op missing");
    return op(bd.?.ctx, caps);
}

export fn ra8_io_blockdev_sync(bd: ?*const Device) c_int {
    if (validate(bd)) |rc| return rc;
    const op = bd.?.iface.?.sync orelse return ok;
    return op(bd.?.ctx);
}

const ctx_null = "ctx (blockdev) must not be nullptr";
const u32_max: u64 = 0xFFFF_FFFF;

fn fsRead(ctx: ?*anyopaque, lba: u64, count: u32, buf: ?[*]u8) callconv(.c) c_int {
    const raw = ctx orelse return nullPtr(ctx_null);
    const dst = buf orelse return nullPtr("buf must not be nullptr");
    // The io fabric addresses 32-bit LBAs; every backend it fronts does too.
    if (lba > u32_max) return err_out_of_range;
    return ra8_io_blockdev_read(@ptrCast(@alignCast(raw)), @intCast(lba), count, dst);
}

fn fsWrite(ctx: ?*anyopaque, lba: u64, count: u32, buf: ?[*]const u8) callconv(.c) c_int {
    const raw = ctx orelse return nullPtr(ctx_null);
    const src = buf orelse return nullPtr("buf must not be nullptr");
    if (lba > u32_max) return err_out_of_range;
    return ra8_io_blockdev_write(@ptrCast(@alignCast(raw)), @intCast(lba), count, src);
}

fn fsGetCapacity(ctx: ?*anyopaque, block_count: ?*u64, block_size: ?*u32) callconv(.c) c_int {
    const raw = ctx orelse return nullPtr(ctx_null);
    const n = block_count orelse return nullPtr("block_count must not be nullptr");
    const size = block_size orelse return nullPtr("block_size must not be nullptr");
    var caps = zeroCaps();
    const rc = ra8_io_blockdev_get_caps(@ptrCast(@alignCast(raw)), &caps);
    if (rc != ok) return rc;
    n.* = caps.block_count;
    size.* = caps.logical_block_bytes;
    return ok;
}

fn fsErase(ctx: ?*anyopaque, lba: u64, count: u64) callconv(.c) c_int {
    const raw = ctx orelse return nullPtr(ctx_null);
    const bd: *const Device = @ptrCast(@alignCast(raw));
    var caps = zeroCaps();
    const rc = ra8_io_blockdev_get_caps(bd, &caps);
    if (rc != ok) return rc;
    if (caps.erase_value != sdhi.erase_value_zero) return err_not_supported;
    if (lba > u32_max or count > u32_max) return err_out_of_range;
    return ra8_io_blockdev_erase(bd, @intCast(lba), @intCast(count));
}

fn zeroCaps() Caps {
    return .{
        .block_count = 0,
        .erase_unit_blocks = 0,
        .program_size_bytes = 0,
        .logical_block_bytes = 0,
        .erase_value = 0,
        .must_erase_before_write = false,
        .read_only = false,
    };
}

export fn ra8_io_blockdev_as_fs_backend(bd: ?*const Device, out: ?*FsBackend) c_int {
    if (validate(bd)) |rc| return rc;
    const backend = out orelse return nullPtr("out must not be nullptr");
    backend.* = .{
        .read_block = fsRead,
        .write_block = fsWrite,
        .get_capacity = fsGetCapacity,
        .erase_blocks = fsErase,
        .ctx = @constCast(bd.?),
    };
    return ok;
}
