//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The lifecycle rules for the serial endpoint's one outstanding request:
//! whether a new request may go out, and whether a decoded response is the
//! answer to the one that did.
//!
//! The link holds exactly one wait at a time, so both decisions read the same
//! small record. Neither touches the extractor callback or the decoder's
//! memory: those stay with the caller.

/// One outstanding request, as the link records it while it waits.
pub const Wait = struct {
    /// UID stamped on the request and echoed by its answer.
    uid: u32,
    /// Message id the request is answered by.
    resp_id: u32,
    /// A request is outstanding.
    armed: bool,
};

/// The two correlation fields of one decoded response.
pub const Response = struct {
    /// UID the peer echoed back.
    uid: u32,
    /// Message id the peer answered with.
    msg_id: u32,
};

/// Why a new request could not be issued.
pub const Refusal = error{
    NotInitialized,
    Busy,
};

/// May another request go out on this link?
///
/// The link carries one wait and one staged transmission, so a second request
/// is refused while either is still in flight. Staged bytes with no armed wait
/// still mean busy: the previous payload has not been clocked out yet.
pub fn issuable(open: bool, wait: Wait, tx_len: u16) Refusal!void {
    if (!open) return Refusal.NotInitialized;
    if (wait.armed or tx_len != 0) return Refusal.Busy;
}

/// Is `response` the answer to `wait`?
///
/// All three conditions have to hold. A UID that matches but a message id that
/// does not is a different question's answer arriving late, and an unarmed
/// wait has no question for any answer to match.
pub fn answers(wait: Wait, response: Response) bool {
    if (!wait.armed) return false;
    if (response.uid != wait.uid) return false;
    return response.msg_id == wait.resp_id;
}
