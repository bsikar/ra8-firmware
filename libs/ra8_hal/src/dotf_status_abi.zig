//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the DOTF self-test and status read (RA8FW-827). The register
//! logic is in internal/dotf_status.zig.

const builtin = @import("builtin");
const common = @import("abi_common.zig");
const power = @import("internal/dotf_power.zig");
const st = @import("internal/dotf_status.zig");

const tag = "DOTF";

/// Host C tests arm failures through this seam, as ra8_hw_wait_flag_clear32
/// does under UNIT_TEST. Freestanding builds never see it.
const hosted = builtin.os.tag != .freestanding;
const seam = struct {
    extern fn ra8_fake_mmio_wait_eval(reg: *const volatile anyopaque, iter: u32, real_cond: bool) bool;
};

const Reg = struct {
    p: *volatile u32,
    pub fn read(self: Reg) u32 {
        return self.p.*;
    }
    pub fn write(self: Reg, v: u32) void {
        self.p.* = v;
    }
    pub fn eval(self: Reg, iter: u32, cond: bool) bool {
        return if (hosted) seam.ra8_fake_mmio_wait_eval(self.p, iter, cond) else cond;
    }
};

fn nullPtr(msg: [*:0]const u8) u16 {
    common.ra8_log_emit_error(tag, msg);
    return common.k_ra8_err_null_ptr;
}

fn reg(channel: u8) Reg {
    return .{ .p = @ptrFromInt(st.reg00(channel)) };
}

export fn ra8_dotf_run_self_test(channel: u8, out_status: ?*u32) u16 {
    const out = out_status orelse return nullPtr("out_status must not be nullptr");
    if (!power.channelInRange(channel)) return common.k_ra8_err_invalid_arg;
    const r = reg(channel);
    const res = st.selfTest(r, r);
    out.* = res.status;
    return if (res.done) common.k_ra8_ok else common.k_ra8_err_hw_timeout;
}

export fn ra8_dotf_get_status(channel: u8, out_mask: ?*u32) u16 {
    const out = out_mask orelse return nullPtr("out_mask must not be nullptr");
    if (!power.channelInRange(channel)) return common.k_ra8_err_invalid_arg;
    out.* = reg(channel).read();
    return common.k_ra8_ok;
}
