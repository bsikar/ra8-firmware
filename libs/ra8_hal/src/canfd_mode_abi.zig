//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the CANFD mode handshakes and open_channel (RA8FW-863). Logic
//! lives in internal/canfd_mode.zig. The priv_ names are kept so the C
//! init and the canfd, ctrl, timing and filter ABIs link unchanged.

const builtin = @import("builtin");
const mode = @import("internal/canfd_mode.zig");

/// Host builds link the C fake-MMIO wait seam (ra8_hw_err.h) so the C
/// suites can hold a status bit. Freestanding builds never see it.
const hosted = builtin.os.tag != .freestanding;
const seam = struct {
    extern fn ra8_fake_mmio_wait_eval(reg: *const volatile anyopaque, iter: u32, real_cond: bool) bool;
};

const Hw = struct {
    base: usize,

    fn at(self: Hw, off: usize) *volatile u32 {
        return @ptrFromInt(self.base + off);
    }
    pub fn read(self: Hw, off: usize) u32 {
        return self.at(off).*;
    }
    pub fn write(self: Hw, off: usize, v: u32) void {
        self.at(off).* = v;
    }
    pub fn wait(self: Hw, off: usize, mask: u32, set: bool) bool {
        const reg = self.at(off);
        var i: u32 = 0;
        while (i < mode.spin) : (i += 1) {
            const cond = ((reg.* & mask) != 0) == set;
            if (if (hosted) seam.ra8_fake_mmio_wait_eval(reg, i, cond) else cond) return true;
        }
        return false;
    }
};

fn hw(reg: *volatile anyopaque) Hw {
    return .{ .base = @intFromPtr(reg) };
}

export fn priv_ra8_canfd_internal_set_channel_mode(reg: *volatile anyopaque, m: c_uint) u16 {
    return mode.setChannelMode(hw(reg), m);
}

export fn priv_ra8_canfd_internal_set_global_mode(reg: *volatile anyopaque, gmdc_value: u32) u16 {
    return mode.setGlobalMode(hw(reg), gmdc_value);
}

export fn priv_ra8_canfd_internal_enable_rx_fifo0(reg: *volatile anyopaque) void {
    mode.enableRxFifo0(hw(reg));
}

export fn priv_ra8_canfd_internal_open_channel(reg: *volatile anyopaque) u16 {
    return mode.openChannel(hw(reg));
}
