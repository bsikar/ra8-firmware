//! XHTML 1.0 named character reference table for the reflow tokenizer.
//!
//! Zig port of reflow_tokenize_entities.c (RA8FW-1216). Companion unit to
//! reflow_tokenize_lex.zig: it holds the one lookup the lexer needs and
//! nothing else - the 253 named character references XHTML 1.0 defines
//! (Latin-1, symbols and special entity sets) plus XML's `apos`, mapped to
//! their Unicode code points. Rows are sorted by name under plain byte
//! order, so the lookup is a bounded binary search with no recursion and no
//! allocation. Matching is exact and case-sensitive: `&Eacute;` and
//! `&eacute;` are different references and `&AMP;` is not one, which is what
//! the XML specification requires of XHTML content documents.
//!
//! The name field is a fixed [11]u8 (NUL padded) rather than a pointer, so
//! a name longer than the tokenizer's `&...;` scan window can hold
//! (k_priv_entity_name_max) fails to compile instead of sitting in the
//! table as a row no input can ever reach. See ADR-0010.
//!
//! The functions keep C linkage (`export fn`) so the C test suites and the
//! remaining C translation units keep linking against them.
//!
//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//! @since 0.1.0

const std = @import("std");

/// Capacity of one table name: one byte more than the longest name the
/// scan window admits, so an over-long literal is a compile error.
const k_priv_entity_name_cap = 11;

/// Longest reference name the tokenizer's `&...;` scan window can hold.
const k_priv_entity_name_max = 10;

/// Comptime name builder: pads a reference name into the fixed [11]u8 row
/// shape (NUL padded), so an over-long literal fails the table's length
/// check instead of sitting in the table as a row no input can reach.
fn entName(comptime s: []const u8) [k_priv_entity_name_cap]u8 {
    var a: [k_priv_entity_name_cap]u8 = std.mem.zeroes([k_priv_entity_name_cap]u8);
    @memcpy(a[0..s.len], s);
    return a;
}

/// One named character reference: its name and the code point it denotes.
const EntityRow = struct {
    name: [k_priv_entity_name_cap]u8,
    cp: u32,
};

