//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the SCI handler attach, error status and stop calls
//! (RA8FW-906, was part of ra8_sci.c). s_sci_state stays defined in
//! ra8_sci.c (ra8_sci_dma_isr shares it). HUM Ch 38.2.5, 38.2.17, 38.2.24.

const builtin = @import("builtin");
const common = @import("abi_common.zig");
const ctl = @import("internal/sci_ctl.zig");
const sd = @import("internal/sci_dma_isr.zig");

extern var s_sci_state: [@as(usize, ctl.channel_max) + 1]sd.State;
extern fn ra8_mstp_enable(id: u16) u16;
extern fn ra8_mstp_disable(id: u16) u16;

const tag = "SCI";
const freestanding = builtin.os.tag == .freestanding;
const sci0_base: usize = 0x4035_8000;
const channel_stride: usize = 0x100;

const Reg = enum(usize) { ccr0 = 0x08, csr = 0x48, cfclr = 0x68 };

fn reg(channel: u8, r: Reg) *volatile u32 {
    return @ptrFromInt(sci0_base + @as(usize, channel) * channel_stride + @backingInt(r));
}

/// PRIMASK save + mask on target, a no-op on host (ra8_register_guard.h).
fn guardEnter() u32 {
    if (!freestanding) return 0;
    const saved = asm volatile ("mrs %[r], primask"
        : [r] "=r" (-> u32),
    );
    asm volatile ("cpsid i" ::: "memory");
    return saved;
}

fn guardExit(saved: u32) void {
    if (!freestanding) return;
    asm volatile ("msr primask, %[s]"
        :
        : [s] "r" (saved),
        : "memory");
}

fn toggleIe(channel: u8, bit: u32, on: bool) void {
    const r = reg(channel, .ccr0);
    r.* = ctl.withIe(r.*, bit, on);
}

/// The RXI ISR reads rx_fn/rx_ctx and read-modify-writes CCR0, so the
/// publish and the RIE toggle run with interrupts masked.
export fn ra8_sci_attach_rx_handler(channel: u8, f: ?sd.RxFn, ctx: ?*anyopaque) u16 {
    if (channel > ctl.channel_max) return common.k_ra8_err_invalid_arg;
    const saved = guardEnter();
    s_sci_state[channel].rx_fn = f;
    s_sci_state[channel].rx_ctx = ctx;
    toggleIe(channel, ctl.ccr0_rie, f != null);
    guardExit(saved);
    return common.k_ra8_ok;
}

/// Same contract for TXI and TIE.
export fn ra8_sci_attach_tx_handler(channel: u8, f: ?sd.TxFn, ctx: ?*anyopaque) u16 {
    if (channel > ctl.channel_max) return common.k_ra8_err_invalid_arg;
    const saved = guardEnter();
    s_sci_state[channel].tx_fn = f;
    s_sci_state[channel].tx_ctx = ctx;
    toggleIe(channel, ctl.ccr0_tie, f != null);
    guardExit(saved);
    return common.k_ra8_ok;
}

export fn ra8_sci_get_errors(channel: u8, out_mask: ?*u8) u16 {
    const out = out_mask orelse {
        common.ra8_log_emit_error(tag, "get_errors: out");
        return common.k_ra8_err_null_ptr;
    };
    if (channel > ctl.channel_max) return common.k_ra8_err_invalid_arg;
    out.* = ctl.errMask(reg(channel, .csr).*);
    return common.k_ra8_ok;
}

export fn ra8_sci_clear_errors(channel: u8) u16 {
    if (channel > ctl.channel_max) return common.k_ra8_err_invalid_arg;
    reg(channel, .cfclr).* = ctl.clear_mask;
    return common.k_ra8_ok;
}

export fn ra8_sci_enter_stop(channel: u8) u16 {
    if (channel > ctl.channel_max) return common.k_ra8_err_invalid_arg;
    reg(channel, .ccr0).* = 0;
    return ra8_mstp_disable(ctl.mstpId(channel));
}

export fn ra8_sci_exit_stop(channel: u8) u16 {
    if (channel > ctl.channel_max) return common.k_ra8_err_invalid_arg;
    return ra8_mstp_enable(ctl.mstpId(channel));
}

/// init/deinit in ra8_sci.c take the MSTP id from here (one table).
export fn priv_ra8_sci_mstp_id(channel: u8) u16 {
    return ctl.mstpId(channel);
}
