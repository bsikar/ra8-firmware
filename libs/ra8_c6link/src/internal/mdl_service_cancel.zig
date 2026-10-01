//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The service's answer to one CancelRequest: decode it, check it names the
//! active job, and encode the Cancelled acknowledgement. The reply is built
//! in scratch and copied out whole, so a response buffer that is too small
//! is left untouched, as the generated pack path guaranteed.

const pull = @import("mdl_pull.zig");
const reply_encode = @import("mdl_reply_encode.zig");
const request_decode = @import("mdl_request_decode.zig");

pub const Error = error{ Malformed, Uncorrelated, NoSpace };

/// Largest Cancelled: two one-byte tags, a one-byte version, a 5-byte job id.
pub const reply_max: usize = 8;

/// Build the Cancelled reply for `request` into `out`; returns its bytes.
pub fn reply(request: []const u8, job: *const pull.JobView, out: []u8) Error![]const u8 {
    const view = try request_decode.cancel(request);
    if (!pull.cancelCorrelates(&view, job)) return error.Uncorrelated;
    var scratch: [reply_max]u8 = undefined;
    const bytes = reply_encode.cancelled(&scratch, view.job_id) catch unreachable;
    if (bytes.len > out.len) return error.NoSpace;
    @memcpy(out[0..bytes.len], bytes);
    return out[0..bytes.len];
}
