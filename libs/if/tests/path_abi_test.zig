//! ra8_path.h at the C boundary: the untrusted-name policy driven through the
//! exported shapes, so null pointers, byte capacities and the `bool*`
//! out-parameters stay pinned host-side. Needs no filesystem backend.

const std = @import("std");
const abi = @import("abi");
const core = abi.core;

const path_abi = abi.path_policy;

test "ra8_path_sanitize_segment writes a NUL-terminated segment and the verbatim flag" {
    var buffer: [16]u8 = undefined;
    var verbatim: u8 = 0xFF;
    try std.testing.expectEqual(
        core.ok,
        path_abi.ra8_path_sanitize_segment("book.txt", &buffer, buffer.len, &verbatim),
    );
    try std.testing.expectEqualStrings("book.txt", std.mem.sliceTo(&buffer, 0));
    try std.testing.expectEqual(@as(u8, 1), verbatim);

    try std.testing.expectEqual(
        core.ok,
        path_abi.ra8_path_sanitize_segment("a/b", &buffer, buffer.len, &verbatim),
    );
    try std.testing.expectEqualStrings("a_b", std.mem.sliceTo(&buffer, 0));
    try std.testing.expectEqual(@as(u8, 0), verbatim);
}

test "ra8_path_sanitize_segment tolerates a null candidate and a null flag" {
    var buffer: [16]u8 = undefined;
    try std.testing.expectEqual(
        core.ok,
        path_abi.ra8_path_sanitize_segment(null, &buffer, buffer.len, null),
    );
    try std.testing.expectEqualStrings("item", std.mem.sliceTo(&buffer, 0));
}

test "ra8_path_sanitize_segment guards the output pointer then the capacity" {
    var buffer: [16]u8 = undefined;
    try std.testing.expectEqual(
        core.err_null_ptr,
        path_abi.ra8_path_sanitize_segment("a", null, buffer.len, null),
    );
    try std.testing.expectEqual(
        core.err_invalid_size,
        path_abi.ra8_path_sanitize_segment("a", &buffer, 1, null),
    );
}

test "ra8_path_join_under composes and refuses" {
    var buffer: [32]u8 = undefined;
    try std.testing.expectEqual(
        core.ok,
        path_abi.ra8_path_join_under("/books", "a.txt", &buffer, buffer.len),
    );
    try std.testing.expectEqualStrings("/books/a.txt", std.mem.sliceTo(&buffer, 0));

    try std.testing.expectEqual(
        core.err_invalid_arg,
        path_abi.ra8_path_join_under("/books", "../etc", &buffer, buffer.len),
    );
    try std.testing.expectEqual(@as(u8, 0), buffer[0]);

    try std.testing.expectEqual(
        core.err_no_mem,
        path_abi.ra8_path_join_under("/books", "a.txt", &buffer, 8),
    );
    try std.testing.expectEqual(@as(u8, 0), buffer[0]);
}

test "ra8_path_join_under guards every pointer and a zero capacity" {
    var buffer: [32]u8 = undefined;
    try std.testing.expectEqual(
        core.err_null_ptr,
        path_abi.ra8_path_join_under("/books", "a", null, buffer.len),
    );
    try std.testing.expectEqual(
        core.err_invalid_size,
        path_abi.ra8_path_join_under("/books", "a", &buffer, 0),
    );
    try std.testing.expectEqual(
        core.err_null_ptr,
        path_abi.ra8_path_join_under(null, "a", &buffer, buffer.len),
    );
    try std.testing.expectEqual(
        core.err_null_ptr,
        path_abi.ra8_path_join_under("/books", null, &buffer, buffer.len),
    );
}

test "ra8_path_contained reports the verdict as a C bool byte" {
    var verdict: u8 = 0xFF;
    try std.testing.expectEqual(
        core.ok,
        path_abi.ra8_path_contained("/a/b", "/a/b/c", &verdict),
    );
    try std.testing.expectEqual(@as(u8, 1), verdict);

    try std.testing.expectEqual(
        core.ok,
        path_abi.ra8_path_contained("/a/b", "/a/bb", &verdict),
    );
    try std.testing.expectEqual(@as(u8, 0), verdict);
}

test "ra8_path_contained guards its pointers and an empty parent" {
    var verdict: u8 = 0;
    try std.testing.expectEqual(
        core.err_null_ptr,
        path_abi.ra8_path_contained(null, "/a", &verdict),
    );
    try std.testing.expectEqual(
        core.err_null_ptr,
        path_abi.ra8_path_contained("/a", null, &verdict),
    );
    try std.testing.expectEqual(
        core.err_null_ptr,
        path_abi.ra8_path_contained("/a", "/a", null),
    );
    try std.testing.expectEqual(
        core.err_invalid_arg,
        path_abi.ra8_path_contained("///", "/a", &verdict),
    );
}
