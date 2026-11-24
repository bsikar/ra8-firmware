//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Per-series glossary container (`TGL1`), recurring-name candidate extraction,
//! consistent honorific policy, and placeholder substitution / restoration.
//! Zero dynamic memory allocation across both in-memory tables and wire views.

const std = @import("std");
const crc32 = @import("crc32");
const err_mod = @import("err");
const sidecar = @import("sidecar");
const lang_mod = @import("lang");

pub const Error = err_mod.TranslateError;
pub const Language = sidecar.Language;

pub const magic: u32 = 0x314C4754; // Little-endian "TGL1".
pub const version: u16 = 1;
pub const header_bytes: usize = 32;
pub const entry_bytes: usize = 12;
pub const max_entries: usize = 64;
pub const max_pool_bytes: usize = 2048;
pub const max_plan_items: usize = 64;
pub const placeholder_len: usize = 7; // "__G00__" .. "__G63__".

/// Canonical honorific policy applied uniformly when substituting and restoring names.
pub const Honorific = enum(u8) {
    none = 0,
    ja_san = 1,
    ja_sama = 2,
    ja_kun = 3,
    ja_chan = 4,
    ja_sensei = 5,
    ja_senpai = 6,
    ja_dono = 7,
    ko_nim = 8,
    ko_ssi = 9,
    ko_gun = 10,
    ko_yang = 11,
    ko_seonsaengnim = 12,
    ko_sunbae = 13,
    ko_hyung = 14,
    ko_oppa = 15,
    ko_noona = 16,
    ko_unnie = 17,

    /// Consistent ASCII suffix policy (keeps `-san`/`-nim` style suffixes).
    pub fn englishSuffix(self: Honorific) []const u8 {
        return switch (self) {
            .none => "",
            .ja_san => "-san",
            .ja_sama => "-sama",
            .ja_kun => "-kun",
            .ja_chan => "-chan",
            .ja_sensei => "-sensei",
            .ja_senpai => "-senpai",
            .ja_dono => "-dono",
            .ko_nim => "-nim",
            .ko_ssi => "-ssi",
            .ko_gun => "-gun",
            .ko_yang => "-yang",
            .ko_seonsaengnim => "-seonsaengnim",
            .ko_sunbae => "-sunbae",
            .ko_hyung => "-hyung",
            .ko_oppa => "-oppa",
            .ko_noona => "-noona",
            .ko_unnie => "-unnie",
        };
    }

    pub fn fromRaw(raw: u8) ?Honorific {
        return switch (raw) {
            0 => .none,
            1 => .ja_san,
            2 => .ja_sama,
            3 => .ja_kun,
            4 => .ja_chan,
            5 => .ja_sensei,
            6 => .ja_senpai,
            7 => .ja_dono,
            8 => .ko_nim,
            9 => .ko_ssi,
            10 => .ko_gun,
            11 => .ko_yang,
            12 => .ko_seonsaengnim,
            13 => .ko_sunbae,
            14 => .ko_hyung,
            15 => .ko_oppa,
            16 => .ko_noona,
            17 => .ko_unnie,
            else => null,
        };
    }
};

const HonorificPattern = struct {
    lang: Language,
    raw: []const u8,
    kind: Honorific,
};

// Ordered longest-first per language so multi-char honorifics match before 1-char suffixes.
const honorific_patterns = [_]HonorificPattern{
    .{ .lang = .japanese, .raw = "\xe3\x81\x9b\xe3\x82\x93\xe3\x81\x9b\xe3\x81\x84", .kind = .ja_sensei },
    .{ .lang = .japanese, .raw = "\xe3\x81\x9b\xe3\x82\x93\xe3\x81\xb1\xe3\x81\x84", .kind = .ja_senpai },
    .{ .lang = .japanese, .raw = "\xe3\x81\xa1\xe3\x82\x83\xe3\x82\x93", .kind = .ja_chan },
    .{ .lang = .japanese, .raw = "\xe5\x85\x88\xe7\x94\x9f", .kind = .ja_sensei },
    .{ .lang = .japanese, .raw = "\xe5\x85\x88\xe8\xbc\xa9", .kind = .ja_senpai },
    .{ .lang = .japanese, .raw = "\xe3\x81\x95\xe3\x82\x93", .kind = .ja_san },
    .{ .lang = .japanese, .raw = "\xe3\x81\x95\xe3\x81\xbe", .kind = .ja_sama },
    .{ .lang = .japanese, .raw = "\xe3\x81\x8f\xe3\x82\x93", .kind = .ja_kun },
    .{ .lang = .japanese, .raw = "\xe3\x81\xa9\xe3\x81\xae", .kind = .ja_dono },
    .{ .lang = .japanese, .raw = "\xe6\xa7\x98", .kind = .ja_sama },
    .{ .lang = .korean, .raw = "\xec\x84\xa0\xec\x83\x9d\xeb\x8b\x98", .kind = .ko_seonsaengnim },
    .{ .lang = .korean, .raw = "\xec\x84\xa0\xeb\xb0\xb0", .kind = .ko_sunbae },
    .{ .lang = .korean, .raw = "\xec\x98\xa4\xeb\xb9\xa0", .kind = .ko_oppa },
    .{ .lang = .korean, .raw = "\xeb\x88\x84\xeb\x82\x98", .kind = .ko_noona },
    .{ .lang = .korean, .raw = "\xec\x96\xb8\xeb\x8b\x88", .kind = .ko_unnie },
    .{ .lang = .korean, .raw = "\xeb\x8b\x98", .kind = .ko_nim },
    .{ .lang = .korean, .raw = "\xec\x94\xa8", .kind = .ko_ssi },
    .{ .lang = .korean, .raw = "\xea\xb5\xb0", .kind = .ko_gun },
    .{ .lang = .korean, .raw = "\xec\x96\x91", .kind = .ko_yang },
    .{ .lang = .korean, .raw = "\xed\x98\x95", .kind = .ko_hyung },
};

