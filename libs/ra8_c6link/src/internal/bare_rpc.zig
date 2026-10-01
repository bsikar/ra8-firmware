//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The request/answer pairing for the Wi-Fi calls that carry no body.
//!
//! Five of the co-processor's Wi-Fi requests are the same shape: an empty
//! body goes out, a bare result code comes back. What differs is only which
//! generated type the body has and which answer id the facade must then wait
//! on. The generated type is the protobuf layer's business and stays at the C
//! membrane; the pairing is a rule, and it lives here.

/// Request ids this host sends for the bare Wi-Fi calls.
///
/// The co-processor's numbering, mirrored from the generated `RpcId`, not
/// anything chosen here.
pub const Req = struct {
    pub const wifi_deinit: u32 = 279;
    pub const wifi_start: u32 = 280;
    pub const wifi_stop: u32 = 281;
    pub const wifi_connect: u32 = 282;
    pub const wifi_disconnect: u32 = 283;
};

/// Answer ids the co-processor sends back for those requests.
pub const Resp = struct {
    pub const wifi_deinit: u32 = 535;
    pub const wifi_start: u32 = 536;
    pub const wifi_stop: u32 = 537;
    pub const wifi_connect: u32 = 538;
    pub const wifi_disconnect: u32 = 539;
};

/// The answer id that pairs with `req_id`, or null when it is not a bare call.
///
/// Every arm names its answer outright. The two numberings happen to run a
/// fixed distance apart today, and a facade that quietly leaned on that
/// arithmetic would keep working right up until the co-processor renumbered
/// one message.
pub fn respFor(req_id: u32) ?u32 {
    return switch (req_id) {
        Req.wifi_deinit => Resp.wifi_deinit,
        Req.wifi_start => Resp.wifi_start,
        Req.wifi_stop => Resp.wifi_stop,
        Req.wifi_connect => Resp.wifi_connect,
        Req.wifi_disconnect => Resp.wifi_disconnect,
        else => null,
    };
}

/// Does `req_id` name a request whose body is empty?
pub fn isBare(req_id: u32) bool {
    return respFor(req_id) != null;
}
