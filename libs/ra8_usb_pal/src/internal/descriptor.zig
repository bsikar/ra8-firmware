//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The four device framework builders behind `inc/ra8_usb_desc.h`, plus the
//! string and language-id encoders. Pure: bytes into a caller-owned buffer,
//! no allocation, no USBX types, no global state. Strings arrive as slices;
//! the C membrane in `src/ra8_usb_desc_abi.zig` is where `const char*` ends.

const wire = @import("wire.zig");

pub const Size = wire.Size;
pub const Limits = wire.Limits;

/// Why a framework could not be encoded.
pub const Error = error{
    /// A field the standard does not allow: a wrong endpoint direction, a
    /// zero packet size, a boot protocol on a non-boot interface.
    InvalidArg,
    /// The buffer is too small for the framework it was asked to hold.
    InvalidSize,
    /// A string is longer than a descriptor can carry, or the power draw is
    /// above what a bus-powered configuration may request.
    RangeCheck,
};

/// Identity and strings shared by every framework.
pub const Device = struct {
    vid: u16 = 0,
    pid: u16 = 0,
    bcd_device: u16 = 0,
    manufacturer: []const u8 = "",
    product: []const u8 = "",
    serial: []const u8 = "",
    langid: u16 = 0,
    max_power_ma: u16 = 0,
    self_powered: bool = false,
    remote_wakeup: bool = false,

    /// The language the strings are published in; zero means US English.
    pub fn effectiveLangid(self: Device) u16 {
        return if (self.langid == 0) wire.Usb.langid_en_us else self.langid;
    }

    /// `bcdDevice`; zero means 1.00.
    pub fn effectiveBcdDevice(self: Device) u16 {
        return if (self.bcd_device == 0) wire.Usb.bcd_device_default else self.bcd_device;
    }

    /// The three string slots in index order.
    pub fn slots(self: Device) [3][]const u8 {
        return .{ self.manufacturer, self.product, self.serial };
    }

    /// `bmAttributes` of this device's one configuration.
    pub fn configAttributes(self: Device) u8 {
        var attributes: u8 = wire.ConfigAttr.base;
        if (self.self_powered) attributes |= wire.ConfigAttr.self_powered;
        if (self.remote_wakeup) attributes |= wire.ConfigAttr.remote_wakeup;
        return attributes;
    }

    /// `bMaxPower`, in the standard's 2 mA units, rounded up.
    pub fn maxPowerUnits(self: Device) u8 {
        return @truncate((self.max_power_ma + 1) / 2);
    }

    fn check(self: Device) Error!void {
        for (self.slots()) |text| {
            if (text.len > Limits.string_chars_max) return Error.RangeCheck;
        }
        if (self.max_power_ma > wire.Usb.power_ma_max) return Error.RangeCheck;
    }
};

pub const CdcAcm = struct {
    notify_ep: u8,
    notify_bytes: u16,
    notify_interval_ms: u8,
    out_ep: u8,
    in_ep: u8,
    data_bytes: u16,
    high_speed: bool = false,

    fn check(self: CdcAcm) Error!void {
        if (!isIn(self.notify_ep) or !isIn(self.in_ep) or isIn(self.out_ep)) {
            return Error.InvalidArg;
        }
        if (self.notify_bytes == 0 or self.data_bytes == 0) return Error.InvalidArg;
    }
};

pub const Msc = struct {
    in_ep: u8,
    out_ep: u8,
    data_bytes: u16,
    high_speed: bool = false,

    fn check(self: Msc) Error!void {
        if (!isIn(self.in_ep) or isIn(self.out_ep)) return Error.InvalidArg;
        if (self.data_bytes == 0) return Error.InvalidArg;
    }
};

pub const HidProtocol = enum(u8) { none = 0, keyboard = 1, mouse = 2 };

pub const Hid = struct {
    in_ep: u8,
    data_bytes: u16,
    poll_interval_ms: u8,
    report_bytes: u16,
    boot_interface: bool = false,
    protocol: HidProtocol = .none,

    fn check(self: Hid) Error!void {
        if (!isIn(self.in_ep)) return Error.InvalidArg;
        if (self.data_bytes == 0 or self.report_bytes == 0 or self.poll_interval_ms == 0) {
            return Error.InvalidArg;
        }
        if (!self.boot_interface and self.protocol != .none) return Error.InvalidArg;
    }
};

pub const Dfu = struct {
    can_download: bool = false,
    can_upload: bool = false,
    manifestation_tolerant: bool = false,
    will_detach: bool = false,
    dfu_mode: bool = false,
    detach_timeout_ms: u16 = 0,
    transfer_bytes: u16 = 0,
    bcd_dfu: u16 = 0,

    fn check(self: Dfu) Error!void {
        if (self.transfer_bytes == 0 or self.bcd_dfu == 0) return Error.InvalidArg;
        if (!self.can_download and !self.can_upload) return Error.InvalidArg;
    }

    /// `bmAttributes` of the functional descriptor.
    pub fn attributes(self: Dfu) u8 {
        var bits: u8 = 0;
        if (self.can_download) bits |= wire.DfuAttr.download;
        if (self.can_upload) bits |= wire.DfuAttr.upload;
        if (self.manifestation_tolerant) bits |= wire.DfuAttr.manifestation_tolerant;
        if (self.will_detach) bits |= wire.DfuAttr.will_detach;
        return bits;
    }
};

