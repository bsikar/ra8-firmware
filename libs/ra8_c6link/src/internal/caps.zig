//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The host-capabilities announcement the co-processor expects first.
//!
//! Pure formatting: no hardware, no link state. Split out so the exact octets
//! can be compared against the protocol in a host test rather than only
//! against the code that wrote them.

/// Layout of the announcement and the five values this host declares.
pub const Caps = struct {
    /// Offset of the event-type octet.
    pub const type_at: u8 = 0;
    /// Offset of the event-length octet.
    pub const len_at: u8 = 1;
    /// Octets before the first TLV.
    pub const hdr: u8 = 2;
    /// TLVs this host emits.
    pub const tags: u8 = 5;
    /// Value width of every tag emitted.
    pub const value_len: u8 = 1;
    /// Octets one one-valued TLV occupies.
    pub const stride: u8 = 3;
    /// Whole frame: two header octets plus five three-octet TLVs.
    pub const bytes: u8 = hdr + (tags * stride);

    /// Host capability word. Zero, as upstream's own host sends.
    pub const host: u8 = 0;
    /// `ESP_PRIV_FIRMWARE_CHIP_ESP32C6`: the part this board carries.
    pub const chip: u8 = 0x0D;
    /// Raw-throughput test direction; disabled, matching `H_TEST_RAW_TP_DIR`.
    pub const raw_tp: u8 = 0;
    /// Flow-control high-water mark, `H_WIFI_TX_DATA_THROTTLE_HIGH_THRESHOLD`.
    pub const throttle_high: u8 = 80;
    /// Flow-control low-water mark, `H_WIFI_TX_DATA_THROTTLE_LOW_THRESHOLD`.
    pub const throttle_low: u8 = 60;

    comptime {
        if (throttle_high <= throttle_low) {
            @compileError("the co-processor requires a high-water mark above the low one");
        }
    }
};

/// Tag identifiers from the vendored `esp_hosted_transport.h`.
pub const Tag = struct {
    /// `ESP_PRIV_EVENT_INIT`, the event type of the whole announcement.
    pub const event_init: u8 = 0x22;

    /// `HOST_CAPABILITIES`, and the four tags the vendored enum continues with.
    pub const host_capabilities: u8 = 0x44;
    pub const chip_id: u8 = host_capabilities + 1;
    pub const test_raw_tp: u8 = host_capabilities + 2;
    pub const throttle_high: u8 = host_capabilities + 3;
    pub const throttle_low: u8 = host_capabilities + 4;
};

/// One one-octet TLV, in the order `send_slave_config()` composes them.
///
/// LEGACY-OK: send_slave_config() is the upstream esp-hosted symbol name
const emitted = [Caps.tags]struct { tag: u8, value: u8 }{
    .{ .tag = Tag.host_capabilities, .value = Caps.host },
    .{ .tag = Tag.chip_id, .value = Caps.chip },
    .{ .tag = Tag.test_raw_tp, .value = Caps.raw_tp },
    .{ .tag = Tag.throttle_high, .value = Caps.throttle_high },
    .{ .tag = Tag.throttle_low, .value = Caps.throttle_low },
};

/// Write the announcement into `out`, reporting its length.
///
/// Returns null when `out` is shorter than the frame, leaving it untouched.
pub fn write(out: []u8) ?u8 {
    if (out.len < Caps.bytes) return null;

    out[Caps.type_at] = Tag.event_init;
    out[Caps.len_at] = Caps.tags * Caps.stride;

    var at: u8 = Caps.hdr;
    for (emitted) |tlv| {
        out[at] = tlv.tag;
        out[at + 1] = Caps.value_len;
        out[at + 2] = tlv.value;
        at += Caps.stride;
    }
    return at;
}
