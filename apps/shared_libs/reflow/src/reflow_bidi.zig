//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Heap-free UAX #9 Bidirectional Algorithm implementation for `reflow`
//! (`RA8FW-1173` / `RA8FW-296`). Determines paragraph base level (`P2`-`P3`
//! or explicit HTML `dir` / CSS `direction`), resolves explicit embeddings,
//! overrides, and isolates (`X1`-`X10`), weak/neutral/bracket types
//! (`W1`-`W7`, `N0`-`N2`), implicit levels (`I1`-`I2`), and performs
//! per-line visual reordering (`L1`-`L4`) while keeping combining/syllable
//! clusters atomic.

const std = @import("std");
const u = @import("reflow_unicode.zig");

const BidiClass = u.BidiClass;

pub const BaseDir = enum(u2) {
    auto = 0,
    ltr = 1,
    rtl = 2,
};

pub const k_max_depth: usize = 125;
const k_max_brackets: usize = 63;

pub const BidiItem = struct {
    cp: u32,
    cluster_id: u16,
    cls: BidiClass,
    orig_cls: BidiClass,
    level: u8,
    removed: bool,
    paired_mirror: bool,
};

const Override = enum(u2) {
    neutral = 0,
    l = 1,
    r = 2,
};

const StackEntry = struct {
    level: u8 = 0,
    override_cls: Override = .neutral,
    isolate: bool = false,
};

const BracketEntry = struct {
    paired: u32 = 0,
    idx: usize = 0,
};

const BracketPair = struct {
    open_idx: usize = 0,
    close_idx: usize = 0,
};

/// Reusable caller/module scratch to keep ARM stack frames tiny (< 512 B).
pub const BidiScratch = struct {
    stack: [k_max_depth + 2]StackEntry = @splat(.{}),
    bracket_stack: [k_max_brackets]BracketEntry = @splat(.{}),
    bracket_pairs: [k_max_brackets]BracketPair = @splat(.{}),
};

/// UAX #9 P2-P3 paragraph base level from an item slice (or explicit `dir`).
pub fn resolveParagraphLevel(items: []const BidiItem, base_dir: BaseDir) u8 {
    if (base_dir == .ltr) return 0;
    if (base_dir == .rtl) return 1;
    var iso_depth: u32 = 0;
    for (items) |it| {
        if (it.orig_cls.isIsolateInitiator()) {
            iso_depth += 1;
            continue;
        }
        if (it.orig_cls == .pdi) {
            if (iso_depth > 0) iso_depth -= 1;
            continue;
        }
        if (iso_depth != 0) continue;
        if (it.orig_cls == .l) return 0;
        if (it.orig_cls == .r or it.orig_cls == .al) return 1;
    }
    return 0;
}

fn findMatchingPdi(items: []const BidiItem, start_idx: usize) usize {
    var depth: u32 = 1;
    var i: usize = start_idx + 1;
    while (i < items.len) : (i += 1) {
        if (items[i].orig_cls.isIsolateInitiator()) {
            depth += 1;
        } else if (items[i].orig_cls == .pdi) {
            depth -= 1;
            if (depth == 0) return i;
        }
    }
    return items.len;
}

fn applyOverride(cls: BidiClass, ov: Override) BidiClass {
    return switch (ov) {
        .neutral => cls,
        .l => .l,
        .r => .r,
    };
}

fn nextEven(lvl: u8) u8 {
    return (lvl + 2) & ~@as(u8, 1);
}

fn nextOdd(lvl: u8) u8 {
    return (lvl + 1) | 1;
}

