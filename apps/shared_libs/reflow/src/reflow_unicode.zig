//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Compact, zero-heap Unicode property tables for Tier-3 text layout and
//! shaping in the reflow engine (`RA8FW-1173` / `RA8FW-296`). Provides UAX #9
//! Bidi_Class and bracket-mirror lookups, UAX #29 grapheme cluster properties,
//! Arabic/Persian contextual joining and presentation forms (including
//! Lam-Alef ligatures), Indic/Thai syllable role classification, and combining
//! mark placement sides. All tables use sorted ranges and binary search so
//! flash footprint stays small enough for EK-RA8D2 MRAM.

const std = @import("std");

pub const BidiClass = enum(u5) {
    l = 0,
    r = 1,
    al = 2,
    en = 3,
    es = 4,
    et = 5,
    an = 6,
    cs = 7,
    nsm = 8,
    bn = 9,
    b = 10,
    s = 11,
    ws = 12,
    on = 13,
    lre = 14,
    lro = 15,
    rle = 16,
    rlo = 17,
    pdf = 18,
    lri = 19,
    rli = 20,
    fsi = 21,
    pdi = 22,

    pub fn isStrong(self: BidiClass) bool {
        return self == .l or self == .r or self == .al;
    }

    pub fn isIsolateInitiator(self: BidiClass) bool {
        return self == .lri or self == .rli or self == .fsi;
    }

    pub fn isExplicitFormatting(self: BidiClass) bool {
        return switch (self) {
            .lre, .lro, .rle, .rlo, .pdf, .bn => true,
            else => false,
        };
    }

    pub fn isRemovedByX9(self: BidiClass) bool {
        return switch (self) {
            .lre, .lro, .rle, .rlo, .pdf, .bn => true,
            else => false,
        };
    }
};

pub const MarkSide = enum(u3) {
    none = 0,
    above = 1,
    below = 2,
    overlay = 3,
};

pub const JoinType = enum(u3) {
    non_joining = 0, // U
    transparent = 1, // T
    right_joining = 2, // R
    dual_joining = 3, // D
    join_causing = 4, // C
};

pub const ArabicForm = enum(u2) {
    isolated = 0,
    final = 1,
    initial = 2,
    medial = 3,
};

pub const GraphemeProp = enum(u4) {
    other = 0,
    cr = 1,
    lf = 2,
    control = 3,
    extend = 4,
    zwj = 5,
    regional_indicator = 6,
    prepend = 7,
    spacing_mark = 8,
};

pub const IndicRole = enum(u4) {
    none = 0,
    consonant = 1,
    vowel_indep = 2,
    virama = 3,
    nukta = 4,
    matra_pre = 5,
    matra_post = 6,
    matra_above = 7,
    matra_below = 8,
    matra_split = 9,
    modifier = 10, // chandrabindu, anusvara, visarga
};

pub const ThaiRole = enum(u4) {
    none = 0,
    leading_vowel = 1, // U+0E40..0E44
    consonant = 2, // U+0E01..0E2E
    vowel_above = 3, // U+0E31, 0E34..0E37, 0E47
    vowel_below = 4, // U+0E38..0E3A
    sara_am = 5, // U+0E33
    vowel_follow = 6, // U+0E30, 0E32, 0E45
    tone_mark = 7, // U+0E48..0E4E
};

fn Range(comptime T: type) type {
    return struct { lo: u32, hi: u32, val: T };
}

fn lookupRange(comptime T: type, table: []const Range(T), cp: u32, default: T) T {
    var lo: usize = 0;
    var hi: usize = table.len;
    while (lo < hi) {
        const mid = lo + (hi - lo) / 2;
        const entry = table[mid];
        if (cp < entry.lo) {
            hi = mid;
        } else if (cp > entry.hi) {
            lo = mid + 1;
        } else {
            return entry.val;
        }
    }
    return default;
}

// Sorted ASCII lookup table (0x00..0x7F).
const k_ascii_bidi: [128]BidiClass = init: {
    var t: [128]BidiClass = @splat(.l);
    var i: usize = 0;
    while (i < 128) : (i += 1) {
        t[i] = switch (i) {
            0x00...0x08, 0x0E...0x1B, 0x7F => .bn,
            0x09, 0x0B, 0x1F => .s,
            0x0A, 0x0D, 0x1C...0x1E => .b,
            0x0C => .ws,
            0x20 => .ws,
            0x21...0x22, 0x26...0x2A, 0x3B...0x40, 0x5B...0x60, 0x7B...0x7E => .on,
            0x23...0x25 => .et,
            0x2B, 0x2D => .es,
            0x2C, 0x2E, 0x2F, 0x3A => .cs,
            0x30...0x39 => .en,
            else => .l,
        };
    }
    break :init t;
};

