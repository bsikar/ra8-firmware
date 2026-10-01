//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Admission rules for the media-download response extractors.
//!
//! Every media call picks its extractor when it goes out: a start expects an
//! Accepted, a pull expects a Chunk, a cancel expects a Cancelled. Two things
//! have to hold before an extractor touches caller state. The envelope must
//! select the same inner response the caller is waiting for, so a reply routed
//! to the wrong handler is refused rather than read as the wrong generated
//! type. And a decoded chunk must both correlate with the session and satisfy
//! the state rules before any of it is copied out. Decoding the generated
//! message stays in C at the membrane; these are the decisions around it.

const envelope = @import("mdl_envelope.zig");
const mdl_chunk = @import("mdl_chunk.zig");
const mdl_session = @import("mdl_session.zig");
const types = @import("mdl_types.zig");

/// The inner response an extractor handles, shared with the envelope rules.
pub const Kind = envelope.Kind;

/// The extractor this response may run, or null when none may.
///
/// `expected` is the kind the caller chose when it sent the request. The
/// envelope decides what the reply actually carries; this is where the two
/// have to agree. A reply the envelope refuses outright and a reply that
/// carries a different media response are the same answer here: no extractor
/// runs, and nothing is decoded.
pub fn selected(view: *const envelope.ResponseView, expected: u8) ?Kind {
    const kind = envelope.accept(view) orelse return null;
    if (@intFromEnum(kind) != expected) return null;
    return kind;
}

/// Whether a decoded chunk may be applied to the caller's session.
///
/// Correlation answers "is this the chunk this session is waiting for" and the
/// semantic rules answer "is this chunk coherent on its own terms". Both have
/// to hold, and keeping the conjunction here means the call site copies a
/// chunk or refuses it on one answer.
pub fn chunkAdmissible(
    key: *const mdl_session.ChunkKeyView,
    view: *const mdl_chunk.View,
    session: *const types.Session,
    requested_bytes: u32,
) bool {
    return mdl_session.chunkCorrelates(key, session, requested_bytes) and
        mdl_chunk.semanticsValid(view);
}