fn resolveExplicit(items: []BidiItem, para_level: u8, sc: *BidiScratch) void {
    var sp: usize = 1;
    sc.stack[0] = .{ .level = para_level, .override_cls = .neutral, .isolate = false };
    var ov_iso: u32 = 0;
    var ov_emb: u32 = 0;
    var valid_iso: u32 = 0;

    for (items, 0..) |*it, idx| {
        const cls = it.orig_cls;
        it.removed = cls.isRemovedByX9();
        if (cls == .b) {
            sp = 1;
            sc.stack[0] = .{ .level = para_level, .override_cls = .neutral, .isolate = false };
            ov_iso = 0;
            ov_emb = 0;
            valid_iso = 0;
            it.level = para_level;
            it.cls = .b;
            continue;
        }
        if (cls == .rle or cls == .lre or cls == .rlo or cls == .lro) {
            it.level = sc.stack[sp - 1].level;
            if (ov_iso == 0 and ov_emb == 0) {
                const is_rtl = (cls == .rle or cls == .rlo);
                const nxt = if (is_rtl) nextOdd(sc.stack[sp - 1].level) else nextEven(sc.stack[sp - 1].level);
                if (nxt <= k_max_depth and sp < sc.stack.len) {
                    const ov: Override = if (cls == .rlo) .r else if (cls == .lro) .l else .neutral;
                    sc.stack[sp] = .{ .level = nxt, .override_cls = ov, .isolate = false };
                    sp += 1;
                    continue;
                }
            }
            if (ov_iso == 0) ov_emb += 1;
            continue;
        }
        if (cls.isIsolateInitiator()) {
            const top = sc.stack[sp - 1];
            it.level = top.level;
            it.cls = applyOverride(.on, top.override_cls);
            const want_rtl = if (cls == .rli)
                true
            else if (cls == .lri)
                false
            else blk: {
                const pdi = findMatchingPdi(items, idx);
                break :blk resolveParagraphLevel(items[idx + 1 .. pdi], .auto) == 1;
            };
            const nxt = if (want_rtl) nextOdd(top.level) else nextEven(top.level);
            if (ov_iso == 0 and ov_emb == 0 and nxt <= k_max_depth and sp < sc.stack.len) {
                valid_iso += 1;
                sc.stack[sp] = .{ .level = nxt, .override_cls = .neutral, .isolate = true };
                sp += 1;
            } else {
                ov_iso += 1;
            }
            continue;
        }
        if (cls == .pdi) {
            if (ov_iso > 0) {
                ov_iso -= 1;
            } else if (valid_iso > 0) {
                ov_emb = 0;
                while (sp > 1 and !sc.stack[sp - 1].isolate) : (sp -= 1) {}
                if (sp > 1) sp -= 1;
                valid_iso -= 1;
            }
            it.level = sc.stack[sp - 1].level;
            it.cls = applyOverride(.on, sc.stack[sp - 1].override_cls);
            continue;
        }
        if (cls == .pdf) {
            it.level = sc.stack[sp - 1].level;
            if (ov_iso == 0) {
                if (ov_emb > 0) {
                    ov_emb -= 1;
                } else if (sp > 1 and !sc.stack[sp - 1].isolate) {
                    sp -= 1;
                }
            }
            continue;
        }
        if (cls == .bn) {
            it.level = sc.stack[sp - 1].level;
            continue;
        }
        const top = sc.stack[sp - 1];
        it.level = top.level;
        it.cls = applyOverride(cls, top.override_cls);
    }
}

fn strongFromLevel(lvl: u8) BidiClass {
    return if ((lvl & 1) != 0) .r else .l;
}

fn nextVisible(items: []const BidiItem, from: usize, end: usize) ?usize {
    var i = from;
    while (i < end) : (i += 1) {
        if (!items[i].removed) return i;
    }
    return null;
}

fn prevVisible(items: []const BidiItem, start: usize, before: usize) ?usize {
    var i = before;
    while (i > start) {
        i -= 1;
        if (!items[i].removed) return i;
    }
    return null;
}

fn resolveWeak(items: []BidiItem, start: usize, end: usize, sos: BidiClass) void {
    var prev_cls: BidiClass = sos;
    // W1 + W2 + W3 in sequence
    var i: usize = start;
    while (i < end) : (i += 1) {
        if (items[i].removed) continue;
        if (items[i].cls == .nsm) {
            items[i].cls = if (items[i].orig_cls.isIsolateInitiator() or items[i].orig_cls == .pdi) .on else prev_cls;
        }
        prev_cls = items[i].cls;
    }

    var last_strong: BidiClass = sos;
    i = start;
    while (i < end) : (i += 1) {
        if (items[i].removed) continue;
        if (items[i].cls == .en and last_strong == .al) {
            items[i].cls = .an; // W2
        } else if (items[i].cls.isStrong()) {
            last_strong = items[i].cls;
        }
    }

    i = start;
    while (i < end) : (i += 1) {
        if (!items[i].removed and items[i].cls == .al) {
            items[i].cls = .r; // W3
        }
    }

    // W4: ES between EN..EN, CS between EN..EN or AN..AN
    i = start;
    while (i < end) : (i += 1) {
        if (items[i].removed) continue;
        const c = items[i].cls;
        if (c == .es or c == .cs) {
            const p_idx = prevVisible(items, start, i) orelse continue;
            const n_idx = nextVisible(items, i + 1, end) orelse continue;
            const p = items[p_idx].cls;
            const n = items[n_idx].cls;
            if (p == .en and n == .en) {
                items[i].cls = .en;
            } else if (c == .cs and p == .an and n == .an) {
                items[i].cls = .an;
            }
        }
    }

    // W5: ET runs adjacent to EN -> EN
    i = start;
    while (i < end) : (i += 1) {
        if (items[i].removed or items[i].cls != .et) continue;
        var run_end = i + 1;
        while (run_end < end) : (run_end += 1) {
            if (!items[run_end].removed and items[run_end].cls != .et) break;
        }
        const before_en = if (prevVisible(items, start, i)) |pi| items[pi].cls == .en else false;
        const after_en = if (nextVisible(items, run_end, end)) |ni| items[ni].cls == .en else false;
        if (before_en or after_en) {
            var k = i;
            while (k < run_end) : (k += 1) {
                if (!items[k].removed) items[k].cls = .en;
            }
        }
        i = run_end - 1;
    }

    // W6 + W7
    last_strong = sos;
    i = start;
    while (i < end) : (i += 1) {
        if (items[i].removed) continue;
        if (items[i].cls == .et or items[i].cls == .es or items[i].cls == .cs) {
            items[i].cls = .on; // W6
        } else if (items[i].cls == .en and last_strong == .l) {
            items[i].cls = .l; // W7
        } else if (items[i].cls == .l or items[i].cls == .r) {
            last_strong = items[i].cls;
        }
    }
}