// Sorted non-ASCII Bidi_Class overrides (default is .l for scripts above 0x7F).
const k_bidi_ranges = [_]Range(BidiClass){
    .{ .lo = 0x0080, .hi = 0x0084, .val = .bn },
    .{ .lo = 0x0085, .hi = 0x0085, .val = .b },
    .{ .lo = 0x0086, .hi = 0x009F, .val = .bn },
    .{ .lo = 0x00A0, .hi = 0x00A0, .val = .cs },
    .{ .lo = 0x00A1, .hi = 0x00A1, .val = .on },
    .{ .lo = 0x00A2, .hi = 0x00A5, .val = .et },
    .{ .lo = 0x00A6, .hi = 0x00A9, .val = .on },
    .{ .lo = 0x00AB, .hi = 0x00AF, .val = .on },
    .{ .lo = 0x00B0, .hi = 0x00B1, .val = .et },
    .{ .lo = 0x00B2, .hi = 0x00B3, .val = .en },
    .{ .lo = 0x00B4, .hi = 0x00B4, .val = .on },
    .{ .lo = 0x00B6, .hi = 0x00B8, .val = .on },
    .{ .lo = 0x00B9, .hi = 0x00B9, .val = .en },
    .{ .lo = 0x00BB, .hi = 0x00BF, .val = .on },
    .{ .lo = 0x00D7, .hi = 0x00D7, .val = .on },
    .{ .lo = 0x00F7, .hi = 0x00F7, .val = .on },
    .{ .lo = 0x0300, .hi = 0x036F, .val = .nsm },
    .{ .lo = 0x0483, .hi = 0x0489, .val = .nsm },
    .{ .lo = 0x0591, .hi = 0x05BD, .val = .nsm },
    .{ .lo = 0x05BE, .hi = 0x05BE, .val = .r },
    .{ .lo = 0x05BF, .hi = 0x05BF, .val = .nsm },
    .{ .lo = 0x05C0, .hi = 0x05C0, .val = .r },
    .{ .lo = 0x05C1, .hi = 0x05C2, .val = .nsm },
    .{ .lo = 0x05C3, .hi = 0x05C3, .val = .r },
    .{ .lo = 0x05C4, .hi = 0x05C5, .val = .nsm },
    .{ .lo = 0x05C6, .hi = 0x05C6, .val = .r },
    .{ .lo = 0x05C7, .hi = 0x05C7, .val = .nsm },
    .{ .lo = 0x05D0, .hi = 0x05FF, .val = .r },
    .{ .lo = 0x0600, .hi = 0x0605, .val = .an },
    .{ .lo = 0x0606, .hi = 0x0607, .val = .on },
    .{ .lo = 0x0608, .hi = 0x0608, .val = .al },
    .{ .lo = 0x0609, .hi = 0x060A, .val = .et },
    .{ .lo = 0x060B, .hi = 0x060B, .val = .al },
    .{ .lo = 0x060C, .hi = 0x060C, .val = .cs },
    .{ .lo = 0x060D, .hi = 0x060D, .val = .al },
    .{ .lo = 0x060E, .hi = 0x060F, .val = .on },
    .{ .lo = 0x0610, .hi = 0x061A, .val = .nsm },
    .{ .lo = 0x061B, .hi = 0x064A, .val = .al }, // includes ALM U+061C
    .{ .lo = 0x064B, .hi = 0x065F, .val = .nsm },
    .{ .lo = 0x0660, .hi = 0x0669, .val = .an },
    .{ .lo = 0x066A, .hi = 0x066A, .val = .et },
    .{ .lo = 0x066B, .hi = 0x066C, .val = .an },
    .{ .lo = 0x066D, .hi = 0x066F, .val = .al },
    .{ .lo = 0x0670, .hi = 0x0670, .val = .nsm },
    .{ .lo = 0x0671, .hi = 0x06D5, .val = .al },
    .{ .lo = 0x06D6, .hi = 0x06DC, .val = .nsm },
    .{ .lo = 0x06DD, .hi = 0x06DD, .val = .an },
    .{ .lo = 0x06DE, .hi = 0x06DE, .val = .on },
    .{ .lo = 0x06DF, .hi = 0x06E4, .val = .nsm },
    .{ .lo = 0x06E5, .hi = 0x06E6, .val = .al },
    .{ .lo = 0x06E7, .hi = 0x06E8, .val = .nsm },
    .{ .lo = 0x06E9, .hi = 0x06E9, .val = .on },
    .{ .lo = 0x06EA, .hi = 0x06ED, .val = .nsm },
    .{ .lo = 0x06EE, .hi = 0x06EF, .val = .al },
    .{ .lo = 0x06F0, .hi = 0x06F9, .val = .en },
    .{ .lo = 0x06FA, .hi = 0x074F, .val = .al },
    .{ .lo = 0x0750, .hi = 0x07BF, .val = .al },
    .{ .lo = 0x07C0, .hi = 0x07FA, .val = .r },
    .{ .lo = 0x0870, .hi = 0x08FF, .val = .al },
    .{ .lo = 0x0900, .hi = 0x0902, .val = .nsm },
    .{ .lo = 0x093A, .hi = 0x093A, .val = .nsm },
    .{ .lo = 0x093C, .hi = 0x093C, .val = .nsm },
    .{ .lo = 0x0941, .hi = 0x0948, .val = .nsm },
    .{ .lo = 0x094D, .hi = 0x094D, .val = .nsm },
    .{ .lo = 0x0951, .hi = 0x0957, .val = .nsm },
    .{ .lo = 0x0962, .hi = 0x0963, .val = .nsm },
    .{ .lo = 0x0981, .hi = 0x0981, .val = .nsm },
    .{ .lo = 0x09BC, .hi = 0x09BC, .val = .nsm },
    .{ .lo = 0x09C1, .hi = 0x09C4, .val = .nsm },
    .{ .lo = 0x09CD, .hi = 0x09CD, .val = .nsm },
    .{ .lo = 0x09E2, .hi = 0x09E3, .val = .nsm },
    .{ .lo = 0x0E31, .hi = 0x0E31, .val = .nsm },
    .{ .lo = 0x0E34, .hi = 0x0E3A, .val = .nsm },
    .{ .lo = 0x0E3F, .hi = 0x0E3F, .val = .et },
    .{ .lo = 0x0E47, .hi = 0x0E4E, .val = .nsm },
    .{ .lo = 0x1AB0, .hi = 0x1AFF, .val = .nsm },
    .{ .lo = 0x1DC0, .hi = 0x1DFF, .val = .nsm },
    .{ .lo = 0x2000, .hi = 0x200A, .val = .ws },
    .{ .lo = 0x200B, .hi = 0x200D, .val = .bn },
    .{ .lo = 0x200E, .hi = 0x200E, .val = .l },
    .{ .lo = 0x200F, .hi = 0x200F, .val = .r },
    .{ .lo = 0x2010, .hi = 0x2027, .val = .on },
    .{ .lo = 0x2028, .hi = 0x2028, .val = .ws },
    .{ .lo = 0x2029, .hi = 0x2029, .val = .b },
    .{ .lo = 0x202A, .hi = 0x202A, .val = .lre },
    .{ .lo = 0x202B, .hi = 0x202B, .val = .rle },
    .{ .lo = 0x202C, .hi = 0x202C, .val = .pdf },
    .{ .lo = 0x202D, .hi = 0x202D, .val = .lro },
    .{ .lo = 0x202E, .hi = 0x202E, .val = .rlo },
    .{ .lo = 0x202F, .hi = 0x202F, .val = .cs },
    .{ .lo = 0x2030, .hi = 0x2034, .val = .et },
    .{ .lo = 0x2035, .hi = 0x2043, .val = .on },
    .{ .lo = 0x2044, .hi = 0x2044, .val = .cs },
    .{ .lo = 0x2045, .hi = 0x205E, .val = .on },
    .{ .lo = 0x205F, .hi = 0x205F, .val = .ws },
    .{ .lo = 0x2060, .hi = 0x2065, .val = .bn },
    .{ .lo = 0x2066, .hi = 0x2066, .val = .lri },
    .{ .lo = 0x2067, .hi = 0x2067, .val = .rli },
    .{ .lo = 0x2068, .hi = 0x2068, .val = .fsi },
    .{ .lo = 0x2069, .hi = 0x2069, .val = .pdi },
    .{ .lo = 0x206A, .hi = 0x206F, .val = .bn },
    .{ .lo = 0x20A0, .hi = 0x20CF, .val = .et },
    .{ .lo = 0x20D0, .hi = 0x20FF, .val = .nsm },
    .{ .lo = 0x2200, .hi = 0x23FF, .val = .on },
    .{ .lo = 0x3000, .hi = 0x3000, .val = .ws },
    .{ .lo = 0x3001, .hi = 0x3020, .val = .on },
    .{ .lo = 0xFB1D, .hi = 0xFB1D, .val = .r },
    .{ .lo = 0xFB1E, .hi = 0xFB1E, .val = .nsm },
    .{ .lo = 0xFB1F, .hi = 0xFB4F, .val = .r },
    .{ .lo = 0xFB50, .hi = 0xFD3D, .val = .al },
    .{ .lo = 0xFD3E, .hi = 0xFD3F, .val = .on },
    .{ .lo = 0xFD40, .hi = 0xFDCF, .val = .al },
    .{ .lo = 0xFDF0, .hi = 0xFDFF, .val = .al },
    .{ .lo = 0xFE00, .hi = 0xFE0F, .val = .nsm },
    .{ .lo = 0xFE20, .hi = 0xFE2F, .val = .nsm },
    .{ .lo = 0xFE70, .hi = 0xFEFE, .val = .al },
    .{ .lo = 0xFEFF, .hi = 0xFEFF, .val = .bn },
    .{ .lo = 0xFFFC, .hi = 0xFFFD, .val = .on },
};

