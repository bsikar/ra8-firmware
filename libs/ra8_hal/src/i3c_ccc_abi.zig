//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for I3C dynamic address assignment and CCC send/receive
//! (RA8FW-822). The descriptor words and FIFO packing are in
//! internal/i3c_ccc.zig.

const common = @import("abi_common.zig");
const ccc_ = @import("internal/i3c_ccc.zig");
const ctl = @import("internal/i3c_ctl.zig");

const tag = "I3C";
const Target = ccc_.Target;

comptime {
    if (@sizeOf(Target) != 9) @compileError("ra8_i3c_daa_target_t is 9 bytes");
}

const Mmio = struct {
    pub fn read32(_: Mmio, off: usize) u32 {
        const p: *volatile u32 = @ptrFromInt(ctl.base + off);
        return p.*;
    }
    pub fn write32(_: Mmio, off: usize, v: u32) void {
        const p: *volatile u32 = @ptrFromInt(ctl.base + off);
        p.* = v;
    }
};

fn nullPtr(msg: [*:0]const u8) u16 {
    common.ra8_log_emit_error(tag, msg);
    return common.k_ra8_err_null_ptr;
}

export fn ra8_i3c_dynamic_address_assign(targets: ?[*]Target, count: u8) u16 {
    const t = targets orelse return nullPtr("targets must not be nullptr");
    if (count == 0 or count > ccc_.max_targets) return common.k_ra8_err_invalid_arg;
    ccc_.daa(Mmio{}, t[0..count]);
    return common.k_ra8_ok;
}

export fn ra8_i3c_set_dynamic_address(static_addr: u8, dynamic_addr: u8) u16 {
    if (static_addr > ccc_.addr_mask or dynamic_addr > ccc_.addr_mask) return common.k_ra8_err_invalid_arg;
    ccc_.setdasa(Mmio{}, static_addr, dynamic_addr);
    return common.k_ra8_ok;
}

export fn ra8_i3c_reset_dynamic_addresses() u16 {
    ccc_.rstdaa(Mmio{});
    return common.k_ra8_ok;
}

export fn ra8_i3c_send_ccc(ccc: u8, target_addr: u8, payload: ?[*]const u8, len: u8) u16 {
    if (target_addr > ccc_.addr_mask) return common.k_ra8_err_invalid_arg;
    if (len > 0 and payload == null) return common.k_ra8_err_null_ptr;
    const bytes: []const u8 = if (payload) |p| p[0..len] else &.{};
    ccc_.send(Mmio{}, ccc, target_addr, bytes);
    return common.k_ra8_ok;
}

export fn ra8_i3c_recv_ccc(ccc: u8, target_addr: u8, buf: ?[*]u8, max_len: u8, got_len: ?*u8) u16 {
    const b = buf orelse return nullPtr("buf must not be nullptr");
    const got = got_len orelse return nullPtr("got_len must not be nullptr");
    if (ccc & ccc_.ccc_direct == 0) return common.k_ra8_err_invalid_arg;
    if (target_addr > ccc_.addr_mask or max_len == 0) return common.k_ra8_err_invalid_arg;
    ccc_.recv(Mmio{}, ccc, target_addr, b[0..max_len]);
    got.* = max_len;
    return common.k_ra8_ok;
}
