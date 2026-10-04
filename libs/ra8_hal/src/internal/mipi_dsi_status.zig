//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! MIPI DSI-2 status and IRQ-enable surface (RA8FW-648): ISR/LINKSR reads,
//! status clears, ack/error report, receive-result slots, payload read,
//! tearing-effect events and per-source interrupt enables, ported from
//! ra8_mipi_dsi_dispatch.c. Registers and logging go through a `dsi` ops
//! value (offsets from the DSI base) so host tests can use a fake.

pub const ok: u16 = 0;
pub const invalid_arg: u16 = 0x103;
pub const no_data: u16 = 0x10A;
pub const null_ptr: u16 = 0x504;

pub const off_isr: u16 = 0x000;
pub const off_linksr: u16 = 0x010;
pub const off_rxsr: u16 = 0x200;
pub const off_rxscr: u16 = 0x204;
pub const off_rxier: u16 = 0x208;
pub const off_akepacmsr: u16 = 0x224;
pub const off_akepscr: u16 = 0x228;
pub const off_rxrssr: u16 = 0x230;
pub const off_rxrsscr: u16 = 0x234;
pub const off_rxrinfoowscr: u16 = 0x23C;
pub const off_rxrss0r: u16 = 0x240;
pub const off_rxppd0r: u16 = 0x2C0;
pub const off_ferrscr: u16 = 0x304;
pub const off_ferrier: u16 = 0x308;
pub const off_plscr: u16 = 0x324;
pub const off_plier: u16 = 0x328;
pub const off_vmscr: u16 = 0x414;
pub const off_vmier: u16 = 0x418;
pub const off_sqch0scr: u16 = 0x5D4;
pub const off_sqch0ier: u16 = 0x5D8;
pub const off_sqch1scr: u16 = 0x614;
pub const off_sqch1ier: u16 = 0x618;

pub const isr_sq0: u32 = 1 << 0;
pub const isr_sq1: u32 = 1 << 4;
pub const isr_vm: u32 = 1 << 8;
pub const isr_rcv: u32 = 1 << 12;
pub const isr_ferr: u32 = 1 << 16;
pub const isr_ppi: u32 = 1 << 20;

pub const sqch_clear_all: u32 = 0x7D09_0110;
pub const vmsr_clear_all: u32 = 0x00D0_0111;
pub const rxsr_clear_all: u32 = 0x57F7_E507;
pub const ferrsr_clear_all: u32 = 0x001F_0007;
pub const plsr_clear_all: u32 = 0x3F00_3000;

pub const rxsr_te_mask: u32 = (1 << 13) | (1 << 15);
pub const rx_slots: u8 = 4;
pub const payload_max: u16 = 16;

/// `ra8_mipi_dsi_event_t` values (1-byte enum).
pub const event_seq0: u8 = 0;
pub const event_seq1: u8 = 1;
pub const event_video: u8 = 2;
pub const event_receive: u8 = 3;
pub const event_fatal: u8 = 4;
pub const event_phy: u8 = 5;

/// Mirrors `ra8_mipi_dsi_link_status_t` (5 B).
pub const LinkStatus = extern struct {
    sequence_ch0_running: bool,
    sequence_ch1_running: bool,
    video_running: bool,
    hs_busy: bool,
    lp_busy: bool,
};

/// Mirrors `ra8_mipi_dsi_ack_error_t` (4 B, vc at offset 2).
pub const AckError = extern struct {
    error_report: u16,
    virtual_channel: u8,
};

/// Mirrors `ra8_mipi_dsi_rx_result_t` (12 B).
pub const RxResult = extern struct {
    data: [2]u8,
    cmd_id: u8,
    virtual_channel: u8,
    long_packet: bool,
    rx_success: bool,
    rx_fatal_error: bool,
    rx_fail: bool,
    rx_packet_data_fail: bool,
    rx_correctable_error: bool,
    rx_ack_and_error: bool,
    info_overwritten: bool,
};

fn bit(v: u32, comptime n: u5) bool {
    return v & (@as(u32, 1) << n) != 0;
}

pub fn getStatus(dsi: anytype, out: ?*u32) u16 {
    const o = out orelse {
        dsi.err("out_mask must not be nullptr");
        return null_ptr;
    };
    o.* = dsi.read32(off_isr);
    return ok;
}

pub fn linkStatusGet(dsi: anytype, out: ?*LinkStatus) u16 {
    const o = out orelse {
        dsi.err("out_status must not be nullptr");
        return null_ptr;
    };
    const v = dsi.read32(off_linksr);
    o.* = .{
        .sequence_ch0_running = bit(v, 0),
        .sequence_ch1_running = bit(v, 4),
        .video_running = bit(v, 8),
        .hs_busy = bit(v, 12),
        .lp_busy = bit(v, 13),
    };
    return ok;
}