pub fn bidiClass(cp: u32) BidiClass {
    if (cp < 0x80) {
        return k_ascii_bidi[cp];
    }
    return lookupRange(BidiClass, &k_bidi_ranges, cp, .l);
}

pub fn isBidiFormatChar(cp: u32) bool {
    return switch (cp) {
        0x061C,
        0x200B,
        0x200C,
        0x200D,
        0x200E,
        0x200F,
        0x202A...0x202E,
        0x2060,
        0x2066...0x2069,
        0xFEFF,
        => true,
        else => false,
    };
}

/// Return the Bidi_Mirrored glyph pair for paired brackets and directional
/// punctuation, or `cp` unchanged when the code point does not mirror.
pub fn bidiMirror(cp: u32) u32 {
    return switch (cp) {
        '(' => ')',
        ')' => '(',
        '<' => '>',
        '>' => '<',
        '[' => ']',
        ']' => '[',
        '{' => '}',
        '}' => '{',
        0x00AB => 0x00BB,
        0x00BB => 0x00AB,
        0x2039 => 0x203A,
        0x203A => 0x2039,
        0x2045 => 0x2046,
        0x2046 => 0x2045,
        0x27E8 => 0x27E9,
        0x27E9 => 0x27E8,
        0x27EA => 0x27EB,
        0x27EB => 0x27EA,
        0x3008 => 0x3009,
        0x3009 => 0x3008,
        0x300A => 0x300B,
        0x300B => 0x300A,
        0x300C => 0x300D,
        0x300D => 0x300C,
        0x300E => 0x300F,
        0x300F => 0x300E,
        0x3010 => 0x3011,
        0x3011 => 0x3010,
        0xFD3E => 0xFD3F,
        0xFD3F => 0xFD3E,
        0xFF08 => 0xFF09,
        0xFF09 => 0xFF08,
        0xFF3B => 0xFF3D,
        0xFF3D => 0xFF3B,
        0xFF5B => 0xFF5D,
        0xFF5D => 0xFF5B,
        else => cp,
    };
}

