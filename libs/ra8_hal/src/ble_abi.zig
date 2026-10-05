//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_ble.h and ra8_ble_internal.h (RA8FW-759), replacing
//! ra8_ble.c. Framing and dispatch are in internal/ble.zig; this file owns
//! the one transport state, the callbacks and the log lines.

const common = @import("abi_common.zig");
const ble = @import("internal/ble.zig");

const tag = "BLE";
const ok = common.k_ra8_ok;

const EventFn = *const fn (ctx: ?*anyopaque, code: u8, params: [*]const u8, len: u8) callconv(.C) void;
const AclFn = *const fn (ctx: ?*anyopaque, handle: u16, payload: [*]const u8, len: u16) callconv(.C) void;

var state: ble.State = .{};
var evt_fn: ?EventFn = null;
var evt_ctx: ?*anyopaque = null;
var acl_fn: ?AclFn = null;
var acl_ctx: ?*anyopaque = null;

const Sink = struct {
    pub fn event(_: Sink, code: u8, params: []const u8) void {
        if (evt_fn) |f| f(evt_ctx, code, params.ptr, @intCast(params.len));
    }
    pub fn acl(_: Sink, handle: u16, payload: []const u8) void {
        if (acl_fn) |f| f(acl_ctx, handle, payload.ptr, @intCast(payload.len));
    }
};

fn errOf(s: ble.Status) u16 {
    return switch (s) {
        .ok => ok,
        .invalid_arg => common.k_ra8_err_invalid_arg,
        .not_supported => common.k_ra8_err_not_supported,
    };
}

/// RA8_CHECK_NULL_PTR: log the message, return null_ptr.
fn nullPtr(msg: [*:0]const u8) u16 {
    common.ra8_log_emit_error(tag, msg);
    return common.k_ra8_err_null_ptr;
}

fn bytes(p: ?[*]const u8, len: usize) []const u8 {
    return if (p) |q| q[0..len] else &.{};
}

export fn ra8_ble_open(cfg: ?*const ble.Config) u16 {
    const c = cfg orelse return nullPtr("cfg must not be NULL");
    const s = ble.cfgCheck(c.*);
    if (s != .ok) return errOf(s);
    if (state.open) return common.k_ra8_err_invalid_arg;
    state.open = true;
    state.reset();
    common.ra8_log_emit_info(tag, "BLE HCI transport open");
    return ok;
}

export fn ra8_ble_close() u16 {
    if (!state.open) return common.k_ra8_err_invalid_arg;
    state.open = false;
    common.ra8_log_emit_info(tag, "BLE HCI transport closed");
    return ok;
}

export fn ra8_ble_hci_send_command(opcode: u16, params: ?[*]const u8, params_len: u8) u16 {
    if (!state.open) return common.k_ra8_err_not_initialized;
    if (params == null and params_len > 0) return common.k_ra8_err_null_ptr;
    state.sendCommand(opcode, bytes(params, params_len));
    return ok;
}

export fn ra8_ble_hci_send_acl_data(handle: u16, payload: ?[*]const u8, len: u16) u16 {
    if (!state.open) return common.k_ra8_err_not_initialized;
    if (payload == null and len > 0) return common.k_ra8_err_null_ptr;
    if (len > ble.max_acl_payload) return common.k_ra8_err_invalid_arg;
    state.sendAcl(handle, bytes(payload, len));
    return ok;
}

export fn ra8_ble_attach_event_handler(f: ?EventFn, ctx: ?*anyopaque) u16 {
    evt_fn = f;
    evt_ctx = ctx;
    return ok;
}

export fn ra8_ble_attach_acl_handler(f: ?AclFn, ctx: ?*anyopaque) u16 {
    acl_fn = f;
    acl_ctx = ctx;
    return ok;
}

export fn ra8_ble_dispatch() u16 {
    if (!state.open) return common.k_ra8_err_not_initialized;
    return errOf(state.dispatch(Sink{}));
}

export fn ra8_ble_set_random_address(addr: ?[*]const u8) u16 {
    const a = addr orelse return nullPtr("addr must not be NULL");
    return ra8_ble_hci_send_command(ble.op_random_address, a, ble.addr_bytes);
}

export fn ra8_ble_set_advertising_data(data: ?[*]const u8, len: u8) u16 {
    if (len > ble.adv_data_max) return common.k_ra8_err_invalid_arg;
    if (data == null and len > 0) return common.k_ra8_err_null_ptr;
    var buf = [_]u8{0} ** (1 + ble.adv_data_max);
    buf[0] = len;
    @memcpy(buf[1 .. 1 + len], bytes(data, len));
    return ra8_ble_hci_send_command(ble.op_adv_data, &buf, 1 + len);
}

export fn ra8_ble_set_advertising_enable(enable: u8) u16 {
    const param = [1]u8{@intFromBool(enable != 0)};
    return ra8_ble_hci_send_command(ble.op_adv_enable, &param, 1);
}

export fn ra8_ble_scan_start(active: u8, interval: u16, window: u16) u16 {
    if (!ble.scanWindowOk(interval, window)) return common.k_ra8_err_invalid_arg;
    if (!state.open) return common.k_ra8_err_not_initialized;
    const params = ble.scanParams(active, interval, window);
    const err = ra8_ble_hci_send_command(ble.op_scan_params, &params, params.len);
    if (err != ok) return err;
    const enable = [2]u8{ 1, 0 };
    return ra8_ble_hci_send_command(ble.op_scan_enable, &enable, enable.len);
}

/// RA8_TEST_HELPER in the C: exported for the host C tests.
export fn ra8_ble_test_reset_capture() void {
    state.reset();
}

/// RA8_TEST_HELPER: NULL or empty leaves the cursor alone.
export fn ra8_ble_test_inject_rx(data: ?[*]const u8, len: u16) void {
    const d = data orelse return;
    state.inject(d[0..len]);
}

/// RA8_TEST_HELPER: the capture buffer, with its length when asked.
export fn ra8_ble_test_tx_capture(out_len: ?*u16) [*]const u8 {
    if (out_len) |p| p.* = state.tx_len;
    return &state.tx;
}
