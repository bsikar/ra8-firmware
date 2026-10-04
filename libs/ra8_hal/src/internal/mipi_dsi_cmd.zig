//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! MIPI DSI command path (RA8FW-630), ported from ra8_mipi_dsi_cmd.c.
//! Registers and the pending-RX globals (mipi_dsi_lifecycle_abi.zig) are reached
//! through a `dsi` ops value so host tests can use a fake register file.

pub const ok: u16 = 0;
pub const invalid_arg: u16 = 0x103;
pub const invalid_state: u16 = 0x104;
pub const busy: u16 = 0x109;
pub const null_ptr: u16 = 0x504;

pub const off_linksr: u16 = 0x010;
pub const off_txppd0r: u16 = 0x160;
pub const off_sqch0set0r: u16 = 0x5C0;
pub const off_sqch1set0r: u16 = 0x600;
pub const off_sqch0dsc: u16 = 0x780;
pub const off_sqch1dsc: u16 = 0x800;

pub const link_sq0run: u32 = 1 << 0;
pub const link_sq1run: u32 = 1 << 4;
pub const link_vrun: u32 = 1 << 8;
pub const sqch_start: u32 = 1 << 0;
pub const sqch_chsel: u32 = 1 << 23;
pub const dsc_b_dtsel_seqrm: u32 = 1 << 24;
pub const dsc_c_finact: u32 = 1 << 0;
pub const dsc_c_auxop: u32 = 1 << 22;
pub const dsc_c_actcode_mask: u32 = 0xFF << 24;

pub const payload_max: u16 = 16;
pub const max_lp_bytes: u16 = 128;
pub const max_hs_bytes: u16 = 1024;
pub const vc_max: u8 = 3;
pub const bta_none: u8 = 0;
pub const bta_read: u8 = 2;

/// Mirrors ra8_mipi_dsi_command_t.
pub const Command = extern struct {
    cmd_id: u8,
    virtual_channel: u8,
    bta: u8,
    low_power: bool,
    ack_request: bool,
    aux_operation: bool,
    action_code: u8,
    tx_len: u16,
    p_tx_buffer: ?[*]const u8,
    p_rx_buffer: ?[*]u8,
};

/// Data types whose low nibble is above 8 are long packets.
pub fn isLong(cmd_id: u8) bool {
    return (cmd_id & 0x0F) > 0x08;
}

pub fn makeDscA(cmd: *const Command) u32 {
    var data0: u32 = 0;
    var data1: u32 = 0;
    const long = isLong(cmd.cmd_id);
    if (long) {
        data0 = cmd.tx_len & 0xFF;
        data1 = (cmd.tx_len >> 8) & 0xFF;
    } else if (cmd.p_tx_buffer) |tx| {
        if (cmd.tx_len > 0) data0 = tx[0];
        if (cmd.tx_len > 1) data1 = tx[1];
    }
    var v: u32 = data0 | (data1 << 8) |
        ((@as(u32, cmd.cmd_id) & 0x3F) << 16) |
        ((@as(u32, cmd.virtual_channel) & 0x3) << 22);
    if (long) v |= 1 << 24;
    if (cmd.low_power) v |= 1 << 25;
    return v | ((@as(u32, cmd.bta) & 0x3) << 26);
}

pub fn makeDscC(cmd: *const Command) u32 {
    var v = dsc_c_finact;
    if (cmd.aux_operation) {
        v |= dsc_c_auxop | ((@as(u32, cmd.action_code) << 24) & dsc_c_actcode_mask);
    }
    return v;
}

pub fn validate(cmd: *const Command) u16 {
    if (cmd.virtual_channel > vc_max) return invalid_arg;
    if (cmd.tx_len > 0 and cmd.p_tx_buffer == null) return null_ptr;
    const cap = if (cmd.low_power) max_lp_bytes else max_hs_bytes;
    if (cmd.tx_len > cap) return invalid_arg;
    return ok;
}

fn checkLinkState(dsi: anytype, cmd: *const Command) u16 {
    const link = dsi.read32(off_linksr);
    if (cmd.low_power and (link & link_vrun) != 0) {
        dsi.errMsg("send_command: LP not allowed during video mode");
        return invalid_state;
    }
    if ((link & (link_sq0run | link_sq1run)) != 0) {
        dsi.errMsg("send_command: sequence busy");
        return busy;
    }
    if (cmd.aux_operation and (link & link_vrun) != 0) return invalid_state;
    return ok;
}