/// The XHTML 1.0 named character references, sorted by name.
///
/// Byte-order sorted (uppercase before lowercase), which is the order
/// internalEntityCmp() compares in and the order the binary search in
/// privReflowTokLookupEntity() depends on. Asserted by
/// tests/src/test_reflow_entities.c over the whole table.
const k_priv_entities = [_]EntityRow{
    .{ .name = entName("AElig"), .cp = 0x00C6 },
    .{ .name = entName("Aacute"), .cp = 0x00C1 },
    .{ .name = entName("Acirc"), .cp = 0x00C2 },
    .{ .name = entName("Agrave"), .cp = 0x00C0 },
    .{ .name = entName("Alpha"), .cp = 0x0391 },
    .{ .name = entName("Aring"), .cp = 0x00C5 },
    .{ .name = entName("Atilde"), .cp = 0x00C3 },
    .{ .name = entName("Auml"), .cp = 0x00C4 },
    .{ .name = entName("Beta"), .cp = 0x0392 },
    .{ .name = entName("Ccedil"), .cp = 0x00C7 },
    .{ .name = entName("Chi"), .cp = 0x03A7 },
    .{ .name = entName("Dagger"), .cp = 0x2021 },
    .{ .name = entName("Delta"), .cp = 0x0394 },
    .{ .name = entName("ETH"), .cp = 0x00D0 },
    .{ .name = entName("Eacute"), .cp = 0x00C9 },
    .{ .name = entName("Ecirc"), .cp = 0x00CA },
    .{ .name = entName("Egrave"), .cp = 0x00C8 },
    .{ .name = entName("Epsilon"), .cp = 0x0395 },
    .{ .name = entName("Eta"), .cp = 0x0397 },
    .{ .name = entName("Euml"), .cp = 0x00CB },
    .{ .name = entName("Gamma"), .cp = 0x0393 },
    .{ .name = entName("Iacute"), .cp = 0x00CD },
    .{ .name = entName("Icirc"), .cp = 0x00CE },
    .{ .name = entName("Igrave"), .cp = 0x00CC },
    .{ .name = entName("Iota"), .cp = 0x0399 },
    .{ .name = entName("Iuml"), .cp = 0x00CF },
    .{ .name = entName("Kappa"), .cp = 0x039A },
    .{ .name = entName("Lambda"), .cp = 0x039B },
    .{ .name = entName("Mu"), .cp = 0x039C },
    .{ .name = entName("Ntilde"), .cp = 0x00D1 },
    .{ .name = entName("Nu"), .cp = 0x039D },
    .{ .name = entName("OElig"), .cp = 0x0152 },
    .{ .name = entName("Oacute"), .cp = 0x00D3 },
    .{ .name = entName("Ocirc"), .cp = 0x00D4 },
    .{ .name = entName("Ograve"), .cp = 0x00D2 },
    .{ .name = entName("Omega"), .cp = 0x03A9 },
    .{ .name = entName("Omicron"), .cp = 0x039F },
    .{ .name = entName("Oslash"), .cp = 0x00D8 },
    .{ .name = entName("Otilde"), .cp = 0x00D5 },
    .{ .name = entName("Ouml"), .cp = 0x00D6 },
    .{ .name = entName("Phi"), .cp = 0x03A6 },
    .{ .name = entName("Pi"), .cp = 0x03A0 },
    .{ .name = entName("Prime"), .cp = 0x2033 },
    .{ .name = entName("Psi"), .cp = 0x03A8 },
    .{ .name = entName("Rho"), .cp = 0x03A1 },
    .{ .name = entName("Scaron"), .cp = 0x0160 },
    .{ .name = entName("Sigma"), .cp = 0x03A3 },
    .{ .name = entName("THORN"), .cp = 0x00DE },
    .{ .name = entName("Tau"), .cp = 0x03A4 },
    .{ .name = entName("Theta"), .cp = 0x0398 },
    .{ .name = entName("Uacute"), .cp = 0x00DA },
    .{ .name = entName("Ucirc"), .cp = 0x00DB },
    .{ .name = entName("Ugrave"), .cp = 0x00D9 },
    .{ .name = entName("Upsilon"), .cp = 0x03A5 },
    .{ .name = entName("Uuml"), .cp = 0x00DC },
    .{ .name = entName("Xi"), .cp = 0x039E },
    .{ .name = entName("Yacute"), .cp = 0x00DD },
    .{ .name = entName("Yuml"), .cp = 0x0178 },
    .{ .name = entName("Zeta"), .cp = 0x0396 },
    .{ .name = entName("aacute"), .cp = 0x00E1 },
    .{ .name = entName("acirc"), .cp = 0x00E2 },
    .{ .name = entName("acute"), .cp = 0x00B4 },
    .{ .name = entName("aelig"), .cp = 0x00E6 },
    .{ .name = entName("agrave"), .cp = 0x00E0 },
    .{ .name = entName("alefsym"), .cp = 0x2135 },
    .{ .name = entName("alpha"), .cp = 0x03B1 },
    .{ .name = entName("amp"), .cp = 0x0026 },
    .{ .name = entName("and"), .cp = 0x2227 },
    .{ .name = entName("ang"), .cp = 0x2220 },
    .{ .name = entName("apos"), .cp = 0x0027 },
    .{ .name = entName("aring"), .cp = 0x00E5 },
    .{ .name = entName("asymp"), .cp = 0x2248 },
    .{ .name = entName("atilde"), .cp = 0x00E3 },
    .{ .name = entName("auml"), .cp = 0x00E4 },
    .{ .name = entName("bdquo"), .cp = 0x201E },
    .{ .name = entName("beta"), .cp = 0x03B2 },
    .{ .name = entName("brvbar"), .cp = 0x00A6 },
    .{ .name = entName("bull"), .cp = 0x2022 },
    .{ .name = entName("cap"), .cp = 0x2229 },
    .{ .name = entName("ccedil"), .cp = 0x00E7 },
    .{ .name = entName("cedil"), .cp = 0x00B8 },
    .{ .name = entName("cent"), .cp = 0x00A2 },
    .{ .name = entName("chi"), .cp = 0x03C7 },
    .{ .name = entName("circ"), .cp = 0x02C6 },
    .{ .name = entName("clubs"), .cp = 0x2663 },
    .{ .name = entName("cong"), .cp = 0x2245 },
    .{ .name = entName("copy"), .cp = 0x00A9 },
    .{ .name = entName("crarr"), .cp = 0x21B5 },
    .{ .name = entName("cup"), .cp = 0x222A },
    .{ .name = entName("curren"), .cp = 0x00A4 },
    .{ .name = entName("dArr"), .cp = 0x21D3 },
    .{ .name = entName("dagger"), .cp = 0x2020 },
    .{ .name = entName("darr"), .cp = 0x2193 },
    .{ .name = entName("deg"), .cp = 0x00B0 },
    .{ .name = entName("delta"), .cp = 0x03B4 },
    .{ .name = entName("diams"), .cp = 0x2666 },
    .{ .name = entName("divide"), .cp = 0x00F7 },
    .{ .name = entName("eacute"), .cp = 0x00E9 },
    .{ .name = entName("ecirc"), .cp = 0x00EA },
    .{ .name = entName("egrave"), .cp = 0x00E8 },
    .{ .name = entName("empty"), .cp = 0x2205 },
    .{ .name = entName("emsp"), .cp = 0x2003 },
    .{ .name = entName("ensp"), .cp = 0x2002 },
    .{ .name = entName("epsilon"), .cp = 0x03B5 },
    .{ .name = entName("equiv"), .cp = 0x2261 },
    .{ .name = entName("eta"), .cp = 0x03B7 },
    .{ .name = entName("eth"), .cp = 0x00F0 },
    .{ .name = entName("euml"), .cp = 0x00EB },
    .{ .name = entName("euro"), .cp = 0x20AC },
    .{ .name = entName("exist"), .cp = 0x2203 },
    .{ .name = entName("fnof"), .cp = 0x0192 },
    .{ .name = entName("forall"), .cp = 0x2200 },
    .{ .name = entName("frac12"), .cp = 0x00BD },
    .{ .name = entName("frac14"), .cp = 0x00BC },
    .{ .name = entName("frac34"), .cp = 0x00BE },
    .{ .name = entName("frasl"), .cp = 0x2044 },
    .{ .name = entName("gamma"), .cp = 0x03B3 },
    .{ .name = entName("ge"), .cp = 0x2265 },
    .{ .name = entName("gt"), .cp = 0x003E },
    .{ .name = entName("hArr"), .cp = 0x21D4 },
    .{ .name = entName("harr"), .cp = 0x2194 },
    .{ .name = entName("hearts"), .cp = 0x2665 },
    .{ .name = entName("hellip"), .cp = 0x2026 },
    .{ .name = entName("iacute"), .cp = 0x00ED },
    .{ .name = entName("icirc"), .cp = 0x00EE },
    .{ .name = entName("iexcl"), .cp = 0x00A1 },
    .{ .name = entName("igrave"), .cp = 0x00EC },
    .{ .name = entName("image"), .cp = 0x2111 },
    .{ .name = entName("infin"), .cp = 0x221E },
    .{ .name = entName("int"), .cp = 0x222B },
    .{ .name = entName("iota"), .cp = 0x03B9 },
    .{ .name = entName("iquest"), .cp = 0x00BF },
    .{ .name = entName("isin"), .cp = 0x2208 },
    .{ .name = entName("iuml"), .cp = 0x00EF },
    .{ .name = entName("kappa"), .cp = 0x03BA },
    .{ .name = entName("lArr"), .cp = 0x21D0 },
    .{ .name = entName("lambda"), .cp = 0x03BB },
    .{ .name = entName("lang"), .cp = 0x2329 },
    .{ .name = entName("laquo"), .cp = 0x00AB },
    .{ .name = entName("larr"), .cp = 0x2190 },
    .{ .name = entName("lceil"), .cp = 0x2308 },
    .{ .name = entName("ldquo"), .cp = 0x201C },
    .{ .name = entName("le"), .cp = 0x2264 },
    .{ .name = entName("lfloor"), .cp = 0x230A },
    .{ .name = entName("lowast"), .cp = 0x2217 },
    .{ .name = entName("loz"), .cp = 0x25CA },
    .{ .name = entName("lrm"), .cp = 0x200E },
    .{ .name = entName("lsaquo"), .cp = 0x2039 },
    .{ .name = entName("lsquo"), .cp = 0x2018 },
    .{ .name = entName("lt"), .cp = 0x003C },
    .{ .name = entName("macr"), .cp = 0x00AF },
    .{ .name = entName("mdash"), .cp = 0x2014 },
    .{ .name = entName("micro"), .cp = 0x00B5 },
    .{ .name = entName("middot"), .cp = 0x00B7 },
    .{ .name = entName("minus"), .cp = 0x2212 },
    .{ .name = entName("mu"), .cp = 0x03BC },
    .{ .name = entName("nabla"), .cp = 0x2207 },
    .{ .name = entName("nbsp"), .cp = 0x00A0 },
    .{ .name = entName("ndash"), .cp = 0x2013 },
    .{ .name = entName("ne"), .cp = 0x2260 },
    .{ .name = entName("ni"), .cp = 0x220B },
    .{ .name = entName("not"), .cp = 0x00AC },
    .{ .name = entName("notin"), .cp = 0x2209 },
    .{ .name = entName("nsub"), .cp = 0x2284 },
    .{ .name = entName("ntilde"), .cp = 0x00F1 },
    .{ .name = entName("nu"), .cp = 0x03BD },
    .{ .name = entName("oacute"), .cp = 0x00F3 },
    .{ .name = entName("ocirc"), .cp = 0x00F4 },
    .{ .name = entName("oelig"), .cp = 0x0153 },
    .{ .name = entName("ograve"), .cp = 0x00F2 },
    .{ .name = entName("oline"), .cp = 0x203E },
    .{ .name = entName("omega"), .cp = 0x03C9 },
    .{ .name = entName("omicron"), .cp = 0x03BF },
    .{ .name = entName("oplus"), .cp = 0x2295 },
    .{ .name = entName("or"), .cp = 0x2228 },
    .{ .name = entName("ordf"), .cp = 0x00AA },
    .{ .name = entName("ordm"), .cp = 0x00BA },
    .{ .name = entName("oslash"), .cp = 0x00F8 },
    .{ .name = entName("otilde"), .cp = 0x00F5 },
    .{ .name = entName("otimes"), .cp = 0x2297 },
    .{ .name = entName("ouml"), .cp = 0x00F6 },
    .{ .name = entName("para"), .cp = 0x00B6 },
    .{ .name = entName("part"), .cp = 0x2202 },
    .{ .name = entName("permil"), .cp = 0x2030 },
    .{ .name = entName("perp"), .cp = 0x22A5 },
    .{ .name = entName("phi"), .cp = 0x03C6 },
    .{ .name = entName("pi"), .cp = 0x03C0 },
    .{ .name = entName("piv"), .cp = 0x03D6 },
    .{ .name = entName("plusmn"), .cp = 0x00B1 },
    .{ .name = entName("pound"), .cp = 0x00A3 },
    .{ .name = entName("prime"), .cp = 0x2032 },
    .{ .name = entName("prod"), .cp = 0x220F },
    .{ .name = entName("prop"), .cp = 0x221D },
    .{ .name = entName("psi"), .cp = 0x03C8 },
    .{ .name = entName("quot"), .cp = 0x0022 },
    .{ .name = entName("rArr"), .cp = 0x21D2 },
    .{ .name = entName("radic"), .cp = 0x221A },
    .{ .name = entName("rang"), .cp = 0x232A },
    .{ .name = entName("raquo"), .cp = 0x00BB },
    .{ .name = entName("rarr"), .cp = 0x2192 },
    .{ .name = entName("rceil"), .cp = 0x2309 },
    .{ .name = entName("rdquo"), .cp = 0x201D },
    .{ .name = entName("real"), .cp = 0x211C },
    .{ .name = entName("reg"), .cp = 0x00AE },
    .{ .name = entName("rfloor"), .cp = 0x230B },
    .{ .name = entName("rho"), .cp = 0x03C1 },
    .{ .name = entName("rlm"), .cp = 0x200F },
    .{ .name = entName("rsaquo"), .cp = 0x203A },
    .{ .name = entName("rsquo"), .cp = 0x2019 },
    .{ .name = entName("sbquo"), .cp = 0x201A },
    .{ .name = entName("scaron"), .cp = 0x0161 },
    .{ .name = entName("sdot"), .cp = 0x22C5 },
    .{ .name = entName("sect"), .cp = 0x00A7 },
    .{ .name = entName("shy"), .cp = 0x00AD },
    .{ .name = entName("sigma"), .cp = 0x03C3 },
    .{ .name = entName("sigmaf"), .cp = 0x03C2 },
    .{ .name = entName("sim"), .cp = 0x223C },
    .{ .name = entName("spades"), .cp = 0x2660 },
    .{ .name = entName("sub"), .cp = 0x2282 },
    .{ .name = entName("sube"), .cp = 0x2286 },
    .{ .name = entName("sum"), .cp = 0x2211 },
    .{ .name = entName("sup"), .cp = 0x2283 },
    .{ .name = entName("sup1"), .cp = 0x00B9 },
    .{ .name = entName("sup2"), .cp = 0x00B2 },
    .{ .name = entName("sup3"), .cp = 0x00B3 },
    .{ .name = entName("supe"), .cp = 0x2287 },
    .{ .name = entName("szlig"), .cp = 0x00DF },
    .{ .name = entName("tau"), .cp = 0x03C4 },
    .{ .name = entName("there4"), .cp = 0x2234 },
    .{ .name = entName("theta"), .cp = 0x03B8 },
    .{ .name = entName("thetasym"), .cp = 0x03D1 },
    .{ .name = entName("thinsp"), .cp = 0x2009 },
    .{ .name = entName("thorn"), .cp = 0x00FE },
    .{ .name = entName("tilde"), .cp = 0x02DC },
    .{ .name = entName("times"), .cp = 0x00D7 },
    .{ .name = entName("trade"), .cp = 0x2122 },
    .{ .name = entName("uArr"), .cp = 0x21D1 },
    .{ .name = entName("uacute"), .cp = 0x00FA },
    .{ .name = entName("uarr"), .cp = 0x2191 },
    .{ .name = entName("ucirc"), .cp = 0x00FB },
    .{ .name = entName("ugrave"), .cp = 0x00F9 },
    .{ .name = entName("uml"), .cp = 0x00A8 },
    .{ .name = entName("upsih"), .cp = 0x03D2 },
    .{ .name = entName("upsilon"), .cp = 0x03C5 },
    .{ .name = entName("uuml"), .cp = 0x00FC },
    .{ .name = entName("weierp"), .cp = 0x2118 },
    .{ .name = entName("xi"), .cp = 0x03BE },
    .{ .name = entName("yacute"), .cp = 0x00FD },
    .{ .name = entName("yen"), .cp = 0x00A5 },
    .{ .name = entName("yuml"), .cp = 0x00FF },
    .{ .name = entName("zeta"), .cp = 0x03B6 },
    .{ .name = entName("zwj"), .cp = 0x200D },
    .{ .name = entName("zwnj"), .cp = 0x200C },
};

