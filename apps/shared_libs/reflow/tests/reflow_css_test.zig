// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Unit tests for inline CSS declaration parsing (`src/reflow_css.zig`).

const std = @import("std");
const css = @import("reflow").css;

const CssStyle = css.CssStyle;

fn styleFor(decls: []const u8) CssStyle {
    var out = std.mem.zeroes(CssStyle);
    _ = css.ra8_css_parse_inline(decls.ptr, @intCast(decls.len), &out);
    return out;
}

test "is_ws and lower" {
    try std.testing.expect(css.priv_reflow_css_is_ws(' '));
    try std.testing.expect(css.priv_reflow_css_is_ws('\t'));
    try std.testing.expect(css.priv_reflow_css_is_ws('\n'));
    try std.testing.expect(css.priv_reflow_css_is_ws('\r'));
    try std.testing.expect(css.priv_reflow_css_is_ws(0x0C));
    try std.testing.expect(!css.priv_reflow_css_is_ws('a'));
    try std.testing.expectEqual(@as(u8, 'a'), css.priv_reflow_css_lower('A'));
    try std.testing.expectEqual(@as(u8, 'z'), css.priv_reflow_css_lower('Z'));
    try std.testing.expectEqual(@as(u8, '5'), css.priv_reflow_css_lower('5'));
}

test "ci_eq matches case-insensitively and rejects prefixes" {
    try std.testing.expect(css.priv_reflow_css_ci_eq("Bold", 4, "bold"));
    try std.testing.expect(css.priv_reflow_css_ci_eq("UNDERLINE", 9, "underline"));
    try std.testing.expect(!css.priv_reflow_css_ci_eq("boldx", 5, "bold"));
    try std.testing.expect(!css.priv_reflow_css_ci_eq("bol", 3, "bold"));
    try std.testing.expect(!css.priv_reflow_css_ci_eq(null, 3, "bold"));
    try std.testing.expect(!css.priv_reflow_css_ci_eq("red", 3, null));
}

test "trim strips whitespace from both ends" {
    var len: usize = 7;
    const trimmed = css.priv_reflow_css_trim("  ab\t \n", &len);
    try std.testing.expectEqual(@as(usize, 2), len);
    try std.testing.expectEqual(@as(u8, 'a'), trimmed[0]);
    len = 3;
    _ = css.priv_reflow_css_trim("   ", &len);
    try std.testing.expectEqual(@as(usize, 0), len);
}

test "hex colours parse in 3 and 6 digit forms" {
    try std.testing.expectEqual(@as(u32, 0xAABBCC), css.internal_parse_color("#aabbcc", 7));
    try std.testing.expectEqual(@as(u32, 0xFFA500), css.internal_parse_color("#FFA500", 7));
    try std.testing.expectEqual(@as(u32, 0xFFAA00), css.internal_parse_color("#fa0", 4));
    try std.testing.expectEqual(@as(u32, 0x112233), css.internal_parse_color("#123", 4));
    try std.testing.expectEqual(@as(u32, css.k_priv_col_invalid), css.internal_parse_color("#12", 3));
    try std.testing.expectEqual(@as(u32, css.k_priv_col_invalid), css.internal_parse_color("#12345", 6));
    try std.testing.expectEqual(@as(u32, css.k_priv_col_invalid), css.internal_parse_color("#zzz", 4));
}

test "named colours parse case-insensitively" {
    try std.testing.expectEqual(@as(u32, 0x000000), css.internal_parse_color("BLACK", 5));
    try std.testing.expectEqual(@as(u32, 0xFFFFFF), css.internal_parse_color("white", 5));
    try std.testing.expectEqual(@as(u32, 0x008000), css.internal_parse_color("Green", 5));
    try std.testing.expectEqual(@as(u32, 0x808080), css.internal_parse_color("grey", 4));
    try std.testing.expectEqual(@as(u32, 0x808080), css.internal_parse_color("gray", 4));
    try std.testing.expectEqual(@as(u32, 0xC0C0C0), css.internal_parse_color("silver", 6));
    try std.testing.expectEqual(@as(u32, 0x800000), css.internal_parse_color("maroon", 6));
    try std.testing.expectEqual(@as(u32, 0x000080), css.internal_parse_color("NAVY", 4));
    try std.testing.expectEqual(@as(u32, css.k_priv_col_invalid), css.internal_parse_color("teal", 4));
}