pub fn bracketPairKind(cp: u32) ?struct { paired: u32, is_open: bool } {
    return switch (cp) {
        '(' => .{ .paired = ')', .is_open = true },
        ')' => .{ .paired = '(', .is_open = false },
        '[' => .{ .paired = ']', .is_open = true },
        ']' => .{ .paired = '[', .is_open = false },
        '{' => .{ .paired = '}', .is_open = true },
        '}' => .{ .paired = '{', .is_open = false },
        0x27E8 => .{ .paired = 0x27E9, .is_open = true },
        0x27E9 => .{ .paired = 0x27E8, .is_open = false },
        0x3008 => .{ .paired = 0x3009, .is_open = true },
        0x3009 => .{ .paired = 0x3008, .is_open = false },
        0x300A => .{ .paired = 0x300B, .is_open = true },
        0x300B => .{ .paired = 0x300A, .is_open = false },
        0x300C => .{ .paired = 0x300D, .is_open = true },
        0x300D => .{ .paired = 0x300C, .is_open = false },
        0x300E => .{ .paired = 0x300F, .is_open = true },
        0x300F => .{ .paired = 0x300E, .is_open = false },
        0xFF08 => .{ .paired = 0xFF09, .is_open = true },
        0xFF09 => .{ .paired = 0xFF08, .is_open = false },
        0xFF3B => .{ .paired = 0xFF3D, .is_open = true },
        0xFF3D => .{ .paired = 0xFF3B, .is_open = false },
        0xFF5B => .{ .paired = 0xFF5D, .is_open = true },
        0xFF5D => .{ .paired = 0xFF5B, .is_open = false },
        else => null,
    };
}