/// Wire / table record indexing one source -> English term inside `TGL1` pool.
pub const EntryRecord = extern struct {
    src_off: u16,
    src_len: u16,
    eng_off: u16,
    eng_len: u16,
    flags: u8 = 0,
    reserved: [3]u8 = .{ 0, 0, 0 },
};

/// Substituted placeholder mapping recorded during masking and consumed during restore.
pub const SubstitutionItem = extern struct {
    entry_index: u16,
    honorific: u8,
    reserved: u8 = 0,
};

/// Fixed-capacity caller-owned substitution plan (`max_plan_items` entries).
pub const SubstitutionPlan = struct {
    count: usize = 0,
    items: [max_plan_items]SubstitutionItem = undefined,

    /// Encode plan entries into flat little-endian bytes (`4 * count` bytes).
    pub fn serialize(self: *const SubstitutionPlan, out: []u8) Error!usize {
        const need = self.count * 4;
        if (out.len < need) return Error.InvalidSize;
        var i: usize = 0;
        while (i < self.count) : (i += 1) {
            const slot = out[i * 4 ..][0..4];
            writeU16Le(slot[0..2], self.items[i].entry_index);
            slot[2] = self.items[i].honorific;
            slot[3] = 0;
        }
        return need;
    }

    /// Decode flat serialized plan bytes back into a `SubstitutionPlan`.
    pub fn deserialize(bytes: []const u8) Error!SubstitutionPlan {
        if ((bytes.len % 4) != 0) return Error.InvalidSize;
        const count = bytes.len / 4;
        if (count > max_plan_items) return Error.InvalidSize;
        var plan = SubstitutionPlan{ .count = count };
        var i: usize = 0;
        while (i < count) : (i += 1) {
            const slot = bytes[i * 4 ..][0..4];
            if (slot[3] != 0) return Error.InvalidArg;
            if (Honorific.fromRaw(slot[2]) == null) return Error.InvalidArg;
            plan.items[i] = .{
                .entry_index = readU16Le(slot[0..2]),
                .honorific = slot[2],
                .reserved = 0,
            };
        }
        return plan;
    }
};

/// Discovered chapter candidate name awaiting optional isolated translation.
pub const Candidate = struct {
    stem_slice: []const u8,
    occurrences: u16,
    saw_honorific: bool,
};

