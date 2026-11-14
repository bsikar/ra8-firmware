//! Inline CSS declaration parsing for the reflow engine: whitespace and
//! case-fold primitives, `prop: value` body parsing, and the public
//! `ra8_css_parse_inline` entry point.
//!
//! Zig port of reflow_css.c (RA8FW-1217). Companion unit to
//! reflow_tokenize_attr.zig: the attribute scanner hands each `style="..."`
//! value here, and this module turns it into an `abi.CssStyle` presence mask
//! plus property values (colour, font-size, emphasis, alignment, display).
//! Pure byte-span parsing: no heap, no recursion, and every loop is bounded
//! by the span length it walks.
//!
//! The sheet side of the CSS library (rules and cascade) stays C until its
//! own slice, so the shared primitives keep C linkage (`@export`) and the
//! remaining C translation units link against them exactly as before.
//!
//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//! @since 0.1.0

const std = @import("std");
const abi = @import("reflow_parse_abi.zig");

const CssStyle = abi.CssStyle;

// ---- Shared constants (inc/reflow_types.h, inc/reflow_css.h, ra8_err.h) ----

/// Style bit VALUES from ::reflow_font_style_t.
const k_reflow_style_bold: u8 = 1 << 0;
const k_reflow_style_italic: u8 = 1 << 1;
const k_reflow_style_underline: u8 = 1 << 2;

/// ::reflow_align_t values.
const k_reflow_align_left: u8 = 0;
const k_reflow_align_right: u8 = 1;
const k_reflow_align_center: u8 = 2;
const k_reflow_align_justify: u8 = 3;

/// ::ra8_css_set_t presence bits for the properties this unit applies.
const k_ra8_css_set_bold: u8 = 1 << 0;
const k_ra8_css_set_italic: u8 = 1 << 1;
const k_ra8_css_set_underline: u8 = 1 << 2;
const k_ra8_css_set_align: u8 = 1 << 3;
const k_ra8_css_set_color: u8 = 1 << 4;
const k_ra8_css_set_fontsize: u8 = 1 << 5;
const k_ra8_css_set_display: u8 = 1 << 6;

/// ::ra8_css_font_unit_t values.
const k_ra8_css_font_px: u8 = 0;
const k_ra8_css_font_pct: u8 = 1;

/// ra8_err.h results.
const k_ra8_ok: c_int = 0;
const k_ra8_err_null_ptr: c_int = 0x504;

// ---- Hex colour scanner constants ----

const k_priv_hex_base: u8 = 16;
const k_priv_hex_a10: u8 = 10;
const k_priv_hex3_len: usize = 3;
const k_priv_hex6_len: usize = 6;
const k_priv_hex_nib: u5 = 4;
const k_priv_hex_chan: u5 = 8;

/// ::k_priv_col_t sentinels and CSS Level 1/2 named colours.
const k_priv_col_invalid: u32 = 0xFFFFFFFF;
const k_priv_col_black: u32 = 0x000000;
const k_priv_col_white: u32 = 0xFFFFFF;
const k_priv_col_red: u32 = 0xFF0000;
const k_priv_col_green: u32 = 0x008000;
const k_priv_col_blue: u32 = 0x0000FF;
const k_priv_col_gray: u32 = 0x808080;
const k_priv_col_silver: u32 = 0xC0C0C0;
const k_priv_col_maroon: u32 = 0x800000;
const k_priv_col_navy: u32 = 0x000080;

// ---- Font-size scanner constants ----

const k_priv_fs_dec: u32 = 10;
const k_priv_fs_pct1: u32 = 100;
const k_priv_fs_frac: usize = 2;
const k_priv_fs_max: u32 = 9999;
const k_priv_fs_ulen: usize = 2;

// ---- Whitespace and case primitives ----

/// True iff `c` is a CSS whitespace byte: space, tab, LF, CR or FF.
pub fn priv_reflow_css_is_ws(c: u8) callconv(.c) bool {
    return c == ' ' or c == '\t' or c == '\n' or c == '\r' or c == 0x0C;
}

/// ASCII case fold: `A`-`Z` to `a`-`z`, every other byte unchanged.
pub fn priv_reflow_css_lower(c: u8) callconv(.c) u8 {
    return if (c >= 'A' and c <= 'Z') c + ('a' - 'A') else c;
}