fn strongForNeutral(cls: BidiClass) ?BidiClass {
    return switch (cls) {
        .l => .l,
        .r, .en, .an => .r,
        else => null,
    };
}

fn resolveBrackets(items: []BidiItem, start: usize, end: usize, sos: BidiClass, emb_dir: BidiClass, sc: *BidiScratch) void {
    var bsp: usize = 0;
    var pair_count: usize = 0;
    var i: usize = start;
    while (i < end) : (i += 1) {
        if (items[i].removed or items[i].cls != .on) continue;
        const info = u.bracketPairKind(items[i].cp) orelse continue;
        if (info.is_open) {
            if (bsp >= k_max_brackets) break;
            sc.bracket_stack[bsp] = .{ .paired = info.paired, .idx = i };
            bsp += 1;
        } else if (bsp > 0) {
            var k = bsp;
            while (k > 0) {
                k -= 1;
                if (sc.bracket_stack[k].paired == items[i].cp) {
                    if (pair_count < k_max_brackets) {
                        sc.bracket_pairs[pair_count] = .{
                            .open_idx = sc.bracket_stack[k].idx,
                            .close_idx = i,
                        };
                        pair_count += 1;
                    }
                    bsp = k;
                    break;
                }
            }
        }
    }

    // Sort pairs by opening index (insertion sort, pair_count <= 63).
    var p: usize = 1;
    while (p < pair_count) : (p += 1) {
        const key = sc.bracket_pairs[p];
        var j = p;
        while (j > 0 and sc.bracket_pairs[j - 1].open_idx > key.open_idx) : (j -= 1) {
            sc.bracket_pairs[j] = sc.bracket_pairs[j - 1];
        }
        sc.bracket_pairs[j] = key;
    }

    const opp_dir: BidiClass = if (emb_dir == .l) .r else .l;
    for (sc.bracket_pairs[0..pair_count]) |pair| {
        var found_emb = false;
        var found_opp = false;
        var k = pair.open_idx + 1;
        while (k < pair.close_idx) : (k += 1) {
            if (items[k].removed) continue;
            if (strongForNeutral(items[k].cls)) |s| {
                if (s == emb_dir) found_emb = true else found_opp = true;
            }
        }
        var chosen: ?BidiClass = null;
        if (found_emb) {
            chosen = emb_dir;
        } else if (found_opp) {
            var ctx_dir = sos;
            var b = pair.open_idx;
            while (b > start) {
                b -= 1;
                if (items[b].removed) continue;
                if (strongForNeutral(items[b].cls)) |s| {
                    ctx_dir = s;
                    break;
                }
            }
            chosen = if (ctx_dir == opp_dir) opp_dir else emb_dir;
        }
        if (chosen) |c| {
            items[pair.open_idx].cls = c;
            items[pair.close_idx].cls = c;
        }
    }
}

fn resolveNeutralsAndImplicit(items: []BidiItem, start: usize, end: usize, sos: BidiClass, eos: BidiClass, run_lvl: u8, sc: *BidiScratch) void {
    const emb_dir = strongFromLevel(run_lvl);
    resolveBrackets(items, start, end, sos, emb_dir, sc);

    var i: usize = start;
    while (i < end) : (i += 1) {
        if (items[i].removed or strongForNeutral(items[i].cls) != null) continue;
        var run_end = i + 1;
        while (run_end < end) : (run_end += 1) {
            if (!items[run_end].removed and strongForNeutral(items[run_end].cls) != null) break;
        }
        const before_strong = if (prevVisible(items, start, i)) |pi| (strongForNeutral(items[pi].cls) orelse sos) else sos;
        const after_strong = if (nextVisible(items, run_end, end)) |ni| (strongForNeutral(items[ni].cls) orelse eos) else eos;
        const resolved = if (before_strong == after_strong) before_strong else emb_dir;
        var k = i;
        while (k < run_end) : (k += 1) {
            if (!items[k].removed) items[k].cls = resolved;
        }
        i = run_end - 1;
    }

    // I1 / I2
    const is_odd = (run_lvl & 1) != 0;
    for (items[start..end]) |*it| {
        if (it.removed) continue;
        if (!is_odd) {
            if (it.cls == .r) {
                it.level +%= 1;
            } else if (it.cls == .an or it.cls == .en) {
                it.level +%= 2;
            }
        } else {
            if (it.cls == .l or it.cls == .an or it.cls == .en) {
                it.level +%= 1;
            }
        }
    }
}

