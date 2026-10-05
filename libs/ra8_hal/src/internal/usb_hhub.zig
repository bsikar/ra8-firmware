//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! USB host hub class driver, ported from ra8_usb_hhub.c (RA8FW-771):
//! request envelopes and the polled 7-step enumeration machine. The
//! machine runs against any `host` value with busReset, setAddress and
//! setup methods, so tests use a recording fake. USB 2.0 Ch 9 and Ch 11.

/// Mirror of ra8_usb_setup_t.
pub const Setup = extern struct {
    bm_request_type: u8,
    b_request: u8,
    w_value: u16,
    w_index: u16,
    w_length: u16,
};

/// Mirror of ra8_usb_hhub_device_t.
pub const Device = extern struct {
    device_address: u8 = 0,
    port_count: u8 = 0,
    vendor_id: u16 = 0,
    product_id: u16 = 0,
};

comptime {
    if (@sizeOf(Setup) != 8) @compileError("ra8_usb_setup_t is 8 bytes");
    if (@sizeOf(Device) != 6) @compileError("ra8_usb_hhub_device_t is 6 bytes");
}

pub const AttachFn = *const fn (?*anyopaque, *const Device) callconv(.c) void;

pub const Step = enum(u8) { idle, bus_reset, set_address, get_dev_desc, get_cfg_desc, set_config, get_hub_desc, done };

pub const ok: u16 = 0;
pub const assigned_address: u8 = 1;
pub const default_config: u8 = 1;
pub const default_ports: u8 = 4;
pub const first_port: u8 = 1;
const desc_device: u8 = 0x01;
const desc_configuration: u8 = 0x02;
const desc_hub: u8 = 0x29;

pub fn getDescriptor(desc_type: u8, length: u16) Setup {
    return .{ .bm_request_type = 0x80, .b_request = 0x06, .w_value = @as(u16, desc_type) << 8, .w_index = 0, .w_length = length };
}

pub fn setAddress(address: u8) Setup {
    return .{ .bm_request_type = 0x00, .b_request = 0x05, .w_value = address, .w_index = 0, .w_length = 0 };
}

pub fn setConfig(value: u8) Setup {
    return .{ .bm_request_type = 0x00, .b_request = 0x09, .w_value = value, .w_index = 0, .w_length = 0 };
}

pub fn getHubDescriptor() Setup {
    return .{ .bm_request_type = 0xA0, .b_request = 0x06, .w_value = @as(u16, desc_hub) << 8, .w_index = 0, .w_length = 9 };
}

pub fn portStatus(port: u8) Setup {
    return .{ .bm_request_type = 0xA3, .b_request = 0x00, .w_value = 0, .w_index = port, .w_length = 4 };
}

pub fn portFeature(port: u8, feature: u16, set: bool) Setup {
    return .{ .bm_request_type = 0x23, .b_request = if (set) 0x03 else 0x01, .w_value = feature, .w_index = port, .w_length = 0 };
}

pub const Hub = struct {
    initialized: bool = false,
    attached: bool = false,
    speed: u8 = 0,
    step: Step = .idle,
    attach_cb: ?AttachFn = null,
    attach_ctx: ?*anyopaque = null,
    device: Device = .{},

    pub fn portOk(self: *const Hub, port: u8) bool {
        return port >= first_port and port <= self.device.port_count;
    }

    /// Run the current step and move to the next one; returns its status.
    pub fn advance(self: *Hub, host: anytype) u16 {
        switch (self.step) {
            .idle => {
                self.step = .bus_reset;
                return host.busReset(self.speed, true);
            },
            .bus_reset => {
                _ = host.busReset(self.speed, false);
                self.step = .set_address;
                return host.setup(self.speed, setAddress(assigned_address));
            },
            .set_address => {
                _ = host.setAddress(self.speed, assigned_address);
                self.step = .get_dev_desc;
                return host.setup(self.speed, getDescriptor(desc_device, 18));
            },
            .get_dev_desc => {
                self.step = .get_cfg_desc;
                return host.setup(self.speed, getDescriptor(desc_configuration, 9));
            },
            .get_cfg_desc => {
                self.step = .set_config;
                return host.setup(self.speed, setConfig(default_config));
            },
            .set_config => {
                self.step = .get_hub_desc;
                return host.setup(self.speed, getHubDescriptor());
            },
            .get_hub_desc => {
                self.publish();
                return ok;
            },
            .done => return ok,
        }
    }

    fn publish(self: *Hub) void {
        self.device = .{ .device_address = assigned_address, .port_count = default_ports };
        self.attached = true;
        self.step = .done;
        if (self.attach_cb) |cb| cb(self.attach_ctx, &self.device);
    }
};