/// Case-insensitive equality of `s[0..len)` with the NUL-terminated `lit`.
///
/// `lit` must not be shorter than `len`; a `lit` that ends before `len`
/// bytes are compared makes the comparison fail, matching the C contract.
pub fn priv_reflow_css_ci_eq(s: ?[*]const u8, len: usize, lit: [*:0]const u8) callconv(.c) bool {
    const sp = s orelse return false;
    var k: usize = 0;
    while (k < len and lit[k] != 0) : (k += 1) {
        if (priv_reflow_css_lower(sp[k]) != priv_reflow_css_lower(lit[k])) {
            return false;
        }
    }
    return k == len and lit[k] == 0;
}

/// Trim CSS whitespace off both ends of `s[0..len.*)`, updating `len` and
/// returning the advanced pointer.
pub fn priv_reflow_css_trim(s: [*]const u8, len: *usize) callconv(.c) [*]const u8 {
    var a: usize = 0;
    var b: usize = len.*;
    while (a < b and priv_reflow_css_is_ws(s[a])) : (a += 1) {}
    while (b > a and priv_reflow_css_is_ws(s[b - 1])) : (b -= 1) {}
    len.* = b - a;
    return s[a..];
}

/// Case-insensitive substring test: `sub` occurs in `s[0..len)`.
fn internal_ci_contains(s: [*]const u8, len: usize, sub: []const u8) bool {
    if (sub.len == 0 or sub.len > len) {
        return false;
    }
    var i: usize = 0;
    while (i + sub.len <= len) : (i += 1) {
        var hit = true;
        for (sub, 0..) |sc, j| {
            if (priv_reflow_css_lower(s[i + j]) != priv_reflow_css_lower(sc)) {
                hit = false;
                break;
            }
        }
        if (hit) {
            return true;
        }
    }
    return false;
}

// ---- Colour parsing ----

/// Hex value of one ASCII digit, or `k_priv_hex_base` if not a hex digit.
fn internal_hex_val(c: u8) u8 {
    if (c >= '0' and c <= '9') {
        return c - '0';
    }
    const l = priv_reflow_css_lower(c);
    if (l >= 'a' and l <= 'f') {
        return l - 'a' + k_priv_hex_a10;
    }
    return k_priv_hex_base;
}

/// Parse `#rgb` (digit-doubled) or `#rrggbb`; invalid otherwise.
fn internal_parse_hex_color(s: [*]const u8, len: usize) u32 {
    var rgb: u32 = 0;
    if (len == k_priv_hex6_len) {
        for (s[0..len]) |c| {
            const v = internal_hex_val(c);
            if (v >= k_priv_hex_base) {
                return k_priv_col_invalid;
            }
            rgb = (rgb << k_priv_hex_nib) | v;
        }
        return rgb;
    }
    if (len == k_priv_hex3_len) {
        for (s[0..len]) |c| {
            const v = internal_hex_val(c);
            if (v >= k_priv_hex_base) {
                return k_priv_col_invalid;
            }
            rgb = (rgb << k_priv_hex_chan) | (@as(u32, v) << k_priv_hex_nib) | v;
        }
        return rgb;
    }
    return k_priv_col_invalid;
}

const ColorName = struct { name: [*:0]const u8, rgb: u32 };

/// CSS Level 1/2 named colour keywords accepted by `color:` declarations.
const k_priv_named_colors = [_]ColorName{
    .{ .name = "black", .rgb = k_priv_col_black },
    .{ .name = "white", .rgb = k_priv_col_white },
    .{ .name = "red", .rgb = k_priv_col_red },
    .{ .name = "green", .rgb = k_priv_col_green },
    .{ .name = "blue", .rgb = k_priv_col_blue },
    .{ .name = "gray", .rgb = k_priv_col_gray },
    .{ .name = "grey", .rgb = k_priv_col_gray },
    .{ .name = "silver", .rgb = k_priv_col_silver },
    .{ .name = "maroon", .rgb = k_priv_col_maroon },
    .{ .name = "navy", .rgb = k_priv_col_navy },
};

/// Parse a CSS colour value: `#rgb`, `#rrggbb`, or a named keyword.
/// Returns `k_priv_col_invalid` when the span is empty or unrecognised.
fn internal_parse_color(s: [*]const u8, len: usize) u32 {
    if (len == 0) {
        return k_priv_col_invalid;
    }
    if (s[0] == '#') {
        return internal_parse_hex_color(s[1..], len - 1);
    }
    for (k_priv_named_colors) |named| {
        if (priv_reflow_css_ci_eq(s, len, named.name)) {
            return named.rgb;
        }
    }
    return k_priv_col_invalid;
}