/// Validated zero-allocation view over serialized `TGL1` glossary bytes.
pub const TglView = struct {
    bytes: []const u8,
    source_lang: Language,
    revision: u32,
    entry_count: u16,
    pool_off: usize,
    pool_bytes: u32,

    /// Validate header magic/version/flags, CRC-32, bounds, and UTF-8 strings.
    pub fn open(bytes: []const u8) Error!TglView {
        if (bytes.len < header_bytes) return Error.InvalidSize;
        if (readU32Le(bytes[0..4]) != magic) return Error.InvalidArg;
        if (readU16Le(bytes[4..6]) != version) return Error.NotSupported;
        if (readU16Le(bytes[6..8]) != 0) return Error.NotSupported;

        const total_u32 = readU32Le(bytes[8..12]);
        if (@as(u64, total_u32) != @as(u64, bytes.len)) return Error.InvalidSize;

        const stored_crc = readU32Le(bytes[12..16]);
        if (crc32.crc32WithZeroWindow(bytes, 12, 4) != stored_crc) return Error.CrcMismatch;

        const revision = readU32Le(bytes[16..20]);
        const lang_raw = bytes[20];
        const source_lang = Language.fromRaw(lang_raw) orelse return Error.InvalidArg;
        if (bytes[21] != 0) return Error.InvalidArg;
        const entry_count = readU16Le(bytes[22..24]);
        const pool_bytes = readU32Le(bytes[24..28]);
        if (!allZero(bytes[28..32])) return Error.InvalidArg;

        const table_bytes: u64 = @as(u64, entry_count) * entry_bytes;
        const pool_off_u64: u64 = @as(u64, header_bytes) + table_bytes;
        const expected_total = pool_off_u64 + @as(u64, pool_bytes);
        if (expected_total != total_u32) return Error.InvalidSize;

        const pool_off: usize = @intCast(pool_off_u64);
        const pool = bytes[pool_off..][0..@intCast(pool_bytes)];
        var i: u16 = 0;
        while (i < entry_count) : (i += 1) {
            const rec = readEntryRecord(bytes, header_bytes, i);
            if (!allZero(&rec.reserved) or rec.src_len == 0 or rec.eng_len == 0) return Error.InvalidArg;
            const src = try slicePool(pool, rec.src_off, rec.src_len);
            const eng = try slicePool(pool, rec.eng_off, rec.eng_len);
            if (!std.unicode.utf8ValidateSlice(src) or !std.unicode.utf8ValidateSlice(eng)) {
                return Error.InvalidArg;
            }
        }

        return .{
            .bytes = bytes,
            .source_lang = source_lang,
            .revision = revision,
            .entry_count = entry_count,
            .pool_off = pool_off,
            .pool_bytes = pool_bytes,
        };
    }

    pub fn entryAt(self: TglView, idx: u16) Error!EntryRecord {
        if (idx >= self.entry_count) return Error.OutOfRange;
        return readEntryRecord(self.bytes, header_bytes, idx);
    }

    pub fn sourceAt(self: TglView, idx: u16) Error![]const u8 {
        const rec = try self.entryAt(idx);
        return self.bytes[self.pool_off + rec.src_off ..][0..rec.src_len];
    }

    pub fn englishAt(self: TglView, idx: u16) Error![]const u8 {
        const rec = try self.entryAt(idx);
        return self.bytes[self.pool_off + rec.eng_off ..][0..rec.eng_len];
    }

    /// Borrow stored English string for exact `source_utf8`, or `null` when absent.
    pub fn lookup(self: TglView, source_utf8: []const u8) Error!?[]const u8 {
        if (!std.unicode.utf8ValidateSlice(source_utf8)) return Error.InvalidArg;
        var i: u16 = 0;
        while (i < self.entry_count) : (i += 1) {
            const src = try self.sourceAt(i);
            if (std.mem.eql(u8, src, source_utf8)) {
                return try self.englishAt(i);
            }
        }
        return null;
    }

    /// Find longest matching glossary stem at the start of `remaining`, plus optional trailing honorific.
    pub fn matchPrefix(self: TglView, remaining: []const u8) Error!?struct {
        entry_idx: u16,
        consumed: usize,
        honorific: Honorific,
    } {
        var best_idx: u16 = 0;
        var best_src_len: usize = 0;
        var found = false;
        var i: u16 = 0;
        while (i < self.entry_count) : (i += 1) {
            const src = try self.sourceAt(i);
            if (src.len > best_src_len and src.len <= remaining.len and std.mem.eql(u8, remaining[0..src.len], src)) {
                best_idx = i;
                best_src_len = src.len;
                found = true;
            }
        }
        if (!found) return null;

        const tail = remaining[best_src_len..];
        const hon = matchLeadingHonorific(self.source_lang, tail);
        const consumed = best_src_len + hon.bytes;
        return .{
            .entry_idx = best_idx,
            .consumed = consumed,
            .honorific = hon.kind,
        };
    }

    /// Replace known glossary terms (with optional honorifics) in `src_utf8` with `__Gdd__` placeholders.
    pub fn maskText(
        self: TglView,
        src_utf8: []const u8,
        out_masked: []u8,
        out_plan: *SubstitutionPlan,
    ) Error!usize {
        var plan = SubstitutionPlan{};
        const need_len = try self.measureMask(src_utf8, &plan);
        if (need_len > out_masked.len) return Error.InvalidSize;
        const written = try self.writeMasked(src_utf8, out_masked);
        out_plan.* = plan;
        return written;
    }

    /// Atomically validate capacities and emit both masked UTF-8 and serialized substitution plan bytes.
    pub fn maskTextWire(
        self: TglView,
        src_utf8: []const u8,
        out_masked: []u8,
        out_plan_bytes: []u8,
    ) Error!struct { masked_len: usize, plan_len: usize } {
        var plan = SubstitutionPlan{};
        const need_len = try self.measureMask(src_utf8, &plan);
        const need_plan_bytes = plan.count * 4;
        if (need_len > out_masked.len or need_plan_bytes > out_plan_bytes.len) return Error.InvalidSize;
        const masked_len = try self.writeMasked(src_utf8, out_masked);
        const plan_len = try plan.serialize(out_plan_bytes);
        return .{ .masked_len = masked_len, .plan_len = plan_len };
    }

    fn measureMask(self: TglView, src_utf8: []const u8, plan: *SubstitutionPlan) Error!usize {
        if (!std.unicode.utf8ValidateSlice(src_utf8)) return Error.InvalidArg;
        plan.* = SubstitutionPlan{};
        var need_len: usize = 0;
        var cursor: usize = 0;

        while (cursor < src_utf8.len) {
            if (try self.matchPrefix(src_utf8[cursor..])) |hit| {
                if (plan.count >= max_plan_items) return Error.InvalidSize;
                need_len += placeholder_len;
                plan.items[plan.count] = .{
                    .entry_index = hit.entry_idx,
                    .honorific = @backingInt(hit.honorific),
                    .reserved = 0,
                };
                plan.count += 1;
                cursor += hit.consumed;
                continue;
            }
            const cp_len = std.unicode.utf8ByteSequenceLength(src_utf8[cursor]) catch return Error.InvalidArg;
            if (cursor + cp_len > src_utf8.len) return Error.InvalidArg;
            need_len += cp_len;
            cursor += cp_len;
        }
        return need_len;
    }

    fn writeMasked(self: TglView, src_utf8: []const u8, out_masked: []u8) Error!usize {
        var out_len: usize = 0;
        var slot: u8 = 0;
        var cursor: usize = 0;
        while (cursor < src_utf8.len) {
            if (try self.matchPrefix(src_utf8[cursor..])) |hit| {
                writePlaceholderToken(out_masked[out_len..][0..placeholder_len], slot);
                out_len += placeholder_len;
                slot += 1;
                cursor += hit.consumed;
                continue;
            }
            const cp_len = std.unicode.utf8ByteSequenceLength(src_utf8[cursor]) catch unreachable;
            @memcpy(out_masked[out_len..][0..cp_len], src_utf8[cursor..][0..cp_len]);
            out_len += cp_len;
            cursor += cp_len;
        }
        return out_len;
    }

    /// Restore `__Gdd__` placeholder tokens back to canonical English terms plus consistent honorific suffix.
    pub fn restoreText(
        self: TglView,
        plan: *const SubstitutionPlan,
        eng_utf8: []const u8,
        out_restored: []u8,
    ) Error!usize {
        return restoreUsingView(self, plan, eng_utf8, out_restored);
    }
};

