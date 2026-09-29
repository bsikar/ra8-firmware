//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! State-free core of `ra8_modem_at`: the project-local string utilities the C
//! kept in place of `string.h`, the final-result-code table, the line
//! accumulator, the capture appender and the URC slot table. Nothing in here
//! touches module state or the C ABI, so every decision the C carried in a
//! TU-private helper is reachable from a host test with both inputs varied.

const std = @import("std");

/// Compile-time tunables mirrored from `ra8_modem_at.h` and the TU-private
/// `ra8_modem_at_internal_const_t` in the C.
pub const max_unsolicited: u8 = 8;
pub const max_prefix_len: u8 = 16;
pub const min_line_buf_bytes: u16 = 16;
pub const default_timeout_ms: u16 = 1000;

/// Mirror of `ra8_modem_line_kind_t` (public in `ra8_modem_at_internal.h`).
pub const LineKind = enum(u8) {
    empty = 0,
    echo = 1,
    urc = 2,
    final_ok = 3,
    final_err = 4,
    payload = 5,
};

/// Mirror of the TU-private `ra8_modem_at_state_t`.
pub const State = enum(u8) {
    idle = 0,
    await_echo = 1,
    await_resp = 2,
    done = 3,
};

/// Mirror of the TU-private `ra8_modem_line_action_t`.
pub const LineAction = enum(u8) {
    cont = 0,
    done_ok = 1,
    done_err = 2,
};

/// `priv_modem_str_len`: length of a NUL-terminated string, capped at
/// `UINT16_MAX` exactly as the C loop's `(i < UINT16_MAX) && (s[i] != 0)`.
pub fn strLen(s: [*]const u8) u16 {
    var i: u16 = 0;
    while (i < std.math.maxInt(u16) and s[i] != 0) : (i += 1) {}
    return i;
}

/// `priv_modem_starts_with`: 1 iff `hay` begins with `needle`. An empty needle
/// matches everything, which the C relied on.
pub fn startsWith(hay: [*]const u8, needle: [*]const u8) u8 {
    var i: usize = 0;
    while (needle[i] != 0) : (i += 1) {
        if (hay[i] != needle[i]) return 0;
    }
    return 1;
}

/// `priv_modem_str_eq`: byte-for-byte equality including length.
pub fn strEq(a: [*]const u8, b: [*]const u8) u8 {
    var i: usize = 0;
    while (a[i] != 0 and b[i] != 0) : (i += 1) {
        if (a[i] != b[i]) return 0;
    }
    return @intFromBool(a[i] == 0 and b[i] == 0);
}

/// Final result codes, in the C's decision order. `OK` is the only
/// non-error final code; everything else sets `is_error`.
pub fn classifyFinal(line: [*]const u8, is_error: *u8) u8 {
    is_error.* = 0;
    if (strEq(line, "OK") != 0) return 1;
    if (strEq(line, "ERROR") != 0) {
        is_error.* = 1;
        return 1;
    }
    if (startsWith(line, "+CME ERROR") != 0) {
        is_error.* = 1;
        return 1;
    }
    if (startsWith(line, "+CMS ERROR") != 0) {
        is_error.* = 1;
        return 1;
    }
    if (strEq(line, "BUSY") != 0) {
        is_error.* = 1;
        return 1;
    }
    if (strEq(line, "NO CARRIER") != 0) {
        is_error.* = 1;
        return 1;
    }
    return 0;
}

/// `internal_append_ch`: append one byte when the NUL still fits. The C's
/// `(*used + 1U) < out_len` leaves the last byte for the terminator.
pub fn appendCh(out: [*]u8, out_len: usize, used: *usize, ch: u8) void {
    if ((used.* + 1) < out_len) {
        out[used.*] = ch;
        used.* += 1;
        out[used.*] = 0;
    }
}

/// `priv_modem_capture_line`: append `line` to `capture`, newline-separated
/// from whatever is already there. A NULL buffer or a zero capacity is a
/// no-op, which is the line-469 OR-decision.
pub fn captureLine(line: [*]const u8, capture: ?[*]u8, capture_len: usize, used: ?*usize) void {
    const buf = capture orelse return;
    if (capture_len == 0) return;
    const used_ptr = used orelse return;
    if (used_ptr.* > 0) appendCh(buf, capture_len, used_ptr, '\n');
    var k: usize = 0;
    while (line[k] != 0) : (k += 1) {
        appendCh(buf, capture_len, used_ptr, line[k]);
    }
}

/// `priv_modem_reset_line_should_clear`: the line-227 AND-decision.
pub fn resetLineShouldClear(line_buf: ?*const anyopaque, line_buf_len: u16) u8 {
    return @intFromBool(line_buf != null and line_buf_len > 0);
}