// ---- Font-size parsing ----

/// Scan a decimal number into hundredths (e.g. "1.2" -> 120); advance `i`
/// past every consumed digit or dot byte and set `any` once a digit lands.
/// Wrapping arithmetic matches the C `unsigned` behaviour on absurd spans.
fn internal_scan_hundredths(s: [*]const u8, len: usize, i: *usize, any: *bool) u32 {
    var hund: u32 = 0;
    while (i.* < len and s[i.*] >= '0' and s[i.*] <= '9') : (i.* += 1) {
        hund = hund *% k_priv_fs_dec +% (s[i.*] - '0');
        any.* = true;
    }
    hund *%= k_priv_fs_pct1; // integer part -> hundredths
    if (i.* < len and s[i.*] == '.') {
        i.* += 1;
        var place: u32 = k_priv_fs_pct1 / k_priv_fs_dec; // tenths
        var fd: usize = 0;
        while (i.* < len and s[i.*] >= '0' and s[i.*] <= '9' and fd < k_priv_fs_frac) : (i.* += 1) {
            hund +%= (s[i.*] - '0') *% place;
            place /= k_priv_fs_dec;
            any.* = true;
            fd += 1;
        }
        while (i.* < len and s[i.*] >= '0' and s[i.*] <= '9') : (i.* += 1) {}
    }
    return hund;
}

/// Clamp a hundredths value to a whole number in [0, k_priv_fs_max].
fn internal_fs_whole(hund: u32) u16 {
    return @intCast(@min(hund / k_priv_fs_pct1, k_priv_fs_max));
}

/// Parse `Npx` / `N%` / `Nem` (N may be fractional) into value + unit.
///
/// `em` is normalised at parse time: 1em = 100%, so the stored hundredths
/// value already IS the percentage. Returns false when no digit was parsed
/// or the unit suffix is unrecognised.
fn internal_parse_fontsize(s: [*]const u8, len: usize, out_val: *u16, out_unit: *u8) bool {
    var i: usize = 0;
    var any = false;
    const hund = internal_scan_hundredths(s, len, &i, &any);
    if (!any) {
        return false;
    }
    const rem = len - i;
    if (rem == k_priv_fs_ulen and priv_reflow_css_ci_eq(s[i..], rem, "px")) {
        out_val.* = internal_fs_whole(hund);
        out_unit.* = k_ra8_css_font_px;
        return true;
    }
    if (rem == 1 and s[i] == '%') {
        out_val.* = internal_fs_whole(hund);
        out_unit.* = k_ra8_css_font_pct;
        return true;
    }
    if (rem == k_priv_fs_ulen and priv_reflow_css_ci_eq(s[i..], rem, "em")) {
        out_val.* = @intCast(@min(hund, k_priv_fs_max));
        out_unit.* = k_ra8_css_font_pct;
        return true;
    }
    return false;
}

// ---- Declaration application ----

/// Set or clear `stylebit` in `out`, marking `setbit` as present.
fn internal_set_emphasis(out: *CssStyle, setbit: u8, stylebit: u8, on: bool) void {
    out.set |= setbit;
    out.style = if (on) out.style | stylebit else out.style & ~stylebit;
}

/// Apply a boolean-emphasis property (font-weight/style/decoration);
/// false for any other property so the caller tries the next handler.
fn internal_apply_emphasis(prop: [*]const u8, plen: usize, val: [*]const u8, vlen: usize, out: *CssStyle) bool {
    if (priv_reflow_css_ci_eq(prop, plen, "font-weight")) {
        const on = priv_reflow_css_ci_eq(val, vlen, "bold") or
            priv_reflow_css_ci_eq(val, vlen, "bolder") or
            priv_reflow_css_ci_eq(val, vlen, "600") or
            priv_reflow_css_ci_eq(val, vlen, "700") or
            priv_reflow_css_ci_eq(val, vlen, "800") or
            priv_reflow_css_ci_eq(val, vlen, "900");
        internal_set_emphasis(out, k_ra8_css_set_bold, k_reflow_style_bold, on);
        return true;
    }
    if (priv_reflow_css_ci_eq(prop, plen, "font-style")) {
        const on = priv_reflow_css_ci_eq(val, vlen, "italic") or
            priv_reflow_css_ci_eq(val, vlen, "oblique");
        internal_set_emphasis(out, k_ra8_css_set_italic, k_reflow_style_italic, on);
        return true;
    }
    if (priv_reflow_css_ci_eq(prop, plen, "text-decoration") or
        priv_reflow_css_ci_eq(prop, plen, "text-decoration-line"))
    {
        const on = internal_ci_contains(val, vlen, "underline");
        internal_set_emphasis(out, k_ra8_css_set_underline, k_reflow_style_underline, on);
        return true;
    }
    return false;
}