fn restoreUsingView(
    view: TglView,
    plan: *const SubstitutionPlan,
    eng_utf8: []const u8,
    out_restored: []u8,
) Error!usize {
    if (!std.unicode.utf8ValidateSlice(eng_utf8)) return Error.InvalidArg;
    if (plan.count > max_plan_items) return Error.InvalidArg;

    // Pass 1 validates every placeholder token and checks output capacity before any write.
    var need_len: usize = 0;
    var cursor: usize = 0;
    while (cursor < eng_utf8.len) {
        if (parsePlaceholderToken(eng_utf8[cursor..])) |slot_idx| {
            if (slot_idx >= plan.count) return Error.InvalidArg;
            const item = plan.items[slot_idx];
            const base = try view.englishAt(item.entry_index);
            const hon = Honorific.fromRaw(item.honorific) orelse return Error.InvalidArg;
            const suffix = honorificToAppend(base, hon);
            need_len += base.len + suffix.len;
            cursor += placeholder_len;
            continue;
        }
        need_len += 1;
        cursor += 1;
    }
    if (need_len > out_restored.len) return Error.InvalidSize;

    var out_len: usize = 0;
    cursor = 0;
    while (cursor < eng_utf8.len) {
        if (parsePlaceholderToken(eng_utf8[cursor..])) |slot_idx| {
            const item = plan.items[slot_idx];
            const base = view.englishAt(item.entry_index) catch unreachable;
            const hon = Honorific.fromRaw(item.honorific) orelse unreachable;
            const suffix = honorificToAppend(base, hon);
            @memcpy(out_restored[out_len..][0..base.len], base);
            out_len += base.len;
            if (suffix.len > 0) {
                @memcpy(out_restored[out_len..][0..suffix.len], suffix);
                out_len += suffix.len;
            }
            cursor += placeholder_len;
            continue;
        }
        out_restored[out_len] = eng_utf8[cursor];
        out_len += 1;
        cursor += 1;
    }
    return out_len;
}

/// Initialize an empty serialized `TGL1` glossary buffer cleanly.
pub fn initEmptyWire(source_lang: Language, revision: u32, out_buf: []u8) Error!usize {
    if (out_buf.len < header_bytes) return Error.InvalidSize;
    writeHeaderBytes(out_buf[0..header_bytes], source_lang, revision, 0, 0, header_bytes);
    const crc = crc32.crc32WithZeroWindow(out_buf[0..header_bytes], 12, 4);
    writeU32Le(out_buf[12..16], crc);
    return header_bytes;
}

