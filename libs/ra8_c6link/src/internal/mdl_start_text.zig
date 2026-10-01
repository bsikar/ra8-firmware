//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Terminated copies of one Start request's text. The backend request takes
//! C strings, and the decoded text is borrowed, unterminated, from the
//! request bytes, so each piece is copied into fixed storage the service
//! owns and the backend request points there.

const rules = @import("mdl_service_rules.zig");
const types = @import("mdl_types.zig");

const Bound = rules.Bound;

/// `ra8_mdl_start_text_t`.
pub const Text = extern struct {
    url: [Bound.url_max]u8 = @splat(0),
    user_agent: [Bound.user_agent_max]u8 = @splat(0),
    referer: [Bound.referer_max]u8 = @splat(0),
    if_none_match: [Bound.etag_max]u8 = @splat(0),
    if_modified_since: [Bound.http_date_max]u8 = @splat(0),
};

/// Copy `src` into `dst` with its terminator. `rules.startValid` already
/// proved `src` is shorter than `dst`.
fn terminated(dst: []u8, src: []const u8) [*:0]const u8 {
    @memcpy(dst[0..src.len], src);
    dst[src.len] = 0;
    return dst[0..src.len :0].ptr;
}

/// Copy a validated Start request into `text` and return the backend
/// request pointing into it.
pub fn request(text: *Text, view: *const rules.StartView) types.Request {
    return .{
        .url = terminated(&text.url, view.url),
        .format = @intCast(view.format),
        .http = .{
            .user_agent = terminated(&text.user_agent, view.user_agent),
            .referer = terminated(&text.referer, view.referer),
            .if_none_match = terminated(&text.if_none_match, view.if_none_match),
            .if_modified_since = terminated(&text.if_modified_since, view.if_modified_since),
            .timeout_ms = view.timeout_ms,
        },
    };
}