fn isIn(addr: u8) bool {
    return (addr & wire.Usb.ep_dir_in) != 0;
}

/// The language-id framework: one little-endian language code.
pub fn langid(value: u16, out: []u8) Error!usize {
    if (out.len < Limits.langid_bytes) return Error.InvalidSize;
    const id = if (value == 0) wire.Usb.langid_en_us else value;
    out[0] = @truncate(id);
    out[1] = @truncate(id >> 8);
    return Limits.langid_bytes;
}

/// The string framework: one entry per non-empty slot, in index order.
pub fn strings(device: Device, out: []u8) Error!usize {
    for (device.slots()) |text| {
        if (text.len > Limits.string_chars_max) return Error.RangeCheck;
    }
    const indices = [3]u8{
        wire.StringIndex.manufacturer,
        wire.StringIndex.product,
        wire.StringIndex.serial,
    };
    var cursor = wire.Cursor{ .buf = out };
    for (device.slots(), indices) |text, index| {
        if (text.len == 0) continue;
        cursor.stringEntry(device.effectiveLangid(), index, text);
    }
    if (cursor.overflow) return Error.InvalidSize;
    return cursor.len;
}

/// The device descriptor of a composite device, IAD-tagged for CDC-ACM.
fn putCompositeDevice(cursor: *wire.Cursor, device: Device) void {
    cursor.put(Size.device);
    cursor.put(wire.Type.device);
    cursor.put16(wire.Usb.bcd_200);
    cursor.put(wire.Class.misc);
    cursor.put(wire.Class.subclass_common);
    cursor.put(wire.Class.protocol_iad);
    cursor.put(wire.Usb.ep0_max_packet);
    putIdentity(cursor, device);
}

/// The device descriptor of a device whose class lives on its interface.
fn putPerInterfaceDevice(cursor: *wire.Cursor, device: Device) void {
    cursor.put(Size.device);
    cursor.put(wire.Type.device);
    cursor.put16(wire.Usb.bcd_200);
    cursor.put(wire.Class.per_interface);
    cursor.put(0); // bDeviceSubClass
    cursor.put(0); // bDeviceProtocol
    cursor.put(wire.Usb.ep0_max_packet);
    putIdentity(cursor, device);
}

/// The identity tail both device descriptors share: ids, then string slots.
fn putIdentity(cursor: *wire.Cursor, device: Device) void {
    cursor.put16(device.vid);
    cursor.put16(device.pid);
    cursor.put16(device.effectiveBcdDevice());
    const indices = [3]u8{
        wire.StringIndex.manufacturer,
        wire.StringIndex.product,
        wire.StringIndex.serial,
    };
    for (device.slots(), indices) |text, index| {
        cursor.put(if (text.len == 0) 0 else index);
    }
    cursor.put(wire.Usb.num_configs);
}

/// The configuration descriptor's header; `wTotalLength` is backfilled later.
fn putConfigOpen(cursor: *wire.Cursor, device: Device, ifaces: u8) void {
    cursor.put(Size.config);
    cursor.put(wire.Type.config);
    cursor.put16(0); // wTotalLength, patched once the configuration closes
    cursor.put(ifaces);
    cursor.put(wire.Usb.config_value);
    cursor.put(0); // iConfiguration
    cursor.put(device.configAttributes());
    cursor.put(device.maxPowerUnits());
}

/// The four CDC class-specific interface descriptors of an ACM port.
fn putCdcFunctional(cursor: *wire.Cursor) void {
    cursor.put(Size.cdc_header);
    cursor.put(wire.Type.cs_iface);
    cursor.put(wire.CdcSubtype.header);
    cursor.put16(wire.Usb.bcd_cdc_120);

    cursor.put(Size.cdc_call_management);
    cursor.put(wire.Type.cs_iface);
    cursor.put(wire.CdcSubtype.call_management);
    cursor.put(0x01); // bmCapabilities: call management over the data class
    cursor.put(0x01); // bDataInterface

    cursor.put(Size.cdc_acm);
    cursor.put(wire.Type.cs_iface);
    cursor.put(wire.CdcSubtype.acm);
    cursor.put(0x02); // bmCapabilities: line coding and serial state

    cursor.put(Size.cdc_union);
    cursor.put(wire.Type.cs_iface);
    cursor.put(wire.CdcSubtype.functional_union);
    cursor.put(0); // bControlInterface
    cursor.put(1); // bSubordinateInterface0
}

/// The interface-association descriptor tying the two CDC interfaces together.
fn putCdcAssociation(cursor: *wire.Cursor) void {
    cursor.put(Size.iad);
    cursor.put(wire.Type.iad);
    cursor.put(0); // bFirstInterface
    cursor.put(cdc_ifaces);
    cursor.put(wire.Class.cdc);
    cursor.put(wire.Class.subclass_acm);
    cursor.put(wire.Class.protocol_at);
    cursor.put(0); // iFunction
}