/// Insert or replace `source_utf8 -> english_utf8` in a serialized `TGL1` buffer, bumping revision.
pub fn upsertWire(
    tgl_bytes: []const u8,
    source_utf8: []const u8,
    english_utf8: []const u8,
    out_buf: []u8,
) Error!usize {
    if (source_utf8.len == 0 or english_utf8.len == 0) return Error.InvalidArg;
    if (source_utf8.len > std.math.maxInt(u16) or english_utf8.len > std.math.maxInt(u16)) {
        return Error.InvalidSize;
    }
    if (!std.unicode.utf8ValidateSlice(source_utf8) or !std.unicode.utf8ValidateSlice(english_utf8)) {
        return Error.InvalidArg;
    }

    const view = try TglView.open(tgl_bytes);
    var match_idx: ?u16 = null;
    var i: u16 = 0;
    while (i < view.entry_count) : (i += 1) {
        if (std.mem.eql(u8, try view.sourceAt(i), source_utf8)) {
            match_idx = i;
            break;
        }
    }

    const new_count: u16 = if (match_idx != null)
        view.entry_count
    else blk: {
        if (view.entry_count >= max_entries) return Error.InvalidSize;
        break :blk view.entry_count + 1;
    };

    var new_pool_len: usize = source_utf8.len + english_utf8.len;
    i = 0;
    while (i < view.entry_count) : (i += 1) {
        if (match_idx != null and i == match_idx.?) continue;
        const rec = try view.entryAt(i);
        new_pool_len += rec.src_len + rec.eng_len;
    }
    if (new_pool_len > max_pool_bytes) return Error.InvalidSize;

    const table_off = header_bytes;
    const pool_off = header_bytes + @as(usize, new_count) * entry_bytes;
    const total_bytes = pool_off + new_pool_len;
    if (out_buf.len < total_bytes) return Error.InvalidSize;

    const new_rev = view.revision +% 1;
    writeHeaderBytes(out_buf[0..header_bytes], view.source_lang, new_rev, new_count, @intCast(new_pool_len), total_bytes);

    var write_pool_cursor: usize = 0;
    var out_idx: usize = 0;
    i = 0;
    while (i < view.entry_count) : (i += 1) {
        const src = if (match_idx != null and i == match_idx.?) source_utf8 else try view.sourceAt(i);
        const eng = if (match_idx != null and i == match_idx.?) english_utf8 else try view.englishAt(i);
        const flags: u8 = if (match_idx != null and i == match_idx.?) 0 else (try view.entryAt(i)).flags;
        writePackedEntry(out_buf, table_off, pool_off, out_idx, &write_pool_cursor, src, eng, flags);
        out_idx += 1;
    }
    if (match_idx == null) {
        writePackedEntry(out_buf, table_off, pool_off, out_idx, &write_pool_cursor, source_utf8, english_utf8, 0);
    }

    const crc = crc32.crc32WithZeroWindow(out_buf[0..total_bytes], 12, 4);
    writeU32Le(out_buf[12..16], crc);
    return total_bytes;
}