/// Packs up to 16 payload bytes little-endian into TXPPD0R..TXPPD3R.
fn stagePayload(dsi: anytype, data: [*]const u8, len: u16) void {
    var words = [4]u32{ 0, 0, 0, 0 };
    const eff = @min(len, payload_max);
    for (0..eff) |i| words[i / 4] |= @as(u32, data[i]) << @intCast((i % 4) * 8);
    for (words, 0..) |w, i| dsi.write32(off_txppd0r + @as(u16, @intCast(i)) * 4, w);
}

fn bufferAddr(cmd: *const Command) u32 {
    const use_rx = cmd.bta == bta_read or cmd.p_rx_buffer != null;
    const addr: usize = if (use_rx) @intFromPtr(cmd.p_rx_buffer) else @intFromPtr(cmd.p_tx_buffer);
    return @truncate(addr);
}

/// LP goes out on sequence channel 0, HS on channel 1.
fn stageAndPulse(dsi: anytype, cmd: *const Command) void {
    const channel: u8 = if (cmd.low_power) 0 else 1;
    if (isLong(cmd.cmd_id) and cmd.tx_len > 0) stagePayload(dsi, cmd.p_tx_buffer.?, cmd.tx_len);
    const dsc = if (channel == 0) off_sqch0dsc else off_sqch1dsc;
    dsi.write32(dsc, makeDscA(cmd));
    dsi.write32(dsc + 4, dsc_b_dtsel_seqrm);
    dsi.write32(dsc + 8, makeDscC(cmd));
    dsi.write32(dsc + 12, bufferAddr(cmd));
    dsi.write32(off_sqch0set0r, sqch_chsel | (if (channel == 0) sqch_start else 0));
    dsi.write32(off_sqch1set0r, sqch_chsel | (if (channel == 1) sqch_start else 0));
}

pub fn sendCommand(dsi: anytype, maybe_cmd: ?*const Command) u16 {
    const cmd = maybe_cmd orelse {
        dsi.errMsg("cmd must not be nullptr");
        return null_ptr;
    };
    const v_err = validate(cmd);
    if (v_err != ok) {
        dsi.errVal("send_command: validate", v_err);
        return v_err;
    }
    const link_err = checkLinkState(dsi, cmd);
    if (link_err != ok) {
        dsi.errVal("send_command: link state", link_err);
        return link_err;
    }
    stageAndPulse(dsi, cmd);
    if (cmd.p_rx_buffer) |rx| dsi.setPendingRx(rx, payload_max);
    return ok;
}

pub fn sendShortPacket(dsi: anytype, cmd_id: u8, vc: u8, p0: u8, p1: u8) u16 {
    const buf = [2]u8{ p0, p1 };
    const cmd = Command{
        .cmd_id = cmd_id,
        .virtual_channel = vc,
        .bta = bta_none,
        .low_power = true,
        .ack_request = false,
        .aux_operation = false,
        .action_code = 0,
        .tx_len = 2,
        .p_tx_buffer = &buf,
        .p_rx_buffer = null,
    };
    return sendCommand(dsi, &cmd);
}

pub fn sendLongPacket(dsi: anytype, cmd_id: u8, vc: u8, data: ?[*]const u8, tx_len: u16, low_power: bool) u16 {
    if (tx_len > 0 and data == null) return null_ptr;
    const cmd = Command{
        .cmd_id = cmd_id,
        .virtual_channel = vc,
        .bta = bta_none,
        .low_power = low_power,
        .ack_request = false,
        .aux_operation = false,
        .action_code = 0,
        .tx_len = tx_len,
        .p_tx_buffer = data,
        .p_rx_buffer = null,
    };
    return sendCommand(dsi, &cmd);
}

/// Arms the pending-RX sink with the caller's length before the kick.
pub fn readPacket(dsi: anytype, cmd_id: u8, vc: u8, p0: u8, p1: u8, rx: ?[*]u8, rx_len: u16) u16 {
    const sink = rx orelse {
        dsi.errMsg("p_rx_buffer must not be nullptr");
        return null_ptr;
    };
    if (rx_len == 0) return invalid_arg;
    const tx = [2]u8{ p0, p1 };
    const cmd = Command{
        .cmd_id = cmd_id,
        .virtual_channel = vc,
        .bta = bta_read,
        .low_power = true,
        .ack_request = true,
        .aux_operation = false,
        .action_code = 0,
        .tx_len = 2,
        .p_tx_buffer = &tx,
        .p_rx_buffer = sink,
    };
    dsi.setPendingRx(sink, rx_len);
    return sendCommand(dsi, &cmd);
}