const cdc_ifaces: u8 = 2;

/// The HID class descriptor, naming the report descriptor that follows it.
fn putHidClass(cursor: *wire.Cursor, report_bytes: u16) void {
    cursor.put(Size.hid);
    cursor.put(wire.Type.hid);
    cursor.put16(wire.Usb.bcd_hid_111);
    cursor.put(0); // bCountryCode
    cursor.put(1); // bNumDescriptors
    cursor.put(wire.Type.report);
    cursor.put16(report_bytes);
}

/// The DFU functional descriptor.
fn putDfuFunctional(cursor: *wire.Cursor, upgrade: Dfu) void {
    cursor.put(Size.dfu);
    cursor.put(wire.Type.dfu);
    cursor.put(upgrade.attributes());
    cursor.put16(upgrade.detach_timeout_ms);
    cursor.put16(upgrade.transfer_bytes);
    cursor.put16(upgrade.bcd_dfu);
}

fn finish(cursor: *wire.Cursor, config_at: usize) Error!usize {
    if (cursor.overflow) return Error.InvalidSize;
    cursor.patchTotalLength(config_at);
    return cursor.len;
}

/// A CDC-ACM serial port: notify interface plus a bulk data interface.
pub fn cdcAcm(device: Device, cdc: CdcAcm, out: []u8) Error!usize {
    try cdc.check();
    try device.check();

    var cursor = wire.Cursor{ .buf = out };
    putCompositeDevice(&cursor, device);
    if (cdc.high_speed) {
        cursor.qualifier(wire.Class.misc, wire.Class.subclass_common, wire.Class.protocol_iad);
    }

    const config_at = cursor.len;
    putConfigOpen(&cursor, device, cdc_ifaces);
    putCdcAssociation(&cursor);
    cursor.iface(0, 1, wire.Class.cdc, wire.Class.subclass_acm, wire.Class.protocol_at);
    putCdcFunctional(&cursor);
    cursor.endpoint(
        cdc.notify_ep,
        wire.EndpointAttr.interrupt,
        cdc.notify_bytes,
        cdc.notify_interval_ms,
    );
    cursor.iface(1, 2, wire.Class.cdc_data, 0, 0);
    cursor.endpoint(cdc.out_ep, wire.EndpointAttr.bulk, cdc.data_bytes, 0);
    cursor.endpoint(cdc.in_ep, wire.EndpointAttr.bulk, cdc.data_bytes, 0);
    return finish(&cursor, config_at);
}

/// Bulk-only mass storage: one interface, one IN and one OUT endpoint.
pub fn msc(device: Device, storage: Msc, out: []u8) Error!usize {
    try storage.check();
    try device.check();

    var cursor = wire.Cursor{ .buf = out };
    putPerInterfaceDevice(&cursor, device);
    if (storage.high_speed) cursor.qualifier(wire.Class.per_interface, 0, 0);

    const config_at = cursor.len;
    putConfigOpen(&cursor, device, 1);
    cursor.iface(0, 2, wire.Class.msc, wire.Class.subclass_scsi, wire.Class.protocol_bulk_only);
    cursor.endpoint(storage.in_ep, wire.EndpointAttr.bulk, storage.data_bytes, 0);
    cursor.endpoint(storage.out_ep, wire.EndpointAttr.bulk, storage.data_bytes, 0);
    return finish(&cursor, config_at);
}

/// A human-interface device: one interface, one interrupt IN endpoint.
pub fn hid(device: Device, human: Hid, out: []u8) Error!usize {
    try human.check();
    try device.check();

    var cursor = wire.Cursor{ .buf = out };
    putPerInterfaceDevice(&cursor, device);

    const config_at = cursor.len;
    putConfigOpen(&cursor, device, 1);
    cursor.iface(
        0,
        1,
        wire.Class.hid,
        if (human.boot_interface) wire.Class.subclass_boot else 0,
        @intFromEnum(human.protocol),
    );
    putHidClass(&cursor, human.report_bytes);
    cursor.endpoint(
        human.in_ep,
        wire.EndpointAttr.interrupt,
        human.data_bytes,
        human.poll_interval_ms,
    );
    return finish(&cursor, config_at);
}

/// Device firmware upgrade: one endpoint-less interface plus its functional
/// descriptor, in either the runtime or the DFU-mode protocol.
pub fn dfu(device: Device, upgrade: Dfu, out: []u8) Error!usize {
    try upgrade.check();
    try device.check();

    var cursor = wire.Cursor{ .buf = out };
    putPerInterfaceDevice(&cursor, device);

    const config_at = cursor.len;
    putConfigOpen(&cursor, device, 1);
    cursor.iface(
        0,
        0,
        wire.Class.application,
        wire.Class.subclass_dfu,
        if (upgrade.dfu_mode) wire.Class.protocol_dfu_mode else wire.Class.protocol_dfu_runtime,
    );
    putDfuFunctional(&cursor, upgrade);
    return finish(&cursor, config_at);
}
