//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Layer-3 switch driver state (RA8FW-535, ported from ra8_layer3_switch.c).
//! Open/close bookkeeping only: route programming is not supported yet, so
//! route add/delete on an open switch answer not_supported, as before.

/// `ra8_layer3_switch_cfg_t` (inc/ra8_layer3_switch.h).
pub const Cfg = extern struct {
    port_count: u8,
    mtu_bytes: u16,
    promiscuous: u8,
};

/// `ra8_layer3_switch_route_t` (inc/ra8_layer3_switch.h).
pub const Route = extern struct {
    dst_ip: u32,
    mask: u32,
    egress_port: u8,
};

pub const Error = error{ InvalidArg, Exists, NotInitialized, NotSupported, InvalidState };

pub const State = struct {
    opened: bool = false,
    promiscuous: bool = false,

    /// Validate `cfg` (port count, then MTU), then refuse a second open.
    pub fn open(self: *State, cfg: Cfg) Error!void {
        if (cfg.port_count == 0) return error.InvalidArg;
        if (cfg.mtu_bytes == 0) return error.InvalidArg;
        if (self.opened) return error.Exists;
        self.opened = true;
        self.promiscuous = cfg.promiscuous != 0;
    }

    /// Route add and delete share one rule: closed is not_initialized, open
    /// is not_supported.
    pub fn route(self: *const State) Error!void {
        if (!self.opened) return error.NotInitialized;
        return error.NotSupported;
    }

    pub fn close(self: *State) Error!void {
        if (!self.opened) return error.InvalidState;
        self.* = .{};
    }
};