/// Caller-owned in-memory series glossary table with save/load persistence.
pub const GlossaryTable = struct {
    source_lang: Language = .unknown,
    revision: u32 = 1,
    entry_count: u16 = 0,
    pool_used: u16 = 0,
    entries: [max_entries]EntryRecord = undefined,
    pool: [max_pool_bytes]u8 = undefined,

    pub fn init(source_lang: Language, revision: u32) GlossaryTable {
        return .{ .source_lang = source_lang, .revision = revision };
    }

    pub fn sourceAt(self: *const GlossaryTable, idx: u16) Error![]const u8 {
        if (idx >= self.entry_count) return Error.OutOfRange;
        const r = self.entries[idx];
        return self.pool[r.src_off..][0..r.src_len];
    }

    pub fn englishAt(self: *const GlossaryTable, idx: u16) Error![]const u8 {
        if (idx >= self.entry_count) return Error.OutOfRange;
        const r = self.entries[idx];
        return self.pool[r.eng_off..][0..r.eng_len];
    }

    pub fn lookup(self: *const GlossaryTable, source_utf8: []const u8) ?[]const u8 {
        var i: u16 = 0;
        while (i < self.entry_count) : (i += 1) {
            const r = self.entries[i];
            const s = self.pool[r.src_off..][0..r.src_len];
            if (std.mem.eql(u8, s, source_utf8)) {
                return self.pool[r.eng_off..][0..r.eng_len];
            }
        }
        return null;
    }

    /// Add a new term or replace an existing term's English mapping cleanly.
    pub fn upsert(self: *GlossaryTable, source_utf8: []const u8, english_utf8: []const u8) Error!void {
        var tmp_in: [header_bytes + max_entries * entry_bytes + max_pool_bytes]u8 = undefined;
        var tmp_out: [header_bytes + max_entries * entry_bytes + max_pool_bytes]u8 = undefined;
        const saved_len = try self.save(&tmp_in);
        const updated_len = try upsertWire(tmp_in[0..saved_len], source_utf8, english_utf8, &tmp_out);
        try self.load(tmp_out[0..updated_len]);
    }

    /// Persist this series glossary as a CRC-32-checked `TGL1` container.
    pub fn save(self: *const GlossaryTable, out_buf: []u8) Error!usize {
        const pool_off = header_bytes + @as(usize, self.entry_count) * entry_bytes;
        const total_bytes = pool_off + self.pool_used;
        if (out_buf.len < total_bytes) return Error.InvalidSize;
        writeHeaderBytes(out_buf[0..header_bytes], self.source_lang, self.revision, self.entry_count, self.pool_used, total_bytes);
        var i: usize = 0;
        while (i < self.entry_count) : (i += 1) {
            writeEntryRecord(out_buf[header_bytes + i * entry_bytes ..][0..entry_bytes], self.entries[i]);
        }
        if (self.pool_used > 0) {
            @memcpy(out_buf[pool_off..][0..self.pool_used], self.pool[0..self.pool_used]);
        }
        const crc = crc32.crc32WithZeroWindow(out_buf[0..total_bytes], 12, 4);
        writeU32Le(out_buf[12..16], crc);
        return total_bytes;
    }

    /// Reload a validated `TGL1` container into this table without allocation.
    pub fn load(self: *GlossaryTable, bytes: []const u8) Error!void {
        const view = try TglView.open(bytes);
        if (view.entry_count > max_entries or view.pool_bytes > max_pool_bytes) {
            return Error.InvalidSize;
        }
        var fresh = GlossaryTable{
            .source_lang = view.source_lang,
            .revision = view.revision,
            .entry_count = view.entry_count,
            .pool_used = @intCast(view.pool_bytes),
        };
        var i: u16 = 0;
        while (i < view.entry_count) : (i += 1) {
            fresh.entries[i] = try view.entryAt(i);
        }
        if (view.pool_bytes > 0) {
            @memcpy(fresh.pool[0..view.pool_bytes], view.bytes[view.pool_off..][0..view.pool_bytes]);
        }
        self.* = fresh;
    }

    /// Mask known terms in `src_utf8`, returning masked bytes length and populating `out_plan`.
    pub fn maskText(self: *const GlossaryTable, src_utf8: []const u8, out_masked: []u8, out_plan: *SubstitutionPlan) Error!usize {
        var tmp: [header_bytes + max_entries * entry_bytes + max_pool_bytes]u8 = undefined;
        const len = try self.save(&tmp);
        const view = try TglView.open(tmp[0..len]);
        return view.maskText(src_utf8, out_masked, out_plan);
    }

    /// Restore masked placeholder tokens in `eng_utf8` using this table's canonical English strings.
    pub fn restoreText(self: *const GlossaryTable, plan: *const SubstitutionPlan, eng_utf8: []const u8, out_restored: []u8) Error!usize {
        var tmp: [header_bytes + max_entries * entry_bytes + max_pool_bytes]u8 = undefined;
        const len = try self.save(&tmp);
        const view = try TglView.open(tmp[0..len]);
        return restoreUsingView(view, plan, eng_utf8, out_restored);
    }
};

/// Scan chapter OCR bubbles for recurring katakana/Hangul runs or honorific-suffixed stems
/// absent from `existing`, returning the deduplicated candidate list in encounter order.
pub fn extractCandidates(
    source_lang: Language,
    existing: *const GlossaryTable,
    bubble_texts: []const []const u8,
    out_candidates: []Candidate,
) Error!usize {
    const raw_cap = 128;
    var stems: [raw_cap][]const u8 = undefined;
    var counts: [raw_cap]u16 = @splat(0);
    var saw_hon: [raw_cap]bool = @splat(false);
    var unique_count: usize = 0;

    for (bubble_texts) |text| {
        if (!std.unicode.utf8ValidateSlice(text)) return Error.InvalidArg;
        var cursor: usize = 0;
        while (cursor < text.len) {
            const token_opt = scanNextCandidateToken(source_lang, text[cursor..]);
            const token = token_opt orelse {
                const step = std.unicode.utf8ByteSequenceLength(text[cursor]) catch return Error.InvalidArg;
                cursor += step;
                continue;
            };
            cursor += token.consumed;
            if (existing.lookup(token.stem) != null) continue;
            recordStem(&stems, &counts, &saw_hon, &unique_count, raw_cap, token.stem, token.has_honorific);
        }
    }

    var written: usize = 0;
    var i: usize = 0;
    while (i < unique_count) : (i += 1) {
        if (!saw_hon[i] and counts[i] < 2) continue;
        if (written >= out_candidates.len) return Error.InvalidSize;
        out_candidates[written] = .{
            .stem_slice = stems[i],
            .occurrences = counts[i],
            .saw_honorific = saw_hon[i],
        };
        written += 1;
    }
    return written;
}

