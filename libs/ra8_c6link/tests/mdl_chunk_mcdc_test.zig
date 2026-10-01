//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! MC/DC evidence for the chunk decisions, moved here from
//! apps/shared_libs/mdl/tests/src/test_ra8_c6link_mdl_decode.c with the same
//! vectors. Each rejected vector pairs with its control and leaves every
//! earlier condition of its short-circuit chain true.
//! Decisions:
//! libs/ra8_c6link/src/internal/mdl_chunk.zig@httpResponseValid
//! libs/ra8_c6link/src/internal/mdl_chunk.zig@semanticsValid

const std = @import("std");

const implementation = @import("implementation");
const chunk = implementation.mdl_chunk;
const types = implementation.mdl_types;

const body: [10]u8 = @splat('z');
const digest: [32]u8 = @splat(0xA5);

/// A chunk whose non-state fields are all acceptable.
fn base(state: u8) chunk.View {
    var view: chunk.View = .{
        .job_id = 1,
        .state = state,
        .retry_after = "",
        .etag = "",
        .last_modified = "",
        .content_type = "",
    };
    switch (state) {
        types.State.complete => {
            view.http_status = 200;
            view.sha256 = &digest;
        },
        types.State.downloading => view.data = &body,
        types.State.failed => view.status = 7,
        else => {},
    }
    return view;
}

fn http(view: chunk.View) bool {
    return chunk.httpResponseValid(&view);
}

fn semantics(view: chunk.View) bool {
    return chunk.semanticsValid(&view);
}

// @par MC/DC:
// Non-terminal arm, `(http_status == 0) and headersEmpty` (2 conditions):
// control, then status 200, then a non-empty ETag -> 3 vectors.
test "non-terminal http metadata, each condition decides" {
    const control = base(types.State.downloading);
    try std.testing.expect(http(control));
    var vector = control;
    vector.http_status = 200;
    try std.testing.expect(!http(vector));
    vector = control;
    vector.etag = "W/x";
    try std.testing.expect(!http(vector));
}

// @par MC/DC:
// COMPLETE arm, status floor, status roof and the four header checks
// (6 conditions): control, then status 99, status 600, and one CR-bearing
// header at a time -> 7 vectors.
test "terminal http metadata, each condition decides" {
    const control = base(types.State.complete);
    try std.testing.expect(http(control));
    for ([_]i32{ 99, 600 }) |status| {
        var vector = control;
        vector.http_status = status;
        try std.testing.expect(!http(vector));
    }
    var vector = control;
    vector.retry_after = "a\rb";
    try std.testing.expect(!http(vector));
    vector = control;
    vector.etag = "a\rb";
    try std.testing.expect(!http(vector));
    vector = control;
    vector.last_modified = "a\rb";
    try std.testing.expect(!http(vector));
    vector = control;
    vector.content_type = "a\rb";
    try std.testing.expect(!http(vector));
}

// @par MC/DC:
// `(total_bytes == 0) or (end <= total_bytes)` (2 conditions): total 0 -> T,-;
// total 100 with end 10 -> F,T; total 5 with end 10 -> F,F -> 3 vectors.
test "the total covers the body, each condition decides" {
    var view = base(types.State.downloading);
    view.total_bytes = 0;
    try std.testing.expect(semantics(view));
    view.total_bytes = 100;
    try std.testing.expect(semantics(view));
    view.total_bytes = 5;
    try std.testing.expect(!semantics(view));
}

// @par MC/DC:
// DOWNLOADING `(status == 0) and (data != 0) and (sha256 == 0)`: control, a
// nonzero status, no body, an attached digest -> 4 vectors.
test "downloading state, each condition decides" {
    const control = base(types.State.downloading);
    try std.testing.expect(semantics(control));
    var vector = control;
    vector.status = 7;
    try std.testing.expect(!semantics(vector));
    vector = control;
    vector.data = null;
    try std.testing.expect(!semantics(vector));
    vector = control;
    vector.sha256 = &digest;
    try std.testing.expect(!semantics(vector));
}

// @par MC/DC:
// COMPLETE `(status == 0) and (data == 0) and (sha256 == 32) and
// ((total == 0) or (total == end))`: control, a nonzero status, a body, a
// short digest, a total closing at the wrong offset -> 5 vectors. The old C
// view's separate null-digest condition is gone: a slice cannot carry a
// length of 32 and no bytes.
test "complete state, each condition decides" {
    const control = base(types.State.complete);
    try std.testing.expect(semantics(control));
    var vector = control;
    vector.status = 7;
    try std.testing.expect(!semantics(vector));
    vector = control;
    vector.data = &body;
    try std.testing.expect(!semantics(vector));
    vector = control;
    vector.sha256 = digest[0..31];
    try std.testing.expect(!semantics(vector));
    vector = control;
    vector.total_bytes = 100;
    try std.testing.expect(!semantics(vector));
}

// @par MC/DC:
// CANCELLED `(status == 0) and (data == 0) and (sha256 == 0)`: control, a
// nonzero status, a body, a digest -> 4 vectors.
test "cancelled state, each condition decides" {
    const control = base(types.State.cancelled);
    try std.testing.expect(semantics(control));
    var vector = control;
    vector.status = 7;
    try std.testing.expect(!semantics(vector));
    vector = control;
    vector.data = &body;
    try std.testing.expect(!semantics(vector));
    vector = control;
    vector.sha256 = &digest;
    try std.testing.expect(!semantics(vector));
}

// @par MC/DC:
// FAILED `(status > 0) and (status <= maxInt(u16)) and (data == 0) and
// (sha256 == 0)`: control, a zero status, status 65536, a body, a digest ->
// 5 vectors.
test "failed state, each condition decides" {
    const control = base(types.State.failed);
    try std.testing.expect(semantics(control));
    for ([_]i32{ 0, 65536 }) |status| {
        var vector = control;
        vector.status = status;
        try std.testing.expect(!semantics(vector));
    }
    var vector = control;
    vector.data = &body;
    try std.testing.expect(!semantics(vector));
    vector = control;
    vector.sha256 = &digest;
    try std.testing.expect(!semantics(vector));
}