const k_mark_side_ranges = [_]Range(MarkSide){
    .{ .lo = 0x0300, .hi = 0x0315, .val = .above },
    .{ .lo = 0x0316, .hi = 0x0333, .val = .below },
    .{ .lo = 0x0334, .hi = 0x0338, .val = .overlay },
    .{ .lo = 0x0339, .hi = 0x033C, .val = .below },
    .{ .lo = 0x033D, .hi = 0x0344, .val = .above },
    .{ .lo = 0x0345, .hi = 0x0345, .val = .below },
    .{ .lo = 0x0346, .hi = 0x0346, .val = .above },
    .{ .lo = 0x0347, .hi = 0x0349, .val = .below },
    .{ .lo = 0x034A, .hi = 0x034C, .val = .above },
    .{ .lo = 0x034D, .hi = 0x034E, .val = .below },
    .{ .lo = 0x0350, .hi = 0x0352, .val = .above },
    .{ .lo = 0x0353, .hi = 0x0356, .val = .below },
    .{ .lo = 0x0357, .hi = 0x0358, .val = .above },
    .{ .lo = 0x0359, .hi = 0x035A, .val = .below },
    .{ .lo = 0x035B, .hi = 0x035B, .val = .above },
    .{ .lo = 0x035C, .hi = 0x035C, .val = .below },
    .{ .lo = 0x035D, .hi = 0x036F, .val = .above },
    .{ .lo = 0x0591, .hi = 0x0591, .val = .below },
    .{ .lo = 0x0592, .hi = 0x0595, .val = .above },
    .{ .lo = 0x0596, .hi = 0x0596, .val = .below },
    .{ .lo = 0x0597, .hi = 0x0599, .val = .above },
    .{ .lo = 0x059A, .hi = 0x059B, .val = .below },
    .{ .lo = 0x059C, .hi = 0x05A1, .val = .above },
    .{ .lo = 0x05A2, .hi = 0x05A7, .val = .below },
    .{ .lo = 0x05A8, .hi = 0x05A9, .val = .above },
    .{ .lo = 0x05AA, .hi = 0x05AA, .val = .below },
    .{ .lo = 0x05AB, .hi = 0x05AF, .val = .above },
    .{ .lo = 0x05B0, .hi = 0x05B8, .val = .below },
    .{ .lo = 0x05B9, .hi = 0x05BA, .val = .above },
    .{ .lo = 0x05BB, .hi = 0x05BD, .val = .below },
    .{ .lo = 0x05BF, .hi = 0x05BF, .val = .above },
    .{ .lo = 0x05C1, .hi = 0x05C5, .val = .above },
    .{ .lo = 0x05C7, .hi = 0x05C7, .val = .below },
    .{ .lo = 0x064B, .hi = 0x064C, .val = .above },
    .{ .lo = 0x064D, .hi = 0x064D, .val = .below },
    .{ .lo = 0x064E, .hi = 0x064F, .val = .above },
    .{ .lo = 0x0650, .hi = 0x0650, .val = .below },
    .{ .lo = 0x0651, .hi = 0x0654, .val = .above },
    .{ .lo = 0x0655, .hi = 0x0656, .val = .below },
    .{ .lo = 0x0657, .hi = 0x065B, .val = .above },
    .{ .lo = 0x065C, .hi = 0x065C, .val = .below },
    .{ .lo = 0x065D, .hi = 0x065F, .val = .above },
    .{ .lo = 0x0670, .hi = 0x0670, .val = .above },
    .{ .lo = 0x0900, .hi = 0x0902, .val = .above },
    .{ .lo = 0x093A, .hi = 0x093A, .val = .above },
    .{ .lo = 0x093C, .hi = 0x093C, .val = .below },
    .{ .lo = 0x0941, .hi = 0x0944, .val = .below },
    .{ .lo = 0x0945, .hi = 0x0948, .val = .above },
    .{ .lo = 0x094D, .hi = 0x094D, .val = .below },
    .{ .lo = 0x0951, .hi = 0x0951, .val = .above },
    .{ .lo = 0x0952, .hi = 0x0952, .val = .below },
    .{ .lo = 0x0953, .hi = 0x0957, .val = .above },
    .{ .lo = 0x0962, .hi = 0x0963, .val = .below },
    .{ .lo = 0x0E31, .hi = 0x0E31, .val = .above },
    .{ .lo = 0x0E34, .hi = 0x0E37, .val = .above },
    .{ .lo = 0x0E38, .hi = 0x0E3A, .val = .below },
    .{ .lo = 0x0E47, .hi = 0x0E4E, .val = .above },
};

pub fn markSide(cp: u32) MarkSide {
    if (cp < 0x0300) {
        return .none;
    }
    return lookupRange(MarkSide, &k_mark_side_ranges, cp, .none);
}

pub fn isCombiningMark(cp: u32) bool {
    if (markSide(cp) != .none) {
        return true;
    }
    if (cp < 0x0300) {
        return false;
    }
    return bidiClass(cp) == .nsm and !isBidiFormatChar(cp);
}

