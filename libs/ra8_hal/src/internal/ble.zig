//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! BLE HCI transport framing (RA8FW-759), moved out of ra8_ble.c: the TX
//! capture, the RX inject cursor, command and ACL framing, the
//! LE_Set_Scan_Parameters packet and the RX dispatch loop. Packet layout is
//! Bluetooth Core v5.4 Vol 4 Part A section 2 (UART transport indicators).

pub const capture_bytes: u16 = 1024;
pub const max_evt_params: u16 = 255;
pub const max_acl_payload: u16 = 251;
pub const addr_bytes: u8 = 6;
pub const adv_data_max: u8 = 31;
pub const scan_min: u16 = 0x0004;
pub const scan_max: u16 = 0x4000;
pub const dispatch_budget: u32 = 64;

pub const pkt_cmd: u8 = 0x01;
pub const pkt_acl: u8 = 0x02;
pub const pkt_event: u8 = 0x04;

/// Core v5.4 Vol 4 Part E 7.8.4, 7.8.7, 7.8.9, 7.8.10 and 7.8.11.
pub const op_random_address: u16 = 0x2005;
pub const op_adv_data: u16 = 0x2008;
pub const op_adv_enable: u16 = 0x200A;
pub const op_scan_params: u16 = 0x200B;
pub const op_scan_enable: u16 = 0x200C;

pub const Status = enum { ok, invalid_arg, not_supported };

/// ra8_ble_config_t: two 0/1 flags, both must be 0 (the C6 owns them).
pub const Config = extern struct {
    use_external_osc: u8,
    deep_sleep_enable: u8,
};

comptime {
    if (@sizeOf(Config) != 2) @compileError("ra8_ble_config_t layout");
}

pub fn cfgCheck(cfg: Config) Status {
    if (cfg.use_external_osc > 1 or cfg.deep_sleep_enable > 1) return .invalid_arg;
    if (cfg.use_external_osc != 0 or cfg.deep_sleep_enable != 0) return .not_supported;
    return .ok;
}

pub fn scanWindowOk(interval: u16, window: u16) bool {
    if (interval < scan_min or interval > scan_max) return false;
    if (window < scan_min or window > scan_max) return false;
    return window <= interval;
}

/// Type, interval LE16, window LE16, public own address, basic filter.
pub fn scanParams(active: u8, interval: u16, window: u16) [7]u8 {
    return .{
        @intFromBool(active != 0),
        @truncate(interval),
        @truncate(interval >> 8),
        @truncate(window),
        @truncate(window >> 8),
        0,
        0,
    };
}

pub const State = struct {
    open: bool = false,
    tx: [capture_bytes]u8 = @splat(0),
    tx_len: u16 = 0,
    rx: [capture_bytes]u8 = @splat(0),
    rx_len: u16 = 0,
    rx_pos: u16 = 0,
    evt: [max_evt_params]u8 = @splat(0),
    acl: [max_acl_payload]u8 = @splat(0),

    pub fn reset(self: *State) void {
        self.tx_len = 0;
        self.rx_len = 0;
        self.rx_pos = 0;
    }

    /// Past the capture size the byte is dropped, as in the C.
    pub fn txByte(self: *State, b: u8) void {
        if (self.tx_len >= capture_bytes) return;
        self.tx[self.tx_len] = b;
        self.tx_len += 1;
    }

    pub fn rxByte(self: *State) ?u8 {
        if (self.rx_pos >= self.rx_len) return null;
        const b = self.rx[self.rx_pos];
        self.rx_pos += 1;
        return b;
    }

    /// Clamped to the inject buffer; an empty slice leaves the state alone.
    pub fn inject(self: *State, bytes: []const u8) void {
        if (bytes.len == 0) return;
        const n: u16 = @intCast(@min(bytes.len, capture_bytes));
        @memcpy(self.rx[0..n], bytes[0..n]);
        self.rx_len = n;
        self.rx_pos = 0;
    }

    pub fn sendCommand(self: *State, opcode: u16, params: []const u8) void {
        self.txByte(pkt_cmd);
        self.txLe16(opcode);
        self.txByte(@intCast(params.len));
        for (params) |b| self.txByte(b);
    }

    pub fn sendAcl(self: *State, handle: u16, payload: []const u8) void {
        self.txByte(pkt_acl);
        self.txLe16(handle);
        self.txLe16(@intCast(payload.len));
        for (payload) |b| self.txByte(b);
    }

    fn txLe16(self: *State, v: u16) void {
        self.txByte(@truncate(v));
        self.txByte(@truncate(v >> 8));
    }

    fn rxLe16(self: *State) ?u16 {
        const lo = self.rxByte() orelse return null;
        const hi = self.rxByte() orelse return null;
        return (@as(u16, hi) << 8) | lo;
    }

    /// Drains up to 64 packets; sink has event(code, params) and
    /// acl(handle, payload). Truncated or unknown packets are invalid_arg.
    pub fn dispatch(self: *State, sink: anytype) Status {
        var budget: u32 = 0;
        while (budget < dispatch_budget) : (budget += 1) {
            const kind = self.rxByte() orelse return .ok;
            const s = switch (kind) {
                pkt_event => self.dispatchEvent(sink),
                pkt_acl => self.dispatchAcl(sink),
                else => Status.invalid_arg,
            };
            if (s != .ok) return s;
        }
        return .ok;
    }

    fn dispatchEvent(self: *State, sink: anytype) Status {
        const code = self.rxByte() orelse return .invalid_arg;
        const plen = self.rxByte() orelse return .invalid_arg;
        for (self.evt[0..plen]) |*b| b.* = self.rxByte() orelse return .invalid_arg;
        sink.event(code, self.evt[0..plen]);
        return .ok;
    }

    fn dispatchAcl(self: *State, sink: anytype) Status {
        const handle = self.rxLe16() orelse return .invalid_arg;
        const len = self.rxLe16() orelse return .invalid_arg;
        if (len > max_acl_payload) return .invalid_arg;
        for (self.acl[0..len]) |*b| b.* = self.rxByte() orelse return .invalid_arg;
        sink.acl(handle, self.acl[0..len]);
        return .ok;
    }
};