const ScannedToken = struct {
    stem: []const u8,
    consumed: usize,
    has_honorific: bool,
};

fn scanNextCandidateToken(source_lang: Language, text: []const u8) ?ScannedToken {
    if (text.len == 0) return null;
    const first_len = std.unicode.utf8ByteSequenceLength(text[0]) catch return null;
    if (first_len > text.len) return null;
    const first_cp = std.unicode.utf8Decode(text[0..first_len]) catch return null;

    if (isKatakanaNameChar(first_cp)) {
        return scanKatakanaCandidate(source_lang, text);
    }
    if (lang_mod.isHangul(first_cp) or lang_mod.isHan(first_cp)) {
        return scanHangulOrHanCandidate(source_lang, text);
    }
    return null;
}

fn scanKatakanaCandidate(source_lang: Language, text: []const u8) ?ScannedToken {
    var end: usize = 0;
    var cp_count: usize = 0;
    while (end < text.len) {
        const len = std.unicode.utf8ByteSequenceLength(text[end]) catch break;
        if (end + len > text.len) break;
        const cp = std.unicode.utf8Decode(text[end..][0..len]) catch break;
        if (!isKatakanaNameChar(cp)) break;
        end += len;
        cp_count += 1;
    }
    if (cp_count < 2) return null;
    const hon = matchLeadingHonorific(source_lang, text[end..]);
    return .{
        .stem = text[0..end],
        .consumed = end + hon.bytes,
        .has_honorific = hon.kind != .none,
    };
}

fn scanHangulOrHanCandidate(source_lang: Language, text: []const u8) ?ScannedToken {
    var end: usize = 0;
    var cp_count: usize = 0;
    while (end < text.len) {
        const len = std.unicode.utf8ByteSequenceLength(text[end]) catch break;
        if (end + len > text.len) break;
        const cp = std.unicode.utf8Decode(text[end..][0..len]) catch break;
        if (!lang_mod.isHangul(cp) and !lang_mod.isHan(cp)) break;
        end += len;
        cp_count += 1;
    }
    if (cp_count < 2) return null;

    // Check whether the Hangul/Han run ends with an honorific suffix (e.g. "Tanaka-nim").
    const run = text[0..end];
    if (matchTrailingHonorific(source_lang, run)) |trail| {
        if (trail.stem.len > 0) {
            return .{
                .stem = trail.stem,
                .consumed = end,
                .has_honorific = true,
            };
        }
    }
    const post_hon = matchLeadingHonorific(source_lang, text[end..]);
    if (post_hon.kind != .none) {
        return .{
            .stem = run,
            .consumed = end + post_hon.bytes,
            .has_honorific = true,
        };
    }
    return .{
        .stem = run,
        .consumed = end,
        .has_honorific = false,
    };
}

fn recordStem(
    stems: *[128][]const u8,
    counts: *[128]u16,
    saw_hon: *[128]bool,
    unique_count: *usize,
    cap: usize,
    stem: []const u8,
    has_hon: bool,
) void {
    var i: usize = 0;
    while (i < unique_count.*) : (i += 1) {
        if (std.mem.eql(u8, stems[i], stem)) {
            counts[i] +|= 1;
            if (has_hon) saw_hon[i] = true;
            return;
        }
    }
    if (unique_count.* >= cap) return;
    stems[unique_count.*] = stem;
    counts[unique_count.*] = 1;
    saw_hon[unique_count.*] = has_hon;
    unique_count.* += 1;
}

fn isKatakanaNameChar(cp: u21) bool {
    return (cp >= 0x30A1 and cp <= 0x30FA) or
        (cp == 0x30FC) or
        (cp >= 0x30FD and cp <= 0x30FF) or
        (cp >= 0x31F0 and cp <= 0x31FF);
}

fn matchLeadingHonorific(source_lang: Language, slice: []const u8) struct { kind: Honorific, bytes: usize } {
    for (honorific_patterns) |pat| {
        if (source_lang != .unknown and pat.lang != source_lang) continue;
        if (slice.len >= pat.raw.len and std.mem.eql(u8, slice[0..pat.raw.len], pat.raw)) {
            return .{ .kind = pat.kind, .bytes = pat.raw.len };
        }
    }
    return .{ .kind = .none, .bytes = 0 };
}

fn matchTrailingHonorific(source_lang: Language, run: []const u8) ?struct { stem: []const u8, kind: Honorific } {
    for (honorific_patterns) |pat| {
        if (source_lang != .unknown and pat.lang != source_lang) continue;
        if (run.len > pat.raw.len and std.mem.eql(u8, run[run.len - pat.raw.len ..], pat.raw)) {
            return .{ .stem = run[0 .. run.len - pat.raw.len], .kind = pat.kind };
        }
    }
    return null;
}

