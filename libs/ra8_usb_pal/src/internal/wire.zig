//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The USB descriptor wire vocabulary: the constant groups the standard fixes,
//! and the append-only cursor every builder writes bytes through. Nothing here
//! knows what a device is; `descriptor.zig` owns that and this file owns the
//! encoding.

/// Descriptor sizes, in bytes, as the USB 2.0 standard fixes them.
pub const Size = struct {
    pub const device: u8 = 18;
    pub const config: u8 = 9;
    pub const iface: u8 = 9;
    pub const endpoint: u8 = 7;
    pub const iad: u8 = 8;
    pub const langid: u8 = 2;
    pub const string_header: u8 = 4;
    pub const qualifier: u8 = 10;
    pub const hid: u8 = 9;
    pub const dfu: u8 = 9;
    pub const cdc_header: u8 = 5;
    pub const cdc_call_management: u8 = 5;
    pub const cdc_acm: u8 = 4;
    pub const cdc_union: u8 = 5;
};

/// `bDescriptorType` values. HID class and DFU functional share 0x21; they are
/// told apart by the interface they follow, exactly as on the wire.
pub const Type = struct {
    pub const device: u8 = 0x01;
    pub const config: u8 = 0x02;
    pub const iface: u8 = 0x04;
    pub const endpoint: u8 = 0x05;
    pub const qualifier: u8 = 0x06;
    pub const iad: u8 = 0x0B;
    pub const hid: u8 = 0x21;
    pub const dfu: u8 = 0x21;
    pub const report: u8 = 0x22;
    pub const cs_iface: u8 = 0x24;
};

/// What a caller may hand the builders, and how much room a framework needs.
pub const Limits = struct {
    pub const string_chars_max: u16 = 64;
    pub const string_slots: u16 = 3;
    pub const framework_bytes_max: u16 = 128;
    pub const langid_bytes: u16 = Size.langid;
    pub const strings_bytes_max: u16 = string_slots * (Size.string_header + string_chars_max);
};

/// Protocol constants that are neither a size nor a type.
pub const Usb = struct {
    pub const bcd_200: u16 = 0x0200;
    pub const bcd_cdc_120: u16 = 0x0120;
    pub const bcd_hid_111: u16 = 0x0111;
    pub const bcd_device_default: u16 = 0x0100;
    pub const langid_en_us: u16 = 0x0409;
    pub const ep0_max_packet: u8 = 64;
    pub const power_ma_max: u16 = 500;
    pub const ep_dir_in: u8 = 0x80;
    pub const config_value: u8 = 1;
    pub const num_configs: u8 = 1;
};

/// Class, subclass and protocol triples the four builders publish.
pub const Class = struct {
    pub const per_interface: u8 = 0x00;
    pub const cdc: u8 = 0x02;
    pub const cdc_data: u8 = 0x0A;
    pub const hid: u8 = 0x03;
    pub const msc: u8 = 0x08;
    pub const misc: u8 = 0xEF;
    pub const application: u8 = 0xFE;

    pub const subclass_common: u8 = 0x02;
    pub const subclass_acm: u8 = 0x02;
    pub const subclass_boot: u8 = 0x01;
    pub const subclass_scsi: u8 = 0x06;
    pub const subclass_dfu: u8 = 0x01;

    pub const protocol_iad: u8 = 0x01;
    pub const protocol_at: u8 = 0x01;
    pub const protocol_bulk_only: u8 = 0x50;
    pub const protocol_dfu_runtime: u8 = 0x01;
    pub const protocol_dfu_mode: u8 = 0x02;
};

/// `bmAttributes` of an endpoint descriptor.
pub const EndpointAttr = struct {
    pub const bulk: u8 = 0x02;
    pub const interrupt: u8 = 0x03;
};

/// `bmAttributes` of a configuration descriptor.
pub const ConfigAttr = struct {
    pub const base: u8 = 0x80;
    pub const self_powered: u8 = 0x40;
    pub const remote_wakeup: u8 = 0x20;
};

/// `iManufacturer` / `iProduct` / `iSerialNumber` slots, in that order.
pub const StringIndex = struct {
    pub const manufacturer: u8 = 1;
    pub const product: u8 = 2;
    pub const serial: u8 = 3;
};

/// `bDescriptorSubtype` of the CDC class-specific interface descriptors.
pub const CdcSubtype = struct {
    pub const header: u8 = 0x00;
    pub const call_management: u8 = 0x01;
    pub const acm: u8 = 0x02;
    pub const functional_union: u8 = 0x06;
};

/// `bmAttributes` of the DFU functional descriptor.
pub const DfuAttr = struct {
    pub const download: u8 = 0x01;
    pub const upload: u8 = 0x02;
    pub const manifestation_tolerant: u8 = 0x04;
    pub const will_detach: u8 = 0x08;
};

/// An append-only writer over a caller-owned buffer.
///
/// A write past the end sets `overflow` and drops the byte rather than
/// trapping, so a builder encodes to the end and reports one size refusal
/// instead of checking every put.
pub const Cursor = struct {
    buf: []u8,
    len: usize = 0,
    overflow: bool = false,

    pub fn put(self: *Cursor, byte: u8) void {
        if (self.len >= self.buf.len) {
            self.overflow = true;
            return;
        }
        self.buf[self.len] = byte;
        self.len += 1;
    }

    pub fn put16(self: *Cursor, value: u16) void {
        self.put(@truncate(value));
        self.put(@truncate(value >> 8));
    }

    pub fn putSlice(self: *Cursor, bytes: []const u8) void {
        for (bytes) |byte| self.put(byte);
    }

    pub fn endpoint(self: *Cursor, addr: u8, attributes: u8, max_packet: u16, interval: u8) void {
        self.put(Size.endpoint);
        self.put(Type.endpoint);
        self.put(addr);
        self.put(attributes);
        self.put16(max_packet);
        self.put(interval);
    }

    pub fn iface(self: *Cursor, number: u8, endpoints: u8, class: u8, subclass: u8, protocol: u8) void {
        self.put(Size.iface);
        self.put(Type.iface);
        self.put(number);
        self.put(0); // bAlternateSetting
        self.put(endpoints);
        self.put(class);
        self.put(subclass);
        self.put(protocol);
        self.put(0); // iInterface
    }

    pub fn qualifier(self: *Cursor, class: u8, subclass: u8, protocol: u8) void {
        self.put(Size.qualifier);
        self.put(Type.qualifier);
        self.put16(Usb.bcd_200);
        self.put(class);
        self.put(subclass);
        self.put(protocol);
        self.put(Usb.ep0_max_packet);
        self.put(Usb.num_configs);
        self.put(0); // bReserved
    }

    /// One entry of the string framework: language, index, length, bytes.
    pub fn stringEntry(self: *Cursor, langid: u16, index: u8, text: []const u8) void {
        self.put16(langid);
        self.put(index);
        self.put(@truncate(text.len));
        self.putSlice(text);
    }

    /// Backfill `wTotalLength` of the configuration that opened at `config_at`.
    pub fn patchTotalLength(self: *Cursor, config_at: usize) void {
        const total: u16 = @truncate(self.len - config_at);
        self.buf[config_at + 2] = @truncate(total);
        self.buf[config_at + 3] = @truncate(total >> 8);
    }
};
