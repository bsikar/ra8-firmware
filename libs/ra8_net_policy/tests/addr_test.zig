//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Address literal parsing and classification, both families.

const std = @import("std");
const testing = std.testing;

const policy = @import("policy");

const AddrClass = policy.AddrClass;

fn classOf(ip: []const u8) AddrClass {
    return policy.classifyIp(ip);
}

test "empty and non-literal text is unknown" {
    try testing.expectEqual(AddrClass.unknown, classOf(""));
    try testing.expectEqual(AddrClass.unknown, classOf("example.com"));
    try testing.expectEqual(AddrClass.unknown, classOf("999.1.1.1"));
    try testing.expectEqual(AddrClass.unknown, classOf("1.2.3"));
    try testing.expectEqual(AddrClass.unknown, classOf("1.2.3.4.5"));
}

test "a canonical dotted quad is public" {
    try testing.expectEqual(AddrClass.public, classOf("8.8.8.8"));
    try testing.expectEqual(AddrClass.public, classOf("1.1.1.1"));
    try testing.expectEqual(AddrClass.public, classOf("203.0.113.7"));
}

test "a leading zero is not a canonical octet" {
    try testing.expectEqual(AddrClass.unknown, classOf("010.0.0.1"));
    try testing.expectEqual(AddrClass.unknown, classOf("1.2.3.04"));
    // a bare zero octet is still canonical
    try testing.expectEqual(AddrClass.public, classOf("8.0.0.8"));
}

test "an over-long digit run or a trailing byte is refused" {
    try testing.expectEqual(AddrClass.unknown, classOf("1.2.3.4444"));
    try testing.expectEqual(AddrClass.unknown, classOf("1.2.3.4 "));
    try testing.expectEqual(AddrClass.unknown, classOf("1.2.3.4x"));
    try testing.expectEqual(AddrClass.unknown, classOf("1.2..4"));
}

test "v4 ranges classify" {
    try testing.expectEqual(AddrClass.unknown, classOf("0.0.0.0"));
    try testing.expectEqual(AddrClass.loopback, classOf("127.0.0.1"));
    try testing.expectEqual(AddrClass.private, classOf("10.0.0.1"));
    try testing.expectEqual(AddrClass.private, classOf("192.168.1.1"));
    try testing.expectEqual(AddrClass.linklocal, classOf("169.254.1.1"));
    try testing.expectEqual(AddrClass.unknown, classOf("224.0.0.1"));
    try testing.expectEqual(AddrClass.unknown, classOf("255.255.255.255"));
}

test "the 172.16/12 edges are inclusive and bounded" {
    try testing.expectEqual(AddrClass.public, classOf("172.15.0.1"));
    try testing.expectEqual(AddrClass.private, classOf("172.16.0.1"));
    try testing.expectEqual(AddrClass.private, classOf("172.31.255.1"));
    try testing.expectEqual(AddrClass.public, classOf("172.32.0.1"));
}

test "the 100.64/10 CGNAT edges are inclusive and bounded" {
    try testing.expectEqual(AddrClass.public, classOf("100.63.0.1"));
    try testing.expectEqual(AddrClass.private, classOf("100.64.0.1"));
    try testing.expectEqual(AddrClass.private, classOf("100.127.0.1"));
    try testing.expectEqual(AddrClass.public, classOf("100.128.0.1"));
}

test "169.254 is link-local but the rest of 169 is public" {
    try testing.expectEqual(AddrClass.public, classOf("169.253.0.1"));
    try testing.expectEqual(AddrClass.public, classOf("169.255.0.1"));
}

test "v6 ranges classify" {
    try testing.expectEqual(AddrClass.loopback, classOf("::1"));
    try testing.expectEqual(AddrClass.unknown, classOf("::"));
    try testing.expectEqual(AddrClass.linklocal, classOf("fe80::1"));
    try testing.expectEqual(AddrClass.private, classOf("fc00::1"));
    try testing.expectEqual(AddrClass.private, classOf("fd12:3456::1"));
    try testing.expectEqual(AddrClass.unknown, classOf("ff02::1"));
    try testing.expectEqual(AddrClass.public, classOf("2001:db8::1"));
}

test "a v6 literal is case-insensitive" {
    try testing.expectEqual(AddrClass.linklocal, classOf("FE80::1"));
    try testing.expectEqual(AddrClass.public, classOf("2001:DB8::AbCd"));
}

test "a mapped literal is unwrapped and classified as v4" {
    try testing.expectEqual(AddrClass.loopback, classOf("::ffff:127.0.0.1"));
    try testing.expectEqual(AddrClass.private, classOf("::ffff:10.0.0.1"));
    try testing.expectEqual(AddrClass.public, classOf("::ffff:8.8.8.8"));
}

test "a full eight-group literal parses without a run" {
    try testing.expectEqual(
        AddrClass.public,
        classOf("2001:0db8:0000:0000:0000:0000:0000:0001"),
    );
    // seven groups and no run is short
    try testing.expectEqual(AddrClass.unknown, classOf("2001:db8:0:0:0:0:1"));
    // nine groups is over budget
    try testing.expectEqual(AddrClass.unknown, classOf("1:2:3:4:5:6:7:8:9"));
}

test "a run must stand for at least one group" {
    // eight groups written out with a run as well leaves the run standing for none
    try testing.expectEqual(AddrClass.unknown, classOf("1:2:3:4:5:6:7:8::"));
    try testing.expectEqual(AddrClass.public, classOf("1:2:3:4:5:6:7::"));
}

test "at most one run is accepted" {
    try testing.expectEqual(AddrClass.unknown, classOf("2001::db8::1"));
}

test "a stray or trailing colon is refused" {
    try testing.expectEqual(AddrClass.unknown, classOf(":1:2:3:4:5:6:7"));
    try testing.expectEqual(AddrClass.unknown, classOf("2001:db8:"));
    try testing.expectEqual(AddrClass.unknown, classOf("1:2:3:4:5:6:7:"));
}

test "an over-long group is refused" {
    try testing.expectEqual(AddrClass.unknown, classOf("12345::1"));
}

test "a zone identifier is refused outright" {
    try testing.expectEqual(AddrClass.unknown, classOf("fe80::1%eth0"));
}

test "a trailing quad must itself be canonical" {
    try testing.expectEqual(AddrClass.unknown, classOf("::ffff:1.2.3"));
    try testing.expectEqual(AddrClass.unknown, classOf("::ffff:1.2.3.999"));
}
