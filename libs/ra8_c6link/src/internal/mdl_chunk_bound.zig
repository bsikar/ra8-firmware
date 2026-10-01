//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The largest Chunk the service could send for one pull, worked out from
//! the wire format rather than by measuring a filler message. The service
//! checks it against the caller's buffer before the backend reads a byte,
//! so a pull is never consumed for a reply that cannot be sent.

const std = @import("std");
const rules = @import("mdl_service_rules.zig");
const types = @import("mdl_types.zig");
const wire = @import("mdl_wire.zig");

const u32_max: u64 = std.math.maxInt(u32);
const u64_max: u64 = std.math.maxInt(u64);

/// A varint field: a one-byte tag, since every Chunk field number is below 16.
fn varintField(value: u64) usize {
    return 1 + wire.varintLen(value);
}

/// A length-delimited field: tag, length varint, payload. Empty is omitted.
fn lenField(len: usize) usize {
    if (len == 0) return 0;
    return 1 + wire.varintLen(len) + len;
}

/// Version, job id and sequence at u32 max, offset and total at u64 max, state.
const header: usize = 3 * varintField(u32_max) + 2 * varintField(u64_max) +
    varintField(types.State.complete);

/// Largest data Chunk carrying up to `max_data` body bytes.
pub fn data(max_data: u32) usize {
    return header + lenField(max_data);
}

/// Largest terminal Chunk: the digest, the highest status, every header one
/// byte short of its buffer, which is the most a terminated field can hold.
pub const terminal: usize = header + lenField(types.Limit.sha256_bytes) +
    varintField(@intCast(rules.Bound.status_max)) +
    lenField(rules.Bound.retry_after_max - 1) + lenField(rules.Bound.etag_max - 1) +
    lenField(rules.Bound.http_date_max - 1) + lenField(rules.Bound.content_type_max - 1);

/// Largest Chunk of either shape one pull of `max_data` bytes could produce.
pub fn worstCase(max_data: u32) usize {
    return @max(data(max_data), terminal);
}
