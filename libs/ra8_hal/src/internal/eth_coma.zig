//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Ethernet Common Agent (inc/ra8_eth_coma.h, RA8FW-550). HUM Ch 31
//! "Ethernet Common Agent (COMA)" p 1590, Ch 31.3.2.7 "CABPIRM" p 1599.

/// COMA control/status block (shares the MFWD window, inc/ra8_ether_regs.h).
pub const regs_base: usize = 0x403C0000;
/// COMA bring-up agent block (RRC / RCEC / CABPIRM).
pub const agent_base: usize = 0x403C9000;

pub const off_ctrl: usize = 0x00;
pub const off_sts: usize = 0x04;
pub const off_ie: usize = 0x08;
pub const off_iclr: usize = 0x0C;

pub const off_rrc: usize = 0x004;
pub const off_rcec: usize = 0x008;
pub const off_cabpirm: usize = 0x140;

pub const rrc_rr: u32 = 1 << 0;
pub const rcec_rce: u32 = 1 << 16;
pub const rcec_ace_mask: u32 = 0x7F;
pub const cabpirm_bpiog: u32 = 1 << 0;
pub const cabpirm_bpr: u32 = 1 << 1;

pub const Error = error{BprTimeout};

/// Register windows plus the bring-up timing; host tests point the bases at
/// fake blocks and set `delay_iters` to 0.
pub const Window = struct {
    regs: usize = regs_base,
    agent: usize = agent_base,
    delay_iters: u32 = 3_000_000,
    bpr_budget: u32 = 1_000_000,

    pub fn reg(w: Window, off: usize) *volatile u32 {
        return @ptrFromInt(w.regs + off);
    }

    pub fn agentReg(w: Window, off: usize) *volatile u32 {
        return @ptrFromInt(w.agent + off);
    }
};

/// Zero CTRL, STS, IE and ICLR (ra8_eth_coma_init after MSTP).
pub fn reset(w: Window) void {
    w.reg(off_ctrl).* = 0;
    w.reg(off_sts).* = 0;
    w.reg(off_ie).* = 0;
    w.reg(off_iclr).* = 0;
}

/// Zero CTRL and IE (ra8_eth_coma_deinit).
pub fn quiesce(w: Window) void {
    w.reg(off_ctrl).* = 0;
    w.reg(off_ie).* = 0;
}

pub fn status(w: Window) u32 {
    return w.reg(off_sts).*;
}

/// ICLR = mask, then STS &= ~mask.
pub fn clearStatus(w: Window, mask: u32) void {
    w.reg(off_iclr).* = mask;
    w.reg(off_sts).* = w.reg(off_sts).* & ~mask;
}

/// Read STS, clear it through ICLR and STS, return what was pending.
pub fn takeStatus(w: Window) u32 {
    const mask = w.reg(off_sts).*;
    w.reg(off_iclr).* = mask;
    w.reg(off_sts).* = 0;
    return mask;
}

fn settle(w: Window) void {
    var i: u32 = 0;
    while (i < w.delay_iters) : (i += 1) asm volatile ("nop");
}

/// Poll CABPIRM.BPR within the budget (HUM Ch 31.3.2.7 p 1599).
pub fn waitPool(w: Window) Error!void {
    var i: u32 = 0;
    while (i < w.bpr_budget) : (i += 1) {
        if (w.agentReg(off_cabpirm).* & cabpirm_bpr != 0) return;
    }
    return error.BprTimeout;
}

/// Steps 1 and 2: pulse RRC.RR, then enable the switch clock alone.
pub fn resetAndClock(w: Window) void {
    w.agentReg(off_rrc).* = rrc_rr;
    w.agentReg(off_rrc).* = 0;
    settle(w);
    w.agentReg(off_rcec).* = rcec_rce;
    settle(w);
}

/// Step 3a: start the buffer-pool reset; BPR self-sets 512 clocks later.
pub fn kickPool(w: Window) void {
    w.agentReg(off_cabpirm).* = cabpirm_bpiog;
}

/// Step 4: fan out every per-agent clock (RCE | ACE[6:0]).
pub fn enableAgents(w: Window) void {
    w.agentReg(off_rcec).* = rcec_rce | rcec_ace_mask;
    settle(w);
}

/// Reset pulse, switch clock, buffer-pool init, then every agent clock.
pub fn bringup(w: Window) Error!void {
    resetAndClock(w);
    kickPool(w);
    try waitPool(w);
    enableAgents(w);
}
