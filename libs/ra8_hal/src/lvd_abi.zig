//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_lvd_api.h (RA8FW-797), replacing ra8_lvd.c. The logic is in
//! internal/lvd.zig. This file exports g_lvd_map and the priv_ra8_lvd_internal_*
//! helpers that lvd_runtime_abi.zig and lvd_events_abi.zig link against.

const common = @import("abi_common.zig");
const lvd = @import("internal/lvd.zig");

const tag = "LVD";
const ok = lvd.codes.ok;

const Hw = struct {
    pub fn read8(_: Hw, a: usize) u8 {
        const p: *volatile u8 = @ptrFromInt(a);
        return p.*;
    }
    pub fn write8(_: Hw, a: usize, v: u8) void {
        const p: *volatile u8 = @ptrFromInt(a);
        p.* = v;
    }
};

const hw = Hw{};

export const g_lvd_map: [4]lvd.Map = lvd.map;

/// RA8_RETURN_ON_ERROR: the message, then the code.
fn failed(rc: u16, msg: [*:0]const u8) bool {
    if (rc == ok) return false;
    common.ra8_log_emit_error(tag, msg);
    common.ra8_log_emit_error_val(tag, "Error", rc);
    return true;
}

fn mapOf(channel: u8, msg: [*:0]const u8) ?lvd.Map {
    const idx = lvd.channelIdx(channel) orelse {
        _ = failed(lvd.codes.invalid_arg, msg);
        return null;
    };
    return lvd.map[idx];
}

export fn priv_ra8_lvd_internal_reject_hvd_after(hvd_val: u32, after_assert_val: u32, hysteresis: u32, negate: u32) bool {
    return lvd.rejectHvdAfter(hvd_val, after_assert_val, hysteresis, negate);
}

export fn priv_ra8_lvd_internal_set_ri_bit(reset_val: u32, reset_on_rise_val: u32, response: u32) bool {
    return lvd.setRiBit(reset_val, reset_on_rise_val, response);
}

export fn priv_ra8_lvd_internal_channel_to_idx(channel: u8, out_idx: *u8) u16 {
    out_idx.* = lvd.channelIdx(channel) orelse return lvd.codes.invalid_arg;
    return ok;
}

export fn priv_ra8_lvd_internal_validate_div(div: u8) u16 {
    return if (lvd.divOk(div)) ok else lvd.codes.invalid_arg;
}

export fn priv_ra8_lvd_internal_read_ri(map: *const lvd.Map) u8 {
    return hw.read8(map.cr0) & lvd.cr0.ri;
}

export fn priv_ra8_lvd_internal_cr0_rmw(map: *const lvd.Map, clear_mask: u8, set_bits: u8) void {
    lvd.cr0Rmw(hw, map.*, clear_mask, set_bits);
}

export fn ra8_lvd_channel_init(channel: u8, cfg: ?*const lvd.Cfg) u16 {
    const c = cfg orelse {
        common.ra8_log_emit_error(tag, "cfg must not be nullptr");
        return lvd.codes.null_ptr;
    };
    const m = mapOf(channel, "lvd_init: bad channel") orelse return lvd.codes.invalid_arg;
    const rc = lvd.validate(m, c.*);
    if (failed(rc, "lvd_init: bad cfg")) return rc;
    lvd.programCmpcr(hw, m, c.*);
    lvd.programCr0Chain(hw, m, c.*);
    common.ra8_log_emit_info_val(tag, "lvd_init ch", channel);
    return ok;
}

export fn ra8_lvd_channel_deinit(channel: u8) u16 {
    const m = mapOf(channel, "lvd_deinit: bad channel") orelse return lvd.codes.invalid_arg;
    lvd.deinit(hw, m);
    return ok;
}

export fn ra8_lvd_set_threshold(channel: u8, threshold: u8) u16 {
    const m = mapOf(channel, "lvd_set_threshold: bad channel") orelse return lvd.codes.invalid_arg;
    if (!lvd.thresholdOk(threshold)) {
        _ = failed(lvd.codes.invalid_arg, "lvd_set_threshold: bad threshold");
        return lvd.codes.invalid_arg;
    }
    lvd.setThreshold(hw, m, threshold);
    return ok;
}

export fn ra8_lvd_set_irq_edge(channel: u8, edge: u8) u16 {
    const m = mapOf(channel, "lvd_set_irq_edge: bad channel") orelse return lvd.codes.invalid_arg;
    if (!lvd.edgeOk(edge)) {
        _ = failed(lvd.codes.invalid_arg, "lvd_set_irq_edge: bad edge");
        return lvd.codes.invalid_arg;
    }
    if (!m.has_irq) return lvd.codes.not_supported;
    lvd.setEdge(hw, m, edge);
    return ok;
}

export fn ra8_lvd_set_irq_kind(channel: u8, kind: u8) u16 {
    const m = mapOf(channel, "lvd_set_irq_kind: bad channel") orelse return lvd.codes.invalid_arg;
    if (!m.has_irq) return lvd.codes.not_supported;
    lvd.setKind(hw, m, kind);
    return ok;
}
