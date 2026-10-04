//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_smbus.h (RA8FW-621), replacing ra8_smbus.c. The C
//! ra8_i2c_bus_ops_t function pointers are wrapped as the `Bus` value.

const common = @import("abi_common.zig");
const sm = @import("internal/smbus.zig");

const tag = "SMBUS";

const WriteFn = *const fn (?*anyopaque, u8, [*]const u8, u32, bool) callconv(.C) u16;
const ReadFn = *const fn (?*anyopaque, u8, [*]u8, u32) callconv(.C) u16;
const TransferFn = *const fn (?*anyopaque, u8, [*]const u8, u32, [*]u8, u32) callconv(.C) u16;

/// ra8_i2c_bus_ops_t: write, read, transfer, ctx.
const BusOps = extern struct {
    write: ?WriteFn,
    read: ?ReadFn,
    transfer: ?TransferFn,
    ctx: ?*anyopaque,
};

/// ra8_smbus_cfg_t.
const Cfg = extern struct { bus: BusOps, pec_enabled: bool };

const CBus = struct {
    ops: BusOps,

    pub fn write(b: CBus, addr: u8, data: []const u8, stop: bool) u16 {
        return b.ops.write.?(b.ops.ctx, addr, data.ptr, @intCast(data.len), stop);
    }
    pub fn read(b: CBus, addr: u8, data: []u8) u16 {
        return b.ops.read.?(b.ops.ctx, addr, data.ptr, @intCast(data.len));
    }
    pub fn transfer(b: CBus, addr: u8, wr: []const u8, rd: []u8) u16 {
        return b.ops.transfer.?(b.ops.ctx, addr, wr.ptr, @intCast(wr.len), rd.ptr, @intCast(rd.len));
    }
    pub fn logError(_: CBus, msg: [*:0]const u8) void {
        common.ra8_log_emit_error(tag, msg);
    }
};

var state: sm.Smbus(CBus) = .{};

fn fail(msg: [*:0]const u8, code: u16) u16 {
    common.ra8_log_emit_error(tag, msg);
    return code;
}

export fn ra8_smbus_pec(data: ?[*]const u8, len: u32) u8 {
    const p = data orelse return sm.pec_init;
    return sm.pec(p[0..len]);
}

export fn ra8_smbus_init(cfg: ?*const Cfg) u16 {
    const c = cfg orelse return fail("smbus_init: cfg", common.k_ra8_err_null_ptr);
    if (c.bus.write == null) return fail("init: bus.write missing", common.k_ra8_err_invalid_arg);
    if (c.bus.read == null) return fail("init: bus.read missing", common.k_ra8_err_invalid_arg);
    if (c.bus.transfer == null) return fail("init: bus.transfer missing", common.k_ra8_err_invalid_arg);
    return state.init(.{ .ops = c.bus }, c.pec_enabled);
}

export fn ra8_smbus_deinit() u16 {
    return state.deinit();
}

export fn ra8_smbus_send_byte(target_7b: u8, data: u8) u16 {
    return state.sendByte(target_7b, data);
}

export fn ra8_smbus_receive_byte(target_7b: u8, out_data: ?*u8) u16 {
    const out = out_data orelse return fail("receive_byte: out_data", common.k_ra8_err_null_ptr);
    return state.receiveByte(target_7b, out);
}

export fn ra8_smbus_write_byte_data(target_7b: u8, cmd: u8, data: u8) u16 {
    return state.writeByteData(target_7b, cmd, data);
}

export fn ra8_smbus_read_byte_data(target_7b: u8, cmd: u8, out_data: ?*u8) u16 {
    const out = out_data orelse return fail("read_byte_data: out_data", common.k_ra8_err_null_ptr);
    return state.readByteData(target_7b, cmd, out);
}

export fn ra8_smbus_block_write(target_7b: u8, cmd: u8, data: ?[*]const u8, len: u8) u16 {
    return state.blockWrite(target_7b, cmd, data, len);
}

export fn ra8_smbus_block_read(target_7b: u8, cmd: u8, buf: ?[*]u8, cap: u8, out_len: ?*u8) u16 {
    const b = buf orelse return fail("block_read: buf", common.k_ra8_err_null_ptr);
    const n = out_len orelse return fail("block_read: out_len", common.k_ra8_err_null_ptr);
    return state.blockRead(target_7b, cmd, b, cap, n);
}

export fn ra8_smbus_alert_register_callback(f: ?sm.AlertFn, ctx: ?*anyopaque) u16 {
    return state.alertRegister(f, ctx);
}

export fn ra8_smbus_alert_dispatch() u16 {
    return state.alertDispatch();
}