// Non-spacing Extend/Prepend/SpacingMark for UAX #29 grapheme clusters.
pub fn graphemeProp(cp: u32) GraphemeProp {
    if (cp == 0x0D) return .cr;
    if (cp == 0x0A) return .lf;
    if (cp < 0x20 or (cp >= 0x7F and cp <= 0x9F) or cp == 0x2028 or cp == 0x2029) {
        return .control;
    }
    if (cp == 0x200D) return .zwj;
    if (cp == 0x200C) return .extend;
    if (cp >= 0x1F1E6 and cp <= 0x1F1FF) return .regional_indicator;
    if (cp == 0x0903 or cp == 0x093B or (cp >= 0x093E and cp <= 0x0940) or
        (cp >= 0x0949 and cp <= 0x094C) or (cp >= 0x094E and cp <= 0x094F) or
        cp == 0x0E30 or cp == 0x0E32 or cp == 0x0E33 or cp == 0x0E45)
    {
        return .spacing_mark;
    }
    if (isCombiningMark(cp) or (cp >= 0xFE00 and cp <= 0xFE0F)) {
        return .extend;
    }
    if ((cp >= 0x0600 and cp <= 0x0605) or cp == 0x06DD or cp == 0x070F) {
        return .prepend;
    }
    return .other;
}

pub fn graphemeBreaksBetween(prev: GraphemeProp, curr: GraphemeProp, ri_odd_run: bool) bool {
    if (prev == .cr and curr == .lf) return false; // GB3
    if (prev == .control or prev == .cr or prev == .lf) return true; // GB4
    if (curr == .control or curr == .cr or curr == .lf) return true; // GB5
    if (curr == .extend or curr == .zwj) return false; // GB9
    if (curr == .spacing_mark) return false; // GB9a
    if (prev == .prepend) return false; // GB9b
    if (prev == .zwj and curr == .other) return true; // non-pictographic ZWJ join handled in shaping
    if (prev == .regional_indicator and curr == .regional_indicator) {
        return !ri_odd_run; // GB12/13
    }
    return true;
}

const ArabicFormsRow = struct {
    cp: u32,
    join: JoinType,
    forms: [4]u32, // isolated, final, initial, medial (0 = none)
};

