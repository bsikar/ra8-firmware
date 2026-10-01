//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! CustomRpc envelope rules for the media-download client.
//!
//! Every media request leaves as a CustomRpc request and comes back as a
//! CustomRpc response, and before the inner generated message is worth
//! decoding the outer one has to be the right one: the co-processor's reply
//! must name the operation we asked for and carry a non-empty body. These are
//! those rules. Building the generated `Rpc` message and decoding the inner
//! payload stay in C at the membrane.

/// Operation ids and the inner response each one expects.
pub const Bound = struct {
    pub const rpc_start: u32 = 0x4D44_0301;
    pub const rpc_next: u32 = 0x4D44_0302;
    pub const rpc_cancel: u32 = 0x4D44_0303;
};

/// Which generated response an operation's reply must carry.
pub const Kind = enum(u8) {
    accepted = 1,
    chunk = 2,
    cancelled = 3,
};

/// The outer CustomRpc response, flattened.
pub const ResponseView = extern struct {
    custom_msg_id: u32 = 0,
    operation: u32 = 0,
    body_len: usize = 0,
    body_present: bool = false,
};

/// The inner response an operation is answered with.
///
/// Null for anything that is not a media operation, which is how a reply
/// routed to the wrong handler is caught before it is decoded.
pub fn kindFor(operation: u32) ?Kind {
    return switch (operation) {
        Bound.rpc_start => .accepted,
        Bound.rpc_next => .chunk,
        Bound.rpc_cancel => .cancelled,
        else => null,
    };
}

/// Whether an operation id names a media operation at all.
pub fn operationValid(operation: u32) bool {
    return kindFor(operation) != null;
}

/// Whether the outer response is the one this call asked for.
///
/// A reply that names a different operation is refused rather than decoded:
/// the inner bytes would be a different generated type, and reading them as
/// the expected one is exactly the confusion this check exists to stop.
pub fn responseCorrelates(view: *const ResponseView) bool {
    return operationValid(view.operation) and view.custom_msg_id == view.operation;
}

/// Whether the outer response carries a body worth decoding.
///
/// An absent body and a present but zero-length body are the same thing here:
/// the decoder hands back a null pointer for both, and neither can contain a
/// generated message.
pub fn bodyPresent(view: *const ResponseView) bool {
    return view.body_present and view.body_len != 0;
}

/// Whether the outer response may be handed to the inner decoder, and as what.
pub fn accept(view: *const ResponseView) ?Kind {
    if (!responseCorrelates(view)) return null;
    if (!bodyPresent(view)) return null;
    return kindFor(view.operation);
}