comptime {
    @setEvalBranchQuota(30000);
    if (k_priv_entities.len != 253) @compileError("entity table must hold 253 rows");
    for (&k_priv_entities, 0..) |*row, i| {
        if (row.name[k_priv_entity_name_max] != 0) @compileError("entity name too long");
        if (i > 0 and std.mem.order(u8, k_priv_entities[i - 1].name[0..], row.name[0..]) != .lt) {
            @compileError("entity table must stay byte-order sorted");
        }
    }
}

/// Compare a non-terminated needle against a table row's name.
///
/// Byte comparison over `len` bytes, then a length tie-break, so the
/// ordering matches the table's sort exactly. A needle that is a strict
/// prefix of the row sorts before it; a row that is a strict prefix of the
/// needle sorts before the needle. Pure.
fn internalEntityCmp(needle: [*]const u8, len: usize, row: *const [k_priv_entity_name_cap]u8) i32 {
    var i: usize = 0;
    while (i < len) : (i += 1) {
        const a = needle[i];
        const b = row[i];
        if (b == 0) {
            return 1; // row ended first: needle sorts after it
        }
        if (a != b) {
            return if (a < b) -1 else 1;
        }
    }
    return if (row[len] == 0) 0 else -1;
}

pub fn priv_reflow_tok_entity_count() callconv(.c) usize {
    return k_priv_entities.len;
}

