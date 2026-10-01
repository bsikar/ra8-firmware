//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The constants the serial endpoint's TLV envelope is written in: the two tag
//! types, the offsets inside one tag header, and the two RPC endpoint names
//! the co-processor registers.

/// The two endpoint names upstream registers on the serial interface.
///
/// A response arrives on `rsp` and an unsolicited event on `evt`. Upstream
/// requires every registered endpoint name to be the same length and its own
/// parser checks that, so the receive path accepts either without a second
/// offset calculation.
pub const Ep = struct {
    pub const rsp: []const u8 = "RPCRsp";
    pub const evt: []const u8 = "RPCEvt";

    /// Shared length of both names, the one the envelope is framed around.
    pub const len: u16 = rsp.len;

    comptime {
        if (rsp.len != evt.len) {
            @compileError("esp-hosted requires every RPC endpoint name to have one length");
        }
    }
};

/// One tag is a type octet then a little-endian 16-bit length, then the value.
pub const Tag = struct {
    /// Tag introducing the endpoint name.
    pub const epname: u8 = 0x01;
    /// Tag introducing the protobuf payload.
    pub const data: u8 = 0x02;

    /// Offset of a tag's type byte.
    pub const type_at: u16 = 0;
    /// Offset of a tag's length, low byte.
    pub const len_lo: u16 = 1;
    /// Offset of a tag's length, high byte.
    pub const len_hi: u16 = 2;
    /// Offset of a tag's value, and so the size of one tag header.
    pub const value: u16 = 3;
};

/// The envelope the two tags make up together.
pub const Envelope = struct {
    /// Offset of the data tag's type byte, just past the endpoint name.
    pub const data_tag: u16 = Tag.value + Ep.len;
    /// Exact envelope cost: two tag headers and the endpoint name between them.
    pub const overhead: u16 = Tag.value + Ep.len + Tag.value;
};