/// Apply a parsed `text-align` value; anything unrecognised falls back to
/// the CSS default (left).
fn internal_apply_align(val: [*]const u8, vlen: usize, out: *CssStyle) void {
    out.set |= k_ra8_css_set_align;
    if (priv_reflow_css_ci_eq(val, vlen, "right")) {
        out.@"align" = k_reflow_align_right;
    } else if (priv_reflow_css_ci_eq(val, vlen, "center")) {
        out.@"align" = k_reflow_align_center;
    } else if (priv_reflow_css_ci_eq(val, vlen, "justify")) {
        out.@"align" = k_reflow_align_justify;
    } else {
        out.@"align" = k_reflow_align_left;
    }
}

/// Apply one trimmed `prop: value` pair to `out`, setting the matching bit.
/// Unrecognised properties are silently ignored (no set bit is touched).
fn internal_apply_decl(prop: [*]const u8, plen: usize, val: [*]const u8, vlen: usize, out: *CssStyle) void {
    if (internal_apply_emphasis(prop, plen, val, vlen, out)) {
        return;
    }
    if (priv_reflow_css_ci_eq(prop, plen, "text-align")) {
        internal_apply_align(val, vlen, out);
    } else if (priv_reflow_css_ci_eq(prop, plen, "color")) {
        const rgb = internal_parse_color(val, vlen);
        if (rgb != k_priv_col_invalid) {
            out.set |= k_ra8_css_set_color;
            out.color = rgb;
        }
    } else if (priv_reflow_css_ci_eq(prop, plen, "font-size")) {
        var fv: u16 = 0;
        var fu: u8 = 0;
        if (internal_parse_fontsize(val, vlen, &fv, &fu)) {
            out.set |= k_ra8_css_set_fontsize;
            out.font_val = fv;
            out.font_unit = fu;
        }
    } else if (priv_reflow_css_ci_eq(prop, plen, "display")) {
        out.set |= k_ra8_css_set_display;
        out.display = if (priv_reflow_css_ci_eq(val, vlen, "none")) 1 else 0;
    }
}

// ---- Public API ----

/// Parse a `prop: value; ...` declaration body (no braces) into `out`.
pub fn priv_reflow_css_parse_decls(s: ?[*]const u8, len: usize, out: *CssStyle) callconv(.c) void {
    var i: usize = 0;
    while (i < len) {
        const sp = s.?; // len > 0 implies a readable span
        var semi = i;
        while (semi < len and sp[semi] != ';') : (semi += 1) {}
        var colon = i;
        while (colon < semi and sp[colon] != ':') : (colon += 1) {}
        if (colon < semi) {
            var plen = colon - i;
            const prop = priv_reflow_css_trim(sp[i..], &plen);
            var vlen = semi - (colon + 1);
            const val = priv_reflow_css_trim(sp[colon + 1 ..], &vlen);
            if (plen > 0 and vlen > 0) {
                internal_apply_decl(prop, plen, val, vlen, out);
            }
        }
        i = semi + 1;
    }
}

/// Parse an inline `style="..."` declaration value into `out`.
///
/// Zeroes `out` first, so the caller needs no separate reset. Returns
/// `k_ra8_err_null_ptr` when either pointer is null.
pub fn ra8_css_parse_inline(decls: ?[*]const u8, len: u32, out: ?*CssStyle) callconv(.c) c_int {
    if (decls == null or out == null) {
        return k_ra8_err_null_ptr;
    }
    out.?.* = std.mem.zeroes(CssStyle);
    priv_reflow_css_parse_decls(decls, len, out.?);
    return k_ra8_ok;
}

