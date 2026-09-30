//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The three standard chapter-9 control requests the DFU sequence needs:
//! read the DEVICE descriptor, assign an address, select the configuration.

const Err = @import("err").Err;
const hal = @import("hal");
const proto = @import("proto");
const tune = @import("tune");

/// GET_DESCRIPTOR(DEVICE) into `desc`.
///
/// A transfer that succeeds but returns fewer than the full 18 bytes is a
/// hardware error rather than a short read, so the caller retries the reset
/// instead of parsing a truncated descriptor.
pub fn getDeviceDescriptor(
    comptime H: type,
    speed: hal.Speed,
    desc: *[proto.DeviceDescriptor.len]u8,
) Err {
    const setup = hal.Setup{
        .bm_request_type = proto.Bm.std_dev_in,
        .b_request = proto.Request.get_descriptor,
        .w_value = @as(u16, proto.DeviceDescriptor.desc_type) << 8,
        .w_index = 0,
        .w_length = proto.DeviceDescriptor.len,
    };
    var received: u16 = 0;
    const err = H.controlXfer(speed, &setup, desc, proto.DeviceDescriptor.len, &received);
    if (!err.isOk()) return err;
    return if (received == proto.DeviceDescriptor.len) .ok else .hw_error;
}

/// `idProduct` out of a DEVICE descriptor, little-endian.
pub fn productId(desc: *const [proto.DeviceDescriptor.len]u8) u32 {
    const lsb = desc[proto.DeviceDescriptor.id_product_offset];
    const msb = desc[proto.DeviceDescriptor.id_product_offset + 1];
    return @as(u32, lsb) | (@as(u32, msb) << 8);
}

/// SET_ADDRESS, then point the host's default control pipe at the new address.
pub fn setAddress(comptime H: type, speed: hal.Speed) Err {
    const setup = hal.Setup{
        .bm_request_type = proto.Bm.std_dev_out,
        .b_request = proto.Request.set_address,
        .w_value = proto.Session.device_address,
        .w_index = 0,
        .w_length = 0,
    };
    const err = H.controlXfer(speed, &setup, null, 0, null);
    if (!err.isOk()) return err;
    H.delayMs(tune.Delay.address_settle_ms);
    return H.setTarget(speed, proto.Session.device_address);
}

/// SET_CONFIGURATION, which is what makes the DFU interface addressable.
pub fn setConfiguration(comptime H: type, speed: hal.Speed) Err {
    const setup = hal.Setup{
        .bm_request_type = proto.Bm.std_dev_out,
        .b_request = proto.Request.set_configuration,
        .w_value = proto.Session.configuration_value,
        .w_index = 0,
        .w_length = 0,
    };
    return H.controlXfer(speed, &setup, null, 0, null);
}