pub fn priv_reflow_tok_entity_at(idx: usize, out_name: *[*]const u8, out_cp: *u32) callconv(.c) bool {
    if (idx >= k_priv_entities.len) {
        return false;
    }
    const row = &k_priv_entities[idx];
    out_name.* = @ptrCast(&row.name);
    out_cp.* = row.cp;
    return true;
}

pub fn priv_reflow_tok_lookup_entity(name: [*]const u8, len: usize, out_cp: *u32) callconv(.c) bool {
    if (len == 0 or len > k_priv_entity_name_max) {
        return false;
    }
    var lo: usize = 0;
    var hi: usize = k_priv_entities.len;
    while (lo < hi) {
        const mid = lo + (hi - lo) / 2;
        const rel = internalEntityCmp(name, len, &k_priv_entities[mid].name);
        if (rel == 0) {
            out_cp.* = k_priv_entities[mid].cp;
            return true;
        }
        if (rel < 0) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    return false;
}

comptime {
    @export(&priv_reflow_tok_entity_count, .{ .name = "priv_reflow_tok_entity_count" });
    @export(&priv_reflow_tok_entity_at, .{ .name = "priv_reflow_tok_entity_at" });
    @export(&priv_reflow_tok_lookup_entity, .{ .name = "priv_reflow_tok_lookup_entity" });
}
