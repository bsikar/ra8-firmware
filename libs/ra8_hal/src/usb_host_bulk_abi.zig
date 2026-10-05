//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the ra8_usb.h host bulk entry points (RA8FW-753), replacing
//! ra8_usb_host_bulk.c. Logic is in internal/usb_host_bulk.zig; the shared
//! pipe/FIFO helpers stay C in ra8_usb.c and ra8_usb_host_ctrl.c.

const common = @import("abi_common.zig");
const hb = @import("internal/usb_host_bulk.zig");

const tag = "USB";

extern fn priv_pick(speed: u8) ?*anyopaque;
extern fn priv_host_program_devadd(reg: *anyopaque, dev_addr: u8) void;
extern fn priv_pipe_quiesce(reg: *anyopaque, pipe_num: u8) void;
extern fn priv_pipecfg_word(ep_addr: u8, dir: u8, ep_type: u8, dblb_in: bool) u16;
extern fn priv_pipebuf_word(pipe_num: u8, max_packet: u16) u16;
extern fn priv_rmw16(reg: *volatile u16, set_mask: u16, clr_mask: u16) void;
extern fn priv_pipe_pid(reg: *anyopaque, pipe_num: u8, pid: u16) void;
extern fn priv_select_cfifo(reg: *anyopaque, pipe_num: u16, is_in_dir: bool) void;
extern fn priv_wait_frdy(reg: *anyopaque) u16;
extern fn priv_fifo_read(reg: *anyopaque, data: [*]u8, len: u16) void;
extern fn ra8_usb_queue_in(speed: u8, pipe: u8, data: ?[*]const u8, len: u16) u16;

fn regs(base: usize) *anyopaque {
    return @ptrFromInt(base);
}

const Hw = struct {
    pub fn pick(_: Hw, speed: u8) ?usize {
        return @intFromPtr(priv_pick(speed) orelse return null);
    }
    pub fn programDevadd(_: Hw, base: usize, dev_addr: u8) void {
        priv_host_program_devadd(regs(base), dev_addr);
    }
    pub fn pipeQuiesce(_: Hw, base: usize, pipe: u8) void {
        priv_pipe_quiesce(regs(base), pipe);
    }
    pub fn pipecfgWord(_: Hw, ep: u8, dir: u8, ep_type: u8, dblb: bool) u16 {
        return priv_pipecfg_word(ep, dir, ep_type, dblb);
    }
    pub fn pipebufWord(_: Hw, pipe: u8, max_packet: u16) u16 {
        return priv_pipebuf_word(pipe, max_packet);
    }
    pub fn rmw16(_: Hw, reg: *volatile u16, set: u16, clr: u16) void {
        priv_rmw16(reg, set, clr);
    }
    pub fn pipePid(_: Hw, base: usize, pipe: u8, pid: u16) void {
        priv_pipe_pid(regs(base), pipe, pid);
    }
    pub fn selectCfifo(_: Hw, base: usize, pipe: u8, is_in: bool) void {
        priv_select_cfifo(regs(base), pipe, is_in);
    }
    pub fn waitFrdy(_: Hw, base: usize) u16 {
        return priv_wait_frdy(regs(base));
    }
    pub fn fifoRead(_: Hw, base: usize, dst: [*]u8, len: u16) void {
        priv_fifo_read(regs(base), dst, len);
    }
    pub fn queueIn(_: Hw, speed: u8, pipe: u8, data: [*]const u8, len: u16) u16 {
        return ra8_usb_queue_in(speed, pipe, data, len);
    }
    pub fn nullPtr(_: Hw, msg: [*:0]const u8) u16 {
        common.ra8_log_emit_error(tag, msg);
        return hb.null_ptr;
    }
};

export fn ra8_usb_host_set_target(speed: u8, dev_addr: u8) u16 {
    return hb.setTarget(Hw{}, speed, dev_addr);
}

export fn ra8_usb_host_pipe_setup(speed: u8, pipe_num: u8, dev_addr: u8, ep_num: u8, device_to_host: bool, max_packet: u16) u16 {
    return hb.pipeSetup(Hw{}, speed, pipe_num, dev_addr, ep_num, device_to_host, max_packet);
}

export fn ra8_usb_host_bulk_out(speed: u8, pipe_num: u8, data: ?[*]const u8, len: u16) u16 {
    return hb.bulkOut(Hw{}, speed, pipe_num, data, len);
}

export fn ra8_usb_host_bulk_in(speed: u8, pipe_num: u8, buf: ?[*]u8, max_len: u16, out_received: ?*u16) u16 {
    return hb.bulkIn(Hw{}, speed, pipe_num, buf, max_len, out_received);
}

export fn ra8_usb_host_line_state(speed: u8) u16 {
    return hb.lineState(Hw{}, speed);
}
