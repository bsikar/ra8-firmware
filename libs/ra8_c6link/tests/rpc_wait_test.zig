//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The lifecycle rules for the serial endpoint's one outstanding request.

const std = @import("std");
const implementation = @import("implementation");

const rpc_wait = implementation.rpc_wait;

fn armed() rpc_wait.Wait {
    return .{ .uid = 9, .resp_id = 0x0201, .armed = true };
}

fn idle() rpc_wait.Wait {
    return .{ .uid = 0, .resp_id = 0, .armed = false };
}

test "an open idle link may issue a request" {
    try rpc_wait.issuable(true, idle(), 0);
}

test "a closed link may not issue a request" {
    try std.testing.expectError(error.NotInitialized, rpc_wait.issuable(false, idle(), 0));
}

test "a closed link reports not initialized before it reports busy" {
    try std.testing.expectError(error.NotInitialized, rpc_wait.issuable(false, armed(), 64));
}

test "an armed wait makes the link busy" {
    try std.testing.expectError(error.Busy, rpc_wait.issuable(true, armed(), 0));
}

test "staged bytes with no armed wait still make the link busy" {
    try std.testing.expectError(error.Busy, rpc_wait.issuable(true, idle(), 1));
}

test "the answer to the outstanding request correlates" {
    try std.testing.expect(rpc_wait.answers(armed(), .{ .uid = 9, .msg_id = 0x0201 }));
}

test "no answer correlates while the wait is unarmed" {
    var wait = armed();
    wait.armed = false;
    try std.testing.expect(!rpc_wait.answers(wait, .{ .uid = 9, .msg_id = 0x0201 }));
}

test "another request's uid does not correlate" {
    try std.testing.expect(!rpc_wait.answers(armed(), .{ .uid = 10, .msg_id = 0x0201 }));
}

test "a late answer to a different question does not correlate" {
    try std.testing.expect(!rpc_wait.answers(armed(), .{ .uid = 9, .msg_id = 0x0202 }));
}

test "a zero uid correlates when that is what was sent" {
    const wait = rpc_wait.Wait{ .uid = 0, .resp_id = 0x0301, .armed = true };
    try std.testing.expect(rpc_wait.answers(wait, .{ .uid = 0, .msg_id = 0x0301 }));
}