// C linkage for the remaining CSS translation units (rules + cascade) and
// the C test suites.
comptime {
    @export(&priv_reflow_css_is_ws, .{ .name = "priv_reflow_css_is_ws" });
    @export(&priv_reflow_css_lower, .{ .name = "priv_reflow_css_lower" });
    @export(&priv_reflow_css_ci_eq, .{ .name = "priv_reflow_css_ci_eq" });
    @export(&priv_reflow_css_trim, .{ .name = "priv_reflow_css_trim" });
    @export(&priv_reflow_css_parse_decls, .{ .name = "priv_reflow_css_parse_decls" });
    @export(&ra8_css_parse_inline, .{ .name = "ra8_css_parse_inline" });
}

// ---- Tests ----

fn styleFor(decls: []const u8) CssStyle {
    var out = std.mem.zeroes(CssStyle);
    _ = ra8_css_parse_inline(decls.ptr, @intCast(decls.len), &out);
    return out;
}

test "is_ws and lower" {
    try std.testing.expect(priv_reflow_css_is_ws(' '));
    try std.testing.expect(priv_reflow_css_is_ws('\t'));
    try std.testing.expect(priv_reflow_css_is_ws('\n'));
    try std.testing.expect(priv_reflow_css_is_ws('\r'));
    try std.testing.expect(priv_reflow_css_is_ws(0x0C));
    try std.testing.expect(!priv_reflow_css_is_ws('a'));
    try std.testing.expectEqual(@as(u8, 'a'), priv_reflow_css_lower('A'));
    try std.testing.expectEqual(@as(u8, 'z'), priv_reflow_css_lower('Z'));
    try std.testing.expectEqual(@as(u8, '5'), priv_reflow_css_lower('5'));
}

test "ci_eq matches case-insensitively and rejects prefixes" {
    try std.testing.expect(priv_reflow_css_ci_eq("Bold", 4, "bold"));
    try std.testing.expect(priv_reflow_css_ci_eq("UNDERLINE", 9, "underline"));
    try std.testing.expect(!priv_reflow_css_ci_eq("boldx", 5, "bold"));
    try std.testing.expect(!priv_reflow_css_ci_eq("bol", 3, "bold"));
    try std.testing.expect(!priv_reflow_css_ci_eq(null, 3, "bold"));
}

test "trim strips whitespace from both ends" {
    var len: usize = 7;
    const trimmed = priv_reflow_css_trim("  ab\t \n", &len);
    try std.testing.expectEqual(@as(usize, 2), len);
    try std.testing.expectEqual(@as(u8, 'a'), trimmed[0]);
    len = 3;
    _ = priv_reflow_css_trim("   ", &len);
    try std.testing.expectEqual(@as(usize, 0), len);
}

test "hex colours parse in 3 and 6 digit forms" {
    try std.testing.expectEqual(@as(u32, 0xAABBCC), internal_parse_color("#aabbcc", 7));
    try std.testing.expectEqual(@as(u32, 0xFFA500), internal_parse_color("#FFA500", 7));
    try std.testing.expectEqual(@as(u32, 0xFFAA00), internal_parse_color("#fa0", 4));
    try std.testing.expectEqual(@as(u32, 0x112233), internal_parse_color("#123", 4));
    try std.testing.expectEqual(@as(u32, k_priv_col_invalid), internal_parse_color("#12", 3));
    try std.testing.expectEqual(@as(u32, k_priv_col_invalid), internal_parse_color("#12345", 6));
    try std.testing.expectEqual(@as(u32, k_priv_col_invalid), internal_parse_color("#zzz", 4));
}

test "named colours parse case-insensitively" {
    try std.testing.expectEqual(@as(u32, 0x000000), internal_parse_color("BLACK", 5));
    try std.testing.expectEqual(@as(u32, 0xFFFFFF), internal_parse_color("white", 5));
    try std.testing.expectEqual(@as(u32, 0x008000), internal_parse_color("Green", 5));
    try std.testing.expectEqual(@as(u32, 0x808080), internal_parse_color("grey", 4));
    try std.testing.expectEqual(@as(u32, 0x808080), internal_parse_color("gray", 4));
    try std.testing.expectEqual(@as(u32, 0xC0C0C0), internal_parse_color("silver", 6));
    try std.testing.expectEqual(@as(u32, 0x800000), internal_parse_color("maroon", 6));
    try std.testing.expectEqual(@as(u32, 0x000080), internal_parse_color("NAVY", 4));
    try std.testing.expectEqual(@as(u32, k_priv_col_invalid), internal_parse_color("teal", 4));
}