test "font sizes parse px, % and em" {
    var v: u16 = 0;
    var u: u8 = 0;
    try std.testing.expect(css.internal_parse_fontsize("12px", 4, &v, &u));
    try std.testing.expectEqual(@as(u16, 12), v);
    try std.testing.expectEqual(css.k_ra8_css_font_px, u);
    try std.testing.expect(css.internal_parse_fontsize("150%", 4, &v, &u));
    try std.testing.expectEqual(@as(u16, 150), v);
    try std.testing.expectEqual(css.k_ra8_css_font_pct, u);
    try std.testing.expect(css.internal_parse_fontsize("1.2em", 5, &v, &u));
    try std.testing.expectEqual(@as(u16, 120), v);
    try std.testing.expectEqual(css.k_ra8_css_font_pct, u);
    try std.testing.expect(css.internal_parse_fontsize("0.5px", 5, &v, &u));
    try std.testing.expectEqual(@as(u16, 0), v);
    try std.testing.expect(css.internal_parse_fontsize("99999px", 7, &v, &u));
    try std.testing.expectEqual(@as(u16, 9999), v);
    try std.testing.expect(!css.internal_parse_fontsize("px", 2, &v, &u));
    try std.testing.expect(!css.internal_parse_fontsize("12pt", 4, &v, &u));
}

test "emphasis properties set style bits" {
    var st = styleFor("font-weight: bold");
    try std.testing.expectEqual(css.k_ra8_css_set_bold, st.set & css.k_ra8_css_set_bold);
    try std.testing.expectEqual(css.k_reflow_style_bold, st.style & css.k_reflow_style_bold);

    st = styleFor("font-style: oblique");
    try std.testing.expectEqual(css.k_ra8_css_set_italic, st.set & css.k_ra8_css_set_italic);
    try std.testing.expectEqual(css.k_reflow_style_italic, st.style & css.k_reflow_style_italic);

    st = styleFor("text-decoration: underline overline");
    try std.testing.expectEqual(css.k_ra8_css_set_underline, st.set & css.k_ra8_css_set_underline);
    try std.testing.expectEqual(css.k_reflow_style_underline, st.style & css.k_reflow_style_underline);

    st = styleFor("font-weight: normal");
    try std.testing.expectEqual(css.k_ra8_css_set_bold, st.set & css.k_ra8_css_set_bold);
    try std.testing.expectEqual(@as(u8, 0), st.style & css.k_reflow_style_bold);

    st = styleFor("text-decoration-line: none");
    try std.testing.expectEqual(@as(u8, 0), st.style & css.k_reflow_style_underline);
}

test "align, colour and display apply through the decl loop" {
    const st = styleFor("text-align:center;color:#FF0000;display:none;font-size:2em");
    try std.testing.expectEqual(css.k_ra8_css_set_align, st.set & css.k_ra8_css_set_align);
    try std.testing.expectEqual(css.k_reflow_align_center, st.@"align");
    try std.testing.expectEqual(css.k_ra8_css_set_color, st.set & css.k_ra8_css_set_color);
    try std.testing.expectEqual(@as(u32, 0xFF0000), st.color);
    try std.testing.expectEqual(css.k_ra8_css_set_display, st.set & css.k_ra8_css_set_display);
    try std.testing.expectEqual(@as(u8, 1), st.display);
    try std.testing.expectEqual(css.k_ra8_css_set_fontsize, st.set & css.k_ra8_css_set_fontsize);
    try std.testing.expectEqual(@as(u16, 200), st.font_val);
    try std.testing.expectEqual(css.k_ra8_css_font_pct, st.font_unit);
}

test "multi-declaration bodies survive whitespace and semicolons" {
    const st = styleFor("  color : gray ;  font-weight : 700 ;;  text-align : justify ; ");
    try std.testing.expectEqual(@as(u32, 0x808080), st.color);
    try std.testing.expectEqual(css.k_reflow_style_bold, st.style & css.k_reflow_style_bold);
    try std.testing.expectEqual(css.k_reflow_align_justify, st.@"align");
}

test "invalid colour leaves the set bit clear" {
    const st = styleFor("color: teal");
    try std.testing.expectEqual(@as(u8, 0), st.set);
}

test "parse_inline zeroes the style and reports null errors" {
    var st: CssStyle = undefined;
    st.set = 0xAA;
    try std.testing.expectEqual(css.k_ra8_ok, css.ra8_css_parse_inline("display:none", 12, &st));
    try std.testing.expectEqual(css.k_ra8_css_set_display, st.set);
    try std.testing.expectEqual(css.k_ra8_err_null_ptr, css.ra8_css_parse_inline(null, 0, &st));
    try std.testing.expectEqual(css.k_ra8_err_null_ptr, css.ra8_css_parse_inline("a", 1, null));
}
