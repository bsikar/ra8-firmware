//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! USB host bulk pipe helpers (RA8FW-753), ported from ra8_usb_host_bulk.c.
//! Registers are reached by r_usb_regs_t offset from the base `ops.pick`
//! returns; the shared FIFO/PID/pipe helpers stay in ra8_usb.c and
//! ra8_usb_host_ctrl.c and come in through `ops`. HUM chapter 31.

pub const ok: u16 = 0;
pub const invalid_arg: u16 = 0x103;
pub const invalid_state: u16 = 0x104;
pub const hw_timeout: u16 = 0x203;
pub const hw_error: u16 = 0x204;
pub const null_ptr: u16 = 0x504;

/// r_usb_regs_t field offsets (ra8_usb_regs.h); every field is u16.
pub const off = struct {
    pub const syssts0: usize = 0x04;
    pub const cfifoctr: usize = 0x22;
    pub const brdyenb: usize = 0x36;
    pub const bempenb: usize = 0x3A;
    pub const brdysts: usize = 0x46;
    pub const nrdysts: usize = 0x48;
    pub const bempsts: usize = 0x4A;
    pub const dcpmaxp: usize = 0x5E;
    pub const pipesel: usize = 0x64;
    pub const pipecfg: usize = 0x68;
    pub const pipebuf: usize = 0x6A;
    pub const pipemaxp: usize = 0x6C;
    pub const pipeperi: usize = 0x6E;
    pub const pipectr: usize = 0x70;
};

pub const max_pipe_num: u8 = 9;
pub const dev_addr_max: u8 = 10;
pub const devsel_shift: u4 = 12;
pub const dcpmaxp_mxps: u16 = 0x007F;
pub const pipemaxp_mxps: u16 = 0x07FF;
pub const pipecfg_epnum_mask: u8 = 0x0F;
pub const pid_stall_bit: u16 = 0x0002;
pub const lnst_mask: u16 = 0x0003;
pub const fifoctr_dtln: u16 = 0x0FFF;
pub const fifoctr_bclr: u16 = 0x4000;
pub const dcpctr_sqclr: u16 = 1 << 8;
pub const pid_nak: u16 = 0;
pub const pid_buf: u16 = 1;
pub const ep_dir_out: u8 = 0;
pub const ep_dir_in: u8 = 1;
pub const ep_type_bulk: u8 = 0;
pub const poll_limit: u32 = 10_000_000;

pub fn r16(base: usize, offset: usize) *volatile u16 {
    return @ptrFromInt(base + offset);
}

fn pipectr(base: usize, pipe: u8) *volatile u16 {
    return r16(base, off.pipectr + @as(usize, pipe - 1) * 2);
}

fn pipeBit(pipe: u8) u16 {
    return @as(u16, 1) << @as(u4, @truncate(pipe));
}

fn waitPipe(base: usize, sts_off: usize, pipe: u8) u16 {
    const bit = pipeBit(pipe);
    var i: u32 = 0;
    while (i < poll_limit) : (i += 1) {
        if (r16(base, sts_off).* & bit != 0) return ok;
        if (pipectr(base, pipe).* & pid_stall_bit != 0) return hw_error;
    }
    return hw_timeout;
}

fn pipeInRange(pipe: u8) bool {
    return pipe != 0 and pipe <= max_pipe_num;
}

pub fn setTarget(ops: anytype, speed: u8, dev_addr: u8) u16 {
    const base = ops.pick(speed) orelse return invalid_arg;
    if (dev_addr > dev_addr_max) return invalid_arg;
    ops.programDevadd(base, dev_addr);
    const dcpmaxp = r16(base, off.dcpmaxp);
    const mxps = dcpmaxp.* & dcpmaxp_mxps;
    dcpmaxp.* = (@as(u16, dev_addr) << devsel_shift) | mxps;
    return ok;
}

pub fn pipeArgsOk(pipe: u8, dev_addr: u8, ep: u8, max_packet: u16) u16 {
    if (!pipeInRange(pipe)) return invalid_arg;
    if (dev_addr > dev_addr_max) return invalid_arg;
    if (ep == 0 or ep > pipecfg_epnum_mask) return invalid_arg;
    if (max_packet == 0 or max_packet > pipemaxp_mxps) return invalid_arg;
    return ok;
}

