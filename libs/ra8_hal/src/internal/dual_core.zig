//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! CPU1 release, halt and run-state over the CPU_CTRL block (RA8FW-766),
//! ported from ra8_dual_core.c. Register access, the ACT poll and logging
//! go through a `hw` value so host tests run on in-memory registers.
//! HUM Ch 2.9.1 (CPU1INITVTOR, CPU1WAITCR, CPU1ACTCSR).

pub const ok: u16 = 0;
pub const invalid_arg: u16 = 0x103;
pub const not_supported: u16 = 0x107;
pub const timeout: u16 = 0x108;
pub const null_ptr: u16 = 0x504;

/// `ra8_dual_core_field_t` (ra8_dual_core.h).
pub const waitcr_cpuwait: u8 = 1 << 0;
pub const actcsr_actreq: u16 = 1 << 0;
pub const actcsr_act: u16 = 1 << 7;
pub const actcsr_key_shift: u4 = 8;
pub const actcsr_key: u16 = 0xA5;
/// `k_ra8_dual_core_release_poll_max`.
pub const release_poll_max: u32 = 1_000_000;
/// CPU1INITVTOR wants a 128 B aligned vector table; the stack is AAPCS 8 B.
pub const align_vtor: usize = 128;
pub const align_sp: usize = 8;

/// The keyed ACTREQ write: 0xA5 in the key byte, ACTREQ set.
pub const actreq_word: u16 = (actcsr_key << actcsr_key_shift) | actcsr_actreq;

fn waitAct(hw: anytype) u16 {
    var i: u32 = 0;
    while (i < release_poll_max) : (i += 1) {
        const act_set = (hw.actcsrRead() & actcsr_act) != 0;
        if (hw.actPoll(i, act_set)) return ok;
    }
    hw.logError("release: ACTCSR.ACT did not assert");
    return timeout;
}

/// Argument checks, then CPU1INITVTOR, CPU1WAITCR = 0 and the keyed
/// ACTREQ, then a bounded poll for ACT (HUM Ch 2.9.1.9 p 130).
pub fn release(hw: anytype, is_cpu0: bool, entry: ?*anyopaque, sp: ?*anyopaque) u16 {
    if (entry == null or sp == null) {
        hw.logError("release: NULL entry or sp");
        return null_ptr;
    }
    const e = @intFromPtr(entry);
    if (e & (align_vtor - 1) != 0) {
        hw.logError("release: entry not 128-byte aligned");
        return invalid_arg;
    }
    if (@intFromPtr(sp) & (align_sp - 1) != 0) {
        hw.logError("release: sp not 8-byte aligned");
        return invalid_arg;
    }
    if (!is_cpu0) {
        hw.logError("release: caller is not CPU0");
        return not_supported;
    }
    hw.initvtorWrite(@truncate(e));
    hw.waitcrWrite(0);
    hw.actcsrWrite(actreq_word);
    return waitAct(hw);
}

pub fn halt(hw: anytype, is_cpu0: bool) u16 {
    if (!is_cpu0) {
        hw.logError("halt: caller is not CPU0");
        return not_supported;
    }
    hw.waitcrWrite(waitcr_cpuwait);
    return ok;
}

/// Running means ACT is set and CPUWAIT is clear.
pub fn isRunning(hw: anytype) bool {
    if (hw.actcsrRead() & actcsr_act == 0) return false;
    return hw.waitcrRead() & waitcr_cpuwait == 0;
}

/// The off-target register model: a write without the 0xA5 key is
/// ignored, ACTREQ latches ACT, and WAITCR keeps only CPUWAIT.
pub const Fake = extern struct {
    initvtor: u32 = 0,
    actcsr: u16 = 0,
    waitcr: u8 = 0,

    pub fn writeActcsr(f: *Fake, value: u16) void {
        if ((value >> actcsr_key_shift) & 0xFF != actcsr_key) return;
        if (value & actcsr_actreq != 0) f.actcsr |= actcsr_act;
    }
    pub fn writeWaitcr(f: *Fake, value: u8) void {
        f.waitcr = value & waitcr_cpuwait;
    }
};
