//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the ra8_dmac.h channel driver (RA8FW-746), replacing
//! the deleted C driver. The logic is in internal/dmac.zig; this file owns the
//! register pointers and the callback slots. ra8_mstp stays C.

const common = @import("abi_common.zig");
const dmac = @import("internal/dmac.zig");

const tag = "DMAC";
/// `k_ra8_dmac0_base_addr` and `k_ra8_dma_base_addr` (ra8_dmac_regs.h).
const dmac0_base: usize = 0x4000A000;
const channel_stride: usize = 0x40;
const dma_shared_base: usize = 0x4000A800;

extern fn ra8_mstp_enable(id: u16) u16;
extern fn ra8_mstp_disable(id: u16) u16;

var slots: dmac.Slots = .{};

const Hw = struct {
    pub fn channel(_: Hw, ch: u8) ?*volatile dmac.ChannelRegs {
        if (ch >= dmac.channel_count) return null;
        return @ptrFromInt(dmac0_base + @as(usize, ch) * channel_stride);
    }
    pub fn dmast(_: Hw) *volatile u8 {
        return @ptrFromInt(dma_shared_base);
    }
    pub fn mstpEnable(_: Hw, id: u16) u16 {
        return ra8_mstp_enable(id);
    }
    pub fn mstpDisable(_: Hw, id: u16) u16 {
        return ra8_mstp_disable(id);
    }
    pub fn nullPtr(_: Hw, msg: [*:0]const u8) u16 {
        common.ra8_log_emit_error(tag, msg);
        return dmac.null_ptr;
    }
    pub fn fail(_: Hw, msg: [*:0]const u8, err: u16) void {
        common.ra8_log_emit_error(tag, msg);
        common.ra8_log_emit_error_val(tag, "Error", err);
    }
    pub fn infoVal(_: Hw, msg: [*:0]const u8, value: u32) void {
        common.ra8_log_emit_info_val(tag, msg, value);
    }
};

export fn priv_ra8_dmac_internal_mode_disables_dts(normal_val: u32, repeat_block_val: u32, mode: u32) bool {
    return dmac.modeDisablesDts(normal_val, repeat_block_val, mode);
}

export fn priv_ra8_dmac_internal_dmint_extra_irq(irq_each: bool, repeat_block_val: u32, mode: u32) bool {
    return dmac.dmintExtraIrq(irq_each, repeat_block_val, mode);
}

export fn ra8_dmac_start(channel: u8, cfg: ?*const dmac.Config) u16 {
    return dmac.start(Hw{}, channel, cfg);
}

export fn ra8_dmac_stop(channel: u8) u16 {
    return dmac.stop(Hw{}, channel);
}

export fn ra8_dmac_start_repeat(channel: u8, cfg: ?*const dmac.Config) u16 {
    return dmac.startWithMode(Hw{}, channel, cfg, dmac.mode_repeat);
}

export fn ra8_dmac_start_block(channel: u8, cfg: ?*const dmac.Config) u16 {
    return dmac.startBlock(Hw{}, channel, cfg);
}

export fn ra8_dmac_set_address_mode(channel: u8, src_mode: u8, dest_mode: u8) u16 {
    return dmac.setAddressMode(Hw{}, channel, src_mode, dest_mode);
}

export fn ra8_dmac_attach_half_complete_handler(channel: u8, f: ?dmac.CallbackFn, ctx: ?*anyopaque) u16 {
    return slots.attach(channel, true, f, ctx);
}

export fn ra8_dmac_attach_callback(channel: u8, f: ?dmac.CallbackFn, ctx: ?*anyopaque) u16 {
    return slots.attach(channel, false, f, ctx);
}

export fn ra8_dmac_dispatch(channel: u8) void {
    slots.dispatch(channel, false);
}

export fn ra8_dmac_dispatch_half(channel: u8) void {
    slots.dispatch(channel, true);
}

export fn ra8_dmac_software_trigger(channel: u8) u16 {
    return dmac.softwareTrigger(Hw{}, channel);
}

export fn ra8_dmac_is_active(channel: u8, out_active: ?*bool) u16 {
    return dmac.isActive(Hw{}, channel, out_active);
}

export fn ra8_dmac_wait_idle(channel: u8, poll_limit: u32) u16 {
    return dmac.waitIdle(Hw{}, channel, poll_limit);
}
