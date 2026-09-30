//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! USB chapter-9 and DFU-class wire constants. Values only: every one of
//! these is fixed by the USB 2.0 specification or by the DFU 1.1 class
//! specification, so nothing here is a tunable.

/// `bmRequestType` bytes: direction, type and recipient in one field.
pub const Bm = struct {
    pub const std_dev_in: u8 = 0x80;
    pub const std_dev_out: u8 = 0x00;
    pub const class_if_out: u8 = 0x21;
    pub const class_if_in: u8 = 0xA1;
};

/// `bRequest` codes. The standard and class request spaces are disjoint, so
/// `get_descriptor` and `dfu_abort` sharing 0x06 is not a collision.
pub const Request = struct {
    pub const get_descriptor: u8 = 0x06;
    pub const set_address: u8 = 0x05;
    pub const set_configuration: u8 = 0x09;
    pub const dfu_dnload: u8 = 0x01;
    pub const dfu_upload: u8 = 0x02;
    pub const dfu_getstatus: u8 = 0x03;
    pub const dfu_abort: u8 = 0x06;
};

/// DEVICE descriptor: the type code, its length, and the fields read from it.
pub const DeviceDescriptor = struct {
    pub const desc_type: u8 = 0x01;
    pub const len: u16 = 18;
    /// `idProduct` little-endian pair, at bytes 10 and 11.
    pub const id_product_offset: usize = 10;
};

/// DFU_GETSTATUS payload: its length and the one field this driver reads.
pub const GetStatus = struct {
    pub const len: u16 = 6;
    pub const state_offset: usize = 4;
};

/// `bState` values from DFU 1.1 table 3.2, limited to the two this driver
/// waits on.
pub const State = struct {
    pub const dfu_idle: u8 = 2;
    pub const dfu_dnload_idle: u8 = 5;
};

/// What this driver pins about the device it drives: one configuration, one
/// interface, one address, one transfer size.
pub const Session = struct {
    pub const device_address: u8 = 1;
    pub const configuration_value: u16 = 1;
    pub const interface: u16 = 0;
    /// `wTransferSize` per DFU block, and so the image's block granularity.
    pub const transfer_size: u16 = 64;
};