const ClearEntry = struct { isr: u32, off: u16, value: u32 };
const clear_table = [_]ClearEntry{
    .{ .isr = isr_sq0, .off = off_sqch0scr, .value = sqch_clear_all },
    .{ .isr = isr_sq1, .off = off_sqch1scr, .value = sqch_clear_all },
    .{ .isr = isr_vm, .off = off_vmscr, .value = vmsr_clear_all },
    .{ .isr = isr_rcv, .off = off_rxscr, .value = rxsr_clear_all },
    .{ .isr = isr_ferr, .off = off_ferrscr, .value = ferrsr_clear_all },
    .{ .isr = isr_ppi, .off = off_plscr, .value = plsr_clear_all },
};

pub fn clearStatus(dsi: anytype, mask: u32) u16 {
    for (clear_table) |e| {
        if (mask & e.isr != 0) dsi.write32(e.off, e.value);
    }
    return ok;
}

pub fn ackErrorGet(dsi: anytype, out: ?*AckError) u16 {
    const o = out orelse {
        dsi.err("out_err must not be nullptr");
        return null_ptr;
    };
    const v = dsi.read32(off_akepacmsr);
    o.error_report = @truncate(v);
    o.virtual_channel = @truncate(((v & 0x000F_0000) >> 16) & 0x3);
    dsi.write32(off_akepscr, v);
    return ok;
}

pub fn decodeRx(raw: u32) RxResult {
    return .{
        .data = .{ @truncate(raw), @truncate(raw >> 8) },
        .cmd_id = @truncate((raw & 0x003F_0000) >> 16),
        .virtual_channel = @truncate((raw & 0x00C0_0000) >> 22),
        .long_packet = bit(raw, 24),
        .rx_success = bit(raw, 25),
        .rx_fatal_error = bit(raw, 26),
        .rx_fail = bit(raw, 27),
        .rx_packet_data_fail = bit(raw, 28),
        .rx_correctable_error = bit(raw, 29),
        .rx_ack_and_error = bit(raw, 30),
        .info_overwritten = bit(raw, 31),
    };
}

pub fn rxResultGet(dsi: anytype, slot: u8, out: ?*RxResult) u16 {
    const o = out orelse {
        dsi.err("out_result must not be nullptr");
        return null_ptr;
    };
    if (slot >= rx_slots) return invalid_arg;
    const valid_bit = @as(u32, 1) << @as(u5, @intCast(slot));
    if (dsi.read32(off_rxrssr) & valid_bit == 0) return no_data;
    const raw = dsi.read32(off_rxrss0r + @as(u16, slot) * 4);
    o.* = decodeRx(raw);
    dsi.write32(off_rxrsscr, valid_bit);
    dsi.write32(off_rxrinfoowscr, valid_bit);
    return ok;
}

pub fn rxPayloadRead(dsi: anytype, dest: ?[*]u8, max_len: u16, out_len: ?*u16) u16 {
    const d = dest orelse {
        dsi.err("dest must not be nullptr");
        return null_ptr;
    };
    const n = out_len orelse {
        dsi.err("out_len must not be nullptr");
        return null_ptr;
    };
    var words: [4]u32 = undefined;
    for (&words, 0..) |*w, i| w.* = dsi.read32(off_rxppd0r + @as(u16, @intCast(i)) * 4);
    const eff = @min(max_len, payload_max);
    var i: u16 = 0;
    while (i < eff) : (i += 1) {
        const shift: u5 = @intCast((i % 4) * 8);
        d[i] = @truncate(words[i / 4] >> shift);
    }
    n.* = eff;
    return ok;
}

pub fn teEventPending(dsi: anytype, out: ?*bool) u16 {
    const o = out orelse {
        dsi.err("out_pending must not be nullptr");
        return null_ptr;
    };
    o.* = dsi.read32(off_rxsr) & rxsr_te_mask != 0;
    return ok;
}

pub fn teEventClear(dsi: anytype) u16 {
    dsi.write32(off_rxscr, rxsr_te_mask);
    return ok;
}

fn ierOffset(event: u8) ?u16 {
    return switch (event) {
        event_seq0 => off_sqch0ier,
        event_seq1 => off_sqch1ier,
        event_video => off_vmier,
        event_receive => off_rxier,
        event_fatal => off_ferrier,
        event_phy => off_plier,
        else => null,
    };
}

pub fn irqEnable(dsi: anytype, event: u8, mask: u32, enable: bool) u16 {
    const off = ierOffset(event) orelse return invalid_arg;
    const cur = dsi.read32(off);
    dsi.write32(off, if (enable) cur | mask else cur & ~mask);
    return ok;
}