/// `priv_modem_payload_prefix_matches`: the line-573 three-condition AND.
pub fn payloadPrefixMatches(line: [*]const u8, expected_response: ?[*]const u8) u8 {
    const exp = expected_response orelse return 0;
    if (exp[0] == 0) return 0;
    return @intFromBool(startsWith(line, exp) != 0);
}

/// `priv_modem_capture_should_clear`: the line-664 AND-decision.
pub fn captureShouldClear(capture: ?*const anyopaque, capture_len: usize) u8 {
    return @intFromBool(capture != null and capture_len > 0);
}

/// `internal_effective_timeout`: caller value, else configured default, else
/// the compiled-in 1000 ms.
pub fn effectiveTimeout(requested: u16, configured: u16) u16 {
    if (requested != 0) return requested;
    if (configured != 0) return configured;
    return default_timeout_ms;
}

/// `seen_exp` seed from `internal_wait_response`: a caller that asked for no
/// prefix has already "seen" it.
pub fn seenExpSeed(expected_response: ?[*]const u8) u8 {
    const exp = expected_response orelse return 1;
    return @intFromBool(exp[0] == 0);
}

/// Line accumulator over the caller-owned buffer (`internal_accumulate` plus
/// `internal_reset_line`). CR and LF both close a line; a line that would fill
/// the buffer is emitted early and the byte that overflowed is dropped.
pub const Accumulator = struct {
    buf: ?[*]u8 = null,
    cap: u16 = 0,
    len: u16 = 0,

    /// Empty the accumulator and NUL the caller buffer when one is installed.
    pub fn reset(self: *Accumulator) void {
        self.len = 0;
        if (resetLineShouldClear(self.buf, self.cap) != 0) {
            self.buf.?[0] = 0;
        }
    }

    /// Feed one received byte. Returns the completed NUL-terminated line when
    /// this byte closed one, else null. A missing buffer holds nothing: the C
    /// would have dereferenced NULL here, but init rejects a NULL buffer so
    /// this path is unreachable through the public API.
    pub fn push(self: *Accumulator, byte: u8) ?[*]const u8 {
        const buf = self.buf orelse return null;
        if (byte == '\r' or byte == '\n') {
            if (self.len == 0) return null;
            buf[self.len] = 0;
            self.len = 0;
            return buf;
        }
        if ((@as(u32, self.len) + 1) >= @as(u32, self.cap)) {
            buf[self.len] = 0;
            self.len = 0;
            return buf;
        }
        buf[self.len] = byte;
        self.len += 1;
        return null;
    }
};

/// Fixed URC dispatch table. Generic over the handler pointer type so the
/// host test can bind a plain Zig function and the ABI layer can bind the C
/// `ra8_modem_at_urc_fn_t`.
pub fn UrcTable(comptime Handler: type) type {
    return struct {
        const Self = @This();

        pub const Slot = struct {
            used: u8 = 0,
            prefix: [max_prefix_len]u8 = @splat(0),
            handler: ?Handler = null,
            ctx: ?*anyopaque = null,
        };

        slots: [max_unsolicited]Slot = @splat(.{}),

        /// `internal_clear_urc_table`: drop every registration.
        pub fn clear(self: *Self) void {
            for (&self.slots) |*slot| {
                slot.used = 0;
                slot.handler = null;
                slot.ctx = null;
            }
        }

        /// `internal_urc_replace`: rebind an existing prefix in place.
        pub fn replace(self: *Self, prefix: [*]const u8, handler: Handler, ctx: ?*anyopaque) u8 {
            for (&self.slots) |*slot| {
                if (slot.used == 0) continue;
                if (strEq(&slot.prefix, prefix) != 0) {
                    slot.handler = handler;
                    slot.ctx = ctx;
                    return 1;
                }
            }
            return 0;
        }

        /// `internal_urc_insert`: take the first free slot, registration order
        /// preserved.
        pub fn insert(self: *Self, prefix: [*]const u8, plen: u16, handler: Handler, ctx: ?*anyopaque) u8 {
            for (&self.slots) |*slot| {
                if (slot.used != 0) continue;
                var k: u16 = 0;
                while (k < plen) : (k += 1) {
                    slot.prefix[k] = prefix[k];
                }
                slot.prefix[plen] = 0;
                slot.handler = handler;
                slot.ctx = ctx;
                slot.used = 1;
                return 1;
            }
            return 0;
        }

        /// `internal_dispatch_urc`, split: the first used slot whose prefix the
        /// line carries. The caller invokes the handler so this stays pure.
        pub fn match(self: *Self, line: [*]const u8) ?*Slot {
            for (&self.slots) |*slot| {
                if (slot.used == 0) continue;
                if (startsWith(line, &slot.prefix) != 0) return slot;
            }
            return null;
        }

        /// Occupied slot count, for tests and the full-table guard.
        pub fn used(self: *const Self) u8 {
            var n: u8 = 0;
            for (self.slots) |slot| {
                if (slot.used != 0) n += 1;
            }
            return n;
        }
    };
}