/// Run UAX #9 resolution (`P2`-`P3`, `X1`-`X10`, `W1`-`W7`, `N0`-`N2`, `I1`-`I2`)
/// on a paragraph slice of items; returns the resolved paragraph embedding level.
pub fn resolveParagraph(items: []BidiItem, base_dir: BaseDir, sc: *BidiScratch) u8 {
    if (items.len == 0) return if (base_dir == .rtl) 1 else 0;
    const para_level = resolveParagraphLevel(items, base_dir);
    resolveExplicit(items, para_level, sc);

    var i: usize = 0;
    var prev_lvl: u8 = para_level;
    while (i < items.len) {
        while (i < items.len and items[i].removed) : (i += 1) {}
        if (i >= items.len) break;
        const run_lvl = items[i].level;
        var run_end = i + 1;
        while (run_end < items.len) : (run_end += 1) {
            if (items[run_end].removed) continue;
            if (items[run_end].level != run_lvl) break;
        }
        const next_lvl: u8 = if (run_end < items.len) items[run_end].level else para_level;
        const sos = strongFromLevel(@max(prev_lvl, run_lvl));
        const eos = strongFromLevel(@max(run_lvl, next_lvl));
        resolveWeak(items, i, run_end, sos);
        resolveNeutralsAndImplicit(items, i, run_end, sos, eos, run_lvl, sc);
        prev_lvl = run_lvl;
        i = run_end;
    }
    return para_level;
}

/// Apply UAX #9 L1-L4 reordering to one visual line slice of arbitrary items
/// exposing `.level`, `.orig_cls`, `.cluster_id`, and `.cp`. Mirrors paired
/// brackets at odd levels and keeps multi-glyph clusters atomic during L2 reversals.
pub fn reorderVisualLine(comptime T: type, line: []T, para_level: u8) void {
    if (line.len == 0) return;

    // L1: reset whitespace / isolate format chars trailing on the line (or before S/B).
    var k: usize = line.len;
    while (k > 0) {
        k -= 1;
        const oc = line[k].orig_cls;
        if (oc == .ws or oc == .fsi or oc == .lri or oc == .rli or oc == .pdi or oc == .b or oc == .s) {
            line[k].level = para_level;
        } else if (!oc.isExplicitFormatting()) {
            break;
        } else {
            line[k].level = para_level;
        }
    }

    // L4: mirror brackets/symbols resolved at odd embedding levels.
    for (line) |*it| {
        if ((it.level & 1) != 0 and it.cp >= 0) {
            const m = u.bidiMirror(@intCast(it.cp));
            if (m != @as(u32, @intCast(it.cp))) {
                it.cp = @intCast(m);
            }
        }
    }

    var min_odd: u8 = 255;
    var max_lvl: u8 = 0;
    for (line) |it| {
        if (it.level > max_lvl) max_lvl = it.level;
        if ((it.level & 1) != 0 and it.level < min_odd) min_odd = it.level;
    }
    if (min_odd == 255) return;

    // L2: reverse contiguous sequences of level >= cur from max down to min_odd.
    var cur_lvl: i16 = @intCast(max_lvl);
    const stop: i16 = @intCast(min_odd);
    while (cur_lvl >= stop) : (cur_lvl -= 1) {
        var i: usize = 0;
        while (i < line.len) {
            if (line[i].level < @as(u8, @intCast(cur_lvl))) {
                i += 1;
                continue;
            }
            var j = i + 1;
            while (j < line.len and line[j].level >= @as(u8, @intCast(cur_lvl))) : (j += 1) {}
            reverseKeepingClusters(T, line[i..j]);
            i = j;
        }
    }
}

fn reverseKeepingClusters(comptime T: type, slice: []T) void {
    if (slice.len <= 1) return;
    // Reverse whole slice, then reverse each contiguous multi-item cluster back
    // so base+combining marks maintain logical order within the cluster.
    std.mem.reverse(T, slice);
    var i: usize = 0;
    while (i < slice.len) {
        const cid = slice[i].cluster_id;
        var j = i + 1;
        while (j < slice.len and slice[j].cluster_id == cid) : (j += 1) {}
        if (j - i > 1) {
            std.mem.reverse(T, slice[i..j]);
        }
        i = j;
    }
}
