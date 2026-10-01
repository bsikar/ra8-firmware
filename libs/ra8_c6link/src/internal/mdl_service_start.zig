//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The service's side of one StartRequest, around the job-id choice and the
//! backend begin the C dispatcher still makes: `admit` decodes, checks and
//! copies the request into the service's text storage, and `accepted`
//! encodes the reply. The reply is built in scratch and copied out whole,
//! so a response buffer that is too small is left untouched.

const reply_encode = @import("mdl_reply_encode.zig");
const request_decode = @import("mdl_request_decode.zig");
const rules = @import("mdl_service_rules.zig");
const start_text = @import("mdl_start_text.zig");
const types = @import("mdl_types.zig");

pub const AdmitError = error{ Malformed, Invalid, Busy };

/// Largest Accepted: four one-byte tags, version 1, job id 5, chunk bound 2,
/// and a format byte up to 255, which takes 2.
pub const accepted_max: usize = 14;

/// Decode and check `request`, refuse it while a job runs, and only then
/// copy it into `text`. Returns the backend request pointing into `text`.
pub fn admit(request: []const u8, active: bool, text: *start_text.Text) AdmitError!types.Request {
    const view = try request_decode.start(request);
    if (!rules.startValid(&view)) return error.Invalid;
    if (active) return error.Busy;
    return start_text.request(text, &view);
}

/// Build the Accepted reply for `job_id` into `out`; returns its bytes.
pub fn accepted(job_id: u32, format: u8, out: []u8) error{NoSpace}![]const u8 {
    var scratch: [accepted_max]u8 = undefined;
    const bytes = reply_encode.accepted(&scratch, job_id, format) catch unreachable;
    if (bytes.len > out.len) return error.NoSpace;
    @memcpy(out[0..bytes.len], bytes);
    return out[0..bytes.len];
}
