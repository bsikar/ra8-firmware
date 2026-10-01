//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The dispatch behind `ra8_usb_device_compose` (RA8FW-317): one class entry plus
//! a device identity become the three framework buffers an enumerating stack
//! hands to the host. Every byte still comes out of `descriptor.zig`; this
//! file adds only the choice of encoder and the order the three encodes run
//! in, so a composed framework set is byte-identical to the three calls it
//! replaces.

const std = @import("std");
const descriptor = @import("descriptor.zig");

/// The encoders this dispatch sits on, re-exported so a caller reaches the
/// device shapes and the encoder errors through one import.
pub const desc = descriptor;

/// What a composition may carry. The encoders underneath model exactly one
/// function, so a second entry has nowhere to go.
pub const Limits = struct {
    pub const classes_max: u8 = 1;
};

pub const Error = error{
    /// No class entry, or a kind the encoders do not name.
    InvalidArg,
    /// More entries than `Limits.classes_max`.
    NotSupported,
} || descriptor.Error;

/// The one function a composed device exposes. A tagged union rather than a
/// kind byte beside a bare union, so an unset or unknown kind cannot reach
/// the encoders at all: it is refused on the way in.
pub const Class = union(enum) {
    cdc_acm: descriptor.CdcAcm,
    hid: descriptor.Hid,
    msc: descriptor.Msc,
    dfu: descriptor.Dfu,
};

/// Caller-owned destinations, one slice each. Capacity is the slice length.
pub const Buffers = struct {
    device: []u8,
    strings: []u8,
    langid: []u8,
};

/// Bytes written into each buffer. Fields are published as each encode
/// succeeds, so a later refusal leaves the earlier lengths readable.
pub const Lengths = struct {
    device: usize = 0,
    strings: usize = 0,
    langid: usize = 0,
};

/// Refuse a class count the encoders cannot serve.
pub fn checkCount(count: u8) Error!void {
    if (count == 0) return Error.InvalidArg;
    if (count > Limits.classes_max) return Error.NotSupported;
}

/// Encode the device framework for one class entry.
fn encodeClass(dev: descriptor.Device, class: Class, out: []u8) Error!usize {
    return switch (class) {
        .cdc_acm => |port| descriptor.cdcAcm(dev, port, out),
        .hid => |human| descriptor.hid(dev, human, out),
        .msc => |storage| descriptor.msc(dev, storage, out),
        .dfu => |upgrade| descriptor.dfu(dev, upgrade, out),
    };
}

/// Device framework, string table, LANGID table, in that order. The order is
/// the contract: a caller that refuses partway keeps whatever earlier lengths
/// were already published.
pub fn compose(dev: descriptor.Device, class: Class, out: Buffers, len: *Lengths) Error!void {
    len.device = try encodeClass(dev, class, out.device);
    len.strings = try descriptor.strings(dev, out.strings);
    len.langid = try descriptor.langid(dev.langid, out.langid);
}
