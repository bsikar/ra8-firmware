//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `libs/ra8_core/inc/ra8_systick.h` (#2830).
//!
//! The arithmetic behind this file deals in Zig errors and the register
//! window deals in named registers. This one maps them onto the `ra8_err_t`
//! codes the header promises and emits the same log lines the C emitted,
//! through `ra8_log_emit_error` in ra8_log.c, which is still C.
//!
//! `ra8_dwt_cyccnt_enable` delegates the DEMCR.TRCENA unlock to ra8_scb
//! rather than poking DEMCR a second time (#588); ra8_scb is Zig too as of
//! the fault block (#2868).

const regs = @import("systick_regs");
const reload_math = @import("systick_reload");

/// `ra8_err_t` values this module returns, from `inc/ra8_err.h`.
const err = struct {
    pub const ok: c_int = 0;
    pub const invalid_arg: c_int = 0x103;
    pub const out_of_range: c_int = 0x208;
    pub const null_ptr: c_int = 0x504;
};

/// `ra8_systick_clock_source_t` values, from `inc/ra8_systick.h`.
const clock_source = struct {
    pub const external: u8 = 0;
    pub const cpu: u8 = 1;
};

const tag: [*:0]const u8 = "ra8_systick";

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;
extern fn ra8_scb_trace_enable() void;

/// Refuse a reload wider than SYST_RVR rather than truncating it, which would
/// silently make the tick far too fast.
fn rejectWideReload(reload: u32) ?c_int {
    if (reload_math.fits(reload)) return null;
    ra8_log_emit_error(tag, "reload exceeds 24-bit SysTick range");
    return err.out_of_range;
}

pub export fn ra8_systick_reload_for(
    cpu_hz: u32,
    tick_hz: u32,
    out_reload: ?*u32,
) callconv(.c) c_int {
    const slot = out_reload orelse {
        ra8_log_emit_error(tag, "out_reload is NULL");
        return err.null_ptr;
    };

    const reload = reload_math.reloadFor(cpu_hz, tick_hz) catch |e| switch (e) {
        error.ZeroRate => {
            ra8_log_emit_error(tag, "cpu_hz / tick_hz must be non-zero");
            return err.invalid_arg;
        },
        error.ClockBelowTick => {
            ra8_log_emit_error(tag, "cpu_hz below one tick period");
            return err.invalid_arg;
        },
        error.OutOfRange => {
            ra8_log_emit_error(tag, "reload exceeds 24-bit SysTick range");
            return err.out_of_range;
        },
    };

    slot.* = reload;
    return err.ok;
}

pub export fn ra8_systick_configure(reload: u32, src: u8, tick_irq: bool) callconv(.c) c_int {
    if (rejectWideReload(reload)) |code| return code;

    // Arm v8-M ARM: clear ENABLE first so the reload and current writes land
    // on a stopped counter, then re-enable with the requested config.
    regs.write(regs.addr.syst_csr, 0);
    regs.write(regs.addr.syst_rvr, reload);
    regs.write(regs.addr.syst_cvr, 0);

    var csr: u32 = regs.bits.csr_enable;
    if (tick_irq) csr |= regs.bits.csr_tickint;
    if (src == clock_source.cpu) csr |= regs.bits.csr_clksource;
    regs.write(regs.addr.syst_csr, csr);

    return err.ok;
}

pub export fn ra8_systick_set_reload(reload: u32) callconv(.c) c_int {
    if (rejectWideReload(reload)) |code| return code;

    // Arm v8-M ARM: re-arm the reload, then any write to SYST_CVR clears it so
    // the next count starts from the new reload. The control bits in SYST_CSR
    // are deliberately left as they are.
    regs.write(regs.addr.syst_rvr, reload);
    regs.write(regs.addr.syst_cvr, 0);
    return err.ok;
}

pub export fn ra8_systick_current_value() callconv(.c) u32 {
    // Reading SYST_CVR is side-effect free; only a write clears the counter.
    return regs.read(regs.addr.syst_cvr);
}

pub export fn ra8_dwt_cyccnt_enable() callconv(.c) void {
    // DEMCR.TRCENA unlocks the DWT unit and has exactly one writer in the
    // tree, ra8_scb, which owns the debug and trace gate (#588).
    ra8_scb_trace_enable();
    regs.setBits(regs.addr.dwt_ctrl, regs.bits.dwt_cyccntena);
}

pub export fn ra8_dwt_cyccnt_reset() callconv(.c) void {
    regs.write(regs.addr.dwt_cyccnt, 0);
}

pub export fn ra8_dwt_cyccnt_read() callconv(.c) u32 {
    return regs.read(regs.addr.dwt_cyccnt);
}
