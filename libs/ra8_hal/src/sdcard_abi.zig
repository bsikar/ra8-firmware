//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_sdcard.h (RA8FW-791), replacing ra8_sdcard.c. The SDHI
//! driver (ra8_sdhi.c) stays C and is reached through externs.

const common = @import("abi_common.zig");
const sd = @import("internal/sdcard.zig");

const tag = "SDCARD";
const ok = common.k_ra8_ok;
const bus_width_4bit: u8 = 4;

/// ra8_sdcard_cfg_t.
const Cfg = extern struct { instance: u8, bus_width: u8 };

comptime {
    if (@sizeOf(Cfg) != 2) @compileError("Cfg size");
}

extern fn ra8_sdhi_init(instance: u8) u16;
extern fn ra8_sdhi_deinit(instance: u8) u16;
extern fn ra8_sdhi_send_command(instance: u8, cmd: u32, arg: u32, out_rsp: *[4]u32) u16;
extern fn ra8_sdhi_set_clock(instance: u8, divider: u32) u16;
extern fn ra8_sdhi_set_bus_width_4bit(instance: u8, rca: u16) u16;
extern fn ra8_sdhi_read_block(instance: u8, lba: u32, buf: [*]u8, count: u32) u16;
extern fn ra8_sdhi_write_block(instance: u8, lba: u32, buf: [*]const u8, count: u32) u16;

const State = struct {
    capacity_blocks: u32 = 0,
    rca: u16 = 0,
    kind: u8 = sd.type_unknown,
    instance: u8 = 0,
    initialized: bool = false,
};

var state: State = .{};

/// RA8_RETURN_ON_ERROR: the message, then "Error" with the code.
fn logFailed(err: u16, msg: [*:0]const u8) bool {
    if (err == ok) return false;
    common.ra8_log_emit_error(tag, msg);
    common.ra8_log_emit_error_val(tag, "Error", err);
    return true;
}

fn nullPtr(msg: [*:0]const u8) u16 {
    common.ra8_log_emit_error(tag, msg);
    return common.k_ra8_err_null_ptr;
}

const Host = struct {
    instance: u8,
    pub fn send(h: Host, cmd: u32, arg: u32, rsp: *[4]u32) u16 {
        return ra8_sdhi_send_command(h.instance, cmd, arg, rsp);
    }
    pub fn failed(_: Host, err: u16, msg: [*:0]const u8) bool {
        return logFailed(err, msg);
    }
};

const Online = struct { rca: u16 = 0, blocks: u32 = 0, high_capacity: bool = false };

/// SDHI bring-up, identify, ACMD41, publish and select. Any card-side
/// failure releases the SDHI again.
fn cardOnline(instance: u8, out: *Online) u16 {
    const hw = ra8_sdhi_init(instance);
    if (logFailed(hw, "sdhi_init")) return hw;
    const host = Host{ .instance = instance };
    const id = sd.identify(host);
    if (id != ok) return release(instance, id);
    var ocr: u32 = 0;
    const e41 = sd.acmd41(host, &ocr);
    if (e41 != ok) return release(instance, e41);
    out.high_capacity = ocr & sd.ocr_ccs != 0;
    const pub_err = sd.publishAndSelect(host, &out.rca, &out.blocks);
    if (pub_err != ok) return release(instance, pub_err);
    return ok;
}

fn release(instance: u8, err: u16) u16 {
    _ = ra8_sdhi_deinit(instance);
    return err;
}

/// Best effort: a failed 4-bit switch stays on the 1-bit bus.
fn negotiateWidth(instance: u8, width: u8, rca: u16) void {
    if (width != bus_width_4bit) return;
    const we = ra8_sdhi_set_bus_width_4bit(instance, rca);
    if (we != ok) common.ra8_log_emit_info_val(tag, "4-bit negotiation failed; staying 1-bit", we);
}

export fn ra8_sdcard_init(cfg: ?*const Cfg) u16 {
    const c = cfg orelse return nullPtr("cfg");
    if (state.initialized) return common.k_ra8_err_invalid_state;
    var on = Online{};
    const on_err = cardOnline(c.instance, &on);
    if (on_err != ok) return on_err;
    negotiateWidth(c.instance, c.bus_width, on.rca);
    const clk = ra8_sdhi_set_clock(c.instance, sd.default_clk_div);
    if (logFailed(clk, "set_clock")) return clk;
    state = .{
        .capacity_blocks = on.blocks,
        .rca = on.rca,
        .kind = sd.classify(on.high_capacity, on.blocks),
        .instance = c.instance,
        .initialized = true,
    };
    common.ra8_log_emit_info_val(tag, "sdcard init blocks", on.blocks);
    return ok;
}

fn checkRange(lba: u32, count: u32) u16 {
    if (count == 0) return common.k_ra8_err_invalid_arg;
    if (!state.initialized) return common.k_ra8_err_invalid_state;
    if (lba +% count > state.capacity_blocks) return common.k_ra8_err_out_of_range;
    return ok;
}

export fn ra8_sdcard_read_blocks(lba: u32, buf: ?[*]u8, count: u32) u16 {
    const b = buf orelse return nullPtr("buf");
    const err = checkRange(lba, count);
    if (err != ok) return err;
    return ra8_sdhi_read_block(state.instance, sd.cardAddress(state.kind, lba), b, count);
}

export fn ra8_sdcard_write_blocks(lba: u32, buf: ?[*]const u8, count: u32) u16 {
    const b = buf orelse return nullPtr("buf");
    const err = checkRange(lba, count);
    if (err != ok) return err;
    return ra8_sdhi_write_block(state.instance, sd.cardAddress(state.kind, lba), b, count);
}

export fn ra8_sdcard_get_capacity(out_blocks: ?*u32) u16 {
    const out = out_blocks orelse return nullPtr("out_blocks");
    if (!state.initialized) return common.k_ra8_err_invalid_state;
    out.* = state.capacity_blocks;
    return ok;
}

export fn ra8_sdcard_get_type(out_type: ?*u8) u16 {
    const out = out_type orelse return nullPtr("out_type");
    if (!state.initialized) return common.k_ra8_err_invalid_state;
    out.* = state.kind;
    return ok;
}

/// State clears before the SDHI release; a release failure (MSTP timeout)
/// reports invalid_state, as the C did.
export fn ra8_sdcard_deinit() u16 {
    if (!state.initialized) return ok;
    const inst = state.instance;
    state = .{};
    if (ra8_sdhi_deinit(inst) != ok) return common.k_ra8_err_invalid_state;
    return ok;
}