test "font sizes parse px, % and em" {
    var v: u16 = 0;
    var u: u8 = 0;
    try std.testing.expect(internal_parse_fontsize("12px", 4, &v, &u));
    try std.testing.expectEqual(@as(u16, 12), v);
    try std.testing.expectEqual(k_ra8_css_font_px, u);
    try std.testing.expect(internal_parse_fontsize("150%", 4, &v, &u));
    try std.testing.expectEqual(@as(u16, 150), v);
    try std.testing.expectEqual(k_ra8_css_font_pct, u);
    try std.testing.expect(internal_parse_fontsize("1.2em", 5, &v, &u));
    try std.testing.expectEqual(@as(u16, 120), v);
    try std.testing.expectEqual(k_ra8_css_font_pct, u);
    try std.testing.expect(internal_parse_fontsize("0.5px", 5, &v, &u));
    try std.testing.expectEqual(@as(u16, 0), v);
    try std.testing.expect(internal_parse_fontsize("99999px", 7, &v, &u));
    try std.testing.expectEqual(@as(u16, 9999), v);
    try std.testing.expect(!internal_parse_fontsize("px", 2, &v, &u));
    try std.testing.expect(!internal_parse_fontsize("12pt", 4, &v, &u));
}

test "emphasis properties set style bits" {
    var st = styleFor("font-weight: bold");
    try std.testing.expectEqual(k_ra8_css_set_bold, st.set & k_ra8_css_set_bold);
    try std.testing.expectEqual(k_reflow_style_bold, st.style & k_reflow_style_bold);

    st = styleFor("font-style: oblique");
    try std.testing.expectEqual(k_ra8_css_set_italic, st.set & k_ra8_css_set_italic);
    try std.testing.expectEqual(k_reflow_style_italic, st.style & k_reflow_style_italic);

    st = styleFor("text-decoration: underline overline");
    try std.testing.expectEqual(k_ra8_css_set_underline, st.set & k_ra8_css_set_underline);
    try std.testing.expectEqual(k_reflow_style_underline, st.style & k_reflow_style_underline);

    st = styleFor("font-weight: normal");
    try std.testing.expectEqual(k_ra8_css_set_bold, st.set & k_ra8_css_set_bold);
    try std.testing.expectEqual(@as(u8, 0), st.style & k_reflow_style_bold);

    st = styleFor("text-decoration-line: none");
    try std.testing.expectEqual(@as(u8, 0), st.style & k_reflow_style_underline);
}

test "align, colour and display apply through the decl loop" {
    const st = styleFor("text-align:center;color:#FF0000;display:none;font-size:2em");
    try std.testing.expectEqual(k_ra8_css_set_align, st.set & k_ra8_css_set_align);
    try std.testing.expectEqual(k_reflow_align_center, st.@"align");
    try std.testing.expectEqual(k_ra8_css_set_color, st.set & k_ra8_css_set_color);
    try std.testing.expectEqual(@as(u32, 0xFF0000), st.color);
    try std.testing.expectEqual(k_ra8_css_set_display, st.set & k_ra8_css_set_display);
    try std.testing.expectEqual(@as(u8, 1), st.display);
    try std.testing.expectEqual(k_ra8_css_set_fontsize, st.set & k_ra8_css_set_fontsize);
    try std.testing.expectEqual(@as(u16, 200), st.font_val);
    try std.testing.expectEqual(k_ra8_css_font_pct, st.font_unit);
}

test "multi-declaration bodies survive whitespace and semicolons" {
    const st = styleFor("  color : gray ;  font-weight : 700 ;;  text-align : justify ; ");
    try std.testing.expectEqual(@as(u32, 0x808080), st.color);
    try std.testing.expectEqual(k_reflow_style_bold, st.style & k_reflow_style_bold);
    try std.testing.expectEqual(k_reflow_align_justify, st.@"align");
}

test "invalid colour leaves the set bit clear" {
    const st = styleFor("color: teal");
    try std.testing.expectEqual(@as(u8, 0), st.set);
}

test "parse_inline zeroes the style and reports null errors" {
    var st: CssStyle = undefined;
    st.set = 0xAA;
    try std.testing.expectEqual(k_ra8_ok, ra8_css_parse_inline("display:none", 12, &st));
    try std.testing.expectEqual(k_ra8_css_set_display, st.set);
    try std.testing.expectEqual(k_ra8_err_null_ptr, ra8_css_parse_inline(null, 0, &st));
    try std.testing.expectEqual(k_ra8_err_null_ptr, ra8_css_parse_inline("a", 1, null));
}