fn honorificToAppend(english_stem: []const u8, hon: Honorific) []const u8 {
    if (hon == .none) return "";
    const suffix = hon.englishSuffix();
    if (english_stem.len >= suffix.len and std.ascii.eqlIgnoreCase(english_stem[english_stem.len - suffix.len ..], suffix)) {
        return "";
    }
    return suffix;
}

fn writePlaceholderToken(dst: *[placeholder_len]u8, slot: u8) void {
    dst[0] = '_';
    dst[1] = '_';
    dst[2] = 'G';
    dst[3] = '0' + (slot / 10);
    dst[4] = '0' + (slot % 10);
    dst[5] = '_';
    dst[6] = '_';
}

fn parsePlaceholderToken(slice: []const u8) ?usize {
    if (slice.len < placeholder_len) return null;
    if (slice[0] != '_' or slice[1] != '_' or slice[2] != 'G' or slice[5] != '_' or slice[6] != '_') {
        return null;
    }
    const d1 = slice[3];
    const d0 = slice[4];
    if (d1 < '0' or d1 > '9' or d0 < '0' or d0 > '9') return null;
    const value: usize = @as(usize, d1 - '0') * 10 + @as(usize, d0 - '0');
    if (value >= max_plan_items) return null;
    return value;
}

fn slicePool(pool: []const u8, off: u16, length: u16) Error![]const u8 {
    const end: usize = @as(usize, off) + @as(usize, length);
    if (end > pool.len) return Error.InvalidSize;
    return pool[off..end];
}

fn writePackedEntry(
    out_buf: []u8,
    table_off: usize,
    pool_off: usize,
    idx: usize,
    pool_cursor: *usize,
    src: []const u8,
    eng: []const u8,
    flags: u8,
) void {
    const src_off: u16 = @intCast(pool_cursor.*);
    @memcpy(out_buf[pool_off + pool_cursor.* ..][0..src.len], src);
    pool_cursor.* += src.len;

    const eng_off: u16 = @intCast(pool_cursor.*);
    @memcpy(out_buf[pool_off + pool_cursor.* ..][0..eng.len], eng);
    pool_cursor.* += eng.len;

    writeEntryRecord(out_buf[table_off + idx * entry_bytes ..][0..entry_bytes], .{
        .src_off = src_off,
        .src_len = @intCast(src.len),
        .eng_off = eng_off,
        .eng_len = @intCast(eng.len),
        .flags = flags,
        .reserved = .{ 0, 0, 0 },
    });
}

fn writeHeaderBytes(
    hdr: *[header_bytes]u8,
    source_lang: Language,
    revision: u32,
    entry_count: u16,
    pool_bytes: u32,
    total_bytes: usize,
) void {
    @memset(hdr, 0);
    writeU32Le(hdr[0..4], magic);
    writeU16Le(hdr[4..6], version);
    writeU16Le(hdr[6..8], 0);
    writeU32Le(hdr[8..12], @intCast(total_bytes));
    writeU32Le(hdr[16..20], revision);
    hdr[20] = @backingInt(source_lang);
    hdr[21] = 0;
    writeU16Le(hdr[22..24], entry_count);
    writeU32Le(hdr[24..28], pool_bytes);
}

fn readEntryRecord(bytes: []const u8, table_off: usize, index: u16) EntryRecord {
    const p = bytes[table_off + @as(usize, index) * entry_bytes ..][0..entry_bytes];
    return .{
        .src_off = readU16Le(p[0..2]),
        .src_len = readU16Le(p[2..4]),
        .eng_off = readU16Le(p[4..6]),
        .eng_len = readU16Le(p[6..8]),
        .flags = p[8],
        .reserved = .{ p[9], p[10], p[11] },
    };
}

fn writeEntryRecord(dst: *[entry_bytes]u8, rec: EntryRecord) void {
    writeU16Le(dst[0..2], rec.src_off);
    writeU16Le(dst[2..4], rec.src_len);
    writeU16Le(dst[4..6], rec.eng_off);
    writeU16Le(dst[6..8], rec.eng_len);
    dst[8] = rec.flags;
    dst[9] = rec.reserved[0];
    dst[10] = rec.reserved[1];
    dst[11] = rec.reserved[2];
}

fn allZero(slice: []const u8) bool {
    for (slice) |b| {
        if (b != 0) return false;
    }
    return true;
}

fn readU16Le(bytes: *const [2]u8) u16 {
    return std.mem.readInt(u16, bytes, .little);
}

fn readU32Le(bytes: *const [4]u8) u32 {
    return std.mem.readInt(u32, bytes, .little);
}

fn writeU16Le(dst: *[2]u8, value: u16) void {
    std.mem.writeInt(u16, dst, value, .little);
}

fn writeU32Le(dst: *[4]u8, value: u32) void {
    std.mem.writeInt(u32, dst, value, .little);
}