// Arabic & Persian contextual joining table (Unicode ArabicShaping + Forms-A/B).
const k_arabic_table = [_]ArabicFormsRow{
    .{ .cp = 0x0621, .join = .non_joining, .forms = .{ 0xFE80, 0, 0, 0 } },
    .{ .cp = 0x0622, .join = .right_joining, .forms = .{ 0xFE81, 0xFE82, 0, 0 } },
    .{ .cp = 0x0623, .join = .right_joining, .forms = .{ 0xFE83, 0xFE84, 0, 0 } },
    .{ .cp = 0x0624, .join = .right_joining, .forms = .{ 0xFE85, 0xFE86, 0, 0 } },
    .{ .cp = 0x0625, .join = .right_joining, .forms = .{ 0xFE87, 0xFE88, 0, 0 } },
    .{ .cp = 0x0626, .join = .dual_joining, .forms = .{ 0xFE89, 0xFE8A, 0xFE8B, 0xFE8C } },
    .{ .cp = 0x0627, .join = .right_joining, .forms = .{ 0xFE8D, 0xFE8E, 0, 0 } },
    .{ .cp = 0x0628, .join = .dual_joining, .forms = .{ 0xFE8F, 0xFE90, 0xFE91, 0xFE92 } },
    .{ .cp = 0x0629, .join = .right_joining, .forms = .{ 0xFE93, 0xFE94, 0, 0 } },
    .{ .cp = 0x062A, .join = .dual_joining, .forms = .{ 0xFE95, 0xFE96, 0xFE97, 0xFE98 } },
    .{ .cp = 0x062B, .join = .dual_joining, .forms = .{ 0xFE99, 0xFE9A, 0xFE9B, 0xFE9C } },
    .{ .cp = 0x062C, .join = .dual_joining, .forms = .{ 0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0 } },
    .{ .cp = 0x062D, .join = .dual_joining, .forms = .{ 0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4 } },
    .{ .cp = 0x062E, .join = .dual_joining, .forms = .{ 0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8 } },
    .{ .cp = 0x062F, .join = .right_joining, .forms = .{ 0xFEA9, 0xFEAA, 0, 0 } },
    .{ .cp = 0x0630, .join = .right_joining, .forms = .{ 0xFEAB, 0xFEAC, 0, 0 } },
    .{ .cp = 0x0631, .join = .right_joining, .forms = .{ 0xFEAD, 0xFEAE, 0, 0 } },
    .{ .cp = 0x0632, .join = .right_joining, .forms = .{ 0xFEAF, 0xFEB0, 0, 0 } },
    .{ .cp = 0x0633, .join = .dual_joining, .forms = .{ 0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4 } },
    .{ .cp = 0x0634, .join = .dual_joining, .forms = .{ 0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8 } },
    .{ .cp = 0x0635, .join = .dual_joining, .forms = .{ 0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC } },
    .{ .cp = 0x0636, .join = .dual_joining, .forms = .{ 0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0 } },
    .{ .cp = 0x0637, .join = .dual_joining, .forms = .{ 0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4 } },
    .{ .cp = 0x0638, .join = .dual_joining, .forms = .{ 0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8 } },
    .{ .cp = 0x0639, .join = .dual_joining, .forms = .{ 0xFEC9, 0xFECA, 0xFECB, 0xFECC } },
    .{ .cp = 0x063A, .join = .dual_joining, .forms = .{ 0xFECD, 0xFECE, 0xFECF, 0xFED0 } },
    .{ .cp = 0x0640, .join = .join_causing, .forms = .{ 0x0640, 0x0640, 0x0640, 0x0640 } },
    .{ .cp = 0x0641, .join = .dual_joining, .forms = .{ 0xFED1, 0xFED2, 0xFED3, 0xFED4 } },
    .{ .cp = 0x0642, .join = .dual_joining, .forms = .{ 0xFED5, 0xFED6, 0xFED7, 0xFED8 } },
    .{ .cp = 0x0643, .join = .dual_joining, .forms = .{ 0xFED9, 0xFEDA, 0xFEDB, 0xFEDC } },
    .{ .cp = 0x0644, .join = .dual_joining, .forms = .{ 0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0 } },
    .{ .cp = 0x0645, .join = .dual_joining, .forms = .{ 0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4 } },
    .{ .cp = 0x0646, .join = .dual_joining, .forms = .{ 0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8 } },
    .{ .cp = 0x0647, .join = .dual_joining, .forms = .{ 0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC } },
    .{ .cp = 0x0648, .join = .right_joining, .forms = .{ 0xFEED, 0xFEEE, 0, 0 } },
    .{ .cp = 0x0649, .join = .right_joining, .forms = .{ 0xFEEF, 0xFEF0, 0, 0 } },
    .{ .cp = 0x064A, .join = .dual_joining, .forms = .{ 0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4 } },
    .{ .cp = 0x0671, .join = .right_joining, .forms = .{ 0xFB50, 0xFB51, 0, 0 } },
    .{ .cp = 0x067E, .join = .dual_joining, .forms = .{ 0xFB56, 0xFB57, 0xFB58, 0xFB59 } },
    .{ .cp = 0x0686, .join = .dual_joining, .forms = .{ 0xFB7A, 0xFB7B, 0xFB7C, 0xFB7D } },
    .{ .cp = 0x0698, .join = .right_joining, .forms = .{ 0xFB8A, 0xFB8B, 0, 0 } },
    .{ .cp = 0x06A9, .join = .dual_joining, .forms = .{ 0xFB8E, 0xFB8F, 0xFB90, 0xFB91 } },
    .{ .cp = 0x06AF, .join = .dual_joining, .forms = .{ 0xFB92, 0xFB93, 0xFB94, 0xFB95 } },
    .{ .cp = 0x06C1, .join = .dual_joining, .forms = .{ 0xFBA6, 0xFBA7, 0xFBA8, 0xFBA9 } },
    .{ .cp = 0x06C3, .join = .right_joining, .forms = .{ 0xFBAA, 0xFBAB, 0, 0 } },
    .{ .cp = 0x06CC, .join = .dual_joining, .forms = .{ 0xFBFC, 0xFBFD, 0xFBFE, 0xFBFF } },
    .{ .cp = 0x06D2, .join = .right_joining, .forms = .{ 0xFBAE, 0xFBAF, 0, 0 } },
};

fn findArabicRow(cp: u32) ?*const ArabicFormsRow {
    var lo: usize = 0;
    var hi: usize = k_arabic_table.len;
    while (lo < hi) {
        const mid = lo + (hi - lo) / 2;
        const row = &k_arabic_table[mid];
        if (cp < row.cp) {
            hi = mid;
        } else if (cp > row.cp) {
            lo = mid + 1;
        } else {
            return row;
        }
    }
    return null;
}

pub fn arabicJoinType(cp: u32) JoinType {
    if (cp == 0x200D) return .join_causing; // ZWJ
    if (cp == 0x200C) return .non_joining; // ZWNJ
    if (isCombiningMark(cp) or cp == 0x0670 or (cp >= 0xFE00 and cp <= 0xFE0F)) {
        return .transparent;
    }
    if (findArabicRow(cp)) |row| {
        return row.join;
    }
    if (cp >= 0x066E and cp <= 0x06D5) {
        return switch (cp) {
            0x0671...0x0673, 0x0675...0x0677, 0x0688...0x0699, 0x06C0, 0x06C2...0x06CB, 0x06CD...0x06CF, 0x06D2...0x06D5 => .right_joining,
            else => .dual_joining,
        };
    }
    return .non_joining;
}

