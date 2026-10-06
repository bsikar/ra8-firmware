//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_dotf_init / ra8_dotf_deinit (RA8FW-838). The sequencing is
//! in internal/dotf_life.zig; the state table and handler are defined in
//! dotf_state_abi.zig and dotf_handler_abi.zig.

const common = @import("abi_common.zig");
const handler = @import("internal/dotf_handler.zig");
const life = @import("internal/dotf_life.zig");
const power = @import("internal/dotf_power.zig");
const state = @import("internal/dotf_state.zig");
const status = @import("internal/dotf_status.zig");

const tag = "DOTF";
const off_convareast: usize = 0x0;
const off_convaread: usize = 0x4;

extern var s_dotf_state: [power.channel_count]state.ChanState;
extern var s_dotf_fn: ?handler.EventFn;
extern var s_dotf_ctx: ?*anyopaque;
extern fn ra8_mstp_enable(id: u16) u16;
extern fn ra8_mstp_disable(id: u16) u16;

fn word(addr: usize) *volatile u32 {
    return @ptrFromInt(addr);
}

const Hw = struct {
    pub fn mstpEnable(_: Hw, id: u16) u16 {
        return ra8_mstp_enable(id);
    }
    pub fn mstpDisable(_: Hw, id: u16) u16 {
        return ra8_mstp_disable(id);
    }
    pub fn channelReset(_: Hw, ch: u8) void {
        const base = status.base + @as(usize, ch) * status.stride;
        word(base + off_convareast).* = 0;
        word(base + off_convaread).* = 0;
        word(status.reg00(ch)).* = state.reg00_disable;
    }
    pub fn disable(_: Hw, ch: u8) void {
        word(status.reg00(ch)).* = state.reg00_disable;
    }
    pub fn stateReset(_: Hw, ch: u8) void {
        state.reset(&s_dotf_state[ch]);
    }
    pub fn clearHandler(_: Hw) void {
        s_dotf_fn = null;
        s_dotf_ctx = null;
    }
    pub fn fail(_: Hw, msg: [*:0]const u8, err: u16) void {
        common.ra8_log_emit_error(tag, msg);
        common.ra8_log_emit_error_val(tag, "Error", err);
    }
    pub fn info(_: Hw, msg: [*:0]const u8) void {
        common.ra8_log_emit_info(tag, msg);
    }
};

export fn ra8_dotf_init() u16 {
    return life.init(Hw{});
}

export fn ra8_dotf_deinit() u16 {
    life.deinit(Hw{});
    return common.k_ra8_ok;
}
