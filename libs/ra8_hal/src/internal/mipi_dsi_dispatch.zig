//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! MIPI DSI-2 interrupt dispatch (RA8FW-658): the per-class trampolines
//! (sequence channels 0/1, video, receive, fatal, PHY) and the ISR fan-out,
//! ported from ra8_mipi_dsi_dispatch.c. Each trampoline reads its status
//! register, writes the defined bits back to the clear register, then hands
//! the captured mask to `dsi.notify`. Registers go through a `dsi` ops value
//! (offsets from the DSI base) so host tests can use a fake.

const st = @import("mipi_dsi_status.zig");

pub const off_rstcr: u16 = 0x110;
pub const off_ferrsr: u16 = 0x300;
pub const off_plsr: u16 = 0x320;
pub const off_vmsr: u16 = 0x410;
pub const off_sqch0sr: u16 = 0x5D0;
pub const off_sqch1sr: u16 = 0x610;

pub const rstcr_swrst: u32 = 1 << 0;
pub const rxsr_rxresp: u32 = 1 << 8;
pub const rxrinfoow_sl0: u32 = 1 << 0;
pub const vmsr_vbufudf: u32 = 1 << 22;
pub const vmsr_vbufovf: u32 = 1 << 23;
pub const isr_all: u32 = st.isr_sq0 | st.isr_sq1 | st.isr_vm | st.isr_rcv | st.isr_ferr | st.isr_ppi;

/// Pending response buffer armed by ra8_mipi_dsi_rx_payload_register.
pub const PendingRx = struct {
    buffer: *?[*]u8,
    len: *u16,
};

/// Read `sr`, clear its defined bits through `scr`, return the snapshot.
fn ackStatus(dsi: anytype, sr: u16, scr: u16, clear_all: u32) u32 {
    const bits = dsi.read32(sr);
    dsi.write32(scr, bits & clear_all);
    return bits;
}

pub fn seq0(dsi: anytype) void {
    const bits = ackStatus(dsi, off_sqch0sr, st.off_sqch0scr, st.sqch_clear_all);
    dsi.notify(st.event_seq0, bits);
}

pub fn seq1(dsi: anytype) void {
    const bits = ackStatus(dsi, off_sqch1sr, st.off_sqch1scr, st.sqch_clear_all);
    dsi.notify(st.event_seq1, bits);
}

/// Buffer over/underflow gets the FSP-recommended soft reset.
pub fn video(dsi: anytype) void {
    const bits = ackStatus(dsi, off_vmsr, st.off_vmscr, st.vmsr_clear_all);
    if (bits & (vmsr_vbufovf | vmsr_vbufudf) != 0) {
        dsi.write32(off_rstcr, rstcr_swrst);
        dsi.write32(off_rstcr, 0);
    }
    dsi.notify(st.event_video, bits);
}

/// A response packet drains RXPPD into the armed buffer, then disarms it.
pub fn receive(dsi: anytype, rx: PendingRx) void {
    const bits = ackStatus(dsi, st.off_rxsr, st.off_rxscr, st.rxsr_clear_all);
    if (bits & rxsr_rxresp != 0) {
        if (rx.buffer.*) |buf| {
            if (rx.len.* > 0) {
                var got: u16 = 0;
                _ = st.rxPayloadRead(dsi, buf, rx.len.*, &got);
                rx.buffer.* = null;
                rx.len.* = 0;
            }
        }
    }
    dsi.write32(st.off_rxrinfoowscr, rxrinfoow_sl0);
    dsi.notify(st.event_receive, bits);
}

pub fn fatal(dsi: anytype) void {
    const bits = ackStatus(dsi, off_ferrsr, st.off_ferrscr, st.ferrsr_clear_all);
    dsi.notify(st.event_fatal, bits);
}

pub fn phy(dsi: anytype) void {
    const bits = ackStatus(dsi, off_plsr, st.off_plscr, st.plsr_clear_all);
    dsi.notify(st.event_phy, bits);
}

/// Fan out on one ISR snapshot. With no source bit set the callback still
/// fires once as a PHY event with mask 0 (the legacy "always invoke" contract).
pub fn dispatch(dsi: anytype, rx: PendingRx) void {
    const snapshot = dsi.read32(st.off_isr);
    if (snapshot & st.isr_sq0 != 0) seq0(dsi);
    if (snapshot & st.isr_sq1 != 0) seq1(dsi);
    if (snapshot & st.isr_vm != 0) video(dsi);
    if (snapshot & st.isr_rcv != 0) receive(dsi, rx);
    if (snapshot & st.isr_ferr != 0) fatal(dsi);
    if (snapshot & st.isr_ppi != 0) phy(dsi);
    if (snapshot & isr_all == 0) dsi.notify(st.event_phy, 0);
}
