//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI exports for ra8_layer3_switch_* (internal/layer3_switch.zig, RA8FW-535). Built as its own object in
//! libra8_hal.a (RA8FW-542) so an image links only the units it calls.

const common = @import("abi_common.zig");
const layer3_switch = @import("internal/layer3_switch.zig");
const k_ra8_ok = common.k_ra8_ok;
const k_ra8_err_invalid_arg = common.k_ra8_err_invalid_arg;
const k_ra8_err_null_ptr = common.k_ra8_err_null_ptr;
const k_ra8_err_invalid_state = common.k_ra8_err_invalid_state;
const k_ra8_err_not_supported = common.k_ra8_err_not_supported;
const k_ra8_err_exists = common.k_ra8_err_exists;
const k_ra8_err_not_initialized = common.k_ra8_err_not_initialized;
const ra8_log_emit_info_val = common.ra8_log_emit_info_val;
const ra8_log_emit_error = common.ra8_log_emit_error;

const l3sw_tag = "L3SW";
var l3sw_state: layer3_switch.State = .{};

fn l3swStatus(err: layer3_switch.Error) u16 {
    return switch (err) {
        error.InvalidArg => k_ra8_err_invalid_arg,
        error.Exists => k_ra8_err_exists,
        error.NotInitialized => k_ra8_err_not_initialized,
        error.NotSupported => k_ra8_err_not_supported,
        error.InvalidState => k_ra8_err_invalid_state,
    };
}

/// `ra8_err_t ra8_layer3_switch_open(const ra8_layer3_switch_cfg_t* cfg)`.
export fn ra8_layer3_switch_open(cfg: ?*const layer3_switch.Cfg) u16 {
    const config = cfg orelse {
        ra8_log_emit_error(l3sw_tag, "cfg must not be nullptr");
        return k_ra8_err_null_ptr;
    };
    l3sw_state.open(config.*) catch |err| return l3swStatus(err);
    ra8_log_emit_info_val(l3sw_tag, "l3sw open ports", config.port_count);
    return k_ra8_ok;
}

/// `ra8_err_t ra8_layer3_switch_route_add(const ra8_layer3_switch_route_t* route)`.
export fn ra8_layer3_switch_route_add(route: ?*const layer3_switch.Route) u16 {
    if (route == null) {
        ra8_log_emit_error(l3sw_tag, "route must not be nullptr");
        return k_ra8_err_null_ptr;
    }
    l3sw_state.route() catch |err| return l3swStatus(err);
    return k_ra8_ok;
}

/// `ra8_err_t ra8_layer3_switch_route_delete(uint32_t dst_ip, uint32_t mask)`.
export fn ra8_layer3_switch_route_delete(dst_ip: u32, mask: u32) u16 {
    _ = dst_ip;
    _ = mask;
    l3sw_state.route() catch |err| return l3swStatus(err);
    return k_ra8_ok;
}

/// `ra8_err_t ra8_layer3_switch_status_get(uint8_t* out_open, uint8_t* out_promisc)`.
export fn ra8_layer3_switch_status_get(out_open: ?*u8, out_promisc: ?*u8) u16 {
    const open_out = out_open orelse {
        ra8_log_emit_error(l3sw_tag, "out_open must not be nullptr");
        return k_ra8_err_null_ptr;
    };
    const promisc_out = out_promisc orelse {
        ra8_log_emit_error(l3sw_tag, "out_promisc must not be nullptr");
        return k_ra8_err_null_ptr;
    };
    open_out.* = @intFromBool(l3sw_state.opened);
    promisc_out.* = @intFromBool(l3sw_state.promiscuous);
    return k_ra8_ok;
}

/// `ra8_err_t ra8_layer3_switch_close(void)`.
export fn ra8_layer3_switch_close() u16 {
    l3sw_state.close() catch |err| return l3swStatus(err);
    return k_ra8_ok;
}
