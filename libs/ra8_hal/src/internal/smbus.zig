//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! SMBus 3.2 protocol layer over an injected I2C bus (RA8FW-621). The bus
//! is a `Bus` value with write/read/transfer/logError methods so host tests
//! can stand in for the C ra8_i2c_bus_ops_t binder.

pub const ok: u16 = 0;
pub const invalid_arg: u16 = 0x103;
pub const invalid_size: u16 = 0x105;
pub const not_initialized: u16 = 0x10F;
pub const crc_mismatch: u16 = 0x405;
pub const null_ptr: u16 = 0x504;

pub const pec_poly: u8 = 0x07;
pub const pec_init: u8 = 0x00;
pub const alert_addr_7b: u8 = 0x0C;
pub const frame_bytes = 258;
pub const rx_bytes = 257;

const rw_write: u1 = 0;
const rw_read: u1 = 1;

/// SMBALERT# callback: (ctx, responding 7-bit address, status bit).
pub const AlertFn = *const fn (ctx: ?*anyopaque, target_7b: u8, status: u8) callconv(.C) void;

/// One CRC-8 (poly 0x07) step over byte `b`.
pub fn pecUpdate(crc: u8, b: u8) u8 {
    var c = crc ^ b;
    for (0..8) |_| {
        c = if ((c & 0x80) != 0) (c << 1) ^ pec_poly else c << 1;
    }
    return c;
}

/// PEC over a whole buffer; an empty buffer yields the initial value.
pub fn pec(data: []const u8) u8 {
    var c = pec_init;
    for (data) |b| c = pecUpdate(c, b);
    return c;
}

/// Wire address byte for a 7-bit target and R/W bit.
pub fn addrByte(target_7b: u8, rw: u1) u8 {
    return @truncate((@as(u32, target_7b) << 1) | rw);
}

fn pecOver(start: u8, bytes: []const u8) u8 {
    var c = start;
    for (bytes) |b| c = pecUpdate(c, b);
    return c;
}

pub fn Smbus(comptime Bus: type) type {
    return struct {
        const Self = @This();

        initialized: bool = false,
        bus: Bus = undefined,
        pec_enabled: bool = false,
        alert_fn: ?AlertFn = null,
        alert_ctx: ?*anyopaque = null,

        pub fn init(s: *Self, bus: Bus, pec_enabled: bool) u16 {
            s.* = .{ .initialized = true, .bus = bus, .pec_enabled = pec_enabled };
            return ok;
        }

        pub fn deinit(s: *Self) u16 {
            if (!s.initialized) return not_initialized;
            s.initialized = false;
            s.alert_fn = null;
            s.alert_ctx = null;
            return ok;
        }

        fn writeFrame(s: *Self, target_7b: u8, frame: []u8, n: usize) u16 {
            var len = n;
            if (s.pec_enabled) {
                frame[len] = pecOver(pecUpdate(pec_init, addrByte(target_7b, rw_write)), frame[0..len]);
                len += 1;
            }
            return s.bus.write(target_7b, frame[0..len], true);
        }

        pub fn sendByte(s: *Self, target_7b: u8, data: u8) u16 {
            if (!s.initialized) return not_initialized;
            var buf = [2]u8{ data, 0 };
            return s.writeFrame(target_7b, &buf, 1);
        }

        pub fn receiveByte(s: *Self, target_7b: u8, out: *u8) u16 {
            if (!s.initialized) return not_initialized;
            var buf = [2]u8{ 0, 0 };
            const len: usize = if (s.pec_enabled) 2 else 1;
            const err = s.bus.read(target_7b, buf[0..len]);
            if (err != ok) return err;
            if (s.pec_enabled) {
                const c = pecUpdate(pecUpdate(pec_init, addrByte(target_7b, rw_read)), buf[0]);
                if (c != buf[1]) {
                    s.bus.logError("receive_byte: PEC mismatch");
                    return crc_mismatch;
                }
            }
            out.* = buf[0];
            return ok;
        }

        pub fn writeByteData(s: *Self, target_7b: u8, cmd: u8, data: u8) u16 {
            if (!s.initialized) return not_initialized;
            var buf = [3]u8{ cmd, data, 0 };
            return s.writeFrame(target_7b, &buf, 2);
        }

        /// PEC over [addr W] [cmd] [addr R] then `tail`.
        fn readPec(target_7b: u8, cmd: u8, tail: []const u8) u8 {
            const head = [3]u8{ addrByte(target_7b, rw_write), cmd, addrByte(target_7b, rw_read) };
            return pecOver(pec(&head), tail);
        }

        pub fn readByteData(s: *Self, target_7b: u8, cmd: u8, out: *u8) u16 {
            if (!s.initialized) return not_initialized;
            var rx = [2]u8{ 0, 0 };
            const len: usize = if (s.pec_enabled) 2 else 1;
            const err = s.bus.transfer(target_7b, &[1]u8{cmd}, rx[0..len]);
            if (err != ok) return err;
            if (s.pec_enabled and readPec(target_7b, cmd, rx[0..1]) != rx[1]) {
                s.bus.logError("read_byte_data: PEC mismatch");
                return crc_mismatch;
            }
            out.* = rx[0];
            return ok;
        }

        pub fn blockWrite(s: *Self, target_7b: u8, cmd: u8, data: ?[*]const u8, len: u8) u16 {
            if (!s.initialized) return not_initialized;
            if (len == 0) return invalid_arg;
            const src = data orelse {
                s.bus.logError("block_write: data");
                return null_ptr;
            };
            var frame: [frame_bytes]u8 = undefined;
            frame[0] = cmd;
            frame[1] = len;
            @memcpy(frame[2 .. 2 + @as(usize, len)], src[0..len]);
            return s.writeFrame(target_7b, &frame, 2 + @as(usize, len));
        }

        pub fn blockRead(s: *Self, target_7b: u8, cmd: u8, buf: [*]u8, cap: u8, out_len: *u8) u16 {
            if (!s.initialized) return not_initialized;
            if (cap == 0) return invalid_arg;
            var rx: [rx_bytes]u8 = @splat(0);
            const want = @as(usize, cap) + 1 + @as(usize, @intFromBool(s.pec_enabled));
            const err = s.bus.transfer(target_7b, &[1]u8{cmd}, rx[0..want]);
            if (err != ok) return err;
            const count = rx[0];
            out_len.* = count;
            if (count > cap) return invalid_size;
            @memcpy(buf[0..count], rx[1 .. 1 + @as(usize, count)]);
            if (s.pec_enabled) {
                const c = readPec(target_7b, cmd, rx[0 .. 1 + @as(usize, count)]);
                if (c != rx[1 + @as(usize, count)]) {
                    s.bus.logError("block_read: PEC mismatch");
                    return crc_mismatch;
                }
            }
            return ok;
        }

        pub fn alertRegister(s: *Self, f: ?AlertFn, ctx: ?*anyopaque) u16 {
            if (!s.initialized) return not_initialized;
            s.alert_fn = f;
            s.alert_ctx = ctx;
            return ok;
        }

        pub fn alertDispatch(s: *Self) u16 {
            if (!s.initialized) return not_initialized;
            var ara = [1]u8{0};
            const err = s.bus.read(alert_addr_7b, &ara);
            if (err != ok) return err;
            if (s.alert_fn) |f| f(s.alert_ctx, ara[0] >> 1, ara[0] & 1);
            return ok;
        }
    };
}