pub fn pipeSetup(ops: anytype, speed: u8, pipe: u8, dev_addr: u8, ep: u8, device_to_host: bool, max_packet: u16) u16 {
    const base = ops.pick(speed) orelse return invalid_arg;
    const arg_err = pipeArgsOk(pipe, dev_addr, ep, max_packet);
    if (arg_err != ok) return arg_err;
    ops.pipeQuiesce(base, pipe);
    const dir = if (device_to_host) ep_dir_out else ep_dir_in;
    r16(base, off.pipesel).* = pipe;
    r16(base, off.pipecfg).* = ops.pipecfgWord(ep, dir, ep_type_bulk, true);
    r16(base, off.pipebuf).* = ops.pipebufWord(pipe, max_packet);
    r16(base, off.pipemaxp).* = (@as(u16, dev_addr) << devsel_shift) | (max_packet & pipemaxp_mxps);
    r16(base, off.pipeperi).* = 0;
    r16(base, off.pipesel).* = 0;
    ops.rmw16(pipectr(base, pipe), dcpctr_sqclr, 0);
    const bit = pipeBit(pipe);
    r16(base, off.brdysts).* = ~bit;
    r16(base, off.nrdysts).* = ~bit;
    r16(base, off.bempsts).* = ~bit;
    return ok;
}

pub fn bulkOut(ops: anytype, speed: u8, pipe: u8, data: ?[*]const u8, len: u16) u16 {
    const d = data orelse return ops.nullPtr("host_bulk_out: data");
    const base = ops.pick(speed) orelse return invalid_arg;
    if (!pipeInRange(pipe)) return invalid_arg;
    const bit = pipeBit(pipe);
    r16(base, off.bempsts).* = ~bit;
    const bempenb = r16(base, off.bempenb);
    bempenb.* = bempenb.* | bit;
    const qerr = ops.queueIn(speed, pipe, d, len);
    if (qerr != ok) return qerr;
    const werr = waitPipe(base, off.bempsts, pipe);
    ops.pipePid(base, pipe, pid_nak);
    r16(base, off.bempsts).* = ~bit;
    return werr;
}

const Packet = struct { err: u16, dtln: u16 = 0, copied: u16 = 0 };

fn rxPacket(ops: anytype, base: usize, pipe: u8, dst: [*]u8, room: u16) Packet {
    const werr = waitPipe(base, off.brdysts, pipe);
    if (werr != ok) return .{ .err = werr };
    r16(base, off.brdysts).* = ~pipeBit(pipe);
    ops.selectCfifo(base, pipe, false);
    const ferr = ops.waitFrdy(base);
    if (ferr != ok) return .{ .err = ferr };
    const cfifoctr = r16(base, off.cfifoctr);
    const dtln = cfifoctr.* & fifoctr_dtln;
    const copy = @min(dtln, room);
    if (copy > 0) ops.fifoRead(base, dst, copy);
    if (copy < dtln) cfifoctr.* = fifoctr_bclr;
    if (dtln == 0) cfifoctr.* = fifoctr_bclr;
    return .{ .err = ok, .dtln = dtln, .copied = copy };
}

fn rxLoop(ops: anytype, base: usize, pipe: u8, buf: [*]u8, max_len: u16, mps: u16, out_rx: *u16) u16 {
    var rx: u16 = 0;
    while (true) {
        const p = rxPacket(ops, base, pipe, buf + rx, max_len -% rx);
        if (p.err != ok) {
            out_rx.* = rx;
            return p.err;
        }
        rx +%= p.copied;
        if (p.dtln < mps or rx >= max_len) break;
    }
    out_rx.* = rx;
    return ok;
}

pub fn bulkIn(ops: anytype, speed: u8, pipe: u8, buf: ?[*]u8, max_len: u16, out_received: ?*u16) u16 {
    const b = buf orelse return ops.nullPtr("host_bulk_in: buf");
    const out = out_received orelse return ops.nullPtr("host_bulk_in: out_received");
    const base = ops.pick(speed) orelse return invalid_arg;
    if (!pipeInRange(pipe)) return invalid_arg;
    r16(base, off.pipesel).* = pipe;
    const mps = r16(base, off.pipemaxp).* & pipemaxp_mxps;
    r16(base, off.pipesel).* = 0;
    if (mps == 0) return invalid_state;
    const brdyenb = r16(base, off.brdyenb);
    brdyenb.* = brdyenb.* | pipeBit(pipe);
    ops.pipePid(base, pipe, pid_buf);
    var rx: u16 = 0;
    const err = rxLoop(ops, base, pipe, b, max_len, mps, &rx);
    ops.pipePid(base, pipe, pid_nak);
    out.* = rx;
    return err;
}

pub fn lineState(ops: anytype, speed: u8) u16 {
    const base = ops.pick(speed) orelse return 0;
    return r16(base, off.syssts0).* & lnst_mask;
}