pub fn arabicPresentationForm(cp: u32, form: ArabicForm) u32 {
    const row = findArabicRow(cp) orelse return cp;
    const f = row.forms[@backingInt(form)];
    if (f != 0) return f;
    return row.forms[@backingInt(ArabicForm.isolated)];
}

pub fn arabicLamAlefForm(alef: u32, final_form: bool) ?u32 {
    const idx: u32 = if (final_form) 1 else 0;
    return switch (alef) {
        0x0622 => 0xFEF5 + idx,
        0x0623 => 0xFEF7 + idx,
        0x0625 => 0xFEF9 + idx,
        0x0627 => 0xFEFB + idx,
        else => null,
    };
}

pub fn isDevanagari(cp: u32) bool {
    return cp >= 0x0900 and cp <= 0x097F;
}

pub fn isBengali(cp: u32) bool {
    return cp >= 0x0980 and cp <= 0x09FF;
}

pub fn isIndic(cp: u32) bool {
    return cp >= 0x0900 and cp <= 0x0DFF;
}

pub fn isThai(cp: u32) bool {
    return cp >= 0x0E00 and cp <= 0x0E7F;
}

pub fn indicRole(cp: u32) IndicRole {
    if (!isIndic(cp)) return .none;
    const base = cp & ~@as(u32, 0x7F);
    const off = cp & 0x7F;
    if (off == 0x4D) return .virama;
    if (off == 0x3C) return .nukta;
    if (off >= 0x01 and off <= 0x03) return .modifier;
    if (off >= 0x04 and off <= 0x14) return .vowel_indep;
    if (off >= 0x15 and off <= 0x39) return .consonant;
    // Pre-base dependent vowels across Devanagari/Bengali/Gurmukhi/Gujarati/Oriya/Tamil.
    if (off == 0x3F and (base == 0x0900 or base == 0x0980 or base == 0x0A00 or base == 0x0A80)) {
        return .matra_pre;
    }
    if (base == 0x0980 or base == 0x0B00 or base == 0x0B80 or base == 0x0D00) {
        if (off == 0x46 or off == 0x47 or off == 0x48) return .matra_pre;
        if (off == 0x4A or off == 0x4B or off == 0x4C) return .matra_split;
    }
    if (off >= 0x41 and off <= 0x44) return .matra_below;
    if (off >= 0x45 and off <= 0x48) return .matra_above;
    if ((off >= 0x3E and off <= 0x4C) or off == 0x55 or off == 0x56 or off == 0x57) {
        return .matra_post;
    }
    return .none;
}

/// Decompose a split Indic vowel matra into (pre_base_sign, post_base_sign).
pub fn splitIndicMatra(cp: u32) ?struct { pre: u32, post: u32 } {
    const base = cp & ~@as(u32, 0x7F);
    const off = cp & 0x7F;
    if (base == 0x0980) { // Bengali
        if (off == 0x4B) return .{ .pre = 0x09C7, .post = 0x09BE };
        if (off == 0x4C) return .{ .pre = 0x09C7, .post = 0x09D7 };
    } else if (base == 0x0B00) { // Oriya
        if (off == 0x4B) return .{ .pre = 0x0B47, .post = 0x0B3E };
        if (off == 0x4C) return .{ .pre = 0x0B47, .post = 0x0B57 };
    } else if (base == 0x0B80) { // Tamil
        if (off == 0x4A) return .{ .pre = 0x0BC6, .post = 0x0BBE };
        if (off == 0x4B) return .{ .pre = 0x0BC7, .post = 0x0BBE };
        if (off == 0x4C) return .{ .pre = 0x0BC6, .post = 0x0BD7 };
    } else if (base == 0x0D00) { // Malayalam
        if (off == 0x4A) return .{ .pre = 0x0D46, .post = 0x0D3E };
        if (off == 0x4B) return .{ .pre = 0x0D47, .post = 0x0D3E };
        if (off == 0x4C) return .{ .pre = 0x0D46, .post = 0x0D57 };
    }
    return null;
}

pub fn thaiRole(cp: u32) ThaiRole {
    if (!isThai(cp)) return .none;
    return switch (cp) {
        0x0E01...0x0E2E => .consonant,
        0x0E30, 0x0E32, 0x0E45 => .vowel_follow,
        0x0E31, 0x0E34...0x0E37, 0x0E47 => .vowel_above,
        0x0E33 => .sara_am,
        0x0E38...0x0E3A => .vowel_below,
        0x0E40...0x0E44 => .leading_vowel,
        0x0E48...0x0E4E => .tone_mark,
        else => .none,
    };
}
