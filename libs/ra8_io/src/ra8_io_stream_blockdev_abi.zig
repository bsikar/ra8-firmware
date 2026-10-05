//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI of inc/ra8_io_stream_blockdev.h (RA8FW-720): a stream sink that
//! gathers bytes into one 512-byte sector and writes each full sector to a
//! block device, zero-padding the last partial sector on flush. Replaces
//! ra8_io_stream_blockdev.c, which is deleted. ra8_io_stream_bind still
//! lives in C and ra8_io_blockdev_write in ra8_io_blockdev_abi.zig; both are
//! reached as externs.

const Stream = @import("ra8_io_log_abi.zig").Stream;
const ram = @import("ra8_io_stream_ram_abi.zig");

const tag = "ra8_io_stream_blockdev";

/// ra8_err_t values this unit returns (ra8_err.h).
pub const ok: c_int = 0;
pub const err_null_ptr: c_int = 0x504;

/// k_ra8_io_block_size_bytes (ra8_io_blockdev.h).
pub const sector_bytes: u32 = 512;
/// Pad value for a partial trailing sector.
pub const pad_byte: u8 = 0;

/// Mirror of ra8_io_stream_blockdev_state_t.
pub const State = extern struct {
    bd: ?*const anyopaque,
    lba: u32,
    fill: u32,
    sector: [sector_bytes]u8,
};

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;
extern fn ra8_io_stream_bind(stream: *Stream, iface: *const ram.Iface, context: ?*anyopaque) c_int;
extern fn ra8_io_blockdev_write(bd: *const anyopaque, lba: u32, count: u32, buf: [*]const u8) c_int;

fn nullPtr(message: [*:0]const u8) c_int {
    ra8_log_emit_error(tag, message);
    return err_null_ptr;
}

fn commitSector(st: *State) c_int {
    const bd = st.bd orelse return nullPtr("st->bd must not be nullptr");
    const e = ra8_io_blockdev_write(bd, st.lba, 1, &st.sector);
    if (e != ok) return e;
    st.lba +%= 1;
    st.fill = 0;
    return ok;
}

fn bdWrite(ctx: ?*anyopaque, buf: ?[*]const u8, len: u32, out_written: ?*u32) callconv(.c) c_int {
    const st: *State = @ptrCast(@alignCast(ctx orelse return nullPtr("ctx must not be nullptr")));
    const src = buf orelse return nullPtr("buf must not be nullptr");
    var done: u32 = 0;
    while (done < len) {
        const chunk = @min(len - done, sector_bytes - st.fill);
        @memcpy(st.sector[st.fill..][0..chunk], src[done..][0..chunk]);
        st.fill += chunk;
        done += chunk;
        if (st.fill == sector_bytes) {
            const e = commitSector(st);
            if (e != ok) {
                if (out_written) |w| w.* = done;
                return e;
            }
        }
    }
    if (out_written) |w| w.* = len;
    return ok;
}

fn bdFlush(ctx: ?*anyopaque) callconv(.c) c_int {
    const st: *State = @ptrCast(@alignCast(ctx orelse return nullPtr("ctx must not be nullptr")));
    if (st.fill == 0) return ok;
    @memset(st.sector[st.fill..], pad_byte);
    return commitSector(st);
}

pub const iface = ram.Iface{ .write = &bdWrite, .flush = &bdFlush };

pub export fn ra8_io_stream_blockdev_init(
    s: ?*Stream,
    state: ?*State,
    bd: ?*const anyopaque,
    start_lba: u32,
) callconv(.c) c_int {
    const stream = s orelse return nullPtr("s must not be nullptr");
    const st = state orelse return nullPtr("state must not be nullptr");
    const dev = bd orelse return nullPtr("bd must not be nullptr");
    st.bd = dev;
    st.lba = start_lba;
    st.fill = 0;
    return ra8_io_stream_bind(stream, &iface, st);
}
