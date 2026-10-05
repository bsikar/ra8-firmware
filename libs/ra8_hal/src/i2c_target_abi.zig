//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the RIIC target (peripheral) role (RA8FW-769), which replaces
//! ra8_i2c_peripheral.c. Same symbols, check order and error codes as the
//! C. s_i2c_state and g_i2c_tag stay owned by ra8_i2c.c. Logic is in
//! internal/i2c_target.zig.

const common = @import("abi_common.zig");
const t = @import("internal/i2c_target.zig");

const ok = common.k_ra8_ok;
const channel_count = 3;
const base: usize = 0x4025E000; // HUM Ch 39.2.1, IICn = base + 0x100 * n
const stride: usize = 0x100;

const Handler = *const fn (?*anyopaque, u8) callconv(.c) void;

/// Mirror of ra8_i2c_state_t (natural C layout).
const State = extern struct {
    initialized: bool,
    bus_held: bool,
    peripheral_active: bool,
    handler: ?Handler,
    ctx: ?*anyopaque,
};

/// Mirror of ra8_i2c_peripheral_cfg_t.
const Cfg = extern struct {
    own_addr_7b: u8,
    slot: u8,
    general_call: bool,
    clock_stretch: bool,
    irq_enable: bool,
};

extern var s_i2c_state: [channel_count]State;
extern const g_i2c_tag: [*:0]const u8;

fn regs(channel: u8) ?*volatile t.Regs {
    if (channel >= channel_count) return null;
    return @ptrFromInt(base + @as(usize, channel) * stride);
}

fn nullPtr(msg: [*:0]const u8) u16 {
    common.ra8_log_emit_error(g_i2c_tag, msg);
    return common.k_ra8_err_null_ptr;
}

export fn priv_ra8_i2c_internal_peripheral_poll_done(icsr1: u8, icsr2: u8) bool {
    return t.pollDone(icsr1, icsr2);
}

export fn priv_ra8_i2c_internal_peripheral_rx_continue(icsr2: u8, received: u32, capacity: u32) bool {
    return t.rxContinue(icsr2, received, capacity);
}

export fn priv_ra8_i2c_internal_peripheral_tx_done(icsr2: u8) bool {
    return t.txDone(icsr2);
}

export fn priv_ra8_i2c_internal_peripheral_tx_continue(icsr2: u8, sent: u32, len: u32) bool {
    return t.txContinue(icsr2, sent, len);
}

export fn priv_ra8_i2c_internal_target_drain_rx(reg: *volatile t.Regs, buf: [*]u8, capacity: u32, out_count: *u32) u16 {
    const rx = t.drainRx(reg, buf[0..capacity]);
    out_count.* = rx.count;
    return if (rx.timed_out) common.k_ra8_err_hw_timeout else ok;
}

export fn ra8_i2c_peripheral_init(channel: u8, cfg: ?*const Cfg) u16 {
    const r = regs(channel) orelse return nullPtr("i2c_target_init: channel");
    const c = cfg orelse return nullPtr("i2c_target_init: cfg");
    if (c.own_addr_7b > t.addr_7b_max) return common.k_ra8_err_invalid_arg;
    if (c.slot > t.slot_max) return common.k_ra8_err_invalid_arg;
    t.arm(r, .{
        .slot = c.slot,
        .addr_7b = c.own_addr_7b,
        .general_call = c.general_call,
        .clock_stretch = c.clock_stretch,
        .irq_enable = c.irq_enable,
    });
    s_i2c_state[channel].peripheral_active = true;
    return ok;
}

export fn ra8_i2c_peripheral_deinit(channel: u8) u16 {
    const r = regs(channel) orelse return common.k_ra8_err_invalid_arg;
    if (!s_i2c_state[channel].peripheral_active) return common.k_ra8_err_not_initialized;
    t.disarm(r);
    s_i2c_state[channel].peripheral_active = false;
    return ok;
}

export fn ra8_i2c_peripheral_poll(channel: u8, out_event: ?*u8) u16 {
    const r = regs(channel) orelse return nullPtr("i2c_target_poll: channel");
    const out = out_event orelse return nullPtr("i2c_target_poll: out_event");
    out.* = t.Event.none;
    if (!s_i2c_state[channel].peripheral_active) return common.k_ra8_err_not_initialized;
    out.* = t.poll(r);
    return ok;
}

export fn ra8_i2c_peripheral_receive(channel: u8, buf: ?[*]u8, capacity: u32, out_received: ?*u32) u16 {
    const r = regs(channel) orelse return nullPtr("i2c_target_receive: channel");
    const b = buf orelse return nullPtr("i2c_target_receive: buf");
    const out = out_received orelse return nullPtr("i2c_target_receive: out_received");
    out.* = 0;
    if (capacity == 0) return common.k_ra8_err_invalid_arg;
    if (!s_i2c_state[channel].peripheral_active) return common.k_ra8_err_not_initialized;
    if (!t.wait(r, t.msk.icsr2_rdrf)) return common.k_ra8_err_hw_timeout;
    _ = r.icdrr; // address byte
    const rx = t.drainRx(r, b[0..capacity]);
    r.icsr2 = r.icsr2 & ~t.msk.icsr2_stop;
    out.* = rx.count;
    if (rx.count == 0 and rx.timed_out) return common.k_ra8_err_hw_timeout;
    return ok;
}

export fn ra8_i2c_peripheral_transmit(channel: u8, data: ?[*]const u8, len: u32, out_sent: ?*u32) u16 {
    const r = regs(channel) orelse return nullPtr("i2c_target_transmit: channel");
    const d = data orelse return nullPtr("i2c_target_transmit: data");
    const out = out_sent orelse return nullPtr("i2c_target_transmit: out_sent");
    out.* = 0;
    if (len == 0) return common.k_ra8_err_invalid_arg;
    if (!s_i2c_state[channel].peripheral_active) return common.k_ra8_err_not_initialized;
    const sent = t.fillTx(r, d[0..len]);
    const ended = t.finishTx(r);
    out.* = sent;
    return if (ended) ok else common.k_ra8_err_hw_timeout;
}

export fn ra8_i2c_peripheral_attach_handler(channel: u8, handler: ?Handler, ctx: ?*anyopaque) u16 {
    if (channel >= channel_count) return common.k_ra8_err_invalid_arg;
    s_i2c_state[channel].handler = handler;
    s_i2c_state[channel].ctx = ctx;
    return ok;
}

export fn ra8_i2c_peripheral_dispatch(channel: u8) void {
    const r = regs(channel) orelse return;
    const st = &s_i2c_state[channel];
    if (!st.peripheral_active) return;
    const handler = st.handler orelse return;
    const icsr1 = r.icsr1;
    const icsr2 = r.icsr2;
    const iccr2 = r.iccr2;
    const event = t.dispatchEvent(icsr1, icsr2, iccr2);
    if (event != t.Event.none) handler(st.ctx, event);
}
